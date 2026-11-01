/*
 * sandbox_run.h -- Run in Sandbox: open a host file in a VM on a throwaway disk.
 *
 * A run's "checkpoint" is a differencing disk (snapshots\sandbox_<id>.vhdx) on
 * top of whatever disk the VM was using. The VM boots from it, so the disk
 * underneath stays frozen. Discarding deletes it; keeping merges it down.
 *
 * It is not a tree checkpoint because snapshot_take forks from the base disk,
 * which would silently drop the state of the branch the VM is on.
 *
 * Disks hold no memory state, so a checkpoint can only be taken with the VM off.
 * A running VM has to be shut down first, which only happens if the caller says
 * so (ASB_RUN_MODE_SHUTDOWN_FIRST), or is sent the file with no checkpoint at all.
 *
 * The run is recorded in vms.cfg in the same atomic write that points the VM at
 * the new disk, so a closed app or a host reboot can't lose track of it. Nothing
 * ever rolls back on its own: only asb_run_finish does, and only when asked.
 */
#ifndef SANDBOX_RUN_H
#define SANDBOX_RUN_H

#include <windows.h>
#include "asb_core.h"

typedef enum {
    ASB_RUN_MODE_CHECKPOINT = 0,   /* the VM must be off, or already in a run */
    ASB_RUN_MODE_SHUTDOWN_FIRST,   /* shut a running VM down first */
    ASB_RUN_MODE_NO_CHECKPOINT     /* copy and open only; nothing can be rolled back */
} AsbRunMode;

typedef enum {
    ASB_RUN_PHASE_IDLE = 0,
    ASB_RUN_PHASE_SHUTTING_DOWN,
    ASB_RUN_PHASE_CHECKPOINT,
    ASB_RUN_PHASE_STARTING,
    ASB_RUN_PHASE_WAITING_GUEST,
    ASB_RUN_PHASE_COPYING,
    ASB_RUN_PHASE_OPENING,
    ASB_RUN_PHASE_READY,
    ASB_RUN_PHASE_FINISHING,
    ASB_RUN_PHASE_FAILED
} AsbRunPhase;

typedef struct {
    BOOL     active;          /* a throwaway checkpoint exists and awaits a decision */
    BOOL     busy;            /* the worker is doing something right now */
    int      phase;           /* AsbRunPhase */
    int      pct;             /* copy progress, 0-100 */
    wchar_t  id[40];
    wchar_t  file[260];       /* name of the file the run was started for */
    wchar_t  message[512];    /* what is happening, or what went wrong */
    wchar_t  guest_path[512]; /* where the last file landed in the guest */
} AsbRunStatus;

/* Why this VM can't take a file, or NULL if it can. */
ASB_API const wchar_t *asb_run_unsuitable_reason(AsbVm vm);

/* Queue a file for the VM and return at once; poll asb_run_get_status or watch the
   state callback. A second file for a VM already in a run joins that run.
   ERROR_BUSY with a message means the VM is running and the mode doesn't allow
   touching it. error may be NULL. */
ASB_API HRESULT asb_run_file(AsbVm vm, const wchar_t *path, AsbRunMode mode,
                             wchar_t *error, size_t error_chars);

/* End the run: keep merges the changes into the VM's disk (shutting it down cleanly
   first), discard force-stops it and deletes them. Returns at once. A failure leaves
   the run in place, with the reason in the status. */
ASB_API HRESULT asb_run_finish(AsbVm vm, BOOL keep, wchar_t *error, size_t error_chars);

ASB_API BOOL asb_run_active(AsbVm vm);
ASB_API void asb_run_get_status(AsbVm vm, AsbRunStatus *out);
/* Forget a finished no-checkpoint run or a failed one's message. */
ASB_API void asb_run_clear_status(AsbVm vm);
/* TRUE once per request: the display window should open now. */
ASB_API BOOL asb_run_take_display_request(AsbVm vm);

ASB_API void asb_set_last_sandbox_vm(const wchar_t *name);
ASB_API const wchar_t *asb_get_last_sandbox_vm(void);

/* ---- Inside the core ---- */
/* Delete overlays no run refers to: left by a crash between creating one and
   recording it, or between recording a finished run and deleting it. */
void asb_run_sweep_orphans(VmInstance *inst, const wchar_t *snap_dir);
/* Percent-encode for vms.cfg, which is written in the C locale: a non-ASCII file
   name would otherwise cut the line short. */
void asb_run_encode_name(const wchar_t *in, wchar_t *out, size_t chars);
void asb_run_decode_name(const wchar_t *in, wchar_t *out, size_t chars);
/* The last VM picked, from vms.cfg, without writing it back. */
void asb_run_load_last_vm(const wchar_t *name);

#endif /* SANDBOX_RUN_H */
