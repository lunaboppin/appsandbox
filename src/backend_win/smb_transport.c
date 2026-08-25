#include "smb_transport.h"
#include "ui.h"
#include <windows.h>
#include <wincrypt.h>
#include <sddl.h>
#include <aclapi.h>
#include <lm.h>
#include <ntsecapi.h>
#include <bcrypt.h>
#include <stdio.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "netapi32.lib")

#define ASB_SMB_CRED_MAGIC 0x31424D53u /* SMB1 */
#define ASB_FIREWALL_RULE L"AppSandbox Shared Resources"

typedef struct {
    DWORD magic;
    DWORD blob_size;
} SmbCredentialHeader;

static CRITICAL_SECTION g_smb_cs;
static INIT_ONCE g_smb_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_smb_cs(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_smb_cs);
    return TRUE;
}

static void credential_path(wchar_t *out, size_t chars)
{
    wchar_t pd[MAX_PATH];
    if (!GetEnvironmentVariableW(L"ProgramData", pd, _countof(pd)))
        wcscpy_s(pd, _countof(pd), L"C:\\ProgramData");
    swprintf_s(out, chars, L"%s\\AppSandbox\\smb-credentials.bin", pd);
}

static HRESULT restrict_credential_file(const wchar_t *path)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &sd, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    if (!SetFileSecurityW(path, DACL_SECURITY_INFORMATION, sd)) {
        DWORD err = GetLastError(); LocalFree(sd); return HRESULT_FROM_WIN32(err);
    }
    LocalFree(sd);
    return S_OK;
}

