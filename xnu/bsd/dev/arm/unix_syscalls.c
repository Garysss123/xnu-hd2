/* HTC HD2 integration/publication changes: Garysss123, 2026-10-04. Original license notices are preserved. */
#if defined(BOARD_CONFIG_QSD8250_LEO)
#include "leo_user_frontier.h"
#endif
/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Copyright 2013, winocm. <winocm@icloud.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 *   Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 *   Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 *   If you are going to use this software in any form that does not involve
 *   releasing the source to this project or improving it, let me know beforehand.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * BSD system calls for ARM.
 */

#include <mach/mach_types.h>
#include <mach/exception.h>

#include <kern/task.h>
#include <kern/thread.h>
#include <kern/assert.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/debug.h>

#include <sys/systm.h>
#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/user.h>
#include <sys/sysproto.h>
#include <sys/sysent.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <mach/thread_act.h>    /* for thread_abort_safely */
#include <mach/thread_status.h>

#include <arm/machine_routines.h>

#include <sys/kdebug.h>
#include <sys/sdt.h>

#include <security/audit/audit.h>

#include <mach/branch_predicates.h>
#include <arm/ios7_compat.h>
#if IOS7_COMPAT_PLATFORM
#include <sys/fcntl.h>
#endif

#if BOARD_CONFIG_ARMPBA8
#include <pexpert/pexpert.h>
#include <sys/syscall.h>
#include <sys/fcntl.h>
#include <libkern/OSAtomic.h>
#endif

/* dynamically generated at build time based on syscalls.master */
extern const char *syscallnames[];

#if BOARD_CONFIG_ARMPBA8
extern volatile uint32_t ios7lab_preferences_mach_header;

__attribute__((noinline)) void
ios7lab_preferences_user_marker(struct proc *p, uint32_t mach_header)
{
	__asm__ volatile("" : : "r"(p), "r"(mach_header) : "memory");
}

/* Ordinary syslogd-only I/O observation, independent of old syscall tracing.
 * 32 atomically reserved calls produce at most64 lines. Never reads log data,
 * iovec entries, or user stat output, and never changes original arguments. */
static unsigned int
ios7lab_syslogd_io_enter(struct proc *p, unsigned int code, void *arguments)
{
    static volatile UInt32 call_count = 0;
    UInt32 old_count;
    int vmabi = 0, fd = -1, flags = 0, mode = 0, cmd = 0, whence = 0;
    int path_error = 0, count_is_iov = 0;
    unsigned long long count = 0, argument = 0;
    long long offset = 0;
    char pathname[256];
    const char *path = "<none>";
    user_addr_t user_path = 0;
    size_t copied = 0;
    boolean_t has_path = FALSE;
    boolean_t interested = code == SYS_open || code == SYS_open_nocancel ||
        code == SYS_write || code == SYS_write_nocancel ||
        code == SYS_writev || code == SYS_writev_nocancel ||
        code == SYS_close || code == SYS_close_nocancel ||
        code == SYS_fstat64 || code == SYS_fcntl ||
        code == SYS_fcntl_nocancel || code == SYS_lseek;
    if (!interested || strncmp(p->p_comm, "syslogd", sizeof(p->p_comm)) ||
        !PE_parse_boot_argn("ios7lab_vmabi", &vmabi, sizeof(vmabi)) || vmabi != 1)
        return 0;
    old_count = call_count;
    /* Drop a contended diagnostic instead of waiting in a syscall path. */
    if (old_count >= 32 || !OSCompareAndSwap(old_count, old_count + 1, &call_count))
        return 0;

    /* Inspect only already-marshaled kernel argument structures, using each
     * call's actual generated type. Original user pointers remain untouched. */
    switch (code) {
    case SYS_open: {
        const struct open_args *a = arguments;
        user_path = a->path; flags = a->flags; mode = a->mode; has_path = TRUE; break;
    }
    case SYS_open_nocancel: {
        const struct open_nocancel_args *a = arguments;
        user_path = a->path; flags = a->flags; mode = a->mode; has_path = TRUE; break;
    }
    case SYS_write: {
        const struct write_args *a = arguments;
        fd = a->fd; count = (unsigned long long)a->nbyte; break;
    }
    case SYS_write_nocancel: {
        const struct write_nocancel_args *a = arguments;
        fd = a->fd; count = (unsigned long long)a->nbyte; break;
    }
    case SYS_writev: {
        const struct writev_args *a = arguments;
        fd = a->fd; count = a->iovcnt; count_is_iov = 1; break;
    }
    case SYS_writev_nocancel: {
        const struct writev_nocancel_args *a = arguments;
        fd = a->fd; count = a->iovcnt; count_is_iov = 1; break;
    }
    case SYS_close: fd = ((const struct close_args *)arguments)->fd; break;
    case SYS_close_nocancel: fd = ((const struct close_nocancel_args *)arguments)->fd; break;
    case SYS_fstat64: fd = ((const struct fstat64_args *)arguments)->fd; break;
    case SYS_fcntl: {
        const struct fcntl_args *a = arguments;
        fd = a->fd; cmd = a->cmd; argument = (unsigned long long)a->arg; break;
    }
    case SYS_fcntl_nocancel: {
        const struct fcntl_nocancel_args *a = arguments;
        fd = a->fd; cmd = a->cmd; argument = (unsigned long long)a->arg; break;
    }
    case SYS_lseek: {
        const struct lseek_args *a = arguments;
        fd = a->fd; offset = (long long)a->offset; whence = a->whence; break;
    }
    }
    if (has_path) {
        pathname[0] = 0;
        path_error = copyinstr(user_path, pathname, sizeof(pathname), &copied);
        if (!path_error) {
            size_t i;
            for (i = 0; i < sizeof(pathname) && pathname[i]; ++i)
                if (pathname[i] == '\n' || pathname[i] == '\r') pathname[i] = '?';
            path = pathname;
        }
        else path = "<copyinstr-error>";
    }
    printf("IOS7LAB SYSLOGIO ENTER call=%u pid=%d code=%u name=%s fd=%d flags=%x mode=%o count=%llu iov-count=%d cmd=%d arg=%llx offset=%lld whence=%d path-copy-error=%d path-bytes=%u path=%s\n",
        (unsigned)old_count + 1, p->p_pid, code, syscallnames[code], fd, flags, mode,
        count, count_is_iov, cmd, argument, offset, whence, path_error, (unsigned)copied, path);
    return (unsigned)old_count + 1;
}


