#pragma once

#ifndef ROS_INC_ARCH_TRAPFRAME_H
#error "Do not include include ros/arch/trapframe64.h directly"
#endif

struct hw_trapframe {
	uint64_t tf_gsbase;
	uint64_t tf_fsbase;
	uint64_t tf_rax;
	uint64_t tf_rbx;
	uint64_t tf_rcx;
	uint64_t tf_rdx;
	uint64_t tf_rbp;
	uint64_t tf_rsi;
	uint64_t tf_rdi;
	uint64_t tf_r8;
	uint64_t tf_r9;
	uint64_t tf_r10;
	uint64_t tf_r11;
	uint64_t tf_r12;
	uint64_t tf_r13;
	uint64_t tf_r14;
	uint64_t tf_r15;
	uint32_t tf_trapno;
	uint32_t tf_padding5;		/* used in trap reflection */
	/* below here defined by x86 hardware (error code optional) */
	uint32_t tf_err;
	uint32_t tf_padding4;		/* used in trap reflection */
	uint64_t tf_rip;
	uint16_t tf_cs;
	uint16_t tf_padding3;		/* used in trap reflection */
	uint32_t tf_padding2;
	uint64_t tf_rflags;
	/* unlike 32 bit, SS:RSP is always pushed, even when not changing rings */
	uint64_t tf_rsp;
	uint16_t tf_ss;
	uint16_t tf_padding1;
	uint32_t tf_padding0;		/* used for partial contexts */
};

struct sw_trapframe {
	uint64_t tf_gsbase;
	uint64_t tf_fsbase;
	uint64_t tf_rbx;
	uint64_t tf_rbp;
	uint64_t tf_r12;
	uint64_t tf_r13;
	uint64_t tf_r14;
	uint64_t tf_r15;
	uint64_t tf_rip;
	uint64_t tf_rsp;
	uint32_t tf_mxcsr;
	uint16_t tf_fpucw;
	uint16_t tf_padding0;		/* used for partial contexts */
};

/* The context is both what we want to run and its current state.  For VMs, that
 * includes status bits from the VMCS for reflected vmexits/hypercalls.  This is
 * not particularly different than how hardware contexts contain info on
 * reflected traps.
 *
 * The VM context also consists of a mountain of state in the VMCS, referenced
 * only in here by guest pcoreid.  Those bits are set once by Akaros to sensible
 * defaults and then are changed during execution of the VM.  The parts of that
 * state that are exposed to the user-VMM are the contents of the trapframe. */

#define VMCTX_FL_PARTIAL		(1 << 0)
#define VMCTX_FL_HAS_FAULT		(1 << 1)

struct vm_trapframe {
	/* Actual processor state */
	uint64_t tf_rax;
	uint64_t tf_rbx;
	uint64_t tf_rcx;
	uint64_t tf_rdx;
	uint64_t tf_rbp;
	uint64_t tf_rsi;
	uint64_t tf_rdi;
	uint64_t tf_r8;
	uint64_t tf_r9;
	uint64_t tf_r10;
	uint64_t tf_r11;
	uint64_t tf_r12;
	uint64_t tf_r13;
	uint64_t tf_r14;
	uint64_t tf_r15;
	uint64_t tf_rip;
	uint64_t tf_rflags;
	uint64_t tf_rsp;
	/* Admin bits */
	uint32_t tf_guest_pcoreid;
	uint32_t tf_flags;
	uint64_t tf_cr3;
	uint32_t tf_trap_inject;
	uint64_t tf_exit_reason;
	uint64_t tf_exit_qual;
	uint32_t tf_intrinfo1;
	uint32_t tf_intrinfo2;
	uint64_t tf_guest_va;
	uint64_t tf_guest_pa;
};
