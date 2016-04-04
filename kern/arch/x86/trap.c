#include <arch/mmu.h>
#include <arch/x86.h>
#include <arch/arch.h>
#include <arch/console.h>
#include <arch/apic.h>
#include <arch/perfmon.h>
#include <ros/common.h>
#include <smp.h>
#include <assert.h>
#include <pmap.h>
#include <trap.h>
#include <monitor.h>
#include <process.h>
#include <mm.h>
#include <stdio.h>
#include <slab.h>
#include <syscall.h>
#include <kdebug.h>
#include <kmalloc.h>
#include <ex_table.h>
#include <arch/mptables.h>
#include <ros/procinfo.h>

taskstate_t ts;

/* Interrupt descriptor table.  64 bit needs 16 byte alignment (i think). */
gatedesc_t __attribute__((aligned (16))) idt[256] = { { 0 } };
pseudodesc_t idt_pd;

/* interrupt handler table, each element is a linked list of handlers for a
 * given IRQ.  Modification requires holding the lock (TODO: RCU) */
struct irq_handler *irq_handlers[NUM_IRQS];
spinlock_t irq_handler_wlock = SPINLOCK_INITIALIZER_IRQSAVE;

static bool try_handle_exception_fixup(struct hw_trapframe *hw_tf)
{
	if (in_kernel(hw_tf)) {
		uintptr_t fixup_ip = get_fixup_ip(hw_tf->tf_rip);

		if (fixup_ip != 0) {
			hw_tf->tf_rip = fixup_ip;
			return true;
		}
	}

	return false;
}

const char *x86_trapname(int trapno)
{
	static const char *const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < sizeof(excnames)/sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	return "(unknown trap)";
}

/* Set stacktop for the current core to be the stack the kernel will start on
 * when trapping/interrupting from userspace.  Don't use this til after
 * smp_percpu_init().  We can probably get the TSS by reading the task register
 * and then the GDT.  Still, it's a pain. */
void set_stack_top(uintptr_t stacktop)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	/* No need to reload the task register, this takes effect immediately */
	x86_set_stacktop_tss(pcpui->tss, stacktop);
	/* Also need to make sure sysenters come in correctly */
	x86_set_sysenter_stacktop(stacktop);
}

/* Note the check implies we only are on a one page stack (or the first page) */
uintptr_t get_stack_top(void)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	uintptr_t stacktop;
	/* so we can check this in interrupt handlers (before smp_boot()) */
	/* TODO: These are dangerous - it assumes we're on a one-page stack.  If we
	 * change it to KSTKSIZE, then we assume stacks are KSTKSIZE-aligned */
	if (!pcpui->tss)
		return ROUNDUP(read_sp(), PGSIZE);
	stacktop = x86_get_stacktop_tss(pcpui->tss);
	if (stacktop != ROUNDUP(read_sp(), PGSIZE))
		panic("Bad stacktop: %p esp one is %p\n", stacktop,
		      ROUNDUP(read_sp(), PGSIZE));
	return stacktop;
}

/* Sends a non-maskable interrupt; the handler will print a trapframe. */
void send_nmi(uint32_t os_coreid)
{
	/* NMI / IPI for x86 are limited to 8 bits */
	uint8_t hw_core = (uint8_t)get_hw_coreid(os_coreid);
	__send_nmi(hw_core);
}

