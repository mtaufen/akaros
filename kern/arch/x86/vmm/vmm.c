/* Copyright 2015 Google Inc.
 *
 * See LICENSE for details.
 */

/* We're not going to falll into the trap of only compiling support
 * for AMD OR Intel for an image. It all gets compiled in, and which
 * one you use depends on on cpuinfo, not a compile-time
 * switch. That's proven to be the best strategy.  Conditionally
 * compiling in support is the path to hell.
 */
#include <assert.h>
#include <pmap.h>
#include <smp.h>
#include <kmalloc.h>

#include <ros/vmm.h>
#include "intel/vmx.h"
#include "vmm.h"
#include <trap.h>
#include <umem.h>

#include <arch/x86.h>


/* TODO: have better cpuid info storage and checks */
bool x86_supports_vmx = FALSE;

static void vmmcp_posted_handler(struct hw_trapframe *hw_tf, void *data);

/* Figure out what kind of CPU we are on, and if it supports any reasonable
 * virtualization. For now, if we're not some sort of newer intel, don't
 * bother. This does all cores. Again, note, we make these decisions at runtime,
 * to avoid getting into the problems that compile-time decisions can cause.
 * At this point, of course, it's still all intel.
 */
void vmm_init(void)
{
	int ret;
	/* Check first for intel capabilities. This is hence two back-to-back
	 * implementationd-dependent checks. That's ok, it's all msr dependent.
	 */
	ret = intel_vmm_init();
	if (! ret) {
		printd("intel_vmm_init worked\n");

		//Register I_VMMCP_POSTED IRQ
		//register_irq(I_VMMCP_POSTED, vmmcp_posted_handler, NULL,
		//		MKBUS(BusLAPIC, 0, 0, 0));
		x86_supports_vmx = TRUE;
		return;
	}

	/* TODO: AMD. Will we ever care? It's not clear. */
	printk("vmm_init failed, ret %d\n", ret);
	return;
}

static void vmmcp_posted_handler(struct hw_trapframe *hw_tf, void *data)
{
	printk("%s\n", __func__);
}

void vmm_pcpu_init(void)
{
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];

	pcpui->guest_pcoreid = -1;
	if (!x86_supports_vmx)
		return;
	if (! intel_vmm_pcpu_init()) {
		printd("vmm_pcpu_init worked\n");
		return;
	}
	/* TODO: AMD. Will we ever care? It's not clear. */
	printk("vmm_pcpu_init failed\n");
}

int vm_post_interrupt(struct vmctl *v)
{
	int vmx_interrupt_notify(struct vmctl *v);
	if (current->vmm.amd) {
		return -1;
	} else {
		return vmx_interrupt_notify(v);
	}
	return -1;
}

/* Initializes a process to run virtual machine contexts, returning the number
 * initialized, optionally setting errno */
int vmm_struct_init(struct proc *p, unsigned int nr_guest_pcores,
                    struct vmm_gpcore_init *u_gpcis, int flags)
{
	struct vmm *vmm = &p->vmm;
	unsigned int i;
	struct vmm_gpcore_init gpci;

	if (flags & ~VMM_ALL_FLAGS) {
		set_errstr("%s: flags is 0x%lx, VMM_ALL_FLAGS is 0x%lx\n", __func__,
		           flags, VMM_ALL_FLAGS);
		set_errno(EINVAL);
		return 0;
	}
	vmm->flags = flags;
	if (!x86_supports_vmx) {
		set_errno(ENODEV);
		return 0;
	}
	qlock(&vmm->qlock);
	if (vmm->vmmcp) {
		set_errno(EINVAL);
		qunlock(&vmm->qlock);
		return 0;
	}
	/* Set this early, so cleanup checks the gpc array */
	vmm->vmmcp = TRUE;
	nr_guest_pcores = MIN(nr_guest_pcores, num_cores);
	vmm->amd = 0;
	vmm->guest_pcores = kzmalloc(sizeof(void*) * nr_guest_pcores, KMALLOC_WAIT);
	for (i = 0; i < nr_guest_pcores; i++) {
		if (copy_from_user(&gpci, &u_gpcis[i],
		                   sizeof(struct vmm_gpcore_init))) {
			set_error(EINVAL, "Bad pointer %p for gps", u_gpcis);
			break;
		}
		vmm->guest_pcores[i] = create_guest_pcore(p, &gpci);
		/* If we failed, we'll clean it up when the process dies */
		if (!vmm->guest_pcores[i]) {
			set_errno(ENOMEM);
			break;
		}
	}
	vmm->nr_guest_pcores = i;
	for (int i = 0; i < VMM_VMEXIT_NR_TYPES; i++)
		vmm->vmexits[i] = 0;
	qunlock(&vmm->qlock);
	return i;
}

