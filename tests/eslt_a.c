/*
    Reads xcr0, sets the fpu state and the processor extended state components
    to predetermined values (where possible), then reads the state components
    to see if the values are still there.

    The purpose of this test is to detect if either:
     (a) some of the extended state fails to save when a process is suspended
     or
     (b) the extended state is corrupted somewhere between saving and
         restoring the state

    We run this test inside a Linux VM hosted by Akaros.

    If this test fails you need to check three things:
      1) That nothing in the test program clobbers fpu or extended
         state registers.
         To check this, statically link the test and check the objdump for the
         portion of the state component that caused the failure. Make sure
         the only write to that component is the one we perform in set_values.
         For example, there is an implementation of string.h's memcmp,
         used on systems that have SSE, that clobbers a data register.
      2) That Akaros' save and restore of fpu and extended state is working.
      3) That Linux's save and restore of fpu and extended state is working.
         It is somewhat unlikely that this third condition will be the issue.
 */


// TODO: These tests need to be updated as Intel adds more extended state components.

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <parlib/uthread.h>
#include <pthread.h>


#define X87        (1 << 0)
#define SSE        (1 << 1)
#define AVX        (1 << 2)
#define BNDREG     (1 << 3)
#define BNDCSR     (1 << 4)
#define OPMASK     (1 << 5)
#define ZMM_HI256  (1 << 6)
#define HI16_ZMM   (1 << 7)
#define PKRU       (1 << 9)

// ------------------------------------------------------------------------
// FPU and extended state source values
// ------------------------------------------------------------------------


// Data registers (MMX registers are aliased to STX registers)
// This half of the fp_test treats them as MMX, the other half as STX
// Each of these strings is 8 bytes long (excluding terminating \0).
// You'll see some extra room in the hex_dump, that's partially for
// exponents because these data registers are aliased to the lower
// 64 bits of the ST0-ST7 floating point registers, and partially
// reserved.
char *mm0 = "|_MM:0_|";
char *mm1 = "|_MM:1_|";
char *mm2 = "|_MM:2_|";
char *mm3 = "|_MM:3_|";
char *mm4 = "|_MM:4_|";
char *mm5 = "|_MM:5_|";
char *mm6 = "|_MM:6_|";
char *mm7 = "|_MM:7_|";

/* SSE: MXCSR and YMM/XMM registers */
// MXCSR is 32 bits, but the high 16 are reserved
// so we only set the low 16.
char *mxcsr = "MX\0\0";

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

// ------------------------------------------------------------------------
// Most recent values in fpu and extended state registers
// ------------------------------------------------------------------------

// Most recent values. Will populate this with current extended
// state and then compare to source vars.

char *mm0_cur;
char *mm1_cur;
char *mm2_cur;
char *mm3_cur;
char *mm4_cur;
char *mm5_cur;
char *mm6_cur;
char *mm7_cur;

char *mxcsr_cur;

char *xmm0_cur;
char *xmm1_cur;
char *xmm2_cur;
char *xmm3_cur;
char *xmm4_cur;
char *xmm5_cur;
char *xmm6_cur;
char *xmm7_cur;
char *xmm8_cur;
char *xmm9_cur;
char *xmm10_cur;
char *xmm11_cur;
char *xmm12_cur;
char *xmm13_cur;
char *xmm14_cur;
char *xmm15_cur;

char *ymm0_cur;
char *ymm1_cur;
char *ymm2_cur;
char *ymm3_cur;
char *ymm4_cur;
char *ymm5_cur;
char *ymm6_cur;
char *ymm7_cur;
char *ymm8_cur;
char *ymm9_cur;
char *ymm10_cur;
char *ymm11_cur;
char *ymm12_cur;
char *ymm13_cur;
char *ymm14_cur;
char *ymm15_cur;

// -------------------------------------------------------------
// Main program:
// -------------------------------------------------------------

static inline unsigned int min(unsigned int a, unsigned int b) {
  if (a < b) {
    return a;
  }
  return b;
}

