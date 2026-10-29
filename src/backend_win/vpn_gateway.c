#define _CRT_SECURE_NO_WARNINGS
#include "vpn_gateway.h"
#include "vpn_configs.h"
#include "asb_core.h"
#include "hcn_network.h"
#include "disk_util.h"
#include "vm_agent.h"
#include "shared_appliance.h"
#include "ui.h"
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

#pragma comment(lib, "crypt32.lib")

#define GATEWAY_VM_NAME     L"AppSandbox.VpnGateway"
#define GATEWAY_UNIQUE_ID   ((UINT64)0xFFFFFFFFFFFFFE00ull)
#define GATEWAY_ADMIN_USER  L"appsandbox"
/* Requested MACs; the seed's netplan matches the adapters by them. */
#define GATEWAY_UPLINK_MAC  "02-15-5D-A5-B1-FE"
#define GATEWAY_UPLINK_MAC_COLON "02:15:5d:a5:b1:fe"
#define GATEWAY_LAN_MAC     "02-15-5D-A5-B1-02"
#define GATEWAY_LAN_MAC_COLON "02:15:5d:a5:b1:02"
#define GATEWAY_RAM_MB      512
#define GATEWAY_IDLE_MS     (5ull * 60ull * 1000ull)
#define GATEWAY_SETUP_TIMEOUT_MS  (20ull * 60ull * 1000ull)
#define GATEWAY_START_TIMEOUT_MS  (3ull * 60ull * 1000ull)
#define GATEWAY_SLOTS       16
/* Used when a config names no DNS server. Reached through the tunnel. */
#define GATEWAY_FALLBACK_DNS "1.1.1.1,1.0.0.1"

static struct {
    BOOL initialized;
    CRITICAL_SECTION lifecycle;   /* held across start, setup and client prep */
    CRITICAL_SECTION state;       /* short: status fields */
    VpnGatewayStatus status;
    VmInstance runtime;
    GUID uplink_network_id, uplink_endpoint_id, lan_network_id, lan_endpoint_id;
    BOOL uplink_created, lan_created;
    BOOL stopping;
    wchar_t slot_config[GATEWAY_SLOTS][ASB_VPN_ID_CHARS];  /* loaded this boot */
    volatile LONG idle_generation;
    wchar_t root[MAX_PATH], os_vhdx[MAX_PATH], seed_iso[MAX_PATH], state_path[MAX_PATH];
    char lan_ip[32];
} g_gw;

static void set_progress(const wchar_t *text)
{
    EnterCriticalSection(&g_gw.state);
    wcscpy_s(g_gw.status.progress_text, _countof(g_gw.status.progress_text), text);
    LeaveCriticalSection(&g_gw.state);
    ui_log(L"VPN gateway: %s", text);
}

static void set_error(const wchar_t *text, HRESULT hr)
{
    EnterCriticalSection(&g_gw.state);
    swprintf_s(g_gw.status.last_error, _countof(g_gw.status.last_error),
               L"%s (0x%08X)", text, hr);
    LeaveCriticalSection(&g_gw.state);
    ui_log(L"VPN gateway: %s (0x%08X)", text, hr);
}

