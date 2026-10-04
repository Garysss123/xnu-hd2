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
 * ARM trap handlers.
 */

#include <mach/mach_types.h>
#include <mach/mach_traps.h>
#include <mach/thread_status.h>
#include <mach_assert.h>
#include <mach_kdp.h>
#include <kern/thread.h>
#include <kern/kalloc.h>
#include <stdarg.h>
#include <vm/vm_kern.h>
#include <vm/pmap.h>
#include <stdarg.h>
#include <machine/machine_routines.h>
#include <arm/misc_protos.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm/boot.h>
#include <pexpert/arm/protos.h>
#include <vm/vm_fault.h>
#include <vm/vm_kern.h>         /* For kernel_map */
#include <libkern/OSByteOrder.h>
#include <libsa/string.h>
#include <arm/armops.h>
#if defined(BOARD_CONFIG_QSD8250_LEO)
#include "ios7lab_fault_witness.h"
#endif
struct proc;
extern char *proc_name_address(void *p);
extern int proc_pid(struct proc *p);

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"


/* IOS7LEO_DYLD_BEGIN */
#if defined(BOARD_CONFIG_QSD8250_LEO)

#include <kern/misc_protos.h>
#include <libkern/OSAtomic.h>
#include <sys/errno.h>
static volatile UInt32 ios7leo_dyld_phase;
static task_t ios7leo_dyld_task;
static arm_saved_state_t ios7leo_dyld_state;
static boolean_t ios7leo_dyld_matches(thread_t thread,arm_saved_state_t *state,uint32_t instruction)
{
    uint64_t info;
    if(!thread || !thread->task || !thread->task->bsd_info ||
       (state->cpsr & 0x3fU)!=0x10U || instruction!=0xfedeffe7U ||
       proc_pid(thread->task->bsd_info)!=1 ||
       strcmp(proc_name_address(thread->task->bsd_info),"launchd")!=0 ||
       thread->task->all_image_info_size!=0xa4U) return FALSE;
    info=thread->task->all_image_info_addr;
    /* Current pinned dyld offsets; no assumed ASLR slide or direct user dereference. */
    if(info<0x20bc8U+0x1000U || info>=0x40000000ULL ||
       info+0xa4U>0x40000000ULL || info+0x278d8U+512U>0x40000000ULL) return FALSE;
    return state->pc==info-0x20bc8U && state->r[0]==info+0x278d8U;
}

static boolean_t ios7leo_dyld_repeat(task_t task,arm_saved_state_t *state)
{
    if(ios7leo_dyld_phase!=2U) return FALSE;
    OSMemoryBarrier();
    return task==ios7leo_dyld_task && bcmp(state,&ios7leo_dyld_state,sizeof(*state))==0;
}
__attribute__((noinline)) static void ios7leo_dyld_witness(task_t task,arm_saved_state_t *state)
{
    char message[512];vm_size_t copied=0;unsigned used=0,index;int error;
    if(!OSCompareAndSwap(0U,1U,&ios7leo_dyld_phase)) return;
    ios7leo_dyld_task=task;ios7leo_dyld_state=*state;
    bzero(message,sizeof(message));
    error=copyinstr((user_addr_t)state->r[0],message,sizeof(message),&copied);
    message[sizeof(message)-1]=0;
    printf("Leo DYLD witness err=%d len=%u trunc=%u\n",error,(unsigned)copied,(unsigned)(error==ENAMETOOLONG));
    if(error==0 || error==ENAMETOOLONG) {
        while(used<sizeof(message)-1 && message[used]) {
            unsigned char ch=(unsigned char)message[used];
            if(ch<0x20U || ch>0x7eU) message[used]='?';
            ++used;
        }
        for(index=0;index<used;index+=48U) {
            unsigned count=used-index;if(count>48U)count=48U;
            printf("dyld> %.*s\n",(int)count,message+index);
        }
        if(!used)printf("dyld> <empty>\n");
    } else printf("dyld> <copy failed; no data printed>\n");
    OSMemoryBarrier();ios7leo_dyld_phase=2U;
}

#endif
/* IOS7LEO_DYLD_END */
typedef enum {
    SLEH_ABORT_TYPE_PREFETCH_ABORT = 3,
    SLEH_ABORT_TYPE_DATA_ABORT = 4,
} sleh_abort_reasons;

void doexception(int exc, mach_exception_code_t code,
                 mach_exception_subcode_t sub)
{
    mach_exception_data_type_t codes[EXCEPTION_CODE_MAX];

    codes[0] = code;
    codes[1] = sub;
    exception_triage(exc, codes, 2);
}

