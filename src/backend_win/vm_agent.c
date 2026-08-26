#include <winsock2.h>
#include "vm_agent.h"
#include "vm_ssh_proxy.h"
#include "asb_core.h"
#include "hcn_network.h"
#include "shared_appliance.h"
#include "ui.h"
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* Superseded by hcs_service_guid(vm->os_type, 1, ...) — kept for grep.
   Windows VMs end up reaching the byte-identical GUID via the helper. */
static const GUID AGENT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0001, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Agent status notification ---- */

#define WM_VM_AGENT_STATUS      (WM_APP + 2)
#define WM_VM_AGENT_SHUTDOWN    (WM_APP + 3)
#define WM_VM_AGENT_GPUCOPY     (WM_APP + 4)
#define WM_VM_HYPERV_VIDEO_OFF  (WM_APP + 12)

static HWND g_agent_hwnd = NULL;

void vm_agent_set_hwnd(HWND hwnd)
{
    g_agent_hwnd = hwnd;
}

/* ---- Per-VM connection state ---- */

typedef struct AgentConn {
    /* Stable VM identifier; survives g_vms[] compaction. The actual
       VmInstance* is resolved via asb_find_vm_by_id() at each use. */
    UINT64         vm_id;
    HANDLE         thread;
    SOCKET         sock;
    volatile BOOL  stop;
    /* Command synchronization */
    volatile BOOL  cmd_pending;
    HANDLE         cmd_done;     /* Event: signaled when response is ready */
    char           cmd[2048];
    char           rsp[256];
    DWORD          cmd_timeout_ms; /* How long the guest may take on cmd */
    unsigned int   cmd_seq;      /* Monotonic sequence ID for tagged commands */
} AgentConn;

/* A guest that is executing a command runs it on the same thread that sends
   heartbeats, so it goes quiet for the duration -- minutes, for the appliance's
   first-boot storage setup. Silence is only a disconnect once it outlasts this
   many missed heartbeats' worth of time (the guest beats every 5s). */
#define AGENT_IDLE_TIMEOUT_MS 30000

#define MAX_AGENTS 16
static AgentConn g_conns[MAX_AGENTS];
static BOOL      g_wsa_init = FALSE;

static AgentConn *find_conn(VmInstance *vm)
{
    int i;
    if (!vm || vm->unique_id == 0) return NULL;
    for (i = 0; i < MAX_AGENTS; i++)
        if (g_conns[i].vm_id == vm->unique_id)
            return &g_conns[i];
    return NULL;
}

static AgentConn *alloc_conn(VmInstance *vm)
{
    int i;
    if (!vm || vm->unique_id == 0) return NULL;
    for (i = 0; i < MAX_AGENTS; i++) {
        if (g_conns[i].vm_id == 0) {
            memset(&g_conns[i], 0, sizeof(AgentConn));
            g_conns[i].vm_id = vm->unique_id;
            g_conns[i].sock = INVALID_SOCKET;
            g_conns[i].cmd_done = CreateEventW(NULL, FALSE, FALSE, NULL);
            return &g_conns[i];
        }
    }
    return NULL;
}

static void free_conn(AgentConn *conn)
{
    if (conn->cmd_done) CloseHandle(conn->cmd_done);
    if (conn->thread) CloseHandle(conn->thread);
    memset(conn, 0, sizeof(AgentConn));
    conn->sock = INVALID_SOCKET;
}

/* ---- Line I/O ---- */

/* Read a single line (up to \n) from socket. Returns length, 0 on close, -1 on error. */
/* SO_RCVTIMEO on the agent socket is deliberately short so the connection
   thread stays responsive to conn->stop. A recv timeout is therefore NOT a
   disconnect: it fires constantly while the guest is busy running a command
   (it emits no heartbeats until the command returns). Treating it as one is
   what dropped the appliance agent five seconds into every long command --
   configure_shared_nic, the SMB mapping, appliance_ready -- leaving the host
   with "no reply" and an agent that had never actually gone away.
   Only silence past the caller's own deadline counts as a dead connection. */
