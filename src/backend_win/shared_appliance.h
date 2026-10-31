#ifndef ASB_SHARED_APPLIANCE_H
#define ASB_SHARED_APPLIANCE_H

#include <windows.h>
#include "hcs_vm.h"

#define ASB_APPLIANCE_BACKEND_NONE        0
#define ASB_APPLIANCE_BACKEND_UBUNTU      1
#define ASB_APPLIANCE_BACKEND_SERVER_CORE 2

#define ASB_APPLIANCE_STATE_UNCONFIGURED 0
#define ASB_APPLIANCE_STATE_PROVISIONING 1
#define ASB_APPLIANCE_STATE_STOPPED      2
#define ASB_APPLIANCE_STATE_STARTING     3
#define ASB_APPLIANCE_STATE_READY        4
#define ASB_APPLIANCE_STATE_UPDATING     5
#define ASB_APPLIANCE_STATE_FAILED       6
#define ASB_APPLIANCE_STATE_STOPPING     7

typedef struct {
    int backend;
    wchar_t storage_parent[MAX_PATH];
    DWORD data_size_gb;
    DWORD ram_mb;
    DWORD cpu_cores;
    wchar_t admin_user[128];
    wchar_t admin_password[256];
    wchar_t windows_iso_path[MAX_PATH];
    wchar_t windows_image_name[256];
    wchar_t product_key[64];
    BOOL enable_domain;   /* Server Core only: promote to the AppSandbox domain controller */
} SharedApplianceConfig;

typedef struct {
    int backend;
    int state;
    int progress;
    BOOL configured;
    BOOL ready;
    BOOL busy;
    BOOL update_available;
    BOOL domain_enabled;  /* the appliance is the AppSandbox domain controller */
    BOOL domain_busy;     /* promotion in progress */
    DWORD data_size_gb;
    DWORD ram_mb;
    DWORD cpu_cores;
    LONG active_clients;
    LONG host_mounts;
    wchar_t storage_root[MAX_PATH];
    wchar_t os_vhdx_path[MAX_PATH];
    wchar_t data_vhdx_path[MAX_PATH];
    wchar_t admin_user[128];
    wchar_t windows_image_name[256];
    wchar_t management_cert_thumbprint[128];
    wchar_t last_error[512];
    wchar_t progress_text[256];
} SharedApplianceStatus;

void shared_appliance_init(void);
void shared_appliance_cleanup(void);
void shared_appliance_get_status(SharedApplianceStatus *out);

HRESULT shared_appliance_setup(const SharedApplianceConfig *config);
HRESULT shared_appliance_start(BOOL wait_ready, DWORD timeout_ms);
HRESULT shared_appliance_stop(BOOL force);
HRESULT shared_appliance_update(void);
HRESULT shared_appliance_grow(DWORD new_size_gb);
HRESULT shared_appliance_rebuild(const SharedApplianceConfig *replacement,
                                 BOOL switch_backend);
HRESULT shared_appliance_delete(void);
HRESULT shared_appliance_reconcile(void);

/* Called by the normal VM lifecycle. It starts the dependency, then creates
   the client's isolated endpoint. allow_missing implements the explicit
   GUI/API bypass; failure otherwise prevents the client VM from starting. */
HRESULT shared_appliance_prepare_client(const VmConfig *config,
                                        VmInstance *runtime,
                                        wchar_t *endpoint_guid,
                                        size_t endpoint_guid_chars,
                                        BOOL allow_missing);
void shared_appliance_release_client(VmInstance *runtime);

HRESULT shared_appliance_mount_host_resource(const wchar_t *resource_id);
HRESULT shared_appliance_unmount_host_resource(const wchar_t *resource_id);
BOOL shared_appliance_host_mounted(const wchar_t *resource_id);
HRESULT shared_appliance_assign_host_letter(const wchar_t *resource_id);
/* \\<appliance>\<share> for a resource, as the host opens it. */
HRESULT shared_appliance_resource_unc_path(const wchar_t *resource_id,
                                           wchar_t *out, size_t chars);
HRESULT shared_appliance_purge_resource(const wchar_t *resource_id);
HRESULT shared_appliance_unpublish_resource(const wchar_t *resource_id);
HRESULT shared_appliance_open_terminal(void);

/* Attach the stopped appliance's OS disk and copy the tail of the guest's own
   agent/setup logs into the host log. The only view of the guest side of a
   failed readiness handshake. No-op while the appliance is running. */
void shared_appliance_collect_guest_log(void);

BOOL shared_appliance_handle_hcs_state(VmInstance *instance, DWORD event);
BOOL shared_appliance_owns_instance(const VmInstance *instance);
VmInstance *shared_appliance_instance_by_id(UINT64 id);
VmInstance *shared_appliance_runtime(void);
BOOL shared_appliance_get_smb_credentials(wchar_t *user, size_t user_chars,
                                          wchar_t *password, size_t password_chars);
const char *shared_appliance_server_ip(void);

/* ---- AppSandbox domain (Windows Server Core appliance only) ----
   Optional: the appliance becomes the domain controller of ASB_DOMAIN_FQDN,
   without password complexity rules. Managed VMs join it; its users and
   groups are managed from the host. Messages carry the appliance's error. */
#define ASB_DOMAIN_FQDN    "appsandbox.internal"
#define ASB_DOMAIN_NETBIOS "APPSANDBOX"
#define ASB_DOMAIN_REPLY_MAX 32768
HRESULT shared_appliance_domain_setup(void);
HRESULT shared_appliance_domain_list(char *json, size_t chars,
                                     wchar_t *message, size_t message_chars);
/* password NULL leaves it unchanged; groups is the complete comma-separated
   list of manageable groups the user should be in. */
HRESULT shared_appliance_domain_user_set(const wchar_t *name, const wchar_t *password,
                                         BOOL enabled, const wchar_t *groups,
                                         wchar_t *message, size_t message_chars);
HRESULT shared_appliance_domain_user_delete(const wchar_t *name,
                                            wchar_t *message, size_t message_chars);
HRESULT shared_appliance_domain_group_add(const wchar_t *name,
                                          wchar_t *message, size_t message_chars);
HRESULT shared_appliance_domain_group_delete(const wchar_t *name,
                                             wchar_t *message, size_t message_chars);
/* The credential-bearing domain_join command for a managed VM whose link to
   the domain controller is dc_ip. FALSE when there is no domain. */
BOOL shared_appliance_domain_join_command(const char *dc_ip, char *command, size_t chars);

/* Shared with the VPN gateway, the other Ubuntu appliance VM. */
HRESULT appliance_download_ubuntu_image(wchar_t *image_path, size_t image_chars);
HRESULT appliance_run_hidden_process(wchar_t *command, DWORD timeout_ms);
void serial_clean_line(const char *line, char *clean, size_t clean_size);
void appliance_start_serial_capture(const wchar_t *vm_name, const wchar_t *label,
                                    const wchar_t *log_path);

#endif
