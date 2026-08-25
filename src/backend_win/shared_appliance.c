#define _CRT_SECURE_NO_WARNINGS
#include "shared_appliance.h"
#include "asb_core.h"
#include "shared_resources.h"
#include "hcn_network.h"
#include "disk_util.h"
#include "vm_agent.h"
#include "ui.h"
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <urlmon.h>
#include <sddl.h>
#include <aclapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <io.h>
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shlwapi.lib")

#define APPLIANCE_VM_NAME L"AppSandbox.SharedAppliance"
#define APPLIANCE_UNIQUE_ID ((UINT64)0xFFFFFFFFFFFFFF00ull)
#define APPLIANCE_SERVICE_USER L"AppSandboxShare"
#define APPLIANCE_IDLE_MS (5ull * 60ull * 1000ull)
#define APPLIANCE_SETUP_TIMEOUT_MS (30ul * 60ul * 1000ul)

#if defined(_M_ARM64)
#define UBUNTU_IMAGE_NAME L"noble-server-cloudimg-arm64.img"
#else
#define UBUNTU_IMAGE_NAME L"noble-server-cloudimg-amd64.img"
#endif
#define UBUNTU_IMAGE_BUILD L"20260814"
#define UBUNTU_IMAGE_BASE L"https://cloud-images.ubuntu.com/noble/" UBUNTU_IMAGE_BUILD L"/"
/* iso_create_appliance_cloud_init emits a NoCloud seed with label CIDATA. */

typedef struct {
    DWORD magic;
    DWORD version;
    DWORD admin_blob_bytes;
    DWORD smb_blob_bytes;
} CredentialHeader;

typedef struct {
    CRITICAL_SECTION cs;
    CRITICAL_SECTION command_cs;
    BOOL initialized;
    SharedApplianceStatus status;
    wchar_t windows_iso_path[MAX_PATH];
    wchar_t seed_iso_path[MAX_PATH];
    wchar_t legacy_paths[2048];
    VmInstance runtime;
    GUID share_network_id;
    GUID share_endpoint_id;
    GUID maintenance_network_id;
    GUID maintenance_endpoint_id;
    BOOL share_endpoint_created;
    BOOL maintenance_endpoint_created;
    BOOL stopping;
    BOOL provisioning_boot;
    volatile LONG ready_worker_active;
    LONG idle_generation;
    LONG restart_attempts;
} SharedApplianceGlobal;

typedef struct {
    SharedApplianceConfig config;
    BOOL rebuild;
    BOOL switch_backend;
    BOOL cleanup_on_failure;
} SetupWorkerArgs;

static SharedApplianceGlobal g_appliance;
static HRESULT host_mapping_command(const AsbSharedResourceInfo *resource, BOOL mount);
static HRESULT reconcile_internal(void);
static volatile LONG g_setup_invalid_parameter;
static wchar_t g_setup_invalid_function[128];
static wchar_t g_setup_invalid_expression[256];
static wchar_t g_setup_invalid_file[MAX_PATH];
static unsigned int g_setup_invalid_line;

static void __cdecl setup_invalid_parameter_handler(const wchar_t *expression,
                                                     const wchar_t *function,
                                                     const wchar_t *file,
                                                     unsigned int line,
                                                     uintptr_t reserved)
{
    (void)reserved;
    lstrcpynW(g_setup_invalid_function, function ? function : L"?",
              _countof(g_setup_invalid_function));
    lstrcpynW(g_setup_invalid_expression, expression ? expression : L"?",
              _countof(g_setup_invalid_expression));
    lstrcpynW(g_setup_invalid_file, file ? file : L"?",
              _countof(g_setup_invalid_file));
    g_setup_invalid_line = line;
    InterlockedExchange(&g_setup_invalid_parameter, 1);
}

static void begin_invalid_parameter_capture(_invalid_parameter_handler *previous)
{
    g_setup_invalid_function[0] = L'\0';
    g_setup_invalid_expression[0] = L'\0';
    g_setup_invalid_file[0] = L'\0';
    g_setup_invalid_line = 0;
    InterlockedExchange(&g_setup_invalid_parameter, 0);
    *previous = _set_invalid_parameter_handler(setup_invalid_parameter_handler);
}

static HRESULT end_invalid_parameter_capture(_invalid_parameter_handler previous,
                                             HRESULT hr)
{
    _set_invalid_parameter_handler(previous);
    if (InterlockedCompareExchange(&g_setup_invalid_parameter, 0, 0) == 0)
        return hr;
    ui_log(L"[DEBUG-crt91] invalid CRT parameter: function=%s expression=%s file=%s line=%u",
           g_setup_invalid_function, g_setup_invalid_expression,
           g_setup_invalid_file, g_setup_invalid_line);
    return E_INVALIDARG;
}

static void program_data_root(wchar_t *out, size_t chars)
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(out, chars, L"%s\\AppSandbox", base);
}

static void appliance_config_path(wchar_t *out, size_t chars)
{
    wchar_t root[MAX_PATH];
    program_data_root(root, _countof(root));
    swprintf_s(out, chars, L"%s\\shared-appliance.cfg", root);
}

static void appliance_credential_path(wchar_t *out, size_t chars)
{
    wchar_t root[MAX_PATH];
    program_data_root(root, _countof(root));
    swprintf_s(out, chars, L"%s\\shared-appliance.cred", root);
}

static void set_error_locked(HRESULT hr, const wchar_t *context)
{
    g_appliance.status.state = ASB_APPLIANCE_STATE_FAILED;
    g_appliance.status.busy = FALSE;
    g_appliance.status.ready = FALSE;
    swprintf_s(g_appliance.status.last_error,
               _countof(g_appliance.status.last_error),
               L"%s (0x%08X)", context ? context : L"Shared appliance failed", hr);
    ui_log(L"Shared appliance: %s", g_appliance.status.last_error);
}

static void set_progress(int pct, const wchar_t *text)
{
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.status.progress = pct;
    if (text) wcscpy_s(g_appliance.status.progress_text,
                       _countof(g_appliance.status.progress_text), text);
    LeaveCriticalSection(&g_appliance.cs);
    if (text) ui_log(L"Shared appliance: %s", text);
}

/* Every host->appliance command funnels through here. vm_agent_send keeps a
   single command slot per VM, so two callers -- the start/readiness sequence
   and a resource sync, say -- would overwrite each other's command and desync
   the agent protocol, which shows up as an empty reply and a dropped agent
   connection. */
static BOOL appliance_send(const char *command, char *response,
                           int response_max, DWORD timeout_ms)
{
    BOOL ok;
    EnterCriticalSection(&g_appliance.command_cs);
    ok = vm_agent_send(&g_appliance.runtime, command, response, response_max, timeout_ms);
    LeaveCriticalSection(&g_appliance.command_cs);
    return ok;
}

