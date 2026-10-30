/*
 * updater-test.exe -- Drives src\app_win\updater.c from the command line, without
 * the elevation the app needs, so test_updater_mock.py can run it against a mock
 * release feed and against signed and tampered files. Not shipped in the installer.
 *
 *   updater-test check [--api URL --asset-prefix URL --release-prefix URL]
 *                      [--current VER] [--prerelease]
 *   updater-test download --url URL --size BYTES --dest PATH
 *   updater-test verify --file PATH --sha256 HEX --version VER --pin HEX [--pin HEX]
 *   updater-test signer --file PATH
 *   updater-test compare A B
 *
 * Each command prints one line and exits 0 on success. With no overrides, check
 * talks to the real feed using the app's real settings.
 */

#include "updater.h"
#include "update_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define MAX_PINS 8

static const wchar_t *arg_value(int argc, wchar_t **argv, const wchar_t *name)
{
    int i;
    for (i = 2; i + 1 < argc; i++) {
        if (wcscmp(argv[i], name) == 0) return argv[i + 1];
    }
    return NULL;
}

static int cmd_check(int argc, wchar_t **argv)
{
    UpdateEndpoint ep = { ASB_UPDATE_API_URL, ASB_UPDATE_ASSET_PREFIX, ASB_UPDATE_RELEASE_PREFIX };
    const wchar_t *current = updater_current_version(), *v;
    BOOL prerelease = FALSE;
    UpdateInfo info;
    wchar_t err[UPDATER_ERR_CHARS] = L"";
    UpdateCheckResult r;
    int i;

    if ((v = arg_value(argc, argv, L"--api")) != NULL) ep.api_url = v;
    if ((v = arg_value(argc, argv, L"--asset-prefix")) != NULL) ep.asset_prefix = v;
    if ((v = arg_value(argc, argv, L"--release-prefix")) != NULL) ep.release_prefix = v;
    if ((v = arg_value(argc, argv, L"--current")) != NULL) current = v;
    for (i = 2; i < argc; i++) {
        if (wcscmp(argv[i], L"--prerelease") == 0) prerelease = TRUE;
    }

    r = updater_check(&ep, current, prerelease, &info, err, _countof(err));
    if (r == UPDATE_CHECK_ERROR) { wprintf(L"error %s\n", err); return 1; }
    if (r == UPDATE_CHECK_NONE) { wprintf(L"none current=%s\n", current); return 0; }
    wprintf(L"available version=%s tag=%s asset=%s size=%llu sha256=%s prerelease=%d url=%s page=%s\n",
            info.version, info.tag, info.asset_name, info.size, info.sha256,
            info.prerelease, info.asset_url, info.release_url);
    return 0;
}

static int cmd_download(int argc, wchar_t **argv)
{
    const wchar_t *url = arg_value(argc, argv, L"--url");
    const wchar_t *size = arg_value(argc, argv, L"--size");
    const wchar_t *dest = arg_value(argc, argv, L"--dest");
    UpdateInfo info;
    wchar_t err[UPDATER_ERR_CHARS] = L"";

    if (!url || !size || !dest) return 2;
    ZeroMemory(&info, sizeof(info));
    wcscpy_s(info.asset_url, _countof(info.asset_url), url);
    info.size = _wcstoui64(size, NULL, 10);
    if (!updater_download(&info, dest, NULL, NULL, NULL, err, _countof(err))) {
        wprintf(L"error %s\n", err);
        return 1;
    }
    wprintf(L"downloaded\n");
    return 0;
}

static int cmd_verify(int argc, wchar_t **argv)
{
    const wchar_t *file = arg_value(argc, argv, L"--file");
    const wchar_t *sha256 = arg_value(argc, argv, L"--sha256");
    const wchar_t *version = arg_value(argc, argv, L"--version");
    const wchar_t *pins[MAX_PINS];
    wchar_t err[UPDATER_ERR_CHARS] = L"";
    int i, count = 0;

    if (!file || !sha256 || !version) return 2;
    for (i = 2; i + 1 < argc && count < MAX_PINS; i++) {
        if (wcscmp(argv[i], L"--pin") == 0) pins[count++] = argv[i + 1];
    }
    if (!updater_verify_installer(file, sha256, version, pins, count, NULL, err, _countof(err))) {
        wprintf(L"refused %s\n", err);
        return 1;
    }
    wprintf(L"verified\n");
    return 0;
}

static int cmd_signer(int argc, wchar_t **argv)
{
    const wchar_t *file = arg_value(argc, argv, L"--file");
    wchar_t signer[65] = L"";
    LONG status = 0;
    HANDLE h;
    BOOL ok;

    if (!file) return 2;
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { wprintf(L"error open %lu\n", GetLastError()); return 1; }
    ok = updater_read_signer(file, h, signer, &status);
    CloseHandle(h);
    wprintf(L"signer=%s status=0x%08lX\n", ok ? signer : L"none", (unsigned long)status);
    return ok ? 0 : 1;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) return 2;
    if (wcscmp(argv[1], L"check") == 0) return cmd_check(argc, argv);
    if (wcscmp(argv[1], L"download") == 0) return cmd_download(argc, argv);
    if (wcscmp(argv[1], L"verify") == 0) return cmd_verify(argc, argv);
    if (wcscmp(argv[1], L"signer") == 0) return cmd_signer(argc, argv);
    if (wcscmp(argv[1], L"compare") == 0 && argc == 4) {
        wprintf(L"%d\n", updater_version_compare(argv[2], argv[3]));
        return 0;
    }
    return 2;
}
