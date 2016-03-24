
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>


/*

This version of the test program rearranges the ids of the tests
so that they get plotted in a different order.

The new order will be

baseline xsave64 test
xsave64 tests
baseline xsaveopt64 test
xsaveopt64 tests
baseline xrstor64 after xsave64 test
xrstor64 after xsasve64 tests
baseline xrstor64 after xsaveopt64 test
xrstor64 after xsaveopt64 tests

But still in the same order, under those categories, that the
tests occurred, so that the first test in "xsave64 tests"
corresponds to the first test in "xrstor64 tests that followed an xsasve64"
and so forth.
*/



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
struct ancillary_state default_as;

uint32_t edx = 0x0;
uint32_t eax = 0x7;

char *mm0 = "|_MM:0_|";
char *xmm0  = "|____XMM:00____|";
char *hi_ymm1 = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0|_YMM_Hi128:01_|";

char *mm1 = "|_MM:1_|";
char *mm2 = "|_MM:2_|";
char *mm3 = "|_MM:3_|";
char *mm4 = "|_MM:4_|";
char *mm5 = "|_MM:5_|";
char *mm6 = "|_MM:6_|";
char *mm7 = "|_MM:7_|";

// Each of these strings is 32 bytes long, excluding the terminating \0.
char *ymm0  = "|____XMM:00____||_YMM_Hi128:00_|";
char *ymm1  = "|____XMM:01____||_YMM_Hi128:01_|";
char *ymm2  = "|____XMM:02____||_YMM_Hi128:02_|";
char *ymm3  = "|____XMM:03____||_YMM_Hi128:03_|";
char *ymm4  = "|____XMM:04____||_YMM_Hi128:04_|";
char *ymm5  = "|____XMM:05____||_YMM_Hi128:05_|";
char *ymm6  = "|____XMM:06____||_YMM_Hi128:06_|";
char *ymm7  = "|____XMM:07____||_YMM_Hi128:07_|";
char *ymm8  = "|____XMM:08____||_YMM_Hi128:08_|";
char *ymm9  = "|____XMM:09____||_YMM_Hi128:09_|";
char *ymm10 = "|____XMM:10____||_YMM_Hi128:10_|";
char *ymm11 = "|____XMM:11____||_YMM_Hi128:11_|";
char *ymm12 = "|____XMM:12____||_YMM_Hi128:12_|";
char *ymm13 = "|____XMM:13____||_YMM_Hi128:13_|";
char *ymm14 = "|____XMM:14____||_YMM_Hi128:14_|";
char *ymm15 = "|____XMM:15____||_YMM_Hi128:15_|";


void dirty_all_data_reg() {

	asm volatile ("movq (%0), %%mm0" : /* No Outputs */ : "r" (mm0) : "%mm0");
	asm volatile ("movq (%0), %%mm1" : /* No Outputs */ : "r" (mm1) : "%mm1");
	asm volatile ("movq (%0), %%mm2" : /* No Outputs */ : "r" (mm2) : "%mm2");
	asm volatile ("movq (%0), %%mm3" : /* No Outputs */ : "r" (mm3) : "%mm3");
	asm volatile ("movq (%0), %%mm4" : /* No Outputs */ : "r" (mm4) : "%mm4");
	asm volatile ("movq (%0), %%mm5" : /* No Outputs */ : "r" (mm5) : "%mm5");
	asm volatile ("movq (%0), %%mm6" : /* No Outputs */ : "r" (mm6) : "%mm6");
	asm volatile ("movq (%0), %%mm7" : /* No Outputs */ : "r" (mm7) : "%mm7");

	asm volatile ("vmovdqu (%0), %%ymm0" : /* No Outputs */ : "r" (ymm0) : "%xmm0");
	asm volatile ("vmovdqu (%0), %%ymm1" : /* No Outputs */ : "r" (ymm1) : "%xmm1");
	asm volatile ("vmovdqu (%0), %%ymm2" : /* No Outputs */ : "r" (ymm2) : "%xmm2");
	asm volatile ("vmovdqu (%0), %%ymm3" : /* No Outputs */ : "r" (ymm3) : "%xmm3");
	asm volatile ("vmovdqu (%0), %%ymm4" : /* No Outputs */ : "r" (ymm4) : "%xmm4");
	asm volatile ("vmovdqu (%0), %%ymm5" : /* No Outputs */ : "r" (ymm5) : "%xmm5");
	asm volatile ("vmovdqu (%0), %%ymm6" : /* No Outputs */ : "r" (ymm6) : "%xmm6");
	asm volatile ("vmovdqu (%0), %%ymm7" : /* No Outputs */ : "r" (ymm7) : "%xmm7");

	asm volatile ("vmovdqu (%0), %%ymm8"  : /* No Outputs */ : "r" (ymm8)  : "%xmm8");
	asm volatile ("vmovdqu (%0), %%ymm9"  : /* No Outputs */ : "r" (ymm9)  : "%xmm9");
	asm volatile ("vmovdqu (%0), %%ymm10" : /* No Outputs */ : "r" (ymm10) : "%xmm10");
	asm volatile ("vmovdqu (%0), %%ymm11" : /* No Outputs */ : "r" (ymm11) : "%xmm11");
	asm volatile ("vmovdqu (%0), %%ymm12" : /* No Outputs */ : "r" (ymm12) : "%xmm12");
	asm volatile ("vmovdqu (%0), %%ymm13" : /* No Outputs */ : "r" (ymm13) : "%xmm13");
	asm volatile ("vmovdqu (%0), %%ymm14" : /* No Outputs */ : "r" (ymm14) : "%xmm14");
	asm volatile ("vmovdqu (%0), %%ymm15" : /* No Outputs */ : "r" (ymm15) : "%xmm15");

}

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