static HRESULT ensure_directory(const wchar_t *path)
{
    wchar_t copy[MAX_PATH];
    wchar_t *p;
    DWORD attrs;
    if (!path || !path[0]) return E_INVALIDARG;
    wcscpy_s(copy, _countof(copy), path);
    for (p = copy + 3; *p; ++p) {
        if (*p == L'\\') {
            *p = L'\0';
            CreateDirectoryW(copy, NULL);
            *p = L'\\';
        }
    }
    if (!CreateDirectoryW(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());
    attrs = GetFileAttributesW(copy);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    return S_OK;
}

static void remove_directory_tree(const wchar_t *directory)
{
    WIN32_FIND_DATAW data;
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    HANDLE find;
    swprintf_s(pattern, _countof(pattern), L"%s\\*", directory);
    find = FindFirstFileW(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                continue;
            swprintf_s(path, _countof(path), L"%s\\%s", directory, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_directory_tree(path);
            else { SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL); DeleteFileW(path); }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(directory);
}

static HRESULT validate_storage_parent(const wchar_t *path)
{
    wchar_t full[MAX_PATH], volume[MAX_PATH], fs[32], probe[MAX_PATH];
    DWORD attrs, serial, max_comp, flags;
    ULARGE_INTEGER free_bytes;
    HANDLE file;
    if (!path || !path[0] || PathIsUNCW(path))
        return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    if (!GetFullPathNameW(path, _countof(full), full, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    if (wcslen(full) + wcslen(L"\\AppSandboxSharedAppliance\\provision.iso") >= 240)
        return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    attrs = GetFileAttributesW(full);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    if (!GetVolumePathNameW(full, volume, _countof(volume)))
        return HRESULT_FROM_WIN32(GetLastError());
    if (GetDriveTypeW(volume) != DRIVE_FIXED)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (!GetVolumeInformationW(volume, NULL, 0, &serial, &max_comp, &flags,
                               fs, _countof(fs)))
        return HRESULT_FROM_WIN32(GetLastError());
    if (_wcsicmp(fs, L"NTFS") != 0 && _wcsicmp(fs, L"ReFS") != 0)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (!GetDiskFreeSpaceExW(full, &free_bytes, NULL, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    if (free_bytes.QuadPart < 8ull * 1024ull * 1024ull * 1024ull)
        return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    swprintf_s(probe, _countof(probe), L"%s\\.asb-appliance-write-%lu.tmp",
               full, GetCurrentProcessId());
    file = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(file);
    return S_OK;
}

static BOOL valid_account_name(const wchar_t *name, BOOL linux_name)
{
    const wchar_t *p;
    size_t length;
    if (!name || !(length = wcslen(name)) || length > 63) return FALSE;
    if (linux_name && !(name[0] == L'_' || (name[0] >= L'a' && name[0] <= L'z')))
        return FALSE;
    for (p = name; *p; ++p) {
        if (!iswalnum(*p) && *p != L'_' && *p != L'-') return FALSE;
        if (linux_name && iswupper(*p)) return FALSE;
    }
    return TRUE;
}

static BOOL valid_xml_text(const wchar_t *value)
{
    return value && !wcspbrk(value, L"<>&\"\r\n");
}

static HRESULT save_config_locked(void)
{
    wchar_t path[MAX_PATH], temp[MAX_PATH], root[MAX_PATH];
    FILE *file = NULL;
    DWORD error;
    program_data_root(root, _countof(root));
    ensure_directory(root);
    appliance_config_path(path, _countof(path));
    swprintf_s(temp, _countof(temp), L"%s.tmp", path);
    if (_wfopen_s(&file, temp, L"w,ccs=UTF-8") != 0 || !file)
        return HRESULT_FROM_WIN32(GetLastError());
    fwprintf(file, L"Version=1\n");
    fwprintf(file, L"Backend=%d\n", g_appliance.status.backend);
    fwprintf(file, L"Configured=%d\n", g_appliance.status.configured ? 1 : 0);
    fwprintf(file, L"StorageRoot=%s\n", g_appliance.status.storage_root);
    fwprintf(file, L"OsVhdx=%s\n", g_appliance.status.os_vhdx_path);
    fwprintf(file, L"DataVhdx=%s\n", g_appliance.status.data_vhdx_path);
    fwprintf(file, L"SeedIso=%s\n", g_appliance.seed_iso_path);
    fwprintf(file, L"WindowsIso=%s\n", g_appliance.windows_iso_path);
    fwprintf(file, L"WindowsImage=%s\n", g_appliance.status.windows_image_name);
    fwprintf(file, L"ManagementCert=%s\n", g_appliance.status.management_cert_thumbprint);
    fwprintf(file, L"AdminUser=%s\n", g_appliance.status.admin_user);
    fwprintf(file, L"DataSizeGB=%lu\n", g_appliance.status.data_size_gb);
    fwprintf(file, L"RamMB=%lu\n", g_appliance.status.ram_mb);
    fwprintf(file, L"CpuCores=%lu\n", g_appliance.status.cpu_cores);
    fflush(file);
    FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(file)));
    fclose(file);
    if (ReplaceFileW(path, temp, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL))
        return S_OK;
    error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND && MoveFileExW(temp, path,
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return S_OK;
    DeleteFileW(temp);
    return HRESULT_FROM_WIN32(error);
}

static HRESULT protect_text(const wchar_t *text, DATA_BLOB *protected_blob)
{
    static const BYTE entropy_bytes[] = "AppSandbox.SharedAppliance.v1";
    DATA_BLOB input, entropy;
    input.pbData = (BYTE *)text;
    input.cbData = (DWORD)((wcslen(text) + 1) * sizeof(wchar_t));
    entropy.pbData = (BYTE *)entropy_bytes;
    entropy.cbData = (DWORD)sizeof(entropy_bytes);
    ZeroMemory(protected_blob, sizeof(*protected_blob));
    if (!CryptProtectData(&input, L"AppSandbox shared appliance credential",
                          &entropy, NULL, NULL,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
                          protected_blob))
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

static HRESULT unprotect_text(const BYTE *bytes, DWORD byte_count,
                              wchar_t *out, size_t out_chars)
{
    static const BYTE entropy_bytes[] = "AppSandbox.SharedAppliance.v1";
    DATA_BLOB input, entropy, plain;
    HRESULT hr = S_OK;
    input.pbData = (BYTE *)bytes;
    input.cbData = byte_count;
    entropy.pbData = (BYTE *)entropy_bytes;
    entropy.cbData = (DWORD)sizeof(entropy_bytes);
    ZeroMemory(&plain, sizeof(plain));
    if (!CryptUnprotectData(&input, NULL, &entropy, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &plain))
        return HRESULT_FROM_WIN32(GetLastError());
    if (plain.cbData < sizeof(wchar_t) ||
        plain.cbData / sizeof(wchar_t) > out_chars) hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    else wcscpy_s(out, out_chars, (const wchar_t *)plain.pbData);
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return hr;
}

static HRESULT credential_security_attributes(SECURITY_ATTRIBUTES *sa,
                                              PSECURITY_DESCRIPTOR *descriptor)
{
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1,
            descriptor, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    sa->nLength = sizeof(*sa);
    sa->lpSecurityDescriptor = *descriptor;
    sa->bInheritHandle = FALSE;
    return S_OK;
}

static HRESULT save_credentials(const wchar_t *admin_password,
                                const wchar_t *smb_password)
{
    wchar_t path[MAX_PATH], temp[MAX_PATH], root[MAX_PATH];
    DATA_BLOB admin_blob, smb_blob;
    CredentialHeader header;
    SECURITY_ATTRIBUTES sa;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written;
    HRESULT hr;
    ZeroMemory(&admin_blob, sizeof(admin_blob));
    ZeroMemory(&smb_blob, sizeof(smb_blob));
    hr = protect_text(admin_password, &admin_blob);
    if (FAILED(hr)) return hr;
    hr = protect_text(smb_password, &smb_blob);
    if (FAILED(hr)) goto cleanup;
    hr = credential_security_attributes(&sa, &descriptor);
    if (FAILED(hr)) goto cleanup;
    program_data_root(root, _countof(root));
    ensure_directory(root);
    appliance_credential_path(path, _countof(path));
    swprintf_s(temp, _countof(temp), L"%s.tmp", path);
    file = CreateFileW(temp, GENERIC_WRITE, 0, &sa, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, NULL);
    if (file == INVALID_HANDLE_VALUE) { hr = HRESULT_FROM_WIN32(GetLastError()); goto cleanup; }
    header.magic = 0x43534141; /* AASC */
    header.version = 1;
    header.admin_blob_bytes = admin_blob.cbData;
    header.smb_blob_bytes = smb_blob.cbData;
    if (!WriteFile(file, &header, sizeof(header), &written, NULL) || written != sizeof(header) ||
        !WriteFile(file, admin_blob.pbData, admin_blob.cbData, &written, NULL) || written != admin_blob.cbData ||
        !WriteFile(file, smb_blob.pbData, smb_blob.cbData, &written, NULL) || written != smb_blob.cbData ||
        !FlushFileBuffers(file)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        CloseHandle(file); file = INVALID_HANDLE_VALUE; DeleteFileW(temp); goto cleanup;
    }
    CloseHandle(file); file = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        hr = HRESULT_FROM_WIN32(GetLastError()); DeleteFileW(temp); goto cleanup;
    }
    hr = S_OK;
cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (descriptor) LocalFree(descriptor);
    if (admin_blob.pbData) { SecureZeroMemory(admin_blob.pbData, admin_blob.cbData); LocalFree(admin_blob.pbData); }
    if (smb_blob.pbData) { SecureZeroMemory(smb_blob.pbData, smb_blob.cbData); LocalFree(smb_blob.pbData); }
    return hr;
}

static HRESULT load_credentials(wchar_t *admin_password, size_t admin_chars,
                                wchar_t *smb_password, size_t smb_chars)
{
    wchar_t path[MAX_PATH];
    CredentialHeader header;
    HANDLE file;
    BYTE *admin_blob = NULL, *smb_blob = NULL;
    DWORD read;
    HRESULT hr = E_FAIL;
    appliance_credential_path(path, _countof(path));
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    if (!ReadFile(file, &header, sizeof(header), &read, NULL) || read != sizeof(header) ||
        header.magic != 0x43534141 || header.version != 1 ||
        header.admin_blob_bytes > 65536 || header.smb_blob_bytes > 65536) {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); goto cleanup;
    }
    admin_blob = (BYTE *)HeapAlloc(GetProcessHeap(), 0, header.admin_blob_bytes);
    smb_blob = (BYTE *)HeapAlloc(GetProcessHeap(), 0, header.smb_blob_bytes);
    if (!admin_blob || !smb_blob) { hr = E_OUTOFMEMORY; goto cleanup; }
    if (!ReadFile(file, admin_blob, header.admin_blob_bytes, &read, NULL) || read != header.admin_blob_bytes ||
        !ReadFile(file, smb_blob, header.smb_blob_bytes, &read, NULL) || read != header.smb_blob_bytes) {
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA); goto cleanup;
    }
    hr = unprotect_text(admin_blob, header.admin_blob_bytes, admin_password, admin_chars);
    if (SUCCEEDED(hr)) hr = unprotect_text(smb_blob, header.smb_blob_bytes, smb_password, smb_chars);
cleanup:
    if (admin_blob) { SecureZeroMemory(admin_blob, header.admin_blob_bytes); HeapFree(GetProcessHeap(), 0, admin_blob); }
    if (smb_blob) { SecureZeroMemory(smb_blob, header.smb_blob_bytes); HeapFree(GetProcessHeap(), 0, smb_blob); }
    CloseHandle(file);
    return hr;
}

static HRESULT generate_password(wchar_t *out, size_t chars)
{
    static const wchar_t alphabet[] = L"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    BYTE random[40];
    size_t i, wanted = min(chars - 1, (size_t)32);
    if (chars < 17) return E_INVALIDARG;
    if (BCryptGenRandom(NULL, random, (ULONG)wanted,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return E_FAIL;
    for (i = 0; i < wanted; ++i) out[i] = alphabet[random[i] % (_countof(alphabet) - 1)];
    out[wanted] = L'\0';
    SecureZeroMemory(random, sizeof(random));
    return S_OK;
}

static void load_config(void)
{
    wchar_t path[MAX_PATH], line[1024];
    FILE *file = NULL;
    appliance_config_path(path, _countof(path));
    if (_wfopen_s(&file, path, L"r,ccs=UTF-8") != 0 || !file) return;
    while (fgetws(line, _countof(line), file)) {
        wchar_t *nl = wcspbrk(line, L"\r\n"); if (nl) *nl = L'\0';
        if (wcsncmp(line, L"Backend=", 8) == 0) g_appliance.status.backend = _wtoi(line + 8);
        else if (wcsncmp(line, L"Configured=", 11) == 0) g_appliance.status.configured = _wtoi(line + 11) != 0;
        else if (wcsncmp(line, L"StorageRoot=", 12) == 0) wcscpy_s(g_appliance.status.storage_root, MAX_PATH, line + 12);
        else if (wcsncmp(line, L"OsVhdx=", 7) == 0) wcscpy_s(g_appliance.status.os_vhdx_path, MAX_PATH, line + 7);
        else if (wcsncmp(line, L"DataVhdx=", 9) == 0) wcscpy_s(g_appliance.status.data_vhdx_path, MAX_PATH, line + 9);
        else if (wcsncmp(line, L"SeedIso=", 8) == 0) wcscpy_s(g_appliance.seed_iso_path, MAX_PATH, line + 8);
        else if (wcsncmp(line, L"WindowsIso=", 11) == 0) wcscpy_s(g_appliance.windows_iso_path, MAX_PATH, line + 11);
        else if (wcsncmp(line, L"WindowsImage=", 13) == 0) wcscpy_s(g_appliance.status.windows_image_name, 256, line + 13);
        else if (wcsncmp(line, L"ManagementCert=", 15) == 0) wcscpy_s(g_appliance.status.management_cert_thumbprint, 128, line + 15);
        else if (wcsncmp(line, L"AdminUser=", 10) == 0) wcscpy_s(g_appliance.status.admin_user, 128, line + 10);
        else if (wcsncmp(line, L"DataSizeGB=", 11) == 0) g_appliance.status.data_size_gb = _wtoi(line + 11);
        else if (wcsncmp(line, L"RamMB=", 6) == 0) g_appliance.status.ram_mb = _wtoi(line + 6);
        else if (wcsncmp(line, L"CpuCores=", 9) == 0) g_appliance.status.cpu_cores = _wtoi(line + 9);
    }
    fclose(file);
    if (g_appliance.status.configured) {
        g_appliance.status.state = ASB_APPLIANCE_STATE_STOPPED;
        wcscpy_s(g_appliance.status.progress_text, 256, L"Stopped");
    }
}

static HRESULT sha256_file_hex(const wchar_t *path, wchar_t hex[65])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE digest[32], buffer[65536];
    DWORD read, object_bytes = 0, cb = 0;
    BYTE *object = NULL;
    NTSTATUS status;
    int i;
    status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (status < 0) return E_FAIL;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (BYTE *)&object_bytes,
                               sizeof(object_bytes), &cb, 0);
    if (status < 0) goto fail;
    object = (BYTE *)HeapAlloc(GetProcessHeap(), 0, object_bytes);
    if (!object) goto fail;
    status = BCryptCreateHash(algorithm, &hash, object, object_bytes, NULL, 0, 0);
    if (status < 0) goto fail;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) goto fail;
    while (ReadFile(file, buffer, sizeof(buffer), &read, NULL) && read) {
        status = BCryptHashData(hash, buffer, read, 0);
        if (status < 0) goto fail;
    }
    status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (status < 0) goto fail;
    for (i = 0; i < 32; ++i) swprintf_s(hex + i * 2, 65 - i * 2, L"%02x", digest[i]);
    hex[64] = L'\0';
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    SecureZeroMemory(digest, sizeof(digest));
    HeapFree(GetProcessHeap(), 0, object);
    return S_OK;
fail:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (object) HeapFree(GetProcessHeap(), 0, object);
    return E_FAIL;
}

static HRESULT download_ubuntu_image(wchar_t *image_path, size_t image_chars)
{
    wchar_t root[MAX_PATH], cache[MAX_PATH], sums[MAX_PATH], image_url[512], sums_url[512];
    wchar_t expected[65] = L"", actual[65];
    char line[1024], wanted[256];
    FILE *file = NULL;
    HRESULT hr;
    program_data_root(root, _countof(root));
    swprintf_s(cache, _countof(cache), L"%s\\cache\\ubuntu-24.04", root);
    hr = ensure_directory(cache); if (FAILED(hr)) return hr;
    swprintf_s(image_path, image_chars, L"%s\\%s", cache, UBUNTU_IMAGE_NAME);
    swprintf_s(sums, _countof(sums), L"%s\\SHA256SUMS", cache);
    swprintf_s(image_url, _countof(image_url), L"%s%s", UBUNTU_IMAGE_BASE, UBUNTU_IMAGE_NAME);
    swprintf_s(sums_url, _countof(sums_url), L"%sSHA256SUMS", UBUNTU_IMAGE_BASE);
    if (URLDownloadToFileW(NULL, sums_url, sums, 0, NULL) != S_OK)
        return HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);
    WideCharToMultiByte(CP_UTF8, 0, UBUNTU_IMAGE_NAME, -1, wanted, sizeof(wanted), NULL, NULL);
    if (_wfopen_s(&file, sums, L"rb") != 0 || !file) return E_FAIL;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, wanted) && strlen(line) >= 64) {
            char hash[65]; memcpy(hash, line, 64); hash[64] = '\0';
            MultiByteToWideChar(CP_UTF8, 0, hash, -1, expected, _countof(expected)); break;
        }
    }
    fclose(file);
    if (!expected[0]) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if (GetFileAttributesW(image_path) == INVALID_FILE_ATTRIBUTES) {
        set_progress(8, L"Downloading pinned Ubuntu 24.04 LTS cloud image...");
        if (URLDownloadToFileW(NULL, image_url, image_path, 0, NULL) != S_OK)
            return HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE);
    }
    hr = sha256_file_hex(image_path, actual); if (FAILED(hr)) return hr;
    if (_wcsicmp(actual, expected) != 0) {
        DeleteFileW(image_path);
        return HRESULT_FROM_WIN32(ERROR_CRC);
    }
    return S_OK;
}

