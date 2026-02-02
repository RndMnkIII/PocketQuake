/*
 * VexRiscv startup code
 * Sets up stack pointer and jumps to main
 * Includes trap handler for misaligned access emulation and crash debugging
 */

.section .text.start
.global _start

_start:
    /* Cold boot: set up stack pointer */
    lui sp, %hi(_stack_top)
    addi sp, sp, %lo(_stack_top)

    /* Check if this is a trap (exception re-entry) or cold boot.
     * On cold boot, mcause is 0. On exception, mcause is non-zero.
     * VexRiscv jumps to mtvec (address 0) on any exception. */
    csrr t0, mcause
    bnez t0, _trap_entry

    /* Enable FPU: set mstatus.FS = 11 (Dirty) — bits [14:13] */
    li t0, (3 << 13)
    csrs mstatus, t0

    /* Set mtvec to point to our trap handler (address 0 = _start) */
    la t0, _start
    csrw mtvec, t0

    /* Mark that we've booted (store magic value at fixed BRAM address) */
    lui t0, 0x0000F     /* Use top of BRAM area 0x0000F000 as scratch */
    li t1, 0xDEAD0001
    sw t1, 0(t0)

    /* Clear BSS section */
    lui a0, %hi(_bss_start)
    addi a0, a0, %lo(_bss_start)
    lui a1, %hi(_bss_end)
    addi a1, a1, %lo(_bss_end)
clear_bss:
    bge a0, a1, done_bss
    sw zero, 0(a0)
    addi a0, a0, 4
    j clear_bss
done_bss:

    /* Jump to main */
    jal ra, main

    /* If main returns, loop forever */
halt:
    j halt


/* ========================================
 * Trap entry - saves all registers and dispatches
 * Trap frame layout: x0-x31 (32 words), mepc, mcause, mtval = 35 words = 140 bytes
 * ======================================== */
_trap_entry:
    /* We need a safe stack. Use top of BRAM scratch area temporarily.
     * Save current sp first so we can include it in the frame. */
    csrw mscratch, sp

    /* Switch to trap stack in BRAM */
    lui sp, %hi(_stack_top)
    addi sp, sp, %lo(_stack_top)

    /* Allocate trap frame: 35 words = 140 bytes */
    addi sp, sp, -140

    /* Save x1-x31 (skip x0 which is always 0) */
    sw x1,   4(sp)
    /* x2 (sp) - save the original sp from mscratch */
    csrr t0, mscratch
    sw t0,   8(sp)
    sw x3,  12(sp)
    sw x4,  16(sp)
    sw x5,  20(sp)
    sw x6,  24(sp)
    sw x7,  28(sp)
    sw x8,  32(sp)
    sw x9,  36(sp)
    sw x10, 40(sp)
    sw x11, 44(sp)
    sw x12, 48(sp)
    sw x13, 52(sp)
    sw x14, 56(sp)
    sw x15, 60(sp)
    sw x16, 64(sp)
    sw x17, 68(sp)
    sw x18, 72(sp)
    sw x19, 76(sp)
    sw x20, 80(sp)
    sw x21, 84(sp)
    sw x22, 88(sp)
    sw x23, 92(sp)
    sw x24, 96(sp)
    sw x25, 100(sp)
    sw x26, 104(sp)
    sw x27, 108(sp)
    sw x28, 112(sp)
    sw x29, 116(sp)
    sw x30, 120(sp)
    sw x31, 124(sp)

    /* Save CSRs: mepc, mcause, mtval */
    csrr t0, mepc
    sw t0, 128(sp)
    csrr t0, mcause
    sw t0, 132(sp)
    csrr t0, mtval
    sw t0, 136(sp)

    /* Store 0 at x0 position (for completeness) */
    sw zero, 0(sp)

    /* Call C handler: handle_misaligned(trap_frame_t *frame)
     * Returns 1 if handled (should return), 0 if fatal */
    mv a0, sp
    jal ra, handle_misaligned

    /* Check return value */
    beqz a0, _fatal_trap

    /* Handled - restore mepc (may have been modified) */
    lw t0, 128(sp)
    csrw mepc, t0

    /* Restore registers */
    lw x1,   4(sp)
    /* Restore original sp to mscratch temporarily */
    lw t0,   8(sp)
    csrw mscratch, t0
    lw x3,  12(sp)
    lw x4,  16(sp)
    lw x5,  20(sp)
    lw x6,  24(sp)
    lw x7,  28(sp)
    lw x8,  32(sp)
    lw x9,  36(sp)
    lw x10, 40(sp)
    lw x11, 44(sp)
    lw x12, 48(sp)
    lw x13, 52(sp)
    lw x14, 56(sp)
    lw x15, 60(sp)
    lw x16, 64(sp)
    lw x17, 68(sp)
    lw x18, 72(sp)
    lw x19, 76(sp)
    lw x20, 80(sp)
    lw x21, 84(sp)
    lw x22, 88(sp)
    lw x23, 92(sp)
    lw x24, 96(sp)
    lw x25, 100(sp)
    lw x26, 104(sp)
    lw x27, 108(sp)
    lw x28, 112(sp)
    lw x29, 116(sp)
    lw x30, 120(sp)
    lw x31, 124(sp)

    /* Restore original sp from mscratch */
    csrr sp, mscratch

    /* Return from trap */
    mret

_fatal_trap:
    /* Call fatal_trap(trap_frame_t *frame) - does not return */
    mv a0, sp
    jal ra, fatal_trap
    j _fatal_trap

/* ========================================
 * Switch to a new runtime stack and jump to entry point.
 * a0 = function pointer, a1 = new stack top
 * Does not return.
 * ======================================== */
.global switch_to_runtime_stack_and_call
switch_to_runtime_stack_and_call:
    mv sp, a1
    jr a0
1:
    j 1b
