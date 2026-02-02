/*
 * Memory Controller Interleaved Access Test
 *
 * Tests SDRAM (64MB), PSRAM/CRAM0 (16MB), and SRAM (256KB)
 * with 1/2/4 byte accesses including byte-enable preservation.
 * Tests DMA contention with concurrent CPU memory access.
 *
 * Video scanout continuously reads SDRAM via burst interface,
 * providing realistic background contention for all SDRAM tests.
 */

#include <stdint.h>
#include "terminal.h"

/* ---- System registers ---- */
#define SYS_DISPLAY_MODE (*(volatile uint32_t *)0x4000000C)
#define SYS_CYCLE_LO    (*(volatile uint32_t *)0x40000004)

/* ---- DMA registers (0x44000000) ---- */
#define DMA_SRC_ADDR  (*(volatile uint32_t *)0x44000000)
#define DMA_DST_ADDR  (*(volatile uint32_t *)0x44000004)
#define DMA_LENGTH    (*(volatile uint32_t *)0x44000008)
#define DMA_FILL_DATA (*(volatile uint32_t *)0x4400000C)
#define DMA_CONTROL   (*(volatile uint32_t *)0x44000010)
#define DMA_STATUS    (*(volatile uint32_t *)0x44000014)

/* ---- Test addresses (non-overlapping) ---- */
#define SDRAM_TEST    0x10400000u  /* SDRAM test area (past FBs) */
#define PSRAM_TEST    0x30100000u  /* PSRAM test area */
#define SRAM_TEST     0x38000000u  /* SRAM test area (256KB at 0x38000000) */
#define SDRAM_DMA_CPU 0x10600000u  /* CPU area during DMA */
#define PSRAM_DMA_CPU 0x30200000u  /* CPU PSRAM area during DMA */
#define SRAM_DMA_CPU  0x38010000u  /* CPU SRAM area during DMA */
#define DMA_TARGET    0x10800000u  /* DMA fill/copy target */
#define DMA_TARGET2   0x10804000u  /* DMA copy destination */

#define N_WORDS   256
#define DMA_SIZE  16384  /* 16KB - long enough for overlap */

/* ---- Counters ---- */
static int pass_count, fail_count;

/* ---- Report helper ---- */
static void report(const char *name, int errs)
{
    term_puts(name);
    term_puts(": ");
    if (errs == 0) {
        term_puts("OK\n");
        pass_count++;
    } else {
        term_puts("FAIL ");
        term_putdec(errs);
        term_putchar('\n');
        fail_count++;
    }
}

/* ============================================ */
/* Word (32-bit) read/write                     */
/* ============================================ */
static int test_word(uint32_t addr, int n)
{
    volatile uint32_t *p = (volatile uint32_t *)addr;
    int err = 0;
    for (int i = 0; i < n; i++)
        p[i] = 0xA5000000u | (uint32_t)i;
    for (int i = 0; i < n; i++)
        if (p[i] != (0xA5000000u | (uint32_t)i)) err++;
    return err;
}

/* ============================================ */
/* Halfword (16-bit) read/write                 */
/* ============================================ */
static int test_half(uint32_t addr, int n)
{
    volatile uint16_t *p = (volatile uint16_t *)addr;
    int err = 0;
    for (int i = 0; i < n; i++)
        p[i] = (uint16_t)(0xBE00u | (i & 0xFF));
    for (int i = 0; i < n; i++)
        if (p[i] != (uint16_t)(0xBE00u | (i & 0xFF))) err++;
    return err;
}

/* ============================================ */
/* Byte (8-bit) read/write                      */
/* ============================================ */
static int test_byte(uint32_t addr, int n)
{
    volatile uint8_t *p = (volatile uint8_t *)addr;
    int err = 0;
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(i ^ 0x55);
    for (int i = 0; i < n; i++)
        if (p[i] != (uint8_t)(i ^ 0x55)) err++;
    return err;
}

