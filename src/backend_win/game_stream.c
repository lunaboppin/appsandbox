/*
 * game_stream.c -- optional game streaming: a nominated VM's image, a throwaway
 * duplicate of it per session, and the sign-ins behind the dashboard. See game_stream.h.
 */

#include <winsock2.h>
#include "game_stream.h"
#include "disk_util.h"
#include "shared_appliance.h"
#include "vm_agent.h"
#include "vm_stream.h"
#include "vm_web.h"
#include "ui.h"

#include <bcrypt.h>
#include <dpapi.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#define GS_MAX_SESSIONS      8
#define GS_MAX_LOGINS        32
#define GS_LOGIN_MS          (12ull * 60ull * 60ull * 1000ull)
#define GS_PASSWORD_KEEP_MS  (15ull * 60ull * 1000ull)
#define GS_PASSWORD_BYTES    256      /* a multiple of CRYPTPROTECTMEMORY_BLOCK_SIZE */
#define GS_START_TIMEOUT_MS  (35ul * 60ul * 1000ul)
#define GS_SIGNED_OUT_POLLS  3        /* of GS_WATCH_MS each, before a session counts as over */
#define GS_WATCH_MS          10000
#define GS_CATALOG_MAX       (4u * 1024u * 1024u)

typedef struct {
    BOOL      used;
    UINT64    vm_id;
    wchar_t   vm_name[32];
    wchar_t   user[24];
    wchar_t   image[MAX_PATH];        /* the image dir this duplicate's disk depends on */
    char      item[128];
    char      mode[16];
    char      state[16];              /* starting, ready, ending, failed */
    char      detail[300];
    volatile LONG stop;
} GsSession;

typedef struct {
    char      token[65];
    wchar_t   user[24];
    BOOL      tls;
    ULONGLONG expires;
    ULONGLONG password_until;
    BYTE      password[GS_PASSWORD_BYTES];
} GsLogin;

typedef struct {
    int       slot;
    wchar_t   password[128];
} GsStart;

static SRWLOCK   g_lock = SRWLOCK_INIT;
static BOOL      g_enabled;
static wchar_t   g_nominated[256];
static int       g_max_sessions = 2;
static int       g_idle_minutes = 30;
static wchar_t   g_group[65];
static BOOL      g_lan;
static wchar_t   g_root[MAX_PATH];    /* <parent>\GameStreaming */
static wchar_t   g_image[MAX_PATH];   /* the current image's base.vhdx */

static GsSession g_sessions[GS_MAX_SESSIONS];
static GsLogin   g_logins[GS_MAX_LOGINS];
static volatile LONG g_threads;
static volatile LONG g_shutting_down;

static struct {
    volatile LONG busy;
    volatile LONG cancel;
    volatile LONG pct;
    wchar_t phase[160];
    wchar_t error[400];
} g_build;

/* ---- Small helpers ---- */

static void set_build_phase(const wchar_t *text)
{
    AcquireSRWLockExclusive(&g_lock);
    wcsncpy_s(g_build.phase, _countof(g_build.phase), text, _TRUNCATE);
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_build.pct, -1);
    ui_log(L"Game Streaming: %s", text);
}

