#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <parlib/arch/arch.h>
#include <parlib/ros_debug.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <ros/syscall.h>
#include <sys/mman.h>
#include <vmm/coreboot_tables.h>
#include <vmm/vmm.h>
#include <vmm/acpi/acpi.h>
#include <vmm/acpi/vmm_simple_dsdt.h>
#include <ros/arch/mmu.h>
#include <ros/arch/membar.h>
#include <ros/vmm.h>
#include <parlib/uthread.h>
#include <vmm/linux_bootparam.h>

#include <vmm/virtio.h>
#include <vmm/virtio_mmio.h>

#include <vmm/virtio_ids.h>
#include <vmm/virtio_config.h>

#include <vmm/sched.h>

#include <sys/eventfd.h>
#include <sys/uio.h>

struct vmctl vmctl;
struct vmm_gpcore_init gpci;

/* Whoever holds the ball runs.  run_vm never actually grabs it - it is grabbed
 * on its behalf. */
uth_mutex_t the_ball;
pthread_t vm_thread;

void (*old_thread_refl)(struct uthread *uth, struct user_context *ctx);

static void copy_vmtf_to_vmctl(struct vm_trapframe *vm_tf, struct vmctl *vmctl)
{
	vmctl->cr3 = vm_tf->tf_cr3;
	vmctl->gva = vm_tf->tf_guest_va;
	vmctl->gpa = vm_tf->tf_guest_pa;
	vmctl->exit_qual = vm_tf->tf_exit_qual;
	if (vm_tf->tf_exit_reason == EXIT_REASON_EPT_VIOLATION)
		vmctl->shutdown = SHUTDOWN_EPT_VIOLATION;
	else
		vmctl->shutdown = SHUTDOWN_UNHANDLED_EXIT_REASON;
	vmctl->ret_code = vm_tf->tf_exit_reason;
	vmctl->interrupt = vm_tf->tf_trap_inject;
	vmctl->intrinfo1 = vm_tf->tf_intrinfo1;
	vmctl->intrinfo2 = vm_tf->tf_intrinfo2;
	/* Most of the HW TF.  Should be good enough for now */
	vmctl->regs.tf_rax = vm_tf->tf_rax;
	vmctl->regs.tf_rbx = vm_tf->tf_rbx;
	vmctl->regs.tf_rcx = vm_tf->tf_rcx;
	vmctl->regs.tf_rdx = vm_tf->tf_rdx;
	vmctl->regs.tf_rbp = vm_tf->tf_rbp;
	vmctl->regs.tf_rsi = vm_tf->tf_rsi;
	vmctl->regs.tf_rdi = vm_tf->tf_rdi;
	vmctl->regs.tf_r8  = vm_tf->tf_r8;
	vmctl->regs.tf_r9  = vm_tf->tf_r9;
	vmctl->regs.tf_r10 = vm_tf->tf_r10;
	vmctl->regs.tf_r11 = vm_tf->tf_r11;
	vmctl->regs.tf_r12 = vm_tf->tf_r12;
	vmctl->regs.tf_r13 = vm_tf->tf_r13;
	vmctl->regs.tf_r14 = vm_tf->tf_r14;
	vmctl->regs.tf_r15 = vm_tf->tf_r15;
	vmctl->regs.tf_rip = vm_tf->tf_rip;
	vmctl->regs.tf_rflags = vm_tf->tf_rflags;
	vmctl->regs.tf_rsp = vm_tf->tf_rsp;
}

static void copy_vmctl_to_vmtf(struct vmctl *vmctl, struct vm_trapframe *vm_tf)
{
	vm_tf->tf_rax = vmctl->regs.tf_rax;
	vm_tf->tf_rbx = vmctl->regs.tf_rbx;
	vm_tf->tf_rcx = vmctl->regs.tf_rcx;
	vm_tf->tf_rdx = vmctl->regs.tf_rdx;
	vm_tf->tf_rbp = vmctl->regs.tf_rbp;
	vm_tf->tf_rsi = vmctl->regs.tf_rsi;
	vm_tf->tf_rdi = vmctl->regs.tf_rdi;
	vm_tf->tf_r8  = vmctl->regs.tf_r8;
	vm_tf->tf_r9  = vmctl->regs.tf_r9;
	vm_tf->tf_r10 = vmctl->regs.tf_r10;
	vm_tf->tf_r11 = vmctl->regs.tf_r11;
	vm_tf->tf_r12 = vmctl->regs.tf_r12;
	vm_tf->tf_r13 = vmctl->regs.tf_r13;
	vm_tf->tf_r14 = vmctl->regs.tf_r14;
	vm_tf->tf_r15 = vmctl->regs.tf_r15;
	vm_tf->tf_rip = vmctl->regs.tf_rip;
	vm_tf->tf_rflags = vmctl->regs.tf_rflags;
	vm_tf->tf_rsp = vmctl->regs.tf_rsp;
	vm_tf->tf_cr3 = vmctl->cr3;
	vm_tf->tf_trap_inject = vmctl->interrupt;
	/* Don't care about the rest of the fields.  The kernel only writes them */
}

/* callback, runs in vcore context.  this sets up our initial context.  once we
 * become runnable again, we'll run the first bits of the vm ctx.  after that,
 * our context will be stopped and started and will just run whatever the guest
 * VM wants.  we'll never come back to this code or to run_vm(). */
static void __build_vm_ctx_cb(struct uthread *uth, void *arg)
{
	struct pthread_tcb *pthread = (struct pthread_tcb*)uth;
	struct vmctl *vmctl = (struct vmctl*)arg;
	struct vm_trapframe *vm_tf;

	__pthread_generic_yield(pthread);
	pthread->state = PTH_BLK_YIELDING;

	memset(&uth->u_ctx, 0, sizeof(struct user_context));
	uth->u_ctx.type = ROS_VM_CTX;
	vm_tf = &uth->u_ctx.tf.vm_tf;

	vm_tf->tf_guest_pcoreid = 0;	/* assuming only 1 guest core */

	copy_vmctl_to_vmtf(vmctl, vm_tf);

	/* other HW/GP regs are 0, which should be fine.  the FP state is still
	 * whatever we were running before, though this is pretty much unnecessary.
	 * we mostly don't want crazy crap in the uth->as, and a non-current_uthread
	 * VM ctx is supposed to have something in their FP state (like HW ctxs). */
	save_fp_state(&uth->as);
	uth->flags |= UTHREAD_FPSAVED | UTHREAD_SAVED;

	uthread_runnable(uth);
}

static void *run_vm(void *arg)
{
	struct vmctl *vmctl = (struct vmctl*)arg;

	assert(vmctl->command == REG_RSP_RIP_CR3);
	/* We need to hack our context, so that next time we run, we're a VM ctx */
	uthread_yield(FALSE, __build_vm_ctx_cb, arg);
}

static void vmm_thread_refl_fault(struct uthread *uth,
                                  struct user_context *ctx)
{
	struct pthread_tcb *pthread = (struct pthread_tcb*)uth;

