/* MSVC shim for the POSIX <strings.h> used by shared engine sources
   (iso-patch-mac/engine/bcd_patch.c). */
#ifndef ASB_COMPAT_STRINGS_H
#define ASB_COMPAT_STRINGS_H
#include <string.h>
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#endif