static HRESULT run_hidden_process(wchar_t *command, DWORD timeout_ms)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code = ERROR_GEN_FAILURE, wait;
    ZeroMemory(&startup, sizeof(startup)); startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process))
        return HRESULT_FROM_WIN32(GetLastError());
    wait = WaitForSingleObject(process.hProcess, timeout_ms);
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exit_code);
    else { TerminateProcess(process.hProcess, ERROR_TIMEOUT); exit_code = ERROR_TIMEOUT; }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return exit_code == 0 ? S_OK : HRESULT_FROM_WIN32(exit_code);
}

static HRESULT launch_process(wchar_t *command)
{
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof(startup)); startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0,
                        NULL, NULL, &startup, &process))
        return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return S_OK;
}

static HRESULT build_ubuntu_os(const SharedApplianceConfig *config)
{
    wchar_t image[MAX_PATH], exe[MAX_PATH], command[2048], resource_dir[MAX_PATH];
    wchar_t admin_password[256], smb_password[256], ssh_public_key[512];
    wchar_t *slash;
    HRESULT hr;
    hr = download_ubuntu_image(image, _countof(image)); if (FAILED(hr)) return hr;
    set_progress(20, L"Converting Ubuntu cloud image to VHDX...");
    GetModuleFileNameW(NULL, exe, _countof(exe)); slash = wcsrchr(exe, L'\\'); if (slash) *slash = L'\0';
    swprintf_s(command, _countof(command),
        L"\"%s\\iso-patch.exe\" --qcow2-to-vhdx \"%s\" --output \"%s\"",
        exe, image, g_appliance.status.os_vhdx_path);
    hr = run_hidden_process(command, 30 * 60 * 1000); if (FAILED(hr)) return hr;
    hr = load_credentials(admin_password, _countof(admin_password), smb_password, _countof(smb_password));
    if (FAILED(hr)) return hr;
    if (!asb_ensure_ssh_key(ssh_public_key, _countof(ssh_public_key))) {
        SecureZeroMemory(admin_password, sizeof(admin_password));
        SecureZeroMemory(smb_password, sizeof(smb_password));
        return E_FAIL;
    }
    swprintf_s(resource_dir, _countof(resource_dir), L"%s\\resources", exe);
    set_progress(55, L"Creating Ubuntu cloud-init seed...");
    hr = iso_create_appliance_cloud_init(g_appliance.seed_iso_path,
            config->admin_user, admin_password, APPLIANCE_SERVICE_USER,
            smb_password, ssh_public_key, resource_dir);
    SecureZeroMemory(admin_password, sizeof(admin_password));
    SecureZeroMemory(smb_password, sizeof(smb_password));
    return hr;
}

static HRESULT build_server_core_os(const SharedApplianceConfig *config)
{
    /* Offline-apply pipeline, same as client VMs: iso-patch.exe mounts the
       Server ISO, applies the image straight onto os.vhdx and stages the
       agent plus a VHDX-first unattend (specialize + oobeSystem). The old
       route -- empty VHDX + bootable autounattend DVD -- dead-ended at the
       Hyper-V UEFI "press any key to boot from CD/DVD" prompt, which no
       unattended flow can satisfy. Image index 1 on Server composite ISOs
       is the Standard *Core* edition, exactly what this appliance wants. */
    wchar_t exe[MAX_PATH], res_dir[MAX_PATH], staging[MAX_PATH], file_path[MAX_PATH];
    wchar_t manifest[MAX_PATH], cmdline[2048], line_w[512];
    wchar_t *slash;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES sa;
    HANDLE h_read = INVALID_HANDLE_VALUE, h_write = INVALID_HANDLE_VALUE;
    BYTE buffer[4096];
    char line_a[512];
    DWORD bytes_read, exit_code = ERROR_GEN_FAILURE;
    int pos = 0, start = 0, end = 0, i;
    BOOL have_exit = FALSE;
    HRESULT hr = E_FAIL;
    GpuDriverShareList no_gpu;

    set_progress(10, L"Applying the Windows Server Core image to the system disk...");
    GetModuleFileNameW(NULL, exe, _countof(exe));
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = L'\0';
    swprintf_s(res_dir, _countof(res_dir), L"%s\\resources", exe);
    if (GetFileAttributesW(res_dir) == INVALID_FILE_ATTRIBUTES)
        wcscpy_s(res_dir, _countof(res_dir), exe);

    swprintf_s(staging, _countof(staging), L"%s\\_appliance_stage",
               g_appliance.status.storage_root);
    remove_directory_tree(staging);
    if (!CreateDirectoryW(staging, NULL))
        return HRESULT_FROM_WIN32(GetLastError());

    swprintf_s(file_path, _countof(file_path), L"%s\\unattend.xml", staging);
    if (!generate_unattend_vhdx(file_path, APPLIANCE_VM_NAME, config->admin_user,
                                config->admin_password, FALSE, L"en-US"))
        return E_FAIL;
    swprintf_s(file_path, _countof(file_path), L"%s\\setup.cmd", staging);
    generate_vhdx_setup_cmd(file_path);
    swprintf_s(file_path, _countof(file_path), L"%s\\SetupComplete.cmd", staging);
    generate_vhdx_setupcomplete(file_path, TRUE);

    ZeroMemory(&no_gpu, sizeof(no_gpu));
    swprintf_s(manifest, _countof(manifest), L"%s\\manifest.txt", staging);
    if (generate_vhdx_manifest(manifest, staging, res_dir, &no_gpu, TRUE) < 0)
        return E_FAIL;

    swprintf_s(cmdline, _countof(cmdline),
               L"\"%s\\iso-patch.exe\" --to-vhdx \"%s\" 1 64 "
               L"--output \"%s\" --stage \"%s\"",
               exe, config->windows_iso_path,
               g_appliance.status.os_vhdx_path, manifest);

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&h_read, &h_write, &sa, 0))
        return HRESULT_FROM_WIN32(GetLastError());
    SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&startup, sizeof(startup)); startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = h_write;
    startup.hStdError = h_write;
    ZeroMemory(&process, sizeof(process));
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup, &process)) {
        DWORD error = GetLastError();
        CloseHandle(h_read); CloseHandle(h_write);
        remove_directory_tree(staging);
        return HRESULT_FROM_WIN32(error);
    }
    CloseHandle(h_write); h_write = INVALID_HANDLE_VALUE;

    ui_log(L"Shared appliance: applying image from \"%s\" (index 1, Core edition)...",
           config->windows_iso_path);
    /* Keep iso-patch's partial output when a step fails so the failure can
       be inspected instead of silently vanishing with only an exit code. */
    SetEnvironmentVariableW(L"ASB_KEEP_PARTIAL_VHDX", L"1");

    while (ReadFile(h_read, buffer + pos, (DWORD)(_countof(buffer) - pos - 1),
                    &bytes_read, NULL) && bytes_read > 0) {
        end = pos + (int)bytes_read;
        buffer[end] = '\0';
        for (i = start; i < end; ++i) {
            if (buffer[i] != '\n' && buffer[i] != '\r') continue;
            buffer[i] = '\0';
            if (i > start) {
                int len = i - start;
                if (len >= (int)_countof(line_a)) len = (int)_countof(line_a) - 1;
                memcpy(line_a, buffer + start, (size_t)len);
                line_a[len] = '\0';
                if (strncmp(line_a, "PROGRESS:", 9) == 0) {
                    int pct = atoi(line_a + 9);
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    swprintf_s(line_w, _countof(line_w),
                               L"Applying the Server Core image (%d%%)...", pct);
                    set_progress(10 + pct / 2, line_w);
                } else if (strncmp(line_a, "STATUS:", 7) == 0) {
                    /* Step changes AND captured external-tool output
                       (bcdboot/BFSVC failure text) ride the STATUS channel. */
                    MultiByteToWideChar(CP_UTF8, 0, line_a + 7, -1,
                                        line_w, _countof(line_w));
                    if (line_w[0]) ui_log(L"Shared appliance: %s", line_w);
                } else if (strncmp(line_a, "ERROR:", 6) == 0) {
                    MultiByteToWideChar(CP_UTF8, 0, line_a + 6, -1,
                                        line_w, _countof(line_w));
                    ui_log(L"Shared appliance: image apply failed: %s", line_w);
                    hr = E_FAIL;
                } else if (strncmp(line_a, "DONE:", 5) == 0) {
                    hr = S_OK;
                }
            }
            start = i + 1;
            if (start < end && (buffer[start] == '\n' || buffer[start] == '\r'))
                start++;
        }
        if (start < end) {
            memmove(buffer, buffer + start, (size_t)(end - start));
            pos = end - start;
        } else {
            pos = 0;
        }
        start = 0;
    }

    WaitForSingleObject(process.hProcess, 45 * 60 * 1000);
    if (GetExitCodeProcess(process.hProcess, &exit_code)) have_exit = TRUE;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(h_read);
    SetEnvironmentVariableW(L"ASB_KEEP_PARTIAL_VHDX", NULL);
    remove_directory_tree(staging);

    if (SUCCEEDED(hr) && (!have_exit || exit_code != 0))
        hr = E_FAIL;
    if (SUCCEEDED(hr))
        set_progress(62, L"Server Core image applied; staging complete.");
    return hr;
}