void idt_init(void)
{
	/* This table is made in trapentry$BITS.S by each macro in that file.
	 * It is layed out such that the ith entry is the ith's traphandler's
	 * (uintptr_t) trap addr, then (uint32_t) trap number. */
	struct trapinfo { uintptr_t trapaddr; uint32_t trapnumber; }
	       __attribute__((packed));
	extern struct trapinfo trap_tbl[];
	extern struct trapinfo trap_tbl_end[];
	int i, trap_tbl_size = trap_tbl_end - trap_tbl;
	extern void ISR_default(void);
	extern void ISR_syscall(void);

	/* set all to default, to catch everything */
	for (i = 0; i < 256; i++)
		SETGATE(idt[i], 0, GD_KT, &ISR_default, 0);

	/* set all entries that have real trap handlers
	 * we need to stop short of the last one, since the last is the default
	 * handler with a fake interrupt number (500) that is out of bounds of
	 * the idt[] */
	for (i = 0; i < trap_tbl_size - 1; i++)
		SETGATE(idt[trap_tbl[i].trapnumber], 0, GD_KT, trap_tbl[i].trapaddr, 0);
	/* Sanity check */
	assert((uintptr_t)ISR_syscall ==
	       ((uintptr_t)idt[T_SYSCALL].gd_off_63_32 << 32 |
	        (uintptr_t)idt[T_SYSCALL].gd_off_31_16 << 16 |
	        (uintptr_t)idt[T_SYSCALL].gd_off_15_0));
	/* turn on trap-based syscall handling and other user-accessible ints
	 * DPL 3 means this can be triggered by the int instruction */
	idt[T_SYSCALL].gd_dpl = 3;
	idt[T_BRKPT].gd_dpl = 3;

	/* Set up our kernel stack when changing rings */
	/* Note: we want 16 byte aligned kernel stack frames (AMD 2:8.9.3) */
	x86_set_stacktop_tss(&ts, (uintptr_t)bootstacktop);
	x86_sysenter_init((uintptr_t)bootstacktop);

#ifdef CONFIG_KTHREAD_POISON
	*kstack_bottom_addr((uintptr_t)bootstacktop) = 0xdeadbeef;
#endif /* CONFIG_KTHREAD_POISON */

	/* Initialize the TSS field of the gdt.  The size of the TSS desc differs
	 * between 64 and 32 bit, hence the pointer acrobatics */
	syssegdesc_t *ts_slot = (syssegdesc_t*)&gdt[GD_TSS >> 3];
	*ts_slot = (syssegdesc_t)SEG_SYS_SMALL(STS_T32A, (uintptr_t)&ts,
	                                       sizeof(taskstate_t), 0);

	/* Init the IDT PD.  Need to do this before ltr for some reason.  (Doing
	 * this between ltr and lidt causes the machine to reboot... */
	idt_pd.pd_lim = sizeof(idt) - 1;
	idt_pd.pd_base = (uintptr_t)idt;

	ltr(GD_TSS);

	asm volatile("lidt %0" : : "m"(idt_pd));

	pic_remap();
	pic_mask_all();

	int ncleft = MAX_NUM_CORES;
	int num_cores_mpacpi;

	ncleft = mpsinit(ncleft);
	ncleft = mpacpi(ncleft);
	num_cores_mpacpi = MAX_NUM_CORES - ncleft;
	printk("MP and ACPI found %d cores\n", num_cores_mpacpi);
	if (num_cores != num_cores_mpacpi)
		warn("Topology (%d) and MP/ACPI (%d) differ on num_cores!", num_cores,
		     num_cores_mpacpi);

	apiconline();
	ioapiconline();

	/* the lapic IRQs need to be unmasked on a per-core basis */
	register_irq(IdtLAPIC_TIMER, timer_interrupt, NULL,
	             MKBUS(BusLAPIC, 0, 0, 0));
	register_irq(IdtLAPIC_ERROR, handle_lapic_error, NULL,
	             MKBUS(BusLAPIC, 0, 0, 0));
	register_irq(IdtLAPIC_PCINT, perfmon_interrupt, NULL,
	             MKBUS(BusLAPIC, 0, 0, 0));
	register_irq(I_KERNEL_MSG, handle_kmsg_ipi, NULL, MKBUS(BusIPI, 0, 0, 0));
}

static void handle_fperr(struct hw_trapframe *hw_tf)
{
	uint16_t fpcw, fpsw;
	uint32_t mxcsr;
	asm volatile ("fnstcw %0" : "=m"(fpcw));
	asm volatile ("fnstsw %0" : "=m"(fpsw));
	asm volatile ("stmxcsr %0" : "=m"(mxcsr));
	print_trapframe(hw_tf);
	printk("Core %d: FP ERR, CW: 0x%04x, SW: 0x%04x, MXCSR 0x%08x\n", core_id(),
	       fpcw, fpsw, mxcsr);
	printk("Core %d: The following faults are unmasked:\n", core_id());
	if (fpsw & ~fpcw & FP_EXCP_IE) {
		printk("\tInvalid Operation: ");
		if (fpsw & FP_SW_SF) {
			if (fpsw & FP_SW_C1)
				printk("Stack overflow\n");
			else
				printk("Stack underflow\n");
		} else {
			printk("invalid arithmetic operand\n");
		}
	}
	if (fpsw & ~fpcw & FP_EXCP_DE)
		printk("\tDenormalized operand\n");
	if (fpsw & ~fpcw & FP_EXCP_ZE)
		printk("\tDivide by zero\n");
	if (fpsw & ~fpcw & FP_EXCP_OE)
		printk("\tNumeric Overflow\n");
	if (fpsw & ~fpcw & FP_EXCP_UE)
		printk("\tNumeric Underflow\n");
	if (fpsw & ~fpcw & FP_EXCP_PE)
		printk("\tInexact result (precision)\n");
	printk("Killing the process.\n");
	proc_destroy(current);
}

static bool __handler_user_page_fault(struct hw_trapframe *hw_tf,
                                      uintptr_t fault_va, int prot)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	int err;

	assert(pcpui->owning_proc == pcpui->cur_proc);
	enable_irq();
	err = handle_page_fault(pcpui->owning_proc, fault_va, prot);
	disable_irq();
	if (err) {
		if (err == -EAGAIN)
			hw_tf->tf_err |= PF_VMR_BACKED;
		return FALSE;
	}
	return TRUE;
}

