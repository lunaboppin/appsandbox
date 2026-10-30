#ifndef VM_BACKSTAGE_H
#define VM_BACKSTAGE_H

#include <winsock2.h>
#include <windows.h>
#include "hcs_vm.h"

/*
 * vm_backstage -- the host's Backstage window for a Windows VM.
 *
 * Backstage (tools/agent/appsandbox-backstage.c) is a SYSTEM shell in the
 * guest's session 0, whose screen the VM itself never shows. This window
 * asks the agent to start it, connects to its channel (:0008), draws the
 * bands of screen it sends and sends mouse and keyboard back. One window
 * per VM; opening it again brings it to the front. Closing the window
 * leaves the session running, like ScreenConnect; "End Backstage session"
 * in its title-bar menu closes it in the VM.
 */
void vm_backstage_open(VmInstance *vm, HINSTANCE hinst);

#endif
