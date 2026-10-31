#ifndef VM_STREAM_H
#define VM_STREAM_H

#include "hcs_vm.h"

/*
 * vm_stream -- Sunshine in the guest, reachable by Moonlight clients.
 *
 * The agent installs and configures Sunshine (tools/agent/agent.c). A NAT VM's
 * address exists only on this PC, so a relay here forwards Moonlight's ports to it,
 * on loopback or on every interface: that choice is who can reach the VM. A bridged
 * VM has its own address on the local network and needs no relay. Off by default.
 */

#define ASB_STREAM_THIS_PC  1   /* relay on 127.0.0.1 */
#define ASB_STREAM_NETWORK  2   /* relay on every interface, plus a firewall rule */

/* Why this VM can't stream, in words for the user, or NULL if it can. */
ASB_API const wchar_t *vm_stream_unsupported_reason(VmInstance *vm);

/* Turn streaming on or off and save it. On a running VM it applies now, otherwise
   at the next start. Errors with a reason go in err. */
ASB_API HRESULT vm_stream_configure(VmInstance *vm, BOOL enabled, int exposure,
                                    BOOL software_encoder, wchar_t *err, size_t err_chars);

/* The VM's streaming state as JSON, including what the guest reports (which blocks
   for a moment while the guest answers). Returns the length written. */
ASB_API int vm_stream_status_json(VmInstance *vm, char *out, int cap);

/* Finish pairing: pin is the 4 digits Moonlight shows. message gets the reason on
   failure. */
ASB_API HRESULT vm_stream_pair(VmInstance *vm, const char *pin, const char *client_name,
                               char *message, int message_cap);

/* Forget every paired client. */
ASB_API HRESULT vm_stream_unpair_all(VmInstance *vm, char *message, int message_cap);

/* For the agent connection: the command that sets the guest up. */
void vm_stream_setup_command(VmInstance *vm, char *cmd, int cap);

void vm_stream_relay_start(VmInstance *vm);
void vm_stream_relay_stop(VmInstance *vm);

/* All of the local addresses a client could use, as JSON strings "a","b". */
int vm_stream_append_host_addresses(char *out, int cap, int pos);

#endif /* VM_STREAM_H */
