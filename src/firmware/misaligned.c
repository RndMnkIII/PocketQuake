/*
 * Misaligned access trap handler for RISC-V
 * Emulates unaligned loads/stores using byte operations
 * Supports both 32-bit and compressed (RVC) instructions
 */

#include "terminal.h"
extern volatile unsigned int pq_dbg_stage;
extern volatile unsigned int pq_dbg_info;

/* Trap frame layout (matches start.S) */
typedef struct {
    unsigned int regs[32];   /* x0-x31 (x0 always 0) at offset 0 */
    unsigned int mepc;       /* at offset 128 */
    unsigned int mcause;     /* at offset 132 */
    unsigned int mtval;      /* at offset 136 */
    unsigned int fregs[32];  /* f0-f31 at offset 140 */
} trap_frame_t;

/* RISC-V 32-bit instruction encodings */
#define OPCODE_LOAD   0x03
#define OPCODE_STORE  0x23
#define OPCODE_FLW    0x07  /* Float load (I-type, funct3=010) */
#define OPCODE_FSW    0x27  /* Float store (S-type, funct3=010) */

#define FUNCT3_LB     0x0
#define FUNCT3_LH     0x1
#define FUNCT3_LW     0x2
#define FUNCT3_LBU    0x4
#define FUNCT3_LHU    0x5

#define FUNCT3_SB     0x0
#define FUNCT3_SH     0x1
#define FUNCT3_SW     0x2

/* mcause values */
#define CAUSE_LOAD_MISALIGNED   4
#define CAUSE_STORE_MISALIGNED  6

/* RVC compressed register mapping: r' = x8 + r'[2:0] */
#define CRV_REG(bits) (8 + ((bits) & 0x7))

/* Valid memory regions for emulation */
#define BRAM_START      0x00000000
#define BRAM_END        0x00010000
#define SDRAM_START     0x10000000
#define SDRAM_END       0x14000000
#define PSRAM_START     0x30000000
#define PSRAM_END       0x38000000
#define SDRAM_UC_START  0x50000000  /* Uncached SDRAM alias */
#define SDRAM_UC_END    0x54000000

/* Check if address range is in valid memory */
__attribute__((section(".text.boot")))
static int addr_valid(unsigned int addr, unsigned int len) {
    unsigned int end = addr + len - 1;
    /* Check for overflow */
    if (end < addr) return 0;
    /* BRAM */
    if (addr >= BRAM_START && end < BRAM_END) return 1;
    /* SDRAM (cached) */
    if (addr >= SDRAM_START && end < SDRAM_END) return 1;
    /* PSRAM */
    if (addr >= PSRAM_START && end < PSRAM_END) return 1;
    /* SDRAM (uncached alias — used for PAK data) */
    if (addr >= SDRAM_UC_START && end < SDRAM_UC_END) return 1;
    return 0;
}

/* Read byte from memory */
__attribute__((section(".text.boot")))
static inline unsigned char read_byte(unsigned int addr) {
    return *(volatile unsigned char *)addr;
}

/* Write byte to memory */
__attribute__((section(".text.boot")))
static inline void write_byte(unsigned int addr, unsigned char val) {
    *(volatile unsigned char *)addr = val;
}

/* Emulate misaligned load (word) */
__attribute__((section(".text.boot")))
static unsigned int emulate_load_word(unsigned int addr) {
    return read_byte(addr) |
           (read_byte(addr + 1) << 8) |
           (read_byte(addr + 2) << 16) |
           (read_byte(addr + 3) << 24);
}

/* Emulate misaligned load */
__attribute__((section(".text.boot")))
static unsigned int emulate_load(unsigned int addr, int funct3) {
    unsigned int val = 0;

    switch (funct3) {
    case FUNCT3_LH:  /* Load halfword (signed) */
        val = read_byte(addr) | (read_byte(addr + 1) << 8);
        val = (int)(signed short)val;
        break;

    case FUNCT3_LHU: /* Load halfword (unsigned) */
        val = read_byte(addr) | (read_byte(addr + 1) << 8);
        break;

    case FUNCT3_LW:  /* Load word */
        val = emulate_load_word(addr);
        break;
    }

    return val;
}

