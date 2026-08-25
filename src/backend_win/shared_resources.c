#define _CRT_SECURE_NO_WARNINGS
#include "shared_resources.h"
#include "smb_transport.h"
#include "asb_core.h"
#include <aclapi.h>
#include <sddl.h>
#include <shlwapi.h>
#include <io.h>
#include <stdio.h>
#include <wctype.h>
#include <wchar.h>

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

static BOOL path_contains(const wchar_t *parent, const wchar_t *child)
{
    size_t n = wcslen(parent);
    if (_wcsnicmp(parent, child, n) != 0) return FALSE;
    return child[n] == L'\0' || child[n] == L'\\';
}

static HRESULT normalize_folder(const wchar_t *path, wchar_t *out, size_t chars)
{
    wchar_t volume[MAX_PATH], fs[32];
    DWORD attrs, serial, max_comp, flags;
    UINT drive;
    size_t len;
    if (!path || !path[0] || PathIsUNCW(path))
        return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    if (!GetFullPathNameW(path, (DWORD)chars, out, NULL))
        return HRESULT_FROM_WIN32(GetLastError());
    len = wcslen(out);
    while (len > 3 && out[len - 1] == L'\\') out[--len] = L'\0';
    attrs = GetFileAttributesW(out);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (!GetVolumePathNameW(out, volume, MAX_PATH))
        return HRESULT_FROM_WIN32(GetLastError());
    drive = GetDriveTypeW(volume);
    if (drive != DRIVE_FIXED)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (!GetVolumeInformationW(volume, NULL, 0, &serial, &max_comp, &flags, fs, 32))
        return HRESULT_FROM_WIN32(GetLastError());
    if (_wcsicmp(fs, L"NTFS") != 0 && _wcsicmp(fs, L"ReFS") != 0)
        return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    return S_OK;
}

static BOOL vm_group_ace_present(const wchar_t *path)
{
    PACL dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    PSID sid = NULL;
    DWORD i;
    BOOL found = FALSE;
    if (!ConvertStringSidToSidW(L"S-1-5-83-0", &sid)) return FALSE;
    if (GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, NULL, NULL, &dacl, NULL, &sd) == ERROR_SUCCESS && dacl) {
        for (i = 0; i < dacl->AceCount; i++) {
            void *ace;
            if (GetAce(dacl, i, &ace)) {
                ACE_HEADER *hdr = (ACE_HEADER *)ace;
                PSID ace_sid = NULL;
                if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE)
                    ace_sid = (PSID)&((ACCESS_ALLOWED_ACE *)ace)->SidStart;
                if (ace_sid && EqualSid(sid, ace_sid)) { found = TRUE; break; }
            }
        }
    }
    if (sd) LocalFree(sd);
    LocalFree(sid);
    return found;
}

static HRESULT set_vm_group_acl(const wchar_t *path, BOOL read_only, BOOL remove)
{
    PACL old_dacl = NULL, new_dacl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    PSID sid = NULL;
    EXPLICIT_ACCESSW ea;
    DWORD err;
    if (!ConvertStringSidToSidW(L"S-1-5-83-0", &sid))
        return HRESULT_FROM_WIN32(GetLastError());
    err = GetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, NULL, NULL, &old_dacl, NULL, &sd);
    if (err != ERROR_SUCCESS) { LocalFree(sid); return HRESULT_FROM_WIN32(err); }
    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = remove ? 0 : (read_only
        ? (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE)
        : (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE));
    ea.grfAccessMode = remove ? REVOKE_ACCESS : SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)sid;
    err = SetEntriesInAclW(1, &ea, old_dacl, &new_dacl);
    if (err == ERROR_SUCCESS)
        err = SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION, NULL, NULL, new_dacl, NULL);
    if (new_dacl) LocalFree(new_dacl);
    if (sd) LocalFree(sd);
    LocalFree(sid);
    return HRESULT_FROM_WIN32(err);
}

