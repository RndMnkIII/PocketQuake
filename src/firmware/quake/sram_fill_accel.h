/*
 * SRAM Fill Accelerator - Hardware z-buffer clear via SRAM
 * Writes a 32-bit fill pattern to sequential SRAM addresses.
 * Runs in background while CPU performs other setup.
 */

#ifndef SRAM_FILL_ACCEL_H
#define SRAM_FILL_ACCEL_H

#define SRAM_FILL_BASE     0x5C000000u

#define SRAM_FILL_DST      (*(volatile unsigned int *)(SRAM_FILL_BASE + 0x00))
#define SRAM_FILL_LENGTH   (*(volatile unsigned int *)(SRAM_FILL_BASE + 0x04))
#define SRAM_FILL_DATA     (*(volatile unsigned int *)(SRAM_FILL_BASE + 0x08))
#define SRAM_FILL_CONTROL  (*(volatile unsigned int *)(SRAM_FILL_BASE + 0x0C))
#define SRAM_FILL_STATUS   (*(volatile unsigned int *)(SRAM_FILL_BASE + 0x10))

static inline void sram_fill_start(unsigned int dst_addr, unsigned int length, unsigned int fill_value)
{
    SRAM_FILL_DST     = dst_addr;
    SRAM_FILL_LENGTH  = length;
    SRAM_FILL_DATA    = fill_value;
    SRAM_FILL_CONTROL = 1;  /* start */
}

static inline void sram_fill_wait(void)
{
    while (SRAM_FILL_STATUS & 1)
        ;
}

#endif /* SRAM_FILL_ACCEL_H */
