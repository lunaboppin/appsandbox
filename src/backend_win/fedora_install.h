#ifndef FEDORA_INSTALL_H
#define FEDORA_INSTALL_H

#include <windows.h>

extern const wchar_t *const FEDORA_NETINST_URLS[];
extern const int FEDORA_NETINST_URL_COUNT;
extern const wchar_t FEDORA_NETINST_NAME[];
extern const wchar_t FEDORA_NETINST_SHA256[];

typedef struct {
    char release[16];    /* "44", from the ISO's .discinfo */
    char hostname[64];
    char locale[64];
    char keyboard[32];
    char tz[64];
    /* Empty static_ip means DHCP. NAT networks have no DHCP server. */
    char static_ip[64];
    char gateway[64];
    char mac[32];        /* colon-separated, empty for "the adapter with a link" */
} FedoraKickstart;

BOOL fedora_kickstart_write(const wchar_t *path, const FedoraKickstart *ks);

#endif
