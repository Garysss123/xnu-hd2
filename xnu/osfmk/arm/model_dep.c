/* HTC HD2 integration/publication changes: Garysss123, 2026-10-04. Original license notices are preserved. */
/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
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
 * ARM machine startup functions
 */

#include <platforms.h>
#include <mach/arm/vm_param.h>
#include <string.h>
#include <sys/time.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <mach/machine.h>
#include <mach/time_value.h>
#include <kern/spl.h>
#include <kern/assert.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <kern/machine.h>
#include <arm/pmap.h>

#include <mach_debug.h>
#include <arm/low_globals.h>

#include <arm/misc_protos.h>

#include <pexpert/pexpert.h>
#include <pexpert/arm/boot.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>

#include <kern/thread.h>
#include <kern/sched.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#include <arm/arch.h>

#include <libkern/kernel_mach_header.h>
#include <libkern/OSKextLibPrivate.h>

#include <IOKit/IOPlatformExpert.h>

extern unsigned int panic_is_inited;
extern struct timeval gIOLastSleepTime;
extern struct timeval gIOLastWakeTime;
extern char firmware_version[32];
extern uint32_t debug_enabled;
extern const char version[];
extern char osversion[];
extern boolean_t panicDialogDesired;
extern int enable_timing;

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

/*
 * Frame pointer definition.
 */

typedef struct _cframe_t {
    struct _cframe_t *prev;
    uintptr_t caller;
} cframe_t;

unsigned int nosym = 1;

void print_threads(uint32_t stackptr);
void panic_arm_thread_backtrace(void *_frame, int nframes, const char *msg,
                                boolean_t regdump, arm_saved_state_t * regs,
                                int crashed, char *crashstr);

/**
 * machine_init
 *
 * Machine-specific initialization.
 */
void machine_init(void)
{
    debug_log_init();
    clock_config();
    return;
}

static void machine_conf(void)
{
    machine_info.memory_size = (typeof(machine_info.memory_size)) mem_size;
    machine_info.max_mem = (typeof(machine_info.max_mem)) max_mem;
}

unsigned int debug_boot_arg;

/*
 * Debugger/DebuggerWithContext.
 *
 * Back to plagiarizing from i386.
 */

/* Routines for address - symbol translation. Not called unless the "keepsyms"
 * boot-arg is supplied.
 */

static int
panic_print_macho_symbol_name(kernel_mach_header_t *mh, vm_address_t search, const char *module_name)
{
    kernel_nlist_t  *sym = NULL;
    struct load_command     *cmd;
    kernel_segment_command_t    *orig_ts = NULL, *orig_le = NULL;
    struct symtab_command   *orig_st = NULL;
    unsigned int            i;
    char                    *strings, *bestsym = NULL;
    vm_address_t            bestaddr = 0, diff, curdiff;

    /* Assume that if it's loaded and linked into the kernel, it's a valid Mach-O */
    
    cmd = (struct load_command *) &mh[1];
    for (i = 0; i < mh->ncmds; i++) {
        if (cmd->cmd == LC_SEGMENT_KERNEL) {
            kernel_segment_command_t *orig_sg = (kernel_segment_command_t *) cmd;
            
            if (strncmp(SEG_TEXT, orig_sg->segname,
                    sizeof(orig_sg->segname)) == 0)
                orig_ts = orig_sg;
            else if (strncmp(SEG_LINKEDIT, orig_sg->segname,
                    sizeof(orig_sg->segname)) == 0)
                orig_le = orig_sg;
            else if (strncmp("", orig_sg->segname,
                    sizeof(orig_sg->segname)) == 0)
                orig_ts = orig_sg; /* pre-Lion i386 kexts have a single unnamed segment */
        }
        else if (cmd->cmd == LC_SYMTAB)
            orig_st = (struct symtab_command *) cmd;
        
        cmd = (struct load_command *) ((uintptr_t) cmd + cmd->cmdsize);
    }
    
    if ((orig_ts == NULL) || (orig_st == NULL) || (orig_le == NULL))
        return 0;
    
    if ((search < orig_ts->vmaddr) ||
        (search >= orig_ts->vmaddr + orig_ts->vmsize)) {
        /* search out of range for this mach header */
        return 0;
    }
    
    sym = (kernel_nlist_t *)(uintptr_t)(orig_le->vmaddr + orig_st->symoff - orig_le->fileoff);
    strings = (char *)(uintptr_t)(orig_le->vmaddr + orig_st->stroff - orig_le->fileoff);
    diff = search;
    
    for (i = 0; i < orig_st->nsyms; i++) {
        if (sym[i].n_type & N_STAB) continue;

        if (sym[i].n_value <= search) {
            curdiff = search - (vm_address_t)sym[i].n_value;
            if (curdiff < diff) {
                diff = curdiff;
                bestaddr = sym[i].n_value;
                bestsym = strings + sym[i].n_un.n_strx;
            }
        }
    }
    
    if (bestsym != NULL) {
        if (diff != 0) {
            kdb_printf("%s : %s + 0x%lx", module_name, bestsym, (unsigned long)diff);
        } else {
            kdb_printf("%s : %s", module_name, bestsym);
        }
        return 1;
    }
    return 0;
}