// Rudimentary hex dumper. Misses some corner cases on
// certain ascii values but good enough for our purposes.
void hex_dump(void *mem, size_t size) {
  // Prints 16 byte lines as space-separated hex pairs
  int i = size;
  int line_i = 0;
  unsigned char *next = mem;
  unsigned int print_ascii = 0;
  unsigned int line_len = min(16, size);

  while(i) {


    if (print_ascii) {
      if ('\a' == *next)      { printf("\\a"); }
      else if ('\b' == *next) { printf("\\b"); }
      else if ('\f' == *next) { printf("\\f"); }
      else if ('\n' == *next) { printf("\\n"); }
      else if ('\r' == *next) { printf("\\r"); }
      else if ('\t' == *next) { printf("\\t"); }
      else if ('\v' == *next) { printf("\\v"); }
      else if ('\\' == *next) { printf("\\ "); }
      else if ('\'' == *next) { printf("\' "); }
      else if ('\"' == *next) { printf("\" "); }
      else if ('\?' == *next) { printf("\? "); }
      else { printf("%c ", *next); }
    }
    else {
      // Print two bytes and a space
      if (0x00 == *next) { printf("-- "); }
      else               { printf("%02x ", *next); }
    }
    // Manipulate counters
    i--;
    line_i++;
    next +=1;

    if (line_len == line_i) { // we just printed the end of a line
      line_i = 0;
      if (print_ascii) { // we just printed the last ascii char of a line
        print_ascii = 0;
        printf("\n");
      }
      else { // we just printed the last hex byte of a line
        print_ascii = 1;
        // now we're going to print the line again, but in ascii
        next -= line_len;
        i += line_len;
      }
    }
  }

  printf("\n");

}

// We implement our own mem_cmp to ensure that it does not clobber
// the fpu or extended state data registers. This returns nonzero
// if the two memory blocks are not equal, but does NOT indicate
// ordering. a != b always implies return 1.
static int mem_cmp (const void *_a, const void *_b, size_t n) {
  char *a = (char*)_a;
  char *b = (char*)_b;
  while(n--) {
    if (*a != *b) return 1;
    a++;
    b++;
  }
  return 0;
}

void fail(char *s, char *cur, char *src, size_t size) {
  printf("Test A failure! Failed at state component: %s\n", s);
  printf("cur val:\n"); hex_dump(cur, size);
  printf("src val:\n"); hex_dump(src, size);
  exit(0);
}

void xgetbv_ecx0(uint32_t *edx, uint32_t *eax) {
  __asm__ ("mov $0, %%ecx\n\t"
           "xgetbv\n\t"
           : "=d" (*edx), "=a" (*eax)
           : /* No inputs */
           : "%ecx" );
}

