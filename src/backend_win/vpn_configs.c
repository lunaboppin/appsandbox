#define _CRT_SECURE_NO_WARNINGS
#include "vpn_configs.h"
#include "ui.h"
#include <windows.h>
#include <wincrypt.h>
#include <sddl.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

#pragma comment(lib, "crypt32.lib")

/* Metadata in %ProgramData%\AppSandbox\vpn-configs.cfg; each config's keys in
   %ProgramData%\AppSandbox\vpn\<id>.bin, DPAPI (machine scope) and readable
   by SYSTEM and Administrators only. */

typedef struct {
    char private_key[64];
    char address[64];
    char mtu[8];
    char public_key[64];
    char preshared_key[64];
    char endpoint[128];
    char keepalive[8];
    char dns[128];
} ParsedConf;

static AsbVpnConfigInfo g_configs[ASB_MAX_VPN_CONFIGS];
static int g_config_count;
static CRITICAL_SECTION g_cs;
static BOOL g_initialized;

static void vpn_root(wchar_t *out, size_t chars)
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(out, chars, L"%s\\AppSandbox", base);
}

static void list_path(wchar_t *out, size_t chars)
{
    wchar_t root[MAX_PATH];
    vpn_root(root, _countof(root));
    swprintf_s(out, chars, L"%s\\vpn-configs.cfg", root);
}

static void secret_path(const wchar_t *id, wchar_t *out, size_t chars)
{
    wchar_t root[MAX_PATH];
    vpn_root(root, _countof(root));
    swprintf_s(out, chars, L"%s\\vpn\\%s.bin", root, id);
}

static HRESULT admin_only_attributes(SECURITY_ATTRIBUTES *sa, PSECURITY_DESCRIPTOR *sd)
{
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, sd, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    sa->nLength = sizeof(*sa);
    sa->lpSecurityDescriptor = *sd;
    sa->bInheritHandle = FALSE;
    return S_OK;
}

static void save_list_locked(void)
{
    wchar_t path[MAX_PATH];
    FILE *f = NULL;
    int i;
    list_path(path, _countof(path));
    if (_wfopen_s(&f, path, L"w,ccs=UTF-8") != 0 || !f) {
        ui_log(L"VPN: could not save the config list.");
        return;
    }
    fwprintf(f, L"Version=1\n");
    for (i = 0; i < g_config_count; i++) {
        const AsbVpnConfigInfo *c = &g_configs[i];
        fwprintf(f, L"[Config]\nId=%s\nName=%s\nEndpoint=%s\nAddress=%s\nDns=%s\n",
                 c->id, c->name, c->endpoint, c->address, c->dns);
    }
    fclose(f);
}

void vpn_configs_init(void)
{
    wchar_t path[MAX_PATH], line[512];
    FILE *f = NULL;
    AsbVpnConfigInfo *cur = NULL;
    if (!g_initialized) {
        InitializeCriticalSection(&g_cs);
        g_initialized = TRUE;
    }
    EnterCriticalSection(&g_cs);
    g_config_count = 0;
    list_path(path, _countof(path));
    if (_wfopen_s(&f, path, L"r,ccs=UTF-8") == 0 && f) {
        while (fgetws(line, _countof(line), f)) {
            size_t len = wcslen(line);
            while (len && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) line[--len] = L'\0';
            if (wcscmp(line, L"[Config]") == 0) {
                cur = g_config_count < ASB_MAX_VPN_CONFIGS ? &g_configs[g_config_count++] : NULL;
                if (cur) ZeroMemory(cur, sizeof(*cur));
            } else if (cur && wcsncmp(line, L"Id=", 3) == 0)
                wcscpy_s(cur->id, _countof(cur->id), line + 3);
            else if (cur && wcsncmp(line, L"Name=", 5) == 0)
                wcscpy_s(cur->name, _countof(cur->name), line + 5);
            else if (cur && wcsncmp(line, L"Endpoint=", 9) == 0)
                wcscpy_s(cur->endpoint, _countof(cur->endpoint), line + 9);
            else if (cur && wcsncmp(line, L"Address=", 8) == 0)
                wcscpy_s(cur->address, _countof(cur->address), line + 8);
            else if (cur && wcsncmp(line, L"Dns=", 4) == 0)
                wcscpy_s(cur->dns, _countof(cur->dns), line + 4);
        }
        fclose(f);
    }
    /* Drop entries whose key file is gone. */
    {
        int i, kept = 0;
        for (i = 0; i < g_config_count; i++) {
            wchar_t secret[MAX_PATH];
            secret_path(g_configs[i].id, secret, _countof(secret));
            if (!g_configs[i].id[0] || GetFileAttributesW(secret) == INVALID_FILE_ATTRIBUTES)
                continue;
            if (kept != i) g_configs[kept] = g_configs[i];
            kept++;
        }
        g_config_count = kept;
    }
    LeaveCriticalSection(&g_cs);
    if (g_config_count)
        ui_log(L"Loaded %d VPN config(s).", g_config_count);
}

