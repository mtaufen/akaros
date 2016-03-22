

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/*
SERIOUS TODO: We MUST pin this test to a single core, because the TSC clocks may
              not be synchronized between cores! If the process is migrated, then
              the count could be wrong.


Tests the performance of xsave64 vs xsaveopt64

xsave64/xrstor64 will be our baseline

We will always time the xsave64 and xrstor64 separately


There four kinds of tests:

1. Baseline tells us difference for init optimization

2. Dirtying outside the loop tells us difference
	for modified optimization

3. Dirtying at top of loop gives us a spectrum for
	xsaveopt64 with different amounts of state changed

4. Dirtying between save and restore tells us cost
	 of ext state use in vcore context (these tests
	 should be compared to baseline, as they will
	 use the init optimization)

 // TODO: also try dirtying ALL of the state, and see if that's any different
 //       from just getting each state component marked as dirty.
*/

// ------------------------------------------------------------
// We treat the ancillary state the same as Akaros:
// ------------------------------------------------------------
struct fp_header_non_64bit {
	uint16_t		fcw;
	uint16_t		fsw;
	uint8_t			ftw;
	uint8_t			padding0;
	uint16_t		fop;
	uint32_t		fpu_ip;
	uint16_t		cs;
	uint16_t		padding1;
	uint32_t		fpu_dp;
	uint16_t		ds;
	uint16_t		padding2;
	uint32_t		mxcsr;
	uint32_t		mxcsr_mask;
};

/* Header for the 64-bit mode FXSAVE map with promoted operand size */
struct fp_header_64bit_promoted {
	uint16_t		fcw;
	uint16_t		fsw;
	uint8_t			ftw;
	uint8_t			padding0;
	uint16_t		fop;
	uint64_t		fpu_ip;
	uint64_t		fpu_dp;
	uint32_t		mxcsr;
	uint32_t		mxcsr_mask;
};

/* Header for the 64-bit mode FXSAVE map with default operand size */
struct fp_header_64bit_default {
	uint16_t		fcw;
	uint16_t		fsw;
	uint8_t			ftw;
	uint8_t			padding0;
	uint16_t		fop;
	uint32_t		fpu_ip;
	uint16_t		cs;
	uint16_t		padding1;
	uint32_t		fpu_dp;
	uint16_t		ds;
	uint16_t		padding2;
	uint32_t		mxcsr;
	uint32_t		mxcsr_mask;
};

/* Just for storage space, not for real use	*/
typedef struct {
	unsigned int stor[4];
} __128bits;

/*
 *  X86_MAX_XCR0 specifies the maximum set of processor extended state
 *  feature components that Akaros supports saving through the
 *  XSAVE instructions.
 *  This may be a superset of available state components on a given
 *  processor. We CPUID at boot and determine the intersection
 *  of Akaros-supported and processor-supported features, and we
 *  save this value to __proc_global_info.x86_default_xcr0 in arch/x86/init.c.
 *  We guarantee that the set of feature components specified by
 *  X86_MAX_XCR0 will fit in the ancillary_state struct.
 *  If you add to the mask, make sure you also extend ancillary_state!
 */

#define X86_MAX_XCR0 0x2ff

