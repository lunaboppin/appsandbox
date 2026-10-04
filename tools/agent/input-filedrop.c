/*
 * input-filedrop.c — receives files dragged onto the host display window and
 * writes them to the console user's Desktop. Part of appsandbox-input.exe
 * (SYSTEM, console session), which is why it impersonates the user while
 * writing: the files are then owned by the user, exactly as if they had
 * copied them in themselves.
 *
 * Channel ASB_CH_FILEDROP (host -> guest). Per connection, a sequence of
 * entries, each a FileDropHeader followed by a UTF-16 relative path and,
 * for files, `size` bytes of data. Directories come before their contents.
 * FILEDROP_END finishes the batch; the guest answers with FileDropResult.
 *
 * Names are relative ("folder\\sub\\file.txt"). Anything absolute, with a
 * drive or stream colon, "." / ".." components or reserved characters is
 * rejected (its data is still drained so the stream stays aligned). A
 * top-level name that already exists on the Desktop becomes "name (2)".
 */

#include "../transport/asb_transport.h"
#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

#define FILEDROP_MAGIC      0x44465341  /* "ASFD" little-endian */
#define FILEDROP_FILE       1
#define FILEDROP_DIR        2
#define FILEDROP_END        3
#define FILEDROP_MAX_NAME   (1024 * sizeof(wchar_t))
#define FILEDROP_CHUNK      (256 * 1024)
#define FILEDROP_MAX_TOPS   256

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 type;
    UINT32 name_bytes;      /* UTF-16 bytes, no terminator */
    UINT32 reserved;
    UINT64 size;            /* file data bytes that follow the name */
} FileDropHeader;

typedef struct {
    UINT32 magic;
    UINT32 files_ok;
    UINT32 errors;
} FileDropResult;
#pragma pack(pop)

void input_log(const char *fmt, ...);   /* appsandbox-input.c */

/* Top-level names seen in this batch and what they were renamed to. */
typedef struct {
    wchar_t from[MAX_PATH];
    wchar_t to[MAX_PATH];
} TopMap;

static int recv_all(AsbConn *c, void *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = asb_recv(c, (char *)buf + got, len - got);
        if (n <= 0)
            return 0;
        got += n;
    }
    return 1;
}

/* Relative, no drive/stream colon, no empty, "." or ".." components, no
   reserved characters. Converts '/' to '\\' in place. */
static BOOL name_is_safe(wchar_t *name)
{
    wchar_t *p, *comp;
    if (!name[0] || name[0] == L'\\' || name[0] == L'/')
        return FALSE;
    for (p = name; *p; p++) {
        if (*p == L'/')
            *p = L'\\';
        if (*p < 32 || wcschr(L":*?\"<>|", *p))
            return FALSE;
    }
    comp = name;
    for (;;) {
        wchar_t *end = wcschr(comp, L'\\');
        size_t len = end ? (size_t)(end - comp) : wcslen(comp);
        if (len == 0 ||
            (len == 1 && comp[0] == L'.') ||
            (len == 2 && comp[0] == L'.' && comp[1] == L'.'))
            return FALSE;
        /* Trailing dots/spaces are silently stripped by Win32 and can alias
           another name. */
        if (comp[len - 1] == L'.' || comp[len - 1] == L' ')
            return FALSE;
        if (!end)
            break;
        comp = end + 1;
    }
    return TRUE;
}

/* A Desktop name not already in use: "name", "name (2)", "name (3)"... */
static BOOL unique_top(const wchar_t *desktop, const wchar_t *top, BOOL is_dir,
                       wchar_t *out, size_t out_chars)
{
    wchar_t base[MAX_PATH], ext[MAX_PATH], full[MAX_PATH * 2];
    const wchar_t *dot = is_dir ? NULL : wcsrchr(top, L'.');
    int n;

    if (dot && dot != top) {
        wcsncpy_s(base, MAX_PATH, top, (size_t)(dot - top));
        wcscpy_s(ext, MAX_PATH, dot);
    } else {
        wcscpy_s(base, MAX_PATH, top);
        ext[0] = L'\0';
    }
    for (n = 1; n < 1000; n++) {
        if (n == 1)
            swprintf_s(out, out_chars, L"%s%s", base, ext);
        else
            swprintf_s(out, out_chars, L"%s (%d)%s", base, n, ext);
        swprintf_s(full, MAX_PATH * 2, L"%s\\%s", desktop, out);
        if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
            return TRUE;
    }
    return FALSE;
}

