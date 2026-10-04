/* HTC HD2 integration/publication changes: Garysss123, 2026-10-04. Original license notices are preserved. */
/*
 * Copyright (c) 2011 Apple Inc. All rights reserved.
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

#include <mach/mach_types.h>
#include <mach/mach_traps.h>
#include <mach/mach_vm_server.h>
#include <mach/mach_port_server.h>
#include <mach/vm_map.h>
#include <kern/task.h>
#include <kern/ipc_tt.h>
#include <vm/vm_protos.h>

#include <arm/ios7_compat.h>
#if IOS7_COMPAT_PLATFORM
#include <pexpert/pexpert.h>
#include <mach/vm_statistics.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_object.h>

/* Original iOS 7 ARMv7 trap -10 passes five 32-bit register words:
 * target, user pointer, size low, size high, flags. This legacy kernel's
 * mach_vm_size_t is only 32 bits (explicit iOS 4.3 compatibility), so its
 * native structure would otherwise interpret size-high as flags.
 * Keep the control ABI unless the PC experiment explicitly opts in. */
struct ios7lab_vm_allocate_wire {
    uint32_t target, addr, size_low, size_high, flags;
};
typedef char ios7lab_vm_allocate_wire_size[
    sizeof(struct ios7lab_vm_allocate_wire) == 20 ? 1 : -1];

/* Private UI-only lab: implement real receive/context/queue/send operations.
 * Guard enforcement and temp-owner/importance/de-nap hints are deliberately
 * bypassed and logged. Do not claim these modern isolation/QoS features. */
