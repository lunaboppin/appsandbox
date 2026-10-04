/*
 * input-gamepad.c — host game controllers as virtual Xbox 360 pads in the
 * guest. Part of appsandbox-input.exe.
 *
 * The host polls XInput and sends INPUT_GAMEPAD packets (one type per
 * controller slot); each slot here becomes a ViGEmBus Xbox 360 target, so
 * every XInput / DirectInput / Windows.Gaming.Input game sees a real pad.
 * Rumble from the game goes back to the host as INPUT_GUEST_RUMBLE.
 *
 * ViGEmBus is installed into the guest by the agent (ViGEmBus_Setup.exe in
 * C:\Windows\AppSandbox). Until it is, connecting fails and is retried
 * every few seconds while controller input arrives.
 */

#include <winsock2.h>
#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4201) /* nameless struct/union in the ViGEm headers */
#include "ViGEm/Client.h"
#pragma warning(pop)

#pragma comment(lib, "setupapi.lib")

#define PAD_MAX             4
#define VIGEM_RETRY_MS      5000

void input_log(const char *fmt, ...);                                   /* appsandbox-input.c */
void gamepad_send_rumble(UINT32 index, UINT32 large_motor, UINT32 small_motor);     /* appsandbox-input.c */

static CRITICAL_SECTION g_pad_cs;
static PVIGEM_CLIENT    g_vigem;
static DWORD            g_vigem_last_try;
static BOOL             g_vigem_fail_logged;
static PVIGEM_TARGET    g_pads[PAD_MAX];
static const void      *g_owner[PAD_MAX];   /* connection that last fed each pad */

static VOID CALLBACK on_x360_notification(PVIGEM_CLIENT client, PVIGEM_TARGET target,
                                          UCHAR large_motor, UCHAR small_motor, UCHAR led,
                                          LPVOID user)
{
    (void)client; (void)target; (void)led;
    gamepad_send_rumble((UINT32)(UINT_PTR)user, large_motor, small_motor);
}

void gamepad_init(void)
{
    InitializeCriticalSection(&g_pad_cs);
}

/* Connect to ViGEmBus if not yet connected. Caller holds g_pad_cs. */
static BOOL ensure_vigem(void)
{
    DWORD now;
    VIGEM_ERROR err;

    if (g_vigem)
        return TRUE;
    now = GetTickCount();
    if (g_vigem_last_try && (DWORD)(now - g_vigem_last_try) < VIGEM_RETRY_MS)
        return FALSE;
    g_vigem_last_try = now;

    g_vigem = vigem_alloc();
    if (!g_vigem)
        return FALSE;
    err = vigem_connect(g_vigem);
    if (!VIGEM_SUCCESS(err)) {
        vigem_free(g_vigem);
        g_vigem = NULL;
        if (!g_vigem_fail_logged) {
            g_vigem_fail_logged = TRUE;
            input_log("Gamepad: ViGEmBus is not available (0x%08X); controllers are ignored until it is installed.",
                      (unsigned)err);
        }
        return FALSE;
    }
    input_log("Gamepad: connected to ViGEmBus.");
    return TRUE;
}

static void remove_pad(UINT32 i)
{
    if (!g_pads[i])
        return;
    vigem_target_x360_unregister_notification(g_pads[i]);
    vigem_target_remove(g_vigem, g_pads[i]);
    vigem_target_free(g_pads[i]);
    g_pads[i] = NULL;
    g_owner[i] = NULL;
    input_log("Gamepad %u: unplugged.", i);
}

/* p1 = buttons | LT << 16 | RT << 24, p2 = LX | LY << 16, p3 = RX | RY << 16
   (the XINPUT_GAMEPAD layout). */
void gamepad_update(const void *owner, UINT32 index, UINT32 p1, UINT32 p2, UINT32 p3)
{
    XUSB_REPORT r;

    if (index >= PAD_MAX)
        return;
    EnterCriticalSection(&g_pad_cs);
    if (!ensure_vigem())
        goto out;
    if (!g_pads[index]) {
        VIGEM_ERROR err;
        g_pads[index] = vigem_target_x360_alloc();
        if (!g_pads[index])
            goto out;
        err = vigem_target_add(g_vigem, g_pads[index]);
        if (!VIGEM_SUCCESS(err)) {
            input_log("Gamepad %u: plugging in failed (0x%08X).", index, (unsigned)err);
            vigem_target_free(g_pads[index]);
            g_pads[index] = NULL;
            goto out;
        }
        vigem_target_x360_register_notification(g_vigem, g_pads[index],
                                                on_x360_notification,
                                                (LPVOID)(UINT_PTR)index);
        input_log("Gamepad %u: plugged in.", index);
    }
    g_owner[index] = owner;

    ZeroMemory(&r, sizeof(r));
    r.wButtons      = (USHORT)(p1 & 0xFFFF);
    r.bLeftTrigger  = (BYTE)((p1 >> 16) & 0xFF);
    r.bRightTrigger = (BYTE)(p1 >> 24);
    r.sThumbLX      = (SHORT)(p2 & 0xFFFF);
    r.sThumbLY      = (SHORT)(p2 >> 16);
    r.sThumbRX      = (SHORT)(p3 & 0xFFFF);
    r.sThumbRY      = (SHORT)(p3 >> 16);
    vigem_target_x360_update(g_vigem, g_pads[index], r);
out:
    LeaveCriticalSection(&g_pad_cs);
}

void gamepad_remove(UINT32 index)
{
    if (index >= PAD_MAX)
        return;
    EnterCriticalSection(&g_pad_cs);
    remove_pad(index);
    LeaveCriticalSection(&g_pad_cs);
}

/* A host connection went away: unplug the pads it was feeding (not ones a
   newer connection has taken over), so nothing stays held in a game. */
void gamepad_release_owner(const void *owner)
{
    UINT32 i;
    EnterCriticalSection(&g_pad_cs);
    for (i = 0; i < PAD_MAX; i++)
        if (g_pads[i] && g_owner[i] == owner)
            remove_pad(i);
    LeaveCriticalSection(&g_pad_cs);
}