/* Map name's first component through the batch's rename table, adding a new
   unique name the first time a top-level item is seen. */
static BOOL map_name(const wchar_t *desktop, const wchar_t *name, BOOL is_dir,
                     TopMap *tops, int *ntops, wchar_t *out, size_t out_chars)
{
    wchar_t top[MAX_PATH];
    const wchar_t *rest = wcschr(name, L'\\');
    size_t top_len = rest ? (size_t)(rest - name) : wcslen(name);
    int i;

    if (top_len >= MAX_PATH)
        return FALSE;
    wcsncpy_s(top, MAX_PATH, name, top_len);
    for (i = 0; i < *ntops; i++)
        if (_wcsicmp(tops[i].from, top) == 0)
            break;
    if (i == *ntops) {
        if (*ntops >= FILEDROP_MAX_TOPS)
            return FALSE;
        wcscpy_s(tops[i].from, MAX_PATH, top);
        /* A nested entry means the top is a folder. */
        if (!unique_top(desktop, top, is_dir || rest != NULL, tops[i].to, MAX_PATH))
            return FALSE;
        (*ntops)++;
    }
    return swprintf_s(out, out_chars, L"%s\\%s%s", desktop, tops[i].to, rest ? rest : L"") > 0;
}

/* Create every missing directory along path (which names a directory). */
static BOOL make_dirs(wchar_t *path)
{
    wchar_t *p;
    for (p = wcschr(path + 3, L'\\'); p; p = wcschr(p + 1, L'\\')) {
        *p = L'\0';
        CreateDirectoryW(path, NULL);
        *p = L'\\';
    }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Desktop of the console user, or the Public Desktop if nobody is logged on.
   *token receives the user's token for impersonation (NULL if none). */
static BOOL get_drop_folder(wchar_t *out, size_t out_chars, HANDLE *token)
{
    PWSTR path = NULL;
    *token = NULL;
    if (WTSQueryUserToken(WTSGetActiveConsoleSessionId(), token) &&
        SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, *token, &path))) {
        wcscpy_s(out, out_chars, path);
        CoTaskMemFree(path);
        return TRUE;
    }
    if (*token) {
        CloseHandle(*token);
        *token = NULL;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_PublicDesktop, 0, NULL, &path))) {
        wcscpy_s(out, out_chars, path);
        CoTaskMemFree(path);
        return TRUE;
    }
    return FALSE;
}