/* Has no concurrency protection - only call this when you know you have the
 * only ref to vmm.  For instance, from __proc_free, where there is only one ref
 * to the proc (and thus proc.vmm). */
void __vmm_struct_cleanup(struct proc *p)
{
	struct vmm *vmm = &p->vmm;
	if (!vmm->vmmcp)
		return;
	for (int i = 0; i < vmm->nr_guest_pcores; i++) {
		if (vmm->guest_pcores[i])
			destroy_guest_pcore(vmm->guest_pcores[i]);
	}
	kfree(vmm->guest_pcores);
	ept_flush(p->env_pgdir.eptp);
	vmm->vmmcp = FALSE;
}

struct guest_pcore *lookup_guest_pcore(struct proc *p, int guest_pcoreid)
{
	/* nr_guest_pcores is written once at setup and never changed */
	if (guest_pcoreid >= p->vmm.nr_guest_pcores)
		return 0;
	return p->vmm.guest_pcores[guest_pcoreid];
}

struct guest_pcore *load_guest_pcore(struct proc *p, int guest_pcoreid)
{
	struct guest_pcore *gpc;
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];

	gpc = lookup_guest_pcore(p, guest_pcoreid);
	if (!gpc)
		return 0;
	assert(pcpui->guest_pcoreid == -1);
	spin_lock(&p->vmm.lock);
	if (gpc->cpu != -1) {
		spin_unlock(&p->vmm.lock);
		return 0;
	}
	gpc->cpu = core_id();
	spin_unlock(&p->vmm.lock);
	/* We've got dibs on the gpc; we don't need to hold the lock any longer. */
	pcpui->guest_pcoreid = guest_pcoreid;
	ept_sync_context(gpc_get_eptp(gpc));
	vmx_load_guest_pcore(gpc);
	/* Load guest's xcr0 */
	lxcr0(gpc->xcr0);
	return gpc;
}

void unload_guest_pcore(struct proc *p, int guest_pcoreid)
{
	struct guest_pcore *gpc;
	struct per_cpu_info *pcpui = &per_cpu_info[core_id()];

	gpc = lookup_guest_pcore(p, guest_pcoreid);
	assert(gpc);
	spin_lock(&p->vmm.lock);
	assert(gpc->cpu != -1);
	ept_sync_context(gpc_get_eptp(gpc));
	vmx_unload_guest_pcore(gpc);
	gpc->cpu = -1;

	/* Save guest's xcr0 and restore Akaros's default. */
	gpc->xcr0 = rxcr0();
	lxcr0(x86_default_xcr0);

	/* As soon as we unlock, this gpc can be started on another core */
	spin_unlock(&p->vmm.lock);
	pcpui->guest_pcoreid = -1;
}

/* emulated msr. For now, an msr value and a pointer to a helper that
 * performs the requested operation.
 */
struct emmsr {
	uint32_t reg;
	char *name;
	bool (*f)(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
	          uint64_t *rax, uint32_t opcode);
	bool written;
	uint32_t edx, eax;
};

static bool emsr_miscenable(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                            uint64_t *rax, uint32_t opcode);
static bool emsr_mustmatch(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                           uint64_t *rax, uint32_t opcode);
static bool emsr_readonly(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                          uint64_t *rax, uint32_t opcode);
static bool emsr_readzero(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                          uint64_t *rax, uint32_t opcode);
static bool emsr_fakewrite(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                           uint64_t *rax, uint32_t opcode);
static bool emsr_ok(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                    uint64_t *rax, uint32_t opcode);
static bool emsr_fake_apicbase(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                               uint64_t *rax, uint32_t opcode);