#endif

#if IOS7_COMPAT_PLATFORM
/* Original iOS7 ARM psynch stubs save16bytes and expose r0..r6 only.
 * Their remaining4 argument words stay at the caller stack+12, hence
 * saved userSP+28. Native register layout and generated structs already
 * agree: do not reorder sequence words or alter synchronization semantics. */
static int
ios7lab_psynch_stack_tail(uint32_t code, uint32_t userSP,
                         uint32_t *arguments, unsigned argumentWords)
{
    uint32_t tail[4];
    int error;
    if (code != 303 && code != 304 && code != 305) return 0;
    if (argumentWords != 11) return EINVAL;
    if ((userSP & 3U) || userSP > 0xffffffffU - 44U) return EFAULT;
    error = copyin((user_addr_t)(userSP + 28U), tail, sizeof(tail));
    if (error) return error;
    memcpy(arguments + 7, tail, sizeof(tail));
    return 0;
}


/* Scoped legacy event-queue compatibility: a real kqueue and CLOEXEC,
 * intentionally without fd-guard enforcement, as in the accepted QEMU path. */
static int
ios7lab_guarded_kqueue(struct proc *p, user_addr_t guard_addr,
                      uint32_t guard_flags, int32_t *retval)
{
    uint64_t guard;
    int error, queue_fd, ignored = 0;
    struct fcntl_args fdflags;
    struct close_args closeargs;
    static unsigned int logs = 0;

    /* Original 2422 ABI requires GUARD_DUP and accepts only bits 0..3. */
    if (!(guard_flags & 2U) || (guard_flags & ~15U))
        return EINVAL;
    error = copyin(guard_addr, &guard, sizeof(guard));
    if (error)
        return error;
    if (!guard)
        return EINVAL;
    error = kqueue(p, NULL, retval);
    if (error)
        return error;
    queue_fd = *retval;
    bzero(&fdflags, sizeof(fdflags));
    fdflags.fd = queue_fd;
    fdflags.cmd = F_SETFD;
    fdflags.arg = FD_CLOEXEC;
    error = fcntl(p, &fdflags, &ignored);
    if (error) {
        bzero(&closeargs, sizeof(closeargs));
        closeargs.fd = queue_fd;
        (void)close(p, &closeargs, &ignored);
        *retval = 0;
        return error;
    }
    if (logs++ < 24)
        printf("IOS7LAB guarded_kqueue pid=%d fd=%d flags=%x real-queue cloexec guard-enforcement-bypassed\n",
               p->p_pid, queue_fd, guard_flags);
    return 0;
}
#endif