static BOOL random_hex_upper(wchar_t *out, int chars)
{
    BYTE raw[16];
    int i;
    static const wchar_t hex[] = L"0123456789ABCDEF";
    if (chars > 32) return FALSE;
    if (BCryptGenRandom(NULL, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return FALSE;
    for (i = 0; i < chars; i++) out[i] = hex[(raw[i / 2] >> (i % 2 ? 0 : 4)) & 15];
    out[chars] = 0;
    return TRUE;
}

static BOOL random_token(char *out, size_t chars)
{
    BYTE raw[32];
    size_t i;
    static const char hex[] = "0123456789abcdef";
    if (chars < 65 || BCryptGenRandom(NULL, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return FALSE;
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }
    out[64] = 0;
    SecureZeroMemory(raw, sizeof(raw));
    return TRUE;
}

/* Constant-time, so a cookie can't be guessed a character at a time. */
static BOOL token_equal(const char *a, const char *b)
{
    unsigned char diff = 0;
    int i;
    if (strlen(a) != 64 || strlen(b) != 64) return FALSE;
    for (i = 0; i < 64; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static int json_append_utf8(char *out, int cap, int pos, const char *key, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    if (pos >= cap - 8) return pos;
    pos += sprintf_s(out + pos, cap - pos, "\"%s\":\"", key);
    for (; *p && pos < cap - 8; p++) {
        if (*p == '"' || *p == '\\') { out[pos++] = '\\'; out[pos++] = (char)*p; }
        else if (*p < 0x20) pos += sprintf_s(out + pos, cap - pos, "\\u%04x", *p);
        else out[pos++] = (char)*p;
    }
    out[pos++] = '"';
    out[pos] = 0;
    return pos;
}

static int json_append_wide(char *out, int cap, int pos, const char *key, const wchar_t *text)
{
    char utf8[1400];
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
    utf8[sizeof(utf8) - 1] = 0;
    return json_append_utf8(out, cap, pos, key, utf8);
}

static const wchar_t *vm_problem(VmInstance *vm)
{
    if (!vm) return L"That VM doesn't exist.";
    if (_wcsicmp(vm->os_type, L"Windows") != 0) return L"Game Streaming needs a Windows VM.";
    if (vm->is_template || vm->is_appliance || vm->session_clone) return L"Choose a regular VM.";
    if (vm->lab_mode) return L"Malware lab VMs are isolated, so they can't be used.";
    if (!vm->install_complete) return L"That VM hasn't finished installing.";
    return NULL;
}

static ULONGLONG dir_size(const wchar_t *dir)
{
    wchar_t pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAW d;
    HANDLE h;
    ULONGLONG total = 0;
    if (_snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir) < 0) return 0;
    h = FindFirstFileW(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(d.cFileName, L".") == 0 || wcscmp(d.cFileName, L"..") == 0) continue;
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (_snwprintf_s(child, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, d.cFileName) >= 0)
                total += dir_size(child);
        } else {
            total += ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return total;
}

static void image_dir_of(const wchar_t *base, wchar_t *dir, size_t chars)
{
    wchar_t *slash;
    wcscpy_s(dir, chars, base);
    slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
}

/* Delete images nothing depends on any more. */
static void prune_images(void)
{
    wchar_t pattern[MAX_PATH], dir[MAX_PATH], current[MAX_PATH] = L"", root[MAX_PATH];
    WIN32_FIND_DATAW d;
    HANDLE h;
    int i;
    AcquireSRWLockShared(&g_lock);
    wcscpy_s(root, MAX_PATH, g_root);
    if (g_image[0]) image_dir_of(g_image, current, MAX_PATH);
    ReleaseSRWLockShared(&g_lock);
    if (!root[0]) return;
    swprintf_s(pattern, MAX_PATH, L"%s\\image-*", root);
    h = FindFirstFileW(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        BOOL used = FALSE;
        swprintf_s(dir, MAX_PATH, L"%s\\%s", root, d.cFileName);
        if (_wcsicmp(dir, current) == 0) continue;
        AcquireSRWLockShared(&g_lock);
        for (i = 0; i < GS_MAX_SESSIONS; i++)
            if (g_sessions[i].used && _wcsicmp(g_sessions[i].image, dir) == 0) used = TRUE;
        ReleaseSRWLockShared(&g_lock);
        if (!used && !InterlockedCompareExchange(&g_build.busy, 0, 0))
            asb_session_clone_remove_leftover(NULL, dir);
    } while (FindNextFileW(h, &d));
    FindClose(h);
}

/* ---- Settings ---- */

BOOL game_stream_has_settings(void) { return g_enabled; }

void game_stream_write_settings(FILE *f)
{
    if (!g_enabled) return;
    fwprintf(f, L"GameStreamEnabled=1\nGameStreamVm=%s\nGameStreamMax=%d\nGameStreamIdle=%d\n",
             g_nominated, g_max_sessions, g_idle_minutes);
    if (g_group[0]) fwprintf(f, L"GameStreamGroup=%s\n", g_group);
    if (g_lan) fwprintf(f, L"GameStreamLan=1\n");
    if (g_root[0]) fwprintf(f, L"GameStreamRoot=%s\n", g_root);
    if (g_image[0]) fwprintf(f, L"GameStreamImage=%s\n", g_image);
}

BOOL game_stream_read_setting(const wchar_t *line)
{
    if (wcsncmp(line, L"GameStreamEnabled=", 18) == 0) g_enabled = _wtoi(line + 18) != 0;
    else if (wcsncmp(line, L"GameStreamVm=", 13) == 0) wcscpy_s(g_nominated, _countof(g_nominated), line + 13);
    else if (wcsncmp(line, L"GameStreamMax=", 14) == 0) g_max_sessions = _wtoi(line + 14);
    else if (wcsncmp(line, L"GameStreamIdle=", 15) == 0) g_idle_minutes = _wtoi(line + 15);
    else if (wcsncmp(line, L"GameStreamGroup=", 16) == 0) wcscpy_s(g_group, _countof(g_group), line + 16);
    else if (wcsncmp(line, L"GameStreamLan=", 14) == 0) g_lan = _wtoi(line + 14) != 0;
    else if (wcsncmp(line, L"GameStreamRoot=", 15) == 0) wcscpy_s(g_root, MAX_PATH, line + 15);
    else if (wcsncmp(line, L"GameStreamImage=", 16) == 0) wcscpy_s(g_image, MAX_PATH, line + 16);
    else return FALSE;
    return TRUE;
}

/* ---- Sign-ins ---- */

BOOL game_stream_dashboard_lan(void) { return g_enabled && g_lan; }

HRESULT game_stream_sign_in(const wchar_t *user, const wchar_t *password, BOOL tls,
                            char *token, size_t token_chars, char *message, int message_cap)
{
    wchar_t why[300] = L"", group[65];
    HRESULT hr;
    int i, slot = 0;
    ULONGLONG now = GetTickCount64(), oldest = ~0ull;

    AcquireSRWLockShared(&g_lock);
    wcscpy_s(group, _countof(group), g_group);
    ReleaseSRWLockShared(&g_lock);
    hr = shared_appliance_domain_validate(user, password, group, why, _countof(why));
    if (FAILED(hr)) {
        WideCharToMultiByte(CP_UTF8, 0, why[0] ? why : L"That sign-in wasn't accepted.", -1,
                            message, message_cap, NULL, NULL);
        return hr;
    }
    if (!random_token(token, token_chars)) return E_FAIL;

    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < GS_MAX_LOGINS; i++) {
        if (g_logins[i].expires < oldest) { oldest = g_logins[i].expires; slot = i; }
    }
    SecureZeroMemory(&g_logins[slot], sizeof(g_logins[slot]));
    strcpy_s(g_logins[slot].token, sizeof(g_logins[slot].token), token);
    wcsncpy_s(g_logins[slot].user, _countof(g_logins[slot].user), user, _TRUNCATE);
    g_logins[slot].tls = tls;
    g_logins[slot].expires = now + GS_LOGIN_MS;
    /* Kept for the session they're about to start, encrypted to this process. */
    if (wcslen(password) * sizeof(wchar_t) < GS_PASSWORD_BYTES) {
        wcscpy_s((wchar_t *)g_logins[slot].password, GS_PASSWORD_BYTES / sizeof(wchar_t), password);
        if (CryptProtectMemory(g_logins[slot].password, GS_PASSWORD_BYTES, CRYPTPROTECTMEMORY_SAME_PROCESS))
            g_logins[slot].password_until = now + GS_PASSWORD_KEEP_MS;
        else
            SecureZeroMemory(g_logins[slot].password, GS_PASSWORD_BYTES);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return S_OK;
}

void game_stream_sign_out(const char *token)
{
    int i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < GS_MAX_LOGINS; i++)
        if (g_logins[i].expires && token_equal(g_logins[i].token, token))
            SecureZeroMemory(&g_logins[i], sizeof(g_logins[i]));
    ReleaseSRWLockExclusive(&g_lock);
}

BOOL game_stream_signed_in(const char *token, BOOL tls, wchar_t *user, size_t user_chars)
{
    int i;
    BOOL ok = FALSE;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_lock);
    if (g_enabled) {
        for (i = 0; i < GS_MAX_LOGINS; i++) {
            if (g_logins[i].expires > now && g_logins[i].tls == tls && token_equal(g_logins[i].token, token)) {
                wcscpy_s(user, user_chars, g_logins[i].user);
                ok = TRUE;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

/* Take the kept password out: it's used once. FALSE when it has lapsed. */
static BOOL take_kept_password(const char *token, wchar_t *password, size_t chars)
{
    int i;
    BOOL ok = FALSE;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < GS_MAX_LOGINS; i++) {
        if (g_logins[i].expires && token_equal(g_logins[i].token, token)) {
            if (g_logins[i].password_until > now &&
                CryptUnprotectMemory(g_logins[i].password, GS_PASSWORD_BYTES, CRYPTPROTECTMEMORY_SAME_PROCESS)) {
                wcscpy_s(password, chars, (const wchar_t *)g_logins[i].password);
                ok = TRUE;
            }
            SecureZeroMemory(g_logins[i].password, GS_PASSWORD_BYTES);
            g_logins[i].password_until = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

/* ---- The catalog ---- */

BOOL game_stream_catalog_file(const char *leaf, wchar_t *path, size_t path_chars)
{
    const char *p;
    wchar_t dir[MAX_PATH] = L"";
    for (p = leaf; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '.') return FALSE;
    if (!leaf[0] || strstr(leaf, "..") || strlen(leaf) > 64) return FALSE;
    AcquireSRWLockShared(&g_lock);
    if (g_image[0]) image_dir_of(g_image, dir, MAX_PATH);
    ReleaseSRWLockShared(&g_lock);
    if (!dir[0]) return FALSE;
    return _snwprintf_s(path, path_chars, _TRUNCATE, L"%s\\catalog\\%S", dir, leaf) >= 0;
}

static char *read_catalog(void)
{
    wchar_t path[MAX_PATH];
    HANDLE f;
    DWORD size, got = 0;
    char *text;
    if (!game_stream_catalog_file("catalog.json", path, MAX_PATH)) return NULL;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    text = size < GS_CATALOG_MAX ? (char *)malloc((size_t)size + 1) : NULL;
    if (text && (!ReadFile(f, text, size, &got, NULL) || got != size)) { free(text); text = NULL; }
    if (text) text[size] = 0;
    CloseHandle(f);
    return text;
}

/* Is this "desktop", or a game the image's scan found? */
static BOOL item_allowed(const char *item)
{
    char *catalog, quoted[140];
    BOOL found = FALSE;
    if (strcmp(item, "desktop") == 0) return TRUE;
    if (strlen(item) > 127 || strpbrk(item, "\"\\")) return FALSE;
    catalog = read_catalog();
    if (catalog) {
        sprintf_s(quoted, sizeof(quoted), "\"%s\"", item);
        found = strstr(catalog, quoted) != NULL;
        free(catalog);
    }
    return found;
}

/* Copy one file out of the guest's catalog folder. */
static HRESULT fetch_catalog_file(VmInstance *vm, const char *leaf, const wchar_t *dir)
{
    char *reply = (char *)malloc(ASB_DOMAIN_REPLY_MAX);
    BYTE *chunk = (BYTE *)malloc(ASB_DOMAIN_REPLY_MAX);
    wchar_t path[MAX_PATH];
    HANDLE f = INVALID_HANDLE_VALUE;
    ULONGLONG offset = 0, total = 1;
    HRESULT hr = E_OUTOFMEMORY;

    if (!reply || !chunk) goto done;
    swprintf_s(path, MAX_PATH, L"%s\\%S", dir, leaf);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { hr = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    while (offset < total) {
        char cmd[128], *colon;
        DWORD bytes = ASB_DOMAIN_REPLY_MAX, wrote = 0;
        sprintf_s(cmd, sizeof(cmd), "session_catalog_read:%s:%llu", leaf, offset);
        if (!vm_agent_send(vm, cmd, reply, ASB_DOMAIN_REPLY_MAX, 30000)) { hr = E_FAIL; goto done; }
        total = _strtoui64(reply + 3, &colon, 10);
        if (*colon != ':' || (!colon[1] && offset < total)) { hr = E_FAIL; goto done; }
        if (!colon[1]) break;
        if (!CryptStringToBinaryA(colon + 1, 0, CRYPT_STRING_BASE64, chunk, &bytes, NULL, NULL) ||
            !bytes || !WriteFile(f, chunk, bytes, &wrote, NULL)) { hr = E_FAIL; goto done; }
        offset += bytes;
    }
    hr = S_OK;
done:
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(reply);
    free(chunk);
    return hr;
}

/* Scan the build VM, then copy the list and each game's picture to the host. */
static HRESULT collect_catalog(VmInstance *vm, const wchar_t *image_dir)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    char reply[1024], *text, *p;
    HANDLE f;
    DWORD size, got = 0;
    HRESULT hr;

    swprintf_s(dir, MAX_PATH, L"%s\\catalog", image_dir);
    CreateDirectoryW(dir, NULL);
    if (!vm_agent_send(vm, "session_catalog_scan", reply, sizeof(reply), 6 * 60 * 1000)) return E_FAIL;
    hr = fetch_catalog_file(vm, "catalog.json", dir);
    if (FAILED(hr)) return hr;

    swprintf_s(path, MAX_PATH, L"%s\\catalog.json", dir);
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return E_FAIL;
    size = GetFileSize(f, NULL);
    text = size < GS_CATALOG_MAX ? (char *)malloc((size_t)size + 1) : NULL;
    if (text && ReadFile(f, text, size, &got, NULL) && got == size) {
        text[size] = 0;
        for (p = text; (p = strstr(p, "\"art\":\"")) != NULL; ) {
            char leaf[80];
            const char *end;
            p += 7;
            end = strchr(p, '"');
            if (!end || end == p || end - p >= (int)sizeof(leaf)) continue;
            memcpy(leaf, p, end - p);
            leaf[end - p] = 0;
            /* A game without its picture still shows, by name. */
            fetch_catalog_file(vm, leaf, dir);
        }
    }
    free(text);
    CloseHandle(f);
    return S_OK;
}

/* ---- Building the image ---- */

static BOOL wait_for_agent(UINT64 id, DWORD timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        VmInstance *vm;
        if (InterlockedCompareExchange(&g_build.cancel, 0, 0) || g_shutting_down) return FALSE;
        vm = asb_find_vm_by_id(id);
        if (!vm) return FALSE;
        if (vm->agent_online && !vm->agent_initializing) return TRUE;
        Sleep(2000);
    }
    return FALSE;
}

static HRESULT build_steps(const wchar_t *source_vhdx, const wchar_t *image_dir, const wchar_t *base,
                           DWORD ram_mb, DWORD cpu_cores, UINT64 *build_id, wchar_t *error, size_t error_chars)
{
    wchar_t sessions[MAX_PATH], name[16], suffix[8], err[300] = L"";
    AsbVmConfig config;
    VmInstance *vm;
    char reply[1200];
    ULONGLONG deadline;
    HRESULT hr;
    UINT64 id;

    set_build_phase(L"Copying the VM's disk");
    hr = vhdx_copy_flat(base, source_vhdx, &g_build.pct, &g_build.cancel);
    if (FAILED(hr)) {
        swprintf_s(error, error_chars, L"Copying the VM's disk failed (0x%08X).", hr);
        return hr;
    }
    /* The VM may have an older agent, without the commands the rest of this needs. */
    set_build_phase(L"Updating the App Sandbox guest tools");
    hr = asb_upgrade_windows_agent_offline(base);
    if (FAILED(hr)) {
        swprintf_s(error, error_chars, L"Updating the guest tools failed (0x%08X). If BitLocker is on in this VM, turn it off first.", hr);
        return hr;
    }

    set_build_phase(L"Starting the copy");
    if (!random_hex_upper(suffix, 6)) return E_FAIL;
    swprintf_s(name, _countof(name), L"GS-B%s", suffix);
    AcquireSRWLockShared(&g_lock);
    swprintf_s(sessions, MAX_PATH, L"%s\\sessions", g_root);
    ReleaseSRWLockShared(&g_lock);
    CreateDirectoryW(sessions, NULL);
    ZeroMemory(&config, sizeof(config));
    config.name = name;
    config.os_type = L"Windows";
    config.base_vhdx = base;
    config.storage_parent = sessions;
    config.ram_mb = ram_mb;
    config.cpu_cores = cpu_cores;
    config.gpu_mode = ASB_GPU_NONE;
    config.network_mode = ASB_NET_NAT;
    config.session_clone = TRUE;
    hr = asb_vm_create(&config);
    if (FAILED(hr)) {
        swprintf_s(error, error_chars, L"Starting the copy failed (0x%08X).", hr);
        return hr;
    }
    vm = asb_vm_instance(asb_vm_find(name));
    if (!vm) return E_FAIL;
    *build_id = id = vm->unique_id;

    /* Installing Sunshine now saves each duplicate the download. */
    hr = vm_stream_configure(vm, TRUE, ASB_STREAM_THIS_PC, TRUE, err, _countof(err));
    if (FAILED(hr)) {
        swprintf_s(error, error_chars, L"%s", err[0] ? err : L"Could not set up streaming on the copy.");
        return hr;
    }
    if (!wait_for_agent(id, 15 * 60 * 1000)) {
        wcscpy_s(error, error_chars, L"The copy didn't start. If BitLocker is on in this VM, turn it off, then build the image again.");
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    set_build_phase(L"Installing Sunshine");
    deadline = GetTickCount64() + 50ull * 60ull * 1000ull;
    for (;;) {
        vm = asb_find_vm_by_id(id);
        if (!vm || InterlockedCompareExchange(&g_build.cancel, 0, 0) || g_shutting_down) return E_ABORT;
        reply[0] = 0;
        if (vm_agent_send(vm, "stream_status", reply, sizeof(reply), 35000)) {
            if (strstr(reply, "\"phase\":\"ready\"")) break;
            if (strstr(reply, "\"phase\":\"failed\"")) {
                wcscpy_s(error, error_chars, L"Installing Sunshine in the copy failed. See the log.");
                return E_FAIL;
            }
        }
        if (GetTickCount64() > deadline) {
            wcscpy_s(error, error_chars, L"Installing Sunshine took too long. Does the VM have internet access?");
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        Sleep(5000);
    }

    set_build_phase(L"Looking for games");
    vm = asb_find_vm_by_id(id);
    if (!vm) return E_FAIL;
    hr = collect_catalog(vm, image_dir);
    if (FAILED(hr)) {
        wcscpy_s(error, error_chars, L"Couldn't read the list of games from the copy.");
        return hr;
    }

    set_build_phase(L"Generalizing Windows with Sysprep");
    vm = asb_find_vm_by_id(id);
    if (!vm || !vm_agent_send(vm, "session_image_prepare", reply, sizeof(reply), 30000)) {
        wcscpy_s(error, error_chars, L"Couldn't start Sysprep in the copy.");
        return E_FAIL;
    }
    deadline = GetTickCount64() + 40ull * 60ull * 1000ull;
    for (;;) {
        vm = asb_find_vm_by_id(id);
        if (!vm || InterlockedCompareExchange(&g_build.cancel, 0, 0) || g_shutting_down) return E_ABORT;
        if (!vm->running) break;
        reply[0] = 0;
        if (vm_agent_send(vm, "session_image_status", reply, sizeof(reply), 10000) &&
            strncmp(reply, "ok:failed|", 10) == 0) {
            swprintf_s(error, error_chars, L"%S", reply + 10);
            return E_FAIL;
        }
        if (GetTickCount64() > deadline) {
            wcscpy_s(error, error_chars, L"Sysprep took too long.");
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        Sleep(5000);
    }

    /* The generalized state goes into the base disk. */
    set_build_phase(L"Saving the image");
    {
        int tries;
        hr = E_FAIL;
        for (tries = 0; tries < 30 && FAILED(hr); tries++) {
            vm = asb_find_vm_by_id(id);
            if (!vm) return E_FAIL;
            hr = vhdx_merge(vm->vhdx_path);
            if (FAILED(hr)) Sleep(2000);
        }
    }
    if (FAILED(hr)) {
        swprintf_s(error, error_chars, L"Saving the image failed (0x%08X).", hr);
        return hr;
    }
    return S_OK;
}

static DWORD WINAPI build_thread(LPVOID param)
{
    wchar_t nominated[256], root[MAX_PATH], image_dir[MAX_PATH] = L"", base[MAX_PATH] = L"";
    wchar_t source[MAX_PATH], suffix[16], error[400] = L"";
    UINT64 build_id = 0;
    DWORD ram = 0, cpu = 0;
    HRESULT hr = E_FAIL;
    VmInstance *vm;
    (void)param;

    AcquireSRWLockShared(&g_lock);
    wcscpy_s(nominated, _countof(nominated), g_nominated);
    wcscpy_s(root, MAX_PATH, g_root);
    ReleaseSRWLockShared(&g_lock);

    vm = asb_vm_instance(asb_vm_find(nominated));
    if (!vm || vm_problem(vm)) {
        wcscpy_s(error, _countof(error), vm ? vm_problem(vm) : L"The nominated VM no longer exists.");
    } else if (vm->running) {
        wcscpy_s(error, _countof(error), L"Shut the nominated VM down first: its disk has to be still while it's copied.");
    } else {
        ULARGE_INTEGER free_bytes;
        ULONGLONG need = vm->storage_root[0] ? dir_size(vm->storage_root) : 0;
        wcscpy_s(source, MAX_PATH, vm->vhdx_path);
        ram = vm->ram_mb;
        cpu = vm->cpu_cores;
        CreateDirectoryW(root, NULL);
        if (GetDiskFreeSpaceExW(root, &free_bytes, NULL, NULL) && free_bytes.QuadPart < need) {
            swprintf_s(error, _countof(error), L"Building the image needs %llu GB free next to the VM, and there is %llu GB.",
                       need >> 30, free_bytes.QuadPart >> 30);
        } else if (!random_hex_upper(suffix, 8)) {
            wcscpy_s(error, _countof(error), L"Random numbers failed.");
        } else {
            swprintf_s(image_dir, MAX_PATH, L"%s\\image-%s", root, suffix);
            swprintf_s(base, MAX_PATH, L"%s\\base.vhdx", image_dir);
            CreateDirectoryW(image_dir, NULL);
            hr = build_steps(source, image_dir, base, ram, cpu, &build_id, error, _countof(error));
        }
    }

    if (build_id) asb_session_clone_delete(build_id);
    if (SUCCEEDED(hr)) {
        AcquireSRWLockExclusive(&g_lock);
        wcscpy_s(g_image, MAX_PATH, base);
        g_build.error[0] = 0;
        g_build.phase[0] = 0;
        ReleaseSRWLockExclusive(&g_lock);
        asb_save();
        ui_log(L"Game Streaming: the image is ready.");
    } else {
        /* The old image, if there is one, stays in use. */
        if (image_dir[0]) asb_session_clone_remove_leftover(NULL, image_dir);
        AcquireSRWLockExclusive(&g_lock);
        if (!error[0]) wcscpy_s(error, _countof(error), L"Building the image was stopped.");
        wcscpy_s(g_build.error, _countof(g_build.error), error);
        g_build.phase[0] = 0;
        ReleaseSRWLockExclusive(&g_lock);
        ui_log(L"Game Streaming: building the image failed: %s", error);
    }
    InterlockedExchange(&g_build.busy, 0);
    prune_images();
    InterlockedDecrement(&g_threads);
    return 0;
}

HRESULT game_stream_build_image(wchar_t *err, size_t err_chars)
{
    HANDLE t;
    if (err && err_chars) err[0] = 0;
    if (!g_enabled || !g_nominated[0]) {
        if (err) wcsncpy_s(err, err_chars, L"Nominate a VM first.", _TRUNCATE);
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
    if (InterlockedExchange(&g_build.busy, 1)) {
        if (err) wcsncpy_s(err, err_chars, L"The image is already being built.", _TRUNCATE);
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    InterlockedExchange(&g_build.cancel, 0);
    AcquireSRWLockExclusive(&g_lock);
    g_build.error[0] = 0;
    wcscpy_s(g_build.phase, _countof(g_build.phase), L"Starting");
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedIncrement(&g_threads);
    t = CreateThread(NULL, 0, build_thread, NULL, 0, NULL);
    if (!t) {
        InterlockedDecrement(&g_threads);
        InterlockedExchange(&g_build.busy, 0);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(t);
    return S_OK;
}

/* ---- A session ---- */

static void session_state(int slot, const char *state, const char *detail)
{
    AcquireSRWLockExclusive(&g_lock);
    strcpy_s(g_sessions[slot].state, sizeof(g_sessions[slot].state), state);
    strncpy_s(g_sessions[slot].detail, sizeof(g_sessions[slot].detail), detail ? detail : "", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_lock);
}

static void session_failed(int slot, const char *why)
{
    wchar_t w[300];
    session_state(slot, "failed", why);
    MultiByteToWideChar(CP_UTF8, 0, why, -1, w, _countof(w));
    ui_log(L"Game Streaming: a session failed: %s", w);
}

/* Is whoever's at this duplicate's console the session's user? */
static BOOL console_user(VmInstance *vm, const wchar_t *user, BOOL *anyone)
{
    char reply[256], expect[128], name[64];
    *anyone = FALSE;
    if (!vm_agent_send(vm, "session_status", reply, sizeof(reply), 8000)) return FALSE;
    *anyone = strlen(reply) > 3;
    WideCharToMultiByte(CP_UTF8, 0, user, -1, name, sizeof(name), NULL, NULL);
    sprintf_s(expect, sizeof(expect), "ok:%s\\%s", ASB_DOMAIN_NETBIOS, name);
    return _stricmp(reply, expect) == 0;
}

/* Stream and browser both come up with the VM: the user picks between them on the
   dashboard and can switch. Runs until the session ends, then cleans up. */
static DWORD WINAPI session_thread(LPVOID param)
{
    GsStart *start = (GsStart *)param;
    int slot = start->slot, signed_out = 0;
    GsSession *s = &g_sessions[slot];
    AsbVmConfig config;
    VmInstance *vm, *source;
    wchar_t sessions[MAX_PATH], base[MAX_PATH], nominated[256], err[300];
    char logon[1024], reply[512];
    UINT64 id = 0;
    BOOL lan, software = FALSE, anyone;
    DWORD ram = 4096, cpu = 4;
    int gpu = ASB_GPU_NONE, idle_minutes;
    ULONGLONG deadline, idle_since;
    HRESULT hr;

    AcquireSRWLockShared(&g_lock);
    swprintf_s(sessions, MAX_PATH, L"%s\\sessions", g_root);
    wcscpy_s(base, MAX_PATH, g_image);
    wcscpy_s(nominated, _countof(nominated), g_nominated);
    lan = g_lan;
    idle_minutes = g_idle_minutes;
    ReleaseSRWLockShared(&g_lock);

    source = asb_vm_instance(asb_vm_find(nominated));
    if (source) {
        ram = source->ram_mb;
        cpu = source->cpu_cores;
        gpu = source->gpu_mode;
        software = source->stream_software;
    }

    session_state(slot, "starting", "Creating your VM");
    CreateDirectoryW(sessions, NULL);
    if (!shared_appliance_domain_autologon_command(s->user, start->password, logon, sizeof(logon))) {
        session_failed(slot, "Your sign-in can't be used to start a VM.");
        goto cleanup;
    }
    SecureZeroMemory(start->password, sizeof(start->password));

    ZeroMemory(&config, sizeof(config));
    config.name = s->vm_name;
    config.os_type = L"Windows";
    config.base_vhdx = base;
    config.storage_parent = sessions;
    config.ram_mb = ram;
    config.cpu_cores = cpu;
    config.gpu_mode = gpu;
    config.network_mode = ASB_NET_NAT;
    config.domain_member = TRUE;
    config.session_clone = TRUE;
    config.session_logon = logon;
    hr = asb_vm_create(&config);
    SecureZeroMemory(logon, sizeof(logon));
    if (FAILED(hr)) {
        sprintf_s(reply, sizeof(reply), "The VM couldn't be created (0x%08X).", hr);
        session_failed(slot, reply);
        goto cleanup;
    }
    vm = asb_vm_instance(asb_vm_find(s->vm_name));
    if (!vm) { session_failed(slot, "The VM couldn't be created."); goto cleanup; }
    id = vm->unique_id;
    AcquireSRWLockExclusive(&g_lock);
    s->vm_id = id;
    ReleaseSRWLockExclusive(&g_lock);
    if (FAILED(vm_stream_configure(vm, TRUE, lan ? ASB_STREAM_NETWORK : ASB_STREAM_THIS_PC, software, err, _countof(err))))
        ui_log(L"Game Streaming: Moonlight isn't available for \"%s\": %s", s->vm_name, err);
    if (FAILED(vm_web_set(vm, TRUE, lan, err, _countof(err))))
        ui_log(L"Game Streaming: browser view isn't available for \"%s\": %s", s->vm_name, err);

    /* Setup, the domain join and its restart, then the sign-in. */
    session_state(slot, "starting", "Setting up Windows");
    deadline = GetTickCount64() + GS_START_TIMEOUT_MS;
    for (;;) {
        if (InterlockedCompareExchange(&s->stop, 0, 0) || g_shutting_down) goto cleanup;
        if (GetTickCount64() > deadline) {
            session_failed(slot, "Your VM took too long to sign you in. Try again, and if it happens again, ask your admin to check the App Sandbox log.");
            goto wait_to_clean;
        }
        vm = asb_find_vm_by_id(id);
        if (!vm || !vm->running) {
            /* Restarts keep the VM running; this one stopped for good. */
            session_failed(slot, "Your VM stopped while it was starting.");
            goto wait_to_clean;
        }
        if (InterlockedCompareExchange(&vm->session_signin_failed, 0, 0)) {
            session_failed(slot, "Your VM couldn't sign you in. Try again, and if it happens again, ask your admin to check the App Sandbox log.");
            goto wait_to_clean;
        }
        if (vm->agent_online && !vm->agent_initializing) {
            if (console_user(vm, s->user, &anyone)) break;
            session_state(slot, "starting", vm->session_logon[0] ? "Setting up Windows" : "Joining the domain and signing you in");
        }
        Sleep(3000);
    }
    vm_agent_send(vm, "session_signed_in", reply, sizeof(reply), 15000);

    if (strcmp(s->item, "desktop") != 0) {
        char cmd[160];
        session_state(slot, "starting", "Opening your game");
        /* Give the shell a moment: a link opened too early goes nowhere. */
        Sleep(10000);
        sprintf_s(cmd, sizeof(cmd), "session_launch:%s", s->item);
        vm = asb_find_vm_by_id(id);
        if (vm) vm_agent_send(vm, cmd, reply, sizeof(reply), 15000);
    }
    session_state(slot, "ready", "");
    ui_log(L"Game Streaming: \"%s\" is ready for %s.", s->vm_name, s->user);

    idle_since = GetTickCount64();
    for (;;) {
        ULONGLONG now;
        if (InterlockedCompareExchange(&s->stop, 0, 0) || g_shutting_down) break;
        Sleep(GS_WATCH_MS);
        vm = asb_find_vm_by_id(id);
        if (!vm || !vm->running) break;
        now = GetTickCount64();
        if (vm_web_connected(id) || now - vm_stream_last_activity(id) < 30000)
            idle_since = now;
        else if (idle_minutes > 0 && now - idle_since > (ULONGLONG)idle_minutes * 60000ull) {
            ui_log(L"Game Streaming: ending \"%s\": nobody has connected for %d minutes.", s->vm_name, idle_minutes);
            break;
        }
        /* The user's own sign-out ends the session. */
        if (vm->agent_online && !vm->agent_initializing) {
            console_user(vm, s->user, &anyone);
            signed_out = anyone ? 0 : signed_out + 1;
            if (signed_out >= GS_SIGNED_OUT_POLLS) break;
        }
    }
    goto cleanup;

wait_to_clean:
    /* Keep the failure on the dashboard until they've seen it. */
    {
        int i;
        for (i = 0; i < 60 && !InterlockedCompareExchange(&s->stop, 0, 0) && !g_shutting_down; i++) Sleep(5000);
    }

cleanup:
    SecureZeroMemory(start->password, sizeof(start->password));
    SecureZeroMemory(logon, sizeof(logon));
    free(start);
    session_state(slot, "ending", "");
    if (id) {
        vm = asb_find_vm_by_id(id);
        if (vm) vm_web_set(vm, FALSE, FALSE, NULL, 0);
        asb_session_clone_delete(id);
    }
    if (SUCCEEDED(shared_appliance_domain_computer_remove(s->vm_name)))
        ui_log(L"Game Streaming: removed \"%s\" and its computer account.", s->vm_name);
    AcquireSRWLockExclusive(&g_lock);
    ZeroMemory(s, sizeof(*s));
    ReleaseSRWLockExclusive(&g_lock);
    if (!g_shutting_down) prune_images();
    InterlockedDecrement(&g_threads);
    return 0;
}

static int find_session(const wchar_t *user)
{
    int i;
    for (i = 0; i < GS_MAX_SESSIONS; i++)
        if (g_sessions[i].used && _wcsicmp(g_sessions[i].user, user) == 0) return i;
    return -1;
}

static void utf8_message(char *message, int cap, const char *text)
{
    if (message && cap) strncpy_s(message, cap, text, _TRUNCATE);
}

HRESULT game_stream_session_start(const char *token, const wchar_t *user, const char *mode, const char *item,
                                  const wchar_t *password, char *message, int message_cap)
{
    GsStart *start;
    GsSession *s;
    HANDLE t;
    int i, slot = -1, active = 0;
    wchar_t suffix[8], image_dir[MAX_PATH];
    ULONGLONG need_bytes = 0;
    MEMORYSTATUSEX mem;
    VmInstance *source;

    if (!item_allowed(item)) {
        utf8_message(message, message_cap, "That game isn't in the list.");
        return E_INVALIDARG;
    }
    start = (GsStart *)calloc(1, sizeof(*start));
    if (!start) return E_OUTOFMEMORY;
    /* A password typed now, once the kept one has lapsed, is checked like a sign-in. */
    if (password && password[0]) {
        wchar_t why[300] = L"", group[65];
        HRESULT hr;
        AcquireSRWLockShared(&g_lock);
        wcscpy_s(group, _countof(group), g_group);
        ReleaseSRWLockShared(&g_lock);
        hr = shared_appliance_domain_validate(user, password, group, why, _countof(why));
        if (FAILED(hr)) {
            free(start);
            WideCharToMultiByte(CP_UTF8, 0, why, -1, message, message_cap, NULL, NULL);
            return hr;
        }
        wcsncpy_s(start->password, _countof(start->password), password, _TRUNCATE);
    } else if (!take_kept_password(token, start->password, _countof(start->password))) {
        free(start);
        utf8_message(message, message_cap, "password");
        return HRESULT_FROM_WIN32(ERROR_NOT_AUTHENTICATED);
    }

    source = asb_vm_instance(asb_vm_find(g_nominated));
    if (source) need_bytes = (ULONGLONG)source->ram_mb * 1024ull * 1024ull;
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem) && mem.ullAvailPhys < need_bytes) {
        SecureZeroMemory(start, sizeof(*start));
        free(start);
        utf8_message(message, message_cap, "This PC doesn't have enough free memory for another VM right now.");
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    }
    if (!random_hex_upper(suffix, 6)) {
        SecureZeroMemory(start, sizeof(*start));
        free(start);
        return E_FAIL;
    }

    AcquireSRWLockExclusive(&g_lock);
    if (!g_enabled || !g_image[0]) {
        utf8_message(message, message_cap, "Game Streaming isn't ready: ask your admin to build the image.");
    } else if (find_session(user) >= 0) {
        utf8_message(message, message_cap, "You already have a session.");
    } else {
        for (i = 0; i < GS_MAX_SESSIONS; i++) {
            if (g_sessions[i].used) active++;
            else if (slot < 0) slot = i;
        }
        if (active >= g_max_sessions || slot < 0 || asb_vm_count() >= ASB_MAX_VMS) {
            slot = -1;
            utf8_message(message, message_cap, "All the sessions are in use. Try again later.");
        }
    }
    if (slot < 0 || !g_enabled || !g_image[0]) {
        ReleaseSRWLockExclusive(&g_lock);
        SecureZeroMemory(start, sizeof(*start));
        free(start);
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    s = &g_sessions[slot];
    ZeroMemory(s, sizeof(*s));
    s->used = TRUE;
    swprintf_s(s->vm_name, _countof(s->vm_name), L"GS-%s", suffix);
    wcsncpy_s(s->user, _countof(s->user), user, _TRUNCATE);
    strncpy_s(s->item, sizeof(s->item), item, _TRUNCATE);
    strcpy_s(s->mode, sizeof(s->mode), strcmp(mode, "moonlight") == 0 ? "moonlight" : "browser");
    strcpy_s(s->state, sizeof(s->state), "starting");
    image_dir_of(g_image, image_dir, MAX_PATH);
    wcscpy_s(s->image, MAX_PATH, image_dir);
    ReleaseSRWLockExclusive(&g_lock);

    start->slot = slot;
    InterlockedIncrement(&g_threads);
    t = CreateThread(NULL, 0, session_thread, start, 0, NULL);
    if (!t) {
        InterlockedDecrement(&g_threads);
        SecureZeroMemory(start, sizeof(*start));
        free(start);
        AcquireSRWLockExclusive(&g_lock);
        ZeroMemory(s, sizeof(*s));
        ReleaseSRWLockExclusive(&g_lock);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(t);
    return S_OK;
}

HRESULT game_stream_session_end(const wchar_t *user)
{
    int slot;
    AcquireSRWLockShared(&g_lock);
    slot = find_session(user);
    if (slot >= 0) InterlockedExchange(&g_sessions[slot].stop, 1);
    ReleaseSRWLockShared(&g_lock);
    return slot >= 0 ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT game_stream_end_session_of_vm(const wchar_t *vm_name)
{
    int i;
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < GS_MAX_SESSIONS; i++) {
        if (g_sessions[i].used && _wcsicmp(g_sessions[i].vm_name, vm_name) == 0) {
            InterlockedExchange(&g_sessions[i].stop, 1);
            hr = S_OK;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return hr;
}

HRESULT game_stream_session_pair(const wchar_t *user, const char *pin, char *message, int message_cap)
{
    UINT64 id = game_stream_session_vm(user, NULL, 0);
    VmInstance *vm = id ? asb_find_vm_by_id(id) : NULL;
    char name[64];
    if (!vm) {
        utf8_message(message, message_cap, "Your session isn't ready yet.");
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
    WideCharToMultiByte(CP_UTF8, 0, user, -1, name, sizeof(name), NULL, NULL);
    return vm_stream_pair(vm, pin, name, message, message_cap);
}

UINT64 game_stream_session_vm(const wchar_t *user, char *name, size_t name_chars)
{
    int slot;
    UINT64 id = 0;
    AcquireSRWLockShared(&g_lock);
    slot = find_session(user);
    if (slot >= 0 && strcmp(g_sessions[slot].state, "ready") == 0) {
        id = g_sessions[slot].vm_id;
        if (name) WideCharToMultiByte(CP_UTF8, 0, g_sessions[slot].vm_name, -1, name, (int)name_chars, NULL, NULL);
    }
    ReleaseSRWLockShared(&g_lock);
    return id;
}

int game_stream_session_json(const wchar_t *user, char *out, int cap)
{
    int slot, pos = 0;
    GsSession copy;
    VmInstance *vm;
    AcquireSRWLockShared(&g_lock);
    slot = find_session(user);
    if (slot >= 0) copy = g_sessions[slot];
    ReleaseSRWLockShared(&g_lock);
    if (slot < 0) return sprintf_s(out, cap, "{\"state\":\"none\"}");
    pos += sprintf_s(out + pos, cap - pos, "{");
    pos = json_append_utf8(out, cap, pos, "state", copy.state);
    pos += sprintf_s(out + pos, cap - pos, ",");
    pos = json_append_utf8(out, cap, pos, "detail", copy.detail);
    pos += sprintf_s(out + pos, cap - pos, ",");
    pos = json_append_utf8(out, cap, pos, "item", copy.item);
    pos += sprintf_s(out + pos, cap - pos, ",");
    pos = json_append_utf8(out, cap, pos, "mode", copy.mode);
    pos += sprintf_s(out + pos, cap - pos, ",");
    pos = json_append_wide(out, cap, pos, "vm", copy.vm_name);
    vm = copy.vm_id ? asb_find_vm_by_id(copy.vm_id) : NULL;
    if (vm && strcmp(copy.state, "ready") == 0) {
        pos += sprintf_s(out + pos, cap - pos, ",\"moonlightPort\":%lu,\"moonlightAddresses\":[",
                         (unsigned long)vm->stream_port);
        if (vm->stream_exposure == ASB_STREAM_NETWORK)
            pos = vm_stream_append_host_addresses(out, cap, pos);
        else
            pos += sprintf_s(out + pos, cap - pos, "\"127.0.0.1\"");
        pos += sprintf_s(out + pos, cap - pos, "]");
    }
    pos += sprintf_s(out + pos, cap - pos, "}");
    return pos;
}

/* ---- Admin ---- */

HRESULT game_stream_configure(const wchar_t *vm_name, int max_sessions, int idle_minutes,
                              const wchar_t *group, BOOL lan, wchar_t *err, size_t err_chars)
{
    SharedApplianceStatus appliance;
    VmInstance *vm = asb_vm_instance(asb_vm_find(vm_name ? vm_name : L""));
    const wchar_t *why = vm_problem(vm);
    wchar_t root[MAX_PATH] = L"", message[300] = L"";
    HRESULT hr;
    BOOL changed_vm;

    if (err && err_chars) err[0] = 0;
    asb_shared_appliance_get_status(&appliance);
    if (!appliance.domain_enabled) {
        if (err) wcsncpy_s(err, err_chars, L"Set up the domain controller (Domain) first.", _TRUNCATE);
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
    if (why) {
        if (err) wcsncpy_s(err, err_chars, why, _TRUNCATE);
        return E_INVALIDARG;
    }
    if (max_sessions < 1 || max_sessions > GS_MAX_SESSIONS || idle_minutes < 0 || idle_minutes > 24 * 60 ||
        (group && wcslen(group) > 64)) {
        if (err) swprintf_s(err, err_chars, L"Sessions must be 1 to %d, and the idle time 0 to 1440 minutes.", GS_MAX_SESSIONS);
        return E_INVALIDARG;
    }
    if (vm->storage_root[0]) {
        wchar_t *slash;
        wcscpy_s(root, MAX_PATH, vm->storage_root);
        slash = wcsrchr(root, L'\\');
        if (slash) { *slash = 0; wcscat_s(root, MAX_PATH, L"\\GameStreaming"); }
        else root[0] = 0;
    }
    if (!root[0]) {
        if (!GetEnvironmentVariableW(L"ProgramData", root, MAX_PATH)) wcscpy_s(root, MAX_PATH, L"C:\\ProgramData");
        wcscat_s(root, MAX_PATH, L"\\AppSandbox\\GameStreaming");
    }

    AcquireSRWLockExclusive(&g_lock);
    changed_vm = _wcsicmp(g_nominated, vm_name) != 0;
    g_enabled = TRUE;
    wcscpy_s(g_nominated, _countof(g_nominated), vm_name);
    g_max_sessions = max_sessions;
    g_idle_minutes = idle_minutes;
    wcsncpy_s(g_group, _countof(g_group), group ? group : L"", _TRUNCATE);
    g_lan = lan;
    /* A new VM means a new image; running sessions keep the old one until they end. */
    if (!g_root[0]) wcscpy_s(g_root, MAX_PATH, root);
    if (changed_vm) g_image[0] = 0;
    ReleaseSRWLockExclusive(&g_lock);
    CreateDirectoryW(g_root, NULL);
    hr = vm_web_dashboard_set(TRUE, lan, message, _countof(message));
    if (FAILED(hr) && err) wcsncpy_s(err, err_chars, message, _TRUNCATE);
    asb_save();
    if (changed_vm) prune_images();
    return hr;
}

static DWORD WINAPI turn_off_thread(LPVOID param)
{
    int i, tries;
    wchar_t root[MAX_PATH];
    (void)param;
    for (tries = 0; tries < 120; tries++) {
        BOOL busy = InterlockedCompareExchange(&g_build.busy, 0, 0) != 0;
        AcquireSRWLockShared(&g_lock);
        for (i = 0; i < GS_MAX_SESSIONS; i++) if (g_sessions[i].used) busy = TRUE;
        ReleaseSRWLockShared(&g_lock);
        if (!busy) break;
        Sleep(1000);
    }
    AcquireSRWLockExclusive(&g_lock);
    wcscpy_s(root, MAX_PATH, g_root);
    g_root[0] = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (root[0]) asb_session_clone_remove_leftover(NULL, root);
    /* Anything an earlier crash left in the directory goes too. */
    shared_appliance_domain_computer_remove(L"GS-*");
    asb_save();
    InterlockedDecrement(&g_threads);
    return 0;
}

HRESULT game_stream_turn_off(void)
{
    int i;
    HANDLE t;
    AcquireSRWLockExclusive(&g_lock);
    g_enabled = FALSE;
    g_nominated[0] = 0;
    g_image[0] = 0;
    g_group[0] = 0;
    g_lan = FALSE;
    for (i = 0; i < GS_MAX_LOGINS; i++) SecureZeroMemory(&g_logins[i], sizeof(g_logins[i]));
    for (i = 0; i < GS_MAX_SESSIONS; i++)
        if (g_sessions[i].used) InterlockedExchange(&g_sessions[i].stop, 1);
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_build.cancel, 1);
    vm_web_dashboard_set(FALSE, FALSE, NULL, 0);
    asb_save();
    InterlockedIncrement(&g_threads);
    t = CreateThread(NULL, 0, turn_off_thread, NULL, 0, NULL);
    if (!t) {
        InterlockedDecrement(&g_threads);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(t);
    return S_OK;
}

int game_stream_status_json(char *out, int cap)
{
    int pos = 0, i, games = 0, listed = 0;
    char *catalog = read_catalog(), *p;
    GsSession copy[GS_MAX_SESSIONS];
    wchar_t nominated[256], group[65], phase[160], error[400];
    BOOL enabled, lan, image;
    int max_sessions, idle;

    if (catalog) {
        for (p = catalog; (p = strstr(p, "\"id\":\"")) != NULL; p += 6) games++;
        free(catalog);
    }
    AcquireSRWLockShared(&g_lock);
    enabled = g_enabled;
    lan = g_lan;
    image = g_image[0] != 0;
    max_sessions = g_max_sessions;
    idle = g_idle_minutes;
    wcscpy_s(nominated, _countof(nominated), g_nominated);
    wcscpy_s(group, _countof(group), g_group);
    wcscpy_s(phase, _countof(phase), g_build.phase);
    wcscpy_s(error, _countof(error), g_build.error);
    memcpy(copy, g_sessions, sizeof(copy));
    ReleaseSRWLockShared(&g_lock);

    pos += sprintf_s(out + pos, cap - pos, "{\"enabled\":%s,\"lan\":%s,\"maxSessions\":%d,\"idleMinutes\":%d,\"imageReady\":%s,\"games\":%d,\"building\":%s,\"buildPct\":%ld,",
                     enabled ? "true" : "false", lan ? "true" : "false", max_sessions, idle,
                     image ? "true" : "false", games,
                     InterlockedCompareExchange(&g_build.busy, 0, 0) ? "true" : "false",
                     InterlockedCompareExchange(&g_build.pct, 0, 0));
    pos = json_append_wide(out, cap, pos, "vm", nominated);
    pos += sprintf_s(out + pos, cap - pos, ",");
    pos = json_append_wide(out, cap, pos, "group", group);
    pos += sprintf_s(out + pos, cap - pos, ",");
    pos = json_append_wide(out, cap, pos, "phase", phase);
    pos += sprintf_s(out + pos, cap - pos, ",");
    pos = json_append_wide(out, cap, pos, "error", error);
    pos += sprintf_s(out + pos, cap - pos, ",\"dashboard\":");
    pos = vm_web_dashboard_json(out, cap, pos);
    pos += sprintf_s(out + pos, cap - pos, ",\"sessions\":[");
    for (i = 0; i < GS_MAX_SESSIONS; i++) {
        if (!copy[i].used) continue;
        pos += sprintf_s(out + pos, cap - pos, "%s{", listed++ ? "," : "");
        pos = json_append_wide(out, cap, pos, "vm", copy[i].vm_name);
        pos += sprintf_s(out + pos, cap - pos, ",");
        pos = json_append_wide(out, cap, pos, "user", copy[i].user);
        pos += sprintf_s(out + pos, cap - pos, ",");
        pos = json_append_utf8(out, cap, pos, "item", copy[i].item);
        pos += sprintf_s(out + pos, cap - pos, ",");
        pos = json_append_utf8(out, cap, pos, "state", copy[i].state);
        pos += sprintf_s(out + pos, cap - pos, ",");
        pos = json_append_utf8(out, cap, pos, "detail", copy[i].detail);
        pos += sprintf_s(out + pos, cap - pos, "}");
    }
    pos += sprintf_s(out + pos, cap - pos, "]}");
    return pos;
}

/* ---- Lifecycle ---- */

/* Sessions end with the app, so any duplicate still on disk was left by a crash. */
static DWORD WINAPI sweep_thread(LPVOID param)
{
    wchar_t sessions[MAX_PATH], pattern[MAX_PATH], dir[MAX_PATH];
    WIN32_FIND_DATAW d;
    HANDLE h;
    (void)param;
    AcquireSRWLockShared(&g_lock);
    swprintf_s(sessions, MAX_PATH, L"%s\\sessions", g_root);
    ReleaseSRWLockShared(&g_lock);
    swprintf_s(pattern, MAX_PATH, L"%s\\GS-*", sessions);
    h = FindFirstFileW(pattern, &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            swprintf_s(dir, MAX_PATH, L"%s\\%s", sessions, d.cFileName);
            ui_log(L"Game Streaming: removing \"%s\", left over from an earlier run.", d.cFileName);
            asb_session_clone_remove_leftover(d.cFileName, dir);
            shared_appliance_domain_computer_remove(d.cFileName);
        } while (FindNextFileW(h, &d) && !g_shutting_down);
        FindClose(h);
    }
    prune_images();
    InterlockedDecrement(&g_threads);
    return 0;
}

void game_stream_init(void)
{
    HANDLE t;
    wchar_t message[300];
    if (!g_enabled) return;
    if (FAILED(vm_web_dashboard_set(TRUE, g_lan, message, _countof(message))))
        ui_log(L"Game Streaming: the dashboard couldn't start: %s", message);
    InterlockedIncrement(&g_threads);
    t = CreateThread(NULL, 0, sweep_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedDecrement(&g_threads);
}

void game_stream_shutdown(void)
{
    int i;
    InterlockedExchange(&g_shutting_down, 1);
    InterlockedExchange(&g_build.cancel, 1);
    for (i = 0; i < 900 && InterlockedCompareExchange(&g_threads, 0, 0) > 0; i++) Sleep(100);
    AcquireSRWLockExclusive(&g_lock);
    SecureZeroMemory(g_logins, sizeof(g_logins));
    ReleaseSRWLockExclusive(&g_lock);
}
