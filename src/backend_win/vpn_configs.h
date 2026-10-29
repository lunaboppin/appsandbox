#ifndef APPSANDBOX_VPN_CONFIGS_H
#define APPSANDBOX_VPN_CONFIGS_H

#include <windows.h>

#define ASB_MAX_VPN_CONFIGS 16
#define ASB_VPN_ID_CHARS    40

/* Non-secret summary of a stored WireGuard config. The private and preshared
   keys live only in the DPAPI-protected file and are never handed out. */
typedef struct {
    wchar_t id[ASB_VPN_ID_CHARS];
    wchar_t name[64];
    wchar_t endpoint[128];   /* Peer Endpoint, host:port */
    wchar_t address[64];     /* Interface Address (IPv4 only) */
    wchar_t dns[128];        /* comma-separated IPv4 DNS servers; may be empty */
} AsbVpnConfigInfo;

void vpn_configs_init(void);
int vpn_configs_count(void);
BOOL vpn_configs_get(int index, AsbVpnConfigInfo *out);
BOOL vpn_configs_find(const wchar_t *id, AsbVpnConfigInfo *out);

/* Parse a WireGuard .conf (the text of the file, as imported or pasted) and
   store it under a new id. Only IPv4 is kept: the VPN network is IPv4-only.
   E_INVALIDARG with a reason in error (may be NULL) when the text is not a
   usable config. */
HRESULT vpn_configs_import(const wchar_t *name, const wchar_t *text,
                           wchar_t *id_out, size_t id_chars,
                           wchar_t *error, size_t error_chars);
HRESULT vpn_configs_delete(const wchar_t *id);

/* The config as the gateway runs it, UTF-8: Table = off, AllowedIPs
   0.0.0.0/0, no DNS line (the guest uses the DNS servers itself). The
   caller must SecureZeroMemory it. */
HRESULT vpn_configs_gateway_conf(const wchar_t *id, char *out, size_t out_size);

#endif