// Sets the extended state components enabled in xcr0
void set_values(uint64_t xcr0) {
  /* X87: x87 FPU, x87 state is always enabled in xcr0 */

  // FNINIT sets the following
  // them from saving
  // FPU Control Word: 0x037f
  // FPU Status Word: 0
  // FPU Tag Word: 0xffff
  // FPU Data Pointer: 0
  // FPU Instruction Pointer: 0
  // FPU Last Instruction Opcode: 0
  __asm__ __volatile__ ("fninit");

  // Bits you can set in the fp header, load/save:
  // FPU Control Word: 12:8 and 5:0, fldcw/fnstcw
  // FPU Status Word: 15:0, none/fnstsw
  // FPU Tag Word: Software cannot directly load or modify the tags in the tag register.
  //               and must use xsave and check xsave area to read.
  // FPU Data Pointer: Have to xrstor/xsave to modify/read
  // FPU Instruction Pointer: Have to xrstor/xsave to modify/read
  // FPU Last Instruction Opcode: Have to xrstor/xsave to modify/read

  // NOTE: This half of the fp_test just uses fninit, the other half
  //       sets custom values in the fpu.

  __asm__("movq (%0), %%mm0" : /* No Outputs */ : "r" (mm0) : "%mm0");
  __asm__("movq (%0), %%mm1" : /* No Outputs */ : "r" (mm1) : "%mm1");
  __asm__("movq (%0), %%mm2" : /* No Outputs */ : "r" (mm2) : "%mm2");
  __asm__("movq (%0), %%mm3" : /* No Outputs */ : "r" (mm3) : "%mm3");
  __asm__("movq (%0), %%mm4" : /* No Outputs */ : "r" (mm4) : "%mm4");
  __asm__("movq (%0), %%mm5" : /* No Outputs */ : "r" (mm5) : "%mm5");
  __asm__("movq (%0), %%mm6" : /* No Outputs */ : "r" (mm6) : "%mm6");
  __asm__("movq (%0), %%mm7" : /* No Outputs */ : "r" (mm7) : "%mm7");




  // Populate SSE:
  if (xcr0 & SSE) {
    // If SSE, load MXCSR and just the XMM portions of the data registers
    __asm__("ldmxcsr (%0)" : /* No Outputs */ : "r" (mxcsr));

    // Populate XMMs (15 regs on 64 bit) with movdqu (note that vmovdqu is AVX, not SSE)
    __asm__("movdqu (%0), %%xmm0" : /* No Outputs */ : "r" (ymm0) : "%xmm0");
    __asm__("movdqu (%0), %%xmm1" : /* No Outputs */ : "r" (ymm1) : "%xmm1");
    __asm__("movdqu (%0), %%xmm2" : /* No Outputs */ : "r" (ymm2) : "%xmm2");
    __asm__("movdqu (%0), %%xmm3" : /* No Outputs */ : "r" (ymm3) : "%xmm3");
    __asm__("movdqu (%0), %%xmm4" : /* No Outputs */ : "r" (ymm4) : "%xmm4");
    __asm__("movdqu (%0), %%xmm5" : /* No Outputs */ : "r" (ymm5) : "%xmm5");
    __asm__("movdqu (%0), %%xmm6" : /* No Outputs */ : "r" (ymm6) : "%xmm6");
    __asm__("movdqu (%0), %%xmm7" : /* No Outputs */ : "r" (ymm7) : "%xmm7");

    __asm__("movdqu (%0), %%xmm8"  : /* No Outputs */ : "r" (ymm8)  : "%xmm8");
    __asm__("movdqu (%0), %%xmm9"  : /* No Outputs */ : "r" (ymm9)  : "%xmm9");
    __asm__("movdqu (%0), %%xmm10" : /* No Outputs */ : "r" (ymm10) : "%xmm10");
    __asm__("movdqu (%0), %%xmm11" : /* No Outputs */ : "r" (ymm11) : "%xmm11");
    __asm__("movdqu (%0), %%xmm12" : /* No Outputs */ : "r" (ymm12) : "%xmm12");
    __asm__("movdqu (%0), %%xmm13" : /* No Outputs */ : "r" (ymm13) : "%xmm13");
    __asm__("movdqu (%0), %%xmm14" : /* No Outputs */ : "r" (ymm14) : "%xmm14");
    __asm__("movdqu (%0), %%xmm15" : /* No Outputs */ : "r" (ymm15) : "%xmm15");
  }

  // Populate AVX:
  if (xcr0 & AVX) {
    // Populate YMMs (15 regs on 64 bit) with AVX version of movdqu (vmovdqu)
    // These instructions are AVX 256 bit encoded because we target the ymm
    // registers
    // Unfortunately, gcc doesn't recognize ymm as a register for clobbers,
    // so we have to just stick with xmm there.
    __asm__("vmovdqu (%0), %%ymm0" : /* No Outputs */ : "r" (ymm0) : "%xmm0");
    __asm__("vmovdqu (%0), %%ymm1" : /* No Outputs */ : "r" (ymm1) : "%xmm1");
    __asm__("vmovdqu (%0), %%ymm2" : /* No Outputs */ : "r" (ymm2) : "%xmm2");
    __asm__("vmovdqu (%0), %%ymm3" : /* No Outputs */ : "r" (ymm3) : "%xmm3");
    __asm__("vmovdqu (%0), %%ymm4" : /* No Outputs */ : "r" (ymm4) : "%xmm4");
    __asm__("vmovdqu (%0), %%ymm5" : /* No Outputs */ : "r" (ymm5) : "%xmm5");
    __asm__("vmovdqu (%0), %%ymm6" : /* No Outputs */ : "r" (ymm6) : "%xmm6");
    __asm__("vmovdqu (%0), %%ymm7" : /* No Outputs */ : "r" (ymm7) : "%xmm7");

    __asm__("vmovdqu (%0), %%ymm8"  : /* No Outputs */ : "r" (ymm8)  : "%xmm8");
    __asm__("vmovdqu (%0), %%ymm9"  : /* No Outputs */ : "r" (ymm9)  : "%xmm9");
    __asm__("vmovdqu (%0), %%ymm10" : /* No Outputs */ : "r" (ymm10) : "%xmm10");
    __asm__("vmovdqu (%0), %%ymm11" : /* No Outputs */ : "r" (ymm11) : "%xmm11");
    __asm__("vmovdqu (%0), %%ymm12" : /* No Outputs */ : "r" (ymm12) : "%xmm12");
    __asm__("vmovdqu (%0), %%ymm13" : /* No Outputs */ : "r" (ymm13) : "%xmm13");
    __asm__("vmovdqu (%0), %%ymm14" : /* No Outputs */ : "r" (ymm14) : "%xmm14");
    __asm__("vmovdqu (%0), %%ymm15" : /* No Outputs */ : "r" (ymm15) : "%xmm15");
  }

// TODO: My workstation doesn't have the below state components,
  //     and some of them don't even exist in hardware you can
  //     buy yet, so there isn't really an easy way to verify
  //     that this test is operating correctly for those
  //     (e.g. we can't hex_dump what we don't have)

  // Populate BNDREG
  if (xcr0 & BNDREG) {
    // TODO
    // Theoretically you could use the BNDMOV instruction for this
    // though it seems like it can only address memory and bnd1 and bnd2,
    // leaving out bnd0 and bnd3... as per Intel manual.
  }

  // Populate BNDCSR
  if (xcr0 & BNDCSR) {
    // TODO
  }

  // Populate OPMASK
  if (xcr0 & OPMASK) {
    // TODO
  }

  // Populate ZMM_HI_256
  if (xcr0 & ZMM_HI256) {
    // TODO
  }

  // Populate HI16_ZMM
  if (xcr0 & HI16_ZMM) {
    // TODO
  }

  // Populate PKRU
  if (xcr0 & PKRU) {
    // TODO
  }

}