struct emmsr emmsrs[] = {
	{MSR_IA32_MISC_ENABLE, "MSR_IA32_MISC_ENABLE", emsr_miscenable},
	{MSR_IA32_SYSENTER_CS, "MSR_IA32_SYSENTER_CS", emsr_ok},
	{MSR_IA32_SYSENTER_EIP, "MSR_IA32_SYSENTER_EIP", emsr_ok},
	{MSR_IA32_SYSENTER_ESP, "MSR_IA32_SYSENTER_ESP", emsr_ok},
	{MSR_IA32_UCODE_REV, "MSR_IA32_UCODE_REV", emsr_fakewrite},
	{MSR_CSTAR, "MSR_CSTAR", emsr_fakewrite},
	{MSR_IA32_VMX_BASIC_MSR, "MSR_IA32_VMX_BASIC_MSR", emsr_fakewrite},
	{MSR_IA32_VMX_PINBASED_CTLS_MSR, "MSR_IA32_VMX_PINBASED_CTLS_MSR",
	 emsr_fakewrite},
	{MSR_IA32_VMX_PROCBASED_CTLS_MSR, "MSR_IA32_VMX_PROCBASED_CTLS_MSR",
	 emsr_fakewrite},
	{MSR_IA32_VMX_PROCBASED_CTLS2, "MSR_IA32_VMX_PROCBASED_CTLS2",
	 emsr_fakewrite},
	{MSR_IA32_VMX_EXIT_CTLS_MSR, "MSR_IA32_VMX_EXIT_CTLS_MSR",
	 emsr_fakewrite},
	{MSR_IA32_VMX_ENTRY_CTLS_MSR, "MSR_IA32_VMX_ENTRY_CTLS_MSR",
	 emsr_fakewrite},
	{MSR_IA32_ENERGY_PERF_BIAS, "MSR_IA32_ENERGY_PERF_BIAS",
	 emsr_fakewrite},
	{MSR_LBR_SELECT, "MSR_LBR_SELECT", emsr_ok},
	{MSR_LBR_TOS, "MSR_LBR_TOS", emsr_ok},
	{MSR_LBR_NHM_FROM, "MSR_LBR_NHM_FROM", emsr_ok},
	{MSR_LBR_NHM_TO, "MSR_LBR_NHM_TO", emsr_ok},
	{MSR_LBR_CORE_FROM, "MSR_LBR_CORE_FROM", emsr_ok},
	{MSR_LBR_CORE_TO, "MSR_LBR_CORE_TO", emsr_ok},

	// grumble.
	{MSR_OFFCORE_RSP_0, "MSR_OFFCORE_RSP_0", emsr_ok},
	{MSR_OFFCORE_RSP_1, "MSR_OFFCORE_RSP_1", emsr_ok},
	// louder.
	{MSR_PEBS_LD_LAT_THRESHOLD, "MSR_PEBS_LD_LAT_THRESHOLD", emsr_ok},
	// aaaaaahhhhhhhhhhhhhhhhhhhhh
	{MSR_ARCH_PERFMON_EVENTSEL0, "MSR_ARCH_PERFMON_EVENTSEL0", emsr_ok},
	{MSR_ARCH_PERFMON_EVENTSEL1, "MSR_ARCH_PERFMON_EVENTSEL0", emsr_ok},
	{MSR_IA32_PERF_CAPABILITIES, "MSR_IA32_PERF_CAPABILITIES", emsr_ok},
	// unsafe.
	{MSR_IA32_APICBASE, "MSR_IA32_APICBASE", emsr_fake_apicbase},

	// mostly harmless.
	{MSR_TSC_AUX, "MSR_TSC_AUX", emsr_fakewrite},
	{MSR_RAPL_POWER_UNIT, "MSR_RAPL_POWER_UNIT", emsr_readzero},

	// TBD
	{MSR_IA32_TSC_DEADLINE, "MSR_IA32_TSC_DEADLINE", emsr_fakewrite},
};

/* this may be the only register that needs special handling.
 * If there others then we might want to extend the emmsr struct.
 */
bool emsr_miscenable(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                     uint64_t *rax, uint32_t opcode)
{
	uint32_t eax, edx;

	rdmsr(msr->reg, eax, edx);
	/* we just let them read the misc msr for now. */
	if (opcode == VMM_MSR_EMU_READ) {
		*rax = eax;
		*rax |= MSR_IA32_MISC_ENABLE_PEBS_UNAVAIL;
		*rdx = edx;
		return TRUE;
	} else {
		/* if they are writing what is already written, that's ok. */
		if (((uint32_t) *rax == eax) && ((uint32_t) *rdx == edx))
			return TRUE;
	}
	printk
		("%s: Wanted to write 0x%x:0x%x, but could not; value was 0x%x:0x%x\n",
		 msr->name, (uint32_t) *rdx, (uint32_t) *rax, edx, eax);
	return FALSE;
}

