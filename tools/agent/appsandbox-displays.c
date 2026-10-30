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
 * "applied: N other display(s) off" or "error ...".
 *
 * Must be launched in the interactive session (WinSta0\Default) so that
 * EnumDisplayDevices / EnumDisplaySettings see all displays.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "user32.lib")

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

static int only_appsandbox(void)
{
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
    if (!others)  { printf("unchanged"); free(paths); free(modes); return 0; }

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
    if (rc == ERROR_SUCCESS) printf("applied: %d other display(s) off", others);
    else printf("error %ld", rc);
    free(paths); free(modes);
    return rc == ERROR_SUCCESS ? 0 : 1;
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