/* ============================================ */
/* Byte-within-word preservation                */
/* Write a word, overwrite single byte, check   */
/* that other bytes are preserved.              */
/* ============================================ */
static int test_byte_preserve(uint32_t addr, int n)
{
    volatile uint32_t *wp = (volatile uint32_t *)addr;
    volatile uint8_t  *bp = (volatile uint8_t  *)addr;
    int err = 0;
    for (int i = 0; i < n; i++) {
        /* Overwrite byte 0 (LSB in little-endian) */
        wp[i] = 0x12345678u;
        bp[i * 4] = 0xAA;
        if (wp[i] != 0x123456AAu) err++;

        /* Overwrite byte 2 */
        wp[i] = 0x12345678u;
        bp[i * 4 + 2] = 0xBB;
        if (wp[i] != 0x12BB5678u) err++;
    }
    return err;
}

/* ============================================ */
/* Halfword-within-word preservation            */
/* ============================================ */
static int test_half_preserve(uint32_t addr, int n)
{
    volatile uint32_t *wp = (volatile uint32_t *)addr;
    volatile uint16_t *hp = (volatile uint16_t *)addr;
    int err = 0;
    for (int i = 0; i < n; i++) {
        /* Overwrite low halfword */
        wp[i] = 0x12345678u;
        hp[i * 2] = 0xCAFEu;
        if (wp[i] != 0x1234CAFEu) err++;

        /* Overwrite high halfword */
        wp[i] = 0x12345678u;
        hp[i * 2 + 1] = 0xBEEFu;
        if (wp[i] != 0xBEEF5678u) err++;
    }
    return err;
}

/* ============================================ */
/* Interleaved SDRAM + PSRAM + SRAM word access */
/* ============================================ */
static int test_interleaved_word(void)
{
    volatile uint32_t *sd = (volatile uint32_t *)SDRAM_TEST;
    volatile uint32_t *ps = (volatile uint32_t *)PSRAM_TEST;
    volatile uint32_t *sr = (volatile uint32_t *)SRAM_TEST;
    int err = 0, n = 128;

    for (int i = 0; i < n; i++) {
        sd[i] = 0xAA000000u | (uint32_t)i;
        ps[i] = 0xBB000000u | (uint32_t)i;
        sr[i] = 0xCC000000u | (uint32_t)i;
    }
    for (int i = 0; i < n; i++) {
        if (sd[i] != (0xAA000000u | (uint32_t)i)) err++;
        if (ps[i] != (0xBB000000u | (uint32_t)i)) err++;
        if (sr[i] != (0xCC000000u | (uint32_t)i)) err++;
    }
    return err;
}

/* ============================================ */
/* Interleaved mixed-size across all memories   */
/* ============================================ */
static int test_interleaved_mixed(void)
{
    volatile uint8_t  *sb = (volatile uint8_t  *)(SDRAM_TEST + 0x1000u);
    volatile uint16_t *sh = (volatile uint16_t *)(SDRAM_TEST + 0x2000u);
    volatile uint32_t *pw = (volatile uint32_t *)(PSRAM_TEST + 0x1000u);
    volatile uint8_t  *srb = (volatile uint8_t  *)(SRAM_TEST + 0x1000u);
    volatile uint16_t *srh = (volatile uint16_t *)(SRAM_TEST + 0x2000u);
    int err = 0, n = 64;

    for (int i = 0; i < n; i++) {
        sb[i] = (uint8_t)(i ^ 0x55);
        pw[i] = 0xCC000000u | (uint32_t)i;
        sh[i] = (uint16_t)(0xDD00u | (i & 0xFF));
        srb[i] = (uint8_t)(i ^ 0xAA);
        srh[i] = (uint16_t)(0xFF00u | (i & 0xFF));
    }
    for (int i = 0; i < n; i++) {
        if (sb[i] != (uint8_t)(i ^ 0x55))                err++;
        if (pw[i] != (0xCC000000u | (uint32_t)i))        err++;
        if (sh[i] != (uint16_t)(0xDD00u | (i & 0xFF)))   err++;
        if (srb[i] != (uint8_t)(i ^ 0xAA))               err++;
        if (srh[i] != (uint16_t)(0xFF00u | (i & 0xFF)))   err++;
    }
    return err;
}

