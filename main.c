/*
 * nsuni_unlock - makes boss/NPC-only characters selectable in
 * Naruto Shippuden: Ultimate Ninja Impact (ULUS10582, v1.00).
 *
 * Adds a 6th tab to the character select screen holding the hidden boss
 * forms (with their own select art) and the boss characters that have no
 * select art at all (shown with the game's own "???" art). Addresses and the
 * reasoning behind each patch are in notes/ulus10582_re.md.
 *
 * Structure follows cello0314/nipvp: a kernel module that patches the game
 * module ("Model") right away under PPSSPP, and from a start-module handler
 * on real hardware.
 */
#include <pspsdk.h>
#include <pspkernel.h>
#include <psputilsforkernel.h>
#include <systemctrl.h>
#include <string.h>

#define MODULE_NAME "nsuni_unlock"
PSP_MODULE_INFO(MODULE_NAME, 0x1007, 1, 1);

#define EMULATOR_DEVCTL__IS_EMULATOR 0x03

#define REF(x) (*(volatile u32 *)(x))
#define NOP 0x00000000

/* MIPS encoders for the relocated table references. */
#define LUI(rt, addr) (0x3C000000 | ((rt) << 16) | ((((u32)(addr) + 0x8000) >> 16) & 0xFFFF))
#define ADDIU(rt, rs, addr) (0x24000000 | ((rs) << 21) | ((rt) << 16) | ((u32)(addr) & 0xFFFF))

/* Original tables in the game module. */
#define ORIG_RECORDS 0x08B88B14  /* 0x20 select records, 0x28 bytes each */
#define ORIG_PAGES   0x08B8907C  /* 5 x {u32 count, u32 *select_ids} */
#define REC_SIZE     0x28
#define ORIG_NREC    0x20
#define ORIG_NPAGE   5

/* Select record fields. */
#define REC_PORTRAIT 0x00  /* index into the select screen's portrait/figure packs */
#define REC_NAMEFRM  0x04  /* frame of the name-logo atlas */
#define REC_BATTLE   0x0C  /* battle character ID, written to 0x08C069C4 */
#define REC_VARIANT  0x10  /* returned by z_un_089df2a0 for select IDs >= 0x1A */
#define REC_VOICE1   0x14  /* confirm voice clips */
#define REC_VOICE2   0x18

/*
 * NOTE: select IDs and battle IDs are separate namespaces. SEL_* values index
 * the select record table; BATTLE_* values are the ID the battle loads.
 * They happen to coincide numerically in places (e.g. 0x1F), but never mix them.
 */
#define SEL_PLACEHOLDER 0x1F  /* "???" record: silhouette art, name frame "???" */
#define SEL_PAIN_TENDO  0x17
#define SEL_MADARA      0x18

/*
 * Tobi = Madara's battle ID (0x21) with variant 1. The CWCheat list also applies
 * ~15 patches via a code cave at 0x08800400 for him; those addresses are not in the
 * notes, so this entry is UNTESTED and may crash or load wrong. Set to 0 to drop it.
 */
#define ENABLE_TOBI 1

/* New select records, appended after the original 0x20. */
struct new_char {
    u32 battle_id;   /* battle namespace */
    u32 voice_from;  /* select namespace: record whose confirm voice to reuse */
    u32 art_from;    /* select namespace: record whose portrait + name logo to reuse */
    u32 variant;     /* written to record +0x10 (omitted = 0) */
};

static const struct new_char new_chars[] = {
    { 0x28, SEL_PLACEHOLDER, SEL_PLACEHOLDER },  /* select 0x20 Orochimaru */
    { 0x29, SEL_PLACEHOLDER, SEL_PLACEHOLDER },  /* select 0x21 Onoki (Tsuchikage) */
    { 0x2A, SEL_PLACEHOLDER, SEL_PLACEHOLDER },  /* select 0x22 Mei Terumi (Mizukage) */
    { 0x1A, SEL_PAIN_TENDO, SEL_PAIN_TENDO },   /* select 0x23 Pain (Shurado) */
    { 0x1B, SEL_PAIN_TENDO, SEL_PAIN_TENDO },   /* select 0x24 Pain (Ningendo) */
    { 0x1C, SEL_PAIN_TENDO, SEL_PAIN_TENDO },   /* select 0x25 Pain (Jigokudo) */
    { 0x1D, SEL_PAIN_TENDO, SEL_PAIN_TENDO },   /* select 0x26 Pain (Gakido) */
    { 0x1E, SEL_PAIN_TENDO, SEL_PAIN_TENDO },   /* select 0x27 Pain (Chikushodo) */
    { 0x1F, SEL_PAIN_TENDO, SEL_PAIN_TENDO },   /* select 0x28 Pain (Chikushodo, female) */
#if ENABLE_TOBI
    { 0x21, SEL_MADARA, SEL_MADARA, 1 },    /* select 0x29 Tobi (Madara battle ID, variant 1) */
#endif
};
#define NUM_NEW (sizeof(new_chars) / sizeof(new_chars[0]))
#define NREC (ORIG_NREC + NUM_NEW)