static void fill_vm_config(VmConfig *config, BOOL provisioning)
{
    ZeroMemory(config, sizeof(*config));
    wcscpy_s(config->name, _countof(config->name), APPLIANCE_VM_NAME);
    wcscpy_s(config->os_type, _countof(config->os_type),
             g_appliance.status.backend == ASB_APPLIANCE_BACKEND_SERVER_CORE ? L"Windows" : L"Linux");
    wcscpy_s(config->vhdx_path, MAX_PATH, g_appliance.status.os_vhdx_path);
    wcscpy_s(config->data_vhdx_path, MAX_PATH, g_appliance.status.data_vhdx_path);
    wcscpy_s(config->storage_root, MAX_PATH, g_appliance.status.storage_root);
    config->ram_mb = g_appliance.status.ram_mb;
    config->hdd_gb = 64;
    config->cpu_cores = g_appliance.status.cpu_cores;
    config->gpu_mode = GPU_NONE;
    config->network_mode = NET_NONE;
    config->is_appliance = TRUE;
    config->test_mode = g_appliance.status.backend == ASB_APPLIANCE_BACKEND_UBUNTU;
    if (provisioning && GetFileAttributesW(g_appliance.seed_iso_path) != INVALID_FILE_ATTRIBUTES) {
        if (g_appliance.status.backend == ASB_APPLIANCE_BACKEND_SERVER_CORE)
            wcscpy_s(config->image_path, MAX_PATH, g_appliance.windows_iso_path);
        wcscpy_s(config->resources_iso_path, MAX_PATH, g_appliance.seed_iso_path);
    }
}

static void cleanup_runtime_network(void)
{
    if (g_appliance.share_endpoint_created) {
        hcn_delete_endpoint(&g_appliance.share_endpoint_id);
        g_appliance.share_endpoint_created = FALSE;
    }
    if (g_appliance.maintenance_endpoint_created) {
        hcn_delete_endpoint(&g_appliance.maintenance_endpoint_id);
        g_appliance.maintenance_endpoint_created = FALSE;
    }
}

static BOOL file_size_bytes(const wchar_t *path, ULONGLONG *out)
{
    HANDLE file;
    LARGE_INTEGER size;
    if (!path || !path[0]) return FALSE;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!GetFileSizeEx(file, &size)) { CloseHandle(file); return FALSE; }
    CloseHandle(file);
    *out = (ULONGLONG)size.QuadPart;
    return TRUE;
}

static HRESULT start_internal(BOOL provisioning, BOOL wait_ready, DWORD timeout_ms);

/* Runs the readiness handshake for a caller that asked not to block. Re-enters
   start_internal, which sees the appliance already STARTING and drops straight
   into the wait. */
static DWORD WINAPI ready_worker(LPVOID parameter)
{
    (void)parameter;
    if (g_appliance.status.state == ASB_APPLIANCE_STATE_STARTING)
        start_internal(FALSE, TRUE, APPLIANCE_SETUP_TIMEOUT_MS);
    InterlockedExchange(&g_appliance.ready_worker_active, 0);
    return 0;
}