void reset_fp()
{
	//asm volatile("fninit");
	xrstor64(&default_as);
}

uint64_t readTSC()
{
	uint32_t edx, eax;
	asm volatile ("rdtsc" : "=d"(edx), "=a"(eax));
	return ((uint64_t)edx << 32) | eax;
}


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


void print_results(char * name, int num, char * instr, int id, double result)
{
	printf("%s-%d-%s\t%d\t%f\n", name, num, instr, id, result);
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
	print_results("tsc overhead", 0, "readTSC()", 0, (double)sum/n);
	printf("You should subtract this from the rest of the timings\n");
	printf("to account for the overhead of the readTSC() function call.\n");
}


uint64_t *save_res;
uint64_t *rstor_res;


void baseline_tests(uint64_t n)
{

	// TODO: This is probably a bullshit measure. Does NOT seem to line up
	// with Agner Fog's tables, which I trust more than my measurements
	// for the time being.
	uint64_t i;
	uint64_t start;
	uint64_t end;


	zero_as(&as);
	reset_fp();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("baseline_xsave", 1, "xsave64", 1, save_res[i]);
		print_results("baseline_xsave", 1, "xrstor64", 51, rstor_res[i]);
	}


	zero_as(&as);
	reset_fp();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("baseline_xsaveopt", 1, "xsaveopt64", 26, save_res[i]);
		print_results("baseline_xsaveopt", 1, "xrstor64", 76, rstor_res[i]);
	}
}

