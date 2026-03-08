/*
 * sys_pocket.c -- PocketQuake system driver
 * Bare-metal VexRiscv on Analogue Pocket
 *
 * PAK file is read on demand from SD card via APF dataslot_read().
 * The PAK directory is cached in memory at init time; file data is
 * fetched on each Sys_FileRead call.
 */

#include "quakedef.h"
#include <stdarg.h>
#include "../dataslot.h"

/* Not a dedicated server */
qboolean isDedicated = false;
volatile unsigned int pq_dbg_stage = 0;
volatile unsigned int pq_dbg_info = 0;

/* Hardware registers (SYS_CYCLE_LO/HI already defined in libc.h) */
#define CPU_FREQ         100000000  /* clk_cpu currently runs at 100 MHz */

/* On-demand PAK reading via APF dataslot */
#define PAK0_SLOT_ID     1      /* base id1 pak0.pak */
#define PAK1_SLOT_ID     2      /* base id1 pak1.pak */
#define MOD_PAK0_SLOT_ID 3      /* mod pak0.pak */
#define MOD_PAK1_SLOT_ID 4      /* mod pak1.pak */
#define MOD_PAK2_SLOT_ID 5      /* mod pak2.pak */
#define MOD_PAK3_SLOT_ID 6      /* mod pak3.pak */
#define MOD_PAK4_SLOT_ID 7      /* mod pak4.pak */
#define PROGS_SLOT_ID    8      /* loose progs.dat */
#define PAK_MAX_SIZE     (48 * 1024 * 1024)  /* 48MB max */

/* Game mode sysreg (captured by FPGA from instance memory_writes at 0xF0) */
#define SYSREG_GAME_MODE   (*(volatile uint32_t *)0x40000098)
/* DMA_BUFFER and DMA_CHUNK_SIZE defined in dataslot.h */

/* PAK file structure */
#define PAK_HEADER_MAGIC  (('P') | ('A' << 8) | ('C' << 16) | ('K' << 24))

typedef struct {
    int ident;
    int dirofs;
    int dirlen;
} pakheader_t;

typedef struct {
    char name[56];
    int filepos;
    int filelen;
} pakfile_t;

#define MAX_PAK_FILES 2048

static pakfile_t pak_dir_cache[MAX_PAK_FILES];  /* cached PAK directory (BSS/SDRAM) */
static pakfile_t *pak_dir = pak_dir_cache;
static int pak_numfiles;
static int pak_total_size;  /* dirofs + dirlen, returned by Sys_FileOpenRead */
static int pak_initialized = 0;

/* (DMA buffer at fixed SDRAM address DMA_BUFFER, read via SDRAM_UNCACHED) */

/* Terminal printf for error reporting */
extern void term_printf(const char *fmt, ...);

/* Volatile-safe copy from uncached SDRAM DMA buffer to dest.
 * Ensures each word is actually read from hardware (not optimized by LTO). */
static void dma_copy(void *dest, int count);

static void Pak_Init(void)
{
    pakheader_t hdr;
    int rc;

    if (pak_initialized)
        return;

    /* DMA PAK header into SDRAM, then read via uncacheable alias to
     * bypass D-cache (bridge DMA writes bypass cache). */
    {
        /* Write sentinels via UNCACHED alias to avoid creating dirty D-cache
         * lines that could be evicted over DMA'd data. */
        volatile unsigned int *buf = (volatile unsigned int *)SDRAM_UNCACHED(DMA_BUFFER);
        for (int i = 0; i < 16; i++)
            buf[i] = 0xBAAD0000 | i;

        /* Verify sentinels via uncached alias */
        volatile unsigned int *uc = (volatile unsigned int *)SDRAM_UNCACHED(DMA_BUFFER);

        /* DMA 64 bytes to read PAK header */
        rc = dataslot_read(PAK0_SLOT_ID, 0, (void *)DMA_BUFFER, 64);

        if (rc != 0) {
            pak_numfiles = 0;
            pak_initialized = 1;
            return;
        }
        hdr.ident = uc[0];
        hdr.dirofs = uc[1];
        hdr.dirlen = uc[2];
    }

    if (hdr.ident != PAK_HEADER_MAGIC) {
        pak_numfiles = 0;
        pak_initialized = 1;
        return;
    }

    pak_total_size = hdr.dirofs + hdr.dirlen;
    pak_numfiles = hdr.dirlen / sizeof(pakfile_t);
    if (pak_numfiles > MAX_PAK_FILES)
        pak_numfiles = MAX_PAK_FILES;

    /* Read PAK directory: DMA to SDRAM, volatile copy from uncacheable alias to BSS. */
    {
        int dir_bytes = pak_numfiles * sizeof(pakfile_t);
        int done = 0;
        while (done < dir_bytes) {
            int chunk = dir_bytes - done;
            if (chunk > DMA_CHUNK_SIZE)
                chunk = DMA_CHUNK_SIZE;
            rc = dataslot_read(PAK0_SLOT_ID, hdr.dirofs + done,
                               (void *)DMA_BUFFER, chunk);
            if (rc != 0)
                break;
            dma_copy((byte *)pak_dir_cache + done, chunk);
            done += chunk;
        }
    }
    if (rc != 0) {
        pak_numfiles = 0;
        pak_initialized = 1;
        return;
    }

    pak_initialized = 1;
}