static bool __handler_kernel_page_fault(struct hw_trapframe *hw_tf,
                                        uintptr_t fault_va, int prot)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	int err;

	/* In general, if there's no cur_proc, a KPF is a bug. */
	if (!pcpui->cur_proc) {
		/* This only runs from test_uaccess(), where it is expected to fail. */
		if (try_handle_exception_fixup(hw_tf))
			return TRUE;
		print_trapframe(hw_tf);
		backtrace_hwtf(hw_tf);
		panic("Proc-less Page Fault in the Kernel at %p!", fault_va);
	}
	/* TODO - handle kernel page faults.  This is dangerous, since we might be
	 * holding locks in the kernel and could deadlock when we HPF.  For now, I'm
	 * just disabling the lock checker, since it'll flip out when it sees there
	 * is a kernel trap.  Will need to think about this a bit, esp when we
	 * properly handle bad addrs and whatnot. */
	pcpui->__lock_checking_enabled--;
	/* It is a bug for the kernel to access user memory while holding locks that
	 * are used by handle_page_fault.  At a minimum, this includes p->vmr_lock
	 * and memory allocation locks.
	 *
	 * In an effort to reduce the number of locks (both now and in the future),
	 * the kernel will not attempt to handle faults on file-back VMRs.  We
	 * probably can turn that on in the future, but I'd rather keep things safe
	 * for now.
	 *
	 * Note that we do not enable IRQs here, unlike in the user case.  Again,
	 * this is to limit the locks we could be grabbing. */
	err = handle_page_fault_nofile(pcpui->cur_proc, fault_va, prot);
	pcpui->__lock_checking_enabled++;
	if (err) {
		if (try_handle_exception_fixup(hw_tf))
			return TRUE;
		print_trapframe(hw_tf);
		backtrace_hwtf(hw_tf);
		/* Turn this on to help debug bad function pointers */
		printd("rsp %p\n\t 0(rsp): %p\n\t 8(rsp): %p\n\t 16(rsp): %p\n"
		       "\t24(rsp): %p\n", hw_tf->tf_rsp,
		       *(uintptr_t*)(hw_tf->tf_rsp +  0),
		       *(uintptr_t*)(hw_tf->tf_rsp +  8),
		       *(uintptr_t*)(hw_tf->tf_rsp + 16),
		       *(uintptr_t*)(hw_tf->tf_rsp + 24));
		panic("Proc-ful Page Fault in the Kernel at %p!", fault_va);
		/* if we want to do something like kill a process or other code, be
		 * aware we are in a sort of irq-like context, meaning the main
		 * kernel code we 'interrupted' could be holding locks - even
		 * irqsave locks. */
	}
	return TRUE;
}

static bool __handle_page_fault(struct hw_trapframe *hw_tf, unsigned long *aux)
{
	uintptr_t fault_va = rcr2();
	int prot = hw_tf->tf_err & PF_ERROR_WRITE ? PROT_WRITE : PROT_READ;

	*aux = fault_va;
	if (in_kernel(hw_tf))
		return __handler_kernel_page_fault(hw_tf, fault_va, prot);
	else
		return __handler_user_page_fault(hw_tf, fault_va, prot);
}

/* Certain traps want IRQs enabled, such as the syscall.  Others can't handle
 * it, like the page fault handler.  Turn them on on a case-by-case basis. */
static void trap_dispatch(struct hw_trapframe *hw_tf)
{
	struct per_cpu_info *pcpui;
	bool handled = TRUE;
	unsigned long aux = 0;
	uintptr_t fixup_ip;

	// Handle processor exceptions.
	switch(hw_tf->tf_trapno) {
		case T_NMI:
			/* Temporarily disable deadlock detection when we print.  We could
			 * deadlock if we were printing when we NMIed. */
			pcpui = &per_cpu_info[core_id()];
			pcpui->__lock_checking_enabled--;
			/* This is a bit hacky, but we don't have a decent API yet */
			extern bool mon_verbose_trace;
			if (mon_verbose_trace) {
				print_trapframe(hw_tf);
				backtrace_hwtf(hw_tf);
			}
			char *fn_name = get_fn_name(get_hwtf_pc(hw_tf));

			printk("Core %d is at %p (%s)\n", core_id(), get_hwtf_pc(hw_tf),
			       fn_name);
			kfree(fn_name);
			print_kmsgs(core_id());
			pcpui->__lock_checking_enabled++;
			break;
		case T_BRKPT:
			enable_irq();
			monitor(hw_tf);
			disable_irq();
			break;
		case T_ILLOP:
		{
			/* TODO: this can PF if there is a concurrent unmap/PM removal. */
			uintptr_t ip = get_hwtf_pc(hw_tf);
			pcpui = &per_cpu_info[core_id()];
			pcpui->__lock_checking_enabled--;		/* for print debugging */
			/* We will muck with the actual TF.  If we're dealing with
			 * userspace, we need to make sure we edit the actual TF that will
			 * get restarted (pcpui), and not the TF on the kstack (which aren't
			 * the same).  See set_current_ctx() for more info. */
			if (!in_kernel(hw_tf))
				hw_tf = &pcpui->cur_ctx->tf.hw_tf;
			printd("bad opcode, eip: %p, next 3 bytes: %x %x %x\n", ip,
			       *(uint8_t*)(ip + 0),
			       *(uint8_t*)(ip + 1),
			       *(uint8_t*)(ip + 2));
			/* rdtscp: 0f 01 f9 */
			if (*(uint8_t*)(ip + 0) == 0x0f,
			    *(uint8_t*)(ip + 1) == 0x01,
			    *(uint8_t*)(ip + 2) == 0xf9) {
				x86_fake_rdtscp(hw_tf);
				pcpui->__lock_checking_enabled++;	/* for print debugging */
				return;
			}
			enable_irq();
			monitor(hw_tf);
			disable_irq();
			pcpui->__lock_checking_enabled++;		/* for print debugging */
			break;
		}
		case T_PGFLT:
			handled = __handle_page_fault(hw_tf, &aux);
			break;
		case T_FPERR:
			handled = try_handle_exception_fixup(hw_tf);
			if (!handled)
				handle_fperr(hw_tf);
			break;
		case T_SYSCALL:
			enable_irq();
			// check for userspace, for now
			assert(hw_tf->tf_cs != GD_KT);
			/* Set up and run the async calls */
			/* TODO: this is using the wrong reg1 for traps for 32 bit */
			prep_syscalls(current,
			              (struct syscall*)x86_get_systrap_arg0(hw_tf),
						  (unsigned int)x86_get_systrap_arg1(hw_tf));
			disable_irq();
			break;
		default:
			if (hw_tf->tf_cs == GD_KT) {
				handled = try_handle_exception_fixup(hw_tf);
				if (!handled) {
					print_trapframe(hw_tf);
					panic("Damn Damn!  Unhandled trap in the kernel!");
				}
			} else {
				handled = FALSE;
			}
	}

	if (!handled)
		reflect_unhandled_trap(hw_tf->tf_trapno, hw_tf->tf_err, aux);
}