/* TODO: this looks like a copy-paste for the read side.  What's the purpose of
 * mustmatch?  No one even uses it. */
bool emsr_mustmatch(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                    uint64_t *rax, uint32_t opcode)
{
	uint32_t eax, edx;

	rdmsr(msr->reg, eax, edx);
	/* we just let them read the misc msr for now. */
	if (opcode == VMM_MSR_EMU_READ) {
		*rax = eax;
		*rdx = edx;
		return TRUE;
	} else {
		/* if they are writing what is already written, that's ok. */
		if (((uint32_t) *rax == eax) && ((uint32_t) *rdx == edx))
			return TRUE;
	}
	printk
		("%s: Wanted to write 0x%x:0x%x, but could not; value was 0x%x:0x%x\n",
		 msr->name, (uint32_t) *rdx, (uint32_t) *rax, edx, eax);
	return FALSE;
}

bool emsr_readonly(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                   uint64_t *rax, uint32_t opcode)
{
	uint32_t eax, edx;

	rdmsr((uint32_t) *rcx, eax, edx);
	if (opcode == VMM_MSR_EMU_READ) {
		*rax = eax;
		*rdx = edx;
		return TRUE;
	}

	printk("%s: Tried to write a readonly register\n", msr->name);
	return FALSE;
}

bool emsr_readzero(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                   uint64_t *rax, uint32_t opcode)
{
	if (opcode == VMM_MSR_EMU_READ) {
		*rax = 0;
		*rdx = 0;
		return TRUE;
	}

	printk("%s: Tried to write a readonly register\n", msr->name);
	return FALSE;
}

/* pretend to write it, but don't write it. */
bool emsr_fakewrite(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                    uint64_t *rax, uint32_t opcode)
{
	uint32_t eax, edx;

	if (!msr->written) {
		rdmsr(msr->reg, eax, edx);
	} else {
		edx = msr->edx;
		eax = msr->eax;
	}
	/* we just let them read the misc msr for now. */
	if (opcode == VMM_MSR_EMU_READ) {
		*rax = eax;
		*rdx = edx;
		return TRUE;
	} else {
		/* if they are writing what is already written, that's ok. */
		if (((uint32_t) *rax == eax) && ((uint32_t) *rdx == edx))
			return TRUE;
		msr->edx = *rdx;
		msr->eax = *rax;
		msr->written = TRUE;
	}
	return TRUE;
}

bool emsr_ok(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
             uint64_t *rax, uint32_t opcode)
{
	if (opcode == VMM_MSR_EMU_READ) {
		rdmsr(msr->reg, *rdx, *rax);
	} else {
		uint64_t val = (uint64_t) *rdx << 32 | *rax;

		write_msr(msr->reg, val);
	}
	return TRUE;
}

/* pretend to write it, but don't write it. */
bool emsr_fake_apicbase(struct emmsr *msr, uint64_t *rcx, uint64_t *rdx,
                        uint64_t *rax, uint32_t opcode)
{
	uint32_t eax, edx;

	if (!msr->written) {
		//rdmsr(msr->reg, eax, edx);
		/* TODO: tightly coupled to the addr in vmrunkernel.  We want this func
		 * to return the val that vmrunkernel put into the VMCS. */
		eax = 0xfee00900;
		edx = 0;
	} else {
		edx = msr->edx;
		eax = msr->eax;
	}
	/* we just let them read the misc msr for now. */
	if (opcode == VMM_MSR_EMU_READ) {
		*rax = eax;
		*rdx = edx;
		return TRUE;
	} else {
		/* if they are writing what is already written, that's ok. */
		if (((uint32_t) *rax == eax) && ((uint32_t) *rdx == edx))
			return 0;
		msr->edx = *rdx;
		msr->eax = *rax;
		msr->written = TRUE;
	}
	return TRUE;
}

bool vmm_emulate_msr(uint64_t *rcx, uint64_t *rdx, uint64_t *rax, int op)
{
	for (int i = 0; i < ARRAY_SIZE(emmsrs); i++) {
		if (emmsrs[i].reg != *rcx)
			continue;
		return emmsrs[i].f(&emmsrs[i], rcx, rdx, rax, op);
	}
	return FALSE;
}