typedef struct ancillary_state {
	/* Legacy region of the XSAVE area */
	union { /* whichever header used depends on the mode */
		struct fp_header_non_64bit			fp_head_n64;
		struct fp_header_64bit_promoted		fp_head_64p;
		struct fp_header_64bit_default		fp_head_64d;
	};
	/* offset 32 bytes */
	__128bits		st0_mm0;	/* 128 bits: 80 for the st0, 48 reserved */
	__128bits		st1_mm1;
	__128bits		st2_mm2;
	__128bits		st3_mm3;
	__128bits		st4_mm4;
	__128bits		st5_mm5;
	__128bits		st6_mm6;
	__128bits		st7_mm7;
	/* offset 160 bytes */
	__128bits		xmm0;
	__128bits		xmm1;
	__128bits		xmm2;
	__128bits		xmm3;
	__128bits		xmm4;
	__128bits		xmm5;
	__128bits		xmm6;
	__128bits		xmm7;
	/* xmm8-xmm15 are only available in 64-bit-mode */
	__128bits		xmm8;
	__128bits		xmm9;
	__128bits		xmm10;
	__128bits		xmm11;
	__128bits		xmm12;
	__128bits		xmm13;
	__128bits		xmm14;
	__128bits		xmm15;
	/* offset 416 bytes */
	__128bits		reserv0;
	__128bits		reserv1;
	__128bits		reserv2;
	__128bits		reserv3;
	__128bits		reserv4;
	__128bits		reserv5;
	/* offset 512 bytes */

	/*
	 * XSAVE header (64 bytes, starting at offset 512 from
	 * the XSAVE area's base address)
	 */

	// xstate_bv identifies the state components in the XSAVE area
	uint64_t		xstate_bv;
	/*
	 *	xcomp_bv[bit 63] is 1 if the compacted format is used, else 0.
	 *	All bits in xcomp_bv should be 0 if the processor does not support the
	 *	compaction extensions to the XSAVE feature set.
	*/
	uint64_t		xcomp_bv;
	__128bits		reserv6;

	/* offset 576 bytes */
	/*
	 *	Extended region of the XSAVE area
	 *	We currently support an extended region of up to 2112 bytes,
	 *	for a total ancillary_state size of 2688 bytes.
	 *	This supports x86 state components up through the zmm31 register.
	 *	If you need more, please ask!
	 *	See the Intel Architecture Instruction Set Extensions Programming
	 *	Reference page 3-3 for detailed offsets in this region.
	*/
	uint8_t			extended_region[2112];

	/* ancillary state  */
} __attribute__((aligned(64))) ancillary_state_t;
// ------------------------------------------------------------
// End Akaros-specific stuff
// ------------------------------------------------------------

#define XSAVE "xsave"
#define XSAVEOPT "xsaveopt"

// The ancillary state region used for all the tests
struct ancillary_state as;

uint32_t edx = 0x0;
uint32_t eax = 0x7;

char *mm0 = "|_MM:0_|";
char *xmm0  = "|____XMM:00____|";
char *hi_ymm1 = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0|_YMM_Hi128:01_|";


void dirty_x87()
{
	 asm volatile ("movq (%0), %%mm0" : /* No Outputs */ : "r" (mm0) : "%mm0");
}
void dirty_xmm()
{
	asm volatile ("movdqu (%0), %%xmm0" : /* No Outputs */ : "r" (xmm0) : "%xmm0");
}
void dirty_hi_ymm()
{
	// Is there any way to dirty just the high bits?
	// I have a feeling this probably marks both AVX and SSE components 1 in XINUSE
	// TODO
	asm volatile ("vmovdqu (%0), %%ymm1" : /* No Outputs */ : "r" (hi_ymm1) : "%xmm1");
}

void dirty_xmm_x87()
{
	dirty_xmm();
	dirty_x87();
}
void dirty_hi_ymm_xmm()
{
	dirty_hi_ymm();
	dirty_xmm();
}
void dirty_hi_ymm_x87()
{
	// TODO: Not sure if there's a way to only dirty the high ymms...
	dirty_hi_ymm();
	dirty_x87();
}
void dirty_hi_ymm_xmm_x87()
{
	dirty_hi_ymm();
	dirty_xmm();
	dirty_x87();
}

void zero_as(struct ancillary_state *as)
{
	memset(as, 0x0, sizeof(struct ancillary_state));
}

void fninit()
{
	asm volatile("fninit");
}

void xsave64(struct ancillary_state *as)
{
	asm volatile("xsave64 %0" : : "m"(*as), "a"(eax), "d"(edx));
}

void xsaveopt64(struct ancillary_state *as)
{
	asm volatile("xsaveopt64 %0" : : "m"(*as), "a"(eax), "d"(edx));
}

void xrstor64(struct ancillary_state *as)
{
	asm volatile("xrstor64 %0" : : "m"(*as));
}

uint64_t readTSC()
{
	uint32_t edx, eax;
	asm volatile ("rdtsc" : "=d"(edx), "=a"(eax));
	return ((uint64_t)edx << 32) | eax;
}


// int akaros_vprintf(const char *fmt, va_list ap)
// {
// 	debugbuf_t b;
// 	debugbuf_t *bp = &b;