/**
 * ifsr_to_human
 *
 * Return a human readable representation of the IFSR bits.
 */
static char *ifsr_to_human(uint32_t ifsr)
{
    switch ((ifsr & 0xF)) {
    case 0:
        return "No function, reset value";
    case 1:
        return "Alignment fault";
    case 2:
        return "Debug event fault";
    case 3:
        return "Access flag fault on section";
    case 4:
        return "No function";
    case 5:
        return "Translation fault on section";
    case 6:
        return "Access flag fault on page";
    case 7:
        return "Translation fault on page";
    case 8:
        return "Precise external abort";
    case 9:
        return "Domain fault on section";
    case 10:
        return "No function";
    case 11:
        return "Domain fault on page";
    case 12:
        return "External abort on translation, level one";
    case 13:
        return "Permission fault on section";
    case 14:
        return "External abort on translation, level two";
    case 15:
        return "Permission fault on page";
    default:
        return "Unknown";
    }
    return "Unknown";
}

/**
 * sleh_fatal_exception
 */
void sleh_fatal_exception(abort_information_context_t * arm_ctx, char *message)
{
#if defined(BOARD_CONFIG_QSD8250_LEO)
    extern int ios7leo_fatal_report(unsigned int,unsigned int,unsigned int,
        unsigned int,unsigned int,unsigned int,unsigned int,const char *);
    /* Saved exception fields from the original vector context, no new probes.
     * Ordinary fatal handler/Debugger/Halt_system below stays byte-for-byte. */
    if(arm_ctx) ios7leo_fatal_report(0xf102U,
       ((unsigned int)__builtin_return_address(0)) & ~1U,
       arm_ctx->pc,arm_ctx->lr,arm_ctx->far,arm_ctx->fsr,15U,message);
    else ios7leo_fatal_report(0xf102U,
       ((unsigned int)__builtin_return_address(0)) & ~1U,0,0,0,0,0,message);
#endif
    debug_mode = TRUE;
    printf("Fatal exception: %s\n", message);
    printf("ARM register state: (saved state %p)\n"
           "  r0: 0x%08x  r1: 0x%08x  r2: 0x%08x  r3: 0x%08x\n"
           "  r4: 0x%08x  r5: 0x%08x  r6: 0x%08x  r7: 0x%08x\n"
           "  r8: 0x%08x  r9: 0x%08x r10: 0x%08x r11: 0x%08x\n"
           " r12: 0x%08x  sp: 0x%08x  lr: 0x%08x  pc: 0x%08x\n"
           "cpsr: 0x%08x fsr: 0x%08x far: 0x%08x\n", arm_ctx,
           arm_ctx->r[0], arm_ctx->r[1], arm_ctx->r[2], arm_ctx->r[3],
           arm_ctx->r[4], arm_ctx->r[5], arm_ctx->r[6], arm_ctx->r[7],
           arm_ctx->r[8], arm_ctx->r[9], arm_ctx->r[10], arm_ctx->r[11],
           arm_ctx->r[12], arm_ctx->sp, arm_ctx->lr, arm_ctx->pc, arm_ctx->cpsr,
           arm_ctx->fsr, arm_ctx->far);
    printf("Current thread: %p\n", current_thread());

    uint32_t ttbcr, ttbr0, ttbr1;
    __asm__ __volatile__("mrc p15, 0, %0, c2, c0, 0":"=r"(ttbr0));
    __asm__ __volatile__("mrc p15, 0, %0, c2, c0, 1":"=r"(ttbr1));
    __asm__ __volatile__("mrc p15, 0, %0, c2, c0, 2":"=r"(ttbcr));

    printf("Control registers:\n"
           "  ttbcr: 0x%08x  ttbr0:  0x%08x  ttbr1:  0x%08x\n",
           ttbcr, ttbr0, ttbr1);
    Debugger("fatal exception");
    printf("We are hanging here ...\n");

    Halt_system();
}

/**
 * sleh_abort
 *
 * Handle prefetch and data aborts. (EXC_BAD_ACCESS IS NOT HERE YET)
 */
