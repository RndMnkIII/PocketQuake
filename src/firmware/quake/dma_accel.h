#ifndef DMA_ACCEL_H
#define DMA_ACCEL_H

/*
 * DMA Clear/Blit Hardware Accelerator
 * Fast SDRAM fill (memset) and copy (memcpy) operations.
 * CPU is free to do non-SDRAM work while DMA is running.
 */

#define HW_DMA_ACCEL 1

#define DMA_BASE        0x44000000
#define DMA_SRC_ADDR    (*(volatile unsigned int *)(DMA_BASE + 0x00))
#define DMA_DST_ADDR    (*(volatile unsigned int *)(DMA_BASE + 0x04))
#define DMA_LENGTH      (*(volatile unsigned int *)(DMA_BASE + 0x08))
#define DMA_FILL_DATA   (*(volatile unsigned int *)(DMA_BASE + 0x0C))
#define DMA_CONTROL     (*(volatile unsigned int *)(DMA_BASE + 0x10))
#define DMA_STATUS      (*(volatile unsigned int *)(DMA_BASE + 0x14))

#define DMA_CTRL_START  0x01
#define DMA_CTRL_COPY   0x02    /* 0=fill, 1=copy */
#define DMA_STATUS_BUSY 0x01

/* Start a fill operation (non-blocking). Length must be 4-byte aligned. */
static inline void dma_fill(unsigned int dst_addr, unsigned int length, unsigned int fill_value)
{
    DMA_DST_ADDR  = dst_addr;
    DMA_LENGTH    = length;
    DMA_FILL_DATA = fill_value;
    DMA_CONTROL   = DMA_CTRL_START;
}

/* Start a copy operation (non-blocking). Length must be 4-byte aligned. */
static inline void dma_copy(unsigned int src_addr, unsigned int dst_addr, unsigned int length)
{
    DMA_SRC_ADDR = src_addr;
    DMA_DST_ADDR = dst_addr;
    DMA_LENGTH   = length;
    DMA_CONTROL  = DMA_CTRL_START | DMA_CTRL_COPY;
}

/* Check if DMA is still running */
static inline int dma_busy(void)
{
    return DMA_STATUS & DMA_STATUS_BUSY;
}

/* Block until DMA completes */
static inline void dma_wait(void)
{
    while (DMA_STATUS & DMA_STATUS_BUSY)
        ;
}

#endif /* DMA_ACCEL_H */