static HRESULT generate_password(wchar_t *password, size_t chars)
{
    static const wchar_t alphabet[] =
        L"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#%_-";
    BYTE random_bytes[48];
    size_t i, wanted = chars > 33 ? 32 : chars - 1;
    if (chars < 17) return E_INVALIDARG;
    if (BCryptGenRandom(NULL, random_bytes, (ULONG)sizeof(random_bytes),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return E_FAIL;
    password[0] = L'A'; password[1] = L'a'; password[2] = L'2'; password[3] = L'!';
    for (i = 4; i < wanted; i++)
        password[i] = alphabet[random_bytes[i] % (_countof(alphabet) - 1)];
    password[wanted] = L'\0';
    SecureZeroMemory(random_bytes, sizeof(random_bytes));
    return S_OK;
}

static HRESULT write_protected_password(const wchar_t *password)
{
    wchar_t path[MAX_PATH];
    DATA_BLOB plain, protected_blob;
    SmbCredentialHeader header;
    HANDLE file;
    DWORD written;
    HRESULT hr = S_OK;

    ZeroMemory(&protected_blob, sizeof(protected_blob));
    plain.pbData = (BYTE *)password;
    plain.cbData = (DWORD)((wcslen(password) + 1) * sizeof(wchar_t));
    if (!CryptProtectData(&plain, L"AppSandbox SMB credential", NULL, NULL, NULL,
                          CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN,
                          &protected_blob))
        return HRESULT_FROM_WIN32(GetLastError());
    credential_path(path, _countof(path));
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        LocalFree(protected_blob.pbData); return hr;
    }
    header.magic = ASB_SMB_CRED_MAGIC;
    header.blob_size = protected_blob.cbData;
    if (!WriteFile(file, &header, sizeof(header), &written, NULL) ||
        written != sizeof(header) ||
        !WriteFile(file, protected_blob.pbData, protected_blob.cbData, &written, NULL) ||
        written != protected_blob.cbData || !FlushFileBuffers(file))
        hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(file);
    LocalFree(protected_blob.pbData);
    if (SUCCEEDED(hr)) hr = restrict_credential_file(path);
    return hr;
}

static HRESULT read_protected_password(wchar_t *password, size_t chars)
{
    wchar_t path[MAX_PATH];
    SmbCredentialHeader header;
    BYTE *encrypted = NULL;
    DATA_BLOB protected_blob, plain;
    HANDLE file;
    DWORD got;
    HRESULT hr = S_OK;

    credential_path(path, _countof(path));
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    if (!ReadFile(file, &header, sizeof(header), &got, NULL) || got != sizeof(header) ||
        header.magic != ASB_SMB_CRED_MAGIC || header.blob_size == 0 ||
        header.blob_size > 65536) {
        CloseHandle(file); return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    encrypted = (BYTE *)HeapAlloc(GetProcessHeap(), 0, header.blob_size);
    if (!encrypted) { CloseHandle(file); return E_OUTOFMEMORY; }
    if (!ReadFile(file, encrypted, header.blob_size, &got, NULL) ||
        got != header.blob_size) {
        hr = HRESULT_FROM_WIN32(GetLastError()); goto done;
    }
    protected_blob.pbData = encrypted; protected_blob.cbData = header.blob_size;
    ZeroMemory(&plain, sizeof(plain));
    if (!CryptUnprotectData(&protected_blob, NULL, NULL, NULL, NULL,
                            CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        hr = HRESULT_FROM_WIN32(GetLastError()); goto done;
    }
    if (plain.cbData < sizeof(wchar_t) ||
        ((wchar_t *)plain.pbData)[plain.cbData / sizeof(wchar_t) - 1] != L'\0')
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    else if (wcscpy_s(password, chars, (wchar_t *)plain.pbData) != 0)
        hr = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
done:
    SecureZeroMemory(encrypted, header.blob_size);
    HeapFree(GetProcessHeap(), 0, encrypted);
    CloseHandle(file);
    return hr;
}

static HRESULT add_deny_logon_rights(void)
{
    wchar_t account[128]; DWORD domain_chars = 0;
    SID_NAME_USE use; PSID sid = NULL; wchar_t *domain = NULL;
    LSA_OBJECT_ATTRIBUTES oa; LSA_HANDLE policy = NULL; NTSTATUS status;
    LSA_UNICODE_STRING rights[4];
    static const wchar_t *names[] = {
        L"SeDenyInteractiveLogonRight", L"SeDenyRemoteInteractiveLogonRight",
        L"SeDenyBatchLogonRight", L"SeDenyServiceLogonRight"
    };
    DWORD sid_size = 0; int i;
    wcscpy_s(account, _countof(account), ASB_SMB_ACCOUNT);
    LookupAccountNameW(NULL, account, NULL, &sid_size, NULL, &domain_chars, &use);
    if (!sid_size) return HRESULT_FROM_WIN32(GetLastError());
    sid = (PSID)LocalAlloc(LPTR, sid_size);
    domain = (wchar_t *)LocalAlloc(LPTR, domain_chars * sizeof(wchar_t));
    if (!sid || !domain) { if (sid) LocalFree(sid); if (domain) LocalFree(domain); return E_OUTOFMEMORY; }
    if (!LookupAccountNameW(NULL, account, sid, &sid_size, domain, &domain_chars, &use)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError()); LocalFree(sid); LocalFree(domain); return hr;
    }
    ZeroMemory(&oa, sizeof(oa)); oa.Length = sizeof(oa);
    status = LsaOpenPolicy(NULL, &oa, POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT, &policy);
    if (status != 0) { LocalFree(sid); LocalFree(domain); return HRESULT_FROM_WIN32(LsaNtStatusToWinError(status)); }
    for (i = 0; i < (int)_countof(names); i++) {
        rights[i].Buffer = (PWSTR)names[i];
        rights[i].Length = (USHORT)(wcslen(names[i]) * sizeof(wchar_t));
        rights[i].MaximumLength = rights[i].Length + sizeof(wchar_t);
    }
    status = LsaAddAccountRights(policy, sid, rights, _countof(rights));
    LsaClose(policy); LocalFree(sid); LocalFree(domain);
    return status == 0 ? S_OK : HRESULT_FROM_WIN32(LsaNtStatusToWinError(status));
}

static HRESULT ensure_account(const wchar_t *password)
{
    USER_INFO_1 ui; USER_INFO_1003 password_info; DWORD parm = 0;
    NET_API_STATUS st;
    ZeroMemory(&ui, sizeof(ui));
    ui.usri1_name = (LPWSTR)ASB_SMB_ACCOUNT;
    ui.usri1_password = (LPWSTR)password;
    ui.usri1_priv = USER_PRIV_USER;
    ui.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT |
                     UF_DONT_EXPIRE_PASSWD | UF_PASSWD_CANT_CHANGE;
    ui.usri1_comment = L"AppSandbox isolated shared-resource account";
    st = NetUserAdd(NULL, 1, (LPBYTE)&ui, &parm);
    if (st == NERR_UserExists) {
        USER_INFO_1 *existing = NULL;
        st = NetUserGetInfo(NULL, ASB_SMB_ACCOUNT, 1, (LPBYTE *)&existing);
        if (st != NERR_Success || !existing || !existing->usri1_comment ||
            wcscmp(existing->usri1_comment,
                   L"AppSandbox isolated shared-resource account") != 0) {
            if (existing) NetApiBufferFree(existing);
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
        NetApiBufferFree(existing);
        password_info.usri1003_password = (LPWSTR)password;
        st = NetUserSetInfo(NULL, ASB_SMB_ACCOUNT, 1003,
                            (LPBYTE)&password_info, &parm);
    }
    if (st != NERR_Success) return HRESULT_FROM_WIN32(st);
    return add_deny_logon_rights();
}

HRESULT smb_transport_get_credentials(wchar_t *user, size_t user_chars,
                                      wchar_t *password, size_t password_chars)
{
    HRESULT hr;
    InitOnceExecuteOnce(&g_smb_once, init_smb_cs, NULL, NULL);
    EnterCriticalSection(&g_smb_cs);
    hr = read_protected_password(password, password_chars);
    if (FAILED(hr)) {
        wchar_t path[MAX_PATH];
        credential_path(path, _countof(path));
        DeleteFileW(path);
        hr = generate_password(password, password_chars);
        if (SUCCEEDED(hr)) hr = write_protected_password(password);
    }
    if (SUCCEEDED(hr)) {
        wchar_t host[256]; DWORD host_chars = _countof(host);
        if (!GetComputerNameW(host, &host_chars)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
        } else {
            swprintf_s(user, user_chars, L"%s\\%s", host, ASB_SMB_ACCOUNT);
        }
    }
    if (SUCCEEDED(hr)) {
        hr = ensure_account(password);
    }
    if (FAILED(hr) && password && password_chars)
        SecureZeroMemory(password, password_chars * sizeof(wchar_t));
    LeaveCriticalSection(&g_smb_cs);
    return hr;
}

static HRESULT set_account_acl(const wchar_t *path, BOOL read_only, BOOL remove)
{
    PACL old_dacl = NULL, new_dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    EXPLICIT_ACCESSW ea;
    DWORD err = GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, NULL, NULL, &old_dacl, NULL, &sd);
    if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessMode = remove ? REVOKE_ACCESS : GRANT_ACCESS;
    ea.grfAccessPermissions = read_only
        ? FILE_GENERIC_READ | FILE_GENERIC_EXECUTE
        : FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = (LPWSTR)ASB_SMB_ACCOUNT;
    err = SetEntriesInAclW(1, &ea, old_dacl, &new_dacl);
    if (err == ERROR_SUCCESS)
        err = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, NULL, NULL, new_dacl, NULL);
    if (new_dacl) LocalFree(new_dacl);
    if (sd) LocalFree(sd);
    return HRESULT_FROM_WIN32(err);
}

static void hidden_share_name(const wchar_t *base, wchar_t *out, size_t chars)
{
    swprintf_s(out, chars, L"%s$", base);
}

static HRESULT ensure_share(const HcsSharedResource *resource)
{
    wchar_t share[80];
    EXPLICIT_ACCESSW ea;
    PACL dacl = NULL;
    SECURITY_DESCRIPTOR sd;
    SHARE_INFO_502 si;
    DWORD parm = 0, err;
    hidden_share_name(resource->share_name, share, _countof(share));
    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = resource->read_only ? GENERIC_READ : GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = (LPWSTR)ASB_SMB_ACCOUNT;
    err = SetEntriesInAclW(1, &ea, NULL, &dacl);
    if (err != ERROR_SUCCESS) return HRESULT_FROM_WIN32(err);
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, dacl, FALSE);
    ZeroMemory(&si, sizeof(si));
    si.shi502_netname = share;
    si.shi502_type = STYPE_DISKTREE | STYPE_SPECIAL | STYPE_TEMPORARY;
    si.shi502_remark = L"AppSandbox isolated shared resource";
    si.shi502_permissions = ACCESS_ALL;
    si.shi502_max_uses = (DWORD)-1;
    si.shi502_path = (LPWSTR)resource->host_path;
    si.shi502_security_descriptor = &sd;
    err = NetShareAdd(NULL, 502, (LPBYTE)&si, &parm);
    if (err == NERR_DuplicateShare || err == ERROR_ALREADY_EXISTS)
        err = NetShareSetInfo(NULL, share, 502, (LPBYTE)&si, &parm);
    LocalFree(dacl);
    return err == NERR_Success ? S_OK : HRESULT_FROM_WIN32(err);
}