static HRESULT save_locked(void)
{
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    FILE *f = NULL;
    int i;
    config_path(path, MAX_PATH);
    swprintf_s(temp, MAX_PATH, L"%s.tmp", path);
    if (_wfopen_s(&f, temp, L"w,ccs=UTF-8") != 0 || !f)
        return HRESULT_FROM_WIN32(GetLastError());
    fwprintf(f, L"Version=1\n");
    for (i = 0; i < g_resource_count; i++) {
        AsbSharedResourceInfo *r = &g_resources[i];
        fwprintf(f, L"[Resource]\nId=%s\nName=%s\nPath=%s\nDriveLetter=%c\nEnabled=%d\nReadOnly=%d\nAclCreated=%d\nSmbAclCreated=%d\n",
                 r->id, r->name, r->host_path, r->drive_letter,
                 r->enabled ? 1 : 0, r->read_only ? 1 : 0, r->acl_created ? 1 : 0,
                 r->smb_acl_created ? 1 : 0);
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
    if (!g_resource_cs_ready) { InitializeCriticalSection(&g_resource_cs); g_resource_cs_ready = TRUE; }
    EnterCriticalSection(&g_resource_cs);
    g_resource_count = 0;
    config_path(path, MAX_PATH);
    if (_wfopen_s(&f, path, L"r,ccs=UTF-8") == 0 && f) {
        while (fgetws(line, _countof(line), f)) {
            wchar_t *nl = wcspbrk(line, L"\r\n"); if (nl) *nl = L'\0';
            if (wcscmp(line, L"[Resource]") == 0) {
                if (g_resource_count >= ASB_MAX_SHARED_RESOURCES) { cur = NULL; continue; }
                cur = &g_resources[g_resource_count++]; ZeroMemory(cur, sizeof(*cur));
            } else if (cur && wcsncmp(line,L"Id=",3)==0) wcscpy_s(cur->id,ASB_SHARED_ID_CHARS,line+3);
            else if (cur && wcsncmp(line,L"Name=",5)==0) wcscpy_s(cur->name,64,line+5);
            else if (cur && wcsncmp(line,L"Path=",5)==0) wcscpy_s(cur->host_path,MAX_PATH,line+5);
            else if (cur && wcsncmp(line,L"DriveLetter=",12)==0) cur->drive_letter=towupper(line[12]);
            else if (cur && wcsncmp(line,L"Enabled=",8)==0) cur->enabled=_wtoi(line+8)!=0;
            else if (cur && wcsncmp(line,L"ReadOnly=",9)==0) cur->read_only=_wtoi(line+9)!=0;
            else if (cur && wcsncmp(line,L"AclCreated=",11)==0) cur->acl_created=_wtoi(line+11)!=0;
            else if (cur && wcsncmp(line,L"SmbAclCreated=",14)==0) cur->smb_acl_created=_wtoi(line+14)!=0;
        }
        fclose(f);
    }
    LeaveCriticalSection(&g_resource_cs);
}

int shared_resources_count(void) { return g_resource_count; }
const AsbSharedResourceInfo *shared_resources_get(int index)
{ return index >= 0 && index < g_resource_count ? &g_resources[index] : NULL; }

static int find_id(const wchar_t *id)
{ int i; for (i=0;i<g_resource_count;i++) if (_wcsicmp(g_resources[i].id,id)==0) return i; return -1; }

static HRESULT validate_info(const AsbSharedResourceInfo *in, int ignore,
                             AsbSharedResourceInfo *normalized)
{
    HRESULT hr;
    int i, v;
    wchar_t vmroot[MAX_PATH];
    if (!in || !in->name[0] || !in->host_path[0] || in->drive_letter < L'D' || in->drive_letter > L'Z')
        return E_INVALIDARG;
    *normalized = *in;
    normalized->drive_letter = towupper(normalized->drive_letter);
    hr = normalize_folder(in->host_path, normalized->host_path, MAX_PATH);
    if (FAILED(hr)) return hr;
    for (i=0;i<g_resource_count;i++) if(i!=ignore) {
        AsbSharedResourceInfo *r=&g_resources[i];
        if (_wcsicmp(r->name,normalized->name)==0 || r->drive_letter==normalized->drive_letter)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        if (path_contains(r->host_path,normalized->host_path) || path_contains(normalized->host_path,r->host_path))
            return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    }
    for (v=0; v<asb_vm_count(); v++) {
        VmInstance *vm=asb_vm_instance(asb_vm_get(v));
        if (!vm) continue;
        if (vm->storage_root[0]) wcscpy_s(vmroot,MAX_PATH,vm->storage_root);
        else { wcscpy_s(vmroot,MAX_PATH,vm->vhdx_path); wchar_t *s=wcsrchr(vmroot,L'\\'); if(s)*s=L'\0'; }
        if (path_contains(vmroot,normalized->host_path) ||
            path_contains(normalized->host_path,vmroot))
            return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    }
    {
        wchar_t pd[MAX_PATH], templates[MAX_PATH];
        if (!GetEnvironmentVariableW(L"ProgramData",pd,MAX_PATH))wcscpy_s(pd,MAX_PATH,L"C:\\ProgramData");
        swprintf_s(templates,MAX_PATH,L"%s\\AppSandbox\\templates",pd);
        if(path_contains(templates,normalized->host_path)||path_contains(normalized->host_path,templates))
            return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
    }
    return S_OK;
}

HRESULT shared_resources_create(const AsbSharedResourceInfo *info, BOOL confirm,
                                wchar_t *created_id, size_t created_id_chars)
{
    AsbSharedResourceInfo n; GUID guid; HRESULT hr;
    EnterCriticalSection(&g_resource_cs);
    if (g_resource_count >= ASB_MAX_SHARED_RESOURCES) { LeaveCriticalSection(&g_resource_cs); return E_OUTOFMEMORY; }
    hr=validate_info(info,-1,&n); if(FAILED(hr)){LeaveCriticalSection(&g_resource_cs);return hr;}
    if (!vm_group_ace_present(n.host_path)) {
        if (!confirm) { LeaveCriticalSection(&g_resource_cs); return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED); }
        hr=set_vm_group_acl(n.host_path,n.read_only,FALSE); if(FAILED(hr)){LeaveCriticalSection(&g_resource_cs);return hr;}
        n.acl_created=TRUE;
    }
    {
        wchar_t guid_buf[ASB_SHARED_ID_CHARS];
        CoCreateGuid(&guid); StringFromGUID2(&guid,guid_buf,ASB_SHARED_ID_CHARS);
        wcsncpy_s(n.id, ASB_SHARED_ID_CHARS, guid_buf + 1, 36);
    }
    g_resources[g_resource_count++]=n;
    hr=save_locked();
    if(FAILED(hr)){g_resource_count--; if(n.acl_created)set_vm_group_acl(n.host_path,n.read_only,TRUE);}
    else if(created_id)wcscpy_s(created_id,created_id_chars,n.id);
    LeaveCriticalSection(&g_resource_cs); return hr;
}