/* Find a file in the PAK */
static int Pak_FindFile(const char *path, int *offset, int *length)
{
    int i;
    Pak_Init();

    for (i = 0; i < pak_numfiles; i++) {
        if (Q_strcasecmp(pak_dir[i].name, path) == 0) {
            *offset = pak_dir[i].filepos;
            *length = pak_dir[i].filelen;
            return 1;
        }
    }
    return 0;
}

/*
===============================================================================
FILE IO
===============================================================================
*/

#define MAX_HANDLES 10

typedef struct {
    int used;
    unsigned char *data;  /* NULL = on-demand PAK via dataslot_read */
    int length;
    int position;
    int slot_id;          /* dataslot ID for on-demand reads */
} syshandle_t;

static syshandle_t sys_handles[MAX_HANDLES];

static int findhandle(void)
{
    int i;
    for (i = 1; i < MAX_HANDLES; i++)
        if (!sys_handles[i].used)
            return i;
    Sys_Error("out of handles");
    return -1;
}

int filelength(FILE *f)
{
    int pos, end;
    pos = ftell(f);
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end;
}

int Sys_FileOpenRead(char *path, int *hndl)
{
    int i;

    i = findhandle();

    /* Intercept requests for pak files — return an on-demand handle.
     * Base id1 directory: pak0→1, pak1→2
     * Mod directory:      pak0→3, pak1→4, pak2→5, pak3→6
     * Detect mod directory: path does NOT contain "/id1/". */
    {
        const char *p = path;
        int slot_id = -1;
        int is_mod_dir = (strstr(path, "/id1/") == NULL && strstr(path, "./id1/") == NULL);
        while (*p) {
            if (p[0]=='p'&&p[1]=='a'&&p[2]=='k'&&
                p[4]=='.'&&p[5]=='p'&&p[6]=='a'&&p[7]=='k') {
                if (is_mod_dir) {
                    if (p[3] == '0') { slot_id = MOD_PAK0_SLOT_ID; break; }
                    if (p[3] == '1') { slot_id = MOD_PAK1_SLOT_ID; break; }
                    if (p[3] == '2') { slot_id = MOD_PAK2_SLOT_ID; break; }
                    if (p[3] == '3') { slot_id = MOD_PAK3_SLOT_ID; break; }
                    if (p[3] == '4') { slot_id = MOD_PAK4_SLOT_ID; break; }
                } else {
                    if (p[3] == '0') { slot_id = PAK0_SLOT_ID; break; }
                    if (p[3] == '1') { slot_id = PAK1_SLOT_ID; break; }
                }
            }
            p++;
        }
        if (slot_id >= 0) {
            /* Probe slot: try reading PAK header to verify file exists.
             * If the slot has no file, dataslot_read returns an error
             * and we fall through to "not found" instead of crashing
             * in COM_LoadPackFile with a bad header. */
            int rc = dataslot_read(slot_id, 0, (void *)DMA_BUFFER, 12);
            if (rc != 0) {
                *hndl = -1;
                return -1;
            }
            sys_handles[i].used = 1;
            sys_handles[i].data = NULL;  /* on-demand: no memory-mapped data */
            sys_handles[i].length = PAK_MAX_SIZE;
            sys_handles[i].position = 0;
            sys_handles[i].slot_id = slot_id;
            *hndl = i;
            return PAK_MAX_SIZE;
        }
    }

    /* Intercept loose progs.dat — load via dataslot */
    {
        int len = strlen(path);
        if (len >= 9 && strcmp(path + len - 9, "progs.dat") == 0) {
            int rc = dataslot_read(PROGS_SLOT_ID, 0, (void *)DMA_BUFFER, sizeof(dprograms_t));
            if (rc == 0) {
                /* Compute file size from progs header: max extent of all sections */
                const dprograms_t *hdr = (const dprograms_t *)SDRAM_UNCACHED(DMA_BUFFER);
                uint32_t fsize = sizeof(dprograms_t);
                uint32_t end;
                end = hdr->ofs_statements + hdr->numstatements * sizeof(dstatement_t);
                if (end > fsize) fsize = end;
                end = hdr->ofs_globaldefs + hdr->numglobaldefs * sizeof(ddef_t);
                if (end > fsize) fsize = end;
                end = hdr->ofs_fielddefs + hdr->numfielddefs * sizeof(ddef_t);
                if (end > fsize) fsize = end;
                end = hdr->ofs_functions + hdr->numfunctions * sizeof(dfunction_t);
                if (end > fsize) fsize = end;
                end = hdr->ofs_strings + hdr->numstrings;
                if (end > fsize) fsize = end;
                end = hdr->ofs_globals + hdr->numglobals * 4;
                if (end > fsize) fsize = end;

                sys_handles[i].used = 1;
                sys_handles[i].data = NULL;
                sys_handles[i].length = fsize;
                sys_handles[i].position = 0;
                sys_handles[i].slot_id = PROGS_SLOT_ID;
                *hndl = i;
                return fsize;
            }
        }
    }

    /* File not found — Quake's COM_FindFile handles PAK contents by
       opening pak files (intercepted above) and seeking to offsets. */
    *hndl = -1;
    return -1;
}