/*
 * Extra tabs (select IDs), shown after the original five, PER_TAB characters
 * each (the last tab holds the remainder). Hidden forms that already have
 * select art come first, then the new records.
 * Naruto 9-Tails 4th Tail Mode (select 0x02) is left out: its load hangs.
 */
#define PER_TAB 5

static const u32 extra_ids[] = {
    0x1A,  /* Killer Bee (8-Tail's Cloak Mode), battle 0x25 */
    0x1B,  /* Killer Bee (Version 2 Mode), battle 0x26 */
    0x0D,  /* Jiraiya (Sage Mode), battle 0x0B */
    0x1E,  /* Naruto Uzumaki (9-Tails: 6th Tail Mode), battle 0x03 */
    0x20, 0x21, 0x22,
    0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
#if ENABLE_TOBI
    0x29,  /* Tobi */
#endif
};
#define NUM_EXTRA (sizeof(extra_ids) / sizeof(extra_ids[0]))
#define NUM_EXTRA_TABS ((NUM_EXTRA + PER_TAB - 1) / PER_TAB)
#define NPAGE_TOTAL (ORIG_NPAGE + NUM_EXTRA_TABS)

/*
 * How a patch's value is produced. For the relocated kinds the register
 * operands are decoded from the original instruction, so each entry is
 * independent and the table order does not matter.
 */
enum patch_kind {
    PK_FIXED,       /* use .value as is */
    PK_PAGES_LUI,   /* lui   rt, hi(new page table) */
    PK_PAGES_ADDIU, /* addiu rt, rs, lo(new page table) */
    PK_REC_LUI,     /* lui   rt, hi(new record table) */
    PK_REC_ADDIU,   /* addiu rt, rs, lo(new record table) */
};

struct patch {
    u32 addr;
    u32 orig;
    u32 value;  /* only for PK_FIXED */
    enum patch_kind kind;
};

#define P_FIXED(a, o, v) { (a), (o), (v), PK_FIXED }
#define P_PAGES_LUI(a, o) { (a), (o), 0, PK_PAGES_LUI }
#define P_PAGES_ADDIU(a, o) { (a), (o), 0, PK_PAGES_ADDIU }
#define P_REC_LUI(a, o) { (a), (o), 0, PK_REC_LUI }
#define P_REC_ADDIU(a, o) { (a), (o), 0, PK_REC_ADDIU }

static const struct patch patches[] = {
    /* select init (0x089DD8E8): page table pointer and page count */
    P_PAGES_LUI(0x089DD9F0, 0x3C1308B9),    /* lui s3,0x8B9 */
    P_PAGES_ADDIU(0x089DDA00, 0x2673907C),  /* addiu s3,s3,-0x6F84 */
    P_FIXED(0x089DD9A4, 0x34050005, 0x34050000 | NPAGE_TOTAL),  /* li a1,5 -> total pages */

    /* record table references */
    P_REC_LUI(0x089DC260, 0x3C0508B9), P_REC_ADDIU(0x089DC264, 0x24A58B14),  /* +0x0C getter */
    P_REC_LUI(0x089DC280, 0x3C0508B9), P_REC_ADDIU(0x089DC284, 0x24A58B14),  /* +0x00 getter */
    P_REC_LUI(0x089DC2A0, 0x3C0508B9), P_REC_ADDIU(0x089DC2A4, 0x24A58B14),  /* +0x04 getter */
    P_REC_LUI(0x089DC398, 0x3C0408B9), P_REC_ADDIU(0x089DC39C, 0x24848B14),  /* confirm voice */
    P_REC_LUI(0x089DC438, 0x3C0508B9), P_REC_ADDIU(0x089DC488, 0x24A58B14),  /* battle -> select search */
    P_REC_LUI(0x089DF2BC, 0x3C0408B9), P_REC_ADDIU(0x089DF2C0, 0x24848B14),  /* variant/unlock state */

    P_FIXED(0x089DC49C, 0x2846001F, 0x28460000 | NREC),  /* slti a2,v0,0x1F -> NREC */

    /* per-slot unlock check in select init: beq v0,zero -> nop */
    P_FIXED(0x089DDB64, 0x1040000A, NOP),

    /* Orochimaru fix (CWCheat list): jal 0x08AE6BD8 -> nop */
    P_FIXED(0x088D8980, 0x0E2B9AF6, NOP),

    /* Mei fix (CWCheat list): Mei's class vtable slot (only Mei uses vtable 0x08BA53E0) */
    P_FIXED(0x08BA54BC, 0x08AF8474, 0x088CC2D4),
};
#define NUM_PATCHES (sizeof(patches) / sizeof(patches[0]))