/* Helper.  For now, this copies out the TF to pcpui.  Eventually, we should
 * consider doing this in trapentry.S
 *
 * TODO: consider having this return the tf used, so we can set tf in trap and
 * irq handlers to edit the TF that will get restarted.  Right now, the kernel
 * uses and restarts tf, but userspace restarts the old pcpui tf.  It is
 * tempting to do this, but note that tf stays on the stack of the kthread,
 * while pcpui->cur_ctx is for the core we trapped in on.  Meaning if we ever
 * block, suddenly cur_ctx is pointing to some old clobbered state that was
 * already returned to and can't be trusted.  Meanwhile tf can always be trusted
 * (like with an in_kernel() check).  The only types of traps from the user that
 * can be expected to have editable trapframes are ones that don't block. */
static void set_current_ctx_hw(struct per_cpu_info *pcpui,
                               struct hw_trapframe *hw_tf)
{
	assert(!irq_is_enabled());
	pcpui->actual_ctx.type = ROS_HW_CTX;
	pcpui->actual_ctx.tf.hw_tf = *hw_tf;
	pcpui->cur_ctx = &pcpui->actual_ctx;
}

static void set_current_ctx_sw(struct per_cpu_info *pcpui,
                               struct sw_trapframe *sw_tf)
{
	assert(!irq_is_enabled());
	pcpui->actual_ctx.type = ROS_SW_CTX;
	pcpui->actual_ctx.tf.sw_tf = *sw_tf;
	pcpui->cur_ctx = &pcpui->actual_ctx;
}

static void set_current_ctx_vm(struct per_cpu_info *pcpui,
                               struct vm_trapframe *vm_tf)
{
	assert(!irq_is_enabled());
	pcpui->actual_ctx.type = ROS_VM_CTX;
	pcpui->actual_ctx.tf.vm_tf = *vm_tf;
	pcpui->cur_ctx = &pcpui->actual_ctx;
}

void trap(struct hw_trapframe *hw_tf)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	/* Copy out the TF for now */
	if (!in_kernel(hw_tf)) {
		set_current_ctx_hw(pcpui, hw_tf);
		/* ignoring state for nested kernel traps.  should be rare. */
		__set_cpu_state(pcpui, CPU_STATE_KERNEL);
	} else {
		inc_ktrap_depth(pcpui);
	}
	printd("Incoming TRAP %d on core %d, TF at %p\n", hw_tf->tf_trapno,
	       core_id(), hw_tf);
	if ((hw_tf->tf_cs & ~3) != GD_UT && (hw_tf->tf_cs & ~3) != GD_KT) {
		print_trapframe(hw_tf);
		panic("Trapframe with invalid CS!");
	}
	trap_dispatch(hw_tf);
	/* Return to the current process, which should be runnable.  If we're the
	 * kernel, we should just return naturally.  Note that current and tf need
	 * to still be okay (might not be after blocking) */
	if (in_kernel(hw_tf)) {
		dec_ktrap_depth(pcpui);
		return;
	}
	proc_restartcore();
	assert(0);
}

static bool vector_is_irq(int apic_vec)
{
	/* arguably, we could limit them to MaxIdtIOAPIC */
	return (IdtPIC <= apic_vec) && (apic_vec <= IdtMAX);
}