/* IOS7LEO_INSTALLD_WITNESS_BEGIN */
#if BOARD_CONFIG_QSD8250_LEO
static volatile UInt32 ios7leo_installd_observed;
__attribute__((noinline)) static void
ios7leo_installd_fault(thread_t thread, const abort_information_context_t *ctx,
                      kern_return_t fault_result)
{
    uint32_t instruction[4] = {0,0,0,0};
    uint32_t pc=ctx->pc, far=ctx->far, fsr=ctx->fsr, psr=ctx->cpsr;
    int error;
    if(!thread || !thread->task || !thread->task->bsd_info ||
       (psr & 0x1fU)!=0x10U ||
       strcmp(proc_name_address(thread->task->bsd_info), "installd")!=0 ||
       !OSCompareAndSwap(0U,1U,&ios7leo_installd_observed)) return;
    /* One existing failed data abort, after the original IRQ-enable point.
     * No recovery/retry or protection change; preserve exception delivery. */
    printf("Leo installd fault result=%d FSR=%08x FAR=%08x\n",
           (int)fault_result,(unsigned)fsr,(unsigned)far);
    printf("Leo installd PC=%08x CPSR=%08x WNR=%u\n",
           (unsigned)pc,(unsigned)psr,(unsigned)((fsr>>11)&1U));
    if(pc<0x1004U || pc>0x40000000U-12U) {
        printf("Leo installd instruction range rejected\n");
        return;
    }
    error=copyin((user_addr_t)(pc-4U),instruction,sizeof(instruction));
    printf("Leo installd instruction copyin=%d start=%08x bytes=16\n",
           error,(unsigned)(pc-4U));
    if(error==0)
        printf("Leo installd LE32 %08x %08x %08x %08x\n",
               (unsigned)instruction[0],(unsigned)instruction[1],
               (unsigned)instruction[2],(unsigned)instruction[3]);
}
#endif
/* IOS7LEO_INSTALLD_WITNESS_END */