static DWORD run_powershell(const wchar_t *command)
{
    STARTUPINFOW si; PROCESS_INFORMATION pi; wchar_t cmd[4096]; DWORD ec = ERROR_GEN_FAILURE;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si); ZeroMemory(&pi, sizeof(pi));
    swprintf_s(cmd, _countof(cmd),
        L"powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"%s\"",
        command);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return GetLastError();
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &ec);
    else { TerminateProcess(pi.hProcess, ERROR_TIMEOUT); ec = ERROR_TIMEOUT; }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return ec;
}

static HRESULT ensure_firewall(const char *subnet_base)
{
    wchar_t command[2048]; DWORD ec;
    swprintf_s(command, _countof(command),
        L"$ErrorActionPreference='Stop'; $n='%s'; "
        L"Get-NetFirewallRule -DisplayName $n -ErrorAction SilentlyContinue | Remove-NetFirewallRule; "
        L"$a=$null; for($i=0;$i -lt 25 -and -not $a;$i++){"
        L"$a=(Get-NetIPAddress -AddressFamily IPv4 -IPAddress '%S.1' -ErrorAction SilentlyContinue).InterfaceAlias;"
        L"if(-not $a){Start-Sleep -Milliseconds 200}}; if(-not $a){throw 'shared adapter unavailable'}; "
        L"New-NetFirewallRule -DisplayName $n -Group 'AppSandbox' -Direction In -Action Allow "
        L"-Protocol TCP -LocalPort 445 -RemoteAddress '%S.0/24' -InterfaceAlias $a -Profile Any | Out-Null",
        ASB_FIREWALL_RULE, subnet_base, subnet_base);
    ec = run_powershell(command);
    return ec == 0 ? S_OK : HRESULT_FROM_WIN32(ec);
}

