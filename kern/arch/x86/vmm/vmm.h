#pragma once

#include <ros/vmm.h>

static inline int cpu_has_vmx(void)
{
	unsigned long ecx = cpuid_ecx(1);
	return ecx & (1<<5); /* CPUID.1:ECX.VMX[bit 5] -> VT */
}

/* maybe someday, not today. */
static inline int cpu_has_svm(const char **msg)
{
	return 0;
}

#define VMM_VMEXIT_NR_TYPES		65

struct guest_pcore {
	int cpu;
	struct proc *proc;
	struct msr_autoload {
		unsigned nr;
		struct vmx_msr_entry guest[NR_AUTOLOAD_MSRS];
		struct vmx_msr_entry host[NR_AUTOLOAD_MSRS];
	} msr_autoload;
	struct vmcs *vmcs;
	uint64_t xcr0;
};

struct vmm {
	spinlock_t lock;	/* protects guest_pcore assignment */
	qlock_t qlock;
	// always false.
	int amd;
	// true if this is a VMMCP.
	bool vmmcp;

	int flags;

	// Number of cores in this VMMCP.
	int nr_guest_pcores;

	// The VMCS is intel-specific. But, maybe, someday, AMD will
	// be back.  Just make this an anon union and we'll work it
	// all out later. Again, remember, we're compiling in support
	// for both architectures to ensure that we can correctly
	// figure out at boot time what we're on and what we should
	// do. This avoids the problem seen years ago with RH6 where
	// you could install a kernel from the ISO, but the kernel it
	// installed would GPF on a K7.
	union {
		void *svm;
		struct guest_pcore **guest_pcores;
	};
	unsigned long vmexits[VMM_VMEXIT_NR_TYPES];
};

void vmm_init(void);
void vmm_pcpu_init(void);

int vmm_struct_init(struct proc *p, unsigned int nr_guest_pcores,
                    struct vmm_gpcore_init *gpcis, int flags);
void __vmm_struct_cleanup(struct proc *p);

int vm_post_interrupt(struct vmctl *v);
int intel_vmx_start(int id);
int intel_vmx_setup(int nvmcs);

struct guest_pcore *create_guest_pcore(struct proc *p,
                                       struct vmm_gpcore_init *gpci);
void destroy_guest_pcore(struct guest_pcore *vcpu);
uint64_t construct_eptp(physaddr_t root_hpa);
void ept_flush(uint64_t eptp);

struct guest_pcore *lookup_guest_pcore(struct proc *p, int guest_pcoreid);
struct guest_pcore *load_guest_pcore(struct proc *p, int guest_pcoreid);
void unload_guest_pcore(struct proc *p, int guest_pcoreid);

#define VMM_MSR_EMU_READ		1
#define VMM_MSR_EMU_WRITE		2
bool vmm_emulate_msr(uint64_t *rcx, uint64_t *rdx, uint64_t *rax, int op);