static int __abort_count = 0;
void sleh_abort(void *context, int reason)
{
#if BOARD_CONFIG_QSD8250_LEO
    kern_return_t leo_installd_fault_result=KERN_SUCCESS;
    boolean_t leo_installd_failed=FALSE;
#endif
#if defined(BOARD_CONFIG_QSD8250_LEO)
    kern_return_t ios7lab_exception_code=KERN_SUCCESS;
#endif
    uint32_t dfsr = 0, dfar = 0, ifsr = 0, ifar = 0, cpsr, exception_type =
        0, exception_subcode = 0;
    abort_information_context_t *arm_ctx =
        (abort_information_context_t *) context;
    thread_t thread = current_thread();

    /*
     * Make sure we get the correct registers only if required.
     */
#if 0
    kprintf("sleh_abort: pc %x lr %x far %x fsr %x psr %x\n", arm_ctx->pc, arm_ctx->lr, arm_ctx->far, arm_ctx->fsr, arm_ctx->cpsr);
#endif
    if (reason == SLEH_ABORT_TYPE_DATA_ABORT) {
        dfsr = arm_ctx->fsr;
        dfar = arm_ctx->far;
    } else if (reason == SLEH_ABORT_TYPE_PREFETCH_ABORT) {
        ifsr = arm_ctx->fsr;
        ifar = arm_ctx->far;
    } else {
        sleh_fatal_exception(arm_ctx, "sleh_abort: weird abort");
    }

    /*
     * We do not want anything entering sleh_abort recursively.
     */
    if (__abort_count != 0) {
        sleh_fatal_exception(arm_ctx, "sleh_abort: recursive abort");
    }
    __abort_count++;

    /*
     * Panic if it's an alignment fault?
     */
    if ((ifsr == 1) || (dfsr == 1)) {
        sleh_fatal_exception(arm_ctx, "sleh_abort: alignment fault");
    }

    if (!kernel_map) {
        sleh_fatal_exception(arm_ctx,
                             "sleh_abort: kernel map is NULL, probably a fault before vm_bootstrap?");
    }

    if (!thread) {
        sleh_fatal_exception(arm_ctx, "sleh_abort: current thread is null?");
    }

    if(ml_at_interrupt_context()) {
        sleh_fatal_exception(arm_ctx, "sleh_abort: Abort in interrupt handler");
    }

    /*
     * See if the abort was in Kernel or User mode.
     */
    cpsr = arm_ctx->cpsr & 0x1F;

    /*
     * Kernel mode. (ARM Supervisor)
     */
    if (cpsr == 0x13) {
        switch (reason) {
            /*
             * Prefetch aborts always include the IFSR and IFAR.
             */
        case SLEH_ABORT_TYPE_PREFETCH_ABORT:{
                /*
                 * Die in a fire.
                 */
                vm_map_t map;
                kern_return_t code;

                /*
                 * Get the kernel thread map.
                 */
                map = kernel_map;

                /*
                 * Attempt to fault the page.
                 */
                __abort_count--;
                code =
                    vm_fault(map, vm_map_trunc_page(arm_ctx->pc),
                             (VM_PROT_EXECUTE | VM_PROT_READ), FALSE,
                             THREAD_UNINT, NULL, vm_map_trunc_page(0));

                if (code != KERN_SUCCESS) {

                    if (current_debugger) {
                        if (kdp_raise_exception(EXC_BREAKPOINT, 0, 0, arm_ctx))
                            return;
                    }

                    /*
                     * Still, die in a fire.
                     */
                    panic_context(0, (void *) arm_ctx,
                                  "Kernel prefetch abort. (faulting address: 0x%08x, saved state 0x%08x)\n"
                                  "  r0: 0x%08x  r1: 0x%08x  r2: 0x%08x  r3: 0x%08x\n"
                                  "  r4: 0x%08x  r5: 0x%08x  r6: 0x%08x  r7: 0x%08x\n"
                                  "  r8: 0x%08x  r9: 0x%08x r10: 0x%08x r11: 0x%08x\n"
                                  " r12: 0x%08x  sp: 0x%08x  lr: 0x%08x  pc: 0x%08x\n"
                                  "cpsr: 0x%08x fsr: 0x%08x far: 0x%08x\n",
                                  ifar, arm_ctx, arm_ctx->r[0], arm_ctx->r[1],
                                  arm_ctx->r[2], arm_ctx->r[3], arm_ctx->r[4],
                                  arm_ctx->r[5], arm_ctx->r[6], arm_ctx->r[7],
                                  arm_ctx->r[8], arm_ctx->r[9], arm_ctx->r[10],
                                  arm_ctx->r[11], arm_ctx->r[12], arm_ctx->sp,
                                  arm_ctx->lr, arm_ctx->pc, arm_ctx->cpsr, ifsr,
                                  ifar);
                }
                return;
            }
        case SLEH_ABORT_TYPE_DATA_ABORT:{
                vm_map_t map;
                kern_return_t code;

                /*
                 * Get the current thread map.
                 */
                map = thread->map;

                /*
                 * Attempt to fault the page.
                 */
                __abort_count--;
                code =
                    vm_fault(map, vm_map_trunc_page(dfar),
                             (dfsr & 0x800) ? (VM_PROT_READ | VM_PROT_WRITE)
                             : (VM_PROT_READ), FALSE, THREAD_UNINT, NULL,
                             vm_map_trunc_page(0));

                if (code != KERN_SUCCESS) {
                    /*
                     * Still, die in a fire.
                     */
                    code =
                        vm_fault(kernel_map, vm_map_trunc_page(dfar),
                                 (dfsr & 0x800) ? (VM_PROT_READ | VM_PROT_WRITE)
                                 : (VM_PROT_READ), FALSE, THREAD_UNINT, NULL,
                                 vm_map_trunc_page(0));
                    if (code != KERN_SUCCESS) {
                        /*
                         * Attempt to fault the page against the kernel map.
                         */
                        if (!thread->recover) {
                            panic_context(0, (void *) arm_ctx,
                                          "Kernel data abort. (faulting address: 0x%08x, saved state 0x%08x)\n"
                                          "  r0: 0x%08x  r1: 0x%08x  r2: 0x%08x  r3: 0x%08x\n"
                                          "  r4: 0x%08x  r5: 0x%08x  r6: 0x%08x  r7: 0x%08x\n"
                                          "  r8: 0x%08x  r9: 0x%08x r10: 0x%08x r11: 0x%08x\n"
                                          " r12: 0x%08x  sp: 0x%08x  lr: 0x%08x  pc: 0x%08x\n"
                                          "cpsr: 0x%08x fsr: 0x%08x far: 0x%08x\n",
                                          dfar, arm_ctx, arm_ctx->r[0], arm_ctx->r[1],
                                          arm_ctx->r[2], arm_ctx->r[3],
                                          arm_ctx->r[4], arm_ctx->r[5],
                                          arm_ctx->r[6], arm_ctx->r[7],
                                          arm_ctx->r[8], arm_ctx->r[9],
                                          arm_ctx->r[10], arm_ctx->r[11],
                                          arm_ctx->r[12], arm_ctx->sp,
                                          arm_ctx->lr, arm_ctx->pc,
                                          arm_ctx->cpsr, dfsr, dfar);
                        } else {
                            /*
                             * If there's a recovery routine, use it.
                             */
                            if (thread->map == kernel_map)
                                panic
                                    ("Attempting to use a recovery routine on a kernel map thread");

                            if (!thread->map)
                                sleh_fatal_exception(arm_ctx,
                                                     "Current thread has no thread map, what?");

                            arm_ctx->pc = thread->recover;
                            arm_ctx->cpsr &= ~(1 << 5);
                            thread->recover = NULL;
                            return;
                        }
                    }
                }
                return;
            }
        default:
            panic("sleh_abort: unknown kernel mode abort, type %d\n", reason);
        }
        /*
         * User mode (ARM User)
         */
    } else if (cpsr == 0x10) {
        switch (reason) {
            /*
             * User prefetch abort
             */
        case SLEH_ABORT_TYPE_PREFETCH_ABORT:{
                /*
                 * Attempt to fault it. Same as data except address comes from IFAR.
                 */
                vm_map_t map;
                kern_return_t code;

                /*
                 * Get the current thread map.
                 */
                map = thread->map;
                /*
                 * Attempt to fault the page.
                 */
                assert(get_preemption_level() == 0);
                __abort_count--;
                code =
                    vm_fault(map, vm_map_trunc_page(arm_ctx->pc),
                             (VM_PROT_EXECUTE | VM_PROT_READ), FALSE,
                             THREAD_UNINT, NULL, vm_map_trunc_page(0));

                /*
                 * Additionally, see if we can fault one page higher as the instruction
                 * may be on a page split boundary. libobjc and all require this???
                 *
                 * Prefaulting the instruction before allows the prefetch mechanism
                 * to not abort.
                 */
                if((arm_ctx->pc & 0xfff) >= 0xff0)
                    vm_fault(map, vm_map_trunc_page(arm_ctx->pc) + PAGE_SIZE,
                             (VM_PROT_EXECUTE | VM_PROT_READ), FALSE,
                             THREAD_UNINT, NULL, vm_map_trunc_page(0));

                if ((code != KERN_SUCCESS) && (code != KERN_ABORTED)) {
                    exception_type = EXC_BAD_ACCESS;
                    exception_subcode = 0;
#if defined(BOARD_CONFIG_QSD8250_LEO)
                    ios7lab_exception_code = code;
#endif

                    /*
                     * Debug only.
                     */
                    printf
                        (ANSI_COLOR_RED "%s[%d]: " ANSI_COLOR_YELLOW "usermode prefetch abort, EXC_BAD_ACCESS at 0x%08x in map %p (pmap %p) (%s)" ANSI_COLOR_RESET" \n",
                         proc_name_address(thread->task->bsd_info),
                         proc_pid(thread->task->bsd_info), arm_ctx->pc, map,
                         map->pmap, ifsr_to_human(ifsr));
                    printf("Thread has ARM register state:\n"
                           "    r0: 0x%08x  r1: 0x%08x  r2: 0x%08x  r3: 0x%08x\n"
                           "    r4: 0x%08x  r5: 0x%08x  r6: 0x%08x  r7: 0x%08x\n"
                           "    r8: 0x%08x  r9: 0x%08x r10: 0x%08x r11: 0x%08x\n"
                           "   r12: 0x%08x  sp: 0x%08x  lr: 0x%08x  pc: 0x%08x\n"
                           "  cpsr: 0x%08x\n", arm_ctx->r[0], arm_ctx->r[1],
                           arm_ctx->r[2], arm_ctx->r[3], arm_ctx->r[4],
                           arm_ctx->r[5], arm_ctx->r[6], arm_ctx->r[7],
                           arm_ctx->r[8], arm_ctx->r[9], arm_ctx->r[10],
                           arm_ctx->r[11], arm_ctx->r[12], arm_ctx->sp,
                           arm_ctx->lr, arm_ctx->pc, arm_ctx->cpsr);
                    printf("dyld_all_image_info_addr: 0x%08x   dyld_all_image_info_size: 0x%08x\n",
                            thread->task->all_image_info_addr, thread->task->all_image_info_size);

                } else {
                    /*
                     * Retry execution of instruction.
                     */
                    ml_set_interrupts_enabled(TRUE);
                    return;
                }
                break;
            }
            /*
             * User Data Abort
             */
        case SLEH_ABORT_TYPE_DATA_ABORT:{
                /*
                 * Attempt to fault it. Same as instruction except address comes from DFAR.
                 */
                vm_map_t map;
                kern_return_t code;

                /*
                 * Get the current thread map.
                 */
                map = thread->map;

                /*
                 * Attempt to fault the page.
                 */
                assert(get_preemption_level() == 0);
                __abort_count--;
                code =
                    vm_fault(map, vm_map_trunc_page(dfar),
                             (dfsr & 0x800) ? (VM_PROT_READ | VM_PROT_WRITE)
                             : (VM_PROT_READ), FALSE, THREAD_UNINT, NULL,
                             vm_map_trunc_page(0));
                if ((code != KERN_SUCCESS) && (code != KERN_ABORTED)) {
#if BOARD_CONFIG_QSD8250_LEO
                    leo_installd_fault_result=code;
                    leo_installd_failed=TRUE;
#endif
#if BOARD_CONFIG_ARMPBA8
                    /* Bounded ordinary crash backtrace for the lab's UI process.
                     * Copy only frame links from the faulting thread's own stack;
                     * preserve the original exception path and stop on any error. */
                    {
                        static unsigned ios7lab_ui_crash_traces;
                        if (!strncmp(proc_name_address(thread->task->bsd_info), "SpringBoard", 12) &&
                            ios7lab_ui_crash_traces < 2 && arm_ctx->sp < 0x3fff0000U) {
                            uint32_t frame = arm_ctx->r[7];
                            uint32_t ceiling = arm_ctx->sp + 65536U;
                            unsigned depth;
                            ++ios7lab_ui_crash_traces;
                            for (depth = 0; depth < 10; ++depth) {
                                uint32_t link[2];
                                if ((frame & 3U) || frame < arm_ctx->sp || frame > ceiling - sizeof(link)) break;
                                if (copyin((void *)(uintptr_t)frame, link, sizeof(link))) break;
                                printf("IOS7LAB UI crash frame=%u return=%08x\n", depth, link[1]);
                                if (link[0] <= frame) break;
                                frame = link[0];
                            }
                        }
                    }
#endif
                    exception_type = EXC_BAD_ACCESS;
                    exception_subcode = 0;

                    /*
                     * Only for debug.
                     */
                    printf
                        (ANSI_COLOR_RED "%s[%d]: " ANSI_COLOR_BLUE "usermode data abort, EXC_BAD_ACCESS at 0x%08x in map %p (pmap %p) (%s)" ANSI_COLOR_RESET "\n",
                         proc_name_address(thread->task->bsd_info),
                         proc_pid(thread->task->bsd_info), dfar, map, map->pmap,
                         ifsr_to_human(dfsr));
                    printf("Thread has ARM register state:\n"
                           "    r0: 0x%08x  r1: 0x%08x  r2: 0x%08x  r3: 0x%08x\n"
                           "    r4: 0x%08x  r5: 0x%08x  r6: 0x%08x  r7: 0x%08x\n"
                           "    r8: 0x%08x  r9: 0x%08x r10: 0x%08x r11: 0x%08x\n"
                           "   r12: 0x%08x  sp: 0x%08x  lr: 0x%08x  pc: 0x%08x\n"
                           "  cpsr: 0x%08x\n", arm_ctx->r[0], arm_ctx->r[1],
                           arm_ctx->r[2], arm_ctx->r[3], arm_ctx->r[4],
                           arm_ctx->r[5], arm_ctx->r[6], arm_ctx->r[7],
                           arm_ctx->r[8], arm_ctx->r[9], arm_ctx->r[10],
                           arm_ctx->r[11], arm_ctx->r[12], arm_ctx->sp,
                           arm_ctx->lr, arm_ctx->pc, arm_ctx->cpsr);
                    printf("dyld_all_image_info_addr: 0x%08x   dyld_all_image_info_size: 0x%08x\n",
                            thread->task->all_image_info_addr, thread->task->all_image_info_size);
                } else {
                    /*
                     * Retry execution of instruction.
                     */
                    ml_set_interrupts_enabled(TRUE);
                    return;
                }
                break;
            }
        default:
            exception_type = EXC_BREAKPOINT;
            exception_subcode = 0;
            break;
        }
        /*
         * Unknown mode.
         */
    } else {
        panic("sleh_abort: Abort in unknown mode, cpsr: 0x%08x\n", cpsr);
    }

    /*
     * If there was a user exception, handle it.
     */
    if (exception_type) {
        ml_set_interrupts_enabled(TRUE);
#if BOARD_CONFIG_QSD8250_LEO
        if(leo_installd_failed)
            ios7leo_installd_fault(thread,arm_ctx,leo_installd_fault_result);
        if (exception_type == EXC_BAD_ACCESS &&
            reason == SLEH_ABORT_TYPE_PREFETCH_ABORT &&
            (arm_ctx->cpsr & 0x1fU) == 0x10U &&
            ios7lab_exception_code != KERN_SUCCESS &&
            ios7lab_exception_code != KERN_ABORTED) {
            /* EXC_BAD_ACCESS code is the kern_return_t and subcode is the
             * bad address.  Capture remains first-SB-only, but the proven
             * LEO user-prefetch exception contract is deterministic for all
             * failed user prefetches. */
            if ((arm_ctx->cpsr & 0x1fU) == 0x10U &&
                ios7lab_exception_code != KERN_SUCCESS &&
                ios7lab_exception_code != KERN_ABORTED &&
                ios7lab_fault_witness_is_springboard(thread, arm_ctx))
                (void)ios7lab_fault_witness_capture(thread, arm_ctx, ifsr, ifar,
                                                     ios7lab_exception_code,
                                                     0U, 0U);
            doexception(exception_type, ios7lab_exception_code, arm_ctx->pc);
        } else
#endif
        doexception(exception_type, exception_subcode, 0);
    }

    /*
     * Done.
     */
    return;
}

