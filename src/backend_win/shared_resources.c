#define _CRT_SECURE_NO_WARNINGS
#include "shared_resources.h"
#include "asb_core.h"
#include "ui.h"
#include <io.h>
#include <stdio.h>
#include <wctype.h>
#include <wchar.h>

/* Resources are appliance metadata. A legacy Path is retained only for the
   migration notice; AppSandbox never opens, moves, shares, or deletes it. */
static AsbSharedResourceInfo g_resources[ASB_MAX_SHARED_RESOURCES];
static int g_resource_count;
static CRITICAL_SECTION g_resource_cs;
static BOOL g_resource_cs_ready;

static void config_path(wchar_t *out, size_t chars)
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(out, chars, L"%s\\AppSandbox\\shared-resources.cfg", base);
}

void shared_resources_share_name(const wchar_t *id, wchar_t *out, size_t chars)
{
    size_t si = 4, gi = 0;
    if (!out || chars < 8) return;
    wcscpy_s(out, chars, L"asb_");
    while (id && id[gi] && si + 1 < chars && si < 24) {
        if (id[gi] != L'-') out[si++] = towlower(id[gi]);
        gi++;
    }
    if (si + 1 < chars) out[si++] = L'$';
    out[si] = L'\0';
}

static HRESULT save_locked(void)
{
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    FILE *f = NULL;
    int i;
    config_path(path, _countof(path));
    swprintf_s(temp, _countof(temp), L"%s.tmp", path);
    if (_wfopen_s(&f, temp, L"w,ccs=UTF-8") != 0 || !f)
        return HRESULT_FROM_WIN32(GetLastError());
    fwprintf(f, L"Version=2\nStorageKind=appliance\n");
    for (i = 0; i < g_resource_count; i++) {
        AsbSharedResourceInfo *r = &g_resources[i];
        fwprintf(f,
            L"[Resource]\nId=%s\nName=%s\nLegacyHostPath=%s\n"
            L"DriveLetter=%c\nHostDriveLetter=%c\nEnabled=%d\nReadOnly=%d\n"
            L"StorageKind=appliance\nRetainedData=%d\n",
            r->id, r->name, r->legacy_host_path,
            r->drive_letter, r->host_drive_letter ? r->host_drive_letter : L'-',
            r->enabled ? 1 : 0, r->read_only ? 1 : 0,
            r->retained_data ? 1 : 0);
    }
    fflush(f);
    FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));
    fclose(f);
    if (!ReplaceFileW(path, temp, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND && MoveFileExW(temp, path,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return S_OK;
        DeleteFileW(temp);
        return HRESULT_FROM_WIN32(err);
    }
    return S_OK;
}

void shared_resources_init(void)
{
    wchar_t path[MAX_PATH], line[1024];
    FILE *f = NULL;
    AsbSharedResourceInfo *cur = NULL;
    BOOL migrated = FALSE;
    if (!g_resource_cs_ready) {
        InitializeCriticalSection(&g_resource_cs);
        g_resource_cs_ready = TRUE;
    }
    EnterCriticalSection(&g_resource_cs);
    g_resource_count = 0;
    config_path(path, _countof(path));
    if (_wfopen_s(&f, path, L"r,ccs=UTF-8") == 0 && f) {
        while (fgetws(line, _countof(line), f)) {
            wchar_t *nl = wcspbrk(line, L"\r\n");
            if (nl) *nl = L'\0';
            if (wcscmp(line, L"[Resource]") == 0) {
                if (g_resource_count >= ASB_MAX_SHARED_RESOURCES) { cur = NULL; continue; }
                cur = &g_resources[g_resource_count++];
                ZeroMemory(cur, sizeof(*cur));
                wcscpy_s(cur->storage_kind, _countof(cur->storage_kind), L"appliance");
            } else if (cur && wcsncmp(line, L"Id=", 3) == 0)
                wcscpy_s(cur->id, _countof(cur->id), line + 3);
            else if (cur && wcsncmp(line, L"Name=", 5) == 0)
                wcscpy_s(cur->name, _countof(cur->name), line + 5);
            else if (cur && wcsncmp(line, L"Path=", 5) == 0) {
                wcscpy_s(cur->legacy_host_path, _countof(cur->legacy_host_path), line + 5);
                wcscpy_s(cur->host_path, _countof(cur->host_path), line + 5);
                migrated = TRUE;
            } else if (cur && wcsncmp(line, L"LegacyHostPath=", 15) == 0) {
                wcscpy_s(cur->legacy_host_path, _countof(cur->legacy_host_path), line + 15);
                wcscpy_s(cur->host_path, _countof(cur->host_path), line + 15);
            } else if (cur && wcsncmp(line, L"DriveLetter=", 12) == 0)
                cur->drive_letter = towupper(line[12]);
            else if (cur && wcsncmp(line, L"HostDriveLetter=", 16) == 0)
                cur->host_drive_letter = line[16] == L'-' ? 0 : towupper(line[16]);
            else if (cur && wcsncmp(line, L"Enabled=", 8) == 0)
                cur->enabled = _wtoi(line + 8) != 0;
            else if (cur && wcsncmp(line, L"ReadOnly=", 9) == 0)
                cur->read_only = _wtoi(line + 9) != 0;
            else if (cur && wcsncmp(line, L"RetainedData=", 13) == 0)
                cur->retained_data = _wtoi(line + 13) != 0;
        }
        fclose(f);
    }
    if (migrated) {
        int i;
        save_locked();
        ui_log(L"Shared resources now use the storage appliance; legacy host folders were disconnected and left untouched:");
        for (i = 0; i < g_resource_count; i++)
            if (g_resources[i].legacy_host_path[0])
                ui_log(L"  %s -> %s", g_resources[i].name, g_resources[i].legacy_host_path);
    }
    LeaveCriticalSection(&g_resource_cs);
}

int shared_resources_count(void) { return g_resource_count; }
const AsbSharedResourceInfo *shared_resources_get(int index)
{ return index >= 0 && index < g_resource_count ? &g_resources[index] : NULL; }

static int find_id(const wchar_t *id)
{
    int i;
    for (i = 0; i < g_resource_count; i++)
        if (_wcsicmp(g_resources[i].id, id) == 0) return i;
    return -1;
}

const AsbSharedResourceInfo *shared_resources_find(const wchar_t *id)
{
    int index = id ? find_id(id) : -1;
    return index >= 0 ? &g_resources[index] : NULL;
}

static HRESULT validate_info(const AsbSharedResourceInfo *in, int ignore,
                             AsbSharedResourceInfo *normalized)
{
    int i;
    if (!in || !in->name[0] || in->drive_letter < L'D' || in->drive_letter > L'Z')
        return E_INVALIDARG;
    *normalized = *in;
    normalized->drive_letter = towupper(normalized->drive_letter);
    if (normalized->host_drive_letter) {
        normalized->host_drive_letter = towupper(normalized->host_drive_letter);
        if (normalized->host_drive_letter < L'D' || normalized->host_drive_letter > L'Z')
            return E_INVALIDARG;
    }
    wcscpy_s(normalized->storage_kind, _countof(normalized->storage_kind), L"appliance");
    normalized->host_path[0] = L'\0';
    normalized->legacy_host_path[0] = L'\0';
    for (i = 0; i < g_resource_count; i++) if (i != ignore) {
        AsbSharedResourceInfo *r = &g_resources[i];
        if (_wcsicmp(r->name, normalized->name) == 0 ||
            r->drive_letter == normalized->drive_letter ||
            (normalized->host_drive_letter && r->host_drive_letter == normalized->host_drive_letter))
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }
    return S_OK;
}

HRESULT shared_resources_create(const AsbSharedResourceInfo *info, BOOL confirm_permissions,
                                wchar_t *created_id, size_t created_id_chars)
{
    AsbSharedResourceInfo n;
    GUID guid;
    HRESULT hr;
    (void)confirm_permissions;
    EnterCriticalSection(&g_resource_cs);
    if (g_resource_count >= ASB_MAX_SHARED_RESOURCES) {
        LeaveCriticalSection(&g_resource_cs); return E_OUTOFMEMORY;
    }
    hr = validate_info(info, -1, &n);
    if (FAILED(hr)) { LeaveCriticalSection(&g_resource_cs); return hr; }
    {
        wchar_t guid_buf[ASB_SHARED_ID_CHARS];
        CoCreateGuid(&guid);
        StringFromGUID2(&guid, guid_buf, _countof(guid_buf));
        wcsncpy_s(n.id, _countof(n.id), guid_buf + 1, 36);
    }
    g_resources[g_resource_count++] = n;
    hr = save_locked();
    if (FAILED(hr)) g_resource_count--;
    else if (created_id) wcscpy_s(created_id, created_id_chars, n.id);
    LeaveCriticalSection(&g_resource_cs);
    return hr;
}

HRESULT shared_resources_update(const wchar_t *id, const AsbSharedResourceInfo *info,
                                BOOL confirm_permissions)
{
    AsbSharedResourceInfo n, old;
    HRESULT hr;
    int idx;
    (void)confirm_permissions;
    EnterCriticalSection(&g_resource_cs);
    idx = find_id(id);
    if (idx < 0) { LeaveCriticalSection(&g_resource_cs); return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    old = g_resources[idx];
    hr = validate_info(info, idx, &n);
    if (FAILED(hr)) { LeaveCriticalSection(&g_resource_cs); return hr; }
    wcscpy_s(n.id, _countof(n.id), old.id);
    wcscpy_s(n.legacy_host_path, _countof(n.legacy_host_path), old.legacy_host_path);
    wcscpy_s(n.host_path, _countof(n.host_path), old.host_path);
    n.retained_data = old.retained_data;
    g_resources[idx] = n;
    hr = save_locked();
    if (FAILED(hr)) g_resources[idx] = old;
    LeaveCriticalSection(&g_resource_cs);
    return hr;
}

HRESULT shared_resources_remove(const wchar_t *id)
{
    int idx, i;
    HRESULT hr;
    AsbSharedResourceInfo old;
    EnterCriticalSection(&g_resource_cs);
    idx = find_id(id);
    if (idx < 0) { LeaveCriticalSection(&g_resource_cs); return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    old = g_resources[idx];
    for (i = idx; i < g_resource_count - 1; i++) g_resources[i] = g_resources[i + 1];
    g_resource_count--;
    hr = save_locked();
    if (FAILED(hr)) {
        for (i = g_resource_count; i > idx; i--) g_resources[i] = g_resources[i - 1];
        g_resources[idx] = old;
        g_resource_count++;
    }
    LeaveCriticalSection(&g_resource_cs);
    return hr;
}

BOOL shared_resources_is_excluded(const wchar_t *list, const wchar_t *id)
{
    const wchar_t *p = list;
    size_t n = wcslen(id);
    if (!p) return FALSE;
    while (*p) {
        while (*p == L',') p++;
        if (_wcsnicmp(p, id, n) == 0 && (p[n] == L',' || p[n] == L'\0')) return TRUE;
        p = wcschr(p, L','); if (!p) break; p++;
    }
    return FALSE;
}

HRESULT shared_resources_set_excluded(wchar_t *list, size_t chars,
                                      const wchar_t *id, BOOL excluded)
{
    wchar_t out[1024] = L"", copy[1024], *ctx = NULL, *tok;
    BOOL found = FALSE;
    if (!list || !id || !id[0]) return E_INVALIDARG;
    wcscpy_s(copy, _countof(copy), list);
    tok = wcstok_s(copy, L",", &ctx);
    while (tok) {
        if (_wcsicmp(tok, id) == 0) found = TRUE;
        else { if (out[0]) wcscat_s(out, _countof(out), L","); wcscat_s(out, _countof(out), tok); }
        tok = wcstok_s(NULL, L",", &ctx);
    }
    if (excluded && !found) {
        if (out[0]) wcscat_s(out, _countof(out), L",");
        if (wcslen(out) + wcslen(id) + 1 >= chars) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        wcscat_s(out, _countof(out), id);
    }
    wcscpy_s(list, chars, out);
    return S_OK;
}

HRESULT shared_resources_set_smb_acl_created(const wchar_t *id, BOOL created)
{
    (void)id; (void)created;
    return S_OK;
}

int shared_resources_build_attachments(const wchar_t *os_type, const wchar_t *exclusions,
                                       HcsSharedResource *out, int capacity)
{
    int i, n = 0;
    if (!os_type || _wcsicmp(os_type, L"Windows") != 0) return 0;
    EnterCriticalSection(&g_resource_cs);
    for (i = 0; i < g_resource_count && n < capacity; i++) {
        AsbSharedResourceInfo *r = &g_resources[i];
        if (!r->enabled || shared_resources_is_excluded(exclusions, r->id)) continue;
        ZeroMemory(&out[n], sizeof(out[n]));
        wcscpy_s(out[n].id, _countof(out[n].id), r->id);
        shared_resources_share_name(r->id, out[n].share_name, _countof(out[n].share_name));
        out[n].drive_letter = r->drive_letter;
        out[n].host_drive_letter = r->host_drive_letter;
        out[n].read_only = r->read_only;
        wcscpy_s(out[n].mapping_result, _countof(out[n].mapping_result), L"pending");
        n++;
    }
    LeaveCriticalSection(&g_resource_cs);
    return n;
}