static void irq_dispatch(struct hw_trapframe *hw_tf)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	struct irq_handler *irq_h;

	if (!in_irq_ctx(pcpui))
		__set_cpu_state(pcpui, CPU_STATE_IRQ);
	inc_irq_depth(pcpui);
	//if (core_id())
	if (hw_tf->tf_trapno != IdtLAPIC_TIMER)	/* timer irq */
	if (hw_tf->tf_trapno != I_KERNEL_MSG)
	if (hw_tf->tf_trapno != 65)	/* qemu serial tends to get this one */
		printd("Incoming IRQ, ISR: %d on core %d\n", hw_tf->tf_trapno,
		       core_id());
	/* TODO: RCU read lock */
	irq_h = irq_handlers[hw_tf->tf_trapno];
	if (!irq_h) {
		warn_once("Received IRQ %d, had no handler registered!",
		          hw_tf->tf_trapno);
		/* If we don't have an IRQ handler, we don't know how to EOI.  Odds are,
		 * it's a LAPIC IRQ, such as I_TESTING */
		if (!lapic_check_spurious(hw_tf->tf_trapno))
			lapic_send_eoi(hw_tf->tf_trapno);
		goto out_no_eoi;
	}
	if (irq_h->check_spurious(hw_tf->tf_trapno))
		goto out_no_eoi;
	/* Can now be interrupted/nested by higher priority IRQs, but not by our
	 * current IRQ vector, til we EOI. */
	enable_irq();
	while (irq_h) {
		irq_h->isr(hw_tf, irq_h->data);
		irq_h = irq_h->next;
	}
	// if we're a general purpose IPI function call, down the cpu_list
	extern handler_wrapper_t handler_wrappers[NUM_HANDLER_WRAPPERS];
	if ((I_SMP_CALL0 <= hw_tf->tf_trapno) &&
	    (hw_tf->tf_trapno <= I_SMP_CALL_LAST))
		down_checklist(handler_wrappers[hw_tf->tf_trapno & 0x0f].cpu_list);
	disable_irq();
	/* Keep in sync with ipi_is_pending */
	irq_handlers[hw_tf->tf_trapno]->eoi(hw_tf->tf_trapno);
	/* Fall-through */
out_no_eoi:
	dec_irq_depth(pcpui);
	if (!in_irq_ctx(pcpui))
		__set_cpu_state(pcpui, CPU_STATE_KERNEL);
}

/* Note IRQs are disabled unless explicitly turned on.
 *
 * In general, we should only get trapno's >= PIC1_OFFSET (32).  Anything else
 * should be a trap.  Even if we don't use the PIC, that should be the standard.
 * It is possible to get a spurious LAPIC IRQ with vector 15 (or similar), but
 * the spurious check should catch that.
 *
 * Note that from hardware's perspective (PIC, etc), IRQs start from 0, but they
 * are all mapped up at PIC1_OFFSET for the cpu / irq_handler. */
void handle_irq(struct hw_trapframe *hw_tf)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];

	/* Copy out the TF for now */
	if (!in_kernel(hw_tf))
		set_current_ctx_hw(pcpui, hw_tf);
	irq_dispatch(hw_tf);
	/* Return to the current process, which should be runnable.  If we're the
	 * kernel, we should just return naturally.  Note that current and tf need
	 * to still be okay (might not be after blocking) */
	if (in_kernel(hw_tf))
		return;
	proc_restartcore();
	assert(0);
}

/* The irq field may be ignored based on the type of Bus. */
int register_irq(int irq, isr_t handler, void *irq_arg, uint32_t tbdf)
{
	struct irq_handler *irq_h;
	int vector;
	irq_h = kzmalloc(sizeof(struct irq_handler), 0);
	assert(irq_h);
	irq_h->dev_irq = irq;
	irq_h->tbdf = tbdf;
	vector = bus_irq_setup(irq_h);
	if (vector == -1) {
		kfree(irq_h);
		return -1;
	}
	printk("IRQ %d, vector %d (0x%x), type %s\n", irq, vector, vector,
	       irq_h->type);
	assert(irq_h->check_spurious && irq_h->eoi);
	irq_h->isr = handler;
	irq_h->data = irq_arg;
	irq_h->apic_vector = vector;
	/* RCU write lock */
	spin_lock_irqsave(&irq_handler_wlock);
	irq_h->next = irq_handlers[vector];
	wmb();	/* make sure irq_h is done before publishing to readers */
	irq_handlers[vector] = irq_h;
	spin_unlock_irqsave(&irq_handler_wlock);
	/* Most IRQs other than the BusIPI should need their irq unmasked.
	 * Might need to pass the irq_h, in case unmask needs more info.
	 * The lapic IRQs need to be unmasked on a per-core basis */
	if (irq_h->unmask && strcmp(irq_h->type, "lapic"))
		irq_h->unmask(irq_h, vector);
	return 0;
}

/* These routing functions only allow the routing of an irq to a single core.
 * If we want to route to multiple cores, we'll probably need to set up logical
 * groups or something and take some additional parameters. */
static int route_irq_h(struct irq_handler *irq_h, int os_coreid)
{
	int hw_coreid;
	if (!irq_h->route_irq) {
		printk("[kernel] apic_vec %d, type %s cannot be routed\n",
		       irq_h->apic_vector, irq_h->type);
		return -1;
	}
	if (os_coreid >= MAX_NUM_CORES) {
		printk("[kernel] os_coreid %d out of range!\n", os_coreid);
		return -1;
	}
	hw_coreid = get_hw_coreid(os_coreid);
	if (hw_coreid == -1) {
		printk("[kernel] os_coreid %d not a valid hw core!\n", os_coreid);
		return -1;
	}
	irq_h->route_irq(irq_h, irq_h->apic_vector, hw_coreid);
	return 0;
}

/* Routes all irqs for a given apic_vector to os_coreid.  Returns 0 if all of
 * them succeeded.  -1 if there were none or if any of them failed.  We don't
 * share IRQs often (if ever anymore), so this shouldn't be an issue. */