void check_values(uint64_t xcr0) {

  __asm__("movq %%mm0, (%0)" : /* No Outputs */ : "r" (mm0_cur) : "memory");
  if (mem_cmp(mm0_cur, mm0, 8)) fail("x87, mm0", mm0_cur, mm0, 8);
  __asm__("movq %%mm1, (%0)" : /* No Outputs */ : "r" (mm1_cur) : "memory");
  if (mem_cmp(mm1_cur, mm1, 8)) fail("x87, mm1", mm1_cur, mm1, 8);
  __asm__("movq %%mm2, (%0)" : /* No Outputs */ : "r" (mm2_cur) : "memory");
  if (mem_cmp(mm2_cur, mm2, 8)) fail("x87, mm2", mm2_cur, mm2, 8);
  __asm__("movq %%mm3, (%0)" : /* No Outputs */ : "r" (mm3_cur) : "memory");
  if (mem_cmp(mm3_cur, mm3, 8)) fail("x87, mm3", mm3_cur, mm3, 8);
  __asm__("movq %%mm4, (%0)" : /* No Outputs */ : "r" (mm4_cur) : "memory");
  if (mem_cmp(mm4_cur, mm4, 8)) fail("x87, mm4", mm4_cur, mm4, 8);
  __asm__("movq %%mm5, (%0)" : /* No Outputs */ : "r" (mm5_cur) : "memory");
  if (mem_cmp(mm5_cur, mm5, 8)) fail("x87, mm5", mm5_cur, mm5, 8);
  __asm__("movq %%mm6, (%0)" : /* No Outputs */ : "r" (mm6_cur) : "memory");
  if (mem_cmp(mm6_cur, mm6, 8)) fail("x87, mm6", mm6_cur, mm6, 8);
  __asm__("movq %%mm7, (%0)" : /* No Outputs */ : "r" (mm7_cur) : "memory");
  if (mem_cmp(mm7_cur, mm7, 8)) fail("x87, mm7", mm7_cur, mm7, 8);

  // Check SSE:
  if (xcr0 & SSE) {
    // If SSE, check MXCSR and just the XMM portions of the data registers
    __asm__("stmxcsr (%0)" : /* No Outputs */ : "r" (mxcsr_cur) : "memory");
    if (mem_cmp(mxcsr_cur, mxcsr, 4)) fail("SSE, MXCSR", mxcsr_cur, mxcsr, 4);

    __asm__("movdqu %%xmm0, (%0)" : /* No Outputs */ : "r" (xmm0_cur) : "memory");
    if (mem_cmp(xmm0_cur, ymm0, 16)) fail("SSE, xmm0", xmm0_cur, ymm0, 16);
    __asm__("movdqu %%xmm1, (%0)" : /* No Outputs */ : "r" (xmm1_cur) : "memory");
    if (mem_cmp(xmm1_cur, ymm1, 16)) fail("SSE, xmm1", xmm1_cur, ymm1, 16);
    __asm__("movdqu %%xmm2, (%0)" : /* No Outputs */ : "r" (xmm2_cur) : "memory");
    if (mem_cmp(xmm2_cur, ymm2, 16)) fail("SSE, xmm2", xmm2_cur, ymm2, 16);
    __asm__("movdqu %%xmm3, (%0)" : /* No Outputs */ : "r" (xmm3_cur) : "memory");
    if (mem_cmp(xmm3_cur, ymm3, 16)) fail("SSE, xmm3", xmm3_cur, ymm3, 16);
    __asm__("movdqu %%xmm4, (%0)" : /* No Outputs */ : "r" (xmm4_cur) : "memory");
    if (mem_cmp(xmm4_cur, ymm4, 16)) fail("SSE, xmm4", xmm4_cur, ymm4, 16);
    __asm__("movdqu %%xmm5, (%0)" : /* No Outputs */ : "r" (xmm5_cur) : "memory");
    if (mem_cmp(xmm5_cur, ymm5, 16)) fail("SSE, xmm5", xmm5_cur, ymm5, 16);
    __asm__("movdqu %%xmm6, (%0)" : /* No Outputs */ : "r" (xmm6_cur) : "memory");
    if (mem_cmp(xmm6_cur, ymm6, 16)) fail("SSE, xmm6", xmm6_cur, ymm6, 16);
    __asm__("movdqu %%xmm7, (%0)" : /* No Outputs */ : "r" (xmm7_cur) : "memory");
    if (mem_cmp(xmm7_cur, ymm7, 16)) fail("SSE, xmm7", xmm7_cur, ymm7, 16);

    __asm__("movdqu %%xmm8, (%0)"  : /* No Outputs */ : "r" (xmm8_cur)  : "memory");
    if (mem_cmp(xmm8_cur, ymm8, 16)) fail("SSE, xmm8", xmm8_cur, ymm8, 16);
    __asm__("movdqu %%xmm9, (%0)"  : /* No Outputs */ : "r" (xmm9_cur)  : "memory");
    if (mem_cmp(xmm9_cur, ymm9, 16)) fail("SSE, xmm9", xmm9_cur, ymm9, 16);
    __asm__("movdqu %%xmm10, (%0)" : /* No Outputs */ : "r" (xmm10_cur) : "memory");
    if (mem_cmp(xmm10_cur, ymm10, 16)) fail("SSE, xmm10", xmm10_cur, ymm10, 16);
    __asm__("movdqu %%xmm11, (%0)" : /* No Outputs */ : "r" (xmm11_cur) : "memory");
    if (mem_cmp(xmm11_cur, ymm11, 16)) fail("SSE, xmm11", xmm11_cur, ymm11, 16);
    __asm__("movdqu %%xmm12, (%0)" : /* No Outputs */ : "r" (xmm12_cur) : "memory");
    if (mem_cmp(xmm12_cur, ymm12, 16)) fail("SSE, xmm12", xmm12_cur, ymm12, 16);
    __asm__("movdqu %%xmm13, (%0)" : /* No Outputs */ : "r" (xmm13_cur) : "memory");
    if (mem_cmp(xmm13_cur, ymm13, 16)) fail("SSE, xmm13", xmm13_cur, ymm13, 16);
    __asm__("movdqu %%xmm14, (%0)" : /* No Outputs */ : "r" (xmm14_cur) : "memory");
    if (mem_cmp(xmm14_cur, ymm14, 16)) fail("SSE, xmm14", xmm14_cur, ymm14, 16);
    __asm__("movdqu %%xmm15, (%0)" : /* No Outputs */ : "r" (xmm15_cur) : "memory");
    if (mem_cmp(xmm15_cur, ymm15, 16)) fail("SSE, xmm15", xmm15_cur, ymm15, 16);
  }

  // Check AVX:
  if (xcr0 & AVX) {
    // Check YMMs
    __asm__("vmovdqu %%ymm0, (%0)" : /* No Outputs */ : "r" (ymm0_cur) : "memory");
    if (mem_cmp(ymm0_cur, ymm0, 32)) fail("AVX, ymm0", ymm0_cur, ymm0, 32);
    __asm__("vmovdqu %%ymm1, (%0)" : /* No Outputs */ : "r" (ymm1_cur) : "memory");
    if (mem_cmp(ymm1_cur, ymm1, 32)) fail("AVX, ymm1", ymm1_cur, ymm1, 32);
    __asm__("vmovdqu %%ymm2, (%0)" : /* No Outputs */ : "r" (ymm2_cur) : "memory");
    if (mem_cmp(ymm2_cur, ymm2, 32)) fail("AVX, ymm2", ymm2_cur, ymm2, 32);
    __asm__("vmovdqu %%ymm3, (%0)" : /* No Outputs */ : "r" (ymm3_cur) : "memory");
    if (mem_cmp(ymm3_cur, ymm3, 32)) fail("AVX, ymm3", ymm3_cur, ymm3, 32);
    __asm__("vmovdqu %%ymm4, (%0)" : /* No Outputs */ : "r" (ymm4_cur) : "memory");
    if (mem_cmp(ymm4_cur, ymm4, 32)) fail("AVX, ymm4", ymm4_cur, ymm4, 32);
    __asm__("vmovdqu %%ymm5, (%0)" : /* No Outputs */ : "r" (ymm5_cur) : "memory");
    if (mem_cmp(ymm5_cur, ymm5, 32)) fail("AVX, ymm5", ymm5_cur, ymm5, 32);
    __asm__("vmovdqu %%ymm6, (%0)" : /* No Outputs */ : "r" (ymm6_cur) : "memory");
    if (mem_cmp(ymm6_cur, ymm6, 32)) fail("AVX, ymm6", ymm6_cur, ymm6, 32);
    __asm__("vmovdqu %%ymm7, (%0)" : /* No Outputs */ : "r" (ymm7_cur) : "memory");
    if (mem_cmp(ymm7_cur, ymm7, 32)) fail("AVX, ymm7", ymm7_cur, ymm7, 32);

    __asm__("vmovdqu %%ymm8, (%0)"  : /* No Outputs */ : "r" (ymm8_cur)  : "memory");
    if (mem_cmp(ymm8_cur, ymm8, 32)) fail("AVX, ymm8", ymm8_cur, ymm8, 32);
    __asm__("vmovdqu %%ymm9, (%0)"  : /* No Outputs */ : "r" (ymm9_cur)  : "memory");
    if (mem_cmp(ymm9_cur, ymm9, 32)) fail("AVX, ymm9", ymm9_cur, ymm9, 32);
    __asm__("vmovdqu %%ymm10, (%0)" : /* No Outputs */ : "r" (ymm10_cur) : "memory");
    if (mem_cmp(ymm10_cur, ymm10, 32)) fail("AVX, ymm10", ymm10_cur, ymm10, 32);
    __asm__("vmovdqu %%ymm11, (%0)" : /* No Outputs */ : "r" (ymm11_cur) : "memory");
    if (mem_cmp(ymm11_cur, ymm11, 32)) fail("AVX, ymm11", ymm11_cur, ymm11, 32);
    __asm__("vmovdqu %%ymm12, (%0)" : /* No Outputs */ : "r" (ymm12_cur) : "memory");
    if (mem_cmp(ymm12_cur, ymm12, 32)) fail("AVX, ymm12", ymm12_cur, ymm12, 32);
    __asm__("vmovdqu %%ymm13, (%0)" : /* No Outputs */ : "r" (ymm13_cur) : "memory");
    if (mem_cmp(ymm13_cur, ymm13, 32)) fail("AVX, ymm13", ymm13_cur, ymm13, 32);
    __asm__("vmovdqu %%ymm14, (%0)" : /* No Outputs */ : "r" (ymm14_cur) : "memory");
    if (mem_cmp(ymm14_cur, ymm14, 32)) fail("AVX, ymm14", ymm14_cur, ymm14, 32);
    __asm__("vmovdqu %%ymm15, (%0)" : /* No Outputs */ : "r" (ymm15_cur) : "memory");
    if (mem_cmp(ymm15_cur, ymm15, 32)) fail("AVX, ymm15", ymm15_cur, ymm15, 32);

  }


// TODO: My workstation doesn't have the below state components,
  //     and some of them don't even exist in hardware you can
  //     buy yet, so there isn't really an easy way to verify
  //     that this test is operating correctly for those
  //     (e.g. we can't hex_dump what we don't have)

  // Check BNDREG
  if (xcr0 & BNDREG) {
    // TODO
  }

  // Check BNDCSR
  if (xcr0 & BNDCSR) {
    // TODO
  }

  // Check OPMASK
  if (xcr0 & OPMASK) {
    // TODO
  }

  // Check ZMM_HI_256
  if (xcr0 & ZMM_HI256) {
    // TODO
  }

  // Check HI16_ZMM
  if (xcr0 & HI16_ZMM) {
    // TODO
  }

  // Check PKRU
  if (xcr0 & PKRU) {
    // TODO
  }

}

