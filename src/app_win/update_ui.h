#ifndef UPDATE_UI_H
#define UPDATE_UI_H

#include <windows.h>

/* The app's side of auto-update: background checks, the update state the web UI
   shows, and holding an update back while VMs are running. UI thread only. */

#define WM_UPDATE_EVENT     (WM_APP + 30)
#define ASB_UPDATE_TIMER_ID 0x5550

/* The web UI has loaded: send the state and arm the background check. */
void update_ui_on_ready(HWND hwnd);

/* Handle a web UI action. Returns TRUE if it was an update action. */
BOOL update_ui_handle_action(const wchar_t *action, const wchar_t *json);

/* WM_UPDATE_EVENT, posted by the worker threads. */
void update_ui_on_event(WPARAM wp, LPARAM lp);

/* WM_TIMER with ASB_UPDATE_TIMER_ID. */
void update_ui_on_timer(void);

/* A VM started, stopped or was removed: a waiting update may now go ahead. */
void update_ui_on_vm_state(void);

#endif /* UPDATE_UI_H */