/*
 * Function:	unix_syscall
 *
 * Inputs:	regs	- pointer to arm save area
 *
 * Outputs:	none
 */
void unix_syscall(arm_saved_state_t * state)
{
    thread_t thread;
    void *vt;
    unsigned int code;
    struct sysent *callp;
    int error = 0;
    vm_offset_t params;
    struct proc *p;
    struct uthread *uthread;
    boolean_t args_in_uthread;
    boolean_t is_vfork;

    assert(state != NULL);
    thread = current_thread();
    uthread = get_bsdthread_info(thread);

    /*
     * Get the approriate proc; may be different from task's for vfork()
     */
    is_vfork = uthread->uu_flag & UT_VFORK;
    if (__improbable(is_vfork != 0))
        p = current_proc();
    else
        p = (struct proc *) get_bsdtask_info(current_task());

    /*
     * Verify that we are not being called from a task without a proc
     */
    if (__improbable(p == NULL)) {
        state->r[0] = EPERM;
        task_terminate_internal(current_task());
        thread_exception_return();
        /*
         * NOTREACHED
         */
    }

    /*
     * Current syscall number is in r12 on ARM
     */
    boolean_t shift_args = FALSE;

    state->cpsr &= ~(1 << 29);  /* C-bit IIRC, turn this into a Define */
    code = state->r[12];

    /*
     * Delayed binding of thread credential to process credential, if we
     * are not running with an explicitly set thread credential.
     */
    kauth_cred_uthread_update(uthread, p);

    callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];
    if (__improbable(callp == sysent)) {
        /*
         * indirect system call... system call number
         * passed as 'arg0'
         */
        code = state->r[0];
        callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];
        shift_args = TRUE;
    }

#if defined(BOARD_CONFIG_QSD8250_LEO)
    ios7leo_frontier_bsd_entry(state,code);
#endif
#if IOS7_COMPAT_PLATFORM
    boolean_t lab_guarded_queue = code == 443 && ios7_compat_vmabi_enabled();
#endif

    if (callp->sy_narg) {
        int arg_count = callp->sy_narg;
        if (!shift_args) {
            int i;
            for (i = 0; i < 12; i++) {
                uthread->uu_arg[i] = state->r[i];
                if (i == (arg_count))
                    break;
            }
        } else {
            int i;
            for (i = 0; i < 11; i++) {
                uthread->uu_arg[i] = state->r[i + 1];
                if (i == (arg_count))
                    break;
            }
        }
    }

#if IOS7_COMPAT_PLATFORM
    int lab_psynch_argument_error = 0;
    if (!shift_args && (code == 303 || code == 304 || code == 305) &&
        ios7_compat_vmabi_enabled()) {
        lab_psynch_argument_error = ios7lab_psynch_stack_tail((uint32_t)code,
            state->sp, (uint32_t *)uthread->uu_arg, (unsigned)callp->sy_narg);
    }
#endif

    /*
     * Set return values
     */
    uthread->uu_flag |= UT_NOTCANCELPT;
    uthread->uu_rval[0] = 0;
    uthread->uu_rval[1] = 0;

#if BOARD_CONFIG_ARMPBA8
    unsigned int lab_syslog_io_index = ios7lab_syslogd_io_enter(p, code, (void *)uthread->uu_arg);
#endif
    AUDIT_SYSCALL_ENTER(code, p, uthread);