static HRESULT start_internal(BOOL provisioning, BOOL wait_ready, DWORD timeout_ms)
{
    VmConfig config;
    wchar_t share_endpoint[64] = L"", maintenance_endpoint[64] = L"";
    char server_ip[32], server_mac[32];
    HRESULT hr;
    ULONGLONG deadline, wait_started, last_report, last_vhdx_bytes;
    BOOL announced_agent, net_configured;
    BOOL agent_seen;
    int agent_connects, commands_sent;
    DWORD elapsed_seconds, saved_progress;
    char response[256] = "";
    const char *base;

    EnterCriticalSection(&g_appliance.cs);
    if (!g_appliance.status.configured && !provisioning) {
        LeaveCriticalSection(&g_appliance.cs); return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
    if (g_appliance.status.ready) { LeaveCriticalSection(&g_appliance.cs); return S_OK; }
    if (g_appliance.status.state == ASB_APPLIANCE_STATE_STARTING) {
        LeaveCriticalSection(&g_appliance.cs);
        goto wait_existing;
    }
    g_appliance.status.state = ASB_APPLIANCE_STATE_STARTING;
    g_appliance.status.busy = TRUE;
    g_appliance.status.last_error[0] = L'\0';
    g_appliance.provisioning_boot = provisioning;
    LeaveCriticalSection(&g_appliance.cs);

    base = hcn_share_subnet_base();
    sprintf_s(server_ip, sizeof(server_ip), "%s.2", base);
    strcpy_s(server_mac, sizeof(server_mac), "02-15-5D-A5-B0-02");
    ui_log(L"Shared appliance: creating share network endpoint (%S)...", server_ip);
    hr = hcn_create_share_network(&g_appliance.share_network_id);
    if (SUCCEEDED(hr)) hr = hcn_create_share_server_endpoint(
        &g_appliance.share_network_id, &g_appliance.share_endpoint_id,
        share_endpoint, _countof(share_endpoint), server_ip, server_mac);
    if (FAILED(hr)) goto fail;
    g_appliance.share_endpoint_created = TRUE;

    if (provisioning) {
        char nat_ip[32];
        ui_log(L"Shared appliance: creating maintenance NAT endpoint...");
        hr = hcn_create_nat_network(&g_appliance.maintenance_network_id);
        if (SUCCEEDED(hr)) {
            sprintf_s(nat_ip, sizeof(nat_ip), "%s.254", hcn_nat_subnet_base());
            hr = hcn_create_endpoint(&g_appliance.maintenance_network_id,
                &g_appliance.maintenance_endpoint_id, maintenance_endpoint,
                _countof(maintenance_endpoint), nat_ip);
        }
        if (FAILED(hr)) goto fail;
        g_appliance.maintenance_endpoint_created = TRUE;
    }

    ZeroMemory(&g_appliance.runtime, sizeof(g_appliance.runtime));
    g_appliance.runtime.unique_id = APPLIANCE_UNIQUE_ID;
    wcscpy_s(g_appliance.runtime.name, _countof(g_appliance.runtime.name), APPLIANCE_VM_NAME);
    wcscpy_s(g_appliance.runtime.os_type, _countof(g_appliance.runtime.os_type),
             g_appliance.status.backend == ASB_APPLIANCE_BACKEND_SERVER_CORE ? L"Windows" : L"Linux");
    wcscpy_s(g_appliance.runtime.vhdx_path, MAX_PATH, g_appliance.status.os_vhdx_path);
    strcpy_s(g_appliance.runtime.share_ip, sizeof(g_appliance.runtime.share_ip), server_ip);
    strcpy_s(g_appliance.runtime.share_mac, sizeof(g_appliance.runtime.share_mac), server_mac);
    /* Must stay within shared_resource_transport[16] INCLUDING the NUL, and
       must equal the token vm_agent.c compares against ("appliance") or the
       appliance runtime loses its transport identity. A previous 16-char
       value ("appliance-server") overflowed by the NUL and fast-failed the
       CRT on every unwrapped boot. */
    wcscpy_s(g_appliance.runtime.shared_resource_transport,
             _countof(g_appliance.runtime.shared_resource_transport), L"appliance");
    /* The appliance keeps whatever agent it was provisioned with -- nothing
       else ever refreshes it -- so a guest-side fix would only reach a rebuilt
       appliance. Refresh it here, while the disk is still offline. Failure is
       not fatal: the existing agent may well be current. */
    if (!provisioning &&
        g_appliance.status.backend == ASB_APPLIANCE_BACKEND_SERVER_CORE) {
        HRESULT agent_hr = asb_upgrade_windows_agent_offline(
            g_appliance.status.os_vhdx_path);
        if (FAILED(agent_hr))
            ui_log(L"Shared appliance: could not refresh the appliance agent (0x%08X); using the installed one.",
                   agent_hr);
    }

    fill_vm_config(&config, provisioning);
    hcs_destroy_stale(APPLIANCE_VM_NAME);
    ui_log(L"Shared appliance: creating compute system (%S boot)...",
           provisioning ? "provisioning" : "normal");
    hr = hcs_create_vm_with_endpoints(&config,
        maintenance_endpoint[0] ? maintenance_endpoint : NULL,
        share_endpoint, &g_appliance.runtime);
    if (FAILED(hr)) goto fail;
    hr = hcs_start_vm(&g_appliance.runtime);
    if (FAILED(hr)) goto fail;
    ui_log(L"Shared appliance: VM started; polling for the guest agent...");
    vm_agent_start(&g_appliance.runtime);
    hcs_start_monitor(&g_appliance.runtime);

wait_existing:
    /* A caller that does not want to block still needs the readiness handshake
       to happen: appliance_ready is what initializes the data volume, creates
       the SMB service account and publishes the shares, and nothing else does
       it. Returning here without it -- as the GUI's Start button did -- leaves
       the appliance running but permanently un-ready, so every resource waits
       forever to be published. Hand the wait to a worker instead. */
    if (!wait_ready) {
        if (InterlockedCompareExchange(&g_appliance.ready_worker_active, 1, 0) == 0) {
            HANDLE worker = CreateThread(NULL, 0, ready_worker, NULL, 0, NULL);
            if (worker) CloseHandle(worker);
            else InterlockedExchange(&g_appliance.ready_worker_active, 0);
        }
        return S_OK;
    }
    deadline = GetTickCount64() + (timeout_ms ? timeout_ms : 120000);
    wait_started = GetTickCount64();
    last_report = wait_started;
    announced_agent = FALSE;
    net_configured = FALSE;
    agent_seen = FALSE;
    agent_connects = 0;
    commands_sent = 0;
    file_size_bytes(g_appliance.status.os_vhdx_path, &last_vhdx_bytes);
    while (GetTickCount64() < deadline) {
        /* Unattended installs run 10-30+ minutes with no agent to talk to.
           Without the heartbeat below the UI sits frozen at 65% and any
           terminal request fails with ERROR_NOT_READY, looking like a hang.
           The OS disk size is polled alongside the timer: Windows Setup
           writing image data is the only host-visible install progress. */
        if (!announced_agent && g_appliance.runtime.agent_online) {
            announced_agent = TRUE;
            set_progress(90, L"Appliance agent online; verifying readiness...");
        }
        /* Report every connect/disconnect. A guest that reboots once during
           first-boot setup looks identical, in a single snapshot of the log,
           to an agent that crashes and is restarted on every command -- but
           the two need completely different fixes, and the count tells them
           apart. */
        if (agent_seen != (g_appliance.runtime.agent_online != FALSE)) {
            agent_seen = g_appliance.runtime.agent_online != FALSE;
            if (agent_seen)
                ui_log(L"Shared appliance: guest agent connected (connection %d).",
                       ++agent_connects);
            else
                ui_log(L"Shared appliance: guest agent disconnected after %d command(s) -- "
                       L"the appliance is rebooting or its agent restarted; waiting for it.",
                       commands_sent);
            net_configured = FALSE;   /* the new boot needs its adapter set up again */
        }
        if (GetTickCount64() - last_report >= 30000) {
            elapsed_seconds = (DWORD)((GetTickCount64() - wait_started) / 1000);
            EnterCriticalSection(&g_appliance.cs);
            saved_progress = g_appliance.status.progress;
            LeaveCriticalSection(&g_appliance.cs);
            if (g_appliance.runtime.agent_online) {
                set_progress(max(saved_progress, 90),
                             L"Verifying the shared appliance agent...");
            } else if (provisioning) {
                wchar_t heartbeat[256];
                ULONGLONG now_bytes = 0, grown_mb = 0;
                BOOL have_size = file_size_bytes(g_appliance.status.os_vhdx_path,
                                                 &now_bytes);
                if (have_size && now_bytes > last_vhdx_bytes)
                    grown_mb = (now_bytes - last_vhdx_bytes) / (1024ULL * 1024ULL);
                if (have_size && grown_mb)
                    swprintf_s(heartbeat, _countof(heartbeat),
                               L"Unattended install running (%u:%02u elapsed): "
                               L"setup wrote %I64u MB so far (%I64u MB on disk)...",
                               elapsed_seconds / 60, elapsed_seconds % 60,
                               grown_mb, now_bytes / (1024ULL * 1024ULL));
                else if (have_size)
                    swprintf_s(heartbeat, _countof(heartbeat),
                               L"Unattended install running (%u:%02u elapsed, "
                               L"install disk at %I64u MB)...",
                               elapsed_seconds / 60, elapsed_seconds % 60,
                               now_bytes / (1024ULL * 1024ULL));
                else
                    swprintf_s(heartbeat, _countof(heartbeat),
                               L"Unattended install running (%u:%02u elapsed)...",
                               elapsed_seconds / 60, elapsed_seconds % 60);
                set_progress(min(66 + elapsed_seconds / 30, 92), heartbeat);
                last_vhdx_bytes = now_bytes;
            }
            last_report = GetTickCount64();
        }
        if (g_appliance.runtime.agent_online) {
            /* Both commands are long-running guest work: shared_net waits up
               to 30s for the private adapter to appear, and appliance_ready
               initializes the data disk, enables PSRemoting and mints the
               management certificate -- minutes on a first boot. A short
               timeout here does not cancel the guest, it just re-sends on the
               next pass, so the agent ends up serving a backlog of overlapping
               heavy commands and the connection eventually drops. Send each
               one once and wait for the guest's own answer. */
            if (!net_configured &&
                g_appliance.status.backend == ASB_APPLIANCE_BACKEND_SERVER_CORE) {
                char net_command[128];
                sprintf_s(net_command, sizeof(net_command), "shared_net:%s:%s",
                          g_appliance.runtime.share_mac, g_appliance.runtime.share_ip);
                commands_sent++;
                if (appliance_send(net_command, response, sizeof(response), 60000))
                    net_configured = TRUE;
                else if (g_appliance.runtime.agent_online)
                    ui_log(L"Shared appliance: private adapter configuration failed (%S); retrying.",
                           response[0] ? response : "no reply");
                /* Otherwise the agent went away mid-command: the transition
                   log above already says so, and saying "failed" here would
                   blame the command for the disconnect. */
            }
            if (appliance_send("appliance_ready",
                              response, sizeof(response), 300000)) {
                if (g_appliance.status.backend == ASB_APPLIANCE_BACKEND_SERVER_CORE &&
                    strncmp(response, "ok:", 3) == 0 && response[3]) {
                    MultiByteToWideChar(CP_UTF8, 0, response + 3, -1,
                                        g_appliance.status.management_cert_thumbprint,
                                        _countof(g_appliance.status.management_cert_thumbprint));
                    EnterCriticalSection(&g_appliance.cs);
                    save_config_locked();
                    LeaveCriticalSection(&g_appliance.cs);
                }
                /* Not the public entry point: status.ready is still FALSE
                   here, and this is the pass that publishes the whole set. */
                hr = reconcile_internal();
                if (FAILED(hr)) goto fail;
                EnterCriticalSection(&g_appliance.cs);
                g_appliance.status.state = ASB_APPLIANCE_STATE_READY;
                g_appliance.status.ready = TRUE;
                g_appliance.status.busy = FALSE;
                g_appliance.status.progress = 100;
                wcscpy_s(g_appliance.status.progress_text, 256, L"Ready");
                g_appliance.restart_attempts = 0;
                LeaveCriticalSection(&g_appliance.cs);
                return S_OK;
            }
        }
        Sleep(1000);
    }
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
fail:
    EnterCriticalSection(&g_appliance.cs);
    set_error_locked(hr,
        hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)
            ? L"The shared appliance did not become ready within the allotted time"
            : L"Failed to start or verify the shared appliance");
    LeaveCriticalSection(&g_appliance.cs);
    return hr;
}

static DWORD WINAPI setup_worker(LPVOID parameter)
{
    SetupWorkerArgs *args = (SetupWorkerArgs *)parameter;
    HRESULT hr;
    wchar_t archived[MAX_PATH];
    SYSTEMTIME now;

    if (args->rebuild && args->switch_backend &&
        GetFileAttributesW(g_appliance.status.data_vhdx_path) != INVALID_FILE_ATTRIBUTES) {
        GetLocalTime(&now);
        swprintf_s(archived, _countof(archived), L"%s\\data-%04u%02u%02u-%02u%02u%02u.vhdx.archive",
            g_appliance.status.storage_root, now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond);
        if (!MoveFileExW(g_appliance.status.data_vhdx_path, archived, MOVEFILE_WRITE_THROUGH)) {
            hr = HRESULT_FROM_WIN32(GetLastError()); goto failed;
        }
    }

    if (!args->rebuild || args->switch_backend ||
        GetFileAttributesW(g_appliance.status.data_vhdx_path) == INVALID_FILE_ATTRIBUTES) {
        set_progress(3, L"Creating shared data VHDX...");
        DeleteFileW(g_appliance.status.data_vhdx_path);
        hr = vhdx_create(g_appliance.status.data_vhdx_path,
                         g_appliance.status.data_size_gb);
        if (FAILED(hr)) goto failed;
    }
    DeleteFileW(g_appliance.status.os_vhdx_path);
    DeleteFileW(g_appliance.seed_iso_path);
    if (args->config.backend == ASB_APPLIANCE_BACKEND_UBUNTU) {
        hr = build_ubuntu_os(&args->config);
        if (FAILED(hr)) goto failed;

        set_progress(65, L"Booting appliance for unattended provisioning...");
        {
            _invalid_parameter_handler previous_handler;
            begin_invalid_parameter_capture(&previous_handler);
            hr = start_internal(TRUE, TRUE, APPLIANCE_SETUP_TIMEOUT_MS);
            hr = end_invalid_parameter_capture(previous_handler, hr);
        }
        if (FAILED(hr)) goto failed;
        EnterCriticalSection(&g_appliance.cs);
        g_appliance.status.configured = TRUE;
        save_config_locked();
        LeaveCriticalSection(&g_appliance.cs);
        /* Provisioning media contains one-use secrets. Remove it before the normal
           appliance boot and drop the temporary maintenance/NAT adapter. */
        shared_appliance_stop(FALSE);
        DeleteFileW(g_appliance.seed_iso_path);
        hr = start_internal(FALSE, TRUE, 120000);
        if (FAILED(hr)) goto failed;
    } else {
        /* Windows Server Core is applied offline; no provisioning media and
           no firmware boot needed. First boot runs specialize/oobeSystem and
           brings the agent up, which can take several minutes. */
        hr = build_server_core_os(&args->config);
        if (FAILED(hr)) goto failed;
        set_progress(65, L"Starting the appliance for first boot...");
        EnterCriticalSection(&g_appliance.cs);
        g_appliance.status.configured = TRUE;
        save_config_locked();
        LeaveCriticalSection(&g_appliance.cs);
        hr = start_internal(FALSE, TRUE, APPLIANCE_SETUP_TIMEOUT_MS);
        if (FAILED(hr)) goto failed;
    }
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.status.busy = FALSE;
    save_config_locked();
    LeaveCriticalSection(&g_appliance.cs);
    SecureZeroMemory(&args->config.admin_password, sizeof(args->config.admin_password));
    SecureZeroMemory(&args->config.product_key, sizeof(args->config.product_key));
    HeapFree(GetProcessHeap(), 0, args);
    return 0;
failed:
    shared_appliance_stop(TRUE);
    if (args->cleanup_on_failure)
        remove_directory_tree(g_appliance.status.storage_root);
    EnterCriticalSection(&g_appliance.cs);
    set_error_locked(hr, L"Shared appliance provisioning failed");
    LeaveCriticalSection(&g_appliance.cs);
    SecureZeroMemory(&args->config.admin_password, sizeof(args->config.admin_password));
    SecureZeroMemory(&args->config.product_key, sizeof(args->config.product_key));
    HeapFree(GetProcessHeap(), 0, args);
    return 1;
}