int Sys_FileOpenWrite(char *path)
{
    /* Write not supported on bare metal */
    (void)path;
    return -1;
}

void Sys_FileClose(int handle)
{
    if (handle >= 0 && handle < MAX_HANDLES)
        sys_handles[handle].used = 0;
}

void Sys_FileSeek(int handle, int position)
{
    if (handle >= 0 && handle < MAX_HANDLES && sys_handles[handle].used) {
        sys_handles[handle].position = position;
    }
}

/* DMA transfer statistics for debugging */
static int dma_total_calls = 0;
static int dma_total_errors = 0;
static int dma_stale_hits = 0;

/* Volatile-safe copy from uncached SDRAM alias.
 * Q_memcpy's src parameter is void* (non-volatile), so LTO can optimize
 * reads from the uncached alias into cached/reordered reads. This function
 * ensures each word is actually read from hardware via volatile. */
static void dma_copy(void *dest, int count)
{
    volatile unsigned int *src =
        (volatile unsigned int *)SDRAM_UNCACHED(DMA_BUFFER);
    unsigned int *dst = (unsigned int *)dest;
    int words = count >> 2;
    int i;
    for (i = 0; i < words; i++)
        dst[i] = src[i];
    /* Handle trailing bytes */
    int tail = count & 3;
    if (tail) {
        volatile unsigned char *sb =
            (volatile unsigned char *)SDRAM_UNCACHED(DMA_BUFFER);
        unsigned char *db = (unsigned char *)dest;
        for (i = words * 4; i < count; i++)
            db[i] = sb[i];
    }
}

#define DMA_SENTINEL  0xBAADF00D

