/* Private LEO first-failed-launchd data-abort observation, 2026-10-05.
 * Genuine immutable entry/failed-VM state and copyin results; no recovery.
 * Existing logger worker drains complete atomically published RAM records.
 */
#ifndef IOS7LEO_DATA_ABORT037_H
#define IOS7LEO_DATA_ABORT037_H
#include "ios7leo_abort037_shared.h"

typedef char ios7leo037_context_bytes[(sizeof(abort_information_context_t)==76U)?1:-1];
typedef char ios7leo037_context_fsr[(__builtin_offsetof(abort_information_context_t,fsr)==68U)?1:-1];
typedef char ios7leo037_context_far[(__builtin_offsetof(abort_information_context_t,far)==72U)?1:-1];

typedef struct ios7leo037_data_snapshot {
    abort_information_context_t entry;
    abort_information_context_t after_vm;
    uint32_t valid;
    uint32_t context_address;
    uint32_t c_thread_entry;
    uint32_t cp15_thread_entry;
    uint32_t owner_uss_entry;
    uint32_t c_thread_after;
    uint32_t cp15_thread_after;
    uint32_t owner_uss_after;
} ios7leo037_data_snapshot_t;

typedef struct ios7leo037_record_queue {
    volatile UInt32 claimed;
    volatile UInt32 ready;
    volatile UInt32 next;
    uint32_t bytes[IOS7LEO_ABORT037_RECORDS];
    char record[IOS7LEO_ABORT037_RECORDS][IOS7LEO_ABORT037_PAYLOAD_MAX];
} ios7leo037_record_queue_t;
static ios7leo037_record_queue_t ios7leo037_records;
typedef char ios7leo037_bss_budget[(sizeof(ios7leo037_record_queue_t)<=4096U)?1:-1];

static void
ios7leo037_record_barrier(void)
{
    __asm__ __volatile__("dmb sy":::"memory");
}

static uint32_t
ios7leo037_cp15_thread(void)
{
    uint32_t value;
    __asm__ __volatile__("mrc p15, 0, %0, c13, c0, 4":"=r"(value)::"memory");
    return value;
}

static void
ios7leo037_data_entry(ios7leo037_data_snapshot_t *snapshot,
                     const abort_information_context_t *context, thread_t owner)
{
    snapshot->entry=*context;
    snapshot->context_address=(uint32_t)(uintptr_t)context;
    snapshot->c_thread_entry=(uint32_t)(uintptr_t)owner;
    snapshot->cp15_thread_entry=ios7leo037_cp15_thread();
    snapshot->owner_uss_entry=(uint32_t)(uintptr_t)(owner?owner->machine.uss:NULL);
    snapshot->valid=1U;
}

static void
ios7leo037_data_after_vm(ios7leo037_data_snapshot_t *snapshot,
                        const abort_information_context_t *context, thread_t owner)
{
    snapshot->after_vm=*context;
    snapshot->c_thread_after=(uint32_t)(uintptr_t)current_thread();
    snapshot->cp15_thread_after=ios7leo037_cp15_thread();
    /* Same cached owner's USS; a different global thread is only recorded. */
    snapshot->owner_uss_after=(uint32_t)(uintptr_t)(owner?owner->machine.uss:NULL);
}

static uint32_t
ios7leo037_context_diff(const abort_information_context_t *a,
                       const abort_information_context_t *b)
{
    uint32_t mask=0U;
    unsigned i;
    for(i=0U;i<13U;++i)if(a->r[i]!=b->r[i])mask|=1U<<i;
    if(a->sp!=b->sp)mask|=1U<<13;
    if(a->lr!=b->lr)mask|=1U<<14;
    if(a->pc!=b->pc)mask|=1U<<15;
    if(a->cpsr!=b->cpsr)mask|=1U<<16;
    if(a->fsr!=b->fsr)mask|=1U<<17;
    if(a->far!=b->far)mask|=1U<<18;
    return mask;
}

static void
ios7leo037_publish_exception_state(thread_t owner,
                                  const abort_information_context_t *context,
                                  uint32_t reason,uint32_t raw_fsr,uint32_t raw_far)
{
    boolean_t previous;
    if(!owner || context!=(const abort_information_context_t *)owner->machine.uss)return;
    /* Raw+44/+48 alias es.exception/es.fsr. Call only after every raw-frame
     * consumer and witness; doexception receives separate frozen locals. */
    previous=ml_set_interrupts_enabled(FALSE);
    owner->machine.es.exception=reason;
    owner->machine.es.fsr=raw_fsr;
    owner->machine.es.far=raw_far;
    ml_set_interrupts_enabled(previous);
}