	/* Hack to call the original pth 2LS op */
	if (!ctx->type == ROS_VM_CTX) {
		old_thread_refl(uth, ctx);
		return;
	}
	__pthread_generic_yield(pthread);
	/* normally we'd handle the vmexit here.  to work within the existing
	 * framework, we just wake the controller thread.  It'll look at our ctx
	 * then make us runnable again */
	pthread->state = PTH_BLK_MUTEX;
	uth_mutex_unlock(the_ball);		/* wake the run_vmthread */
}



/* this will start the vm thread, and return when the thread has blocked,
 * with the right info in vmctl. */
static void run_vmthread(struct vmctl *vmctl)
{
	struct vm_trapframe *vm_tf;

	if (!vm_thread) {
		/* first time through, we make the vm thread.  the_ball was already
		 * grabbed right after it was alloc'd. */
		if (pthread_create(&vm_thread, NULL, run_vm, vmctl)) {
			perror("pth_create");
			exit(-1);
		}
		/* hack in our own handlers for some 2LS ops */
		old_thread_refl = sched_ops->thread_refl_fault;
		sched_ops->thread_refl_fault = vmm_thread_refl_fault;
	} else {
		copy_vmctl_to_vmtf(vmctl, &vm_thread->uthread.u_ctx.tf.vm_tf);
		uth_mutex_lock(the_ball);	/* grab it for the vm_thread */
		uthread_runnable((struct uthread*)vm_thread);
	}
	uth_mutex_lock(the_ball);
	/* We woke due to a vm exit.  Need to unlock for the next time we're run */
	uth_mutex_unlock(the_ball);
	/* the vm stopped.  we can do whatever we want before rerunning it.  since
	 * we're controlling the uth, we need to handle its vmexits.  we'll fill in
	 * the vmctl, since that's the current framework. */
	copy_vmtf_to_vmctl(&vm_thread->uthread.u_ctx.tf.vm_tf, vmctl);
}

/* By 1999, you could just scan the hardware
 * and work it out. But 2005, that was no longer possible. How sad.
 * so we have to fake acpi to make it all work.
 * This will be copied to memory at 0xe0000, so the kernel can find it.
 */

/* assume they're all 256 bytes long just to make it easy.
 * Just have pointers that point to aligned things.
 */

struct acpi_table_rsdp rsdp = {
	.signature = ACPI_SIG_RSDP,
	.oem_id = "AKAROS",
	.revision = 2,
	.length = 36,
};

struct acpi_table_xsdt xsdt = {
	.header = {
		.signature = ACPI_SIG_DSDT,
		.revision = 2,
		.oem_id = "AKAROS",
		.oem_table_id = "ALPHABET",
		.oem_revision = 0,
		.asl_compiler_id = "RON ",
		.asl_compiler_revision = 0,
	},
};
struct acpi_table_fadt fadt = {
	.header = {
		.signature = ACPI_SIG_FADT,
		.revision = 2,
		.oem_id = "AKAROS",
		.oem_table_id = "ALPHABET",
		.oem_revision = 0,
		.asl_compiler_id = "RON ",
		.asl_compiler_revision = 0,
	},
};


/* This has to be dropped into memory, then the other crap just follows it.
 */
struct acpi_table_madt madt = {
	.header = {
		.signature = ACPI_SIG_MADT,
		.revision = 2,
		.oem_id = "AKAROS",
		.oem_table_id = "ALPHABET",
		.oem_revision = 0,
		.asl_compiler_id = "RON ",
		.asl_compiler_revision = 0,
	},

	.address = 0xfee00000ULL,
};

struct acpi_madt_local_apic Apic0 = {.header = {.type = ACPI_MADT_TYPE_LOCAL_APIC, .length = sizeof(struct acpi_madt_local_apic)},
				     .processor_id = 0, .id = 0};
struct acpi_madt_io_apic Apic1 = {.header = {.type = ACPI_MADT_TYPE_IO_APIC, .length = sizeof(struct acpi_madt_io_apic)},
				  .id = 1, .address = 0xfec00000, .global_irq_base = 0};
struct acpi_madt_local_x2apic X2Apic0 = {
	.header = {
		.type = ACPI_MADT_TYPE_LOCAL_X2APIC,
		.length = sizeof(struct acpi_madt_local_x2apic)
	},
	.local_apic_id = 0,
	.uid = 0
};

struct acpi_madt_interrupt_override isor[] = {
	/* I have no idea if it should be source irq 2, global 0, or global 2, source 0. Shit. */
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 2, .global_irq = 0, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 1, .global_irq = 1, .inti_flags = 0},
	//{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 //.bus = 0, .source_irq = 2, .global_irq = 2, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 3, .global_irq = 3, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 4, .global_irq = 4, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 5, .global_irq = 5, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 6, .global_irq = 6, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 7, .global_irq = 7, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 8, .global_irq = 8, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 9, .global_irq = 9, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 10, .global_irq = 10, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 11, .global_irq = 11, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 12, .global_irq = 12, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 13, .global_irq = 13, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 14, .global_irq = 14, .inti_flags = 0},
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 15, .global_irq = 15, .inti_flags = 0},
	// VMMCP routes irq 32 to gsi 17
	{.header = {.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, .length = sizeof(struct acpi_madt_interrupt_override)},
	 .bus = 0, .source_irq = 32, .global_irq = 17, .inti_flags = 5},
};


/* this test will run the "kernel" in the negative address space. We hope. */
void *low1m;
uint8_t low4k[4096];
unsigned long long stack[1024];
volatile int shared = 0;
volatile int quit = 0;
int mcp = 1;
int virtioirq = 17;

/* total hack. If the vm runs away we want to get control again. */
unsigned int maxresume = (unsigned int) -1;

#define MiB 0x100000u
#define GiB (1u<<30)
#define GKERNBASE (16*MiB)
#define KERNSIZE (128*MiB+GKERNBASE)
uint8_t _kernel[KERNSIZE];

unsigned long long *p512, *p1, *p2m;

void **my_retvals;
int nr_threads = 4;
int debug = 0;
int resumeprompt = 0;
/* unlike Linux, this shared struct is for both host and guest. */
//	struct virtqueue *constoguest =
//		vring_new_virtqueue(0, 512, 8192, 0, inpages, NULL, NULL, "test");
uint64_t virtio_mmio_base = 0x100000000ULL;

void vapic_status_dump(FILE *f, void *vapic);
static void set_posted_interrupt(int vector);

#if __GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 1)
#error "Get a gcc newer than 4.4.0"
#else
#define BITOP_ADDR(x) "+m" (*(volatile long *) (x))
#endif

#define LOCK_PREFIX "lock "
#define ADDR				BITOP_ADDR(addr)
static inline int test_and_set_bit(int nr, volatile unsigned long *addr);

pthread_t timerthread_struct;

void *timer_thread(void *arg)
{
	uint8_t vector;
	uint32_t initial_count;
	while (1) {
		vector = ((uint32_t *)gpci.vapic_addr)[0x32] & 0xff;
		initial_count = ((uint32_t *)gpci.vapic_addr)[0x38];
		if (vector && initial_count) {
			set_posted_interrupt(vector);
			ros_syscall(SYS_vmm_poke_guest, 0, 0, 0, 0, 0, 0);
		}
		uthread_usleep(100000);
	}
	fprintf(stderr, "SENDING TIMER\n");
}



// FIXME. probably remove consdata TODO
volatile int consdata = 0;
static struct virtio_mmio_dev cons_mmio_dev;