#if BOARD_CONFIG_ARMPBA8
    static boolean_t lab_preferences_marker_seen = FALSE;
    int lab_marker_enabled = 0;
    if (!lab_preferences_marker_seen &&
        !strncmp(p->p_comm, "Preferences", sizeof(p->p_comm)) &&
        PE_parse_boot_argn("ios7lab_vmabi", &lab_marker_enabled, sizeof(lab_marker_enabled)) &&
        lab_marker_enabled == 1) {
        lab_preferences_marker_seen = TRUE;
		ios7lab_preferences_user_marker(p, ios7lab_preferences_mach_header);
    }
    unsigned int lab_trace_index = 0;
    static unsigned int lab_trace_count[32] = {0};
    static unsigned int lab_trace_late_count[32] = {0};
    int lab_trace_enabled = 0;
    boolean_t lab_interest = code == SYS_csops || code == SYS_csops_audittoken || code == SYS_proc_info || code == SYS_exit || code >= NUM_SYSENT;
    boolean_t lab_path_interest = code == SYS_open || code == SYS_open_nocancel ||
        code == SYS_stat64 || code == SYS_lstat64 || code == SYS_access || code == SYS_execve;
    boolean_t lab_preferences_path = p->p_pid > 0 && p->p_pid < 32 &&
        !strncmp(p->p_comm, "Preferences", sizeof(p->p_comm)) && lab_path_interest &&
        lab_trace_count[p->p_pid] < 1200;
    if (p->p_pid > 0 && p->p_pid < 32 &&
        (lab_trace_count[p->p_pid] < 300 || lab_preferences_path ||
         (lab_interest && lab_trace_late_count[p->p_pid] < 64)) &&
        code != SYS_sigprocmask &&
        PE_parse_boot_argn("ios7lab_syscalls", &lab_trace_enabled, sizeof(lab_trace_enabled)) &&
        lab_trace_enabled == 1) {
        if (lab_trace_count[p->p_pid] >= 300) ++lab_trace_late_count[p->p_pid];
        lab_trace_index = ++lab_trace_count[p->p_pid];
        const uint32_t *lab_regs = &state->r[shift_args ? 1 : 0];
        printf("IOS7LAB pid=%d syscall %u ENTER %u %s raw_regs=%08x,%08x,%08x,%08x,%08x,%08x\n",
               p->p_pid, lab_trace_index, code, lab_guarded_queue ? "guarded_kqueue_np" : syscallnames[code >= NUM_SYSENT ? 63 : code],
               lab_regs[0], lab_regs[1], lab_regs[2], lab_regs[3], lab_regs[4], lab_regs[5]);
        if (lab_path_interest) {
            char lab_path[192];
            size_t lab_copied = 0;
            int lab_copy = copyinstr((user_addr_t)uthread->uu_arg[0], lab_path,
                                    sizeof(lab_path), &lab_copied);
            if (lab_copy == 0)
                printf("IOS7LAB pid=%d syscall %u PATH %s\n", p->p_pid, lab_trace_index, lab_path);
        }
    }
#endif
#if IOS7_COMPAT_PLATFORM
    if (lab_psynch_argument_error) {
        error = lab_psynch_argument_error;
    } else if (lab_guarded_queue) {
        unsigned int first = shift_args ? 1 : 0;
        error = ios7lab_guarded_kqueue(p, (user_addr_t)state->r[first],
                                      state->r[first + 1], &uthread->uu_rval[0]);
    } else
#endif
    error = (*(callp->sy_call)) (p, (void *) uthread->uu_arg, &(uthread->uu_rval[0]));
    AUDIT_SYSCALL_EXIT(code, p, uthread, error);
#if BOARD_CONFIG_ARMPBA8
    if (lab_syslog_io_index)
        printf("IOS7LAB SYSLOGIO EXIT call=%u pid=%d code=%u error=%d result0=%08x result1=%08x\n",
            lab_syslog_io_index, p->p_pid, code, error,
            (unsigned)uthread->uu_rval[0], (unsigned)uthread->uu_rval[1]);
#endif
#if BOARD_CONFIG_ARMPBA8
    if (lab_trace_index)
        printf("IOS7LAB pid=%d syscall %u EXIT error=%d values=%08x,%08x\n",
               p->p_pid, lab_trace_index, error, (unsigned)uthread->uu_rval[0], (unsigned)uthread->uu_rval[1]);
#endif

#if 0
    kprintf("SYSCALL: %s (%d, routine %p), args %p, return %x (%x, %x) pc 0x%08x\n", syscallnames[code >= NUM_SYSENT ? 63 : code], code, callp->sy_call, (void *) uthread->uu_arg, error, uthread->uu_rval[0], uthread->uu_rval[1], state->pc);
#endif

#if CONFIG_MACF
    mac_thread_userret(code, error, thread);
