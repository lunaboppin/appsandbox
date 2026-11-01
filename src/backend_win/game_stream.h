#ifndef GAME_STREAM_H
#define GAME_STREAM_H

#include <stdio.h>
#include "asb_core.h"

/*
 * game_stream -- optional: domain users start games from a web dashboard.
 *
 * An admin nominates a Windows VM. Building an image copies its disk, so the VM stays
 * free to run, scans the copy for games and generalizes it with sysprep. Each session
 * then runs a throwaway duplicate on a differencing disk over that image: it joins the
 * domain, signs its user in once and opens what they picked. The duplicate and its AD
 * computer account are deleted when the session ends.
 *
 * What protects it:
 *  - Off until set up, and only for the AppSandbox domain's users (optionally one group).
 *  - The appliance checks the password. The host keeps it, encrypted in memory, only
 *    long enough to start a session (GS_PASSWORD_KEEP_MS), and it reaches the guest as
 *    an LSA secret that the agent deletes once the user is signed in.
 *  - The dashboard uses the browser gateway's listeners and checks (vm_web.c): network
 *    access is HTTPS only, and only when the admin chose it.
 *  - A duplicate's image holds no pairing or Sunshine password of the source VM.
 */

/* ---- Settings (vms.cfg [Settings]) ---- */

BOOL game_stream_has_settings(void);
void game_stream_write_settings(FILE *f);
BOOL game_stream_read_setting(const wchar_t *line);

/* ---- Lifecycle ---- */

void game_stream_init(void);       /* after the VM list has loaded */
void game_stream_shutdown(void);   /* app exit: every session ends */

/* ---- Admin: the setup dialog and the API ---- */

/* Everything the dialog shows, as JSON. */
ASB_API int game_stream_status_json(char *out, int cap);

/* Turn it on or change its settings. err gets the reason on failure. */
ASB_API HRESULT game_stream_configure(const wchar_t *vm_name, int max_sessions, int idle_minutes,
                                      const wchar_t *group, BOOL lan, wchar_t *err, size_t err_chars);

/* Build or refresh the image, on a worker; progress shows in the status. */
ASB_API HRESULT game_stream_build_image(wchar_t *err, size_t err_chars);

/* End one session by its VM's name, or turn the feature off and delete its image. */
ASB_API HRESULT game_stream_end_session_of_vm(const wchar_t *vm_name);
ASB_API HRESULT game_stream_turn_off(void);

/* ---- The dashboard (vm_web.c) ---- */

BOOL game_stream_dashboard_lan(void);

/* Check a sign-in. token gets the session cookie's value. */
HRESULT game_stream_sign_in(const wchar_t *user, const wchar_t *password, BOOL tls,
                            char *token, size_t token_chars, char *message, int message_cap);
void game_stream_sign_out(const char *token);
/* The user behind a cookie value, or FALSE. */
BOOL game_stream_signed_in(const char *token, BOOL tls, wchar_t *user, size_t user_chars);

/* The catalog file or a game's picture, by name. FALSE for any other name. */
BOOL game_stream_catalog_file(const char *leaf, wchar_t *path, size_t path_chars);

/* This user's session, as JSON: {"state":"none"} when there isn't one. */
int game_stream_session_json(const wchar_t *user, char *out, int cap);

/* Start this user's session. password is the one to use when the kept one has lapsed. */
HRESULT game_stream_session_start(const char *token, const wchar_t *user, const char *mode, const char *item,
                                  const wchar_t *password, char *message, int message_cap);
HRESULT game_stream_session_end(const wchar_t *user);
HRESULT game_stream_session_pair(const wchar_t *user, const char *pin, char *message, int message_cap);
/* The VM of this user's ready session, or 0. */
UINT64 game_stream_session_vm(const wchar_t *user, char *name, size_t name_chars);

#endif /* GAME_STREAM_H */