void shared_appliance_init(void)
{
    if (g_appliance.initialized) return;
    ZeroMemory(&g_appliance, sizeof(g_appliance));
    InitializeCriticalSection(&g_appliance.cs);
    InitializeCriticalSection(&g_appliance.command_cs);
    g_appliance.initialized = TRUE;
    g_appliance.status.state = ASB_APPLIANCE_STATE_UNCONFIGURED;
    g_appliance.status.data_size_gb = 256;
    load_config();
}

void shared_appliance_cleanup(void)
{
    int i;
    if (!g_appliance.initialized) return;
    for (i = 0; i < shared_resources_count(); ++i) {
        const AsbSharedResourceInfo *resource = shared_resources_get(i);
        if (resource && resource->host_drive_letter)
            host_mapping_command(resource, FALSE);
    }
    g_appliance.status.host_mounts = 0;
    shared_appliance_stop(FALSE);
    cleanup_runtime_network();
}

void shared_appliance_get_status(SharedApplianceStatus *out)
{
    if (!out) return;
    EnterCriticalSection(&g_appliance.cs);
    *out = g_appliance.status;
    LeaveCriticalSection(&g_appliance.cs);
}

HRESULT shared_appliance_setup(const SharedApplianceConfig *config)
{
    SetupWorkerArgs *args;
    HANDLE thread;
    HRESULT hr;
    wchar_t root[MAX_PATH], smb_password[128];
    DWORD attrs;
    BOOL owned_stale_root = FALSE;
    if (!config || (config->backend != ASB_APPLIANCE_BACKEND_UBUNTU &&
        config->backend != ASB_APPLIANCE_BACKEND_SERVER_CORE) ||
        !config->admin_user[0] || !config->admin_password[0] ||
        config->data_size_gb < 8 || config->data_size_gb > 65536)
        return E_INVALIDARG;
    if (!valid_account_name(config->admin_user,
            config->backend == ASB_APPLIANCE_BACKEND_UBUNTU))
        return E_INVALIDARG;
    if (config->backend == ASB_APPLIANCE_BACKEND_SERVER_CORE &&
        (!config->windows_iso_path[0] || !config->windows_image_name[0] ||
         !valid_xml_text(config->windows_image_name) ||
         (config->product_key[0] && !valid_xml_text(config->product_key))))
        return E_INVALIDARG;
    hr = validate_storage_parent(config->storage_parent); if (FAILED(hr)) return hr;
    swprintf_s(root, _countof(root), L"%s\\AppSandboxSharedAppliance", config->storage_parent);
    attrs = GetFileAttributesW(root);
    if (attrs != INVALID_FILE_ATTRIBUTES && !g_appliance.status.configured) {
        /* A terminated first-time setup leaves the atomically persisted config
           and its partial managed tree behind. Reuse only that exact tree;
           an unrelated directory with the reserved name remains a collision. */
        owned_stale_root = g_appliance.status.storage_root[0] &&
            _wcsicmp(g_appliance.status.storage_root, root) == 0;
        if (!owned_stale_root)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }
    hr = ensure_directory(root); if (FAILED(hr)) return hr;
    hr = generate_password(smb_password, _countof(smb_password)); if (FAILED(hr)) return hr;
    hr = save_credentials(config->admin_password, smb_password);
    SecureZeroMemory(smb_password, sizeof(smb_password));
    if (FAILED(hr)) return hr;

    EnterCriticalSection(&g_appliance.cs);
    if (g_appliance.status.busy) { LeaveCriticalSection(&g_appliance.cs); return HRESULT_FROM_WIN32(ERROR_BUSY); }
    g_appliance.status.backend = config->backend;
    g_appliance.status.state = ASB_APPLIANCE_STATE_PROVISIONING;
    g_appliance.status.busy = TRUE;
    g_appliance.status.ready = FALSE;
    g_appliance.status.progress = 0;
    g_appliance.status.data_size_gb = config->data_size_gb;
    g_appliance.status.ram_mb = config->ram_mb ? config->ram_mb :
        (config->backend == ASB_APPLIANCE_BACKEND_UBUNTU ? 1024 : 2048);
    g_appliance.status.cpu_cores = config->cpu_cores ? config->cpu_cores :
        (config->backend == ASB_APPLIANCE_BACKEND_UBUNTU ? 1 : 2);
    wcscpy_s(g_appliance.status.storage_root, MAX_PATH, root);
    swprintf_s(g_appliance.status.os_vhdx_path, MAX_PATH, L"%s\\os.vhdx", root);
    swprintf_s(g_appliance.status.data_vhdx_path, MAX_PATH, L"%s\\data.vhdx", root);
    swprintf_s(g_appliance.seed_iso_path, MAX_PATH, L"%s\\provision.iso", root);
    wcscpy_s(g_appliance.status.admin_user, 128, config->admin_user);
    wcscpy_s(g_appliance.status.windows_image_name, 256, config->windows_image_name);
    wcscpy_s(g_appliance.windows_iso_path, MAX_PATH, config->windows_iso_path);
    wcscpy_s(g_appliance.status.progress_text, 256, L"Queued");
    save_config_locked();
    LeaveCriticalSection(&g_appliance.cs);

    args = (SetupWorkerArgs *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*args));
    if (!args) return E_OUTOFMEMORY;
    args->config = *config;
    args->cleanup_on_failure = attrs == INVALID_FILE_ATTRIBUTES;
    thread = CreateThread(NULL, 0, setup_worker, args, 0, NULL);
    if (!thread) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        if (args->cleanup_on_failure) remove_directory_tree(root);
        HeapFree(GetProcessHeap(), 0, args);
        return hr;
    }
    CloseHandle(thread);
    return S_OK;
}

HRESULT shared_appliance_start(BOOL wait_ready, DWORD timeout_ms)
{
    return start_internal(FALSE, wait_ready, timeout_ms);
}

HRESULT shared_appliance_stop(BOOL force)
{
    HRESULT hr = S_OK;
    if (!g_appliance.initialized || !g_appliance.runtime.running) return S_OK;
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.stopping = TRUE;
    g_appliance.status.state = ASB_APPLIANCE_STATE_STOPPING;
    g_appliance.status.busy = TRUE;
    LeaveCriticalSection(&g_appliance.cs);
    hr = force ? hcs_terminate_vm(&g_appliance.runtime) : hcs_stop_vm(&g_appliance.runtime);
    if (FAILED(hr) && !force) hr = hcs_terminate_vm(&g_appliance.runtime);
    if (SUCCEEDED(hr) && !force) {
        ULONGLONG deadline = GetTickCount64() + 30000;
        while (g_appliance.runtime.running && GetTickCount64() < deadline)
            Sleep(100);
        if (g_appliance.runtime.running)
            hr = hcs_terminate_vm(&g_appliance.runtime);
    }
    vm_agent_stop(&g_appliance.runtime);
    hcs_stop_monitor(&g_appliance.runtime);
    hcs_unregister_vm_callback(&g_appliance.runtime);
    if (g_appliance.runtime.handle) {
        hcs_close_handle_sync(g_appliance.runtime.handle);
        g_appliance.runtime.handle = NULL;
    }
    cleanup_runtime_network();
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.runtime.running = FALSE;
    g_appliance.status.ready = FALSE;
    g_appliance.status.busy = FALSE;
    g_appliance.status.state = g_appliance.status.configured ?
        ASB_APPLIANCE_STATE_STOPPED : ASB_APPLIANCE_STATE_UNCONFIGURED;
    wcscpy_s(g_appliance.status.progress_text, 256, L"Stopped");
    g_appliance.stopping = FALSE;
    LeaveCriticalSection(&g_appliance.cs);
    return hr;
}

/* Publishing before the startup pass has run appliance_ready would create the
   share directories under a mount point the data volume is not mounted on yet,
   and would race the startup command sequence. The startup pass publishes the
   whole set itself, so deferring costs nothing. */
HRESULT shared_appliance_reconcile(void)
{
    if (!g_appliance.runtime.agent_online)
        return HRESULT_FROM_WIN32(ERROR_NOT_CONNECTED);
    if (!g_appliance.status.ready)
        return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    return reconcile_internal();
}

