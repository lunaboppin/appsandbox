/*
 * appsandbox-displays.exe — Tiny helper that enumerates active monitors
 * and prints count + resolutions to stdout, then exits.
 *
 * Output format (single line):
 *   <count>,<W>x<H>,<W>x<H>,...
 *
 * Example:
 *   2,1920x1080,1024x768
 *
 * --only-appsandbox: switch off every active monitor except the AppSandbox
 * virtual display and make it the main one, saved to the display database.
 * With GPU-PV the host GPU's driver can expose a display output of its own,
 * which Windows may make the main monitor; the AppSandbox window then shows
 * the secondary one. Prints "unchanged", "no appsandbox display",
 * "applied: N other display(s) off" or "error ...". It also keeps the AppSandbox
 * display in SDR (see sdr_only), adding "colour: ..." when that changed something.
 *
 * --stream-prep: run by Sunshine before a stream. Asks the display driver for
 * the size in SUNSHINE_CLIENT_WIDTH/HEIGHT and waits for it to appear, so capture
 * starts at the client's size. Always exits 0, because a failure here would stop
 * the stream, and the picture is only scaled if the size doesn't change.
 *
 * Must be launched in the interactive session (WinSta0\Default) so that
 * EnumDisplayDevices / EnumDisplaySettings see all displays.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

static BOOL is_appsandbox_display(const wchar_t *gdi_name)
{
    DISPLAY_DEVICEW dd;
    DWORD i;
    for (i = 0; ; i++) {
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) return FALSE;
        if (wcscmp(dd.DeviceName, gdi_name) == 0)
            return wcsstr(dd.DeviceString, L"App Sandbox Virtual Display") != NULL;
    }
}

/* Windows can compose an SDR display in FP16 scRGB (Auto Color Management, or "wide
   color"). The driver's own frames are converted back to 8-bit, but Desktop
   Duplication hands out the FP16 desktop, and Sunshine and Parsec gamma-correct it a
   second time, so streams look washed out. The AppSandbox display is SDR only: turn
   wide color and HDR off on it. Returns what changed, or NULL. */
static const char *sdr_only(const DISPLAYCONFIG_PATH_INFO *path)
{
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 info2;
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info;
    ZeroMemory(&info2, sizeof(info2));
    info2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    info2.header.size = sizeof(info2);
    info2.header.adapterId = path->targetInfo.adapterId;
    info2.header.id = path->targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&info2.header) == ERROR_SUCCESS) {
        DISPLAYCONFIG_SET_WCG_STATE wcg;
        DISPLAYCONFIG_SET_HDR_STATE hdr;
        if (info2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR) return NULL;
        if (info2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR) {
            ZeroMemory(&hdr, sizeof(hdr));
            hdr.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
            hdr.header.size = sizeof(hdr);
            hdr.header.adapterId = path->targetInfo.adapterId;
            hdr.header.id = path->targetInfo.id;
            DisplayConfigSetDeviceInfo(&hdr.header);
        }
        ZeroMemory(&wcg, sizeof(wcg));
        wcg.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_WCG_STATE;
        wcg.header.size = sizeof(wcg);
        wcg.header.adapterId = path->targetInfo.adapterId;
        wcg.header.id = path->targetInfo.id;
        DisplayConfigSetDeviceInfo(&wcg.header);
        if (DisplayConfigGetDeviceInfo(&info2.header) != ERROR_SUCCESS) return "colour: error";
        return info2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR
            ? "colour: wide colour off" : "colour: still not SDR";
    }
    /* Before Windows 11 24H2: one advanced color switch. */
    ZeroMemory(&info, sizeof(info));
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    info.header.size = sizeof(info);
    info.header.adapterId = path->targetInfo.adapterId;
    info.header.id = path->targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS && info.advancedColorEnabled) {
        DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE set;
        ZeroMemory(&set, sizeof(set));
        set.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
        set.header.size = sizeof(set);
        set.header.adapterId = path->targetInfo.adapterId;
        set.header.id = path->targetInfo.id;
        return DisplayConfigSetDeviceInfo(&set.header) == ERROR_SUCCESS
            ? "colour: advanced colour off" : "colour: error";
    }
    return NULL;
}