extern kmod_info_t * kmod; /* the list of modules */

static void
panic_print_kmod_symbol_name(vm_address_t search)
{
    u_int i;

    if (gLoadedKextSummaries == NULL)
        return;
    for (i = 0; i < gLoadedKextSummaries->numSummaries; ++i) {
        OSKextLoadedKextSummary *summary = gLoadedKextSummaries->summaries + i;

        if ((search >= summary->address) &&
            (search < (summary->address + summary->size)))
        {
            kernel_mach_header_t *header = (kernel_mach_header_t *)(uintptr_t) summary->address;
            if (panic_print_macho_symbol_name(header, search, summary->name) == 0) {
                kdb_printf("%s + %llu", summary->name, (unsigned long)search - summary->address);
            }
            break;
        }
    }
}

static void
panic_print_symbol_name(vm_address_t search)
{
    /* try searching in the kernel */
    if (panic_print_macho_symbol_name(&_mh_execute_header, search, "mach_kernel") == 0) {
        /* that failed, now try to search for the right kext */
        panic_print_kmod_symbol_name(search);
    }
}

/* Generate a backtrace, given a frame pointer - this routine
 * should walk the stack safely. The trace is appended to the panic log
 * and conditionally, to the console. If the trace contains kernel module
 * addresses, display the module name, load address and dependencies.
 */

#define DUMPFRAMES 32
#define PBT_TIMEOUT_CYCLES (5 * 1000 * 1000 * 1000ULL)
void
panic_arm_backtrace(void *_frame, int nframes, const char *msg, boolean_t regdump, arm_saved_state_t *regs)
{
    cframe_t    *frame = (cframe_t *)_frame;
    vm_offset_t raddrs[DUMPFRAMES];
    vm_offset_t PC = 0;
    int frame_index;
    boolean_t keepsyms = FALSE;
    int cn = cpu_number();

    PE_parse_boot_argn("keepsyms", &keepsyms, sizeof (keepsyms));

    if (msg != NULL) {
        kdb_printf("%s", msg);
    }

    kdb_printf("Backtrace (CPU %d), "
    "Frame : Return Address\n", cn);

    for (frame_index = 0; frame_index < nframes; frame_index++) {
        vm_offset_t curframep = (vm_offset_t) frame;

        if (!curframep)
            break;

        if (curframep & 0x3) {
            kdb_printf("Unaligned frame\n");
            goto invalid;
        }

        if (!kvtophys(curframep) ||
            !kvtophys(curframep + sizeof(cframe_t) - 1)) {
            kdb_printf("No mapping exists for frame pointer\n");
            goto invalid;
        }

        kdb_printf("%p : 0x%lx ", frame, frame->caller);
        if (frame_index < DUMPFRAMES)
            raddrs[frame_index] = frame->caller;

        /* Display address-symbol translation only if the "keepsyms"
         * boot-arg is suppplied, since we unload LINKEDIT otherwise.
         * This routine is potentially unsafe; also, function
         * boundary identification is unreliable after a strip -x.
         *
         * Right now, OSKextRemoveKextBootstrap is nulled out, so it
         * doesn't really matter at the moment. Dofix.
         */
#if 0
        if (keepsyms)
#endif
            panic_print_symbol_name((vm_address_t)frame->caller);
        
        kdb_printf("\n");

        frame = frame->prev;
    }

    if (frame_index >= nframes)
        kdb_printf("\tBacktrace continues...\n");

    goto out;

invalid:
    kdb_printf("Backtrace terminated-invalid frame pointer %p\n",frame);
out:

    /* Identify kernel modules in the backtrace and display their
     * load addresses and dependencies. This routine should walk
     * the kmod list safely.
     */
    if (frame_index)
        kmod_panic_dump((vm_offset_t *)&raddrs[0], frame_index);

    if (PC != 0)
        kmod_panic_dump(&PC, 1);

    panic_display_system_configuration();
}


