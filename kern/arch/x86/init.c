/* See COPYRIGHT for copyright information. */

#include <smp.h>

#include <arch/x86.h>
#include <arch/pci.h>
#include <arch/console.h>
#include <arch/perfmon.h>
#include <arch/init.h>
#include <console.h>
#include <monitor.h>
#include <arch/usb.h>
#include <assert.h>


/*
	x86_default_xcr0 is the Akaros-wide
	default value for the xcr0 register.

	It is set on every processor during
	per-cpu init.
*/
uint64_t x86_default_xcr0;
struct ancillary_state x86_default_fpu;
uint32_t kerndate;

#define capchar2ctl(x) ((x) - '@')

/* irq handler for the console (kb, serial, etc) */
static void irq_console(struct hw_trapframe *hw_tf, void *data)
{
	uint8_t c;
	struct cons_dev *cdev = (struct cons_dev*)data;
	assert(cdev);
	if (cons_get_char(cdev, &c))
		return;
	/* Control code intercepts */
	switch (c) {
		case capchar2ctl('G'):
			/* traditional 'ctrl-g', will put you in the monitor gracefully */
			send_kernel_message(core_id(), __run_mon, 0, 0, 0, KMSG_ROUTINE);
			return;
		case capchar2ctl('Q'):
			/* force you into the monitor.  you might deadlock. */
			printk("\nForcing entry to the monitor\n");
			monitor(hw_tf);
			return;
		case capchar2ctl('B'):
			/* backtrace / debugging for the core receiving the irq */
			printk("\nForced trapframe and backtrace for core %d\n", core_id());
			if (!hw_tf) {
				printk("(no hw_tf, we probably polled the console)\n");
				return;
			}
			print_trapframe(hw_tf);
			backtrace_hwtf(hw_tf);
			return;
	}
	/* Do our work in an RKM, instead of interrupt context.  Note the RKM will
	 * cast 'c' to a char. */
	send_kernel_message(core_id(), __cons_add_char, (long)&cons_buf, (long)c,
	                    0, KMSG_ROUTINE);
}

static void cons_poller(void *arg)
{
	while (1) {
		kthread_usleep(10000);
		irq_console(0, arg);
	}
}

static void cons_irq_init(void)
{
	struct cons_dev *i;
	/* Register interrupt handlers for all console devices */
	SLIST_FOREACH(i, &cdev_list, next) {
		register_irq(i->irq, irq_console, i, MKBUS(BusISA, 0, 0, 0));
#ifdef CONFIG_POLL_CONSOLE
		ktask("cons_poller", cons_poller, i);
#endif /* CONFIG_POLL_CONSOLE */
	}
}

#define CPUID_XSAVE_SUPPORT         (1 << 26)
#define CPUID_XSAVEOPT_SUPPORT      (1 << 0)
void x86_extended_state_init() {
	uint32_t eax, ebx, ecx, edx;
	uint64_t proc_supported_features; /* proc supported user state components */

	// Note: The cpuid function comes from arch/x86.h
	// arg1 is eax input, arg2 is ecx input, then
	// eax, ebx, ecx, edx.

	// First check general XSAVE support. Die if not supported.
	cpuid(0x01, 0x00, 0, 0, &ecx, 0);
	if (!(CPUID_XSAVE_SUPPORT & ecx)) {
		panic("No XSAVE support! Refusing to boot.\n");
	}

	// Next check XSAVEOPT support. Die if not supported.
	cpuid(0x0d, 0x01, &eax, 0, 0, 0);
	if (!(CPUID_XSAVEOPT_SUPPORT & eax)) {
		panic("No XSAVEOPT support! Refusing to boot.\n");
	}

	// Next determine the user state components supported
	// by the processor and set x86_default_xcr0.

	cpuid(0x0d, 0x00, &eax, 0, 0, &edx);
	proc_supported_features = ((uint64_t)edx << 32) | eax;

	// Intersection of processor-supported and Akaros-supported
	// features is the Akaros-wide default at runtime.
	x86_default_xcr0 = X86_MAX_XCR0 & proc_supported_features;
}

void arch_init()
{

	x86_extended_state_init();

	// TODO/XXX: Think about moving fninit and the save_fp_state call
	//           into x86_extended_state_init.

	/* need to reinit before saving, in case boot agents used the FPU or it is
	 * o/w dirty.  had this happen on c89, which had a full FP stack after
	 * booting. */
	asm volatile ("fninit");
	save_fp_state(&x86_default_fpu); /* used in arch/trap.h for fpu init */
	pci_init();
	vmm_init();
	perfmon_global_init();
	// this returns when all other cores are done and ready to receive IPIs
	#ifdef CONFIG_SINGLE_CORE
		smp_percpu_init();
	#else
		smp_boot();
	#endif
	proc_init();

	cons_irq_init();
	intel_lpc_init();
#ifdef CONFIG_ENABLE_LEGACY_USB
	printk("Legacy USB support enabled, expect SMM interference!\n");
#else
	usb_disable_legacy();
#endif
	check_timing_stability();
}