static int only_appsandbox(void)
{
    const char *colour;
    UINT32 np = 0, nm = 0, i, idx;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    int ours = -1, others = 0;
    LONG rc;

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) {
        printf("error query");
        return 1;
    }
    paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof(*paths));
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof(*modes));
    if (!paths || !modes ||
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) != ERROR_SUCCESS) {
        printf("error query");
        free(paths); free(modes);
        return 1;
    }
    for (i = 0; i < np; i++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
        ZeroMemory(&src, sizeof(src));
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
            ours < 0 && is_appsandbox_display(src.viewGdiDeviceName))
            ours = (int)i;
        else
            others++;
    }
    if (ours < 0) { printf("no appsandbox display"); free(paths); free(modes); return 0; }
    colour = sdr_only(&paths[ours]);
    if (!others) {
        if (colour) printf("applied: %s", colour); else printf("unchanged");
        free(paths); free(modes); return 0;
    }

    for (i = 0; i < np; i++) {
        if ((int)i == ours) continue;
        paths[i].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
        paths[i].sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        paths[i].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }
    /* The source at the desktop origin is the main display. */
    idx = paths[ours].sourceInfo.modeInfoIdx;
    if (idx < nm && modes[idx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        modes[idx].sourceMode.position.x = 0;
        modes[idx].sourceMode.position.y = 0;
    }
    rc = SetDisplayConfig(np, paths, nm, modes,
                          SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                          SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES);
    if (rc == ERROR_SUCCESS) printf("applied: %d other display(s) off%s%s", others,
                                    colour ? "; " : "", colour ? colour : "");
    else printf("error %ld", rc);
    free(paths); free(modes);
    return rc == ERROR_SUCCESS ? 0 : 1;
}

/* sdr_only for the AppSandbox display, wherever it is now. */
static void sdr_ours(void)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths;
    DISPLAYCONFIG_MODE_INFO *modes;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np) return;
    paths = (DISPLAYCONFIG_PATH_INFO *)calloc(np, sizeof(*paths));
    modes = (DISPLAYCONFIG_MODE_INFO *)calloc(nm ? nm : 1, sizeof(*modes));
    if (paths && modes &&
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
            ZeroMemory(&src, sizeof(src));
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS &&
                is_appsandbox_display(src.viewGdiDeviceName)) {
                sdr_only(&paths[i]);
                break;
            }
        }
    }
    free(paths); free(modes);
}

static int stream_prep(void)
{
    char sw[16], sh[16];
    DWORD w, h, value, tick;
    BOOL asked = FALSE;
    HKEY key;
    DEVMODEW dm;
    if (!GetEnvironmentVariableA("SUNSHINE_CLIENT_WIDTH", sw, sizeof(sw)) ||
        !GetEnvironmentVariableA("SUNSHINE_CLIENT_HEIGHT", sh, sizeof(sh)))
        return 0;
    w = (DWORD)atoi(sw);
    h = (DWORD)atoi(sh);
    if (w < 640 || h < 480 || w > 8192 || h > 8192) return 0;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\AppSandbox\\Display", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return 0;
    value = (w << 16) | h;
    /* Writing the same data again doesn't notify, and the driver has to hear every request. */
    RegDeleteValueW(key, L"StreamMode");
    RegSetValueExW(key, L"StreamMode", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(key);
    tick = GetTickCount();
    while (GetTickCount() - tick < 10000) {
        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmPelsWidth == w && dm.dmPelsHeight == h) {
            Sleep(500);   /* the monitor just arrived: let the desktop settle */
            break;
        }
        /* Windows can keep the desktop size it saved for this monitor and scale it. */
        if (!asked && GetTickCount() - tick > 2500) {
            asked = TRUE;
            dm.dmPelsWidth = w;
            dm.dmPelsHeight = h;
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
            ChangeDisplaySettingsExW(NULL, &dm, NULL, CDS_UPDATEREGISTRY, NULL);
        }
        Sleep(200);
    }
    /* A monitor that just arrived can come up in wide colour: capture must start in SDR. */
    sdr_ours();
    return 0;
}

int main(int argc, char **argv)
{
    DISPLAY_DEVICEW dd;
    DEVMODEW dm;
    char buf[512];
    int count = 0;
    int off = 0;
    DWORD i;

    if (argc > 1 && strcmp(argv[1], "--only-appsandbox") == 0)
        return only_appsandbox();
    if (argc > 1 && strcmp(argv[1], "--stream-prep") == 0)
        return stream_prep();

    buf[0] = '\0';

    memset(&dd, 0, sizeof(dd));
    dd.cb = sizeof(dd);

    for (i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
            dd.cb = sizeof(dd);
            continue;
        }

        memset(&dm, 0, sizeof(dm));
        dm.dmSize = sizeof(dm);

        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm) &&
            dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
            if (count > 0 && off < (int)sizeof(buf) - 1) {
                buf[off++] = ',';
                buf[off] = '\0';
            }
            if (off < (int)sizeof(buf) - 1) {
                int n = snprintf(buf + off, sizeof(buf) - (size_t)off, "%lux%lu",
                                 dm.dmPelsWidth, dm.dmPelsHeight);
                if (n < 0) break;
                off += n;
                if (off >= (int)sizeof(buf)) {
                    off = (int)sizeof(buf) - 1;
                    buf[off] = '\0';
                    break;
                }
            }
        }
        count++;

        dd.cb = sizeof(dd);
    }

    printf("%d,%s", count, buf);
    return 0;
}
