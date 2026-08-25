#ifndef ASB_SHARED_RESOURCES_H
#define ASB_SHARED_RESOURCES_H

#include <windows.h>

#define ASB_MAX_SHARED_RESOURCES 16
#define ASB_SHARED_ID_CHARS 40

typedef struct {
    wchar_t id[ASB_SHARED_ID_CHARS];
    wchar_t name[64];
    wchar_t host_path[MAX_PATH];       /* legacy path: migration display only */
    wchar_t legacy_host_path[MAX_PATH];
    wchar_t drive_letter;
    wchar_t host_drive_letter;
    wchar_t storage_kind[16];         /* L"appliance" */
    BOOL enabled;
    BOOL read_only;
    BOOL retained_data;
} AsbSharedResourceInfo;

typedef struct {
    wchar_t id[ASB_SHARED_ID_CHARS];
    wchar_t share_name[64];
    wchar_t host_path[MAX_PATH];
    wchar_t drive_letter;
    wchar_t host_drive_letter;
    BOOL read_only;
    BOOL smb_acl_created;
    wchar_t mapping_result[32];
    wchar_t failure[128];
} HcsSharedResource;

void shared_resources_init(void);
int shared_resources_count(void);
const AsbSharedResourceInfo *shared_resources_get(int index);
const AsbSharedResourceInfo *shared_resources_find(const wchar_t *id);
void shared_resources_share_name(const wchar_t *id, wchar_t *out, size_t chars);
HRESULT shared_resources_create(const AsbSharedResourceInfo *info,
                                BOOL confirm_permissions,
                                wchar_t *created_id, size_t created_id_chars);
HRESULT shared_resources_update(const wchar_t *id,
                                const AsbSharedResourceInfo *info,
                                BOOL confirm_permissions);
HRESULT shared_resources_remove(const wchar_t *id);
int shared_resources_build_attachments(const wchar_t *os_type,
                                       const wchar_t *exclusions,
                                       HcsSharedResource *out, int capacity);
BOOL shared_resources_is_excluded(const wchar_t *exclusions, const wchar_t *id);
HRESULT shared_resources_set_excluded(wchar_t *exclusions, size_t exclusions_chars,
                                      const wchar_t *id, BOOL excluded);
HRESULT shared_resources_set_smb_acl_created(const wchar_t *id, BOOL created);

#endif