int route_irqs(int apic_vec, int os_coreid)
{
	struct irq_handler *irq_h;
	int ret = -1;
	if (!vector_is_irq(apic_vec)) {
		printk("[kernel] vector %d is not an IRQ vector!\n", apic_vec);
		return -1;
	}
	irq_h = irq_handlers[apic_vec];
	while (irq_h) {
		assert(irq_h->apic_vector == apic_vec);
		ret = route_irq_h(irq_h, os_coreid);
		irq_h = irq_h->next;
	}
	return ret;
}

/* It's a moderate pain in the ass to put these in bit-specific files (header
 * hell with the set_current_ helpers) */
void sysenter_callwrapper(struct syscall *sysc, unsigned long count,
                          struct sw_trapframe *sw_tf)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	set_current_ctx_sw(pcpui, sw_tf);
	__set_cpu_state(pcpui, CPU_STATE_KERNEL);
	/* Once we've set_current_ctx, we can enable interrupts.  This used to be
	 * mandatory (we had immediate KMSGs that would muck with cur_ctx).  Now it
	 * should only help for sanity/debugging. */
	enable_irq();
	/* Set up and run the async calls.  This may block, and we could migrate to
	 * another core.  If you use pcpui again, you need to reread it. */
	prep_syscalls(current, sysc, count);
	disable_irq();
	proc_restartcore();
}

/* Declared in x86/arch.h */
void send_ipi(uint32_t os_coreid, uint8_t vector)
{
	int hw_coreid = get_hw_coreid(os_coreid);
	if (hw_coreid == -1) {
		panic("Unmapped OS coreid (OS %d)!\n", os_coreid);
		return;
	}
	__send_ipi(hw_coreid, vector);
}

/****************** VM exit handling ******************/

static bool handle_vmexit_cpuid(struct vm_trapframe *tf)
{
	uint32_t eax, ebx, ecx, edx;

	cpuid(tf->tf_rax, tf->tf_rcx, &eax, &ebx, &ecx, &edx);
	tf->tf_rax = eax;
	tf->tf_rbx = ebx;
	tf->tf_rcx = ecx;
	tf->tf_rdx = edx;
	tf->tf_rip += 2;
	return TRUE;
}

static bool handle_vmexit_ept_fault(struct vm_trapframe *tf)
{
	int prot = 0;
	int ret;

	prot |= tf->tf_exit_qual & VMX_EPT_FAULT_READ ? PROT_READ : 0;
	prot |= tf->tf_exit_qual & VMX_EPT_FAULT_WRITE ? PROT_WRITE : 0;
	prot |= tf->tf_exit_qual & VMX_EPT_FAULT_INS ? PROT_EXEC : 0;
	ret = handle_page_fault(current, tf->tf_guest_pa, prot);
	if (ret) {
		/* TODO: maybe put ret in the TF somewhere */
		return FALSE;
	}
	return TRUE;
}

static bool handle_vmexit_nmi(struct vm_trapframe *tf)
{
	/* Sanity checks, make sure we really got an NMI.  Feel free to remove. */
	assert((tf->tf_intrinfo2 & INTR_INFO_INTR_TYPE_MASK) == INTR_TYPE_NMI_INTR);
	assert((tf->tf_intrinfo2 & INTR_INFO_VECTOR_MASK) == T_NMI);
	/* our NMI handler from trap.c won't run.  but we don't need the lock
	 * disabling stuff. */
	extern bool mon_verbose_trace;

	if (mon_verbose_trace) {
		print_vmtrapframe(tf);
		/* TODO: a backtrace of the guest would be nice here. */
	}
	printk("Core %d is at %p\n", core_id(), get_vmtf_pc(tf));
	return TRUE;
}

bool handle_vmexit_msr(struct vm_trapframe *tf)
{
	bool ret;

	ret = vmm_emulate_msr(&tf->tf_rcx, &tf->tf_rdx, &tf->tf_rax,
	                      (tf->tf_exit_reason == EXIT_REASON_MSR_READ
						   ? VMM_MSR_EMU_READ : VMM_MSR_EMU_WRITE));
	if (ret)
		tf->tf_rip += 2;
	return ret;
}

bool handle_vmexit_extirq(struct vm_trapframe *tf)
{
	struct hw_trapframe hw_tf;

	/* For now, we just handle external IRQs.  I think guest traps should go to
	 * the guest, based on our vmctls */
	assert((tf->tf_intrinfo2 & INTR_INFO_INTR_TYPE_MASK) == INTR_TYPE_EXT_INTR);
	/* TODO: Our IRQ handlers all expect TFs.  Let's fake one.  A bunch of
	 * handlers (e.g. backtrace/perf) will probably be unhappy about a user TF
	 * that is really a VM, so this all needs work. */
	hw_tf.tf_gsbase = 0;
	hw_tf.tf_fsbase = 0;
	hw_tf.tf_rax = tf->tf_rax;
	hw_tf.tf_rbx = tf->tf_rbx;
	hw_tf.tf_rcx = tf->tf_rcx;
	hw_tf.tf_rdx = tf->tf_rdx;
	hw_tf.tf_rbp = tf->tf_rbp;
	hw_tf.tf_rsi = tf->tf_rsi;
	hw_tf.tf_rdi = tf->tf_rdi;
	hw_tf.tf_r8 = tf->tf_r8;
	hw_tf.tf_r9 = tf->tf_r9;
	hw_tf.tf_r10 = tf->tf_r10;
	hw_tf.tf_r11 = tf->tf_r11;
	hw_tf.tf_r12 = tf->tf_r12;
	hw_tf.tf_r13 = tf->tf_r13;
	hw_tf.tf_r14 = tf->tf_r14;
	hw_tf.tf_r15 = tf->tf_r15;
	hw_tf.tf_trapno = tf->tf_intrinfo2 & INTR_INFO_VECTOR_MASK;
	hw_tf.tf_err = 0;
	hw_tf.tf_rip = tf->tf_rip;
	hw_tf.tf_cs = GD_UT;	/* faking a user TF, even though it's a VM */
	hw_tf.tf_rflags = tf->tf_rflags;
	hw_tf.tf_rsp = tf->tf_rsp;
	hw_tf.tf_ss = GD_UD;

	irq_dispatch(&hw_tf);
	/* Consider returning whether or not there was a handler registered */
	return TRUE;
}