static int applied;

static int originals_match(void) {
    for (u32 i = 0; i < NUM_PATCHES; i++)
        if (REF(patches[i].addr) != patches[i].orig)
            return 0;
    return REF(ORIG_PAGES) == 5 && REF(ORIG_RECORDS + SEL_PLACEHOLDER * REC_SIZE + REC_BATTLE) == 0x40;
}

static u32 patch_value(const struct patch *p, const u8 *recs, const u32 *pages) {
    u32 rt = (p->orig >> 16) & 0x1F;
    u32 rs = (p->orig >> 21) & 0x1F;

    switch (p->kind) {
    case PK_PAGES_LUI:   return LUI(rt, pages);
    case PK_PAGES_ADDIU: return ADDIU(rt, rs, pages);
    case PK_REC_LUI:     return LUI(rt, recs);
    case PK_REC_ADDIU:   return ADDIU(rt, rs, recs);
    default:             return p->value;
    }
}

static void build_records(u8 *recs) {
    memcpy(recs, (void *)ORIG_RECORDS, ORIG_NREC * REC_SIZE);
    for (u32 i = 0; i < NUM_NEW; i++) {
        u8 *rec = recs + (ORIG_NREC + i) * REC_SIZE;
        const u8 *voice = recs + new_chars[i].voice_from * REC_SIZE;
        const u8 *art = recs + new_chars[i].art_from * REC_SIZE;
        memcpy(rec, recs + SEL_PLACEHOLDER * REC_SIZE, REC_SIZE);
        /* Donor art: portrait index (< 0x20) and name-logo frame of an existing record. */
        *(u32 *)(rec + REC_PORTRAIT) = *(const u32 *)(art + REC_PORTRAIT);
        *(u32 *)(rec + REC_NAMEFRM) = *(const u32 *)(art + REC_NAMEFRM);
        *(u32 *)(rec + REC_BATTLE) = new_chars[i].battle_id;
        *(u32 *)(rec + REC_VARIANT) = new_chars[i].variant;
        *(u32 *)(rec + REC_VOICE1) = *(const u32 *)(voice + REC_VOICE1);
        *(u32 *)(rec + REC_VOICE2) = *(const u32 *)(voice + REC_VOICE2);
    }
}

static void apply_patches(void) {
    if (applied || !originals_match())
        return;

    /*
     * The game reads these tables, so they have to live in user memory.
     * The block is intentionally never freed: the game uses it until exit.
     */
    u32 size = NREC * REC_SIZE + NPAGE_TOTAL * 8 + sizeof(extra_ids);
    SceUID block_id = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER, "nsuni_tables",
                                                    PSP_SMEM_High, size, NULL);
    if (block_id < 0)
        return;
    u8 *block = (u8 *)sceKernelGetBlockHeadAddr(block_id);
    if (!block) {
        sceKernelFreePartitionMemory(block_id);
        return;
    }
    u8 *recs = block;
    u32 *pages = (u32 *)(recs + NREC * REC_SIZE);
    u32 *ids = pages + NPAGE_TOTAL * 2;

    build_records(recs);
    memcpy(pages, (void *)ORIG_PAGES, ORIG_NPAGE * 8);
    memcpy(ids, extra_ids, sizeof(extra_ids));
    for (u32 t = 0; t < NUM_EXTRA_TABS; t++) {
        u32 first = t * PER_TAB;
        u32 left = NUM_EXTRA - first;
        pages[(ORIG_NPAGE + t) * 2] = left < PER_TAB ? left : PER_TAB;
        pages[(ORIG_NPAGE + t) * 2 + 1] = (u32)(ids + first);
    }

    for (u32 i = 0; i < NUM_PATCHES; i++)
        REF(patches[i].addr) = patch_value(&patches[i], recs, pages);

    applied = 1;

    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
}

static STMOD_HANDLER previous;

/* Real hardware: plugins start before the game, so wait for "Model" to load. */
static int OnModuleStart(SceModule *mod) {
    if (strcmp(mod->modname, "Model") == 0)
        apply_patches();
    if (!previous)
        return 0;
    return previous(mod);
}

int module_start(SceSize args, void *argp) {
    int is_emulator = 0;
    sceIoDevctl("kemulator:", EMULATOR_DEVCTL__IS_EMULATOR, NULL, 0, &is_emulator, 4);

    /* PPSSPP starts plugins after the game module is loaded but before it runs. */
    if (is_emulator)
        apply_patches();
    else
        previous = sctrlHENSetStartModuleHandler(OnModuleStart);

    return 0;
}

int module_stop(SceSize args, void *argp) {
    return 0;
}