int Sys_FileRead(int handle, void *dest, int count)
{
    syshandle_t *h;
    int remaining;

    if (handle < 0 || handle >= MAX_HANDLES)
        return 0;
    h = &sys_handles[handle];
    if (!h->used)
        return 0;

    remaining = h->length - h->position;
    if (count > remaining)
        count = remaining;
    if (count <= 0)
        return 0;

    if (h->data == NULL) {
        /* On-demand PAK read: DMA to SDRAM, copy from uncacheable alias. */
        {
            int done = 0;
            while (done < count) {
                int chunk = count - done;
                if (chunk > DMA_CHUNK_SIZE)
                    chunk = DMA_CHUNK_SIZE;

                dma_total_calls++;

                /* Plant sentinel via UNCACHED alias to avoid creating dirty
                 * D-cache lines that could be evicted over DMA'd data. */
                volatile unsigned int *buf = (volatile unsigned int *)SDRAM_UNCACHED(DMA_BUFFER);
                buf[0] = DMA_SENTINEL;

                int rc = dataslot_read(h->slot_id, h->position + done,
                                       (void *)DMA_BUFFER, chunk);
                if (rc != 0) {
                    dma_total_errors++;
                    term_printf("DMA ERR: rc=%d off=%x len=%x #%d\n",
                                rc, h->position + done, chunk, dma_total_calls);
                    return done;
                }

                /* Check sentinel survived → DMA didn't write (false DONE) */
                volatile unsigned int *uc =
                    (volatile unsigned int *)SDRAM_UNCACHED(DMA_BUFFER);
                if (uc[0] == DMA_SENTINEL) {
                    dma_stale_hits++;
                    if (dma_stale_hits <= 8)
                        term_printf("STALE! off=%x #%d\n",
                                    h->position + done, dma_total_calls);
                    /* Retry */
                    rc = dataslot_read(h->slot_id, h->position + done,
                                       (void *)DMA_BUFFER, chunk);
                    if (rc != 0 || uc[0] == DMA_SENTINEL)
                        return done;
                }

                /* Volatile copy: ensures each word is actually read from
                 * uncached SDRAM, preventing LTO from caching/reordering. */
                dma_copy((byte *)dest + done, chunk);
                done += chunk;
            }
        }
    } else {
        /* Memory-mapped data (not currently used, kept for safety) */
        Q_memcpy(dest, h->data + h->position, count);
    }

    h->position += count;
    return count;
}

/* Print DMA stats — call from Host_Init or similar */
void Sys_PrintDmaStats(void)
{
    Sys_Printf("DMA: %d calls, %d errs, %d stale\n",
               dma_total_calls, dma_total_errors, dma_stale_hits);
}

int Sys_FileWrite(int handle, void *data, int count)
{
    (void)handle;
    (void)data;
    (void)count;
    return 0;
}

/* Save region layout (must match file.c defines) */
#define SAV_REGION_BASE  0x13C00000
#define SAV_SLOT_SIZE    (128 * 1024)
#define SAV_MAX_SLOTS    10

int Sys_FileTime(char *path)
{
    int offset, length;
    if (Pak_FindFile(path, &offset, &length))
        return 1;

    /* Check config slot for persisted config.cfg (bridge auto-loaded SDRAM).
     * Config is slot 12 (after 12 save slots) in the SDRAM region. */
    int len = strlen(path);
    if (len >= 4 && strcmp(path + len - 4, ".cfg") == 0) {
        uint32_t cfg_addr = SAV_REGION_BASE + SAV_MAX_SLOTS * SAV_SLOT_SIZE;
        uint32_t saved_size = *(volatile uint32_t *)SDRAM_UNCACHED(cfg_addr);
        if (saved_size > 0 && saved_size < SAV_SLOT_SIZE)
            return 1;
    }

    /* Check for loose progs.dat via dataslot probe */
    if (len >= 9 && strcmp(path + len - 9, "progs.dat") == 0) {
        int rc = dataslot_read(PROGS_SLOT_ID, 0, (void *)DMA_BUFFER, 4);
        if (rc == 0)
            return 1;
    }

    return -1;
}

void Sys_mkdir(char *path)
{
    (void)path;
}

/*
===============================================================================
SYSTEM IO
===============================================================================
*/

void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
    (void)startaddr;
    (void)length;
    /* All memory is RWX on bare metal */
}

/* Terminal printf (defined in terminal.c) */
extern void term_printf(const char *fmt, ...);
extern void term_puts(const char *s);
extern void term_putchar(char c);

#define SYS_PRINTF_ENABLE 1

void Sys_Error(char *error, ...)
{
    va_list argptr;
    char buf[256];

    va_start(argptr, error);
    vsprintf(buf, error, argptr);
    va_end(argptr);

    /* Ensure terminal is visible for fatal diagnostics. */
    (*(volatile unsigned int *)0x4000000C) = 0;
    term_printf("Sys_Error: %s\n", buf);

    /* Halt */
    while (1) {}
}

void Sys_Printf(char *fmt, ...)
{
#if !SYS_PRINTF_ENABLE
    (void)fmt;
    return;
#else
    va_list argptr;
    char buf[256];

    va_start(argptr, fmt);
    vsprintf(buf, fmt, argptr);
    va_end(argptr);

    term_puts(buf);
#endif
}

void Sys_Quit(void)
{
    while (1) {}
}