#endif

    if (error)
        state->cpsr |= (1 << 29);  /* C-bit IIRC, turn this into a Define */

    if (error == ERESTART) {
        panic("unix_syscall: restarting syscall\n");
    } else if(error != EJUSTRETURN) {
        if(error) {
            state->r[0] = error;
        } else { /* Not error. */
            switch(callp->sy_return_type) {
                case _SYSCALL_RET_INT_T:
                    state->r[0] = (int)uthread->uu_rval[0];
                    state->r[1] = (int)uthread->uu_rval[1];
                    break;
                case _SYSCALL_RET_UINT_T:
                    state->r[0] = (u_int)uthread->uu_rval[0];
                    state->r[1] = (u_int)uthread->uu_rval[1];
                    break;
                case _SYSCALL_RET_OFF_T:
                case _SYSCALL_RET_UINT64_T:
                    state->r[0] = uthread->uu_rval[0];
                    state->r[1] = uthread->uu_rval[1];
                    break;
                case _SYSCALL_RET_ADDR_T:
                case _SYSCALL_RET_SIZE_T:
                case _SYSCALL_RET_SSIZE_T: {
                    user_addr_t *retp = (user_addr_t *)&uthread->uu_rval[0];
                    state->r[0] = *retp;
                    state->r[1] = 0;
                }
                    break;
                case _SYSCALL_RET_NONE:
                    break;
                default:
                    panic("unix_syscall: unknown return type");
                    break;
            }
        }
    }

    uthread->uu_flag &= ~UT_NOTCANCELPT;

    /*
     * panic if funnel is held
     */
    syscall_exit_funnelcheck();

    if (uthread->uu_lowpri_window) {
        /*
         * task is marked as a low priority I/O type
         * and the I/O we issued while in this system call
         * collided with normal I/O operations... we'll
         * delay in order to mitigate the impact of this
         * task on the normal operation of the system
         */
        throttle_lowpri_io(TRUE);
    }

#if defined(BOARD_CONFIG_QSD8250_LEO)
    ios7leo_frontier_bsd_return(state,error);
#endif

    thread_exception_return();
}

void unix_syscall_return(int error)
{
    thread_t thread_act;
    struct uthread *uthread;
    struct proc *proc;
    arm_saved_state_t *state;
    unsigned int code = 0;
    struct sysent *callp;

    thread_act = current_thread();
    proc = current_proc();
    uthread = get_bsdthread_info(thread_act);

    state = find_user_regs(thread_act);

    if (state->r[12] != 0)
        code = state->r[12];

    callp = (code >= NUM_SYSENT) ? &sysent[63] : &sysent[code];

    AUDIT_SYSCALL_EXIT(code, proc, uthread, error);

    if (error == ERESTART) {
        panic("unix_syscall: restarting syscall\n");
    } else if(error != EJUSTRETURN) {
        if(error) {
            state->r[0] = error;
        } else { /* Not error. */
            switch(callp->sy_return_type) {
                case _SYSCALL_RET_INT_T:
                    state->r[0] = uthread->uu_rval[0];
                    state->r[1] = uthread->uu_rval[1];
                    break;
                case _SYSCALL_RET_UINT_T:
                    state->r[0] = (u_int)uthread->uu_rval[0];
                    state->r[1] = (u_int)uthread->uu_rval[1];
                    break;
                case _SYSCALL_RET_OFF_T:
                case _SYSCALL_RET_UINT64_T:
                    state->r[0] = uthread->uu_rval[0];
                    state->r[1] = uthread->uu_rval[1];
                    break;
                case _SYSCALL_RET_ADDR_T:
                case _SYSCALL_RET_SIZE_T:
                case _SYSCALL_RET_SSIZE_T: {
                    user_addr_t *retp = (user_addr_t *)&uthread->uu_rval[0];
                    state->r[0] = *retp;
                    state->r[1] = 0;
                }
                    break;
                case _SYSCALL_RET_NONE:
                    break;
                default:
                    panic("unix_syscall: unknown return type");
                    break;
            }
            state->cpsr &= ~(1 << 29);  /* C-bit IIRC, turn this into a Define */
        }
    }

    uthread->uu_flag &= ~UT_NOTCANCELPT;

#if FUNNEL_DEBUG
    /*
     * if we're holding the funnel panic
     */
    syscall_exit_funnelcheck();
#endif /* FUNNEL_DEBUG */

    if (uthread->uu_lowpri_window) {
        /*
         * task is marked as a low priority I/O type
         * and the I/O we issued while in this system call
         * collided with normal I/O operations... we'll
         * delay in order to mitigate the impact of this
         * task on the normal operation of the system
         */
        throttle_lowpri_io(TRUE);
    }
    if (kdebug_enable && (code != 180)) {
        if (callp->sy_return_type == _SYSCALL_RET_SSIZE_T)
            KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_END, error, uthread->uu_rval[1], 0, 0, 0);
        else
            KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_EXCP_SC, code) | DBG_FUNC_END, error, uthread->uu_rval[0], uthread->uu_rval[1], 0, 0);
    }

#if defined(BOARD_CONFIG_QSD8250_LEO)
    ios7leo_frontier_bsd_return(state,error);
#endif

    thread_exception_return();
    /*
     * NOTREACHED
     */
}