int vpn_configs_count(void) { return g_config_count; }

BOOL vpn_configs_get(int index, AsbVpnConfigInfo *out)
{
    BOOL ok = FALSE;
    if (!out || !g_initialized) return FALSE;
    EnterCriticalSection(&g_cs);
    if (index >= 0 && index < g_config_count) { *out = g_configs[index]; ok = TRUE; }
    LeaveCriticalSection(&g_cs);
    return ok;
}

BOOL vpn_configs_find(const wchar_t *id, AsbVpnConfigInfo *out)
{
    int i;
    BOOL ok = FALSE;
    if (!id || !id[0] || !g_initialized) return FALSE;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_config_count; i++) {
        if (_wcsicmp(g_configs[i].id, id) == 0) {
            if (out) *out = g_configs[i];
            ok = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

/* ---- Parsing ---- */

static void set_error(wchar_t *error, size_t chars, const wchar_t *text)
{
    if (error && chars) wcscpy_s(error, chars, text);
}

static char *trim(char *s)
{
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
    return s;
}

/* Base64 WireGuard key: 43 characters from the base64 alphabet plus '='. */
static BOOL valid_key(const char *k)
{
    int i;
    if (strlen(k) != 44 || k[43] != '=') return FALSE;
    for (i = 0; i < 43; i++) {
        char c = k[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/')) return FALSE;
    }
    return TRUE;
}

static BOOL valid_ipv4(const char *s, BOOL allow_prefix)
{
    int a, b, c, d, p = 32, n = 0;
    char tail = 0;
    if (allow_prefix && strchr(s, '/'))
        n = sscanf_s(s, "%d.%d.%d.%d/%d%c", &a, &b, &c, &d, &p, &tail, 1);
    else
        n = sscanf_s(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail, 1);
    if (n != (allow_prefix && strchr(s, '/') ? 5 : 4)) return FALSE;
    return a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 &&
           d >= 0 && d <= 255 && p >= 0 && p <= 32;
}

/* host:port with a DNS name or IPv4 host. IPv6 endpoints are not supported:
   the gateway's uplink is IPv4-only. */
static BOOL valid_endpoint(const char *s)
{
    const char *colon = strrchr(s, ':');
    const char *p;
    int port;
    if (!colon || colon == s || strlen(s) >= 128 || strchr(s, '[')) return FALSE;
    for (p = s; p < colon; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return FALSE;
    }
    port = atoi(colon + 1);
    for (p = colon + 1; *p; p++) if (*p < '0' || *p > '9') return FALSE;
    return port > 0 && port < 65536;
}

static BOOL all_digits(const char *s, int max_len)
{
    int n = 0;
    if (!*s) return FALSE;
    for (; *s; s++, n++) if (*s < '0' || *s > '9' || n >= max_len) return FALSE;
    return TRUE;
}

/* Keep only the IPv4 entries of a comma-separated list. */
static void keep_ipv4(const char *value, char *out, size_t out_size, BOOL allow_prefix)
{
    char copy[256], *ctx = NULL, *item;
    out[0] = '\0';
    strcpy_s(copy, sizeof(copy), value);
    for (item = strtok_s(copy, ",", &ctx); item; item = strtok_s(NULL, ",", &ctx)) {
        item = trim(item);
        if (!valid_ipv4(item, allow_prefix)) continue;
        if (strlen(out) + strlen(item) + 2 >= out_size) break;
        if (out[0]) strcat_s(out, out_size, ",");
        strcat_s(out, out_size, item);
    }
}

static HRESULT parse_conf(const char *text, ParsedConf *conf,
                          wchar_t *error, size_t error_chars)
{
    char *copy, *ctx = NULL, *line;
    enum { NONE, IFACE, PEER, OTHER } section = NONE;
    int peers = 0;
    size_t len = strlen(text);
    ZeroMemory(conf, sizeof(*conf));
    copy = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (!copy) return E_OUTOFMEMORY;
    memcpy(copy, text, len + 1);
    for (line = strtok_s(copy, "\n", &ctx); line; line = strtok_s(NULL, "\n", &ctx)) {
        char *key, *value, *eq, *hash;
        line = trim(line);
        if (!*line || *line == '#' || *line == ';') continue;
        if (*line == '[') {
            if (_stricmp(line, "[Interface]") == 0) section = IFACE;
            else if (_stricmp(line, "[Peer]") == 0) { section = PEER; peers++; }
            else section = OTHER;
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        key = trim(line);
        value = trim(eq + 1);
        hash = strchr(value, '#');
        if (hash) { *hash = '\0'; value = trim(value); }
        if (section == IFACE) {
            if (_stricmp(key, "PrivateKey") == 0) strncpy_s(conf->private_key, sizeof(conf->private_key), value, _TRUNCATE);
            else if (_stricmp(key, "Address") == 0) keep_ipv4(value, conf->address, sizeof(conf->address), TRUE);
            else if (_stricmp(key, "DNS") == 0) keep_ipv4(value, conf->dns, sizeof(conf->dns), FALSE);
            else if (_stricmp(key, "MTU") == 0) strncpy_s(conf->mtu, sizeof(conf->mtu), value, _TRUNCATE);
        } else if (section == PEER && peers == 1) {
            if (_stricmp(key, "PublicKey") == 0) strncpy_s(conf->public_key, sizeof(conf->public_key), value, _TRUNCATE);
            else if (_stricmp(key, "PresharedKey") == 0) strncpy_s(conf->preshared_key, sizeof(conf->preshared_key), value, _TRUNCATE);
            else if (_stricmp(key, "Endpoint") == 0) strncpy_s(conf->endpoint, sizeof(conf->endpoint), value, _TRUNCATE);
            else if (_stricmp(key, "PersistentKeepalive") == 0) strncpy_s(conf->keepalive, sizeof(conf->keepalive), value, _TRUNCATE);
        }
    }
    SecureZeroMemory(copy, len);
    HeapFree(GetProcessHeap(), 0, copy);

    if (!valid_key(conf->private_key)) { set_error(error, error_chars, L"[Interface] PrivateKey is missing or not a WireGuard key."); return E_INVALIDARG; }
    if (!conf->address[0]) { set_error(error, error_chars, L"[Interface] Address has no IPv4 address."); return E_INVALIDARG; }
    if (peers < 1) { set_error(error, error_chars, L"The config has no [Peer] section."); return E_INVALIDARG; }
    if (!valid_key(conf->public_key)) { set_error(error, error_chars, L"[Peer] PublicKey is missing or not a WireGuard key."); return E_INVALIDARG; }
    if (conf->preshared_key[0] && !valid_key(conf->preshared_key)) { set_error(error, error_chars, L"[Peer] PresharedKey is not a WireGuard key."); return E_INVALIDARG; }
    if (!valid_endpoint(conf->endpoint)) { set_error(error, error_chars, L"[Peer] Endpoint must be host:port (IPv6 endpoints are not supported)."); return E_INVALIDARG; }
    if (conf->mtu[0] && !all_digits(conf->mtu, 5)) conf->mtu[0] = '\0';
    if (!conf->keepalive[0] || !all_digits(conf->keepalive, 5)) strcpy_s(conf->keepalive, sizeof(conf->keepalive), "25");
    return S_OK;
}

static void format_gateway_conf(const ParsedConf *c, char *out, size_t out_size)
{
    out[0] = '\0';
    sprintf_s(out, out_size,
        "[Interface]\nPrivateKey = %s\nAddress = %s\n%s%s%sTable = off\n"
        "[Peer]\nPublicKey = %s\n%s%s%sEndpoint = %s\nAllowedIPs = 0.0.0.0/0\n"
        "PersistentKeepalive = %s\n",
        c->private_key, c->address,
        c->mtu[0] ? "MTU = " : "", c->mtu, c->mtu[0] ? "\n" : "",
        c->public_key,
        c->preshared_key[0] ? "PresharedKey = " : "", c->preshared_key,
        c->preshared_key[0] ? "\n" : "",
        c->endpoint, c->keepalive);
}

static BOOL valid_name(const wchar_t *name)
{
    size_t i, n = name ? wcslen(name) : 0;
    if (n == 0 || n >= 64) return FALSE;
    for (i = 0; i < n; i++) if (name[i] < 0x20 || name[i] == 0x7f) return FALSE;
    return TRUE;
}

HRESULT vpn_configs_import(const wchar_t *name, const wchar_t *text,
                           wchar_t *id_out, size_t id_chars,
                           wchar_t *error, size_t error_chars)
{
    ParsedConf conf;
    char *utf8 = NULL, gateway[1024];
    int bytes;
    GUID guid;
    wchar_t id[ASB_VPN_ID_CHARS], dir[MAX_PATH], secret[MAX_PATH];
    DATA_BLOB input, output;
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR sd = NULL;
    HANDLE file;
    DWORD written;
    HRESULT hr;
    AsbVpnConfigInfo info;

    if (error && error_chars) error[0] = L'\0';
    if (!g_initialized) vpn_configs_init();
    if (!valid_name(name)) { set_error(error, error_chars, L"Give the config a name of 1-63 characters."); return E_INVALIDARG; }
    if (!text || !text[0]) { set_error(error, error_chars, L"The config is empty."); return E_INVALIDARG; }
    if (g_config_count >= ASB_MAX_VPN_CONFIGS) { set_error(error, error_chars, L"The VPN config limit (16) is reached."); return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES); }

    bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0 || bytes > 64 * 1024) { set_error(error, error_chars, L"The config is too large."); return E_INVALIDARG; }
    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!utf8) return E_OUTOFMEMORY;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, bytes, NULL, NULL);
    hr = parse_conf(utf8, &conf, error, error_chars);
    SecureZeroMemory(utf8, bytes);
    HeapFree(GetProcessHeap(), 0, utf8);
    if (FAILED(hr)) { SecureZeroMemory(&conf, sizeof(conf)); return hr; }
    format_gateway_conf(&conf, gateway, sizeof(gateway));

    CoCreateGuid(&guid);
    swprintf_s(id, _countof(id), L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
               guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
               guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
               guid.Data4[6], guid.Data4[7]);

    vpn_root(dir, _countof(dir));
    wcscat_s(dir, _countof(dir), L"\\vpn");
    hr = admin_only_attributes(&sa, &sd);
    if (FAILED(hr)) goto done;
    if (!CreateDirectoryW(dir, &sa) && GetLastError() != ERROR_ALREADY_EXISTS) {
        hr = HRESULT_FROM_WIN32(GetLastError()); goto done;
    }
    input.pbData = (BYTE *)gateway;
    input.cbData = (DWORD)strlen(gateway) + 1;
    ZeroMemory(&output, sizeof(output));
    if (!CryptProtectData(&input, L"AppSandbox VPN config", NULL, NULL, NULL,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        hr = HRESULT_FROM_WIN32(GetLastError()); goto done;
    }
    secret_path(id, secret, _countof(secret));
    file = CreateFileW(secret, GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        hr = HRESULT_FROM_WIN32(GetLastError());
    } else {
        hr = WriteFile(file, output.pbData, output.cbData, &written, NULL) &&
             written == output.cbData ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(file);
        if (FAILED(hr)) DeleteFileW(secret);
    }
    LocalFree(output.pbData);
    if (FAILED(hr)) goto done;

    ZeroMemory(&info, sizeof(info));
    wcscpy_s(info.id, _countof(info.id), id);
    wcscpy_s(info.name, _countof(info.name), name);
    MultiByteToWideChar(CP_UTF8, 0, conf.endpoint, -1, info.endpoint, _countof(info.endpoint));
    MultiByteToWideChar(CP_UTF8, 0, conf.address, -1, info.address, _countof(info.address));
    MultiByteToWideChar(CP_UTF8, 0, conf.dns, -1, info.dns, _countof(info.dns));
    EnterCriticalSection(&g_cs);
    g_configs[g_config_count++] = info;
    save_list_locked();
    LeaveCriticalSection(&g_cs);
    if (id_out && id_chars) wcscpy_s(id_out, id_chars, id);
    ui_log(L"VPN: imported config \"%s\" (%s).", info.name, info.endpoint);

done:
    if (sd) LocalFree(sd);
    SecureZeroMemory(gateway, sizeof(gateway));
    SecureZeroMemory(&conf, sizeof(conf));
    if (FAILED(hr) && !(error && error[0]))
        set_error(error, error_chars, L"The config could not be saved.");
    return hr;
}

HRESULT vpn_configs_delete(const wchar_t *id)
{
    int i;
    wchar_t secret[MAX_PATH];
    if (!id || !id[0] || !g_initialized) return E_INVALIDARG;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_config_count; i++)
        if (_wcsicmp(g_configs[i].id, id) == 0) break;
    if (i == g_config_count) { LeaveCriticalSection(&g_cs); return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    secret_path(g_configs[i].id, secret, _countof(secret));
    ui_log(L"VPN: deleted config \"%s\".", g_configs[i].name);
    for (; i + 1 < g_config_count; i++) g_configs[i] = g_configs[i + 1];
    g_config_count--;
    save_list_locked();
    LeaveCriticalSection(&g_cs);
    DeleteFileW(secret);
    return S_OK;
}

HRESULT vpn_configs_gateway_conf(const wchar_t *id, char *out, size_t out_size)
{
    wchar_t secret[MAX_PATH];
    HANDLE file;
    BYTE buffer[4096];
    DWORD read = 0;
    DATA_BLOB input, output;
    HRESULT hr = S_OK;
    if (!out || !out_size || !vpn_configs_find(id, NULL)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    secret_path(id, secret, _countof(secret));
    file = CreateFileW(secret, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL) || read == 0)
        hr = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_INVALID_DATA);
    CloseHandle(file);
    if (FAILED(hr)) return hr;
    input.pbData = buffer;
    input.cbData = read;
    ZeroMemory(&output, sizeof(output));
    if (!CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return HRESULT_FROM_WIN32(GetLastError());
    if (output.cbData == 0 || output.cbData > out_size || output.pbData[output.cbData - 1] != 0)
        hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    else
        memcpy(out, output.pbData, output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return hr;
}