static void
ios7leo037_record_length(unsigned stage,int n)
{
    static const char failed[]="ABORT037 FORMAT_ERROR\n";
    if(stage>=IOS7LEO_ABORT037_RECORDS)return;
    if(n<=0 || (uint32_t)n>=IOS7LEO_ABORT037_PAYLOAD_MAX) {
        /* Fixed complete failure record, not truncated or invented fields. */
        memcpy(ios7leo037_records.record[stage],failed,sizeof(failed)-1U);
        ios7leo037_records.bytes[stage]=(uint32_t)sizeof(failed)-1U;
    } else ios7leo037_records.bytes[stage]=(uint32_t)n;
}

static int
ios7leo037_format_context(char *payload,const char *phase,
                          const abort_information_context_t *state)
{
    return snprintf(payload,IOS7LEO_ABORT037_PAYLOAD_MAX,
        "ABORT037 %s HEX R=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x SP=%08x LR=%08x PC=%08x PSR=%08x FSR=%08x FAR=%08x\n",
        phase,(unsigned)state->r[0],(unsigned)state->r[1],
        (unsigned)state->r[2],(unsigned)state->r[3],(unsigned)state->r[4],
        (unsigned)state->r[5],(unsigned)state->r[6],(unsigned)state->r[7],
        (unsigned)state->r[8],(unsigned)state->r[9],(unsigned)state->r[10],
        (unsigned)state->r[11],(unsigned)state->r[12],(unsigned)state->sp,
        (unsigned)state->lr,(unsigned)state->pc,(unsigned)state->cpsr,
        (unsigned)state->fsr,(unsigned)state->far);
}