static void save_state(void)
{
    FILE *f = NULL;
    if (_wfopen_s(&f, g_gw.state_path, L"w,ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"Version=1\nConfigured=%d\n", g_gw.status.configured ? 1 : 0);
    fclose(f);
}

void vpn_gateway_init(void)
{
    wchar_t base[MAX_PATH], line[128];
    FILE *f = NULL;
    if (g_gw.initialized) return;
    InitializeCriticalSection(&g_gw.lifecycle);
    InitializeCriticalSection(&g_gw.state);
    if (!GetEnvironmentVariableW(L"ProgramData", base, MAX_PATH))
        wcscpy_s(base, MAX_PATH, L"C:\\ProgramData");
    swprintf_s(g_gw.root, MAX_PATH, L"%s\\AppSandbox\\AppSandboxVpnGateway", base);
    swprintf_s(g_gw.os_vhdx, MAX_PATH, L"%s\\os.vhdx", g_gw.root);
    swprintf_s(g_gw.seed_iso, MAX_PATH, L"%s\\provision.iso", g_gw.root);
    swprintf_s(g_gw.state_path, MAX_PATH, L"%s\\AppSandbox\\vpn-gateway.cfg", base);
    if (_wfopen_s(&f, g_gw.state_path, L"r,ccs=UTF-8") == 0 && f) {
        while (fgetws(line, _countof(line), f))
            if (wcsncmp(line, L"Configured=1", 12) == 0) g_gw.status.configured = TRUE;
        fclose(f);
    }
    if (g_gw.status.configured && GetFileAttributesW(g_gw.os_vhdx) == INVALID_FILE_ATTRIBUTES)
        g_gw.status.configured = FALSE;
    wcscpy_s(g_gw.status.progress_text, _countof(g_gw.status.progress_text),
             g_gw.status.configured ? L"Stopped" : L"Not set up");
    g_gw.initialized = TRUE;
}

void vpn_gateway_get_status(VpnGatewayStatus *out)
{
    if (!out) return;
    if (!g_gw.initialized) { ZeroMemory(out, sizeof(*out)); return; }
    EnterCriticalSection(&g_gw.state);
    *out = g_gw.status;
    out->running = g_gw.runtime.running;
    LeaveCriticalSection(&g_gw.state);
}

const char *vpn_gateway_lan_ip(void)
{
    if (!g_gw.lan_ip[0])
        sprintf_s(g_gw.lan_ip, sizeof(g_gw.lan_ip), "%s.2", hcn_vpn_subnet_base());
    return g_gw.lan_ip;
}

static BOOL gateway_send(const char *command, char *response, int response_max, DWORD timeout_ms)
{
    response[0] = '\0';
    return vm_agent_send(&g_gw.runtime, command, response, response_max, timeout_ms);
}

static void delete_endpoints(void)
{
    if (g_gw.uplink_created) { hcn_delete_endpoint(&g_gw.uplink_endpoint_id); g_gw.uplink_created = FALSE; }
    if (g_gw.lan_created) { hcn_delete_endpoint(&g_gw.lan_endpoint_id); g_gw.lan_created = FALSE; }
}

/* ---- Setup ---- */

/* Convert the cached Ubuntu cloud image to the gateway's OS disk and write its
   seed. The provisioning boot itself happens in start_vm. */
static HRESULT build_os_disk(void)
{
    wchar_t image[MAX_PATH], exe[MAX_PATH], command[2048], resource_dir[MAX_PATH];
    wchar_t ssh_public_key[512], *slash;
    char uplink_ip[32], uplink_gateway[32];
    HRESULT hr;
    if (!CreateDirectoryW(g_gw.root, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return HRESULT_FROM_WIN32(GetLastError());
    set_progress(L"Downloading the Ubuntu cloud image...");
    hr = appliance_download_ubuntu_image(image, _countof(image));
    if (FAILED(hr)) return hr;
    set_progress(L"Converting the Ubuntu image to the gateway disk...");
    GetModuleFileNameW(NULL, exe, _countof(exe));
    slash = wcsrchr(exe, L'\\'); if (slash) *slash = L'\0';
    DeleteFileW(g_gw.os_vhdx);
    swprintf_s(command, _countof(command),
        L"\"%s\\iso-patch.exe\" --qcow2-to-vhdx \"%s\" --output \"%s\"",
        exe, image, g_gw.os_vhdx);
    hr = appliance_run_hidden_process(command, 30 * 60 * 1000);
    if (FAILED(hr)) return hr;
    if (!asb_ensure_ssh_key(ssh_public_key, _countof(ssh_public_key))) return E_FAIL;
    swprintf_s(resource_dir, _countof(resource_dir), L"%s\\resources", exe);
    sprintf_s(uplink_ip, sizeof(uplink_ip), "%s.253", hcn_nat_subnet_base());
    sprintf_s(uplink_gateway, sizeof(uplink_gateway), "%s.1", hcn_nat_subnet_base());
    set_progress(L"Creating the gateway cloud-init seed...");
    DeleteFileW(g_gw.seed_iso);
    return iso_create_vpn_gateway_cloud_init(g_gw.seed_iso, GATEWAY_ADMIN_USER,
        ssh_public_key, resource_dir, uplink_ip, uplink_gateway,
        GATEWAY_UPLINK_MAC_COLON, vpn_gateway_lan_ip(), GATEWAY_LAN_MAC_COLON);
}

/* ---- Start / stop ---- */

static HRESULT start_vm(BOOL provisioning)
{
    VmConfig *config;
    wchar_t uplink_guid[64] = L"", lan_guid[64] = L"", log_path[MAX_PATH];
    char uplink_ip[32];
    HRESULT hr;

    delete_endpoints();
    hr = hcn_create_nat_network(&g_gw.uplink_network_id);
    if (SUCCEEDED(hr)) {
        sprintf_s(uplink_ip, sizeof(uplink_ip), "%s.253", hcn_nat_subnet_base());
        hr = hcn_create_endpoint_with_mac(&g_gw.uplink_network_id, &g_gw.uplink_endpoint_id,
            uplink_guid, _countof(uplink_guid), uplink_ip, GATEWAY_UPLINK_MAC);
    }
    if (FAILED(hr)) { set_error(L"Could not create the gateway's uplink endpoint", hr); return hr; }
    g_gw.uplink_created = TRUE;
    hr = hcn_create_vpn_network(&g_gw.lan_network_id);
    if (SUCCEEDED(hr))
        hr = hcn_create_static_endpoint(&g_gw.lan_network_id, &g_gw.lan_endpoint_id,
            lan_guid, _countof(lan_guid), vpn_gateway_lan_ip(), GATEWAY_LAN_MAC);
    if (FAILED(hr)) {
        set_error(L"Could not create the VPN network", hr);
        delete_endpoints();
        return hr;
    }
    g_gw.lan_created = TRUE;

    config = (VmConfig *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VmConfig));
    if (!config) { delete_endpoints(); return E_OUTOFMEMORY; }
    wcscpy_s(config->name, _countof(config->name), GATEWAY_VM_NAME);
    wcscpy_s(config->os_type, _countof(config->os_type), L"Linux");
    wcscpy_s(config->vhdx_path, MAX_PATH, g_gw.os_vhdx);
    wcscpy_s(config->storage_root, MAX_PATH, g_gw.root);
    config->ram_mb = GATEWAY_RAM_MB;
    config->hdd_gb = 8;
    config->cpu_cores = 1;
    config->gpu_mode = GPU_NONE;
    config->network_mode = NET_NONE;
    config->is_appliance = TRUE;
    if (provisioning) wcscpy_s(config->resources_iso_path, MAX_PATH, g_gw.seed_iso);

    ZeroMemory(&g_gw.runtime, sizeof(g_gw.runtime));
    g_gw.runtime.unique_id = GATEWAY_UNIQUE_ID;
    wcscpy_s(g_gw.runtime.name, _countof(g_gw.runtime.name), GATEWAY_VM_NAME);
    wcscpy_s(g_gw.runtime.os_type, _countof(g_gw.runtime.os_type), L"Linux");
    wcscpy_s(g_gw.runtime.vhdx_path, MAX_PATH, g_gw.os_vhdx);
    /* The second adapter's MAC comes from share_mac. */
    strcpy_s(g_gw.runtime.share_mac, sizeof(g_gw.runtime.share_mac), GATEWAY_LAN_MAC);
    hcs_destroy_stale(GATEWAY_VM_NAME);
    hr = hcs_create_vm_with_endpoints(config, uplink_guid, lan_guid, &g_gw.runtime);
    HeapFree(GetProcessHeap(), 0, config);
    if (SUCCEEDED(hr)) hr = hcs_start_vm(&g_gw.runtime);
    if (FAILED(hr)) {
        set_error(L"Could not start the gateway VM", hr);
        delete_endpoints();
        return hr;
    }
    swprintf_s(log_path, _countof(log_path), L"%s\\console.log", g_gw.root);
    appliance_start_serial_capture(GATEWAY_VM_NAME, L"vpn gateway", log_path);
    vm_agent_start(&g_gw.runtime);
    hcs_start_monitor(&g_gw.runtime);
    return S_OK;
}

static HRESULT stop_vm(BOOL force)
{
    HRESULT hr = S_OK;
    if (!g_gw.runtime.running) { delete_endpoints(); return S_OK; }
    g_gw.stopping = TRUE;
    hr = force ? hcs_terminate_vm(&g_gw.runtime) : hcs_stop_vm(&g_gw.runtime);
    if (FAILED(hr) && !force) hr = hcs_terminate_vm(&g_gw.runtime);
    if (SUCCEEDED(hr) && !force) {
        ULONGLONG deadline = GetTickCount64() + 30000;
        while (g_gw.runtime.running && GetTickCount64() < deadline) Sleep(100);
        if (g_gw.runtime.running) hr = hcs_terminate_vm(&g_gw.runtime);
    }
    vm_agent_stop(&g_gw.runtime);
    hcs_stop_monitor(&g_gw.runtime);
    hcs_unregister_vm_callback(&g_gw.runtime);
    if (g_gw.runtime.handle) {
        hcs_close_handle_sync(g_gw.runtime.handle);
        g_gw.runtime.handle = NULL;
    }
    delete_endpoints();
    EnterCriticalSection(&g_gw.state);
    g_gw.runtime.running = FALSE;
    g_gw.status.ready = FALSE;
    ZeroMemory(g_gw.slot_config, sizeof(g_gw.slot_config));
    LeaveCriticalSection(&g_gw.state);
    g_gw.stopping = FALSE;
    return hr;
}

/* Wait for the agent, then set up forwarding and the kill switch. */
static HRESULT wait_ready(ULONGLONG timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + timeout_ms, last_note = GetTickCount64();
    char command[64], response[256];
    while (!g_gw.runtime.agent_online) {
        if (!g_gw.runtime.running) return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
        if (GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        if (GetTickCount64() - last_note >= 60000) {
            last_note = GetTickCount64();
            ui_log(L"VPN gateway: waiting for its agent (%I64u s)...",
                   (GetTickCount64() - (deadline - timeout_ms)) / 1000);
        }
        Sleep(500);
    }
    sprintf_s(command, sizeof(command), "vpn_init:%s.0/24", hcn_vpn_subnet_base());
    if (!gateway_send(command, response, sizeof(response), 60000) ||
        strcmp(response, "ok") != 0) {
        ui_log(L"VPN gateway: routing setup failed (%S).", response[0] ? response : "no reply");
        return E_FAIL;
    }
    EnterCriticalSection(&g_gw.state);
    g_gw.status.ready = TRUE;
    LeaveCriticalSection(&g_gw.state);
    return S_OK;
}

static HRESULT ensure_running_locked(void)
{
    HRESULT hr;
    BOOL provisioning;
    if (g_gw.runtime.running && g_gw.status.ready) return S_OK;
    if (g_gw.runtime.running) stop_vm(TRUE);
    EnterCriticalSection(&g_gw.state);
    g_gw.status.busy = TRUE;
    g_gw.status.last_error[0] = L'\0';
    LeaveCriticalSection(&g_gw.state);
    provisioning = !g_gw.status.configured;
    if (provisioning) {
        hr = build_os_disk();
        if (FAILED(hr)) { set_error(L"Gateway setup failed", hr); goto done; }
        set_progress(L"Setting up the gateway (installing WireGuard; a few minutes)...");
    } else {
        set_progress(L"Starting...");
    }
    hr = start_vm(provisioning);
    if (FAILED(hr)) goto done;
    hr = wait_ready(provisioning ? GATEWAY_SETUP_TIMEOUT_MS : GATEWAY_START_TIMEOUT_MS);
    if (FAILED(hr)) {
        set_error(provisioning ? L"Gateway setup did not finish" : L"Gateway did not come up", hr);
        stop_vm(TRUE);
        goto done;
    }
    if (provisioning) {
        EnterCriticalSection(&g_gw.state);
        g_gw.status.configured = TRUE;
        LeaveCriticalSection(&g_gw.state);
        save_state();
    }
    set_progress(L"Running");
done:
    EnterCriticalSection(&g_gw.state);
    g_gw.status.busy = FALSE;
    LeaveCriticalSection(&g_gw.state);
    return hr;
}

/* ---- Tunnels ---- */

/* Find the slot running config_id, loading it into a free slot if needed. */
static HRESULT ensure_tunnel_locked(const wchar_t *config_id, int *slot_out)
{
    char conf[2048], command[3200], response[256];
    char *b64 = NULL;
    DWORD b64_len = 0;
    int slot, free_slot = -1;
    HRESULT hr;
    AsbVpnConfigInfo info;

    for (slot = 0; slot < GATEWAY_SLOTS; slot++) {
        if (_wcsicmp(g_gw.slot_config[slot], config_id) == 0) { *slot_out = slot; return S_OK; }
        if (!g_gw.slot_config[slot][0] && free_slot < 0) free_slot = slot;
    }
    if (free_slot < 0) return HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
    if (!vpn_configs_find(config_id, &info)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    hr = vpn_configs_gateway_conf(config_id, conf, sizeof(conf));
    if (FAILED(hr)) return hr;
    if (!CryptBinaryToStringA((const BYTE *)conf, (DWORD)strlen(conf),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64_len) ||
        !(b64 = (char *)HeapAlloc(GetProcessHeap(), 0, b64_len)) ||
        !CryptBinaryToStringA((const BYTE *)conf, (DWORD)strlen(conf),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64_len)) {
        hr = b64 ? HRESULT_FROM_WIN32(GetLastError()) : E_OUTOFMEMORY;
        goto done;
    }
    sprintf_s(command, sizeof(command), "vpn_set:%d:%s", free_slot, b64);
    ui_log(L"VPN gateway: bringing up \"%s\" (%s)...", info.name, info.endpoint);
    if (!gateway_send(command, response, sizeof(response), 60000)) {
        hr = E_FAIL;
        ui_log(L"VPN gateway: tunnel \"%s\" could not be started (no reply).", info.name);
        goto done;
    }
    if (strcmp(response, "ok") == 0) {
        ui_log(L"VPN gateway: tunnel \"%s\" is up.", info.name);
    } else if (strcmp(response, "ok:no_handshake") == 0) {
        ui_log(L"VPN gateway: tunnel \"%s\" is configured but its server has not answered yet; "
               L"VMs using it stay offline until it does.", info.name);
    } else {
        hr = E_FAIL;
        ui_log(L"VPN gateway: tunnel \"%s\" failed to start (%S).", info.name, response);
        goto done;
    }
    wcscpy_s(g_gw.slot_config[free_slot], ASB_VPN_ID_CHARS, config_id);
    *slot_out = free_slot;
    hr = S_OK;
done:
    SecureZeroMemory(conf, sizeof(conf));
    SecureZeroMemory(command, sizeof(command));
    if (b64) { SecureZeroMemory(b64, b64_len); HeapFree(GetProcessHeap(), 0, b64); }
    return hr;
}

static HRESULT bind_locked(VmInstance *vm)
{
    char command[64], response[256];
    int slot;
    HRESULT hr = ensure_tunnel_locked(vm->vpn_id, &slot);
    if (FAILED(hr)) return hr;
    sprintf_s(command, sizeof(command), "vpn_bind:%d:%s", slot, vm->vpn_ip);
    if (!gateway_send(command, response, sizeof(response), 30000) || strcmp(response, "ok") != 0) {
        ui_log(L"VPN gateway: routing \"%s\" into its tunnel failed (%S).",
               vm->name, response[0] ? response : "no reply");
        return E_FAIL;
    }
    return S_OK;
}

/* After a (re)start the gateway has no tunnels or rules: restore those of
   every VPN VM that is already running. */
static void restore_bindings_locked(const VmInstance *except)
{
    int i;
    for (i = 0; i < asb_vm_count(); i++) {
        VmInstance *vm = asb_vm_instance(asb_vm_get(i));
        if (!vm || vm == except || !vm->running || vm->network_mode != NET_VPN || !vm->vpn_ip[0])
            continue;
        if (FAILED(bind_locked(vm)))
            ui_log(L"VPN gateway: \"%s\" stays offline until it is restarted.", vm->name);
    }
}

static BOOL vpn_ip_in_use(const VmInstance *self, const char *ip)
{
    int i;
    if (strcmp(ip, vpn_gateway_lan_ip()) == 0) return TRUE;
    for (i = 0; i < asb_vm_count(); i++) {
        VmInstance *other = asb_vm_instance(asb_vm_get(i));
        if (other && other != self && other->vpn_ip[0] && strcmp(other->vpn_ip, ip) == 0)
            return TRUE;
    }
    return FALSE;
}

HRESULT vpn_gateway_prepare_client(VmInstance *vm, GUID *network_id, GUID *endpoint_id,
                                   wchar_t *endpoint_guid, size_t endpoint_guid_chars)
{
    AsbVpnConfigInfo info;
    unsigned int hash = 2166136261u;
    const wchar_t *p;
    int octet, attempts;
    char mac[32], actual_mac[32] = "";
    HRESULT hr;

    if (!vm || !network_id || !endpoint_id || !endpoint_guid) return E_INVALIDARG;
    endpoint_guid[0] = L'\0';
    if (!g_gw.initialized) vpn_gateway_init();
    if (!vpn_configs_find(vm->vpn_id, &info)) {
        ui_log(L"VPN: \"%s\" has no VPN config selected, or its config was deleted.", vm->name);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    InterlockedIncrement(&g_gw.idle_generation);
    EnterCriticalSection(&g_gw.lifecycle);
    hr = ensure_running_locked();
    if (FAILED(hr)) goto done;

    for (p = vm->name; *p; ++p) { hash ^= towlower(*p); hash *= 16777619u; }
    octet = 10 + (int)(hash % 240u);
    for (attempts = 0; attempts < 240; ++attempts) {
        sprintf_s(vm->vpn_ip, sizeof(vm->vpn_ip), "%s.%d", hcn_vpn_subnet_base(), octet);
        if (!vpn_ip_in_use(vm, vm->vpn_ip)) break;
        if (++octet > 249) octet = 10;
    }
    if (attempts == 240) { vm->vpn_ip[0] = '\0'; hr = HRESULT_FROM_WIN32(ERROR_ADDRESS_ALREADY_ASSOCIATED); goto done; }
    sprintf_s(mac, sizeof(mac), "02-15-5E-%02X-%02X-%02X",
              (hash >> 16) & 0xff, (hash >> 8) & 0xff, hash & 0xff);
    hr = hcn_create_vpn_network(network_id);
    if (SUCCEEDED(hr))
        hr = hcn_create_static_endpoint(network_id, endpoint_id, endpoint_guid,
                                        endpoint_guid_chars, vm->vpn_ip, mac);
    if (FAILED(hr)) { vm->vpn_ip[0] = '\0'; goto done; }
    if (SUCCEEDED(hcn_get_endpoint_mac(endpoint_id, actual_mac, sizeof(actual_mac))) && actual_mac[0])
        strcpy_s(vm->vpn_mac, sizeof(vm->vpn_mac), actual_mac);
    else
        strcpy_s(vm->vpn_mac, sizeof(vm->vpn_mac), mac);

    hr = bind_locked(vm);
    if (FAILED(hr)) {
        hcn_delete_endpoint(endpoint_id);
        endpoint_guid[0] = L'\0';
        vm->vpn_ip[0] = '\0';
        goto done;
    }
    restore_bindings_locked(vm);
    EnterCriticalSection(&g_gw.state);
    g_gw.status.active_clients++;
    LeaveCriticalSection(&g_gw.state);
    ui_log(L"VPN: \"%s\" routes through \"%s\" at %S.", vm->name, info.name, vm->vpn_ip);
done:
    LeaveCriticalSection(&g_gw.lifecycle);
    InterlockedIncrement(&g_gw.idle_generation);
    return hr;
}

static DWORD WINAPI idle_stop_thread(LPVOID parameter)
{
    LONG generation = (LONG)(LONG_PTR)parameter;
    ULONGLONG deadline = GetTickCount64() + GATEWAY_IDLE_MS;
    while (GetTickCount64() < deadline) {
        if (generation != g_gw.idle_generation) return 0;
        Sleep(1000);
    }
    EnterCriticalSection(&g_gw.lifecycle);
    if (generation == g_gw.idle_generation && g_gw.status.active_clients == 0 &&
        g_gw.runtime.running) {
        ui_log(L"VPN gateway: idle, stopping.");
        stop_vm(FALSE);
        set_progress(L"Stopped");
    }
    LeaveCriticalSection(&g_gw.lifecycle);
    return 0;
}

void vpn_gateway_release_client(VmInstance *vm)
{
    char command[64], response[256];
    LONG generation;
    HANDLE thread;
    if (!vm || !vm->vpn_ip[0] || !g_gw.initialized) return;
    if (g_gw.runtime.running && g_gw.runtime.agent_online) {
        sprintf_s(command, sizeof(command), "vpn_unbind:%s", vm->vpn_ip);
        gateway_send(command, response, sizeof(response), 15000);
    }
    vm->vpn_ip[0] = '\0';
    vm->vpn_mac[0] = '\0';
    EnterCriticalSection(&g_gw.state);
    if (g_gw.status.active_clients > 0) g_gw.status.active_clients--;
    LeaveCriticalSection(&g_gw.state);
    generation = InterlockedIncrement(&g_gw.idle_generation);
    if (g_gw.status.active_clients == 0) {
        thread = CreateThread(NULL, 0, idle_stop_thread, (LPVOID)(LONG_PTR)generation, 0, NULL);
        if (thread) CloseHandle(thread);
    }
}

void vpn_gateway_client_dns(const VmInstance *vm, char *out, size_t out_size)
{
    AsbVpnConfigInfo info;
    if (!out || !out_size) return;
    strcpy_s(out, out_size, GATEWAY_FALLBACK_DNS);
    if (vm && vpn_configs_find(vm->vpn_id, &info) && info.dns[0])
        WideCharToMultiByte(CP_UTF8, 0, info.dns, -1, out, (int)out_size, NULL, NULL);
}

void vpn_gateway_unload_config(const wchar_t *config_id)
{
    char command[32], response[256];
    int slot;
    if (!g_gw.initialized || !config_id) return;
    EnterCriticalSection(&g_gw.lifecycle);
    for (slot = 0; slot < GATEWAY_SLOTS; slot++) {
        if (_wcsicmp(g_gw.slot_config[slot], config_id) != 0) continue;
        if (g_gw.runtime.running && g_gw.runtime.agent_online) {
            sprintf_s(command, sizeof(command), "vpn_down:%d", slot);
            gateway_send(command, response, sizeof(response), 30000);
        }
        g_gw.slot_config[slot][0] = L'\0';
    }
    LeaveCriticalSection(&g_gw.lifecycle);
}

HRESULT vpn_gateway_stop(void)
{
    HRESULT hr;
    if (!g_gw.initialized) return S_OK;
    EnterCriticalSection(&g_gw.lifecycle);
    hr = stop_vm(FALSE);
    set_progress(g_gw.status.configured ? L"Stopped" : L"Not set up");
    LeaveCriticalSection(&g_gw.lifecycle);
    return hr;
}

void vpn_gateway_cleanup(void)
{
    if (!g_gw.initialized) return;
    stop_vm(FALSE);
}

/* The gateway exited on its own (crash, guest shutdown). Its VMs lose their
   route, which is the kill switch working; the next VPN VM start brings it
   back and restores every running VM's binding. */
BOOL vpn_gateway_handle_hcs_state(VmInstance *instance, DWORD event)
{
    if (!g_gw.initialized || instance != &g_gw.runtime) return FALSE;
    if (event == 0x00000001 || event == 0x00000002 || event == 0x00000100) {
        vm_agent_stop(instance);
        hcs_stop_monitor(instance);
        instance->running = FALSE;
        hcs_unregister_vm_callback(instance);
        if (instance->handle) {
            hcs_close_handle_sync(instance->handle);
            instance->handle = NULL;
        }
        if (!g_gw.stopping) {
            delete_endpoints();
            ui_log(L"VPN gateway: stopped unexpectedly; VPN VMs are offline until it restarts.");
        }
        EnterCriticalSection(&g_gw.state);
        g_gw.status.ready = FALSE;
        ZeroMemory(g_gw.slot_config, sizeof(g_gw.slot_config));
        LeaveCriticalSection(&g_gw.state);
    }
    return TRUE;
}

VmInstance *vpn_gateway_instance_by_id(UINT64 id)
{
    return g_gw.initialized && id == GATEWAY_UNIQUE_ID ? &g_gw.runtime : NULL;
}