/* Emulate misaligned store */
__attribute__((section(".text.boot")))
static void emulate_store(unsigned int addr, unsigned int val, int funct3) {
    switch (funct3) {
    case FUNCT3_SH:  /* Store halfword */
        write_byte(addr, val & 0xFF);
        write_byte(addr + 1, (val >> 8) & 0xFF);
        break;

    case FUNCT3_SW:  /* Store word */
        write_byte(addr, val & 0xFF);
        write_byte(addr + 1, (val >> 8) & 0xFF);
        write_byte(addr + 2, (val >> 16) & 0xFF);
        write_byte(addr + 3, (val >> 24) & 0xFF);
        break;
    }
}

/* Debug counter for misaligned traps */
static unsigned int misaligned_count = 0;

/* Read instruction at PC using byte reads (safe for 2-byte aligned RVC PCs) */
__attribute__((section(".text.boot")))
static unsigned int read_instr_at(unsigned int pc) {
    unsigned char *p = (unsigned char *)pc;
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/* Try to handle compressed (RVC) misaligned load/store.
 * Returns 1 if handled, 0 if not an RVC load/store. */
__attribute__((section(".text.boot")))
static int handle_rvc_misaligned(trap_frame_t *frame, unsigned int instr16) {
    unsigned int quadrant = instr16 & 0x3;
    unsigned int funct3 = (instr16 >> 13) & 0x7;
    unsigned int addr;
    unsigned int val;

    if (quadrant == 0) {
        /* Quadrant 0: C.LW, C.SW, C.FLW, C.FSW
         * Format CL/CS: [15:13 funct3] [12:10 imm] [9:7 rs1'] [6:5 imm] [4:2 rd'/rs2'] [1:0 op]
         * C.LW/C.SW offset: {imm[5:3], imm[2], imm[6]} << 2, zero-extended, scaled by 4
         *   imm bits: [12:10] = bits 5,4,3; [6] = bit 2; [5] = bit 6
         */
        unsigned int rs1p = CRV_REG((instr16 >> 7) & 0x7);
        unsigned int rd_rs2p = CRV_REG((instr16 >> 2) & 0x7);
        /* Offset: bit5=[12], bit4=[11], bit3=[10], bit2=[6], bit6=[5] */
        unsigned int offset = (((instr16 >> 10) & 0x7) << 3) |  /* bits 5:3 */
                              (((instr16 >> 6) & 0x1) << 2) |   /* bit 2 */
                              (((instr16 >> 5) & 0x1) << 6);    /* bit 6 */

        addr = frame->regs[rs1p] + offset;

        if (funct3 == 0x2) {
            /* C.LW: load word */
            if (!addr_valid(addr, 4)) return 0;
            val = emulate_load_word(addr);
            if (rd_rs2p != 0) frame->regs[rd_rs2p] = val;
            frame->mepc += 2;
            return 1;
        }
        if (funct3 == 0x6) {
            /* C.SW: store word */
            if (!addr_valid(addr, 4)) return 0;
            emulate_store(addr, frame->regs[rd_rs2p], FUNCT3_SW);
            frame->mepc += 2;
            return 1;
        }
        if (funct3 == 0x3) {
            /* C.FLW: float load word */
            if (!addr_valid(addr, 4)) return 0;
            frame->fregs[rd_rs2p] = emulate_load_word(addr);
            frame->mepc += 2;
            return 1;
        }
        if (funct3 == 0x7) {
            /* C.FSW: float store word */
            if (!addr_valid(addr, 4)) return 0;
            emulate_store(addr, frame->fregs[rd_rs2p], FUNCT3_SW);
            frame->mepc += 2;
            return 1;
        }
    }

    if (quadrant == 2) {
        /* Quadrant 2: C.LWSP, C.SWSP, C.FLWSP, C.FSWSP */

        if (funct3 == 0x2) {
            /* C.LWSP: lw rd, offset(sp)
             * offset: {imm[5], imm[4:2], imm[7:6]} zero-extended, scaled by 4
             * Bits: [12]=bit5, [6:4]=bits4:2, [3:2]=bits7:6 */
            unsigned int rd = (instr16 >> 7) & 0x1F;
            unsigned int offset = (((instr16 >> 12) & 0x1) << 5) |  /* bit 5 */
                                  (((instr16 >> 4) & 0x7) << 2) |   /* bits 4:2 */
                                  (((instr16 >> 2) & 0x3) << 6);    /* bits 7:6 */
            addr = frame->regs[2] + offset;  /* sp = x2 */

            if (!addr_valid(addr, 4)) return 0;
            val = emulate_load_word(addr);
            if (rd != 0) frame->regs[rd] = val;
            frame->mepc += 2;
            return 1;
        }
        if (funct3 == 0x6) {
            /* C.SWSP: sw rs2, offset(sp)
             * offset: {imm[5:2], imm[7:6]} zero-extended, scaled by 4
             * Bits: [12:9]=bits5:2, [8:7]=bits7:6 */
            unsigned int rs2 = (instr16 >> 2) & 0x1F;
            unsigned int offset = (((instr16 >> 9) & 0xF) << 2) |   /* bits 5:2 */
                                  (((instr16 >> 7) & 0x3) << 6);    /* bits 7:6 */
            addr = frame->regs[2] + offset;  /* sp = x2 */

            if (!addr_valid(addr, 4)) return 0;
            emulate_store(addr, frame->regs[rs2], FUNCT3_SW);
            frame->mepc += 2;
            return 1;
        }
        if (funct3 == 0x3) {
            /* C.FLWSP: flw rd, offset(sp) */
            unsigned int rd = (instr16 >> 7) & 0x1F;
            unsigned int offset = (((instr16 >> 12) & 0x1) << 5) |
                                  (((instr16 >> 4) & 0x7) << 2) |
                                  (((instr16 >> 2) & 0x3) << 6);
            addr = frame->regs[2] + offset;

            if (!addr_valid(addr, 4)) return 0;
            frame->fregs[rd] = emulate_load_word(addr);
            frame->mepc += 2;
            return 1;
        }
        if (funct3 == 0x7) {
            /* C.FSWSP: fsw rs2, offset(sp) */
            unsigned int rs2 = (instr16 >> 2) & 0x1F;
            unsigned int offset = (((instr16 >> 9) & 0xF) << 2) |
                                  (((instr16 >> 7) & 0x3) << 6);
            addr = frame->regs[2] + offset;

            if (!addr_valid(addr, 4)) return 0;
            emulate_store(addr, frame->fregs[rs2], FUNCT3_SW);
            frame->mepc += 2;
            return 1;
        }
    }

    return 0;  /* Not an RVC load/store we handle */
}

/* Decode and handle misaligned access
 * Returns 1 if handled, 0 if should trap normally */
__attribute__((section(".text.boot")))
int handle_misaligned(trap_frame_t *frame) {
    unsigned int mcause = frame->mcause;

    /* Only handle misaligned load/store traps */
    if (mcause != CAUSE_LOAD_MISALIGNED && mcause != CAUSE_STORE_MISALIGNED)
        return 0;

    unsigned int instr = read_instr_at(frame->mepc);
    unsigned int opcode = instr & 0x7F;
    unsigned int funct3 = (instr >> 12) & 0x7;
    unsigned int rd = (instr >> 7) & 0x1F;
    unsigned int rs1 = (instr >> 15) & 0x1F;
    unsigned int rs2 = (instr >> 20) & 0x1F;
    int imm;
    unsigned int addr;

    misaligned_count++;

    /* Check if this is a compressed (RVC) instruction: bits [1:0] != 11 */
    if ((instr & 0x3) != 0x3) {
        return handle_rvc_misaligned(frame, instr & 0xFFFF);
    }

    /* 32-bit instructions below */

    if (opcode == OPCODE_LOAD) {
        /* I-type immediate: instr[31:20] sign-extended */
        imm = ((int)instr) >> 20;
        addr = frame->regs[rs1] + imm;

        /* Validate address before accessing */
        unsigned int access_len = (funct3 == FUNCT3_LW) ? 4 :
                                  (funct3 == FUNCT3_LH || funct3 == FUNCT3_LHU) ? 2 : 1;
        if (!addr_valid(addr, access_len))
            return 0;

        /* Emulate the load */
        unsigned int val = emulate_load(addr, funct3);

        /* Write to destination register (rd=0 is hardwired to 0, ignore) */
        if (rd != 0) {
            frame->regs[rd] = val;
        }

        /* Advance PC past the instruction */
        frame->mepc += 4;
        return 1;
    }

    if (opcode == OPCODE_STORE) {
        /* S-type immediate: {instr[31:25], instr[11:7]} sign-extended */
        imm = ((instr >> 7) & 0x1F) | (((int)instr >> 20) & 0xFFFFFFE0);
        addr = frame->regs[rs1] + imm;

        /* Validate address before accessing */
        unsigned int access_len = (funct3 == FUNCT3_SW) ? 4 :
                                  (funct3 == FUNCT3_SH) ? 2 : 1;
        if (!addr_valid(addr, access_len))
            return 0;

        /* Get value from source register */
        unsigned int val = frame->regs[rs2];

        /* Emulate the store */
        emulate_store(addr, val, funct3);

        /* Advance PC past the instruction */
        frame->mepc += 4;
        return 1;
    }

    /* FLW: float load word (I-type, opcode 0x07, funct3=010) */
    if (opcode == OPCODE_FLW) {
        imm = ((int)instr) >> 20;
        addr = frame->regs[rs1] + imm;

        if (!addr_valid(addr, 4))
            return 0;

        unsigned int val = emulate_load_word(addr);
        frame->fregs[rd] = val;
        frame->mepc += 4;
        return 1;
    }

    /* FSW: float store word (S-type, opcode 0x27, funct3=010) */
    if (opcode == OPCODE_FSW) {
        imm = ((instr >> 7) & 0x1F) | (((int)instr >> 20) & 0xFFFFFFE0);
        addr = frame->regs[rs1] + imm;

        if (!addr_valid(addr, 4))
            return 0;

        unsigned int val = frame->fregs[rs2];
        emulate_store(addr, val, FUNCT3_SW);
        frame->mepc += 4;
        return 1;
    }

    /* Not a load/store instruction - can't handle */
    return 0;
}

/* mcause name lookup */
__attribute__((section(".text.boot")))
static const char *mcause_name(unsigned int mc) {
    switch (mc) {
    case 0:  return "INSTR MISALIGN";
    case 1:  return "INSTR ACCESS";
    case 2:  return "ILLEGAL INSTR";
    case 4:  return "LOAD MISALIGN";
    case 5:  return "LOAD ACCESS";
    case 6:  return "STORE MISALIGN";
    case 7:  return "STORE ACCESS";
    default: return "UNKNOWN";
    }
}

/* Fatal trap handler - called when we can't handle the exception */
__attribute__((section(".text.boot")))
void fatal_trap(trap_frame_t *frame) {
    /* Ensure terminal is visible for fatal diagnostics. */
    (*(volatile unsigned int *)0x4000000C) = 0;

    /* term_printf can itself trap (misaligned access), so snapshot first.
     * Nested traps reuse the same trap-frame slot at top of BRAM stack. */
    trap_frame_t snap = *frame;
    unsigned int dbg_stage = pq_dbg_stage;
    unsigned int dbg_info = pq_dbg_info;
    unsigned int handled = misaligned_count;

    term_printf("\n!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    term_printf("!!! CPU TRAP OCCURRED !!!\n");
    term_printf("!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    term_printf("mcause: 0x%08x (%s)\n", snap.mcause, mcause_name(snap.mcause));
    term_printf("mepc:   0x%08x\n", snap.mepc);
    term_printf("mtval:  0x%08x\n", snap.mtval);
    term_printf("sp:     0x%08x\n", snap.regs[2]);
    term_printf("ra:     0x%08x\n", snap.regs[1]);
    term_printf("a0:     0x%08x\n", snap.regs[10]);
    term_printf("a1:     0x%08x\n", snap.regs[11]);
    term_printf("s0:     0x%08x\n", snap.regs[8]);
    term_printf("dbg_stage: 0x%08x\n", dbg_stage);
    term_printf("dbg_info:  0x%08x\n", dbg_info);
    term_printf("traps handled: %d\n", handled);

    if (addr_valid(snap.mepc, 4)) {
        unsigned int instr = read_instr_at(snap.mepc);
        term_printf("instr@mepc: 0x%08x\n", instr);
        /* Show if compressed or 32-bit */
        if ((instr & 0x3) != 0x3)
            term_printf("  (RVC 16-bit: 0x%04x)\n", instr & 0xFFFF);
    }

    term_printf("!!!!!!!!!!!!!!!!!!!!!!!!!\n");

    while (1) {}
}