static void handle_drop_conn(AsbConn *c)
{
    wchar_t desktop[MAX_PATH], *name = NULL, *full = NULL;
    TopMap *tops = NULL;
    BYTE *buf = NULL;
    HANDLE token = NULL;
    int ntops = 0;
    UINT32 files_ok = 0, errors = 0;
    UINT64 bytes = 0;
    BOOL impersonating = FALSE;

    name = (wchar_t *)malloc(FILEDROP_MAX_NAME + sizeof(wchar_t));
    full = (wchar_t *)malloc(sizeof(wchar_t) * (MAX_PATH * 2 + 1024));
    tops = (TopMap *)calloc(FILEDROP_MAX_TOPS, sizeof(TopMap));
    buf  = (BYTE *)malloc(FILEDROP_CHUNK);
    if (!name || !full || !tops || !buf)
        goto done;

    if (!get_drop_folder(desktop, MAX_PATH, &token)) {
        input_log("File drop: no Desktop folder to write to.");
        goto done;
    }
    if (token && ImpersonateLoggedOnUser(token))
        impersonating = TRUE;

    for (;;) {
        FileDropHeader h;
        BOOL ok;

        if (!recv_all(c, &h, sizeof(h)) || h.magic != FILEDROP_MAGIC)
            break;
        if (h.type == FILEDROP_END) {
            FileDropResult r;
            r.magic = FILEDROP_MAGIC;
            r.files_ok = files_ok;
            r.errors = errors;
            asb_send(c, &r, sizeof(r));
            input_log("File drop: %u file(s), %llu bytes to '%ls' (%u error(s)).",
                      files_ok, bytes, desktop, errors);
            files_ok = errors = 0;
            bytes = 0;
            ntops = 0;
            continue;
        }
        if (h.name_bytes == 0 || h.name_bytes > FILEDROP_MAX_NAME || (h.name_bytes & 1) ||
            (h.type != FILEDROP_FILE && h.type != FILEDROP_DIR))
            break;   /* malformed: drop the connection */
        if (!recv_all(c, name, (int)h.name_bytes))
            break;
        name[h.name_bytes / sizeof(wchar_t)] = L'\0';

        ok = name_is_safe(name) &&
             map_name(desktop, name, h.type == FILEDROP_DIR, tops, &ntops,
                      full, MAX_PATH * 2 + 1024);
        if (!ok)
            input_log("File drop: rejected name '%ls'.", name);

        if (h.type == FILEDROP_DIR) {
            if (ok && !make_dirs(full)) {
                input_log("File drop: cannot create folder '%ls' (%lu).", full, GetLastError());
                ok = FALSE;
            }
            if (!ok) errors++;
            continue;
        }

        /* File: write it, or drain its data if it cannot be written. */
        {
            HANDLE f = INVALID_HANDLE_VALUE;
            UINT64 left = h.size;
            if (ok) {
                wchar_t *slash = wcsrchr(full, L'\\');
                if (slash) { *slash = L'\0'; make_dirs(full); *slash = L'\\'; }
                f = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
                if (f == INVALID_HANDLE_VALUE)
                    input_log("File drop: cannot create '%ls' (%lu).", full, GetLastError());
            }
            while (left > 0) {
                int chunk = left > FILEDROP_CHUNK ? FILEDROP_CHUNK : (int)left;
                DWORD wrote;
                if (!recv_all(c, buf, chunk))
                    goto conn_lost;
                if (f != INVALID_HANDLE_VALUE &&
                    (!WriteFile(f, buf, (DWORD)chunk, &wrote, NULL) || wrote != (DWORD)chunk)) {
                    input_log("File drop: write to '%ls' failed (%lu).", full, GetLastError());
                    CloseHandle(f);
                    DeleteFileW(full);
                    f = INVALID_HANDLE_VALUE;
                    ok = FALSE;
                }
                left -= (UINT64)chunk;
            }
            if (f != INVALID_HANDLE_VALUE) {
                CloseHandle(f);
                files_ok++;
                bytes += h.size;
            } else {
                errors++;
            }
            continue;
conn_lost:
            if (f != INVALID_HANDLE_VALUE) {
                CloseHandle(f);
                DeleteFileW(full);   /* never leave a truncated file behind */
            }
            input_log("File drop: connection lost mid-file.");
            break;
        }
    }

done:
    if (impersonating)
        RevertToSelf();
    if (token)
        CloseHandle(token);
    free(name);
    free(full);
    free(tops);
    free(buf);
}

static DWORD WINAPI drop_conn_thread(LPVOID param)
{
    AsbConn *c = (AsbConn *)param;
    handle_drop_conn(c);
    asb_close(c);
    return 0;
}

static DWORD WINAPI filedrop_listen_thread(LPVOID param)
{
    AsbListener *l;
    (void)param;

    for (;;) {
        l = asb_listen(ASB_CH_FILEDROP);
        if (l)
            break;
        Sleep(5000);   /* ivshmem hosts have no file-drop region: stay idle */
    }
    input_log("File drop: listening.");
    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        HANDLE t;
        if (!c) {
            Sleep(100);
            continue;
        }
        t = CreateThread(NULL, 0, drop_conn_thread, c, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            asb_close(c);
    }
}

void filedrop_start(void)
{
    HANDLE t = CreateThread(NULL, 0, filedrop_listen_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
}