static HRESULT reconcile_internal(void)
{
    wchar_t admin_password[256], smb_password[256], smb_user[128];
    char user[128], password[256], command[1024], response[256];
    int i, count;
    HcsSharedResource resources[ASB_MAX_SHARED_RESOURCES];
    HRESULT hr;
    response[0] = '\0';   /* a send that never reaches the guest leaves it untouched */
    if (!g_appliance.runtime.agent_online) return HRESULT_FROM_WIN32(ERROR_NOT_CONNECTED);
    hr = load_credentials(admin_password, _countof(admin_password), smb_password, _countof(smb_password));
    if (FAILED(hr)) return hr;
    wcscpy_s(smb_user, _countof(smb_user), APPLIANCE_SERVICE_USER);
    WideCharToMultiByte(CP_UTF8, 0, smb_user, -1, user, sizeof(user), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, smb_password, -1, password, sizeof(password), NULL, NULL);
    sprintf_s(command, sizeof(command), "appliance_account:%s:%s", user, password);
    if (!appliance_send(command, response, sizeof(response), 30000)) {
        /* vm_agent_send redacts this command, so name the step here or the
           only trace of the failure is an anonymous "credential-bearing
           command" line. The reply carries the guest's Win32/HRESULT code. */
        ui_log(L"Shared appliance: creating the %s SMB service account failed (%S).",
               APPLIANCE_SERVICE_USER, response[0] ? response : "no reply");
        SecureZeroMemory(admin_password, sizeof(admin_password));
        SecureZeroMemory(smb_password, sizeof(smb_password));
        SecureZeroMemory(password, sizeof(password));
        return HRESULT_FROM_WIN32(ERROR_LOGON_FAILURE);
    }
    SecureZeroMemory(password, sizeof(password));
    count = shared_resources_build_attachments(L"Windows", L"", resources, _countof(resources));
    for (i = 0; i < count; ++i) {
        char share[128];
        WideCharToMultiByte(CP_UTF8, 0, resources[i].share_name, -1, share, sizeof(share), NULL, NULL);
        sprintf_s(command, sizeof(command), "appliance_reconcile:%s:%s",
                  share, resources[i].read_only ? "ro" : "rw");
        if (!appliance_send(command, response, sizeof(response), 30000)) {
            ui_log(L"Shared appliance: publishing share %S failed (%S).",
                   share, response[0] ? response : "no reply");
            SecureZeroMemory(admin_password, sizeof(admin_password));
            SecureZeroMemory(smb_password, sizeof(smb_password));
            return E_FAIL;
        }
    }
    /* build_attachments only returns the enabled definitions, so a resource
       that was disabled would otherwise stay served under its old name. Its
       directory and data are left alone -- only the share goes away. */
    for (i = 0; i < shared_resources_count(); ++i) {
        const AsbSharedResourceInfo *resource = shared_resources_get(i);
        wchar_t share_w[64];
        char share[128];
        if (!resource || resource->enabled) continue;
        shared_resources_share_name(resource->id, share_w, _countof(share_w));
        WideCharToMultiByte(CP_UTF8, 0, share_w, -1, share, sizeof(share), NULL, NULL);
        sprintf_s(command, sizeof(command), "appliance_remove:%s", share);
        appliance_send(command, response, sizeof(response), 30000);
    }
    SecureZeroMemory(admin_password, sizeof(admin_password));
    SecureZeroMemory(smb_password, sizeof(smb_password));
    return S_OK;
}

static BOOL appliance_client_ip_in_use(const VmInstance *self, const char *ip)
{
    int i;
    for (i = 0; i < asb_vm_count(); ++i) {
        VmInstance *other = asb_vm_instance(asb_vm_get(i));
        if (other && other != self && !other->share_network_cleaned && other->share_ip[0] &&
            strcmp(other->share_ip, ip) == 0) return TRUE;
    }
    return FALSE;
}

HRESULT shared_appliance_prepare_client(const VmConfig *config,
                                        VmInstance *runtime,
                                        wchar_t *endpoint_guid,
                                        size_t endpoint_guid_chars,
                                        BOOL allow_missing)
{
    const char *base;
    unsigned int hash = 2166136261u;
    const wchar_t *p;
    int octet, attempts;
    HRESULT hr;
    if (!endpoint_guid || !runtime) return E_INVALIDARG;
    endpoint_guid[0] = L'\0';
    if (!config || config->shared_resource_count <= 0 ||
        _wcsicmp(config->os_type, L"Windows") != 0) return S_FALSE;
    hr = shared_appliance_start(TRUE, 120000);
    if (FAILED(hr)) return allow_missing ? S_FALSE : HRESULT_FROM_WIN32(ERROR_NOT_READY);
    base = hcn_share_subnet_base();
    for (p = config->name; *p; ++p) { hash ^= towlower(*p); hash *= 16777619u; }
    octet = 17 + (int)(hash % 230u);
    for (attempts = 0; attempts < 238; ++attempts) {
        sprintf_s(runtime->share_ip, sizeof(runtime->share_ip), "%s.%d", base, octet);
        if (!appliance_client_ip_in_use(runtime, runtime->share_ip)) break;
        if (++octet > 246) octet = 17;
    }
    if (attempts == 238) return HRESULT_FROM_WIN32(ERROR_ADDRESS_ALREADY_ASSOCIATED);
    sprintf_s(runtime->share_host_ip, sizeof(runtime->share_host_ip), "%s.2", base);
    sprintf_s(runtime->share_mac, sizeof(runtime->share_mac), "02-15-5D-%02X-%02X-%02X",
              (hash >> 16) & 0xff, (hash >> 8) & 0xff, hash & 0xff);
    runtime->share_network_id = g_appliance.share_network_id;
    hr = hcn_create_share_endpoint(&runtime->share_network_id,
        &runtime->share_endpoint_id, endpoint_guid, endpoint_guid_chars,
        runtime->share_ip, runtime->share_mac);
    if (SUCCEEDED(hr)) {
        runtime->share_network_cleaned = FALSE;
        InterlockedIncrement(&g_appliance.status.active_clients);
        InterlockedIncrement(&g_appliance.idle_generation);
    }
    return hr;
}

static DWORD WINAPI idle_stop_thread(LPVOID parameter)
{
    LONG generation = (LONG)(LONG_PTR)parameter;
    ULONGLONG deadline = GetTickCount64() + APPLIANCE_IDLE_MS;
    while (GetTickCount64() < deadline) {
        if (generation != g_appliance.idle_generation) return 0;
        Sleep(1000);
    }
    if (generation == g_appliance.idle_generation &&
        g_appliance.status.active_clients == 0 && g_appliance.status.host_mounts == 0)
        shared_appliance_stop(FALSE);
    return 0;
}

void shared_appliance_release_client(VmInstance *runtime)
{
    HANDLE thread;
    LONG generation;
    BOOL attached = runtime && !runtime->share_network_cleaned && runtime->share_ip[0];
    if (attached) {
        hcn_delete_endpoint(&runtime->share_endpoint_id);
        runtime->share_network_cleaned = TRUE;
        runtime->share_ip[0] = '\0';
        runtime->share_host_ip[0] = '\0';
        runtime->share_mac[0] = '\0';
    }
    if (attached && g_appliance.status.active_clients > 0)
        InterlockedDecrement(&g_appliance.status.active_clients);
    generation = InterlockedIncrement(&g_appliance.idle_generation);
    if (g_appliance.status.active_clients == 0 && g_appliance.status.host_mounts == 0) {
        thread = CreateThread(NULL, 0, idle_stop_thread, (LPVOID)(LONG_PTR)generation, 0, NULL);
        if (thread) CloseHandle(thread);
    }
}

HRESULT shared_appliance_grow(DWORD new_size_gb)
{
    ULONGLONG old_bytes, new_bytes;
    char command[128], response[256];
    HRESULT hr, guest_hr = S_OK;
    BOOL was_ready = g_appliance.status.ready;
    if (new_size_gb <= g_appliance.status.data_size_gb) return E_INVALIDARG;
    if (g_appliance.status.active_clients || g_appliance.status.host_mounts)
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    hr = shared_appliance_stop(FALSE); if (FAILED(hr)) return hr;
    hr = vhdx_resize_grow(g_appliance.status.data_vhdx_path, new_size_gb,
                          &old_bytes, &new_bytes);
    if (FAILED(hr)) return hr;
    hr = shared_appliance_start(TRUE, 120000); if (FAILED(hr)) return hr;
    sprintf_s(command, sizeof(command), "appliance_grow:%lu", new_size_gb);
    if (!appliance_send(command, response, sizeof(response), 120000))
        guest_hr = HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.status.data_size_gb = new_size_gb;
    hr = save_config_locked();
    LeaveCriticalSection(&g_appliance.cs);
    if (!was_ready) shared_appliance_stop(FALSE);
    if (FAILED(hr)) return hr;
    if (FAILED(guest_hr)) {
        EnterCriticalSection(&g_appliance.cs);
        wcscpy_s(g_appliance.status.last_error, _countof(g_appliance.status.last_error),
            L"The data VHDX grew, but guest partition/filesystem expansion failed; manual expansion is required.");
        LeaveCriticalSection(&g_appliance.cs);
    }
    return guest_hr;
}

HRESULT shared_appliance_update(void)
{
    char response[256];
    HRESULT hr;
    if (g_appliance.status.active_clients || g_appliance.status.host_mounts)
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    hr = shared_appliance_stop(FALSE); if (FAILED(hr)) return hr;
    hr = start_internal(TRUE, TRUE, 120000); if (FAILED(hr)) return hr;
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.status.state = ASB_APPLIANCE_STATE_UPDATING;
    g_appliance.status.busy = TRUE;
    LeaveCriticalSection(&g_appliance.cs);
    if (!appliance_send("appliance_update", response, sizeof(response), 30 * 60 * 1000))
        hr = E_FAIL;
    shared_appliance_stop(FALSE);
    if (SUCCEEDED(hr)) hr = start_internal(FALSE, TRUE, 120000);
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.status.state = SUCCEEDED(hr) ? ASB_APPLIANCE_STATE_READY : ASB_APPLIANCE_STATE_FAILED;
    g_appliance.status.busy = FALSE;
    LeaveCriticalSection(&g_appliance.cs);
    return hr;
}