float Sys_FloatTime(void)
{
    static unsigned int initialized = 0;
    static unsigned int last_lo = 0;
    static float accum_seconds = 0.0f;
    unsigned int lo = SYS_CYCLE_LO;

    if (!initialized) {
        initialized = 1;
        last_lo = lo;
        return 0.0;
    }

    /* 32-bit cycle delta naturally handles wrap-around without 64-bit math. */
    accum_seconds += (float)(lo - last_lo) * (1.0f / (float)CPU_FREQ);
    last_lo = lo;
    return accum_seconds;
}

char *Sys_ConsoleInput(void)
{
    return NULL;
}

void Sys_Sleep(void)
{
}

void Sys_SendKeyEvents(void)
{
    IN_SendKeyEvents();
}

void Sys_HighFPPrecision(void)
{
}

void Sys_LowFPPrecision(void)
{
}

/*
===============================================================================
MAIN
===============================================================================
*/

/* Heap symbols from linker */
extern char _heap_start[];
extern char _heap_end[];

/* Game mode: captured by FPGA from instance memory_writes at 0xF0000010-1C.
 * Exposed as sysreg at 0x40000098 (mode) and 0x4000009C-A4 (name words).
 *   0 = base Quake (no extra args)
 *   1 = -game <name> (generic mod, e.g. xmenquake)
 *   2 = -hipnotic (Scourge of Armagon expansion)
 *   3 = -rogue (Dissolution of Eternity expansion)
 */
#define SYSREG_GAME_NAME0  (*(volatile uint32_t *)0x4000009C)
#define SYSREG_GAME_NAME1  (*(volatile uint32_t *)0x400000A0)
#define SYSREG_GAME_NAME2  (*(volatile uint32_t *)0x400000A4)

#define GAME_MODE_GAME     1
#define GAME_MODE_HIPNOTIC 2
#define GAME_MODE_ROGUE    3

/* Game name buffer — filled from sysreg at startup */
static char game_name_buf[16];

/* Static arguments for Quake */
static char *quake_argv_base[]     = { "quake", NULL };
static char *quake_argv_game[]     = { "quake", "-game", game_name_buf, NULL };
static char *quake_argv_hipnotic[] = { "quake", "-hipnotic", NULL };
static char *quake_argv_rogue[]    = { "quake", "-rogue", NULL };

/* External: called from main.c */
void __attribute__((noinline, aligned(4))) quake_main(void)
{
    static quakeparms_t parms;
    float time, oldtime, newtime;

    /* Set up parameters */
    parms.basedir = ".";
    parms.cachedir = NULL;
    /* Read game mode from FPGA sysreg (captured from bridge memory_writes) */
    {
        uint32_t game_mode = SYSREG_GAME_MODE;

        /* Copy game name from sysreg words into buffer */
        {
            uint32_t w0 = SYSREG_GAME_NAME0;
            uint32_t w1 = SYSREG_GAME_NAME1;
            uint32_t w2 = SYSREG_GAME_NAME2;
            Q_memcpy(game_name_buf + 0, &w0, 4);
            Q_memcpy(game_name_buf + 4, &w1, 4);
            Q_memcpy(game_name_buf + 8, &w2, 4);
            game_name_buf[12] = '\0';
        }

        Sys_Printf("Game mode: %d name: '%s'\n", game_mode, game_name_buf);

        switch (game_mode) {
        case GAME_MODE_GAME:
            parms.argc = 3;
            parms.argv = quake_argv_game;
            break;
        case GAME_MODE_HIPNOTIC:
            parms.argc = 2;
            parms.argv = quake_argv_hipnotic;
            break;
        case GAME_MODE_ROGUE:
            parms.argc = 2;
            parms.argv = quake_argv_rogue;
            break;
        default:
            parms.argc = 1;
            parms.argv = quake_argv_base;
            break;
        }
    }

    /* Set up heap - use the linker-defined heap region */
    parms.membase = (void *)_heap_start;
    parms.memsize = (int)(_heap_end - _heap_start);

    /* Initialize Quake engine */
    Host_Init(&parms);

    /* Main loop */
    oldtime = Sys_FloatTime();
    while (1) {
        newtime = Sys_FloatTime();
        time = newtime - oldtime;

        if (time < 0.001)
            continue;
        if (time > 0.1f)
            time = 0.1f;

        Host_Frame(time);
        oldtime = newtime;
    }
}