// For traversing the linked list of descriptors
// Also based on Linux's lguest.c
uint32_t get_next_desc(struct vring_desc *desc, uint32_t i, uint32_t max)
{
	uint32_t next;

	if (!(desc[i].flags & VRING_DESC_F_NEXT)) {
		// No more in the chain, so return max to signal that we reached the end
		return max;
	}

	next = desc[i].next;

	// TODO: what does this wmb actually end up compiling as now that we're out of linux?
	wmb(); // just because lguest put it here. not sure why they did that yet.

	// TODO: Make this an actual error with a real error message
	// TODO: Figure out what lguest.c's "bad_driver" function does
	if (next >= max) {
		printf("NONONONONONO. NO!\nYou can not tell me I have a desc at an index outside the queue.\nYou liar!\nvmrunkernel.c-get_next_desc\n");
	}

	return next;
}

// TODO: Rename this fn
// Based on wait_for_vq_desc in Linux lguest.c
uint32_t next_avail_vq_desc(struct virtio_vq *vq, struct iovec iov[], // TODO: scatterlist is just some type sitting in our virtio.h. We can clean this up.
                            uint32_t *olen, uint32_t *ilen)
{
	uint32_t i, head, max;
	struct vring_desc *desc;
	eventfd_t event;

	// The first thing we do is read from the eventfd. If nothing has been written to it yet,
	// then the driver isn't done setting things up and we want to wait for it to finish.
	// For example, dereferencing the vq->vring.avail pointer could segfault if the driver
	// has not yet written a valid address to it.
	if (eventfd_read(vq->eventfd, &event))
		printf("next_avail_vq_desc event read failed?\n");
	// TODO: I think I want a memory barrier here? In case the event fd gets written but the avail idx hasn't yet?
	mb();

	while (vq->last_avail == vq->vring.avail->idx) {
		// If we got poked but there are no queues available, then just return 0
		// this will work for now because we're only in these handlers during an
		// exit due to EPT viol. due to driver write to QUEUE_NOTIFY register on dev.
		// return 0;



		// TODO: We will move from just returning zero to just sleeping
		//       again if we were kicked but nothing is available.
		// NOTE: I do not kick the guest with an irq here. I do that in
		//       the individual service functions when it is necessary.

		// TODO: What to do here about VRING_DESC_F_NO_NOTIFY flag?
		// NOTE: If you look at the comments in virtio_ring.h, the VRING_DESC_F_NO_NOTIFY
		//       flag is set by the host to say to the guest "Don't kick me when you add
		//       a buffer." But this comment also says that it is an optimization, is not
		//       always reliable, and that the guest will still kick the host when out of
		//       buffers. So I'm leaving that out for now, and we can revisit why it might
		//       improve performance sometime in the future.
		//       TODO: That said, I might still need to unset the bit. It should be unset
		//             by default, because it is only supposed to be set by the host and
		//             I never set it. But this is worth double-checking.


		if (eventfd_read(vq->eventfd, &event))
			printf("next_avail_vq_desc event read failed?\n");

		// TODO: Do I want a memory barrier here? In case the event fd gets written but the avail idx hasn't yet?
		mb();
	}

	// Mod because it's a *ring*
	// TODO: maybe switch to using vq->vring.num for ours too
	head = vq->vring.avail->ring[vq->last_avail % vq->vring.num];
	vq->last_avail++;

	// TODO: make this an actual error
	if (head >= vq->vring.num)
		printf("dumb dumb dumb driver. head >= vq->vring.num in next_avail_vq_desc in vmrunkernel\n");

	// Don't know how many output buffers or input buffers there are yet, depends on desc chain.
	*olen = *ilen = 0;

	max = vq->vring.num; // Since vring.num is the size of the queue, max is the most buffers we could possibly find
	desc = vq->vring.desc; // qdesc is the address of the descriptor (array?TODO) set by the driver
	i = head;

	// TODO: lguest manually checks that the pointers on the vring fields aren't goofy when the driver
	//       initally says they are ready, we should probably do that somewhere too.

	/*NOTE: (from lguest)
	 * We have to read the descriptor after we read the descriptor number,
	 * but there's a data dependency there so the CPU shouldn't reorder
	 * that: no rmb() required.
	 */


	do {

		// If it's an indirect descriptor, we travel through the layer of indirection and then
		// we're at table of descriptors chained by `next`, and since they are chained we can
		// handle them the same way as direct descriptors once we're through that indirection.
		if (desc[i].flags & VRING_DESC_F_INDIRECT) {
			// TODO: lguest says bad_driver if they gave us an indirect desc but didn't set the right
			//       feature bit for indirect descs. Not gonna check that for now, since I might rearrange
			//       where the feature bits live, and it won't be particularly dangerous since we live in
			//       a bubble for the time being. But we should start checking that in the future.
			//       Before the bubble bursts.

			// TODO: Should also error if we find an indirect within an indirect (only one table per desc)
			//       lguest seems to interpret this as "the only indirect desc can be the first in the chain"
			//       I trust Rusty on that interpretation. (desc != vq->vring.desc is a bad_driver)
			if (desc != vq->vring.desc)
				printf("Bad! Indirect desc within indirect desc!\n");

			// TODO: handle error (again, see lguest's bad_driver_vq) if these checks fail too
			if (desc[i].flags & VRING_DESC_F_NEXT) // can't set NEXT if you're INDIRECT (e.g. table vs linked list entry)
				printf("virtio Error: indirect and next both set\n");

			if (desc[i].len % sizeof(struct vring_desc)) // nonzero mod indicates wrong table size
				printf("virtio Error; bad size for indirect table\n");

			// NOTE: Virtio spec says the device MUST ignore the write-only flag in the
			//       descriptor for an indirect table. So we ignore it.

			max = desc[i].len / sizeof(struct vring_desc);
			desc = (void *)desc[i].addr; // TODO: check that this pointer isn't goofy
			i = 0;


			// TODO: Make this a real error too. The driver MUST NOT create a descriptor chain longer
			//       than the Queue Size of the device.
			// Mike XXX: Where did we put the queue size of the device? lguest has it on pci config
			//           since we're not pci, I think we want vq->vring.num. In fact, in lguest vring.num
			//           is the same as pci config's queue size, and we are going to let the driver
			//           set the vring.num for mmio (I figure), since I think this is where we'll put
			//           the thing written to the QueueNum register (how big the queues the driver will
			//           use are).
			// TODO: do we allow the driver to write something greater than QueueNumMax to QueueNum?
			//       checking both vring.num and maxqnum for now, need to double check whether we
			//       actually just need vring.num to be checked.
			if (max > vq->vring.num || max > vq->maxqnum) {
				//TODO make this an actual error
				printf("indirect desc has too many entries. number greater than vq->maxqnum\n");
			}
		}

		// Now build the scatterlist of descriptors
		// TODO: And, you know, we ought to check the pointers on these descriptors too!
		// TODO: You better make sure you pass a big enough scatterlist to this function
		//       for whatever the eventual value of *olen + *ilen will be!
		iov[*olen + *ilen].iov_len = desc[i].len;
		iov[*olen + *ilen].iov_base = (void *)desc[i].addr; // NOTE: .v is basically our scatterlist/iovec's iov_base

		if (desc[i].flags & VRING_DESC_F_WRITE) {
			// input descriptor, increment *ilen
			(*ilen)++;
		}
		else {
			// output descriptor, check that this is *before* we read any input descriptors
			// and then increment *olen if we're ok

			// TODO: Make this an actual error
			if (*ilen) {
				printf("Bad! Output descriptor came after an input descriptor!\n");
			}

			(*olen)++;
		}

		if (*olen + *ilen > max) {
			// TODO: make this an actual error!
			printf("The descriptor probably looped somewhere! BAD! (*olen + *ilen > max)\n");
		}


	} while ((i = get_next_desc(desc, i, max)) != max);

	return head;

}