void
DebuggerWithContext(
    __unused unsigned int   reason,
    __unused void       *ctx,
    const char      *message)
{
    Debugger(message);
}

void
Debugger(
    const char  *message)
{
    unsigned long pi_size = 0;
    void *stackptr;
    int cn = cpu_number();

    hw_atomic_add(&debug_mode, 1);   
    if (!panic_is_inited) {
        /* Halt forever. */
#ifdef _ARM_ARCH_7
        asm("cpsid if; wfi; b .");
#else
        asm("cpsid if; b .");
#endif
    }

    printf("Debugger called: <%s>\n", message);
    kprintf("Debugger called: <%s>\n", message);

    /*
     * Skip the graphical panic box if no panic string.
     * This is the case if we're being called from
     *   host_reboot(,HOST_REBOOT_DEBUGGER)
     * as a quiet way into the debugger.
     */

    if (panicstr) {
        disable_preemption();

        /* Obtain current frame pointer */
#if defined (__arm__)
        __asm__ volatile("mov %0, r7" : "=r" (stackptr));
#endif

        /* Print backtrace - callee is internally synchronized */
        panic_arm_backtrace(stackptr, 48, NULL, FALSE, NULL);

        /* Draw panic dialog if needed */
        draw_panic_dialog();
    }

    /* Enter KDP if necessary. */
    if(current_debugger)
        __asm__ __volatile__("bkpt #0");

    hw_atomic_sub(&debug_mode, 1);   
}

#define VECTORS_BASE 0xFFFF0000

/*
 * VBARNS support will break kdp for now.
 */
lowglo* lowGlo = (lowglo*)VECTORS_BASE;

extern void *flag_kdp_trigger_reboot;
extern void *manual_pkt;

/**
 * machine_startup
 *
 * Configure core kernel variables and go to Mach kernel bootstrap.
 */
void machine_startup(void)
{
    machine_conf();

    if (PE_parse_boot_argn("debug", &debug_boot_arg, sizeof(debug_boot_arg))) {
        panicDebugging = TRUE;
        if (debug_boot_arg & DB_HALT)
            halt_in_debugger = 1;
        if (debug_boot_arg & DB_PRT)
            disable_debug_output = FALSE;
        if (debug_boot_arg & DB_SLOG)
            systemLogDiags = TRUE;
        if (debug_boot_arg & DB_LOG_PI_SCRN)
            logPanicDataToScreen = TRUE;

        /*
         * Set up low globals
         */
        lowGlo->lgOSVersion = (uint32_t) version;
        lowGlo->lgRebootFlag = (uint32_t) &flag_kdp_trigger_reboot;
        lowGlo->lgManualPacket = (uint32_t) &manual_pkt;
    } else {
        debug_boot_arg = 0;
    }

    /*
     * Cause a breakpoint trap to the debugger before proceeding
     * any further if the proper option bit was specified in
     * the boot flags.
     */
    if (halt_in_debugger) {
        Debugger("inline call to debugger(machine_startup)");
        halt_in_debugger = 0;
        active_debugger = 1;
    }

    kernel_bootstrap();
    return;
}

/**
 * machine_boot_info
 *
 * Return string of boot args passed to kernel.
 */