static bool handle_vmexit_xsetbv(struct vm_trapframe *tf)
{
	// The VM's requested-feature bitmap is represented by edx:eax
	uint64_t vm_rfbm = (tf->tf_rdx << 32) | tf->tf_rax;

	// If the VM tries to set xcr0 to a superset
	// of Akaros's default value, kill the VM.

	// Bit in vm_rfbm and x86_default_xcr0:        Ok. Requested and allowed.
	// Bit in vm_rfbm but not x86_default_xcr0:    Bad! Requested, not allowed.
	// Bit not in vm_rfbm but in x86_default_xcr0: Ok. Not requested.

	// vm_rfbm & (~x86_default_xcr0) is nonzero if any bits
	// are set in vm_rfbm but not x86_default_xcr0

	if (vm_rfbm & (~__proc_global_info.x86_default_xcr0))
		return FALSE;


	// If attempting to use vm_rfbm for xsetbv
	// causes a fault, we reflect to the VMM.
	if (safe_lxcr0(vm_rfbm))
		return FALSE;


	// If no fault, advance the instruction pointer
	// and return TRUE to make the VM resume.
	tf->tf_rip += 3; // XSETBV is a 3-byte instruction
	return TRUE;
}

static void vmexit_dispatch(struct vm_trapframe *tf)
{
	bool handled = FALSE;

	/* Do not block in any of these functions.
	 *
	 * If we block, we'll probably need to finalize the context.  If we do, then
	 * there's a chance the guest pcore can start somewhere else, and then we
	 * can't get the GPC loaded again.  Plus, they could be running a GPC with
	 * an unresolved vmexit.  It's just mess.
	 *
	 * If we want to enable IRQs, we can do so on a case-by-case basis.  Don't
	 * do it for external IRQs - the irq_dispatch code will handle it. */
	switch (tf->tf_exit_reason) {
	case EXIT_REASON_VMCALL:
		if (current->vmm.flags & VMM_VMCALL_PRINTF) {
			printk("%c", tf->tf_rdi);
			tf->tf_rip += 3;
			handled = TRUE;
		}
		break;
	case EXIT_REASON_CPUID:
		handled = handle_vmexit_cpuid(tf);
		break;
	case EXIT_REASON_EPT_VIOLATION:
		handled = handle_vmexit_ept_fault(tf);
		break;
	case EXIT_REASON_EXCEPTION_NMI:
		handled = handle_vmexit_nmi(tf);
		break;
	case EXIT_REASON_MSR_READ:
	case EXIT_REASON_MSR_WRITE:
		handled = handle_vmexit_msr(tf);
		break;
	case EXIT_REASON_EXTERNAL_INTERRUPT:
		handled = handle_vmexit_extirq(tf);
		break;
	case EXIT_REASON_XSETBV:
		handled = handle_vmexit_xsetbv(tf);
		break;
	default:
		printd("Unhandled vmexit: reason 0x%x, exit qualification 0x%x\n",
		       tf->tf_exit_reason, tf->tf_exit_qual);
	}
	if (!handled) {
		tf->tf_flags |= VMCTX_FL_HAS_FAULT;
		if (reflect_current_context()) {
			/* VM contexts shouldn't be in vcore context, so this should be
			 * pretty rare (unlike SCPs or VC ctx page faults). */
			printk("[kernel] Unable to reflect VM Exit\n");
			print_vmtrapframe(tf);
			proc_destroy(current);
		}
	}
}


static inline uint64_t min(uint64_t a, uint64_t b) {
  if (a < b) {
    return a;
  }
  return b;
}

