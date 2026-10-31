/*
 * file_drop.h -- the guest end of Run in Sandbox (ASB_CH_FILEDROP).
 *
 * The host pushes one file at a time over its own Hyper-V socket, so a big
 * copy never holds up the agent's control channel, then asks for it to be
 * opened. Nothing here reaches back to the host: the guest only ever gets a
 * copy of the bytes.
 */
#ifndef FILE_DROP_H
#define FILE_DROP_H

#include <windows.h>

typedef void (*FileDropLog)(const char *fmt, ...);

/* Start the listener thread. stop_event ends it. */
void file_drop_start(FileDropLog log, HANDLE stop_event);

/* appsandbox-agent.exe --open-file <path>, run in the signed-in user's session:
   opens the file with its default handler. Exit codes below. */
#define FILE_DROP_OPEN_OK         0
#define FILE_DROP_OPEN_FAILED     1
#define FILE_DROP_OPEN_NO_HANDLER 3
int file_drop_open_main(void);

#endif /* FILE_DROP_H */