int main() {
  pthread_mcp_init();
  uint32_t edx = 0;
  uint32_t eax = 0;
  uint64_t xcr0 = 0;

  printf("Starting ext_state_leak_test_a.\n");

  xgetbv_ecx0(&edx, &eax);
  xcr0 = ((uint64_t)edx << 32) | eax;

  /* This test currently tries to load and read up through the YMM registers
     and is designed ONLY FOR 64 BIT SYSTEMS! (so you get the extra XMM/YMM registers)
   */

  // Allocate memory for reading out current values of fpu and extended state

  mm0_cur = malloc(8);
  mm1_cur = malloc(8);
  mm2_cur = malloc(8);
  mm3_cur = malloc(8);
  mm4_cur = malloc(8);
  mm5_cur = malloc(8);
  mm6_cur = malloc(8);
  mm7_cur = malloc(8);

  mxcsr_cur = malloc(4);

  xmm0_cur  = malloc(16);
  xmm1_cur  = malloc(16);
  xmm2_cur  = malloc(16);
  xmm3_cur  = malloc(16);
  xmm4_cur  = malloc(16);
  xmm5_cur  = malloc(16);
  xmm6_cur  = malloc(16);
  xmm7_cur  = malloc(16);
  xmm8_cur  = malloc(16);
  xmm9_cur  = malloc(16);
  xmm10_cur = malloc(16);
  xmm11_cur = malloc(16);
  xmm12_cur = malloc(16);
  xmm13_cur = malloc(16);
  xmm14_cur = malloc(16);
  xmm15_cur = malloc(16);

  ymm0_cur  = malloc(32);
  ymm1_cur  = malloc(32);
  ymm2_cur  = malloc(32);
  ymm3_cur  = malloc(32);
  ymm4_cur  = malloc(32);
  ymm5_cur  = malloc(32);
  ymm6_cur  = malloc(32);
  ymm7_cur  = malloc(32);
  ymm8_cur  = malloc(32);
  ymm9_cur  = malloc(32);
  ymm10_cur = malloc(32);
  ymm11_cur = malloc(32);
  ymm12_cur = malloc(32);
  ymm13_cur = malloc(32);
  ymm14_cur = malloc(32);
  ymm15_cur = malloc(32);

  // Fill the values once, and then they should never change!
  set_values(xcr0);
  // And check check check to see if those values
  // ever get corrupted by something!
  while(1) {
    check_values(xcr0);
  }
}