// TODO: Rename this to something more succinct and understandable!
// Based on the add_used function in lguest.c
// Adds descriptor chain to the used ring of the vq
static void add_used_desc(struct virtio_vq *vq, uint32_t head, uint32_t len)
{
	// NOTE: len is the total length of the descriptor chain (in bytes)
	//       that was written to.
	//       So you should pass 0 if you didn't write anything, and pass
	//       the number of bytes you wrote otherwise.
	vq->vring.used->ring[vq->vring.used->idx % vq->vring.num].id = head;
	vq->vring.used->ring[vq->vring.used->idx % vq->vring.num].len = len;
	// TODO: what does this wmb actually end up compiling as now that we're out of linux?
	wmb(); // So the values get written to the used buffer before we update idx
	vq->vring.used->idx++;
}


static void *cons_receiveq_fn(void *_vq) // host -> guest
{
	struct virtio_vq *vq = _vq;
	uint32_t head;
	uint32_t olen, ilen;
	uint32_t i, j;
	// TODO: scatterlist dvec. I think we need this to be big enough for the max buffers in a queue.
	//       But I think that is set by the driver... for now I just made it really big. Don't really
	//       want to dynamically allocate due to overhead. Maybe we'll arbitrarily define a MaxQueueNum
	//       for our mmio devices and use that here.(I think that's what qemu does)
	static struct iovec iov[1024];
	int num_read;

	printf("cons_receiveq_fn called.\n\targ: %p, qname: %s\n", vq, vq->name);

	// NOTE: This will wait in 2 places: reading from stdin and reading from eventfd in next_avail_vq_desc
	while(1) {
		head = next_avail_vq_desc(vq, iov, &olen, &ilen);

		if (olen) {
			// TODO: Make this an actual error!
			printf("cons_receiveq ERROR: output buffers in console input queue!\n");
		}



		/*
			TODO: Does the driver give us empty buffers? Does it even matter?
			      i.e. we might get junk, but it's junk from the guest so giving
			      the same junk back shouldn't be a problem. Unless it then thinks
			      that that junk is input that it needs to process. So maybe
			      we should clear it just in case? TODO: Check the virtio spec
			      to see if providing clean buffers is the responsibility of the
			      device or the driver.
		*/


		// TODO: Some sort of console abort (e.g. type q and enter to quit)
		// readv from stdin as much as we can (either to end of buffers or end of input)
		num_read = readv(0, iov, ilen);
		if (num_read < 0) {
			exit(0); // some error happened TODO better error handling here
		}

		// You pass the number of bytes written to add_used_desc
		add_used_desc(vq, head, num_read);

		// set the low bit of the interrupt status register and trigger an interrupt
		virtio_mmio_set_vring_irq(vq->vqdev->transport_dev); // just assuming that the mmio transport was used for now
		// I think this is how we send the guest an interrupt. Definitely the way we have to do it
		// concurrently. Not sure if doing this during an exit will mess things up...
		// also not sure if 0xE5 is the right one to send... TODO
		set_posted_interrupt(0xE5);
		ros_syscall(SYS_vmm_poke_guest, 0, 0, 0, 0, 0, 0);

	}
	return NULL;
}

static void *cons_transmitq_fn(void *_vq) // guest -> host
{
	struct virtio_vq *vq = _vq;
	uint32_t head;
	uint32_t olen, ilen;
	uint32_t i, j;
	// TODO: scatterlist dvec. I think we need this to be big enough for the max buffers in a queue.
	//       But I think that is set by the driver... for now I just made it really big. Don't really
	//       want to dynamically allocate due to overhead. Maybe we'll arbitrarily define a MaxQueueNum
	//       for our mmio devices and use that here.(I think that's what qemu does)
	static struct iovec iov[1024];

	while(1) {

	// TODO: Look at the lguest.c implementation of this as well, they do things
	//       slightly differently and I want to double check my work!

		// 1. get the buffers:
		head = next_avail_vq_desc(vq, iov, &olen, &ilen);

		if (ilen) {
			// TODO: Make this an actual error!
			printf("cons_transmitq ERROR: input buffers in console output queue!\n");
		}

		// 2. process the buffers:
		for (i = 0; i < olen; ++i) {
			for (j = 0; j < iov[i].iov_len; ++j) {
				printf("%c", ((char *)iov[i].iov_base)[j]);
			}
		}
		fflush(stdout);

		// 3: Add all the buffers to the used ring:
		// Pass 0 because we wrote nothing.
		add_used_desc(vq, head, 0);

		// 4. set the low bit of the interrupt status register and trigger an interrupt
		virtio_mmio_set_vring_irq(vq->vqdev->transport_dev); // just assuming that the mmio transport was used for now
		// I think this is how we send the guest an interrupt. Definitely the way we have to do it
		// concurrently. Not sure if doing this during an exit will mess things up...
		// also not sure if 0xE5 is the right one to send... TODO
		set_posted_interrupt(0xE5);
		ros_syscall(SYS_vmm_poke_guest, 0, 0, 0, 0, 0, 0);
	}
	return NULL;
}



/*
5.3.6 Device Operation

1. For output, a buffer containing the characters is placed in the port’s transmitq.
2. When a buffer is used in the receiveq (signalled by an interrupt), the contents is the input
   to the port associated with the virtqueue for which the notification was received.
...

5.3.6.1 Driver Requirements: Device Operation

The driver MUST NOT put a device-readable in a receiveq.
The driver MUST NOT put a device-writable buffer in a transmitq.

*/

static struct virtio_vq_dev cons_vqdev = {
	name: "console",
	dev_id: VIRTIO_ID_CONSOLE,
	dev_feat: (uint64_t)1 << VIRTIO_F_VERSION_1,
	numvqs: 2,
	transport_dev: &cons_mmio_dev,
	vqs: {
			{
				name: "cons_receiveq (host dev to guest driver)",
				maxqnum: 64,
				srv_fn: cons_receiveq_fn,
				vqdev: &cons_vqdev
			},
			{
				name: "cons_transmitq (guest driver to host dev)",
				maxqnum: 64,
				srv_fn: cons_transmitq_fn,
				vqdev: &cons_vqdev
			},
		}
};

// TODO: still have to figure out what maxqnum is...

// Recieve thread (not sure whether it's "vm is recving" or "vmm is recving" yet)
static void * netrecv(void *arg)
{
	return NULL;
}


// Send thread (not sure whether it's "vm is sending" or "vmm is sending" yet)
static void * netsend(void *arg)
{
	return NULL;
}