__attribute__((noinline)) static void
ios7leo037_failed_data_witness(thread_t owner,
                              const ios7leo037_data_snapshot_t *snapshot,
                              kern_return_t fault_result,uint32_t early_bad_address)
{
    uint32_t instructions[4]={0U,0U,0U,0U};
    uint32_t stack[32]={0U};
    uint32_t pc,sp,equalities=0U;
    unsigned index;
    int error,pid,n;
    const char *name;
    if(!snapshot->valid || !owner || !owner->task || !owner->task->bsd_info ||
       fault_result==KERN_SUCCESS || fault_result==KERN_ABORTED ||
       (snapshot->entry.cpsr&0x1fU)!=0x10U) return;
    name=proc_name_address(owner->task->bsd_info);
    if(!name || strcmp(name,"launchd")!=0)return;
    pid=proc_pid(owner->task->bsd_info);
    /* PID1 remains eligible: never exclude an actual root launchd failure. */
    if(pid<=0 || !OSCompareAndSwap(0U,1U,&ios7leo037_records.claimed))return;

    if(snapshot->context_address==snapshot->owner_uss_entry)equalities|=1U;
    if(snapshot->cp15_thread_entry==snapshot->c_thread_entry)equalities|=2U;
    if(snapshot->c_thread_after==snapshot->c_thread_entry)equalities|=4U;
    if(snapshot->owner_uss_after==snapshot->owner_uss_entry)equalities|=8U;
    if(snapshot->cp15_thread_after==snapshot->c_thread_after)equalities|=16U;
    n=snprintf(ios7leo037_records.record[0],IOS7LEO_ABORT037_PAYLOAD_MAX,
        "ABORT037 META HEX PID=%x RESULT=%x BAD=%08x CTX=%08x CTH=%08x/%08x CPTH=%08x/%08x OUSS=%08x/%08x EQ=%x DIFF=%x\n",
        (unsigned)pid,(unsigned)fault_result,(unsigned)early_bad_address,
        (unsigned)snapshot->context_address,(unsigned)snapshot->c_thread_entry,
        (unsigned)snapshot->c_thread_after,(unsigned)snapshot->cp15_thread_entry,
        (unsigned)snapshot->cp15_thread_after,(unsigned)snapshot->owner_uss_entry,
        (unsigned)snapshot->owner_uss_after,(unsigned)equalities,
        (unsigned)ios7leo037_context_diff(&snapshot->entry,&snapshot->after_vm));
    ios7leo037_record_length(0U,n);
    ios7leo037_record_length(1U,ios7leo037_format_context(
        ios7leo037_records.record[1],"ENTRY",&snapshot->entry));
    ios7leo037_record_length(2U,ios7leo037_format_context(
        ios7leo037_records.record[2],"AFTER_VM",&snapshot->after_vm));

    /* Copies use the native current task's copyin, after the existing IRQ-enable
     * point. Failed/rejected reads never emit zero-filled pretend memory words. */
    pc=snapshot->entry.pc;
    if(pc<0x1004U || pc>0x40000000U-12U) {
        n=snprintf(ios7leo037_records.record[3],IOS7LEO_ABORT037_PAYLOAD_MAX,
            "ABORT037 INSN ATTEMPTED=0 REJECT=range PC=%08x\n",(unsigned)pc);
    } else {
        error=copyin((user_addr_t)(pc-4U),instructions,sizeof(instructions));
        if(error)n=snprintf(ios7leo037_records.record[3],IOS7LEO_ABORT037_PAYLOAD_MAX,
            "ABORT037 INSN ATTEMPTED=1 VALID=0 ERR=%d START=%08x BYTES=16\n",error,(unsigned)(pc-4U));
        else n=snprintf(ios7leo037_records.record[3],IOS7LEO_ABORT037_PAYLOAD_MAX,
            "ABORT037 INSN ATTEMPTED=1 VALID=1 ERR=0 START=%08x BYTES=16 LE32=%08x,%08x,%08x,%08x\n",
            (unsigned)(pc-4U),(unsigned)instructions[0],(unsigned)instructions[1],
            (unsigned)instructions[2],(unsigned)instructions[3]);
    }
    ios7leo037_record_length(3U,n);
    sp=snapshot->entry.sp;
    /* Validate all384bytes before any stack copy. Each subsequent chunk is an
     * independent128B observation, not an atomic snapshot of user memory. */
    for(index=0U;index<3U;++index) {
        uint32_t chunk_offset=index*128U;
        if((sp&3U) || sp<0x1000U || sp>0x40000000U-384U) {
            n=snprintf(ios7leo037_records.record[4U+index],IOS7LEO_ABORT037_PAYLOAD_MAX,
                "ABORT037 STACK=%u OFFSET=%08x ATTEMPTED=0 VALID=0 REJECT=range SP=%08x BYTES=128\n",
                index,(unsigned)chunk_offset,(unsigned)sp);
        } else {
            uint32_t start=sp+chunk_offset;
            bzero(stack,sizeof(stack));
            error=copyin((user_addr_t)start,stack,sizeof(stack));
            if(error)n=snprintf(ios7leo037_records.record[4U+index],IOS7LEO_ABORT037_PAYLOAD_MAX,
                "ABORT037 STACK=%u OFFSET=%08x ATTEMPTED=1 VALID=0 ERR=%d START=%08x BYTES=128 COPIED=unknown\n",
                index,(unsigned)chunk_offset,error,(unsigned)start);
            else n=snprintf(ios7leo037_records.record[4U+index],IOS7LEO_ABORT037_PAYLOAD_MAX,
                "ABORT037 STACK=%u OFFSET=%08x ATTEMPTED=1 VALID=1 ERR=0 START=%08x BYTES=128 COPIED=128 LE32=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                index,(unsigned)chunk_offset,(unsigned)start,
                (unsigned)stack[0],(unsigned)stack[1],(unsigned)stack[2],(unsigned)stack[3],
                (unsigned)stack[4],(unsigned)stack[5],(unsigned)stack[6],(unsigned)stack[7],
                (unsigned)stack[8],(unsigned)stack[9],(unsigned)stack[10],(unsigned)stack[11],
                (unsigned)stack[12],(unsigned)stack[13],(unsigned)stack[14],(unsigned)stack[15],
                (unsigned)stack[16],(unsigned)stack[17],(unsigned)stack[18],(unsigned)stack[19],
                (unsigned)stack[20],(unsigned)stack[21],(unsigned)stack[22],(unsigned)stack[23],
                (unsigned)stack[24],(unsigned)stack[25],(unsigned)stack[26],(unsigned)stack[27],
                (unsigned)stack[28],(unsigned)stack[29],(unsigned)stack[30],(unsigned)stack[31]);
        }
        ios7leo037_record_length(4U+index,n);
    }
    /* All seven records and lengths are immutable before the only publication. */
    ios7leo037_record_barrier();
    ios7leo037_records.ready=1U;
}

uint32_t
ios7leo_abort037_take(uint8_t *payload,uint32_t capacity)
{
    UInt32 next;
    uint32_t bytes;
    if(!payload || !capacity || ios7leo037_records.ready!=1U)return 0U;
    ios7leo037_record_barrier();
    do {
        next=ios7leo037_records.next;
        if(next>=IOS7LEO_ABORT037_RECORDS)return 0U;
        bytes=ios7leo037_records.bytes[next];
        if(!bytes || bytes>=IOS7LEO_ABORT037_PAYLOAD_MAX || capacity<bytes)return 0U;
    } while(!OSCompareAndSwap(next,next+1U,&ios7leo037_records.next));
    memcpy(payload,ios7leo037_records.record[next],bytes);
    return bytes;
}
#endif