HRESULT shared_resources_update(const wchar_t *id, const AsbSharedResourceInfo *info, BOOL confirm)
{
    AsbSharedResourceInfo n, old; HRESULT hr; int idx; BOOL changed;
    EnterCriticalSection(&g_resource_cs); idx=find_id(id);
    if(idx<0){LeaveCriticalSection(&g_resource_cs);return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);}
    old=g_resources[idx]; hr=validate_info(info,idx,&n); if(FAILED(hr)){LeaveCriticalSection(&g_resource_cs);return hr;}
    wcscpy_s(n.id,ASB_SHARED_ID_CHARS,old.id);
    changed=(_wcsicmp(old.host_path,n.host_path)!=0 || old.read_only!=n.read_only);
    n.acl_created=!changed ? old.acl_created : FALSE;
    n.smb_acl_created=!changed ? old.smb_acl_created : FALSE;
    if(changed && old.acl_created){
        if(!confirm){LeaveCriticalSection(&g_resource_cs);return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);}
        hr=set_vm_group_acl(n.host_path,n.read_only,FALSE);if(FAILED(hr)){LeaveCriticalSection(&g_resource_cs);return hr;}
        n.acl_created=TRUE;
        if(_wcsicmp(old.host_path,n.host_path)!=0)set_vm_group_acl(old.host_path,old.read_only,TRUE);
    } else if(!vm_group_ace_present(n.host_path)){
        if(!confirm){g_resources[idx]=old;LeaveCriticalSection(&g_resource_cs);return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);}
        hr=set_vm_group_acl(n.host_path,n.read_only,FALSE);if(FAILED(hr)){g_resources[idx]=old;LeaveCriticalSection(&g_resource_cs);return hr;}n.acl_created=TRUE;
    }
    g_resources[idx]=n; hr=save_locked();
    if(FAILED(hr)){
        g_resources[idx]=old;
        if(changed&&n.acl_created){
            if(_wcsicmp(old.host_path,n.host_path)==0)set_vm_group_acl(old.host_path,old.read_only,FALSE);
            else {set_vm_group_acl(n.host_path,n.read_only,TRUE);if(old.acl_created)set_vm_group_acl(old.host_path,old.read_only,FALSE);}
        }
    }
    if (SUCCEEDED(hr) && changed)
        smb_transport_remove_resource(old.id, old.host_path, old.smb_acl_created);
    LeaveCriticalSection(&g_resource_cs); return hr;
}