/* Keep the 031 witness in the trap translation unit so its fixed BSS/ring ABI
 * is independently reviewed with trap.o and cannot drift as a second TU. */
#if defined(BOARD_CONFIG_QSD8250_LEO)
#include "ios7lab_fault_witness.c"
#endif

/**
 * irq_handler
 *
 * Handle irqs and pass them over to the platform expert.
 */
boolean_t irq_handler(void *context)
{
    /*
     * Increase/decrease CPU interrupt level.
     */
    cpu_data_t *datap = current_cpu_datap();
    assert(datap);
#if defined(BOARD_CONFIG_QSD8250_LEO)
    ios7lab_irq_observe(context);
#endif
    datap->cpu_interrupt_level++;

    /*
     * Disable system preemption, dispatch the interrupt and go.
     */
    __disable_preemption();

    /*
     * Dispatch the interrupt.
     */
    boolean_t ret = pe_arm_dispatch_interrupt(context);

    /*
     * Go.
     */
    datap->cpu_interrupt_level--;

    __enable_preemption();

    return ret;
}

void irq_iokit_dispatch(uint32_t irq)
{
    cpu_data_t *datap = current_cpu_datap();
    if(datap->handler) {
        datap->handler(datap->target, NULL, datap->nub, irq);
    }
}

