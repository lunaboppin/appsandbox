#ifndef ASB_SMB_TRANSPORT_H
#define ASB_SMB_TRANSPORT_H

#include <windows.h>
#include "shared_resources.h"

#define ASB_SMB_ACCOUNT L"AppSandboxShare"

/* Lazily provisions the app-owned account, DPAPI credential, firewall rule,
   resource ACLs and hidden SMB shares. Per-resource failures are recorded in
   the supplied attachment array and do not fail VM startup. */
HRESULT smb_transport_prepare(HcsSharedResource *resources, int count,
                              const char *subnet_base);

/* Decrypt into caller-owned memory immediately before sending a mapping
   command. The caller must SecureZeroMemory(password) after use. */
HRESULT smb_transport_get_credentials(wchar_t *user, size_t user_chars,
                                      wchar_t *password, size_t password_chars);

/* Removes only AppSandbox-created share and account ACEs. Never deletes data. */
void smb_transport_remove_resource(const wchar_t *id, const wchar_t *host_path,
                                   BOOL remove_tracked_acl);

/* Removes app-owned firewall/share infrastructure when it is no longer used. */
void smb_transport_cleanup_stale(void);
void smb_transport_cleanup_unused(void);

#endif