/* ============================================ */
/* DMA helpers                                  */
/* ============================================ */
static void dma_start_fill(uint32_t dst, uint32_t len, uint32_t pattern)
{
    DMA_DST_ADDR  = dst;
    DMA_LENGTH    = len;
    DMA_FILL_DATA = pattern;
    DMA_CONTROL   = 0x01;  /* bit0=start, bit1=0 → fill mode */
}

static void dma_start_copy(uint32_t src, uint32_t dst, uint32_t len)
{
    DMA_SRC_ADDR = src;
    DMA_DST_ADDR = dst;
    DMA_LENGTH   = len;
    DMA_CONTROL  = 0x03;  /* bit0=start, bit1=1 → copy mode */
}

static inline int  dma_busy(void) { return DMA_STATUS & 1; }
static inline void dma_wait(void) { while (dma_busy()); }

/* ============================================ */
/* DMA fill + CPU PSRAM+SRAM work (true overlap)*/
/* DMA owns SDRAM bus; CPU uses PSRAM/SRAM.     */
/* ============================================ */
static int test_dma_fill_psram_sram(void)
{
    volatile uint32_t *cp = (volatile uint32_t *)PSRAM_DMA_CPU;
    volatile uint32_t *cs = (volatile uint32_t *)SRAM_DMA_CPU;
    volatile uint32_t *dt = (volatile uint32_t *)DMA_TARGET;
    int err = 0, n = 128;

    dma_start_fill(DMA_TARGET, DMA_SIZE, 0xDEADBEEFu);

    /* CPU does PSRAM + SRAM R/W while DMA fills SDRAM */
    for (int i = 0; i < n; i++) {
        cp[i] = 0xCAFE0000u | (uint32_t)i;
        cs[i] = 0xFACE0000u | (uint32_t)i;
    }
    for (int i = 0; i < n; i++) {
        if (cp[i] != (0xCAFE0000u | (uint32_t)i)) err++;
        if (cs[i] != (0xFACE0000u | (uint32_t)i)) err++;
    }

    dma_wait();

    /* Verify DMA fill result */
    for (int i = 0; i < (int)(DMA_SIZE / 4); i++)
        if (dt[i] != 0xDEADBEEFu) err++;

    return err;
}

/* ============================================ */
/* DMA fill + CPU SDRAM stall test              */
/* CPU SDRAM access is blocked while DMA runs.  */
/* Verifies stalling doesn't corrupt data.      */
/* ============================================ */
static int test_dma_fill_sdram(void)
{
    volatile uint32_t *cs = (volatile uint32_t *)SDRAM_DMA_CPU;
    volatile uint32_t *dt = (volatile uint32_t *)DMA_TARGET;
    int err = 0, n = 128;

    /* Pre-fill CPU SDRAM area */
    for (int i = 0; i < n; i++)
        cs[i] = 0xFACE0000u | (uint32_t)i;

    dma_start_fill(DMA_TARGET, DMA_SIZE, 0xDEADBEEFu);

    /* CPU reads SDRAM (stalls until DMA releases bus) */
    for (int i = 0; i < n; i++)
        if (cs[i] != (0xFACE0000u | (uint32_t)i)) err++;

    dma_wait();

    /* Verify DMA fill */
    for (int i = 0; i < (int)(DMA_SIZE / 4); i++)
        if (dt[i] != 0xDEADBEEFu) err++;

    return err;
}

/* ============================================ */
/* DMA copy + CPU PSRAM work (true overlap)     */
/* ============================================ */
static int test_dma_copy_psram(void)
{
    volatile uint32_t *src = (volatile uint32_t *)DMA_TARGET;
    volatile uint32_t *dst = (volatile uint32_t *)DMA_TARGET2;
    volatile uint32_t *cp  = (volatile uint32_t *)(PSRAM_DMA_CPU + 0x1000u);
    int err = 0, n = 128;

    /* Fill source region */
    for (int i = 0; i < (int)(DMA_SIZE / 4); i++)
        src[i] = 0xC0DE0000u | (uint32_t)i;

    dma_start_copy(DMA_TARGET, DMA_TARGET2, DMA_SIZE);

    /* CPU does PSRAM R/W while DMA copies SDRAM */
    for (int i = 0; i < n; i++)
        cp[i] = 0xBBBB0000u | (uint32_t)i;
    for (int i = 0; i < n; i++)
        if (cp[i] != (0xBBBB0000u | (uint32_t)i)) err++;

    dma_wait();

    /* Verify DMA copy */
    for (int i = 0; i < (int)(DMA_SIZE / 4); i++)
        if (dst[i] != (0xC0DE0000u | (uint32_t)i)) err++;

    return err;
}