HRESULT shared_resources_remove(const wchar_t *id)
{
    int idx,i; HRESULT hr; AsbSharedResourceInfo old;
    EnterCriticalSection(&g_resource_cs);idx=find_id(id);
    if(idx<0){LeaveCriticalSection(&g_resource_cs);return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);}
    old=g_resources[idx];for(i=idx;i<g_resource_count-1;i++)g_resources[i]=g_resources[i+1];g_resource_count--;
    hr=save_locked();if(FAILED(hr)){for(i=g_resource_count;i>idx;i--)g_resources[i]=g_resources[i-1];g_resources[idx]=old;g_resource_count++;}
    else {
        smb_transport_remove_resource(old.id, old.host_path, old.smb_acl_created);
        if(old.acl_created)set_vm_group_acl(old.host_path,old.read_only,TRUE);
        if(g_resource_count==0)smb_transport_cleanup_unused();
    }
    LeaveCriticalSection(&g_resource_cs);return hr;
}

BOOL shared_resources_is_excluded(const wchar_t *list,const wchar_t *id)
{
    const wchar_t *p=list;size_t n=wcslen(id);if(!p)return FALSE;
    while(*p){while(*p==L',')p++;if(_wcsnicmp(p,id,n)==0&&(p[n]==L','||p[n]==L'\0'))return TRUE;p=wcschr(p,L',');if(!p)break;p++;}return FALSE;
}

HRESULT shared_resources_set_excluded(wchar_t *list,size_t chars,const wchar_t *id,BOOL excluded)
{
    wchar_t out[1024]=L"";wchar_t copy[1024],*ctx=NULL,*tok;BOOL found=FALSE;
    if(!list||!id||!id[0])return E_INVALIDARG;wcscpy_s(copy,1024,list);
    tok=wcstok_s(copy,L",",&ctx);while(tok){if(_wcsicmp(tok,id)==0)found=TRUE;else{if(out[0])wcscat_s(out,1024,L",");wcscat_s(out,1024,tok);}tok=wcstok_s(NULL,L",",&ctx);}
    if(excluded&&!found){if(out[0])wcscat_s(out,1024,L",");if(wcslen(out)+wcslen(id)+1>=chars)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);wcscat_s(out,1024,id);}
    wcscpy_s(list,chars,out);return S_OK;
}

HRESULT shared_resources_set_smb_acl_created(const wchar_t *id, BOOL created)
{
    int idx; HRESULT hr;
    EnterCriticalSection(&g_resource_cs);
    idx = find_id(id);
    if (idx < 0) { LeaveCriticalSection(&g_resource_cs); return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    if (g_resources[idx].smb_acl_created == created) {
        LeaveCriticalSection(&g_resource_cs); return S_OK;
    }
    g_resources[idx].smb_acl_created = created;
    hr = save_locked();
    if (FAILED(hr)) g_resources[idx].smb_acl_created = !created;
    LeaveCriticalSection(&g_resource_cs);
    return hr;
}

int shared_resources_build_attachments(const wchar_t *os_type,const wchar_t *exclusions,
                                       HcsSharedResource *out,int capacity)
{
    int i,n=0;if(!os_type||_wcsicmp(os_type,L"Windows")!=0)return 0;
    EnterCriticalSection(&g_resource_cs);for(i=0;i<g_resource_count&&n<capacity;i++){
        AsbSharedResourceInfo *r=&g_resources[i];if(!r->enabled||shared_resources_is_excluded(exclusions,r->id))continue;
        wcscpy_s(out[n].id,ASB_SHARED_ID_CHARS,r->id);wcscpy_s(out[n].host_path,MAX_PATH,r->host_path);
        {
            int si=4,gi=0;wcscpy_s(out[n].share_name,64,L"asb_");
            while(r->id[gi]&&si<20){if(r->id[gi]!=L'-')out[n].share_name[si++]=towlower(r->id[gi]);gi++;}
            out[n].share_name[si]=L'\0';
        }
        out[n].drive_letter=r->drive_letter;out[n].read_only=r->read_only;
        out[n].smb_acl_created=r->smb_acl_created;
        wcscpy_s(out[n].mapping_result,32,L"pending");out[n].failure[0]=L'\0';n++;
    }LeaveCriticalSection(&g_resource_cs);return n;
}