HRESULT shared_appliance_rebuild(const SharedApplianceConfig *replacement,
                                 BOOL switch_backend)
{
    SetupWorkerArgs *args;
    HANDLE thread;
    HRESULT hr;
    if (!replacement || g_appliance.status.active_clients || g_appliance.status.host_mounts)
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    hr = shared_appliance_stop(FALSE); if (FAILED(hr)) return hr;
    args = (SetupWorkerArgs *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*args));
    if (!args) return E_OUTOFMEMORY;
    args->config = *replacement;
    args->rebuild = TRUE;
    args->switch_backend = switch_backend;
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.status.backend = replacement->backend;
    g_appliance.status.state = ASB_APPLIANCE_STATE_PROVISIONING;
    g_appliance.status.busy = TRUE;
    g_appliance.status.ready = FALSE;
    wcscpy_s(g_appliance.status.admin_user, 128, replacement->admin_user);
    wcscpy_s(g_appliance.status.windows_image_name, 256, replacement->windows_image_name);
    wcscpy_s(g_appliance.windows_iso_path, MAX_PATH, replacement->windows_iso_path);
    save_config_locked();
    LeaveCriticalSection(&g_appliance.cs);
    thread = CreateThread(NULL, 0, setup_worker, args, 0, NULL);
    if (!thread) { hr = HRESULT_FROM_WIN32(GetLastError()); HeapFree(GetProcessHeap(), 0, args); return hr; }
    CloseHandle(thread);
    return S_OK;
}

static HRESULT host_mapping_command(const AsbSharedResourceInfo *resource, BOOL mount)
{
    wchar_t admin[256], password[256], command[4096], share[64], target[256];
    DWORD exit_code;
    HRESULT hr;
    if (!resource || !resource->host_drive_letter) return E_INVALIDARG;
    if (mount) {
        hr = load_credentials(admin, _countof(admin), password, _countof(password));
        if (FAILED(hr)) return hr;
        SetEnvironmentVariableW(L"ASB_SMB_PASSWORD", password);
        SetEnvironmentVariableW(L"ASB_SMB_USER", APPLIANCE_SERVICE_USER);
    }
    shared_resources_share_name(resource->id, share, _countof(share));
    swprintf_s(target, _countof(target), L"\\\\%S\\%s", shared_appliance_server_ip(), share);
    if (mount) {
        swprintf_s(command, _countof(command),
          L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
          L"$p=ConvertTo-SecureString $env:ASB_SMB_PASSWORD -AsPlainText -Force;"
          L"$c=[pscredential]::new($env:ASB_SMB_USER,$p);"
          L"New-SmbGlobalMapping -LocalPath '%c:' -RemotePath '%s' -Credential $c "
          L"-Persistent $false -RequireIntegrity $true -ErrorAction Stop|Out-Null\"",
          resource->host_drive_letter, target);
    } else {
        swprintf_s(command, _countof(command),
          L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
          L"Remove-SmbGlobalMapping -LocalPath '%c:' -Force -ErrorAction SilentlyContinue\"",
          resource->host_drive_letter);
    }
    hr = run_hidden_process(command, 60000);
    exit_code = HRESULT_CODE(hr);
    (void)exit_code;
    SetEnvironmentVariableW(L"ASB_SMB_PASSWORD", NULL);
    SetEnvironmentVariableW(L"ASB_SMB_USER", NULL);
    SecureZeroMemory(password, sizeof(password));
    SecureZeroMemory(admin, sizeof(admin));
    return hr;
}

HRESULT shared_appliance_mount_host_resource(const wchar_t *resource_id)
{
    const AsbSharedResourceInfo *resource = shared_resources_find(resource_id);
    HRESULT hr;
    if (!resource) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    hr = shared_appliance_start(TRUE, 120000); if (FAILED(hr)) return hr;
    hr = host_mapping_command(resource, TRUE);
    if (SUCCEEDED(hr)) InterlockedIncrement(&g_appliance.status.host_mounts);
    return hr;
}

HRESULT shared_appliance_unmount_host_resource(const wchar_t *resource_id)
{
    const AsbSharedResourceInfo *resource = shared_resources_find(resource_id);
    HRESULT hr;
    if (!resource) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    hr = host_mapping_command(resource, FALSE);
    if (SUCCEEDED(hr) && g_appliance.status.host_mounts > 0)
        InterlockedDecrement(&g_appliance.status.host_mounts);
    InterlockedIncrement(&g_appliance.idle_generation);
    return hr;
}

HRESULT shared_appliance_purge_resource(const wchar_t *resource_id)
{
    const AsbSharedResourceInfo *resource = shared_resources_find(resource_id);
    wchar_t share_w[64]; char share[128], command[256], response[256];
    HRESULT hr;
    if (!resource) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (g_appliance.status.active_clients || g_appliance.status.host_mounts)
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    hr = shared_appliance_start(TRUE, 120000); if (FAILED(hr)) return hr;
    shared_resources_share_name(resource->id, share_w, _countof(share_w));
    WideCharToMultiByte(CP_UTF8, 0, share_w, -1, share, sizeof(share), NULL, NULL);
    sprintf_s(command, sizeof(command), "appliance_purge:%s", share);
    return appliance_send(command, response, sizeof(response), 120000) ? S_OK : E_FAIL;
}

HRESULT shared_appliance_unpublish_resource(const wchar_t *resource_id)
{
    const AsbSharedResourceInfo *resource = shared_resources_find(resource_id);
    wchar_t share_w[64];
    char share[128], command[256], response[256];
    if (!resource) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (!g_appliance.status.ready) return S_FALSE;
    shared_resources_share_name(resource->id, share_w, _countof(share_w));
    WideCharToMultiByte(CP_UTF8, 0, share_w, -1, share, sizeof(share), NULL, NULL);
    sprintf_s(command, sizeof(command), "appliance_remove:%s", share);
    return appliance_send(command, response, sizeof(response), 30000)
        ? S_OK : E_FAIL;
}

HRESULT shared_appliance_open_terminal(void)
{
    wchar_t command[4096], program_data[MAX_PATH], private_key[MAX_PATH];
    if (!g_appliance.status.ready) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    if (g_appliance.status.backend == ASB_APPLIANCE_BACKEND_UBUNTU) {
        if (!GetEnvironmentVariableW(L"ProgramData", program_data, _countof(program_data)))
            wcscpy_s(program_data, _countof(program_data), L"C:\\ProgramData");
        swprintf_s(private_key, _countof(private_key),
                   L"%s\\AppSandbox\\ssh\\id_appsandbox", program_data);
        swprintf_s(command, _countof(command),
                   L"wt.exe -w 0 new-tab ssh -i \"%s\" %s@%S",
                   private_key, g_appliance.status.admin_user,
                   shared_appliance_server_ip());
    } else {
        if (!g_appliance.status.management_cert_thumbprint[0])
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        swprintf_s(command, _countof(command),
          L"wt.exe -w 0 new-tab powershell.exe -NoExit -Command \""
          L"$ip='%S';$expected='%s';$tcp=[Net.Sockets.TcpClient]::new();"
          L"$tcp.Connect($ip,5986);$tls=[Net.Security.SslStream]::new($tcp.GetStream(),$false,{$true});"
          L"$tls.AuthenticateAsClient('AppSandbox.SharedAppliance');"
          L"$actual=$tls.RemoteCertificate.GetCertHashString();$tls.Dispose();$tcp.Dispose();"
          L"if($actual -ne $expected){throw 'Shared appliance management certificate mismatch'};"
          L"$o=New-PSSessionOption -SkipCACheck -SkipCNCheck -SkipRevocationCheck;"
          L"Enter-PSSession -ComputerName $ip -UseSSL -SessionOption $o -Credential (Get-Credential '%s')\"",
          shared_appliance_server_ip(), g_appliance.status.management_cert_thumbprint,
          g_appliance.status.admin_user);
    }
    return launch_process(command);
}

static DWORD WINAPI restart_worker(LPVOID unused)
{
    static const DWORD delays[] = { 2000, 5000, 15000 };
    int attempt;
    (void)unused;
    for (attempt = 0; attempt < 3; ++attempt) {
        Sleep(delays[attempt]);
        g_appliance.restart_attempts = attempt + 1;
        if (SUCCEEDED(shared_appliance_start(TRUE, 120000))) return 0;
    }
    EnterCriticalSection(&g_appliance.cs);
    g_appliance.status.state = ASB_APPLIANCE_STATE_FAILED;
    wcscpy_s(g_appliance.status.last_error, _countof(g_appliance.status.last_error),
        L"The shared appliance exited unexpectedly and failed three recovery attempts.");
    LeaveCriticalSection(&g_appliance.cs);
    return 1;
}

BOOL shared_appliance_handle_hcs_state(VmInstance *instance, DWORD event)
{
    HANDLE thread;
    if (instance != &g_appliance.runtime) return FALSE;
    if (event == 0x00000001 || event == 0x00000002 || event == 0x00000100) {
        vm_agent_stop(instance);
        hcs_stop_monitor(instance);
        instance->running = FALSE;
        hcs_unregister_vm_callback(instance);
        if (instance->handle) {
            hcs_close_handle_sync(instance->handle);
            instance->handle = NULL;
        }
        cleanup_runtime_network();
        EnterCriticalSection(&g_appliance.cs);
        g_appliance.status.ready = FALSE;
        g_appliance.status.busy = FALSE;
        g_appliance.status.state = g_appliance.status.configured ?
            ASB_APPLIANCE_STATE_STOPPED : ASB_APPLIANCE_STATE_PROVISIONING;
        LeaveCriticalSection(&g_appliance.cs);
        if (!g_appliance.stopping && g_appliance.status.active_clients > 0) {
            thread = CreateThread(NULL, 0, restart_worker, NULL, 0, NULL);
            if (thread) CloseHandle(thread);
        }
    }
    return TRUE;
}

BOOL shared_appliance_owns_instance(const VmInstance *instance)
{
    return instance == &g_appliance.runtime;
}

VmInstance *shared_appliance_instance_by_id(UINT64 id)
{
    return id == APPLIANCE_UNIQUE_ID ? &g_appliance.runtime : NULL;
}

/* Live instance for UI console viewers; NULL before initialization. */
VmInstance *shared_appliance_runtime(void)
{
    return g_appliance.initialized ? &g_appliance.runtime : NULL;
}

BOOL shared_appliance_get_smb_credentials(wchar_t *user, size_t user_chars,
                                          wchar_t *password, size_t password_chars)
{
    wchar_t admin[256];
    HRESULT hr;
    if (!user || !password) return FALSE;
    hr = load_credentials(admin, _countof(admin), password, password_chars);
    SecureZeroMemory(admin, sizeof(admin));
    if (FAILED(hr)) return FALSE;
    wcscpy_s(user, user_chars, APPLIANCE_SERVICE_USER);
    return TRUE;
}

const char *shared_appliance_server_ip(void)
{
    static char ip[32];
    sprintf_s(ip, sizeof(ip), "%s.2", hcn_share_subnet_base());
    return ip;
}