char *machine_boot_info(char *buf, vm_size_t size)
{
    return (PE_boot_args());
}

#if defined(BOARD_CONFIG_QSD8250_LEO)
#include <kern/thread.h>
#include <kern/task.h>
/* Exact BSD process APIs, as used by existing OSFMK callers. */
extern int proc_selfpid(void);
extern void proc_selfname(char *buf, int size);
#include <libkern/OSAtomic.h>
#include "leo_user_frontier.h"
/* One boot-lifetime identity; DIAGNOSTIC only, no user memory reads. */
enum {LF_ARM=1,LF_TRIGGER_RETURN=2,LF_ENTRY=3,LF_RETURN=4,LF_WAIT=5,LF_SERVICE_REQUEST=6,LF_SERVICE_REPLY=7,LF_TRIGGER_REPLY=8,LF_LATE_WAIT=9};
enum {LF_MACH=1,LF_BSD=2,LF_SPECIAL=3};
typedef struct {
 uint32_t seq,kind,domain,code,r0,r1,pc,lr,sp,cpsr,r12,state,continuation,stage;
 uint64_t wait_event;
} leo_frontier_record;
typedef struct {
 volatile uint32_t guard,loss,unknown_loss,first_wait;
 thread_t thread;task_t task;uint64_t tid;uint32_t pid;
 uint32_t head,tail,entries,pending,pending_domain,pending_code,trigger_return,wait_return,reported_loss,reported_unknown;
 uint32_t service_entries,service_pending_token,service_pending_id,service_trigger_reply;
 volatile uint32_t late_wait;
 uint32_t activity_requests,activity_replies,activity_last_request,activity_last_reply,activity_status_kind,activity_status,activity_disposition,activity_saturated;
 uint64_t activity_tick;
 leo_frontier_record records[104];
} leo_frontier_state;
static leo_frontier_state leo_frontier;
volatile uint32_t ios7leo_frontier_phase;
const arm_saved_state_t * volatile ios7leo_frontier_uss;
typedef char leo_frontier_record_64[(sizeof(leo_frontier_record)==64)?1:-1];
typedef char leo_frontier_budget_8k[(sizeof(leo_frontier_state)<=8192)?1:-1];
static void lf_barrier(void){__asm__ __volatile__("dmb ish":::"memory");}
static void lf_loss(void)
{
 for(unsigned i=0;i<4;i++){
  uint32_t n=leo_frontier.loss;
  if(n==UINT32_MAX){leo_frontier.unknown_loss=1;return;}
  if(OSCompareAndSwap(n,n+1,&leo_frontier.loss))return;
 }
 leo_frontier.unknown_loss=1;
}
static int lf_service_target(thread_t self)
{
 if(ios7leo_frontier_phase<2 || ios7leo_frontier_phase>3)return 0;
 lf_barrier();
 return self==leo_frontier.thread && self->task==leo_frontier.task && self->thread_id==leo_frontier.tid;
}
static int lf_target(thread_t self)
{
 if(ios7leo_frontier_phase!=2)return 0;
 lf_barrier();
 return self==leo_frontier.thread && self->task==leo_frontier.task && self->thread_id==leo_frontier.tid;
}
static const arm_saved_state_t *lf_user(thread_t self)
{
 const arm_saved_state_t *s=self->machine.uss;
 return s==&self->machine.user_regs && (s->cpsr&0x1fU)==0x10U?s:0;
}
static void lf_record_locked(uint32_t kind,uint32_t domain,uint32_t code,
 const arm_saved_state_t *s,uint32_t state,uint64_t event,uint32_t continuation,uint32_t stage)
{
 if(leo_frontier.head>=104){lf_loss();return;}
 leo_frontier_record *r=&leo_frontier.records[leo_frontier.head];
 r->seq=leo_frontier.head+1;r->kind=kind;r->domain=domain;r->code=code;
 r->r0=s?s->r[0]:0;r->r1=s?s->r[1]:0;r->pc=s?s->pc:0;r->lr=s?s->lr:0;
 r->sp=s?s->sp:0;r->cpsr=s?s->cpsr:0;r->r12=s?s->r[12]:0;
 r->state=state;r->wait_event=event;r->continuation=continuation;r->stage=stage;
 lf_barrier();leo_frontier.head++;
}
void ios7leo_frontier_arm(void)
{
 char name[16]={0};int pid=proc_selfpid();if(pid<=0)return;
 proc_selfname(name,sizeof(name));if(strcmp(name,"backboardd"))return;
 thread_t self=current_thread();const arm_saved_state_t *s=lf_user(self);
 if(!s || !OSCompareAndSwap(0,1,&ios7leo_frontier_phase))return;
 /* Normal synchronous kobject caller context. References only, no allocation. */
 thread_reference(self);task_reference(self->task);
 leo_frontier.thread=self;leo_frontier.task=self->task;leo_frontier.tid=self->thread_id;leo_frontier.pid=(uint32_t)pid;
 lf_record_locked(LF_ARM,LF_MACH,s->r[12],s,0,0,0,0);
 ios7leo_frontier_uss=s;lf_barrier();ios7leo_frontier_phase=2;
}
static void lf_entry(const arm_saved_state_t *s,uint32_t domain,uint32_t code)
{
 thread_t self=current_thread();if(!lf_target(self))return;
 if(!s || s!=lf_user(self)){lf_loss();return;}
 if(!OSCompareAndSwap(0,1,&leo_frontier.guard)){lf_loss();return;}
 if(ios7leo_frontier_phase==2){
  if(leo_frontier.entries>=16 || leo_frontier.pending){lf_loss();ios7leo_frontier_phase=3;}
  else{leo_frontier.entries++;leo_frontier.pending=1;leo_frontier.pending_domain=domain;leo_frontier.pending_code=code;lf_record_locked(LF_ENTRY,domain,code,s,0,0,0,leo_frontier.entries);}
 }
 lf_barrier();leo_frontier.guard=0;
}
static void lf_return(const arm_saved_state_t *s,uint32_t domain,int error)
{
 thread_t self=current_thread();if(!lf_target(self))return;
 if(!s || s!=lf_user(self)){lf_loss();return;}
 if(!OSCompareAndSwap(0,1,&leo_frontier.guard)){lf_loss();return;}
 if(ios7leo_frontier_phase==2){
  if(leo_frontier.pending && leo_frontier.pending_domain==domain){
   uint32_t stage=leo_frontier.entries;
   if(leo_frontier.first_wait && !leo_frontier.wait_return){stage|=0x10000U;leo_frontier.wait_return=1;}
   lf_record_locked(LF_RETURN,domain,leo_frontier.pending_code,s,domain==LF_BSD?(uint32_t)error:0,0,0,stage);leo_frontier.pending=0;
   if(leo_frontier.entries==16)ios7leo_frontier_phase=3;
  }else if(!leo_frontier.pending && !leo_frontier.trigger_return){uint32_t stage=0;if(leo_frontier.first_wait&&!leo_frontier.wait_return){stage|=0x10000U;leo_frontier.wait_return=1;}lf_record_locked(LF_TRIGGER_RETURN,domain,s->r[12],s,0,0,0,stage);leo_frontier.trigger_return=1;}
  else{lf_loss();ios7leo_frontier_phase=3;}
 }
 lf_barrier();leo_frontier.guard=0;
}
void ios7leo_frontier_mach_entry(const arm_saved_state_t *s)
{
 if(ios7leo_frontier_phase!=2||!s)return;
 int32_t code=(int32_t)s->r[12];if(code>=0)return;
 lf_entry(s,code==INT32_MIN?LF_SPECIAL:LF_MACH,(uint32_t)code);
}
void ios7leo_frontier_mach_return(const arm_saved_state_t *s)
{
 if(ios7leo_frontier_phase!=2||!s)return;
 int32_t code=(int32_t)s->r[12];if(code>=0)return;
 lf_return(s,code==INT32_MIN?LF_SPECIAL:LF_MACH,0);
}
void ios7leo_frontier_bsd_entry(const arm_saved_state_t *s,uint32_t code){lf_entry(s,LF_BSD,code);}
void ios7leo_frontier_bsd_return(const arm_saved_state_t *s,int error){lf_return(s,LF_BSD,error);}
uint32_t ios7leo_frontier_service_begin(uint32_t id,uint32_t size,uint32_t bits,uint32_t routine,uint32_t selector)
{
 if(!lf_service_target(current_thread()))return 0;
 if(!OSCompareAndSwap(0,1,&leo_frontier.guard)){lf_loss();return 0;}
 if(leo_frontier.activity_requests!=UINT32_MAX)leo_frontier.activity_requests++;else leo_frontier.activity_saturated=1;
 leo_frontier.activity_last_request=id;leo_frontier.activity_tick=mach_absolute_time();
 uint32_t token=0;
 if(leo_frontier.service_entries<32 && !leo_frontier.service_pending_token && leo_frontier.head<104){
  token=++leo_frontier.service_entries;leo_frontier.service_pending_token=token;leo_frontier.service_pending_id=id;
  lf_record_locked(LF_SERVICE_REQUEST,4,id,0,0,0,0,token);
  leo_frontier_record *r=&leo_frontier.records[leo_frontier.head-1];
  r->r0=size;r->r1=bits;r->pc=routine;r->lr=selector;
 }else if(leo_frontier.service_pending_token || leo_frontier.head>=104){lf_loss();leo_frontier.unknown_loss=1;}
 lf_barrier();leo_frontier.guard=0;return token;
}
void ios7leo_frontier_service_reply(uint32_t token,uint32_t id,uint32_t selector,uint32_t reply_id,uint32_t size,uint32_t bits,uint32_t status_kind,uint32_t status,uint32_t descriptors,uint32_t disposition)
{
 if(!lf_service_target(current_thread()))return;
 if(!OSCompareAndSwap(0,1,&leo_frontier.guard)){lf_loss();return;}
 if(leo_frontier.activity_replies!=UINT32_MAX)leo_frontier.activity_replies++;else leo_frontier.activity_saturated=1;
 leo_frontier.activity_last_reply=id;leo_frontier.activity_status_kind=status_kind;
 leo_frontier.activity_status=status_kind?status:0;leo_frontier.activity_disposition=disposition;
 leo_frontier.activity_tick=mach_absolute_time();
 uint32_t kind=0;
 if(token){
  if(token==leo_frontier.service_pending_token && id==leo_frontier.service_pending_id){
   kind=LF_SERVICE_REPLY;leo_frontier.service_pending_token=0;
  }else{lf_loss();leo_frontier.unknown_loss=1;}
 }else if(!leo_frontier.service_trigger_reply && !leo_frontier.service_entries && id==2865 && selector==25){
  /* Arming occurs inside this successful original MIG call, so no entry
   * record exists. This reply must never masquerade as a paired request. */
  kind=LF_TRIGGER_REPLY;leo_frontier.service_trigger_reply=1;
 }
 if(kind && leo_frontier.head<104){
  lf_record_locked(kind,4,id,0,disposition,0,0,token);
  leo_frontier_record *r=&leo_frontier.records[leo_frontier.head-1];
  r->r0=reply_id;r->r1=size;r->pc=bits;r->lr=status_kind;
  r->sp=status_kind?status:0;r->r12=descriptors;
 }else if(kind){lf_loss();leo_frontier.unknown_loss=1;}
 lf_barrier();leo_frontier.guard=0;
}
uint32_t ios7leo_frontier_activity(struct ios7leo_service_activity *out)
{
 if(!out || ios7leo_frontier_phase<2)return 0;
 if(!OSCompareAndSwap(0,1,&leo_frontier.guard))return 0;
 lf_barrier();
 out->pid=leo_frontier.pid;out->tid=leo_frontier.tid;
 out->requests=leo_frontier.activity_requests;out->replies=leo_frontier.activity_replies;
 out->last_request=leo_frontier.activity_last_request;out->last_reply=leo_frontier.activity_last_reply;
 out->status_kind=leo_frontier.activity_status_kind;out->status=leo_frontier.activity_status;
 out->disposition=leo_frontier.activity_disposition;out->saturated=leo_frontier.activity_saturated;
 out->service_pairs=leo_frontier.service_entries;out->limit_reached=leo_frontier.service_entries>=32;
 out->late_wait=leo_frontier.late_wait;out->loss=leo_frontier.loss;out->unknown=leo_frontier.unknown_loss;
 out->last_tick=leo_frontier.activity_tick;
 lf_barrier();leo_frontier.guard=0;return 1;
}
void ios7leo_frontier_wait(thread_t self)
{
 if(!(self->state&TH_WAIT)||!lf_service_target(self))return;
 unsigned late=ios7leo_frontier_phase==3;
 volatile uint32_t *flag=late?&leo_frontier.late_wait:&leo_frontier.first_wait;
 if(!OSCompareAndSwap(0,1,flag))return;
 if(!OSCompareAndSwap(0,1,&leo_frontier.guard)){lf_loss();leo_frontier.unknown_loss=1;return;}
 if(late)lf_record_locked(LF_LATE_WAIT,leo_frontier.service_pending_token?4:0,leo_frontier.service_pending_id,lf_user(self),(uint32_t)self->state,(uint64_t)self->wait_event,(uint32_t)(uintptr_t)self->continuation,3);
 else lf_record_locked(LF_WAIT,leo_frontier.pending_domain,leo_frontier.pending_code,lf_user(self),(uint32_t)self->state,(uint64_t)self->wait_event,(uint32_t)(uintptr_t)self->continuation,leo_frontier.pending?2U:(leo_frontier.trigger_return?1U:0U));
 lf_barrier();leo_frontier.guard=0;
}
uint32_t ios7leo_frontier_take(uint8_t *payload,uint32_t maximum,uint32_t *lost,uint32_t *unknown_out)
{
 /* Called only by the normal logger. Pop/copy under tryguard, then format a
  * whole existing kind1 record payload outside all producer/IRQ locks. */
 if(!payload||!maximum||!lost||!unknown_out||ios7leo_frontier_phase<2)return 0;
 {
  leo_frontier_record r;uint32_t loss,unknown,pid;uint64_t tid;
  if(!OSCompareAndSwap(0,1,&leo_frontier.guard))return 0;
  lf_barrier();
  if(leo_frontier.tail==leo_frontier.head){
   loss=leo_frontier.loss;unknown=leo_frontier.unknown_loss;
   unsigned report=loss!=leo_frontier.reported_loss || unknown!=leo_frontier.reported_unknown;
   leo_frontier.reported_loss=loss;leo_frontier.reported_unknown=unknown;lf_barrier();leo_frontier.guard=0;
   if(!report)return 0;
   *lost=loss;*unknown_out=unknown;
   int n=snprintf((char *)payload,maximum,"UIEVENT LOSS=%u UNK=%u missing-events-not-blockage-proof\n",loss,unknown);
   return n>0&&(uint32_t)n<maximum?(uint32_t)n:0;
  }
  r=leo_frontier.records[leo_frontier.tail++];loss=leo_frontier.loss;unknown=leo_frontier.unknown_loss;pid=leo_frontier.pid;tid=leo_frontier.tid;
  lf_barrier();leo_frontier.guard=0;
  *lost=loss;*unknown_out=unknown;
  if(r.kind==LF_SERVICE_REQUEST || r.kind==LF_SERVICE_REPLY || r.kind==LF_TRIGGER_REPLY){
   int n;
   if(r.kind==LF_SERVICE_REQUEST)n=snprintf((char *)payload,maximum,"UISERVICE S=%u PID=%u TID=%llu K=REQUEST TOKEN=%u ID=%u SIZE=%u BITS=%08x ROUTINE=%08x SELECTOR=%u LOSS=%u UNK=%u trusted-kernel-message\n",r.seq,pid,(unsigned long long)tid,r.stage,r.code,r.r0,r.r1,r.pc,r.lr,loss,unknown);
   else n=snprintf((char *)payload,maximum,"UISERVICE S=%u PID=%u TID=%llu K=%s TOKEN=%u REQID=%u REPLYID=%u SIZE=%u BITS=%08x STATUS_KIND=%u STATUS_VALID=%u STATUS=%08x DESCRIPTORS=%u DISPOSITION=%u LOSS=%u UNK=%u no-common-status-for-unknown-complex\n",r.seq,pid,(unsigned long long)tid,r.kind==LF_TRIGGER_REPLY?"TRIGGER_REPLY_WITHOUT_ENTRY":"REPLY",r.stage,r.code,r.r0,r.r1,r.pc,r.lr,r.lr!=0,r.sp,r.r12,r.state,loss,unknown);
   if(n<=0||(uint32_t)n>=maximum){lf_loss();leo_frontier.unknown_loss=1;return 0;}
   return (uint32_t)n;
  }
  if(r.kind==LF_LATE_WAIT){
   int n=snprintf((char *)payload,maximum,"UIWAIT_LATE S=%u PID=%u TID=%llu STATE=%08x EVENT=%016llx CONT=%08x PC=%08x LR=%08x SP=%08x CPSR=%08x R12=%08x SERVICE_PENDING=%u LAST_SERVICE_ID=%u LOSS=%u UNK=%u post-trap-budget-no-followup-Mach-return-observation-not-hang-proof\n",r.seq,pid,(unsigned long long)tid,r.state,(unsigned long long)r.wait_event,r.continuation,r.pc,r.lr,r.sp,r.cpsr,r.r12,r.domain==4,r.code,loss,unknown);
   if(n<=0||(uint32_t)n>=maximum){lf_loss();leo_frontier.unknown_loss=1;return 0;}
   return (uint32_t)n;
  }
  int n=snprintf((char *)payload,maximum,"UIEVENT S=%u K=%u PID=%u TID=%llu D=%u C=%08x R0=%08x R1=%08x PC=%08x LR=%08x SP=%08x CPSR=%08x R12=%08x ST_OR_BSDERR=%08x EV=%016llx CONT=%08x PH=%u LOSS=%u UNK=%u raw-user-trap;return-flag-not-wake-time\n",r.seq,r.kind,pid,(unsigned long long)tid,r.domain,r.code,r.r0,r.r1,r.pc,r.lr,r.sp,r.cpsr,r.r12,r.state,(unsigned long long)r.wait_event,r.continuation,r.stage,loss,unknown);
  if(n<=0||(uint32_t)n>=maximum){lf_loss();leo_frontier.unknown_loss=1;return 0;}
  return (uint32_t)n;
 }
}
#endif