// 	b.idx = 0;
// 	b.cnt = 0;
// 	akaros_vprintfmt((void*)putch, (void*)&bp, fmt, ap);
// 	sys_cputs(b.buf, b.idx);

// 	return b.cnt;
// }

// int akaros_printf(const char *format, ...)
// {
// 	va_list ap;
// 	int ret;

// 	va_start(ap, format);
// 	if (in_vcore_context())
// 		ret = akaros_vprintf(format, ap);
// 	else
// 		ret = vprintf(format, ap);
// 	va_end(ap);
// 	return ret;
// }

void print_intro()
{
	printf("The type of test is identified by a number:\n"
	       "1. Baseline tells us difference for init optimization\n"
	       "2. Dirtying outside the loop tells us difference\n"
	       "   for modified optimization\n"
	       "3. Dirtying at top of loop gives us a spectrum for\n"
	       "   xsaveopt64 with different amounts of state changed\n"
		   "4. Dirtying between save and restore helps estimate cost\n"
		   "   of ext state use in vcore context (these tests\n"
		   "   should be compared to baseline, as they will\n"
		   "   use the init optimization)\n"
		);
	printf("\nThe result format is: result[tabtab]test_name-test_number-tested_instr\n");
}


void print_results(char * name, int num, char * instr, double result)
{
	printf("%f\t\t%s-%d-%s\n", result, name, num, instr);
}

void tsc_test(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;
	uint64_t sum = 0;
	for (i = 0; i < n; ++i) {
		start = readTSC();
		end = readTSC();
		sum += (end - start);
	}

	/*
		Timing measurements come with at least a single readTSC call
		of overhead baked in. Assuming that you have the proper
		time in the registers as soon as the rdtsc instruction completes,
		after your "start" value is measured you have the cost of a bit
		shift and or, and then a pop to return. Your "end" value has the
		cost of a function call (readTSC) and the actual rdtsc instruction
		baked in. Thus end - start contains the cost of a readTSC call in
		addition to whatever you measured.
	*/
	print_results("tsc overhead", 0, "readTSC()", (double)sum/n);
	printf("You should subtract this from the rest of the timings\n");
	printf("to account for the overhead of the readTSC() function call.\n");
}

void baseline_tests(uint64_t n)
{

	// TODO: This is probably a bullshit measure. Does NOT seem to line up
	// with Agner Fog's tables, which I trust more than my measurements
	// for the time being.
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	fninit();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("baseline_xsave", 1, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("baseline_xsave", 1, "xrstor64", end - start);
	}


	zero_as(&as);
	fninit();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("baseline_xsaveopt", 1, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("baseline_xsaveopt", 1, "xrstor64", end - start);
	}
}