/* ============================================ */
/* Main                                         */
/* ============================================ */
int main(void)
{
    SYS_DISPLAY_MODE = 0;  /* Terminal mode */
    term_init();

    term_puts("=== Mem Controller Test ===\n\n");

    pass_count = 0;
    fail_count = 0;

    uint32_t t0 = SYS_CYCLE_LO;

    /* ---- SDRAM ---- */
    term_puts("-- SDRAM --\n");
    report("word R/W",       test_word(SDRAM_TEST, N_WORDS));
    report("half R/W",       test_half(SDRAM_TEST, N_WORDS * 2));
    report("byte R/W",       test_byte(SDRAM_TEST, N_WORDS * 4));
    report("byte preserve",  test_byte_preserve(SDRAM_TEST, 64));
    report("half preserve",  test_half_preserve(SDRAM_TEST, 64));

    /* ---- PSRAM ---- */
    term_puts("-- PSRAM --\n");
    report("word R/W",       test_word(PSRAM_TEST, N_WORDS));
    report("half R/W",       test_half(PSRAM_TEST, N_WORDS * 2));
    report("byte R/W",       test_byte(PSRAM_TEST, N_WORDS * 4));
    report("byte preserve",  test_byte_preserve(PSRAM_TEST, 64));
    report("half preserve",  test_half_preserve(PSRAM_TEST, 64));

    /* ---- SRAM diagnostic ---- */
    term_puts("-- SRAM --\n");
    {
        volatile uint32_t *sp = (volatile uint32_t *)SRAM_TEST;
        /* Use offset 100 to avoid stale data at addr 0 */
        volatile uint32_t *tp = sp + 100;
        /* Read before writing - shows stale/random data */
        uint32_t before = tp[0];
        term_puts("pre: ");
        term_puthex(before, 8);
        /* Write a unique pattern (never used before) */
        tp[0] = 0x1337C0DEu;
        uint32_t after = tp[0];
        term_puts(" wr 1337C0DE rd ");
        term_puthex(after, 8);
        term_putchar('\n');
    }
    report("word R/W",       test_word(SRAM_TEST, N_WORDS));
    report("half R/W",       test_half(SRAM_TEST, N_WORDS * 2));
    report("byte R/W",       test_byte(SRAM_TEST, N_WORDS * 4));
    report("byte preserve",  test_byte_preserve(SRAM_TEST, 64));
    report("half preserve",  test_half_preserve(SRAM_TEST, 64));

    /* ---- Interleaved ---- */
    term_puts("-- Interleaved --\n");
    report("all 3 word",      test_interleaved_word());
    report("mixed sizes",     test_interleaved_mixed());

    /* ---- DMA contention ---- */
    term_puts("-- DMA Contention --\n");
    report("fill+PSRAM+SRAM", test_dma_fill_psram_sram());
    report("fill+SDRAM stall", test_dma_fill_sdram());
    report("copy+PSRAM",      test_dma_copy_psram());

    uint32_t t1 = SYS_CYCLE_LO;

    term_putchar('\n');
    term_putdec(pass_count);
    term_putchar('/');
    term_putdec(pass_count + fail_count);
    term_puts(" pass  ");
    term_putdec((int32_t)(t1 - t0));
    term_puts(" cyc\n");

    if (fail_count == 0)
        term_puts("ALL PASSED\n");
    else {
        term_putdec(fail_count);
        term_puts(" FAILED\n");
    }

    while (1)
        __asm__ volatile("wfi");

    return 0;
}
