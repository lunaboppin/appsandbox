#ifndef APPSANDBOX_VPN_GATEWAY_H
#define APPSANDBOX_VPN_GATEWAY_H

#include <windows.h>
#include "hcs_vm.h"

/* The VPN gateway: a hidden Ubuntu VM that runs every WireGuard tunnel and
   is the only way off the Private VPN network. It is set up on first use,
   started when a VPN VM starts and stopped once no VPN VM has used it for a
   few minutes. */

typedef struct {
    BOOL configured;      /* OS disk provisioned */
    BOOL running;
    BOOL ready;           /* agent online and routing initialised */
    BOOL busy;            /* setting up or starting */
    int  active_clients;  /* running VMs routed through it */
    wchar_t progress_text[128];
    wchar_t last_error[256];
} VpnGatewayStatus;

void vpn_gateway_init(void);
void vpn_gateway_cleanup(void);
void vpn_gateway_get_status(VpnGatewayStatus *out);

/* Start (and on first use set up) the gateway, load vm->vpn_id's tunnel,
   give the VM an address on the VPN network and create its endpoint there,
   routed into that tunnel. Fills network_id/endpoint_id and the endpoint's
   GUID string for the VM document; vm->vpn_ip and vm->vpn_mac are set.
   Runs for minutes on first use: call from VM start threads only. */
HRESULT vpn_gateway_prepare_client(VmInstance *vm, GUID *network_id, GUID *endpoint_id,
                                   wchar_t *endpoint_guid, size_t endpoint_guid_chars);
/* Unbind a stopped VM's address. The endpoint itself is deleted with the
   VM's other network state. */
void vpn_gateway_release_client(VmInstance *vm);

/* Comma-separated DNS servers for a VPN VM (the config's, or a public
   resolver reached through the tunnel when it names none), and the gateway
   address it routes through. */
void vpn_gateway_client_dns(const VmInstance *vm, char *out, size_t out_size);
const char *vpn_gateway_lan_ip(void);

/* Drop a config's tunnel from the running gateway (after it is deleted). */
void vpn_gateway_unload_config(const wchar_t *config_id);
HRESULT vpn_gateway_stop(void);
/* Set the gateway up now (if it isn't) so the first VPN VM need not wait. */
void vpn_gateway_setup_in_background(void);

BOOL vpn_gateway_handle_hcs_state(VmInstance *instance, DWORD event);
VmInstance *vpn_gateway_instance_by_id(UINT64 id);

#endif