void x87_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	fninit();
	dirty_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("x87_xsave", 2, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("x87_xsave", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	fninit();
	dirty_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("x87_xsaveopt", 2, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("x87_xsaveopt", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_x87();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("x87_xsave", 3, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("x87_xsave", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_x87();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("x87_xsaveopt", 3, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("x87_xsaveopt", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("x87_xsave", 4, "xsave64", end - start);
		fninit();
		dirty_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("x87_xsave", 4, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("x87_xsaveopt", 4, "xsaveopt64", end - start);
		fninit();
		dirty_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("x87_xsaveopt", 4, "xrstor64", end - start);
	}
}

void xmm_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	fninit();
	dirty_xmm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("xmm_xsave", 2, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_xsave", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	fninit();
	dirty_xmm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("xmm_xsaveopt", 2, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_xsaveopt", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_xmm();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("xmm_xsave", 3, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_xsave", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_xmm();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("xmm_xsaveopt", 3, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_xsaveopt", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("xmm_xsave", 4, "xsave64", end - start);
		fninit();
		dirty_xmm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_xsave", 4, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("xmm_xsaveopt", 4, "xsaveopt64", end - start);
		fninit();
		dirty_xmm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_xsaveopt", 4, "xrstor64", end - start);
	}
}

void hi_ymm_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	fninit();
	dirty_hi_ymm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xsave", 2, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xsave", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	fninit();
	dirty_hi_ymm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xsaveopt", 2, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xsaveopt", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_hi_ymm();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xsave", 3, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xsave", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_hi_ymm();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xsaveopt", 3, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xsaveopt", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xsave", 4, "xsave64", end - start);
		fninit();
		dirty_hi_ymm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xsave", 4, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xsaveopt", 4, "xsaveopt64", end - start);
		fninit();
		dirty_hi_ymm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xsaveopt", 4, "xrstor64", end - start);
	}
}

void xmm_x87_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	fninit();
	dirty_xmm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("xmm_x87_xsave", 2, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_x87_xsave", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	fninit();
	dirty_xmm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("xmm_x87_xsaveopt", 2, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_x87_xsaveopt", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_xmm_x87();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("xmm_x87_xsave", 3, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_x87_xsave", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_xmm_x87();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("xmm_x87_xsaveopt", 3, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_x87_xsaveopt", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("xmm_x87_xsave", 4, "xsave64", end - start);
		fninit();
		dirty_xmm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_x87_xsave", 4, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("xmm_x87_xsaveopt", 4, "xsaveopt64", end - start);
		fninit();
		dirty_xmm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("xmm_x87_xsaveopt", 4, "xrstor64", end - start);
	}
}

void hi_ymm_xmm_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	fninit();
	dirty_hi_ymm_xmm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsave", 2, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsave", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	fninit();
	dirty_hi_ymm_xmm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsaveopt", 2, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsaveopt", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_hi_ymm_xmm();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsave", 3, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsave", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_hi_ymm_xmm();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsaveopt", 3, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsaveopt", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsave", 4, "xsave64", end - start);
		fninit();
		dirty_hi_ymm_xmm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsave", 4, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsaveopt", 4, "xsaveopt64", end - start);
		fninit();
		dirty_hi_ymm_xmm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_xsaveopt", 4, "xrstor64", end - start);
	}
}

void hi_ymm_x87_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	fninit();
	dirty_hi_ymm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsave", 2, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsave", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	fninit();
	dirty_hi_ymm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsaveopt", 2, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsaveopt", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_hi_ymm_x87();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsave", 3, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsave", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_hi_ymm_x87();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsaveopt", 3, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsaveopt", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsave", 4, "xsave64", end - start);
		fninit();
		dirty_hi_ymm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsave", 4, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsaveopt", 4, "xsaveopt64", end - start);
		fninit();
		dirty_hi_ymm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_x87_xsaveopt", 4, "xrstor64", end - start);
	}
}

void hi_ymm_xmm_x87_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	fninit();
	dirty_hi_ymm_xmm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsave", 2, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsave", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	fninit();
	dirty_hi_ymm_xmm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsaveopt", 2, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsaveopt", 2, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_hi_ymm_xmm_x87();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsave", 3, "xsave64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsave", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		fninit();
		dirty_hi_ymm_xmm_x87();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsaveopt", 3, "xsaveopt64", end - start);
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsaveopt", 3, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsave", 4, "xsave64", end - start);
		fninit();
		dirty_hi_ymm_xmm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsave", 4, "xrstor64", end - start);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsaveopt", 4, "xsaveopt64", end - start);
		fninit();
		dirty_hi_ymm_xmm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		print_results("hi_ymm_xmm_x87_xsaveopt", 4, "xrstor64", end - start);
	}
}



int main()
{

	int numiter = 100;

// TODO: According to Agner, Intel has a performance
	// counter called "core clock cycles", that is apparently
	// the most accurate measure... should take a look at this.
	// print_intro();
	// printf("\n");

	// tsc_test(numiter);
	// printf("\n");

	baseline_tests(numiter);
	// printf("\n");

	x87_tests(numiter);
	// printf("\n");

	xmm_tests(numiter);
	// printf("\n");

	// printf("PLEASE NOTE!: I'm not sure if my method here marks just YMM or both YMM and XMM as XINUSE!");
	hi_ymm_tests(numiter);
	// printf("\n");

	xmm_x87_tests(numiter);
	// printf("\n");

	hi_ymm_xmm_tests(numiter);
	// printf("\n");

	hi_ymm_x87_tests(numiter);
	// printf("\n");

	hi_ymm_xmm_x87_tests(numiter);
	// printf("\n");
}