int ios7lab_mach_port_construct_trap(void *saved_registers)
{
    uint32_t *regs = (uint32_t *)saved_registers;
    uint32_t options[6] = {0};
    /* Runtime ABI selector is shared. */
    int rv = MACH_SEND_INVALID_DEST;
    task_t task = TASK_NULL;
    mach_port_name_t name = MACH_PORT_NULL;
    ipc_port_t send_port = IP_NULL;
    static unsigned int logged = 0;
    if (!ios7_compat_vmabi_enabled())
        return rv;
    task = port_name_to_task(regs[0]);
    if (task != current_task())
        goto done;
    if (copyin((user_addr_t)regs[1], options, sizeof(options))) {
        rv = MACH_SEND_INVALID_DATA;
        goto done;
    }
    if ((options[0] & ~0x7FU) || regs[3] || options[2] || options[3] || options[4] || options[5]) {
        rv = KERN_INVALID_ARGUMENT;
        goto done;
    }
    rv = mach_port_allocate(task->itk_space, MACH_PORT_RIGHT_RECEIVE, &name);
    if (rv != KERN_SUCCESS)
        goto done;
    rv = mach_port_set_context(task->itk_space, name, regs[2]);
    if (rv != KERN_SUCCESS)
        goto cleanup;
    if (options[0] & 0x2U) {
        mach_port_limits_t limits;
        limits.mpl_qlimit = options[1];
        rv = mach_port_set_attributes(task->itk_space, name, MACH_PORT_LIMITS_INFO,
                                      (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT);
        if (rv != KERN_SUCCESS)
            goto cleanup;
    }
    if (options[0] & 0x10U) {
        rv = ipc_object_copyin(task->itk_space, name, MACH_MSG_TYPE_MAKE_SEND, (ipc_object_t *)&send_port);
        if (rv != KERN_SUCCESS)
            goto cleanup;
        rv = mach_port_insert_right(task->itk_space, name, send_port, MACH_MSG_TYPE_PORT_SEND);
        if (rv != KERN_SUCCESS) {
            ipc_port_release_send(send_port);
            goto cleanup;
        }
    }
    rv = copyout(&name, (user_addr_t)regs[4], sizeof(name));
    if (rv == KERN_SUCCESS)
        goto done;
cleanup:
    (void)mach_port_destroy(task->itk_space, name);
done:
    if (logged++ < 12)
        printf("IOS7LAB port_construct flags=%x qlimit=%u context=%08x:%08x result=%d name=%x bypass_hints=%x\n",
               options[0], options[1], regs[3], regs[2], rv, name, options[0] & 0x6DU);
    if (task)
        task_deallocate(task);
    return rv;
}

/* Complete the already-selected guard-bypass mode for checked-in receive
 * rights. Validate ownership and manipulate the real context under port lock.
 * This intentionally does not implement guarded-right isolation/strictness. */
static int ios7lab_mach_port_context_guard(void *saved_registers, int unguard)
{
    uint32_t *regs = (uint32_t *)saved_registers;
    /* Runtime ABI selector is shared. */
    int rv = MACH_SEND_INVALID_DEST;
    task_t task = TASK_NULL;
    ipc_port_t port = IP_NULL;
    static unsigned int logged = 0;
    if (!ios7_compat_vmabi_enabled())
        return rv;
    task = port_name_to_task(regs[0]);
    if (task != current_task())
        goto done;
    if (regs[3] || (!unguard && regs[4] > 1)) {
        rv = KERN_INVALID_ARGUMENT;
        goto done;
    }
    rv = ipc_port_translate_receive(task->itk_space, regs[1], &port);
    if (rv != KERN_SUCCESS)
        goto done;
    if (unguard && port->ip_context != (mach_vm_address_t)regs[2]) {
        rv = KERN_INVALID_ARGUMENT;
    } else {
        port->ip_context = unguard ? 0 : (mach_vm_address_t)regs[2];
    }
    ip_unlock(port);
done:
    if (logged++ < 24)
        printf("IOS7LAB port_%sguard name=%x context=%08x:%08x strict=%u result=%d guard_enforcement=disabled\n",
               unguard ? "un" : "", regs[1], regs[3], regs[2], unguard ? 0 : regs[4], rv);
    if (task)
        task_deallocate(task);
    return rv;
}

int ios7lab_mach_port_guard_trap(void *saved_registers)
{
    return ios7lab_mach_port_context_guard(saved_registers, 0);
}

int ios7lab_mach_port_unguard_trap(void *saved_registers)
{
    return ios7lab_mach_port_context_guard(saved_registers, 1);
}

int ios7lab_mach_port_destruct_trap(void *saved_registers)
{
    uint32_t *regs = (uint32_t *)saved_registers;
    /* Runtime ABI selector is shared. */
    int rv = MACH_SEND_INVALID_DEST;
    int32_t delta = (int32_t)regs[2];
    task_t task = TASK_NULL;
    static unsigned int logged = 0;
    if (!ios7_compat_vmabi_enabled())
        return rv;
    task = port_name_to_task(regs[0]);
    if (task != current_task())
        goto done;
    if (delta > 0 || delta < -65535) {
        rv = KERN_INVALID_VALUE;
        goto done;
    }
    if (delta) {
        rv = mach_port_mod_refs(task->itk_space, regs[1], MACH_PORT_RIGHT_SEND, delta);
        if (rv != KERN_SUCCESS)
            goto done;
    }
    rv = mach_port_mod_refs(task->itk_space, regs[1], MACH_PORT_RIGHT_RECEIVE, -1);
done:
    if (logged++ < 12)
        printf("IOS7LAB port_destruct name=%x delta=%d result=%d guard_enforcement=disabled\n",
               regs[1], delta, rv);
    if (task)
        task_deallocate(task);
    return rv;
}

/* ARMv7's original trap stub places the eighth argument in r8, preserving
 * the r7 frame pointer. Match the saved register frame, not a packed C ABI.
 * Native mapping semantics follow Apple's common mach_vm_map fast trap. */
int ios7lab_mach_vm_map_trap(void *saved_registers)
{
    uint32_t *regs = (uint32_t *)saved_registers;
    /* Runtime ABI selector is shared. */
    int rv = MACH_SEND_INVALID_DEST;
    uint64_t address64 = 0;
    mach_vm_offset_t address32 = 0;
    task_t task = TASK_NULL;
    static unsigned int logged = 0;
    if (!ios7_compat_vmabi_enabled())
        return rv;
    task = port_name_to_task(regs[0]);
    if (task != current_task())
        goto done;
    if (copyin((user_addr_t)regs[1], (char *)&address64, sizeof(address64)))
        goto done;
    if (regs[3] || regs[5] || (address64 >> 32)) {
        rv = KERN_INVALID_ARGUMENT;
        goto done;
    }
    address32 = (mach_vm_offset_t)address64;
    rv = mach_vm_map(task->map, &address32, regs[2], regs[4], regs[6],
                     IPC_PORT_NULL, 0, FALSE, regs[8], VM_PROT_ALL,
                     VM_INHERIT_DEFAULT);
    if (rv == KERN_SUCCESS) {
        address64 = (uint64_t)address32;
        rv = copyout(&address64, (user_addr_t)regs[1], sizeof(address64));
    }
done:
    if (logged++ < 12)
        printf("IOS7LAB vm_map64 size=%08x:%08x mask=%08x:%08x flags=%x prot=%x result=%d address=%08x\n",
               regs[3], regs[2], regs[5], regs[4], regs[6], regs[8], rv, address32);
    if (task)
        task_deallocate(task);
    return rv;
}

static int
ios7lab_vm_allocate(struct ios7lab_vm_allocate_wire *wire)
{
    uint64_t address64 = 0;
    mach_vm_offset_t address32 = 0;
    task_t task = port_name_to_task(wire->target);
    int rv = MACH_SEND_INVALID_DEST;
    static unsigned int logged = 0;

    if (task != current_task())
        goto done;
    if (copyin((user_addr_t)wire->addr, (char *)&address64, sizeof(address64)))
        goto done;
    if (wire->size_high ||
        (!(wire->flags & VM_FLAGS_ANYWHERE) && (address64 >> 32))) {
        rv = KERN_INVALID_ARGUMENT;
        goto done;
    }
    address32 = (wire->flags & VM_FLAGS_ANYWHERE) ? 0 : (mach_vm_offset_t)address64;
    rv = mach_vm_allocate(task->map, &address32, wire->size_low, wire->flags);
    if (rv == KERN_SUCCESS) {
        address64 = (uint64_t)address32;
        rv = copyout(&address64, (user_addr_t)wire->addr, sizeof(address64));
    }
done:
    if (logged++ < 8)
        printf("IOS7LAB vm_allocate64 size=%08x:%08x flags=%x result=%d address=%08x\n",
               wire->size_high, wire->size_low, wire->flags, rv, address32);
    if (task)
        task_deallocate(task);
    return rv;
}
#endif


/* IOS7LEO_VM_BEGIN */
#if BOARD_CONFIG_QSD8250_LEO
#include <mach/vm_statistics.h>
/* Original iOS7 ARM32 trap-10 register wire and 64-bit in/out address.
 * Only the existing real allocation adapter is selected on Leo; no IPC bypasses. */
struct ios7leo_vm_allocate_wire {
    uint32_t target, addr, size_low, size_high, flags;
};
typedef char ios7leo_vm_allocate_wire_size[
    sizeof(struct ios7leo_vm_allocate_wire) == 20 ? 1 : -1];

static int
ios7leo_vm_allocate(struct ios7leo_vm_allocate_wire *wire)
{
    uint64_t address64 = 0;
    mach_vm_offset_t address32 = 0;
    task_t task = port_name_to_task(wire->target);
    int rv = MACH_SEND_INVALID_DEST;
    static unsigned int logged = 0;

    if (task != current_task())
        goto done;
    if (copyin((user_addr_t)wire->addr, (char *)&address64, sizeof(address64)))
        goto done;
    if (wire->size_high ||
        (!(wire->flags & VM_FLAGS_ANYWHERE) && (address64 >> 32))) {
        rv = KERN_INVALID_ARGUMENT;
        goto done;
    }
    address32 = (wire->flags & VM_FLAGS_ANYWHERE) ? 0 : (mach_vm_offset_t)address64;
    rv = mach_vm_allocate(task->map, &address32, wire->size_low, wire->flags);
    if (rv == KERN_SUCCESS) {
        address64 = (uint64_t)address32;
        rv = copyout(&address64, (user_addr_t)wire->addr, sizeof(address64));
    }
done:
    if (logged++ < 8)
        printf("Leo vm_allocate64 size=%08x:%08x flags=%x result=%d address=%08x\n",
               wire->size_high, wire->size_low, wire->flags, rv, address32);
    if (task)
        task_deallocate(task);
    return rv;
}
#endif
/* IOS7LEO_VM_END */
int
_kernelrpc_mach_vm_allocate_trap(struct _kernelrpc_mach_vm_allocate_trap_args *args)
{
/* IOS7LEO_VM_BEGIN */
#if BOARD_CONFIG_QSD8250_LEO
    return ios7leo_vm_allocate((struct ios7leo_vm_allocate_wire *)args);
#endif
/* IOS7LEO_VM_END */

#if BOARD_CONFIG_ARMPBA8
    int lab_abi = 0;
    if (PE_parse_boot_argn("ios7lab_vmabi", &lab_abi, sizeof(lab_abi)) && lab_abi == 1)
        return ios7lab_vm_allocate((struct ios7lab_vm_allocate_wire *)args);
#endif
	mach_vm_offset_t addr;
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	if (copyin(args->addr, (char *)&addr, sizeof (addr)))
		goto done;

	rv = mach_vm_allocate(task->map, &addr, args->size, args->flags);
	if (rv == KERN_SUCCESS)
		rv = copyout(&addr, args->addr, sizeof (addr));
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}

int
_kernelrpc_mach_vm_deallocate_trap(struct _kernelrpc_mach_vm_deallocate_args *args)
{
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_vm_deallocate(task->map, args->address, args->size);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}

int
_kernelrpc_mach_vm_protect_trap(struct _kernelrpc_mach_vm_protect_args *args)
{
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_vm_protect(task->map, args->address, args->size,
	    args->set_maximum, args->new_protection);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}

int
_kernelrpc_mach_port_allocate_trap(struct _kernelrpc_mach_port_allocate_args *args)
{
	task_t task = port_name_to_task(args->target);
	mach_port_name_t name;
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_port_allocate(task->itk_space, args->right, &name);
	if (rv == KERN_SUCCESS)
		rv = copyout(&name, args->name, sizeof (name));

	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}

int
_kernelrpc_mach_port_destroy_trap(struct _kernelrpc_mach_port_destroy_args *args)
{
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_port_destroy(task->itk_space, args->name);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}

int
_kernelrpc_mach_port_deallocate_trap(struct _kernelrpc_mach_port_deallocate_args *args)
{
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_port_deallocate(task->itk_space, args->name);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}

int
_kernelrpc_mach_port_mod_refs_trap(struct _kernelrpc_mach_port_mod_refs_args *args)
{
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_port_mod_refs(task->itk_space, args->name, args->right, args->delta);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}


int
_kernelrpc_mach_port_move_member_trap(struct _kernelrpc_mach_port_move_member_args *args)
{
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_port_move_member(task->itk_space, args->member, args->after);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}

int
_kernelrpc_mach_port_insert_right_trap(struct _kernelrpc_mach_port_insert_right_args *args)
{
	task_t task = port_name_to_task(args->target);
	ipc_port_t port;
	mach_msg_type_name_t disp;
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = ipc_object_copyin(task->itk_space, args->poly, args->polyPoly,
	    (ipc_object_t *)&port);
	if (rv != KERN_SUCCESS)
		goto done;
	disp = ipc_object_copyin_type(args->polyPoly);

	rv = mach_port_insert_right(task->itk_space, args->name, port, disp);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}

int
_kernelrpc_mach_port_insert_member_trap(struct _kernelrpc_mach_port_insert_member_args *args)
{
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_port_insert_member(task->itk_space, args->name, args->pset);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}


int
_kernelrpc_mach_port_extract_member_trap(struct _kernelrpc_mach_port_extract_member_args *args)
{
	task_t task = port_name_to_task(args->target);
	int rv = MACH_SEND_INVALID_DEST;

	if (task != current_task())
		goto done;

	rv = mach_port_extract_member(task->itk_space, args->name, args->pset);
	
done:
	if (task)
		task_deallocate(task);
	return (rv);
}