static int recv_line_wait(SOCKET s, char *buf, int buf_size, DWORD timeout_ms)
{
    int pos = 0;
    ULONGLONG deadline = GetTickCount64() +
        (timeout_ms ? timeout_ms : AGENT_IDLE_TIMEOUT_MS);
    while (pos < buf_size - 1) {
        fd_set rfds;
        struct timeval tv;
        char c;
        int n, ready;

        /* select() decides whether data is there, so recv() is only ever
           called when it will not block. That makes n == 0 mean the peer
           really closed and n < 0 a real error, instead of depending on how
           this socket provider chooses to report an SO_RCVTIMEO expiry --
           which is what silently hung up on the guest mid-command. */
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        ready = select(0, &rfds, NULL, NULL, &tv);
        if (ready == SOCKET_ERROR) return -1;
        if (ready == 0) {
            if (GetTickCount64() >= deadline) return -1;
            continue;   /* guest is busy running a command; it owes us nothing yet */
        }

        n = recv(s, &c, 1, 0);
        if (n <= 0) return n;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

static int recv_line(SOCKET s, char *buf, int buf_size)
{
    return recv_line_wait(s, buf, buf_size, 0);
}

static int send_line(SOCKET s, const char *msg)
{
    int len = (int)strlen(msg);
    int n;
    n = send(s, msg, len, 0);
    if (n <= 0) return n;
    n = send(s, "\n", 1, 0);
    return n;
}

/* ---- RuntimeId lookup ---- */

static BOOL get_vm_runtime_id(VmInstance *instance, GUID *out)
{
    static const GUID zero_guid = {0};

    if (memcmp(&instance->runtime_id, &zero_guid, sizeof(GUID)) != 0) {
        *out = instance->runtime_id;
        return TRUE;
    }

    if (hcs_find_runtime_id(instance->name, out)) {
        instance->runtime_id = *out;
        return TRUE;
    }

    return FALSE;
}

/* ---- Non-blocking connect with timeout ---- */

static SOCKET connect_to_agent(VmInstance *vm, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    GUID runtime_id;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;

    if (!get_vm_runtime_id(vm, &runtime_id))
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* Non-blocking connect */
    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family = AF_HYPERV;
    addr.VmId = runtime_id;
    hcs_service_guid(vm->os_type, 1, &addr.ServiceId);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    /* Back to blocking with timeouts */
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

/* ---- Notify UI of agent status change ---- */

static void notify_agent_status(VmInstance *vm)
{
    if (g_agent_hwnd)
        PostMessageW(g_agent_hwnd, WM_VM_AGENT_STATUS, 0, (LPARAM)vm);
}

/* Fire-and-forget: ask the guest agent to write the AppSandbox public key into
   authorized_keys. The guest replies async (untagged) "ssh_key_deployed" or
   "ssh_key_failed" (handled in process_async_message). Sent once SSH is ready;
   no-op if not requested, already done, or the key is missing. */
static void vm_agent_send_deploy_key(SOCKET s, VmInstance *vm)
{
    char cmd[640], pubkey_a[512];
    if (!vm->ssh_deploy_key || !vm->ssh_pubkey[0] || vm->ssh_key_deployed)
        return;
    WideCharToMultiByte(CP_UTF8, 0, vm->ssh_pubkey, -1, pubkey_a, sizeof(pubkey_a), NULL, NULL);
    sprintf_s(cmd, sizeof(cmd), "ssh_deploy_key %s", pubkey_a);
    send_line(s, cmd);
    ui_log(L"Requested SSH key deploy for \"%s\".", vm->name);
}

/* Process an untagged (async) message from the agent.
   Returns: 0 = handled, 1 = os_shutdown (caller should break),
            2 = service_stopping (caller should break and let the reconnect
                loop retry - SCM restarts the service on failure, so we must
                keep trying to reconnect). */
static int process_async_message(VmInstance *vm, SOCKET s, const char *buf)
{
    if (strcmp(buf, "heartbeat") == 0) {
        vm->last_heartbeat = GetTickCount64();
    } else if (strcmp(buf, "os_shutdown") == 0) {
        ui_log(L"Guest OS shutting down for \"%s\".", vm->name);
        vm->agent_online = FALSE;
        vm->idd_ready = FALSE;
        vm->shutdown_requested = TRUE;
        vm->shutdown_time = GetTickCount64();
        notify_agent_status(vm);
        return 1;
    } else if (strcmp(buf, "service_stopping") == 0) {
        ui_log(L"Agent service stopped in \"%s\".", vm->name);
        return 2;
    } else if (strncmp(buf, "gpu_copy_progress:", 18) == 0) {
        ui_log(L"GPU copy progress for \"%s\": %S", vm->name, buf + 18);
    } else if (strncmp(buf, "gpu_copy_done:", 14) == 0) {
        ui_log(L"GPU copy complete for \"%s\" (%S files).", vm->name, buf + 14);
    } else if (strncmp(buf, "gpu_copy_error:", 15) == 0) {
        ui_log(L"GPU copy error for \"%s\": %S", vm->name, buf + 15);
    } else if (strncmp(buf, "gpu_device_status:", 18) == 0) {
        ui_log(L"[%s] GPU: %S", vm->name, buf + 18);
    } else if (strcmp(buf, "gpu_device_ok") == 0) {
        ui_log(L"[%s] GPU device recovered successfully.", vm->name);
    } else if (strncmp(buf, "gpu_device_failed:", 18) == 0) {
        ui_log(L"[%s] GPU device still failing (problem %S).", vm->name, buf + 18);
    } else if (strncmp(buf, "idd_status:", 11) == 0) {
        /* Latch display readiness from the guest's own driver-state report
           ("running" via devcon). This is the non-destructive readiness
           signal -- it never touches the frame channel, so polling it can't
           steal the single consumer slot or blank the display. asb_vm_idd_ready
           gates display-open on it. */
        vm->idd_ready = (strcmp(buf + 11, "ok") == 0);
        ui_log(L"[%s] IDD driver: %S", vm->name, buf + 11);
        /* The initial status can be a transient not_found/stopped result.
           Notify the UI on every status change so a later periodic recovery
           to "ok" can open the display without requiring a VM restart. */
        notify_agent_status(vm);
    } else if (strncmp(buf, "hyperv_video:", 13) == 0) {
        ui_log(L"[%s] Hyper-V Video: %S", vm->name, buf + 13);
        /* NULL-guard like notify_agent_status: headless never sets the HWND,
           and PostMessageW(NULL, ...) would queue thread messages on this
           never-pumped agent thread. */
        if (g_agent_hwnd && strcmp(buf + 13, "disabled") == 0)
            PostMessageW(g_agent_hwnd, WM_VM_HYPERV_VIDEO_OFF, 0, (LPARAM)vm);
    } else if (strncmp(buf, "displays:", 9) == 0) {
        ui_log(L"[%s] Displays: %S", vm->name, buf + 9);
    } else if (strncmp(buf, "log:", 4) == 0) {
        ui_log(L"[%s] %S", vm->name, buf + 4);
    } else if (strcmp(buf, "gpu_query") == 0) {
        if (vm->gpu_mode != 0 && vm->gpu_shares.count > 0) {
            char header[64];
            int gi;
            sprintf_s(header, sizeof(header), "gpu_query_response:%d",
                      vm->gpu_shares.count);
            send_line(s, header);
            for (gi = 0; gi < vm->gpu_shares.count; gi++) {
                const GpuDriverShare *ds = &vm->gpu_shares.shares[gi];
                char line[8192];
                char share_a[128], dest_a[512], filter_a[4096];
                WideCharToMultiByte(CP_UTF8, 0, ds->share_name, -1,
                                    share_a, sizeof(share_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->guest_path, -1,
                                    dest_a, sizeof(dest_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->file_filter, -1,
                                    filter_a, sizeof(filter_a), NULL, NULL);
                sprintf_s(line, sizeof(line), "%s|%s|%s", share_a, dest_a, filter_a);
                send_line(s, line);
            }
        } else {
            send_line(s, "gpu_none");
        }
    } else if (strcmp(buf, "ssh_ready") == 0) {
        vm->ssh_state = 2;
        vm_ssh_proxy_start(vm);
        ui_log(L"SSH ready for \"%s\".", vm->name);
        vm_agent_send_deploy_key(s, vm);   /* deploy the AppSandbox key now SSH is up */
        notify_agent_status(vm);
    } else if (strcmp(buf, "ssh_key_deployed") == 0) {
        vm->ssh_key_deployed = TRUE;
        ui_log(L"SSH key deployed for \"%s\".", vm->name);
        notify_agent_status(vm);
    } else if (strcmp(buf, "ssh_key_failed") == 0) {
        ui_log(L"SSH key deploy FAILED for \"%s\".", vm->name);
        notify_agent_status(vm);
    } else if (strcmp(buf, "ssh_failed") == 0) {
        vm->ssh_state = 3;
        ui_log(L"SSH install failed for \"%s\".", vm->name);
        notify_agent_status(vm);
    } else if (strcmp(buf, "ssh_installing") == 0) {
        vm->ssh_state = 1;
        ui_log(L"SSH installing for \"%s\"...", vm->name);
        notify_agent_status(vm);
    }
    return 0;
}

/* Send a tagged command and wait for the tagged response.
   Processes any interleaved async messages while waiting.
   Returns: response length on success, 0 on close, -1 on error. */
static int send_tagged_cmd(SOCKET s, VmInstance *vm, unsigned int *seq,
                           const char *cmd, char *rsp, int rsp_size,
                           DWORD timeout_ms)
{
    char tagged[4096];
    char prefix[32];
    int pfx_len, n;

    (*seq)++;
    sprintf_s(tagged, sizeof(tagged), "%u:%s", *seq, cmd);
    sprintf_s(prefix, sizeof(prefix), "%u:", *seq);
    pfx_len = (int)strlen(prefix);

    if (send_line(s, tagged) <= 0) return -1;

    for (;;) {
        n = recv_line_wait(s, rsp, rsp_size, timeout_ms);
        if (n <= 0) return n;

        if (strncmp(rsp, prefix, pfx_len) == 0) {
            /* Tagged response - strip prefix */
            memmove(rsp, rsp + pfx_len, strlen(rsp + pfx_len) + 1);
            return (int)strlen(rsp);
        }

        /* Untagged = async message, process inline. A shutdown or service
           stop invalidates the in-flight command; do not continue startup
           configuration against a guest that is going away. */
        if (process_async_message(vm, s, rsp) != 0) return -1;
    }
}

static BOOL configure_guest_nat(SOCKET s, VmInstance *vm, AgentConn *conn,
                                char *response, int response_max)
{
    char ip_cmd[160], nat_mac[32] = "";
    HRESULT mac_hr;
    int n;

    if (!vm || !conn || !response || response_max <= 0 ||
        vm->network_mode != NET_NAT || vm->nat_ip[0] == '\0')
        return TRUE;

    mac_hr = hcn_get_endpoint_mac(&vm->endpoint_id, nat_mac, sizeof(nat_mac));
    if (SUCCEEDED(mac_hr) && nat_mac[0]) {
        sprintf_s(ip_cmd, sizeof(ip_cmd), "set_ip:%s/24:%s.1:%s",
                  vm->nat_ip, hcn_nat_subnet_base(), nat_mac);
        ui_log(L"NAT endpoint MAC for \"%s\": %S", vm->name, nat_mac);
    } else {
        /* Older HCN builds may omit MacAddress from the endpoint query. */
        sprintf_s(ip_cmd, sizeof(ip_cmd), "set_ip:%s/24:%s.1",
                  vm->nat_ip, hcn_nat_subnet_base());
        ui_log(L"Could not read NAT endpoint MAC for \"%s\" (0x%08X); using guest adapter discovery.",
               vm->name, mac_hr);
    }

    n = send_tagged_cmd(s, vm, &conn->cmd_seq, ip_cmd,
                        response, response_max, 60000);
    if (n <= 0) return FALSE;
    ui_log(L"NAT IP config for \"%s\": %S", vm->name, response);
    return TRUE;
}

/* ---- Persistent connection thread ---- */

static DWORD WINAPI agent_thread_proc(LPVOID param)
{
    AgentConn *conn = (AgentConn *)param;
    VmInstance *vm;

    /* Resolve VM by stable ID each iteration. If asb_find_vm_by_id
       returns NULL the VM has been deleted (slot reclaimed) -- exit
       cleanly. Pointer freshness is now guaranteed for the body of
       each iteration; we never stash a stale &g_vms[idx]. */
    while (!conn->stop && (vm = asb_find_vm_by_id(conn->vm_id)) != NULL) {
        char buf[256];
        int n;
        BOOL nat_configured = FALSE;
        SOCKET s;

        /* Try to connect */
        s = connect_to_agent(vm, 3000);
        if (s == INVALID_SOCKET) {
            /* Retry in 3 seconds, checking stop flag each second */
            int wait;
            for (wait = 0; wait < 3000 && !conn->stop; wait += 500)
                Sleep(500);
            continue;
        }

        conn->sock = s;

        /* Wait for hello from agent */
        n = recv_line(s, buf, sizeof(buf));
        if (n <= 0 || strcmp(buf, "hello") != 0) {
            closesocket(s);
            conn->sock = INVALID_SOCKET;
            continue;
        }

        vm->agent_initializing = TRUE;
        vm->agent_online = TRUE;
        vm->idd_ready = FALSE;   /* re-evaluated by the agent's idd_status, sent right after hello */
        vm->shutdown_requested = FALSE;
        vm->last_heartbeat = GetTickCount64();
        ui_log(L"Agent online for \"%s\".", vm->name);

        /* Mark install complete on first agent connection */
        if (!vm->install_complete && !vm->is_template) {
            vm->install_complete = TRUE;
            vm_save_state_json(vm->vhdx_path, TRUE);
            ui_log(L"Install complete for \"%s\".", vm->name);
        }

        /* A host VHDX grow is irreversible, while guest partition/filesystem
           expansion is explicitly best-effort. Attempt this request once on
           the first agent connection after resize, then clear it regardless of
           the reply so later boots do not retry silently. */
        if (vm->guest_grow_target_gb) {
            char grow_cmd[64];
            DWORD target_gb = vm->guest_grow_target_gb;
            sprintf_s(grow_cmd, sizeof(grow_cmd), "grow_root:%lu", target_gb);
            n = send_tagged_cmd(s, vm, &conn->cmd_seq, grow_cmd, buf, sizeof(buf), 180000);
            vm->guest_grow_target_gb = 0;
            asb_save();
            if (n <= 0) {
                ui_log(L"Guest disk expansion for \"%s\" could not be delivered; expand it manually.", vm->name);
                goto disconnected;
            }
            if (strcmp(buf, "ok") == 0)
                ui_log(L"Guest root partition/filesystem expanded for \"%s\".", vm->name);
            else
                ui_log(L"Guest disk expansion failed for \"%s\" (%S); expand it manually.", vm->name, buf);
            notify_agent_status(vm);
        }

        /* Configure the independent host-only NIC, then recreate signed global
           SMB mappings after every agent connection. Credentials are redacted
           from logs and scrubbed immediately after delivery. */
        if (vm->shared_resource_count > 0 &&
            _wcsicmp(vm->shared_resource_transport, L"appliance") == 0 &&
            vm->share_ip[0] && vm->share_host_ip[0] && vm->share_mac[0]) {
            int ri;
            wchar_t user_w[128], password_w[128];
            char user[256], password[256];
            char net_cmd[128];
            HRESULT cred_hr;
            vm->shared_resource_error[0] = L'\0';
            sprintf_s(net_cmd, sizeof(net_cmd), "shared_net:%s:%s",
                      vm->share_mac, vm->share_ip);
            n = send_tagged_cmd(s, vm, &conn->cmd_seq, net_cmd, buf, sizeof(buf), 120000);
            if (n <= 0) goto disconnected;
            if (strcmp(buf, "ok") != 0) {
                swprintf_s(vm->shared_resource_error,
                           _countof(vm->shared_resource_error),
                           L"Private SMB adapter configuration failed: %S", buf);
                for (ri = 0; ri < vm->shared_resource_count; ri++) {
                    wcscpy_s(vm->shared_resources[ri].mapping_result,
                             _countof(vm->shared_resources[ri].mapping_result), L"unavailable");
                    swprintf_s(vm->shared_resources[ri].failure,
                               _countof(vm->shared_resources[ri].failure), L"%S", buf);
                }
                ui_log(L"Shared-resource adapter failed for \"%s\" (%S).",
                       vm->name, buf);
                notify_agent_status(vm);
                goto shared_mapping_done;
            }

            /* Configure NAT immediately after the private adapter is ready.
               Mapping an appliance share may wait for SMB startup; it must not
               delay internet configuration or leave NAT running during a later
               guest shutdown. */
            if (!configure_guest_nat(s, vm, conn, buf, sizeof(buf)))
                goto disconnected;
            nat_configured = TRUE;

            cred_hr = shared_appliance_get_smb_credentials(
                user_w, _countof(user_w), password_w, _countof(password_w))
                ? S_OK : HRESULT_FROM_WIN32(ERROR_LOGON_FAILURE);
            if (FAILED(cred_hr)) {
                swprintf_s(vm->shared_resource_error,
                           _countof(vm->shared_resource_error),
                           L"SMB credential unavailable: 0x%08X", cred_hr);
                ui_log(L"Shared-resource credentials unavailable for \"%s\" (0x%08X).",
                       vm->name, cred_hr);
                notify_agent_status(vm);
                goto shared_mapping_done;
            }
            WideCharToMultiByte(CP_UTF8, 0, user_w, -1, user, sizeof(user), NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, password_w, -1, password, sizeof(password), NULL, NULL);
            for (ri = 0; ri < vm->shared_resource_count; ri++) {
                char map_cmd[1024], share[64];
                HcsSharedResource *r = &vm->shared_resources[ri];
                if (_wcsicmp(r->mapping_result, L"unavailable") == 0) continue;
                WideCharToMultiByte(CP_UTF8, 0, r->share_name, -1,
                                    share, sizeof(share), NULL, NULL);
                sprintf_s(map_cmd, sizeof(map_cmd), "shared_smb_map:%c:%s:%s:%s:%s",
                          (char)r->drive_letter, vm->share_host_ip, share, user, password);
                n = send_tagged_cmd(s, vm, &conn->cmd_seq,
                                    map_cmd, buf, sizeof(buf), 120000);
                SecureZeroMemory(map_cmd, sizeof(map_cmd));
                if (n <= 0) {
                    SecureZeroMemory(password_w, sizeof(password_w));
                    SecureZeroMemory(password, sizeof(password));
                    SecureZeroMemory(user_w, sizeof(user_w));
                    SecureZeroMemory(user, sizeof(user));
                    goto disconnected;
                }
                if (strcmp(buf, "ok") != 0) {
                    wcscpy_s(r->mapping_result, _countof(r->mapping_result), L"failed");
                    swprintf_s(r->failure, _countof(r->failure), L"%S", buf);
                    swprintf_s(vm->shared_resource_error,
                        _countof(vm->shared_resource_error),
                        L"%c: %S", r->drive_letter, buf);
                    ui_log(L"Shared resource mapping failed for \"%s\" %c: (%S).",
                           vm->name, r->drive_letter, buf);
                } else {
                    wcscpy_s(r->mapping_result, _countof(r->mapping_result), L"mapped");
                    r->failure[0] = L'\0';
                    ui_log(L"Mapped shared resource for \"%s\" at %c:.",
                           vm->name, r->drive_letter);
                }
            }
            SecureZeroMemory(password_w, sizeof(password_w));
            SecureZeroMemory(password, sizeof(password));
            SecureZeroMemory(user_w, sizeof(user_w));
            SecureZeroMemory(user, sizeof(user));
            notify_agent_status(vm);
        }
shared_mapping_done:

        /* NAT-only VMs do not enter the appliance block above. */
        if (!nat_configured && !configure_guest_nat(s, vm, conn, buf, sizeof(buf)))
            goto disconnected;

        /* Send GPU share info to agent (if GPU-PV is assigned).
           Fire-and-forget - no response expected, so no tagging needed. */
        if (vm->gpu_mode != 0 && vm->gpu_shares.count > 0) {
            char header[64];
            int gi;
            sprintf_s(header, sizeof(header), "gpu_query_response:%d",
                      vm->gpu_shares.count);
            send_line(s, header);
            for (gi = 0; gi < vm->gpu_shares.count; gi++) {
                const GpuDriverShare *ds = &vm->gpu_shares.shares[gi];
                char line[8192];
                char share_a[128], dest_a[512], filter_a[4096];

                WideCharToMultiByte(CP_UTF8, 0, ds->share_name, -1,
                                    share_a, sizeof(share_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->guest_path, -1,
                                    dest_a, sizeof(dest_a), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, ds->file_filter, -1,
                                    filter_a, sizeof(filter_a), NULL, NULL);

                sprintf_s(line, sizeof(line), "%s|%s|%s",
                          share_a, dest_a, filter_a);
                send_line(s, line);
            }
            ui_log(L"Sent %d GPU share(s) to agent for \"%s\".",
                   vm->gpu_shares.count, vm->name);
        } else {
            send_line(s, "gpu_none");
        }

        /* Request SSH install/enable if configured */
        if (vm->ssh_enabled) {
            n = send_tagged_cmd(s, vm, &conn->cmd_seq, "ssh_enable", buf, sizeof(buf), 600000);
            if (n <= 0) goto disconnected;
            if (strcmp(buf, "ssh_ready") == 0) {
                vm->ssh_state = 2;
                vm_ssh_proxy_start(vm);
                ui_log(L"SSH ready for \"%s\".", vm->name);
                vm_agent_send_deploy_key(s, vm);   /* deploy the AppSandbox key now SSH is up */
            } else if (strcmp(buf, "ssh_installing") == 0) {
                vm->ssh_state = 1;
                ui_log(L"SSH installing for \"%s\"...", vm->name);
            } else if (strcmp(buf, "ssh_failed") == 0) {
                vm->ssh_state = 3;
                ui_log(L"SSH install failed for \"%s\".", vm->name);
            }
        }

        /* Startup commands are complete. Let the background synchronizer
           retry any resource whose appliance/share was not ready yet. */
        vm->agent_initializing = FALSE;
        asb_request_shared_resource_sync();

        /* Notify UI - agent online + SSH state are all set now */
        notify_agent_status(vm);

        /* Connected - read loop */
        while (!conn->stop) {
            fd_set rfds;
            struct timeval tv;
            int ret;

            /* Check for pending command first */
            if (conn->cmd_pending) {
                char tagged[4096];
                conn->cmd_seq++;
                sprintf_s(tagged, sizeof(tagged), "%u:%s", conn->cmd_seq, conn->cmd);
                if (send_line(s, tagged) <= 0) break;
                /* Read lines until we get our tagged response. The caller's
                   own timeout is the deadline: a guest running a minutes-long
                   command is silent, not gone. */
                for (;;) {
                    n = recv_line_wait(s, conn->rsp, sizeof(conn->rsp),
                                       conn->cmd_timeout_ms);
                    if (n <= 0) {
                        conn->rsp[0] = '\0';
                        conn->cmd_pending = FALSE;
                        SetEvent(conn->cmd_done);
                        goto disconnected;
                    }
                    /* Check for our sequence tag */
                    {
                        char prefix[32];
                        int pfx_len;
                        sprintf_s(prefix, sizeof(prefix), "%u:", conn->cmd_seq);
                        pfx_len = (int)strlen(prefix);
                        if (strncmp(conn->rsp, prefix, pfx_len) == 0) {
                            /* Tagged response - strip prefix */
                            memmove(conn->rsp, conn->rsp + pfx_len, strlen(conn->rsp + pfx_len) + 1);
                            break;
                        }
                    }
                    /* Untagged = async message, process inline */
                    process_async_message(vm, s, conn->rsp);
                }
                conn->cmd_pending = FALSE;
                SetEvent(conn->cmd_done);
                continue;
            }

            /* Wait for data with 200ms timeout */
            FD_ZERO(&rfds);
            FD_SET(s, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = 200000;

            ret = select(0, &rfds, NULL, NULL, &tv);
            if (ret < 0) break;
            if (ret == 0) continue; /* timeout - loop back to check cmd_pending/stop */

            n = recv_line(s, buf, sizeof(buf));
            if (n <= 0) break; /* connection lost */

            { int rc = process_async_message(vm, s, buf);
              if (rc == 1 || rc == 2) break;   /* os_shutdown or service_stopping - reconnect loop will retry */
            }
        }

        disconnected:
        /* Connection lost */
        vm->agent_online = FALSE;
        vm->agent_initializing = FALSE;
        vm->idd_ready = FALSE;
        /* Atomically claim the socket so we never double-close a handle that
           vm_agent_stop() may have already closed (and whose value could have
           been recycled by another socket()/accept()). */
        {
            SOCKET old = (SOCKET)InterlockedExchangePointer(
                (PVOID volatile *)&conn->sock, (PVOID)INVALID_SOCKET);
            if (old != INVALID_SOCKET)
                closesocket(old);
        }
        ui_log(L"Agent offline for \"%s\".", vm->name);
        notify_agent_status(vm);

        /* Wake up any blocked command sender */
        if (conn->cmd_pending) {
            conn->rsp[0] = '\0';
            conn->cmd_pending = FALSE;
            SetEvent(conn->cmd_done);
        }

        /* Don't reconnect if the VM is no longer running */
        if (!vm->running)
            break;
    }

    return 0;
}

/* ---- Public API ---- */

void vm_agent_start(VmInstance *instance)
{
    AgentConn *conn;
    WSADATA wsa;

    if (!g_wsa_init) {
        WSAStartup(MAKEWORD(2, 2), &wsa);
        g_wsa_init = TRUE;
    }

    /* Already running? */
    conn = find_conn(instance);
    if (conn && conn->thread) return;

    conn = alloc_conn(instance);
    if (!conn) {
        ui_log(L"Agent: too many connections");
        return;
    }

    conn->stop = FALSE;
    conn->thread = CreateThread(NULL, 0, agent_thread_proc, conn, 0, NULL);
}

void vm_agent_stop(VmInstance *instance)
{
    AgentConn *conn = find_conn(instance);
    if (!conn) return;

    conn->stop = TRUE;

    /* Unblock recv/select by closing the socket. Atomically claim it so we
       never double-close a handle the agent thread may close concurrently at
       its disconnected: label (a recycled value could close a live unrelated
       socket). */
    {
        SOCKET old = (SOCKET)InterlockedExchangePointer(
            (PVOID volatile *)&conn->sock, (PVOID)INVALID_SOCKET);
        if (old != INVALID_SOCKET)
            closesocket(old);
    }

    if (conn->thread) {
        WaitForSingleObject(conn->thread, 5000);
    }

    instance->agent_online = FALSE;
    instance->agent_initializing = FALSE;
    instance->idd_ready = FALSE;
    free_conn(conn);
    notify_agent_status(instance);
}

BOOL vm_agent_send(VmInstance *instance, const char *command,
                   char *response, int response_max, DWORD timeout_ms)
{
    AgentConn *conn = find_conn(instance);
    BOOL ok;
    BOOL redact = command &&
        (strncmp(command, "appliance_account:", 18) == 0 ||
         strncmp(command, "shared_smb_map:", 15) == 0);

    if (!conn || !instance->agent_online) {
        ui_log(L"Agent: not connected to \"%s\"", instance->name);
        return FALSE;
    }

    /* Hand the command to the connection thread (it owns the socket and is the
       sole sender). NOTE: this is a single slot per VM (conn->cmd), not a queue,
       so concurrent callers for the SAME VM would clobber -- safe here because
       per VM only one caller exists (shutdown). */
    ResetEvent(conn->cmd_done);
    strcpy_s(conn->cmd, sizeof(conn->cmd), command);
    /* The connection thread must not give up on the reply before we do. */
    conn->cmd_timeout_ms = timeout_ms;
    conn->cmd_pending = TRUE;

    /* timeout_ms == 0  =>  FIRE-AND-FORGET. Used for shutdown/restart, which ride
       the guest powering off: the agent replies "ok" then kills itself, so there
       is no reliable synchronous reply to wait for. The connection thread sends
       the queued command and consumes the (ignored) reply on its OWN read loop;
       we return immediately, so we never block the single-threaded HTTP request
       loop / the GUI thread. Delivery is confirmed by the HCS SystemExited
       monitor, not by this reply. */
    if (timeout_ms == 0)
        return TRUE;

    /* Otherwise wait up to timeout_ms for the agent's tagged reply (idd_connect
       expects a prompt "ok"). A real disconnect unblocks us: agent_thread_proc
       SetEvent()s cmd_done with an empty rsp on recv<=0, so we return FALSE. */
    if (WaitForSingleObject(conn->cmd_done, timeout_ms) != WAIT_OBJECT_0) {
        if (redact) ui_log(L"Agent: credential-bearing command timed out");
        else ui_log(L"Agent: command \"%S\" timed out", command);
        conn->cmd_pending = FALSE;
        return FALSE;
    }

    if (response && response_max > 0)
        strncpy_s(response, response_max, conn->rsp, _TRUNCATE);

    ok = (strcmp(conn->rsp, "ok") == 0 || strncmp(conn->rsp, "ok:", 3) == 0);
    if (redact) ui_log(L"Agent: credential-bearing command -> %S", conn->rsp);
    else ui_log(L"Agent: %S -> %S", command, conn->rsp);
    return ok;
}

BOOL vm_agent_shutdown(VmInstance *instance)
{
    return vm_agent_send(instance, "shutdown", NULL, 0, 0);   /* 0 = fire-and-forget */
}

BOOL vm_agent_restart(VmInstance *instance)
{
    return vm_agent_send(instance, "restart", NULL, 0, 0);   /* 0 = fire-and-forget */
}

BOOL vm_agent_ping(VmInstance *instance)
{
    return vm_agent_send(instance, "ping", NULL, 0, 5000);
}

/* Map one shared resource into an already-running guest.
   agent_thread_proc does this for every resource the VM booted with, straight
   on its own socket; these two go through the queued-command slot so a
   resource added, dropped or re-scoped while the VM runs takes effect without
   a restart. Both update the caller's HcsSharedResource bookkeeping. */
BOOL vm_agent_map_shared_resource(VmInstance *instance, HcsSharedResource *resource)
{
    wchar_t user_w[128], password_w[128];
    char user[256], password[256], share[64], command[1024], response[128] = "";
    BOOL ok;

    if (!instance || !resource || !resource->drive_letter) return FALSE;
    if (!instance->running || !instance->agent_online ||
        _wcsicmp(instance->shared_resource_transport, L"appliance") != 0 ||
        instance->share_host_ip[0] == '\0') {
        wcscpy_s(resource->mapping_result, _countof(resource->mapping_result), L"pending");
        return FALSE;
    }
    if (!shared_appliance_get_smb_credentials(user_w, _countof(user_w),
                                              password_w, _countof(password_w))) {
        wcscpy_s(resource->mapping_result, _countof(resource->mapping_result), L"failed");
        wcscpy_s(resource->failure, _countof(resource->failure),
                 L"SMB credential unavailable");
        return FALSE;
    }
    WideCharToMultiByte(CP_UTF8, 0, user_w, -1, user, sizeof(user), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, password_w, -1, password, sizeof(password), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, resource->share_name, -1, share, sizeof(share), NULL, NULL);
    sprintf_s(command, sizeof(command), "shared_smb_map:%c:%s:%s:%s:%s",
              (char)resource->drive_letter, instance->share_host_ip, share, user, password);
    ok = vm_agent_send(instance, command, response, sizeof(response), 60000);
    SecureZeroMemory(command, sizeof(command));
    SecureZeroMemory(password, sizeof(password));
    SecureZeroMemory(password_w, sizeof(password_w));
    SecureZeroMemory(user, sizeof(user));
    SecureZeroMemory(user_w, sizeof(user_w));
    if (ok) {
        wcscpy_s(resource->mapping_result, _countof(resource->mapping_result), L"mapped");
        resource->failure[0] = L'\0';
        instance->shared_resource_error[0] = L'\0';
        ui_log(L"Mapped shared resource for \"%s\" at %c:.",
               instance->name, resource->drive_letter);
    } else {
        wcscpy_s(resource->mapping_result, _countof(resource->mapping_result), L"failed");
        swprintf_s(resource->failure, _countof(resource->failure), L"%S",
                   response[0] ? response : "no reply");
        swprintf_s(instance->shared_resource_error,
                   _countof(instance->shared_resource_error), L"%c: %S",
                   resource->drive_letter, response[0] ? response : "no reply");
        ui_log(L"Shared resource mapping failed for \"%s\" %c: (%S).",
               instance->name, resource->drive_letter,
               response[0] ? response : "no reply");
    }
    notify_agent_status(instance);
    return ok;
}

BOOL vm_agent_unmap_shared_resource(VmInstance *instance, wchar_t drive_letter)
{
    char command[64], response[128] = "";
    BOOL ok;
    if (!instance || !drive_letter) return FALSE;
    if (!instance->running || !instance->agent_online) return FALSE;
    sprintf_s(command, sizeof(command), "shared_smb_unmap:%c", (char)drive_letter);
    ok = vm_agent_send(instance, command, response, sizeof(response), 60000);
    if (ok)
        ui_log(L"Unmapped shared resource for \"%s\" at %c:.", instance->name, drive_letter);
    else
        ui_log(L"Shared resource unmap failed for \"%s\" %c: (%S).",
               instance->name, drive_letter, response[0] ? response : "no reply");
    notify_agent_status(instance);
    return ok;
}