HRESULT smb_transport_prepare(HcsSharedResource *resources, int count,
                              const char *subnet_base)
{
    wchar_t user[128], password[128];
    HRESULT hr, first_failure = S_OK;
    int i;
    if (!resources || count <= 0 || !subnet_base) return E_INVALIDARG;
    hr = smb_transport_get_credentials(user, _countof(user), password, _countof(password));
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&g_smb_cs);
    hr = ensure_firewall(subnet_base);
    if (FAILED(hr)) {
        for (i = 0; i < count; i++) {
            wcscpy_s(resources[i].mapping_result,
                     _countof(resources[i].mapping_result), L"unavailable");
            swprintf_s(resources[i].failure, _countof(resources[i].failure),
                       L"firewall_failed:0x%08X", hr);
        }
        SecureZeroMemory(password, sizeof(password));
        SecureZeroMemory(user, sizeof(user));
        LeaveCriticalSection(&g_smb_cs);
        return hr;
    }
    for (i = 0; i < count; i++) {
        DWORD attrs = GetFileAttributesW(resources[i].host_path);
        if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            hr = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        else {
            hr = set_account_acl(resources[i].host_path, resources[i].read_only, TRUE);
            if (SUCCEEDED(hr)) hr = set_account_acl(resources[i].host_path,
                                                     resources[i].read_only, FALSE);
            if (SUCCEEDED(hr)) {
                hr = shared_resources_set_smb_acl_created(resources[i].id, TRUE);
                if (SUCCEEDED(hr)) resources[i].smb_acl_created = TRUE;
                else set_account_acl(resources[i].host_path, resources[i].read_only, TRUE);
            }
            if (SUCCEEDED(hr)) hr = ensure_share(&resources[i]);
        }
        if (FAILED(hr)) {
            if (SUCCEEDED(first_failure)) first_failure = hr;
            wcscpy_s(resources[i].mapping_result,
                     _countof(resources[i].mapping_result), L"unavailable");
            swprintf_s(resources[i].failure, _countof(resources[i].failure),
                       L"host_smb_failed:0x%08X", hr);
            ui_log(L"Shared resource %c: host SMB provisioning failed (0x%08X).",
                   resources[i].drive_letter, hr);
        } else {
            wcscpy_s(resources[i].mapping_result,
                     _countof(resources[i].mapping_result), L"pending");
            resources[i].failure[0] = L'\0';
            ui_log(L"SMB share: %c: %s$ -> %s%s", resources[i].drive_letter,
                   resources[i].share_name, resources[i].host_path,
                   resources[i].read_only ? L" (read-only)" : L"");
        }
    }
    SecureZeroMemory(password, sizeof(password));
    SecureZeroMemory(user, sizeof(user));
    LeaveCriticalSection(&g_smb_cs);
    return first_failure;
}