void x87_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	reset_fp();
	dirty_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("x87_xsave", 2, "xsave64", 2, save_res[i]);
		print_results("x87_xsave", 2, "xrstor64", 52, rstor_res[i]);
	}

	zero_as(&as);
	reset_fp();
	dirty_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("x87_xsaveopt", 2, "xsaveopt64", 27, save_res[i]);
		print_results("x87_xsaveopt", 2, "xrstor64", 77, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_x87();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("x87_xsave", 3, "xsave64", 3, save_res[i]);
		print_results("x87_xsave", 3, "xrstor64", 53, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_x87();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("x87_xsaveopt", 3, "xsaveopt64", 28, save_res[i]);
		print_results("x87_xsaveopt", 3, "xrstor64", 78, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("x87_xsave", 4, "xsave64", 4, save_res[i]);
		print_results("x87_xsave", 4, "xrstor64", 54, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("x87_xsaveopt", 4, "xsaveopt64", 29, save_res[i]);
		print_results("x87_xsaveopt", 4, "xrstor64", 79, rstor_res[i]);
	}
}

void xmm_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	reset_fp();
	dirty_xmm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_xsave", 2, "xsave64", 5, save_res[i]);
		print_results("xmm_xsave", 2, "xrstor64", 55, rstor_res[i]);
	}

	zero_as(&as);
	reset_fp();
	dirty_xmm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_xsaveopt", 2, "xsaveopt64", 30, save_res[i]);
		print_results("xmm_xsaveopt", 2, "xrstor64", 80, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_xmm();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_xsave", 3, "xsave64", 6, save_res[i]);
		print_results("xmm_xsave", 3, "xrstor64", 56, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_xmm();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_xsaveopt", 3, "xsaveopt64", 31, save_res[i]);
		print_results("xmm_xsaveopt", 3, "xrstor64", 81, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_xmm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_xsave", 4, "xsave64", 7, save_res[i]);
		print_results("xmm_xsave", 4, "xrstor64", 57, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_xmm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_xsaveopt", 4, "xsaveopt64", 32, save_res[i]);
		print_results("xmm_xsaveopt", 4, "xrstor64", 82, rstor_res[i]);
	}
}

void hi_ymm_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	reset_fp();
	dirty_hi_ymm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xsave", 2, "xsave64", 8, save_res[i]);
		print_results("hi_ymm_xsave", 2, "xrstor64", 58, rstor_res[i]);
	}

	zero_as(&as);
	reset_fp();
	dirty_hi_ymm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xsaveopt", 2, "xsaveopt64", 33, save_res[i]);
		print_results("hi_ymm_xsaveopt", 2, "xrstor64", 83, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_hi_ymm();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xsave", 3, "xsave64", 9, save_res[i]);
		print_results("hi_ymm_xsave", 3, "xrstor64", 59, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_hi_ymm();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xsaveopt", 3, "xsaveopt64", 34, save_res[i]);
		print_results("hi_ymm_xsaveopt", 3, "xrstor64", 84, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_hi_ymm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xsave", 4, "xsave64", 10, save_res[i]);
		print_results("hi_ymm_xsave", 4, "xrstor64", 60, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_hi_ymm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xsaveopt", 4, "xsaveopt64", 35, save_res[i]);
		print_results("hi_ymm_xsaveopt", 4, "xrstor64", 85, rstor_res[i]);
	}
}

void xmm_x87_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	reset_fp();
	dirty_xmm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_x87_xsave", 2, "xsave64", 11, save_res[i]);
		print_results("xmm_x87_xsave", 2, "xrstor64", 61, rstor_res[i]);
	}

	zero_as(&as);
	reset_fp();
	dirty_xmm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_x87_xsaveopt", 2, "xsaveopt64", 36, save_res[i]);
		print_results("xmm_x87_xsaveopt", 2, "xrstor64", 86, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_xmm_x87();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_x87_xsave", 3, "xsave64", 12, save_res[i]);
		print_results("xmm_x87_xsave", 3, "xrstor64", 62, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_xmm_x87();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_x87_xsaveopt", 3, "xsaveopt64", 37, save_res[i]);
		print_results("xmm_x87_xsaveopt", 3, "xrstor64", 87, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_xmm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_x87_xsave", 4, "xsave64", 13, save_res[i]);
		print_results("xmm_x87_xsave", 4, "xrstor64", 63, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_xmm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("xmm_x87_xsaveopt", 4, "xsaveopt64", 38, save_res[i]);
		print_results("xmm_x87_xsaveopt", 4, "xrstor64", 88, rstor_res[i]);
	}
}

void hi_ymm_xmm_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	reset_fp();
	dirty_hi_ymm_xmm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_xsave", 2, "xsave64", 14, save_res[i]);
		print_results("hi_ymm_xmm_xsave", 2, "xrstor64", 64, rstor_res[i]);
	}

	zero_as(&as);
	reset_fp();
	dirty_hi_ymm_xmm();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_xsaveopt", 2, "xsaveopt64", 39, save_res[i]);
		print_results("hi_ymm_xmm_xsaveopt", 2, "xrstor64", 89, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_hi_ymm_xmm();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_xsave", 3, "xsave64", 15, save_res[i]);
		print_results("hi_ymm_xmm_xsave", 3, "xrstor64", 65, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_hi_ymm_xmm();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_xsaveopt", 3, "xsaveopt64", 40, save_res[i]);
		print_results("hi_ymm_xmm_xsaveopt", 3, "xrstor64", 90, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_hi_ymm_xmm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_xsave", 4, "xsave64", 16, save_res[i]);
		print_results("hi_ymm_xmm_xsave", 4, "xrstor64", 66, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_hi_ymm_xmm();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_xsaveopt", 4, "xsaveopt64", 41, save_res[i]);
		print_results("hi_ymm_xmm_xsaveopt", 4, "xrstor64", 91, rstor_res[i]);
	}
}

void hi_ymm_x87_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	reset_fp();
	dirty_hi_ymm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_x87_xsave", 2, "xsave64", 17, save_res[i]);
		print_results("hi_ymm_x87_xsave", 2, "xrstor64", 67, rstor_res[i]);
	}

	zero_as(&as);
	reset_fp();
	dirty_hi_ymm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_x87_xsaveopt", 2, "xsaveopt64", 42, save_res[i]);
		print_results("hi_ymm_x87_xsaveopt", 2, "xrstor64", 92, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_hi_ymm_x87();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_x87_xsave", 3, "xsave64", 18, save_res[i]);
		print_results("hi_ymm_x87_xsave", 3, "xrstor64", 68, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_hi_ymm_x87();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_x87_xsaveopt", 3, "xsaveopt64", 43, save_res[i]);
		print_results("hi_ymm_x87_xsaveopt", 3, "xrstor64", 93, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_hi_ymm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_x87_xsave", 4, "xsave64", 19, save_res[i]);
		print_results("hi_ymm_x87_xsave", 4, "xrstor64", 69, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_hi_ymm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_x87_xsaveopt", 4, "xsaveopt64", 44, save_res[i]);
		print_results("hi_ymm_x87_xsaveopt", 4, "xrstor64", 94, rstor_res[i]);
	}
}

void hi_ymm_xmm_x87_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	reset_fp();
	dirty_hi_ymm_xmm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_x87_xsave", 2, "xsave64", 20, save_res[i]);
		print_results("hi_ymm_xmm_x87_xsave", 2, "xrstor64", 70, rstor_res[i]);
	}

	zero_as(&as);
	reset_fp();
	dirty_hi_ymm_xmm_x87();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_x87_xsaveopt", 2, "xsaveopt64", 45, save_res[i]);
		print_results("hi_ymm_xmm_x87_xsaveopt", 2, "xrstor64", 95, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_hi_ymm_xmm_x87();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_x87_xsave", 3, "xsave64", 21, save_res[i]);
		print_results("hi_ymm_xmm_x87_xsave", 3, "xrstor64", 71, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_hi_ymm_xmm_x87();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_x87_xsaveopt", 3, "xsaveopt64", 46, save_res[i]);
		print_results("hi_ymm_xmm_x87_xsaveopt", 3, "xrstor64", 96, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_hi_ymm_xmm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_x87_xsave", 4, "xsave64", 22, save_res[i]);
		print_results("hi_ymm_xmm_x87_xsave", 4, "xrstor64", 72, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_hi_ymm_xmm_x87();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("hi_ymm_xmm_x87_xsaveopt", 4, "xsaveopt64", 47, save_res[i]);
		print_results("hi_ymm_xmm_x87_xsaveopt", 4, "xrstor64", 97, rstor_res[i]);
	}
}

void all_data_reg_tests(uint64_t n)
{
	uint64_t i;
	uint64_t start;
	uint64_t end;

	zero_as(&as);
	reset_fp();
	dirty_all_data_reg();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("all_data_reg_xsave", 2, "xsave64", 23, save_res[i]);
		print_results("all_data_reg_xsave", 2, "xrstor64", 73, rstor_res[i]);
	}

	zero_as(&as);
	reset_fp();
	dirty_all_data_reg();
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("all_data_reg_xsaveopt", 2, "xsaveopt64", 48, save_res[i]);
		print_results("all_data_reg_xsaveopt", 2, "xrstor64", 98, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_all_data_reg();
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("all_data_reg_xsave", 3, "xsave64", 24, save_res[i]);
		print_results("all_data_reg_xsave", 3, "xrstor64", 74, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		reset_fp();
		dirty_all_data_reg();
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("all_data_reg_xsaveopt", 3, "xsaveopt64", 49, save_res[i]);
		print_results("all_data_reg_xsaveopt", 3, "xrstor64", 99, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsave64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_all_data_reg();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("all_data_reg_xsave", 4, "xsave64", 25, save_res[i]);
		print_results("all_data_reg_xsave", 4, "xrstor64", 75, rstor_res[i]);
	}


	zero_as(&as);
	for (i = 0; i < n; ++i) {
		start = readTSC();
		xsaveopt64(&as);
		end = readTSC();
		save_res[i] = end - start;
		reset_fp();
		dirty_all_data_reg();
		start = readTSC();
		xrstor64(&as);
		end = readTSC();
		rstor_res[i] = end - start;
	}
	for (i = 0; i < n; ++i) {
		print_results("all_data_reg_xsaveopt", 4, "xsaveopt64", 50, save_res[i]);
		print_results("all_data_reg_xsaveopt", 4, "xrstor64", 100, rstor_res[i]);
	}
}

int main()
{

	int numiter = 1000000;

	save_res = malloc(numiter * sizeof(uint64_t));
	rstor_res = malloc(numiter * sizeof(uint64_t));

	// Set up a default extended state that we can use for resets
	memset(&default_as, 0x00, sizeof(struct ancillary_state));
	asm volatile ("fninit");
	edx = 0x0;
	eax = 0x1;
	asm volatile("xsave64 %0" : : "m"(default_as), "a"(eax), "d"(edx));
	default_as.fp_head_64d.mxcsr = 0x1f80;
	eax = 0x7; // Set eax back to state components up to AVX


// TODO: According to Agner, Intel has a performance
	// counter called "core clock cycles", that is apparently
	// the most accurate measure... should take a look at this.
	baseline_tests(numiter);
	x87_tests(numiter);
	xmm_tests(numiter);
	// printf("PLEASE NOTE!: I'm not sure if my method here marks just YMM or both YMM and XMM as XINUSE!");
	hi_ymm_tests(numiter);
	xmm_x87_tests(numiter);
	hi_ymm_xmm_tests(numiter);
	hi_ymm_x87_tests(numiter);
	hi_ymm_xmm_x87_tests(numiter);
	all_data_reg_tests(numiter);

	free(save_res);
	free(rstor_res);
}