/**
 * sleh_undef
 *
 * Handle undefined instructions and VFP usage.
 */
void sleh_undef(arm_saved_state_t * state)
{
/* IOS7LEO_DYLD_BEGIN */
#if defined(BOARD_CONFIG_QSD8250_LEO)
    boolean_t leo_dyld_match=FALSE,leo_dyld_skip_dump=FALSE;
    int leo_instruction_error=-1;
#endif
/* IOS7LEO_DYLD_END */

    uint32_t cpsr, exception_type = 0, exception_subcode = 0;
    arm_saved_state_t *arm_ctx = (arm_saved_state_t *) state;
    thread_t thread = current_thread();

    if (!thread) {
        panic("sleh_undef: current thread is NULL\n");
    }

    /*
     * See if the abort was in Kernel or User mode. 
     */
    cpsr = arm_ctx->cpsr & 0x1F;

    /*
     * Kernel mode. (ARM Supervisor) 
     */
    if (cpsr == 0x13) {
        /*
         * Fall through to bad kernel handler. 
         */
        panic_context(0, (void *) arm_ctx,
                      "Kernel undefined instruction. (saved state 0x%08x)\n"
                      "  r0: 0x%08x  r1: 0x%08x  r2: 0x%08x  r3: 0x%08x\n"
                      "  r4: 0x%08x  r5: 0x%08x  r6: 0x%08x  r7: 0x%08x\n"
                      "  r8: 0x%08x  r9: 0x%08x r10: 0x%08x r11: 0x%08x\n"
                      " r12: 0x%08x  sp: 0x%08x  lr: 0x%08x  pc: 0x%08x\n"
                      "cpsr: 0x%08x\n", arm_ctx, arm_ctx->r[0], arm_ctx->r[1],
                      arm_ctx->r[2], arm_ctx->r[3], arm_ctx->r[4],
                      arm_ctx->r[5], arm_ctx->r[6], arm_ctx->r[7],
                      arm_ctx->r[8], arm_ctx->r[9], arm_ctx->r[10],
                      arm_ctx->r[11], arm_ctx->r[12], arm_ctx->sp, arm_ctx->lr,
                      arm_ctx->pc, arm_ctx->cpsr);
    } else if (cpsr == 0x10) {
        vm_map_t map;
        uint32_t instruction, thumb_offset;
        /*
         * Get the current thread map. 
         */
        map = thread->map;

        /*
         * Get the current instruction. Do not let the pmaps change.
         */
        spl_t spl = splhigh();
        thumb_offset = (arm_ctx->cpsr & (1 << 5)) ? 1 : 0;

/* IOS7LEO_DYLD_BEGIN */
#if defined(BOARD_CONFIG_QSD8250_LEO)
        leo_instruction_error =
#endif
/* IOS7LEO_DYLD_END */
        copyin((uint8_t *) (arm_ctx->pc + thumb_offset), &instruction,
               sizeof(uint32_t));
        splx(spl);

        /* i should really fix this crap properly........ */

        /*
         * Check the instruction encoding to see if it's a coprocessor instruction.
         */
        instruction = OSSwapInt32(instruction);

        /*
         * dyld's faulting one. I really just need to redo all of the VFP detection
         * code, which will happen one day... I just hate myself for this.
         */
        if(instruction != 0xfedeffe7)
        {
            /*
             * NEON instruction.
             */
            thread->machine.vfp_dirty = 0;
            if (!thread->machine.vfp_enable) {
                /*
                 * Enable VFP.
                 */
                vfp_enable_exception(TRUE);
                vfp_context_load(&thread->machine.vfp_regs);
                /*
                 * Continue user execution.
                 */
                thread->machine.vfp_enable = TRUE;
            }
            return;
        }


/* IOS7LEO_DYLD_BEGIN */
#if defined(BOARD_CONFIG_QSD8250_LEO)
        leo_dyld_match=leo_instruction_error==0 && ios7leo_dyld_matches(thread,arm_ctx,instruction);
        leo_dyld_skip_dump=leo_dyld_match && ios7leo_dyld_repeat(thread->task,arm_ctx);
        if(!leo_dyld_skip_dump) {
#endif
/* IOS7LEO_DYLD_END */
        printf
            (ANSI_COLOR_RED "%s[%d]: " ANSI_COLOR_GREEN "usermode undefined instruction, EXC_BAD_INSTRUCTION at 0x%08x in map %p (pmap %p)" ANSI_COLOR_RESET "\n",
             proc_name_address(thread->task->bsd_info),
             proc_pid(thread->task->bsd_info), arm_ctx->pc, map, map->pmap);
        printf("Thread has ARM register state:\n"
               "    r0: 0x%08x  r1: 0x%08x  r2: 0x%08x  r3: 0x%08x\n"
               "    r4: 0x%08x  r5: 0x%08x  r6: 0x%08x  r7: 0x%08x\n"
               "    r8: 0x%08x  r9: 0x%08x r10: 0x%08x r11: 0x%08x\n"
               "   r12: 0x%08x  sp: 0x%08x  lr: 0x%08x  pc: 0x%08x\n"
               "  cpsr: 0x%08x\n", arm_ctx->r[0], arm_ctx->r[1], arm_ctx->r[2],
               arm_ctx->r[3], arm_ctx->r[4], arm_ctx->r[5], arm_ctx->r[6],
               arm_ctx->r[7], arm_ctx->r[8], arm_ctx->r[9], arm_ctx->r[10],
               arm_ctx->r[11], arm_ctx->r[12], arm_ctx->sp, arm_ctx->lr,
               arm_ctx->pc, arm_ctx->cpsr);
        printf("dyld_all_image_info_addr: 0x%08x   dyld_all_image_info_size: 0x%08x\n",
            thread->task->all_image_info_addr, thread->task->all_image_info_size);
/* IOS7LEO_DYLD_BEGIN */
#if defined(BOARD_CONFIG_QSD8250_LEO)
        }
#endif
/* IOS7LEO_DYLD_END */


        /*
         * xxx gate
         */
        exception_type = EXC_BAD_INSTRUCTION;
        exception_subcode = 0;
    } else if (cpsr == 0x17) {
        panic("sleh_undef: undefined instruction in system mode");
    }

    /*
     * If there was a user exception, handle it.
     */
    if (exception_type) {
        ml_set_interrupts_enabled(TRUE);
/* IOS7LEO_DYLD_BEGIN */
#if defined(BOARD_CONFIG_QSD8250_LEO)
        if(leo_dyld_match) ios7leo_dyld_witness(thread->task,arm_ctx);
#endif
/* IOS7LEO_DYLD_END */

        doexception(exception_type, exception_subcode, 0);
    }

    /*
     * Done.
     */
    return;
}
