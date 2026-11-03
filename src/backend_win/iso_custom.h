#ifndef ISO_CUSTOM_H
#define ISO_CUSTOM_H

/* A Windows ISO's own setup customisation: an autounattend.xml at its root and
   a sources\$OEM$ folder. App Sandbox doesn't run Windows Setup (iso-patch applies
   the image straight to the VHDX), so neither would be used on their own. These
   helpers carry them over: the $OEM$ files are staged onto the disk the way Setup
   copies them, and the ISO's answer file is merged into ours, keeping its
   specialize commands, locale and extra first-logon commands while App Sandbox
   keeps the computer name, the admin account, autologon and the OOBE skip. */

#include <windows.h>

/* Names inside the folder that asb_core copies the ISO's customisation into. */
#define ISO_CUSTOM_UNATTEND L"autounattend.xml"
#define ISO_CUSTOM_OEM      L"$OEM$"

/* Merge the ISO's answer file (theirs) into the App Sandbox one (ours, rewritten in
   place). summary gets a one-line description of what was kept and dropped.
   Returns FALSE, leaving ours untouched, when either file can't be parsed. */
BOOL iso_custom_merge_unattend(const wchar_t *ours_path, const wchar_t *theirs_path,
                               wchar_t *summary, size_t summary_cap);

/* Append the ISO's $OEM$ files to an iso-patch staging manifest: $$ goes to \Windows,
   $1 to the root of the system drive. A SetupComplete.cmd is renamed to
   SetupComplete.iso.cmd (ours calls it at the end) and Panther is skipped, since the
   answer file there is ours. Also stages \Windows\AppSandbox\custom-iso.txt, which
   tells SetupComplete.cmd to leave the ISO's taskbar layout alone.
   Returns the number of entries written, or -1 on error. */
int iso_custom_append_manifest(const wchar_t *manifest_path, const wchar_t *custom_dir,
                               const wchar_t *iso_path);

#endif