// Rudimentary hex dumper. Misses some corner cases on
// certain ascii values but good enough for our purposes.
void hex_dump(void *mem, uint64_t size) {
  // Prints 16 byte lines as space-separated hex pairs
  uint64_t i = size;
  uint64_t line_i = 0;
  unsigned char *next = mem;
  uint64_t print_ascii = 0;
  uint64_t line_len = min(16, size);

  while(i) {


    if (print_ascii) {
      if ('\a' == *next)      { printk("\\a"); }
      else if ('\b' == *next) { printk("\\b"); }
      else if ('\f' == *next) { printk("\\f"); }
      else if ('\n' == *next) { printk("\\n"); }
      else if ('\r' == *next) { printk("\\r"); }
      else if ('\t' == *next) { printk("\\t"); }
      else if ('\v' == *next) { printk("\\v"); }
      else if ('\\' == *next) { printk("\\ "); }
      else if ('\'' == *next) { printk("\' "); }
      else if ('\"' == *next) { printk("\" "); }
      else if ('\?' == *next) { printk("\? "); }
      else { printk("%c ", *next); }
    }
    else {
      // Print two bytes and a space
      if (0x00 == *next) { printk("-- "); }
      else               { printk("%02x ", *next); }
    }
    // Manipulate counters
    i--;
    line_i++;
    next +=1;

    if (line_len == line_i) { // we just printed the end of a line
      line_i = 0;
      if (print_ascii) { // we just printed the last ascii char of a line
        print_ascii = 0;
        printk("\n");
      }
      else { // we just printed the last hex byte of a line
        print_ascii = 1;
        // now we're going to print the line again, but in ascii
        next -= line_len;
        i += line_len;
      }
    }
  }

  printk("\n");

}

struct ancillary_state custom_anc;

int vmehdeb = 0;
void vmehde() {
	vmehdeb = 1;
}

void vmehdd() {
	vmehdeb = 0;
}

void hd_vcpd(const char *file, int line) {
	if (vmehdeb) {
		struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
		struct proc *p = pcpui->cur_proc;
		struct preempt_data *vcpd = &p->procdata->vcore_preempt_data[pcpui->owning_vcoreid];
		printk("\nVCPD:\n%s:%d:core%d\n", file, line, core_id_early());
		hex_dump(&vcpd->preempt_anc, 416);
	}
}

void hd_custom_anc() {
	if (vmehdeb) {
		printk("\nCUSTOM_ANC:\n");
		hex_dump(&custom_anc, 416);
	}
}

static void save_vc_fp_state(struct preempt_data *vcpd)
{
	save_fp_state(&vcpd->preempt_anc);
	vcpd->rflags |= VC_FPU_SAVED;
}


void handle_vmexit(struct vm_trapframe *tf)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];
	struct proc *p = pcpui->cur_proc;
	// TODO: use 0 or owning_vcoreid?
	struct preempt_data *vcpd = &p->procdata->vcore_preempt_data[pcpui->owning_vcoreid];

	//save_fp_state(&vcpd->preempt_anc);
	// save_fp_state(&custom_anc);
	// char *pcustom_anc = (char *)&custom_anc;
	// for (uint64_t i = 0; i < sizeof(struct ancillary_state); ++i) {
	// 	pcustom_anc[i] = 0;
	// }
	// save_fp_state(&custom_anc);

	// static struct ancillary_state fst;
	// static struct ancillary_state snd;
	// char *pfst = (char *)&fst;
	// char *psnd = (char *)&snd;

	// // Manually zero, can't trust sys fns not to clobber
	// for (uint64_t i = 0; i < sizeof(struct ancillary_state); ++i) {
	// 	pfst[i] = 0;
	// 	psnd[i] = 0;
	// }

	// save_fp_state(&fst);
	// if (vmehdeb) {
	// 	printk("Hex duump of the saved thing (fst):\n");
	// 	hex_dump(&fst, sizeof(struct ancillary_state));
	// }

	tf->tf_rip = vmcs_read(GUEST_RIP);
	tf->tf_rflags = vmcs_read(GUEST_RFLAGS);
	tf->tf_rsp = vmcs_read(GUEST_RSP);
	tf->tf_cr2 = rcr2();
	tf->tf_cr3 = vmcs_read(GUEST_CR3);
	tf->tf_guest_pcoreid = pcpui->guest_pcoreid;
	tf->tf_flags |= VMCTX_FL_PARTIAL;
	tf->tf_exit_reason = vmcs_read(VM_EXIT_REASON);
	tf->tf_exit_qual = vmcs_read(EXIT_QUALIFICATION);
	tf->tf_intrinfo1 = vmcs_read(GUEST_INTERRUPTIBILITY_INFO);
	tf->tf_intrinfo2 = vmcs_read(VM_EXIT_INTR_INFO);
	tf->tf_guest_va = vmcs_read(GUEST_LINEAR_ADDRESS);
	tf->tf_guest_pa = vmcs_read(GUEST_PHYSICAL_ADDRESS);

	set_current_ctx_vm(pcpui, tf);
	tf = &pcpui->cur_ctx->tf.vm_tf;
	vmexit_dispatch(tf);


	/* We're either restarting a partial VM ctx (vmcs was launched, loaded on
	 * the core, etc) or a SW vc ctx for the reflected trap.  Or the proc is
	 * dying and we'll handle a __death KMSG shortly. */
	// save_fp_state(&snd);
	// for (uint64_t i = 0; i < sizeof(struct ancillary_state); ++i) {
	// 	if (*pfst != *psnd)
	// 		panic("FP State comparison failed on byte %d", i);
	// 	pfst++;
	// 	psnd++;

	// }
	// if (vmehdeb) {
	// 	printk("Hex duump of the saved thing (snd):\n");
	// 	hex_dump(&snd, sizeof(struct ancillary_state));
	// }
	proc_restartcore();
}

void x86_finalize_vmtf(struct vm_trapframe *tf)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];

	x86_vmtf_clear_partial(tf);
	unload_guest_pcore(pcpui->cur_proc, pcpui->guest_pcoreid);
}
