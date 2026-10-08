/* iOS7 ARM saved-register wire, not the legacy native C argument structure. */
#ifndef IOS7LAB_VM_RANGE_WIRE_H
#define IOS7LAB_VM_RANGE_WIRE_H
#include <stdint.h>
struct ios7lab_vm_range_wire {
    uint32_t target, address_low, address_high, size_low, size_high;
};
struct ios7lab_vm_protect_wire {
    uint32_t target, address_low, address_high, size_low, size_high;
    uint32_t set_maximum, new_protection;
};
typedef char ios7lab_vm_range_wire_size[sizeof(struct ios7lab_vm_range_wire) == 20 ? 1 : -1];
typedef char ios7lab_vm_protect_wire_size[sizeof(struct ios7lab_vm_protect_wire) == 28 ? 1 : -1];
static int ios7lab_vm_range32(const struct ios7lab_vm_range_wire *wire,
                            uint32_t *address, uint32_t *size)
{
    if (wire->address_high || wire->size_high) return 0;
    *address = wire->address_low;
    *size = wire->size_low;
    return 1;
}
#endif
