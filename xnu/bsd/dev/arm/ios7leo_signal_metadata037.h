/* LEO037 private metadata helpers. No public structure or signal ABI change.
 * Caller supplies the existing BSD/Mach constants and fixed-width types.
 * Origin is only for caught SIGBUS/SIGSEGV from EXC_BAD_ACCESS; normal pending
 * bits, signal selection, disposition, and coalescing remain original BSD.
 */
#ifndef IOS7LEO_SIGNAL_METADATA037_H
#define IOS7LEO_SIGNAL_METADATA037_H
struct ios7leo_signal_metadata037 {
    uint64_t address;
    int si_code;
    int synchronous;
};
static inline int
ios7leo_fault_origin037(int exception, int sig)
{
    return exception == EXC_BAD_ACCESS && (sig == SIGBUS || sig == SIGSEGV) ? sig : 0;
}
static inline char
ios7leo_ordinary_origin037(char origin, int sig, uint32_t pending)
{
    /* This must run before the ordinary signal's bit is added. A still-pending
     * synchronous occurrence remains the coalesced occurrence. An old marker
     * whose bit was removed by sigwait/ignore/ptrace is not fresh provenance.
     */
    if (origin == sig && (sig == SIGBUS || sig == SIGSEGV) && !(pending & sigmask(sig)))
        return 0;
    return origin;
}
static inline struct ios7leo_signal_metadata037
ios7leo_take_origin037(char *origin, int sig, int exception,
    uint64_t exception_code, uint64_t exception_address)
{
    struct ios7leo_signal_metadata037 result = {0, 0, 0};
    if (*origin != sig || !(sig == SIGBUS || sig == SIGSEGV))
        return result;
    /* The caller holds the process lock and consumes before releasing it.
     * Keep uu_exception/code/subcode for the original kern_exit diagnostics.
     */
    *origin = 0;
    if (ios7leo_fault_origin037(exception, sig) == 0)
        return result;
    result.synchronous = 1;
    result.address = exception_address;
    if (sig == SIGBUS)
        result.si_code = BUS_ADRERR;
    else if (exception_code == KERN_INVALID_ADDRESS)
        result.si_code = SEGV_MAPERR;
    else if (exception_code == KERN_PROTECTION_FAILURE)
        result.si_code = SEGV_ACCERR;
    /* Unknown real Mach codes remain code0, not a fabricated permission fault. */
    return result;
}
#endif