void smb_transport_remove_resource(const wchar_t *id, const wchar_t *host_path,
                                   BOOL remove_tracked_acl)
{
    wchar_t base[64] = L"asb_", share[80]; int si = 4, gi = 0;
    while (id && id[gi] && si < 20) { if (id[gi] != L'-') base[si++] = towlower(id[gi]); gi++; }
    base[si] = L'\0'; hidden_share_name(base, share, _countof(share));
    NetShareDel(NULL, share, 0);
    if (remove_tracked_acl && host_path && host_path[0])
        set_account_acl(host_path, FALSE, TRUE);
}

void smb_transport_cleanup_stale(void)
{
    SHARE_INFO_1 *shares = NULL; DWORD read = 0, total = 0, resume = 0, i;
    wchar_t command[512];
    if (NetShareEnum(NULL, 1, (LPBYTE *)&shares, MAX_PREFERRED_LENGTH,
                     &read, &total, &resume) == NERR_Success) {
        for (i = 0; i < read; i++)
            if (shares[i].shi1_netname &&
                wcslen(shares[i].shi1_netname) == 21 &&
                _wcsnicmp(shares[i].shi1_netname, L"asb_", 4) == 0 &&
                shares[i].shi1_netname[20] == L'$' &&
                shares[i].shi1_remark &&
                wcscmp(shares[i].shi1_remark,
                       L"AppSandbox isolated shared resource") == 0)
                NetShareDel(NULL, shares[i].shi1_netname, 0);
        NetApiBufferFree(shares);
    }
    swprintf_s(command, _countof(command),
        L"Get-NetFirewallRule -DisplayName '%s' -ErrorAction SilentlyContinue | Remove-NetFirewallRule",
        ASB_FIREWALL_RULE);
    run_powershell(command);
}

void smb_transport_cleanup_unused(void)
{
    wchar_t path[MAX_PATH];
    USER_INFO_1 *existing = NULL;
    smb_transport_cleanup_stale();
    if (NetUserGetInfo(NULL, ASB_SMB_ACCOUNT, 1, (LPBYTE *)&existing) == NERR_Success &&
        existing && existing->usri1_comment &&
        wcscmp(existing->usri1_comment,
               L"AppSandbox isolated shared-resource account") == 0)
        NetUserDel(NULL, ASB_SMB_ACCOUNT);
    if (existing) NetApiBufferFree(existing);
    credential_path(path, _countof(path));
    DeleteFileW(path);
}