/**
 * mach_syscall_trace
 */
extern const char *mach_syscall_name_table[];
void mach_syscall_trace(arm_saved_state_t * state)
{
#if BOARD_CONFIG_ARMPBA8
    static unsigned int lab_time_logs = 0;
    if ((int32_t)state->r[12] == -3 && lab_time_logs++ < 6)
        printf("IOS7LAB absolute_time=%08x:%08x actual-kernel-clock\n", state->r[1], state->r[0]);
    static unsigned int lab_task_logs = 0;
    int enabled = 0;
    int32_t trap = (int32_t)state->r[12];
    if ((trap == -44 || trap == -45 || trap == -46) && lab_task_logs < 32 &&
        PE_parse_boot_argn("ios7lab_syscalls", &enabled, sizeof(enabled)) && enabled == 1) {
        ++lab_task_logs;
        printf("IOS7LAB task-identity trap=%d result=%x argument1=%x argument2=%x\n",
               trap, state->r[0], state->r[1], state->r[2]);
    }
#endif
}

/*
 * Halt a cpu.
 */
void halt_cpu(void)
{
    halt_all_cpus(FALSE);
}

int reset_mem_on_reboot = 1;
/*
 * Halt the system or reboot.
 */
void halt_all_cpus(boolean_t reboot)
{
    if (reboot) {
        printf("MACH Reboot\n");
        if (PE_halt_restart)
            (*PE_halt_restart) (kPERestartCPU);
    } else {
        printf("CPU halted\n");
        if (PE_halt_restart)
            (*PE_halt_restart) (kPEHaltCPU);
    }
    while (1) ;
}