// ^ since the queues will have the same names in the vm and the host we'll just
// make the functions have the same name too.

static struct virtio_vq_dev vq_net_dev = {
	name: "net",
	dev_id: VIRTIO_ID_NET,
	dev_feat: VIRTIO_F_VERSION_1,
	numvqs: 2,
	vqs: {
			{name: "netrecv", maxqnum: 64, srv_fn: netrecv}, // queue 0 is the console dev receiveq
			{name: "netsend", maxqnum: 64, srv_fn: netsend}, // queue 1 is the console dev transmitq
		}
};

void lowmem() {
	__asm__ __volatile__ (".section .lowmem, \"aw\"\n\tlow: \n\t.=0x1000\n\t.align 0x100000\n\t.previous\n");
}

static uint8_t acpi_tb_checksum(uint8_t *buffer, uint32_t length)
{
	uint8_t sum = 0;
	uint8_t *end = buffer + length;
	fprintf(stderr, "tbchecksum %p for %d", buffer, length);
	while (buffer < end) {
		if (end - buffer < 2)
			fprintf(stderr, "%02x\n", sum);
		sum = (uint8_t)(sum + *(buffer++));
	}
	fprintf(stderr, " is %02x\n", sum);
	return (sum);
}

static void gencsum(uint8_t *target, void *data, int len)
{
	uint8_t csum;
	// blast target to zero so it does not get counted
	// (it might be in the struct we checksum) And, yes, it is, goodness.
	fprintf(stderr, "gencsum %p target %p source %d bytes\n", target, data, len);
	*target = 0;
	csum  = acpi_tb_checksum((uint8_t *)data, len);
	*target = ~csum + 1;
	fprintf(stderr, "Cmoputed is %02x\n", *target);
}

static inline int test_and_set_bit(int nr, volatile unsigned long *addr)
{
	int oldbit;

	asm volatile(LOCK_PREFIX "bts %2,%1\n\t"
		     "sbb %0,%0" : "=r" (oldbit), ADDR : "Ir" (nr) : "memory");

	return oldbit;
}

static void pir_dump()
{
	unsigned long *pir_ptr = gpci.posted_irq_desc;
	int i;
	fprintf(stderr, "-------Begin PIR dump-------\n");
	for (i = 0; i < 8; i++){
		fprintf(stderr, "Byte %d: 0x%016x\n", i, pir_ptr[i]);
	}
	fprintf(stderr, "-------End PIR dump-------\n");
}

static void set_posted_interrupt(int vector)
{
	test_and_set_bit(vector, gpci.posted_irq_desc);
	/* LOCKed instruction provides the mb() */
	test_and_set_bit(VMX_POSTED_OUTSTANDING_NOTIF, gpci.posted_irq_desc);
}

