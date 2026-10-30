#ifndef UPDATER_H
#define UPDATER_H

#include <windows.h>

/* Finds, downloads and verifies App Sandbox updates from GitHub Releases.
   No UI and no dependency on the core DLL: update_ui.c drives it from the app,
   and tools\updater-test exercises it against a mock release feed. */

#define UPDATER_ERR_CHARS 256

/* An installer can never be bigger than this; anything larger is refused. */
#define UPDATER_MAX_INSTALLER_BYTES (1024ULL * 1024 * 1024)

typedef struct {
    const wchar_t *api_url;        /* releases listing */
    const wchar_t *asset_prefix;   /* a download URL must start with this */
    const wchar_t *release_prefix; /* a release page URL must start with this */
} UpdateEndpoint;

typedef struct {
    wchar_t   version[32];     /* from the asset name, e.g. 0.1.7.0 */
    wchar_t   tag[128];
    wchar_t   asset_name[128];
    wchar_t   asset_url[1024];
    wchar_t   release_url[512];
    wchar_t   sha256[65];      /* GitHub's digest for the asset, lowercase hex */
    ULONGLONG size;
    BOOL      prerelease;
} UpdateInfo;

typedef enum {
    UPDATE_CHECK_ERROR = -1,
    UPDATE_CHECK_NONE = 0,
    UPDATE_CHECK_AVAILABLE = 1
} UpdateCheckResult;

typedef void (*UpdaterProgressFn)(int percent, void *ctx);

/* This build's version, from Directory.Build.props. */
const wchar_t *updater_current_version(void);

/* Parse "a.b.c" or "a.b.c.d" (digits only, each part <= 65535). */
BOOL updater_parse_version(const wchar_t *text, WORD out[4]);

/* -1, 0 or 1. An unparsable version compares lower than any valid one. */
int updater_version_compare(const wchar_t *a, const wchar_t *b);

/* Pick the newest usable release newer than current from a GitHub releases
   listing. Skips drafts, pre-releases (unless asked for), and any release whose
   installer asset is missing, misnamed, unhashed or hosted outside the endpoint. */
UpdateCheckResult updater_select_release(const char *json, size_t len,
                                         const wchar_t *current, BOOL include_prerelease,
                                         const UpdateEndpoint *endpoint,
                                         UpdateInfo *out, wchar_t *err, size_t err_chars);

/* Fetch the listing and select from it. */
UpdateCheckResult updater_check(const UpdateEndpoint *endpoint, const wchar_t *current,
                                BOOL include_prerelease, UpdateInfo *out,
                                wchar_t *err, size_t err_chars);

/* Download info->asset_url to dest (through dest.part), enforcing info->size.
   cancel may be NULL; set it non-zero from another thread to stop. */
BOOL updater_download(const UpdateInfo *info, const wchar_t *dest,
                      UpdaterProgressFn progress, void *ctx, volatile LONG *cancel,
                      wchar_t *err, size_t err_chars);

/* Verify a downloaded installer: SHA-256 equals expected_sha256, the Authenticode
   signature is intact and made by one of the pinned certificates, and its file
   version equals version. On success *held (optional) receives a handle that
   denies writers, so the verified bytes are the ones launched; the caller closes it. */
BOOL updater_verify_installer(const wchar_t *path, const wchar_t *expected_sha256,
                              const wchar_t *version,
                              const wchar_t *const *pins, int pin_count,
                              HANDLE *held, wchar_t *err, size_t err_chars);

/* The SHA-256 of the certificate that signed path (uppercase hex) and
   WinVerifyTrust's raw status. Returns FALSE when no signer could be read. */
BOOL updater_read_signer(const wchar_t *path, HANDLE file, wchar_t out[65], LONG *status);

/* The installed version's folder, from the installer's uninstall key. */
BOOL updater_install_location(wchar_t *out, size_t out_chars);

/* TRUE when this process runs from the folder the installer put it in. A build
   tree or an unzipped copy must not be replaced by an install somewhere else. */
BOOL updater_is_installed(void);

/* %ProgramData%\AppSandbox\updates, created or re-locked so that only SYSTEM and
   Administrators can touch it: an elevated app runs what lands here. */
BOOL updater_updates_dir(wchar_t *out, size_t out_chars);

/* Delete leftover installers. Running again means the last one finished. */
void updater_clean_updates_dir(void);

/* Start the verified installer for an unattended upgrade. It waits for this
   process to exit and reopens the app whether or not it succeeds. */
BOOL updater_launch_installer(const wchar_t *path, wchar_t *err, size_t err_chars);

void updater_load_prefs(BOOL *auto_check, BOOL *include_prerelease);
void updater_save_prefs(BOOL auto_check, BOOL include_prerelease);

#endif /* UPDATER_H */
