/*
 * lab_recorder.h -- Malware lab recorder inside a Windows guest.
 *
 * Between lab_start and lab_stop it records what changed in the VM, the way
 * an online sandbox report does: every file created, modified, renamed or
 * deleted (NTFS change journal), every registry key and value added,
 * changed or deleted (before/after snapshot of HKLM\SOFTWARE, the services
 * and session-manager keys and every loaded user hive), every process
 * started (with parent and command line) and every network connection.
 * lab_stop builds a JSON report the host fetches in chunks with lab_chunk.
 */
#ifndef LAB_RECORDER_H
#define LAB_RECORDER_H

#include <windows.h>

typedef void (*LabLogFn)(const char *fmt, ...);

void lab_init(LabLogFn log);
/* reply: "ok" or "error:<why>" */
void lab_start(char *reply, size_t reply_size);
/* reply: "ok:<report bytes>" or "error:<why>" */
void lab_stop(char *reply, size_t reply_size);
/* reply: "ok:<base64 of the next part>" ("ok:" at the end) or "error:<why>" */
void lab_chunk(size_t offset, char *reply, size_t reply_size);

/* Largest raw part per lab_chunk, so its base64 fits the host's reply line. */
#define LAB_CHUNK_BYTES 18000

#endif