int main(int argc, char **argv)
{
	struct boot_params *bp;
	char *cmdline_default = "earlyprintk=vmcall,keep"
		                    " console=hvc0"
		                    " virtio_mmio.device=1M@0x100000000:32"
		                    " nosmp"
		                    " maxcpus=1"
		                    " acpi.debug_layer=0x2"
		                    " acpi.debug_level=0xffffffff"
		                    " apic=debug"
		                    " noexec=off"
		                    " nohlt"
		                    " init=/bin/launcher"
		                    " lapic=notscdeadline"
		                    " lapictimerfreq=1000000"
		                    " pit=none";
	char *cmdline_extra = "\0";
	char *cmdline;
	uint64_t *p64;
	void *a = (void *)0xe0000;
	struct acpi_table_rsdp *r;
	struct acpi_table_fadt *f;
	struct acpi_table_madt *m;
	struct acpi_table_xsdt *x;
	uint64_t virtiobase = 0x100000000ULL;
	// lowmem is a bump allocated pointer to 2M at the "physbase" of memory
	void *lowmem = (void *) 0x1000000;
	//struct vmctl vmctl;
	int amt;
	int vmmflags = 0; // Disabled probably forever. VMM_VMCALL_PRINTF;
	uint64_t entry = 0x1200000, kerneladdress = 0x1200000;
	int nr_gpcs = 1;
	int ret;
	void * xp;
	int kfd = -1;
	static char cmd[512];
	int i;
	uint8_t csum;
	void *coreboot_tables = (void *) 0x1165000;
	void *a_page;
	struct vm_trapframe *vm_tf;
	uint64_t tsc_freq_khz;

	the_ball = uth_mutex_alloc();
	uth_mutex_lock(the_ball);

	fprintf(stderr, "%p %p %p %p\n", PGSIZE, PGSHIFT, PML1_SHIFT,
			PML1_PTE_REACH);


	// mmap is not working for us at present.
	if ((uint64_t)_kernel > GKERNBASE) {
		fprintf(stderr, "kernel array @%p is above , GKERNBASE@%p sucks\n", _kernel, GKERNBASE);
		exit(1);
	}
	memset(_kernel, 0, sizeof(_kernel));
	memset(lowmem, 0xff, 2*1048576);
	memset(low4k, 0xff, 4096);
	// avoid at all costs, requires too much instruction emulation.
	//low4k[0x40e] = 0;
	//low4k[0x40f] = 0xe0;

	//Place mmap(Gan)
	a_page = mmap((void *)0xfee00000, PGSIZE, PROT_READ | PROT_WRITE,
		              MAP_POPULATE | MAP_ANONYMOUS, -1, 0);
	fprintf(stderr, "a_page mmap pointer %p\n", a_page);

	if (a_page == (void *) -1) {
		perror("Could not mmap APIC");
		exit(1);
	}
	if (((uint64_t)a_page & 0xfff) != 0) {
		perror("APIC page mapping is not page aligned");
		exit(1);
	}

	memset(a_page, 0, 4096);
	((uint32_t *)a_page)[0x30/4] = 0x01060015;
	//((uint32_t *)a_page)[0x30/4] = 0xDEADBEEF;


	argc--, argv++;
	// switches ...
	// Sorry, I don't much like the gnu opt parsing code.
	while (1) {
		if (*argv[0] != '-')
			break;
		switch(argv[0][1]) {
		case 'd':
			debug++;
			break;
		case 'v':
			vmmflags |= VMM_VMCALL_PRINTF;
			break;
		case 'm':
			argc--, argv++;
			maxresume = strtoull(argv[0], 0, 0);
			break;
		case 'i':
			argc--, argv++;
			virtioirq = strtoull(argv[0], 0, 0);
			break;
		case 'c':
			argc--, argv++;
			cmdline_extra = argv[0];
		default:
			fprintf(stderr, "BMAFR\n");
			break;
		}
		argc--, argv++;
	}
	if (argc < 1) {
		fprintf(stderr, "Usage: %s vmimage [-n (no vmcall printf)] [coreboot_tables [loadaddress [entrypoint]]]\n", argv[0]);
		exit(1);
	}
	if (argc > 1)
		coreboot_tables = (void *) strtoull(argv[1], 0, 0);
	if (argc > 2)
		kerneladdress = strtoull(argv[2], 0, 0);
	if (argc > 3)
		entry = strtoull(argv[3], 0, 0);
	kfd = open(argv[0], O_RDONLY);
	if (kfd < 0) {
		perror(argv[0]);
		exit(1);
	}
	// read in the kernel.
	xp = (void *)kerneladdress;
	for(;;) {
		amt = read(kfd, xp, 1048576);
		if (amt < 0) {
			perror("read");
			exit(1);
		}
		if (amt == 0) {
			break;
		}
		xp += amt;
	}
	fprintf(stderr, "Read in %d bytes\n", xp-kerneladdress);
	close(kfd);

	// The low 1m so we can fill in bullshit like ACPI. */
	// And, sorry, due to the STUPID format of the RSDP for now we need the low 1M.
	low1m = mmap((int*)4096, MiB-4096, PROT_READ | PROT_WRITE,
	                 MAP_ANONYMOUS, -1, 0);
	if (low1m != (void *)4096) {
		perror("Unable to mmap low 1m");
		exit(1);
	}
	memset(low1m, 0xff, MiB-4096);
	r = a;
	fprintf(stderr, "install rsdp to %p\n", r);
	*r = rsdp;
	a += sizeof(*r);
	r->xsdt_physical_address = (uint64_t)a;
	gencsum(&r->checksum, r, ACPI_RSDP_CHECKSUM_LENGTH);
	if ((csum = acpi_tb_checksum((uint8_t *) r, ACPI_RSDP_CHECKSUM_LENGTH)) != 0) {
		fprintf(stderr, "RSDP has bad checksum; summed to %x\n", csum);
		exit(1);
	}

	/* Check extended checksum if table version >= 2 */
	gencsum(&r->extended_checksum, r, ACPI_RSDP_XCHECKSUM_LENGTH);
	if ((rsdp.revision >= 2) &&
	    (acpi_tb_checksum((uint8_t *) r, ACPI_RSDP_XCHECKSUM_LENGTH) != 0)) {
		fprintf(stderr, "RSDP has bad checksum v2\n");
		exit(1);
	}

	/* just leave a bunch of space for the xsdt. */
	/* we need to zero the area since it has pointers. */
	x = a;
	a += sizeof(*x) + 8*sizeof(void *);
	memset(x, 0, a - (void *)x);
	fprintf(stderr, "install xsdt to %p\n", x);
	*x = xsdt;
	x->table_offset_entry[0] = 0;
	x->table_offset_entry[1] = 0;
	x->header.length = a - (void *)x;

	f = a;
	fprintf(stderr, "install fadt to %p\n", f);
	*f = fadt;
	x->table_offset_entry[0] = (uint64_t)f; // fadt MUST be first in xsdt!
	a += sizeof(*f);
	f->header.length = a - (void *)f;

	f->Xdsdt = (uint64_t) a;
	fprintf(stderr, "install dsdt to %p\n", a);
	memcpy(a, &DSDT_DSDTTBL_Header, 36);
	a += 36;

	gencsum(&f->header.checksum, f, f->header.length);
	if (acpi_tb_checksum((uint8_t *)f, f->header.length) != 0) {
		fprintf(stderr, "fadt has bad checksum v2\n");
		exit(1);
	}

	m = a;
	*m = madt;
	x->table_offset_entry[3] = (uint64_t) m;
	a += sizeof(*m);
	fprintf(stderr, "install madt to %p\n", m);
	memmove(a, &Apic0, sizeof(Apic0));
	a += sizeof(Apic0);
	memmove(a, &Apic1, sizeof(Apic1));
	a += sizeof(Apic1);
	memmove(a, &X2Apic0, sizeof(X2Apic0));
	a += sizeof(X2Apic0);
	memmove(a, &isor, sizeof(isor));
	a += sizeof(isor);
	m->header.length = a - (void *)m;

	gencsum(&m->header.checksum, m, m->header.length);
	if (acpi_tb_checksum((uint8_t *) m, m->header.length) != 0) {
		fprintf(stderr, "madt has bad checksum v2\n");
		exit(1);
	}

	gencsum(&x->header.checksum, x, x->header.length);
	if ((csum = acpi_tb_checksum((uint8_t *) x, x->header.length)) != 0) {
		fprintf(stderr, "XSDT has bad checksum; summed to %x\n", csum);
		exit(1);
	}



	fprintf(stderr, "allchecksums ok\n");

	hexdump(stdout, r, a-(void *)r);

	a = (void *)(((unsigned long)a + 0xfff) & ~0xfff);
	gpci.posted_irq_desc = a;
	memset(a, 0, 4096);
	a += 4096;
	gpci.vapic_addr = a;
	//vmctl.vapic = (uint64_t) a_page;
	memset(a, 0, 4096);
	((uint32_t *)a)[0x30/4] = 0x01060014;
	p64 = a;
	// set up apic values? do we need to?
	// qemu does this.
	//((uint8_t *)a)[4] = 1;
	a += 4096;
	gpci.apic_addr = (void*)0xfee00000;

	/* Allocate memory for, and zero the bootparams
	 * page before writing to it, or Linux thinks
	 * we're talking crazy.
	 */
	a += 4096;
	bp = a;
	memset(bp, 0, 4096);

	/* Set the kernel command line parameters */
	a += 4096;
	cmdline = a;
	a += 4096;
	bp->hdr.cmd_line_ptr = (uintptr_t) cmdline;
	tsc_freq_khz = get_tsc_freq()/1000;
	sprintf(cmdline, "%s tscfreq=%lld %s", cmdline_default, tsc_freq_khz,
	        cmdline_extra);


	/* Put the e820 memory region information in the boot_params */
	bp->e820_entries = 3;
	int e820i = 0;

	bp->e820_map[e820i].addr = 0;
	bp->e820_map[e820i].size = 16 * 1048576;
	bp->e820_map[e820i++].type = E820_RESERVED;

	bp->e820_map[e820i].addr = 16 * 1048576;
	bp->e820_map[e820i].size = 128 * 1048576;
	bp->e820_map[e820i++].type = E820_RAM;

	bp->e820_map[e820i].addr = 0xf0000000;
	bp->e820_map[e820i].size = 0x10000000;
	bp->e820_map[e820i++].type = E820_RESERVED;

	if (ros_syscall(SYS_vmm_setup, nr_gpcs, &gpci, vmmflags, 0, 0, 0) !=
	    nr_gpcs) {
		perror("Guest pcore setup failed");
		exit(1);
	}

	fprintf(stderr, "Run with %d cores and vmmflags 0x%x\n", nr_gpcs, vmmflags);
	mcp = 1;
	if (mcp) {
		my_retvals = malloc(sizeof(void*) * nr_threads);
		if (!my_retvals)
			perror("Init threads/malloc");

		pthread_can_vcore_request(FALSE);	/* 2LS won't manage vcores */
		pthread_need_tls(FALSE);
		pthread_mcp_init();					/* gives us one vcore */
		vcore_request(nr_threads - 1);		/* ghetto incremental interface */
		for (int i = 0; i < nr_threads; i++) {
			xp = __procinfo.vcoremap;
			fprintf(stderr, "%p\n", __procinfo.vcoremap);
			fprintf(stderr, "Vcore %d mapped to pcore %d\n", i,
			    	__procinfo.vcoremap[i].pcoreid);
		}
	}

	ret = syscall(33, 1);
	if (ret < 0) {
		perror("vm setup");
		exit(1);
	}
	ret = posix_memalign((void **)&p512, 4096, 3*4096);
	fprintf(stderr, "memalign is %p\n", p512);
	if (ret) {
		perror("ptp alloc");
		exit(1);
	}
	p1 = &p512[512];
	p2m = &p512[1024];
	uint64_t kernbase = 0; //0xffffffff80000000;
	uint64_t highkernbase = 0xffffffff80000000;
	p512[PML4(kernbase)] = (unsigned long long)p1 | 7;
	p1[PML3(kernbase)] = /*0x87; */(unsigned long long)p2m | 7;
	p512[PML4(highkernbase)] = (unsigned long long)p1 | 7;
	p1[PML3(highkernbase)] = /*0x87; */(unsigned long long)p2m | 7;
#define _2MiB (0x200000)

	for (i = 0; i < 512; i++) {
		p2m[PML2(kernbase + i * _2MiB)] = 0x87 | i * _2MiB;
	}

	kernbase >>= (0+12);
	kernbase <<= (0 + 12);
	uint8_t *kernel = (void *)GKERNBASE;
	//write_coreboot_table(coreboot_tables, ((void *)VIRTIOBASE) /*kernel*/, KERNSIZE + 1048576);
	hexdump(stdout, coreboot_tables, 512);
	fprintf(stderr, "kernbase for pml4 is 0x%llx and entry is %llx\n", kernbase, entry);
	fprintf(stderr, "p512 %p p512[0] is 0x%lx p1 %p p1[0] is 0x%x\n", p512, p512[0], p1, p1[0]);
	vmctl.interrupt = 0;
	vmctl.command = REG_RSP_RIP_CR3;
	vmctl.cr3 = (uint64_t) p512;
	vmctl.regs.tf_rip = entry;
	vmctl.regs.tf_rsp = (uint64_t) &stack[1024];
	vmctl.regs.tf_rsi = (uint64_t) bp;
	if (mcp) {
		/* set up virtio bits, which depend on threads being enabled. */
		//register_virtio_mmio(&cons_vqdev, virtio_mmio_base);
		cons_mmio_dev.addr = virtio_mmio_base;
		cons_mmio_dev.vqdev = &cons_vqdev;

		// Create the eventfds and launch the service threads for the console
		// TODO: Do this in a better place!
		cons_mmio_dev.vqdev->vqs[0].eventfd = eventfd(0, 0); // TODO: Look into "semaphore mode"
		fprintf(stderr, "eventfd is: %d\n", cons_mmio_dev.vqdev->vqs[0].eventfd);
		if (pthread_create(&cons_mmio_dev.vqdev->vqs[0].srv_th,
			               NULL,
			               cons_mmio_dev.vqdev->vqs[0].srv_fn,
			               &cons_mmio_dev.vqdev->vqs[0])) {
			// service thread creation failed
			// TODO: Make this an actual error.
			fprintf(stderr, "pth_create failed for cons_mmio_dev vq 0 (receive)\n");
		}



		cons_mmio_dev.vqdev->vqs[1].eventfd = eventfd(0, 0); // TODO: Look into "semaphore mode"
		fprintf(stderr, "eventfd is: %d\n", cons_mmio_dev.vqdev->vqs[1].eventfd);
		if (pthread_create(&cons_mmio_dev.vqdev->vqs[1].srv_th,
			               NULL,
			               cons_mmio_dev.vqdev->vqs[1].srv_fn,
			               &cons_mmio_dev.vqdev->vqs[1])) {
			// service thread creation failed
			// TODO: Make this an actual error.
			fprintf(stderr, "pth_create failed for cons_mmio_dev vq 1 (transmit)\n");
		}


	}
	fprintf(stderr, "threads started\n");
	fprintf(stderr, "Writing command :%s:\n", cmd);

	if (debug)
		vapic_status_dump(stderr, (void *)gpci.vapic_addr);

	run_vmthread(&vmctl);

	if (debug)
		vapic_status_dump(stderr, (void *)gpci.vapic_addr);

	if (mcp) {
		/* Start up timer thread */
		if (pthread_create(&timerthread_struct, NULL, timer_thread, NULL)) {
			fprintf(stderr, "pth_create failed for timer thread.");
			perror("pth_create");
		}
	}

	vm_tf = &(vm_thread->uthread.u_ctx.tf.vm_tf);

	while (1) {

		int c;
		uint8_t byte;
		//vmctl.command = REG_RIP;
		if (maxresume-- == 0) {
			debug = 1;
			resumeprompt = 1;
		}
		if (debug) {
			fprintf(stderr, "RIP %p, exit reason 0x%x\n", vm_tf->tf_rip,
			        vm_tf->tf_exit_reason);
			showstatus(stderr, (struct guest_thread*)&vm_thread);
		}
		if (resumeprompt) {
			fprintf(stderr, "RESUME?\n");
			c = getchar();
			if (c == 'q')
				break;
		}
		if (vm_tf->tf_exit_reason == EXIT_REASON_EPT_VIOLATION) {
			uint64_t gpa, *regp, val;
			uint8_t regx;
			int store, size;
			int advance;
			if (decode((struct guest_thread *) vm_thread, &gpa, &regx, &regp,
			           &store, &size, &advance)) {
				fprintf(stderr, "RIP %p, shutdown 0x%x\n", vm_tf->tf_rip,
				        vm_tf->tf_exit_reason);
				showstatus(stderr, (struct guest_thread*)&vm_thread);
				quit = 1;
				break;
			}
			if (debug) fprintf(stderr, "%p %p %p %p %p %p\n", gpa, regx, regp, store, size, advance);

			if ((gpa & ~0xfffULL) == virtiobase) {
				// printf("DO SOME VIRTIO\n");
				// Lucky for us the various virtio ops are well-defined.
				//virtio_mmio((struct guest_thread *)vm_thread, gpa, regx, regp, store);
				if (store) {
					virtio_mmio_wr_reg(&cons_mmio_dev, gpa, (uint32_t *)regp);
				}
				else {
					*regp = virtio_mmio_rd_reg(&cons_mmio_dev, gpa);
				}
				// printf("after virtio read or wr in vmrunkernel \n");


				if (debug) fprintf(stderr, "store is %d:\n", store);
				if (debug) fprintf(stderr, "REGP IS %16x:\n", *regp);
			} else if ((gpa & 0xfee00000) == 0xfee00000) {
				// until we fix our include mess, just put the proto here.
				//int apic(struct vmctl *v, uint64_t gpa, int destreg, uint64_t *regp, int store);
				//apic(&vmctl, gpa, regx, regp, store);
			} else if ((gpa & 0xfec00000) == 0xfec00000) {
				// until we fix our include mess, just put the proto here.
				do_ioapic((struct guest_thread *)vm_thread, gpa, regx, regp,
				          store);
			} else if (gpa < 4096) {
				uint64_t val = 0;
				memmove(&val, &low4k[gpa], size);
				hexdump(stdout, &low4k[gpa], size);
				fprintf(stderr, "Low 1m, code %p read @ %p, size %d, val %p\n",
				        vm_tf->tf_rip, gpa, size, val);
				memmove(regp, &low4k[gpa], size);
				hexdump(stdout, regp, size);
			} else {
				fprintf(stderr, "EPT violation: can't handle %p\n", gpa);
				fprintf(stderr, "RIP %p, exit reason 0x%x\n", vm_tf->tf_rip,
				        vm_tf->tf_exit_reason);
				fprintf(stderr, "Returning 0xffffffff\n");
				showstatus(stderr, (struct guest_thread*)&vm_thread);
				// Just fill the whole register for now.
				*regp = (uint64_t) -1;
			}
			vm_tf->tf_rip += advance;
			if (debug)
				fprintf(stderr, "Advance rip by %d bytes to %p\n",
				        advance, vm_tf->tf_rip);
			//vmctl.shutdown = 0;
			//vmctl.gpa = 0;
			//vmctl.command = REG_ALL;
		} else {
			switch (vm_tf->tf_exit_reason) {
			case  EXIT_REASON_VMCALL:
				byte = vm_tf->tf_rdi;
				printf("%c", byte);
				if (byte == '\n') printf("%c", '%');
				vm_tf->tf_rip += 3;
				break;
			case EXIT_REASON_EXTERNAL_INTERRUPT:
				//debug = 1;
				if (debug)
					fprintf(stderr, "XINT 0x%x 0x%x\n",
					        vm_tf->tf_intrinfo1, vm_tf->tf_intrinfo2);
				if (debug) pir_dump();
				//vmctl.command = RESUME;
				break;
			case EXIT_REASON_IO_INSTRUCTION:
				fprintf(stderr, "IO @ %p\n", vm_tf->tf_rip);
				io((struct guest_thread *)vm_thread);
				//vmctl.shutdown = 0;
				//vmctl.gpa = 0;
				//vmctl.command = REG_ALL;
				break;
			case EXIT_REASON_INTERRUPT_WINDOW:
				printf("does this ever happen?\n");
				if (consdata) {
					if (debug) fprintf(stderr, "inject an interrupt\n");
					virtio_mmio_set_vring_irq(&cons_mmio_dev);
					vm_tf->tf_trap_inject = 0x80000000 | virtioirq;
					//vmctl.command = RESUME;
					consdata = 0;
				}
				break;
			case EXIT_REASON_MSR_WRITE:
			case EXIT_REASON_MSR_READ:
				fprintf(stderr, "Do an msr\n");
				if (msrio((struct guest_thread *)vm_thread, &gpci,
				          vm_tf->tf_exit_reason)) {
					// uh-oh, msrio failed
					// well, hand back a GP fault which is what Intel does
					fprintf(stderr, "MSR FAILED: RIP %p, shutdown 0x%x\n",
					        vm_tf->tf_rip, vm_tf->tf_exit_reason);
					showstatus(stderr, (struct guest_thread*)&vm_thread);

					// Use event injection through vmctl to send
					// a general protection fault
					// vmctl.interrupt gets written to the VM-Entry
					// Interruption-Information Field by vmx
					vm_tf->tf_trap_inject = VM_TRAP_VALID
					                      | VM_TRAP_ERROR_CODE
					                      | VM_TRAP_HARDWARE
					                      | 13; // GPF
				} else {
					vm_tf->tf_rip += 2;
				}
				break;
			case EXIT_REASON_MWAIT_INSTRUCTION:
			  fflush(stdout);
				if (debug)fprintf(stderr, "\n================== Guest MWAIT. =======================\n");
				if (debug)fprintf(stderr, "Wait for cons data\n");
				while (!consdata)
					;
				//debug = 1;
				if (debug)
					vapic_status_dump(stderr, gpci.vapic_addr);
				if (debug)fprintf(stderr, "Resume with consdata ...\n");
				vm_tf->tf_rip += 3;
				//fprintf(stderr, "RIP %p, shutdown 0x%x\n", vmctl.regs.tf_rip, vmctl.shutdown);
				//showstatus(stderr, (struct guest_thread*)&vm_thread);
				break;
			case EXIT_REASON_HLT:
				fflush(stdout);
				if (debug)fprintf(stderr, "\n================== Guest halted. =======================\n");
				if (debug)fprintf(stderr, "Wait for cons data\n");
				while (!consdata)
					;
				//debug = 1;
				if (debug)fprintf(stderr, "Resume with consdata ...\n");
				vm_tf->tf_rip += 1;
				//fprintf(stderr, "RIP %p, shutdown 0x%x\n", vmctl.regs.tf_rip, vmctl.shutdown);
				//showstatus(stderr, (struct guest_thread*)&vm_thread);
				break;
			case EXIT_REASON_APIC_ACCESS:
				if (1 || debug)fprintf(stderr, "APIC READ EXIT\n");

				uint64_t gpa, *regp, val;
				uint8_t regx;
				int store, size;
				int advance;
				if (decode((struct guest_thread *)vm_thread, &gpa, &regx,
				           &regp, &store, &size, &advance)) {
					fprintf(stderr, "RIP %p, shutdown 0x%x\n", vm_tf->tf_rip,
					        vm_tf->tf_exit_reason);
					showstatus(stderr, (struct guest_thread*)&vm_thread);
					quit = 1;
					break;
				}

				int apic(struct guest_thread *vm_thread, uint64_t gpa,
				         int destreg, uint64_t *regp, int store);
				apic((struct guest_thread *)vm_thread, gpa, regx, regp, store);
				vm_tf->tf_rip += advance;
				if (debug)
					fprintf(stderr, "Advance rip by %d bytes to %p\n",
					        advance, vm_tf->tf_rip);
				//vmctl.shutdown = 0;
				//vmctl.gpa = 0;
				//vmctl.command = REG_ALL;
				break;
			case EXIT_REASON_APIC_WRITE:
				if (1 || debug)fprintf(stderr, "APIC WRITE EXIT\n");
				break;
			default:
				fprintf(stderr, "Don't know how to handle exit %d\n",
				        vm_tf->tf_exit_reason);
				fprintf(stderr, "RIP %p, shutdown 0x%x\n", vm_tf->tf_rip,
				        vm_tf->tf_exit_reason);
				showstatus(stderr, (struct guest_thread*)&vm_thread);
				quit = 1;
				break;
			}
		}
		if (debug) fprintf(stderr, "at bottom of switch, quit is %d\n", quit);
		if (quit)
			break;
		if (consdata) {
			if (debug) fprintf(stderr, "inject an interrupt\n");
			if (debug)
				fprintf(stderr, "XINT 0x%x 0x%x\n", vm_tf->tf_intrinfo1,
				        vm_tf->tf_intrinfo2);
			vm_tf->tf_trap_inject = 0x80000000 | virtioirq;
			virtio_mmio_set_vring_irq(&cons_mmio_dev);
			consdata = 0;
			//debug = 1;
			//vmctl.command = RESUME;
		}
		if (debug) fprintf(stderr, "NOW DO A RESUME\n");
		copy_vmtf_to_vmctl(vm_tf, &vmctl);
		run_vmthread(&vmctl);
		copy_vmctl_to_vmtf(&vmctl, vm_tf);
	}

	/* later.
	for (int i = 0; i < nr_threads-1; i++) {
		int ret;
		if (pthread_join(my_threads[i], &my_retvals[i]))
			perror("pth_join failed");
		fprintf(stderr, "%d %d\n", i, ret);
	}
 */

	fflush(stdout);
	exit(0);
}
