#ifndef VM_WEB_H
#define VM_WEB_H

#include "hcs_vm.h"

/*
 * vm_web -- use a VM from a web browser.
 *
 * A small web server in the app. Video comes from the same IDD frame channel as the
 * display window, encoded here to H.264 and decoded in the browser with WebCodecs;
 * audio, input and clipboard use the guest helpers' existing channels. All of it
 * travels on one WebSocket.
 *
 * What protects it:
 *  - Off for every VM until turned on, and off again when the app exits. Each turn-on
 *    makes a new random 256-bit access key, which only the app and the API can read.
 *  - The key is exchanged for an HttpOnly, SameSite=Strict session cookie tied to one
 *    VM. Failed logins are limited per address. Host and Origin are checked on every
 *    request, against DNS rebinding and other sites opening the WebSocket.
 *  - It listens on 127.0.0.1 over HTTP (browsers treat that as a secure context).
 *    Exposing a VM to the network is its own choice: that adds an HTTPS-only listener,
 *    with a self-signed certificate whose fingerprint the app shows, and a firewall
 *    rule for private and domain networks only.
 *  - Malware lab VMs are refused.
 */

/* Why this VM can't be used from a browser, in words for the user, or NULL. */
ASB_API const wchar_t *vm_web_unsupported_reason(VmInstance *vm);

/* Turn browser access on (new key each time) or off. lan = TRUE also allows the
   network. err gets the reason on failure. */
ASB_API HRESULT vm_web_set(VmInstance *vm, BOOL enabled, BOOL lan, wchar_t *err, size_t err_chars);

/* State as JSON: enabled, lan, url, lanUrls, fingerprint, reach, connected. */
ASB_API int vm_web_status_json(VmInstance *vm, char *out, int cap);

/* Close every listener and session (app exit). */
ASB_API void vm_web_shutdown(void);

#endif /* VM_WEB_H */
