#ifndef IOS7LAB_LEO_ENTRY_H
#define IOS7LAB_LEO_ENTRY_H
#include <pexpert/arm/boot.h>
void ios7leo_checkpoint(boot_args *, unsigned int);
void ios7leo_forget_early_maps(void);
void ios7leo_debug_putc(char);
void ios7leo_kernel_continuing(void);
void ios7leo_timer_observe(unsigned int raw_gpt_ticks, unsigned int irq7_count);
int ios7leo_panic_text_begin(unsigned int,unsigned int,unsigned int);
void ios7leo_panic_text_putc(char);
void ios7leo_panic_text_end(void);
int ios7leo_fatal_report(unsigned int category, unsigned int callsite,
                        unsigned int pc, unsigned int lr,
                        unsigned int far, unsigned int fsr,
                        unsigned int valid_mask, const char *message);
void ios7leo_final_boundary(unsigned int) __attribute__((noreturn));
#endif
