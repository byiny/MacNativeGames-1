/* games/totk/runtime/botw_view_patches.h without the game: graphics-lod.txt / graphics-far.txt and the TOTK_LOD_SCALE /
   TOTK_FAR_SCALE parsing, the factors' ranges, the three stubs' words, and the patches on a fake main built here (only the
   shipped words the patches check, and a zeroed text padding and data page, at 1.9.0's offsets; no game code in the
   repository). Each stub is run by a small interpreter of its own instructions, from the site to the place it returns to.
   LOD: K = 1 must give the game's z + bias bit for bit, K = 2 z x 0.5 + bias; render distance: d x m for K = 1, d x m x K
   otherwise; depth of field: a flag of 0 (a page of zeros) is the game's own test, any other flag returns without
   drawing. A word that differs, a padding or data slot that is not empty, a missing data page or a bad factor is refused
   with nothing written, and the stubs do not touch the FOV's, the blend's or each other's bytes.
   1.6.0 (the end of this file): its render distance (getTraverseDist's exit gives d x K, the 5 inline null paths K instead
   of 1.0: K = 1 gives d and 0x3f800000, bit for bit) and depth of field (the entry's cbz), on a fake main at 1.6.0's
   offsets; the stubs keep clear of the sound-link veneer (+0), the camera blend's (+16) and the FOV stubs (+48 .. +180).
   1.6.0's model detail distance (lod160): the 32 sites' five checked words each, one bad word refused with nothing written,
   the 32 stubs (both register forms, s1 / s2 and s5 / s6) run with x16 and sp checked, K = 1 giving the fadd's bits and
   K = 2 z x 0.5 + bias. */
#include "../games/totk/runtime/botw_view_patches.h"
#include <stdio.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The 1.9.0 layout of the app's main: text ends at main+0x15a1c64, the file at main+0x20a7dd4, the data page at main+0x20a8000. */
#define TEXT_END 0x15a1c64u
#define DATA 0x20a8000u
#define IMAGE (DATA + 0x4000u)
#define BASE 0x300000000ull   /* where the fake main is "mapped", for the adrp */
#define ALIGNED 0x15a1c70u    /* the text end, 16-byte aligned */
#define LOD_AT (ALIGNED + 128)    /* in the VR camera veneer's room (+128 .. +200) */
#define DISP_AT (ALIGNED - 12)    /* in the 12 bytes between the text end and the aligned end */
#define DOF_AT (ALIGNED + 884)    /* after the sensor caves (+200 .. +884) */
static const view_patch *const patches[3] = {&view_lod, &view_disp, &view_dof};

static uint8_t *fake_main(void) {
    uint8_t *m = malloc(IMAGE);
    if (!m) { printf("FAIL: no memory\n"); exit(1); }
    for (uint32_t i = 0; i < TEXT_END; i++) m[i] = (uint8_t)(i * 131u + 7u);
    memset(m + TEXT_END, 0, IMAGE - TEXT_END);
    for (unsigned p = 0; p < 3; p++)
        for (unsigned i = 0; i < patches[p]->guards; i++) memcpy(m + patches[p]->guard[i].off, &patches[p]->guard[i].original, 4);
    for (unsigned i = 0; i < FOV_GUARDS; i++) memcpy(m + fov_guard[i].off, &fov_guard[i].original, 4);   /* botw_fov.h's, for with_the_others */
    return m;
}
static uint32_t g_image = IMAGE;   /* the fake main's size: 1.9.0's, or 1.6.0's while v160() runs */
static uint8_t *copy_of(const uint8_t *m) { uint8_t *c = malloc(g_image); if (c) memcpy(c, m, g_image); return c; }
static unsigned bytes_changed(const uint8_t *a, const uint8_t *b) {
    unsigned n = 0;
    for (uint32_t i = 0; i < g_image; i++) n += a[i] != b[i];
    return n;
}
static uint32_t word_at(const uint8_t *m, uint32_t off) { uint32_t w; memcpy(&w, m + off, 4); return w; }
static uint32_t bits_of(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static uint32_t slot_bits(double k, int lod) {
    uint32_t b = 0;
    int ok = lod ? view_lod_bits(k, &b) : view_disp_bits(k, &b);
    CHECK(ok, "%s factor %g converts", lod ? "lod" : "far", k);
    return b;
}
static void set_word(uint8_t *m, uint32_t slot, uint32_t v) { memcpy(m + DATA + slot, &v, 4); }

#define SP_TOP 0x7000u   /* the fake stack: x[31] is sp; the 8 words below SP_TOP are the stubs' to push to */
#define SP_LOW (SP_TOP - 64u)
typedef struct { uint64_t x[32]; float s[32]; uint64_t stack[8]; unsigned nzcv; } Regs;   /* nzcv: bits 3..0 = N Z C V */
/* Runs main from pc until it reaches stop1 or stop2 (before executing it); only the stubs' instructions are known. Returns
   the pc reached, 0 on anything else. */
static uint64_t run(uint8_t *m, Regs *r, uint64_t pc, uint64_t stop1, uint64_t stop2) {
    for (int steps = 0; steps < 20; steps++) {
        if (pc == stop1 || pc == stop2) return pc;
        uint32_t w = word_at(m, (uint32_t)pc);
        unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31, ra = (w >> 10) & 31;
        if ((w & 0xfc000000u) == 0x14000000u) { int32_t imm = (int32_t)(w << 6) >> 6; pc += (int64_t)imm * 4; continue; }   /* b */
        if ((w & 0x9f000000u) == 0x90000000u) {   /* adrp */
            int32_t imm = (int32_t)(((((w >> 5) & 0x7ffffu) << 2) | ((w >> 29) & 3u)) << 11) >> 11;
            r->x[rd] = (((BASE + pc) >> 12) + (int64_t)imm) << 12; pc += 4; continue;
        }
        if ((w & 0xfe000000u) == 0x34000000u) {   /* cbz w / cbnz w */
            int32_t imm = (int32_t)(((w >> 5) & 0x7ffffu) << 13) >> 13;
            int zero = (uint32_t)r->x[rd] == 0, nz = w >> 24 & 1;
            pc += zero != nz ? (int64_t)imm * 4 : 4; continue;
        }
        if ((w & 0xffc00000u) == 0xb9400000u || (w & 0xffc00000u) == 0xbd400000u) {   /* ldr w / ldr s from the data page */
            uint64_t a = r->x[rn] + ((w >> 10) & 0xfff) * 4;
            if (a < BASE || a + 4 > BASE + g_image) return 0;
            if (w >> 26 & 1) memcpy(&r->s[rd], m + (a - BASE), 4); else { uint32_t v; memcpy(&v, m + (a - BASE), 4); r->x[rd] = v; }
            pc += 4; continue;
        }
        if ((w & 0xffe00c00u) == 0xf8000c00u && rn == 31) {   /* str x, [sp, #imm]! */
            r->x[31] += (int64_t)((int32_t)(((w >> 12) & 0x1ffu) << 23) >> 23);
            if (r->x[31] < SP_LOW || r->x[31] >= SP_TOP) return 0;
            r->stack[(r->x[31] - SP_LOW) / 8] = r->x[rd]; pc += 4; continue;
        }
        if ((w & 0xffe00c00u) == 0xf8400400u && rn == 31) {   /* ldr x, [sp], #imm */
            if (r->x[31] < SP_LOW || r->x[31] >= SP_TOP) return 0;
            r->x[rd] = r->stack[(r->x[31] - SP_LOW) / 8]; r->x[31] += (int64_t)((int32_t)(((w >> 12) & 0x1ffu) << 23) >> 23); pc += 4; continue;
        }
        if ((w & 0xffc00000u) == 0xf9400000u) {   /* ldr x, [xn, #imm]: the stack or the data page */
            uint64_t a = r->x[rn] + ((w >> 10) & 0xfff) * 8;
            if (rn == 31) { if (a < SP_LOW || a + 8 > SP_TOP) return 0; r->x[rd] = r->stack[(a - SP_LOW) / 8]; }
            else { if (a < BASE || a + 8 > BASE + g_image) return 0; memcpy(&r->x[rd], m + (a - BASE), 8); }
            pc += 4; continue;
        }
        if ((w & 0xffe0fc1fu) == 0xeb00001fu) {   /* cmp x, x */
            uint64_t a = r->x[rn], b = r->x[rm], d = a - b;
            r->nzcv = (unsigned)(d >> 63) << 3 | (unsigned)(d == 0) << 2 | (unsigned)(a >= b) << 1 | (unsigned)((((a ^ b) & (a ^ d)) >> 63) & 1);
            pc += 4; continue;
        }
        if (w == 0xa9437bfdu) {   /* ldp x29, x30, [sp, #0x30]: getTraverseDist's exit (1.6.0) */
            if (r->x[31] < SP_LOW || r->x[31] + 0x40 > SP_TOP) return 0;
            r->x[29] = r->stack[(r->x[31] + 0x30 - SP_LOW) / 8]; r->x[30] = r->stack[(r->x[31] + 0x38 - SP_LOW) / 8]; pc += 4; continue;
        }
        if ((w & 0xffe01fe0u) == 0x1e201000u && ((w >> 13) & 0xff) == 0x70) { r->s[rd] = 1.0f; pc += 4; continue; }   /* fmov s, #1.0 */
        if ((w & 0xffe0fc1fu) == 0x1e202000u || w == (0x1e202008u | (rn << 5))) {   /* fcmp s, s / fcmp s, #0.0 */
            float a = r->s[rn], b = (w & 8) ? 0.0f : r->s[rm];
            r->nzcv = isnan(a) || isnan(b) ? 3u : a == b ? 6u : a < b ? 8u : 2u; pc += 4; continue;
        }
        if ((w & 0xff000010u) == 0x54000000u && ((w & 15) == 0 || (w & 15) == 12)) {   /* b.eq / b.gt */
            int32_t imm = (int32_t)(((w >> 5) & 0x7ffffu) << 13) >> 13; unsigned n = r->nzcv >> 3 & 1, z = r->nzcv >> 2 & 1, v = r->nzcv & 1;
            int taken = (w & 15) == 0 ? z : !z && n == v;
            pc += taken ? (int64_t)imm * 4 : 4; continue;
        }
        if ((w & 0xffe00000u) == 0x52a00000u) { r->x[rd] = (uint64_t)((w >> 5) & 0xffffu) << 16; pc += 4; continue; }   /* movz w, #imm, lsl #16 */
        if ((w & 0xfffffc00u) == 0x1e270000u) { uint32_t v = (uint32_t)r->x[rn]; memcpy(&r->s[rd], &v, 4); pc += 4; continue; }   /* fmov s, w */
        if ((w & 0xffe08000u) == 0x1f000000u) { r->s[rd] = fmaf(r->s[rn], r->s[rm], r->s[ra]); pc += 4; continue; }   /* fmadd s */
        if ((w & 0xffe0fc00u) == 0x1e200800u) { volatile float p = r->s[rn] * r->s[rm]; r->s[rd] = p; pc += 4; continue; }   /* fmul s */
        printf("unknown word %08x at main+%#llx\n", w, (unsigned long long)pc); return 0;
    }
    return 0;
}
static void poison(Regs *r) {
    memset(r, 0, sizeof *r);
    for (unsigned i = 0; i < 31; i++) r->x[i] = 0x1111000000000000ull + i;
    for (unsigned i = 0; i < 32; i++) r->s[i] = 100.0f + (float)i;
}
/* Every register but the ones in the masks (x, then s) as it was. */
static void kept(const Regs *a, const Regs *b, uint32_t xfree, uint32_t sfree, const char *what) {
    for (unsigned i = 0; i < 32; i++) {
        if (!(xfree >> i & 1)) CHECK(a->x[i] == b->x[i], "%s: x%u kept", what, i);
        if (!(sfree >> i & 1)) CHECK(bits_of(a->s[i]) == bits_of(b->s[i]), "%s: s%u kept", what, i);
    }
}
/* LOD: s1 = z, s2 = bias through the sum at main+0x10575e8; the stub gives s1 at main+0x10575ec. */
static float lod_through(uint8_t *m, float z, float bias) {
    Regs r, before; poison(&r); r.s[1] = z; r.s[2] = bias; before = r;
    if (run(m, &r, view_lod.site, VIEW_LOD_BACK, VIEW_LOD_BACK) != VIEW_LOD_BACK) return NAN;
    kept(&r, &before, 1u << 11, 1u << 1 | 1u << 3, "lod");
    return r.s[1];
}
/* Render distance: s0 = d, s1 = m through the multiply at main+0x10a4688; s0 at main+0x10a468c. ret is the return
   address the function saved at [sp, #8] (its frame: stp x29, x30, [sp, #-0x10]!). */
static float disp_from(uint8_t *m, float d, float mul, uint64_t ret) {
    Regs r, before; poison(&r); r.s[0] = d; r.s[1] = mul; r.x[31] = SP_TOP - 16; r.stack[(SP_TOP - 8 - SP_LOW) / 8] = ret; before = r;
    if (run(m, &r, view_disp.site, VIEW_DISP_BACK, VIEW_DISP_BACK) != VIEW_DISP_BACK) return NAN;
    kept(&r, &before, 1u << 8 | 1u << 9 | 1u << 10, 1u << 0 | 1u << 1, "disp");
    return r.s[0];
}
static float disp_through(uint8_t *m, float d, float mul) { return disp_from(m, d, mul, BASE + 0x10a4c58u); }   /* another caller */
/* Depth of field: w9 = the enable byte; 1 = the draw (main+0xfe09f0), 0 = the ret (main+0xfe0a38), -1 = lost. */
static int dof_through(uint8_t *m, uint32_t enable) {
    Regs r, before; poison(&r); r.x[9] = enable; before = r;
    uint64_t pc = run(m, &r, view_dof.site, VIEW_DOF_BACK, VIEW_DOF_RET);
    kept(&r, &before, 1u << 10, 0, "dof");
    return pc == VIEW_DOF_BACK ? 1 : pc == VIEW_DOF_RET ? 0 : -1;
}

static void words(void) {
    CHECK(view_stub_at(&view_lod, TEXT_END) == LOD_AT && view_stub_at(&view_disp, TEXT_END) == DISP_AT && view_stub_at(&view_dof, TEXT_END) == DOF_AT, "the stubs' places");
    /* Render distance (-12 .. +24, 9 words) before the FOV stub (+32), the model detail distance in the VR camera veneer's room
       (+128 .. +200), the depth of field after the sensor caves (+200 .. +884, game_patches_botw.c). */
    CHECK(view_disp.skip == -12 && view_disp.skip + 4 * view_disp.words <= 32, "the render distance stub sits before the FOV stub");
    CHECK(view_lod.skip == 128 && view_lod.skip + 4 * view_lod.words <= 200, "the model detail distance stub sits in the VR camera veneer's room");
    CHECK(view_dof.skip == 200 + 684 && view_dof.skip + 4 * view_dof.words <= 0x390, "the depth of field after the caves, inside the padding of the real layout");
    CHECK(VIEW_LOD_SLOT >= 0x60u && VIEW_DISP_SLOT == VIEW_LOD_SLOT + 4 && VIEW_DOF_SLOT == VIEW_DISP_SLOT + 4, "slots past the FOV's +0x48 / +0x4c");
    for (unsigned p = 0; p < 3; p++) {
        unsigned at = 0;
        for (unsigned i = 0; i < patches[p]->guards; i++) at += patches[p]->guard[i].off == patches[p]->site;
        CHECK(at == 1, "patch %u: the site is a guard", p);
        CHECK(patches[p]->words <= VIEW_MAX_WORDS, "patch %u fits the word buffer", p);
    }
    uint32_t w[VIEW_MAX_WORDS];
    /* As the system assembler gives them (clang -c, otool -t). */
    CHECK(view_lod.build(w, BASE, LOD_AT, DATA) && w[1] == 0xbd406163u && w[2] == 0x1f030821u, "ldr s3, [x11, #0x60]; fmadd s1, s1, s3, s2");
    CHECK(view_disp.build(w, BASE, DISP_AT, DATA) && w[0] == 0x1e210800u && w[2] == 0xf94007e9u && w[3] == 0xf940390au && w[5] == 0x54000060u && w[6] == 0xbd406501u && w[7] == 0x1e210800u, "fmul; the saved return address against +0x70; ldr s1, [x8, #0x64]; fmul");
    CHECK(view_dof.build(w, BASE, DOF_AT, DATA) && w[1] == 0xb940694au && w[2] == 0x3500006au && w[3] == 0x34000049u, "ldr w10, [x10, #0x68]; cbnz; cbz");
    CHECK(view_lod.guard[4].original == 0x1e222821u && view_dof.guard[2].original == 0x34000269u, "the games' fadd s1, s1, s2 and cbz w9");
    CHECK(fov_branch(view_lod.site, LOD_AT) != 0 && fov_branch(view_disp.site, DISP_AT) != 0 && fov_branch(view_dof.site, DOF_AT) != 0, "the branches build");
    CHECK(view_lod.build(w, BASE, 1ull << 40, DATA) == 0 && view_dof.build(w, BASE, DOF_AT, 1ull << 40) == 0, "out of range: 0");
}

static void parsing(void) {
    CHECK(view_lod_parse_file("100") == 1 && view_lod_parse_file("400\n") == 4 && view_lod_parse_file(" 10000 ") == 100 && view_lod_parse_file("150") == 1.5, "lod file");
    CHECK(view_lod_parse_file("max") == VIEW_LOD_MAX && view_lod_parse_file("max\n") == VIEW_LOD_MAX && view_lod_parse_file(" max") == VIEW_LOD_MAX, "lod file: max");
    const char *bad[] = {"", "\n", "99", "10001", "abc", "120x", "nan", "inf", "1 2", "-100", "0"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(view_lod_parse_file(bad[i]) == 0, "lod file \"%s\" refused", bad[i]);
    CHECK(view_lod_parse_env("4") == 4 && view_lod_parse_env("0.1") == 0.1 && view_lod_parse_env("100") == 100 && view_lod_parse_env("max") == VIEW_LOD_MAX, "lod env");
    const char *worse[] = {"", "0", "-1", "0.09", "100.1", "abc", "nan", "2x", "inf"};
    for (unsigned i = 0; i < sizeof worse / sizeof *worse; i++) CHECK(view_lod_parse_env(worse[i]) == 0, "lod env \"%s\" refused", worse[i]);
    CHECK(view_disp_parse_file("100") == 1 && view_disp_parse_file("125\n") == 1.25 && view_disp_parse_file("150") == 1.5 && view_disp_parse_file("10") == 0.1, "far file (percent)");
    const char *far_bad[] = {"", "9", "151", "300", "1000", "abc", "150x", "nan", "inf", "0", "-100"};
    for (unsigned i = 0; i < sizeof far_bad / sizeof *far_bad; i++) CHECK(view_disp_parse_file(far_bad[i]) == 0, "far file \"%s\" refused", far_bad[i]);
    CHECK(view_disp_parse_env("1.5") == 1.5 && view_disp_parse_env("0.1") == 0.1, "far env");
    const char *far_worse[] = {"", "0", "0.09", "1.51", "2", "10", "100", "abc", "max", "nan"};
    for (unsigned i = 0; i < sizeof far_worse / sizeof *far_worse; i++) CHECK(view_disp_parse_env(far_worse[i]) == 0, "far env \"%s\" refused", far_worse[i]);
    uint32_t b = 0x12345678u;
    const double off[] = {0, -1, NAN, INFINITY, 0.09, 1e6 * 1.01};
    for (unsigned i = 0; i < sizeof off / sizeof *off; i++) CHECK(!view_lod_bits(off[i], &b) && b == 0x12345678u, "lod factor %g refused", off[i]);
    CHECK(view_lod_bits(1, &b) && b == bits_of(1.0f) && view_lod_bits(2, &b) && b == bits_of(0.5f) && view_lod_bits(1e6, &b) && b == bits_of((float)1e-6) && view_lod_bits(0.1, &b) && b == bits_of((float)10.0), "lod c = 1/K");
    b = 0x12345678u;
    for (unsigned i = 0; i < 6; i++) CHECK(!view_disp_bits(((double[]){0, -1, NAN, INFINITY, 0.09, 10.1})[i], &b) && b == 0x12345678u, "far factor %u refused", i);
    CHECK(view_disp_bits(1, &b) && b == bits_of(1.0f) && view_disp_bits(1.5, &b) && b == bits_of(1.5f) && !view_disp_bits(2, &b) && view_disp_bits(0.1, &b) && b == bits_of(0.1f), "far K");
}

static void lod_values(uint8_t *m) {
    const float z[] = {-1.0f, -0.001f, -10.5f, -137.25f, -5000.0f, -1e6f, 0.0f, -0.0f, 3.5f, 1e30f, -1e30f, INFINITY, -INFINITY, 1e-30f, 123456.789f};
    const float bias[] = {0.0f, 1.0f, -2.5f, 0.3f, -100.0f, 1e-20f, 7777.7f, -INFINITY};
    for (unsigned i = 0; i < sizeof z / sizeof *z; i++)
        for (unsigned j = 0; j < sizeof bias / sizeof *bias; j++) {
            volatile float game = z[i] + bias[j];   /* the shipped fadd */
            float got = lod_through(m, z[i], bias[j]);
            CHECK(isnan(game) ? isnan(got) : bits_of(got) == bits_of(game), "K = 1: z %g bias %g: %08x, the game's %08x", z[i], bias[j], bits_of(got), bits_of(game));
        }
}
static void patching(void) {
    uint8_t *m = fake_main(), *shipped = copy_of(m);
    uint32_t found = 1; uint32_t w[VIEW_MAX_WORDS];
    /* LOD */
    CHECK(view_lod_apply(m, BASE, TEXT_END, DATA, IMAGE, 1.0, &found) == 0 && found == 1, "lod done");
    view_lod.build(w, BASE, LOD_AT, DATA);
    CHECK(word_at(m, view_lod.site) == fov_branch(view_lod.site, LOD_AT) && !memcmp(m + LOD_AT, w, 4 * view_lod.words), "lod: the branch and the stub");
    CHECK(word_at(m, DATA + VIEW_LOD_SLOT) == bits_of(1.0f), "lod: c = 1");
    CHECK(bytes_changed(m, shipped) <= 4 + 4 * view_lod.words + 4, "lod: nothing else written (%u bytes)", bytes_changed(m, shipped));
    lod_values(m);
    set_word(m, VIEW_LOD_SLOT, slot_bits(2, 1));   /* live: no new install */
    CHECK(lod_through(m, -100.0f, 3.0f) == -100.0f * 0.5f + 3.0f && lod_through(m, -7.0f, -1.0f) == -3.5f - 1.0f, "K = 2: z x 0.5 + bias");
    const double ks[] = {0.1, 1.5, 4, 100, 1e6};
    for (unsigned i = 0; i < 5; i++) {
        set_word(m, VIEW_LOD_SLOT, slot_bits(ks[i], 1));
        float c = (float)(1.0 / ks[i]), got = lod_through(m, -250.0f, 12.0f);
        CHECK(bits_of(got) == bits_of(fmaf(-250.0f, c, 12.0f)), "K = %g", ks[i]);
        CHECK(fabsf(got - (12.0f - 250.0f / (float)ks[i])) <= 1e-3f * fabsf(got) + 1e-4f, "K = %g: z / K + bias (%g)", ks[i], got);
    }
    /* Render distance */
    CHECK(view_disp_apply(m, BASE, TEXT_END, DATA, IMAGE, 1.0, &found) == 0, "disp done");
    view_disp.build(w, BASE, DISP_AT, DATA);
    CHECK(word_at(m, view_disp.site) == fov_branch(view_disp.site, DISP_AT) && !memcmp(m + DISP_AT, w, 4 * view_disp.words), "disp: the branch and the stub");
    CHECK(word_at(m, DATA + VIEW_DISP_SLOT) == bits_of(1.0f), "disp: K = 1");
    {   uint64_t keep; memcpy(&keep, m + DATA + VIEW_DISP_KEEP_SLOT, 8); CHECK(keep == BASE + VIEW_DISP_KEEP_RET, "disp: the kept caller's return address on the page"); }
    const float d[] = {100.0f, 0.0f, 37.5f, 1234.567f, 1e-30f, 3e38f, INFINITY, 0.3f}, mul[] = {1.0f, 2.0f, 0.5f, 3.3f, 0.0f};
    for (unsigned i = 0; i < sizeof d / sizeof *d; i++)
        for (unsigned j = 0; j < sizeof mul / sizeof *mul; j++) {
            volatile float game = d[i] * mul[j];
            float got = disp_through(m, d[i], mul[j]);
            CHECK(isnan(game) ? isnan(got) : bits_of(got) == bits_of(game), "K = 1: d %g m %g: %08x, the game's %08x", d[i], mul[j], bits_of(got), bits_of(game));
        }
    const double kd[] = {0.1, 1.25, 1.5};
    for (unsigned i = 0; i < 3; i++) {
        set_word(m, VIEW_DISP_SLOT, slot_bits(kd[i], 0));
        volatile float dm = 100.0f * 1.5f; volatile float want = dm * (float)kd[i];
        CHECK(bits_of(disp_through(m, 100.0f, 1.5f)) == bits_of(want), "K = %g: d x m x K", kd[i]);
        CHECK(bits_of(disp_from(m, 100.0f, 1.5f, BASE + VIEW_DISP_KEEP_RET)) == bits_of(dm), "K = %g: the placement check's call keeps d x m", kd[i]);
        CHECK(bits_of(disp_from(m, 100.0f, 1.5f, BASE + VIEW_DISP_KEEP_RET + 4)) == bits_of(want), "K = %g: a return address 4 bytes off is scaled", kd[i]);
    }
    {   /* Above the 1.5 cap: refused, the slot's word as it was. */
        uint32_t b = 0x12345678u; const double over[] = {2, 3, 10};
        for (unsigned i = 0; i < 3; i++) CHECK(!view_disp_bits(over[i], &b) && b == 0x12345678u && view_disp_apply(m, BASE, TEXT_END, DATA, IMAGE, over[i], NULL) == -2, "far factor %g is over the cap", over[i]);
    }
    /* Depth of field */
    CHECK(view_dof_apply(m, BASE, TEXT_END, DATA, IMAGE, 0, &found) == 0, "dof done");
    view_dof.build(w, BASE, DOF_AT, DATA);
    CHECK(word_at(m, view_dof.site) == fov_branch(view_dof.site, DOF_AT) && !memcmp(m + DOF_AT, w, 4 * view_dof.words), "dof: the branch and the stub");
    CHECK(word_at(m, DATA + VIEW_DOF_SLOT) == 0, "dof: on");
    const uint32_t enable[] = {0, 1, 2, 0xff};
    for (unsigned i = 0; i < 4; i++) CHECK(dof_through(m, enable[i]) == (enable[i] != 0), "on: the game's own test for %u", enable[i]);
    for (uint32_t flag = 1; flag < 4; flag++) {
        set_word(m, VIEW_DOF_SLOT, flag);
        for (unsigned i = 0; i < 4; i++) CHECK(dof_through(m, enable[i]) == 0, "off (%u): returns without drawing for %u", flag, enable[i]);
    }
    set_word(m, VIEW_DOF_SLOT, 0);
    CHECK(dof_through(m, 1) == 1, "back on");
    /* The whole: three branches, three stubs, three slots, nothing else. */
    CHECK(bytes_changed(m, shipped) <= 3 * 4 + 4 * (view_lod.words + view_disp.words + view_dof.words) + 3 * 4 + 8, "nothing else written (%u bytes)", bytes_changed(m, shipped));
    /* A second pass refuses at the site (it is a branch now) and writes nothing. */
    uint8_t *again = copy_of(m);
    found = 0;
    CHECK(view_lod_apply(m, BASE, TEXT_END, DATA, IMAGE, 1.0, &found) == 5 && found == fov_branch(view_lod.site, LOD_AT), "lod: a second pass refuses");
    CHECK(view_disp_apply(m, BASE, TEXT_END, DATA, IMAGE, 1.0, NULL) == 8 && view_dof_apply(m, BASE, TEXT_END, DATA, IMAGE, 0, NULL) == 3 && !memcmp(m, again, IMAGE), "disp, dof: and write nothing");
    free(again);
    free(m); free(shipped);
}

static void refusing(void) {
    for (unsigned p = 0; p < 3; p++) {
        const view_patch *v = patches[p];
        for (unsigned bad = 0; bad < v->guards; bad++) {
            uint8_t *t = fake_main();
            uint32_t other = v->guard[bad].original ^ 0x20u; memcpy(t + v->guard[bad].off, &other, 4);
            uint8_t *before = copy_of(t); uint32_t found = 0;
            int r = view_apply(v, t, BASE, TEXT_END, DATA, IMAGE, 0, &found);
            CHECK(r == (int)bad + 1 && found == other, "patch %u guard %u differs: refused there (%d, %08x)", p, bad, r, found);
            CHECK(!memcmp(t, before, IMAGE), "patch %u guard %u differs: nothing written", p, bad);
            free(before); free(t);
        }
        const uint32_t taken[] = {(uint32_t)view_stub_at(v, TEXT_END), (uint32_t)view_stub_at(v, TEXT_END) + 4 * v->words - 1, DATA + v->slot, DATA + v->slot + 3};
        for (unsigned i = 0; i < 4; i++) {
            uint8_t *t = fake_main(); t[taken[i]] = 1;
            uint8_t *before = copy_of(t);
            CHECK(view_apply(v, t, BASE, TEXT_END, DATA, IMAGE, 0, NULL) == -1 && !memcmp(t, before, IMAGE), "patch %u: main+%#x taken", p, taken[i]);
            free(before); free(t);
        }
        uint8_t *t = fake_main(), *before = copy_of(t);
        CHECK(view_apply(v, t, BASE, TEXT_END, DATA, DATA, 0, NULL) == -1 && !memcmp(t, before, IMAGE), "patch %u: no data page", p);
        CHECK(view_apply(v, t, BASE, TEXT_END, DATA + 8, IMAGE, 0, NULL) == -1 && !memcmp(t, before, IMAGE), "patch %u: page not aligned", p);
        CHECK(view_apply(v, t, BASE, 0x15a1ffcu, DATA, IMAGE, 0, NULL) == -1 && !memcmp(t, before, IMAGE), "patch %u: no room", p);
        CHECK(view_apply(v, t, BASE, TEXT_END, DATA, IMAGE, 0, NULL) == 0, "patch %u: and then it goes in", p);
        free(before); free(t);
    }
    uint8_t *t = fake_main(), *before = copy_of(t);
    const double off[] = {0, -1, NAN, 0.09, 1e7};
    for (unsigned i = 0; i < 5; i++) CHECK(view_lod_apply(t, BASE, TEXT_END, DATA, IMAGE, off[i], NULL) == -2 && !memcmp(t, before, IMAGE), "lod factor %g", off[i]);
    const double far_off[] = {0, -1, NAN, 0.09, 11};
    for (unsigned i = 0; i < 5; i++) CHECK(view_disp_apply(t, BASE, TEXT_END, DATA, IMAGE, far_off[i], NULL) == -2 && !memcmp(t, before, IMAGE), "far factor %g", far_off[i]);
    free(before); free(t);
}

/* With botw_fov.h's stub (+32), the camera blend's veneer (+80, 24 bytes), the arrow gravity veneer (+104, 24), the VR camera
   veneer (+128, 72) and the sensor caves (+200, 684 bytes: they cover upstream's heap veneers at +256 and +272) in the
   padding, in either order. */
static void with_the_others(void) {
    uint8_t *a = fake_main(), *b = fake_main();
    uint8_t others[24 + 24 + 72 + 684];   /* blend +80, arrow +104, then +128 .. +200 left empty for the model detail distance (the VR camera veneer's room), caves +200 */ for (unsigned i = 0; i < sizeof others; i++) others[i] = (uint8_t)(0xa0 + i);
    CHECK(fov_apply(a, BASE, TEXT_END, DATA, IMAGE, 1.2, NULL) == 0, "the FOV first");
    memset(others + 48, 0, 72);
    memcpy(a + ALIGNED + 80, others, sizeof others); memcpy(b + ALIGNED + 80, others, sizeof others);
    uint8_t *fov_only = copy_of(a);
    CHECK(view_lod_apply(a, BASE, TEXT_END, DATA, IMAGE, 2.0, NULL) == 0 && view_disp_apply(a, BASE, TEXT_END, DATA, IMAGE, 1.5, NULL) == 0 && view_dof_apply(a, BASE, TEXT_END, DATA, IMAGE, 1, NULL) == 0, "the three after the FOV and the others");
    CHECK(!memcmp(a + ALIGNED + 32, fov_only + ALIGNED + 32, 128 - 32) && !memcmp(a + ALIGNED + 144, fov_only + ALIGNED + 144, 884 - 144) && !memcmp(a + DATA, fov_only + DATA, 0x50), "their bytes and slots untouched");
    CHECK(view_lod_apply(b, BASE, TEXT_END, DATA, IMAGE, 2.0, NULL) == 0 && view_disp_apply(b, BASE, TEXT_END, DATA, IMAGE, 1.5, NULL) == 0 && view_dof_apply(b, BASE, TEXT_END, DATA, IMAGE, 1, NULL) == 0, "the three first");
    CHECK(fov_apply(b, BASE, TEXT_END, DATA, IMAGE, 1.2, NULL) == 0, "the FOV after them");
    CHECK(!memcmp(a, b, IMAGE), "the same main either way");
    CHECK(word_at(a, DATA + VIEW_LOD_SLOT) == bits_of(0.5f) && word_at(a, DATA + VIEW_DISP_SLOT) == bits_of(1.5f) && word_at(a, DATA + VIEW_DOF_SLOT) == 1, "the slots");
    free(a); free(b); free(fov_only);
}

/* ---- Breath of the Wild 1.6.0: text ends at main+0x2129ba4, the app's file (the rewriter's code after the ELF) at
   main+0x2e0e988, the data page at main+0x2e10000; the stubs at the aligned end +960 (depth of field) and +984 (render
   distance), after the 32 model detail distance stubs (+192 .. +960). */
#define T160 0x2129ba4u
#define D160 0x2e10000u
#define I160 (D160 + 0x4000u)
#define A160 0x2129bb0u
#define DOF160_AT (A160 + 960)
#define FAR160_AT (A160 + 984)
#define NULL160(i) (view_far160.guard[4 + 5 * (i) + 3])   /* null path i's fmov sN, #1.0 */
static uint8_t *fake160(void) {
    uint8_t *m = malloc(I160);
    if (!m) { printf("FAIL: no memory\n"); exit(1); }
    for (uint32_t i = 0; i < T160; i++) m[i] = (uint8_t)(i * 131u + 7u);
    memset(m + T160, 0, I160 - T160);
    for (unsigned i = 0; i < view_dof160.guards; i++) memcpy(m + view_dof160.guard[i].off, &view_dof160.guard[i].original, 4);
    for (unsigned i = 0; i < view_far160.guards; i++) memcpy(m + view_far160.guard[i].off, &view_far160.guard[i].original, 4);
    for (unsigned i = 0; i < view_lod160.guards; i++) memcpy(m + view_lod160.guard[i].off, &view_lod160.guard[i].original, 4);
    return m;
}
/* Render distance, site 0: getTraverseDist's exit with d in s0; the stub restores x29 / x30 as the game's ldp does and gives
   s0 at main+0x156203c. Site i (1..5): a null path's fmov; the stub gives sN at site + 4 (the adrp x8). */
static float far160_through(uint8_t *m, unsigned site, float d) {
    Regs r, before; poison(&r); r.x[31] = SP_LOW; r.s[0] = d;
    for (unsigned i = 0; i < 8; i++) r.stack[i] = 0x2222000000000000ull + i;
    before = r; before.x[29] = r.stack[6]; before.x[30] = r.stack[7];
    if (!site) {
        if (run(m, &r, VIEW_TRAV160_SITE, VIEW_TRAV160_BACK, VIEW_TRAV160_BACK) != VIEW_TRAV160_BACK) return NAN;
        kept(&r, &before, 1u << 8 | 1u << 9, 1u << 0 | 1u << 1 | 1u << 2, "far160 exit");
        return r.s[0];
    }
    const view_guard *g = &NULL160(site - 1); const unsigned sn = g->original & 31;
    before = r;
    if (run(m, &r, g->off, g->off + 4, g->off + 4) != g->off + 4) return NAN;
    kept(&r, &before, 1u << 8, 1u << sn, "far160 null");
    return r.s[sn];
}
/* DepthOfField::draw: w8 = the enable byte; 1 = the draw (main+0x14140ec), 0 = the epilogue (main+0x1416508), -1 = lost. */
static int dof160_through(uint8_t *m, uint32_t enable) {
    Regs r, before; poison(&r); r.x[8] = enable; before = r;
    uint64_t pc = run(m, &r, view_dof160.site, VIEW_DOF160_BACK, VIEW_DOF160_RET);
    kept(&r, &before, 1u << 10, 0, "dof160");
    return pc == VIEW_DOF160_BACK ? 1 : pc == VIEW_DOF160_RET ? 0 : -1;
}
static void v160(void) {
    g_image = I160;
    const view_patch *const ps[2] = {&view_far160, &view_dof160};
    uint32_t w[VIEW_MAX_WORDS], found = 1;
    CHECK(view_stub_at(&view_far160, T160) == FAR160_AT && view_stub_at(&view_dof160, T160) == DOF160_AT, "1.6.0: the stubs' places");
    /* Behind the 32 LOD stubs (+192 .. +960), apart from each other, inside the page (aligned end 0x2129bb0, page end 0x212a000). */
    CHECK(view_lod160.skip + 4 * view_lod160.words * view_lod160.sites <= view_dof160.skip && view_dof160.skip + 4 * view_dof160.words <= view_far160.skip &&
          view_far160.skip + 4 * view_far160.words <= 0x450, "1.6.0: the stubs keep apart");
    CHECK(view_far160.slot == VIEW_DISP_SLOT && view_dof160.slot == VIEW_DOF_SLOT, "1.6.0: the 1.9.0 slots");
    for (unsigned p = 0; p < 2; p++) {
        unsigned at = 0;
        for (unsigned i = 0; i < ps[p]->guards; i++) at += ps[p]->guard[i].off == ps[p]->site;
        CHECK(at == 1, "1.6.0 patch %u: the site is a guard", p);
    }
    CHECK(view_dof160.words <= VIEW_MAX_WORDS && view_far160.build == NULL && view_far160.words == VIEW_FAR160_EXIT + 3 * VIEW_FAR160_NULLS && view_far160.sites == 1 + VIEW_FAR160_NULLS, "1.6.0: the words fit; far160 builds its own");
    for (unsigned i = 0; i < VIEW_FAR160_NULLS; i++) {
        const view_guard *g = &NULL160(i);
        CHECK((g->original & ~31u) == 0x1e2e1000u && (view_far160.guard[4 + 5 * i + 1].original & ~31u) == 0xbd431100u &&
              (view_far160.guard[4 + 5 * i + 1].original & 31) == (g->original & 31) && (view_far160.guard[4 + 5 * i + 4].original & 0x9f00001fu) == 0x90000008u,
              "1.6.0 far null %u: fmov sN, #1.0 after ldr sN, [x8, #0x310], before adrp x8", i);
    }
    /* As the system assembler gives them (clang -c, otool -t). */
    CHECK(view_dof160.build(w, BASE, DOF160_AT, D160) && w[1] == 0xb940694au && w[2] == 0x3500006au && w[3] == 0x34000048u, "1.6.0: ldr w10, [x10, #0x68]; cbnz w10; cbz w8");
    CHECK(view_far160.guard[2].original == 0xa9437bfdu && view_dof160.guard[2].original == 0x34012108u, "1.6.0: the game's words");

    /* Render distance: getTraverseDist's exit and the 5 null paths. */
    uint8_t *m = fake160(), *shipped = copy_of(m);
    CHECK(view_far160_apply(m, BASE, T160, D160, I160, 1.0, &found) == 0 && found == 1, "1.6.0 far: done");
    CHECK(word_at(m, VIEW_TRAV160_SITE) == fov_branch(VIEW_TRAV160_SITE, FAR160_AT) && word_at(m, FAR160_AT) == 0xa9437bfdu &&
          word_at(m, FAR160_AT + 4) == fov_adrp(8, BASE + FAR160_AT + 4, BASE + D160) && word_at(m, FAR160_AT + 8) == 0xbd406501u &&
          word_at(m, FAR160_AT + 12) == 0x1e2e1002u && word_at(m, FAR160_AT + 16) == 0x1e222020u && word_at(m, FAR160_AT + 20) == 0x540000c0u &&
          word_at(m, FAR160_AT + 24) == 0x1e202008u && word_at(m, FAR160_AT + 28) == 0x5400006cu && word_at(m, FAR160_AT + 32) == 0x52a85909u &&
          word_at(m, FAR160_AT + 36) == 0x1e270120u && word_at(m, FAR160_AT + 40) == 0x1e210800u &&
          word_at(m, FAR160_AT + 44) == fov_branch(FAR160_AT + 44, VIEW_TRAV160_BACK),
          "1.6.0 far exit: the branch; ldp; adrp x8; ldr s1; fmov s2, #1.0; fcmp s1, s2; b.eq; fcmp s0, #0; b.gt; 100.0; fmul; b back");
    CHECK(0x42c80000u == bits_of(100.0f), "1.6.0 far: 0x42c80000 is the callers' 100.0");
    for (unsigned i = 0; i < VIEW_FAR160_NULLS; i++) {
        const view_guard *g = &NULL160(i); const uint32_t at = FAR160_AT + 4 * VIEW_FAR160_EXIT + 12 * i;
        CHECK(word_at(m, g->off) == fov_branch(g->off, at) && word_at(m, at) == fov_adrp(8, BASE + at, BASE + D160) &&
              word_at(m, at + 4) == (0xbd406500u | (g->original & 31)) && word_at(m, at + 8) == fov_branch(at + 8, g->off + 4),
              "1.6.0 far null %u: the branch; adrp x8; ldr sN, [x8, #0x64]; b site + 4", i);
    }
    CHECK(word_at(m, D160 + VIEW_DISP_SLOT) == 0x3f800000u, "1.6.0 far: K = 1");
    {   /* Only the slot, the stubs and the 6 site words changed. */
        unsigned other = 0;
        for (uint32_t o = 0; o < I160; o++) {
            int allowed = (o >= D160 + VIEW_DISP_SLOT && o < D160 + VIEW_DISP_SLOT + 4) || (o >= FAR160_AT && o < FAR160_AT + 4 * view_far160.words) ||
                          (o >= VIEW_TRAV160_SITE && o < VIEW_TRAV160_SITE + 4);
            for (unsigned i = 0; i < VIEW_FAR160_NULLS; i++) allowed |= o >= NULL160(i).off && o < NULL160(i).off + 4;
            other += !allowed && m[o] != shipped[o];
        }
        CHECK(other == 0, "1.6.0 far: nothing else written (%u bytes)", other);
    }
    /* K = 1: the game's d (a multiply by 1.0) and the game's 1.0, bit for bit. */
    const float d[] = {100.0f, 0.0f, -0.0f, -1.0f, -250.0f, -INFINITY, 1.0f, 37.5f, 1e-30f, 123456.789f, 1e30f, 3.4e38f, INFINITY, 1e-45f};   /* 0 and < 0 stay: the callers' own fallback */
    for (unsigned i = 0; i < sizeof d / sizeof *d; i++)
        CHECK(bits_of(far160_through(m, 0, d[i])) == bits_of(d[i]), "1.6.0 far exit, K = 1: d %g kept", d[i]);
    for (unsigned i = 1; i <= VIEW_FAR160_NULLS; i++) CHECK(bits_of(far160_through(m, i, 5.0f)) == 0x3f800000u, "1.6.0 far null %u, K = 1: the game's 1.0", i);
    const double ks[] = {0.1, 0.5, 1.1, 1.25, 1.4, 1.5};
    for (unsigned k = 0; k < 6; k++) {
        uint32_t bits; view_disp_bits(ks[k], &bits); memcpy(m + D160 + VIEW_DISP_SLOT, &bits, 4);   /* live: no new install */
        volatile float want = 250.0f * (float)ks[k];
        CHECK(bits_of(far160_through(m, 0, 250.0f)) == bits_of(want), "1.6.0 far exit, K = %g: d x K", ks[k]);
        volatile float hundred = 100.0f * (float)ks[k];   /* 1.9.0's (d > 0 ? d : 100) x K: d <= 0 gives 100 x K, which the callers keep */
        const float dead[] = {0.0f, -0.0f, -1.0f, -250.0f, -1e-30f, -INFINITY};
        for (unsigned i = 0; i < sizeof dead / sizeof *dead; i++)
            CHECK(bits_of(far160_through(m, 0, dead[i])) == bits_of(hundred) && far160_through(m, 0, dead[i]) > 0, "1.6.0 far exit, K = %g: d %g gives 100 x K", ks[k], dead[i]);
        for (unsigned i = 1; i <= VIEW_FAR160_NULLS; i++) CHECK(bits_of(far160_through(m, i, 5.0f)) == bits, "1.6.0 far null %u, K = %g: K", i, ks[k]);
    }
    {   /* The numbers the owner reads: K = 1.5 gives 150 for d <= 0 and 300 for d = 200; K = 1 leaves 0 at 0. */
        uint32_t bits; view_disp_bits(1.5, &bits); memcpy(m + D160 + VIEW_DISP_SLOT, &bits, 4);
        CHECK(far160_through(m, 0, 0.0f) == 150.0f && far160_through(m, 0, -5.0f) == 150.0f && far160_through(m, 0, 200.0f) == 300.0f, "1.6.0 far exit, K = 1.5: 0 and -5 give 150, 200 gives 300");
        CHECK(far160_through(m, 0, NAN) == 150.0f, "1.6.0 far exit, K = 1.5: NaN fails the callers' d > 0 test too, so 150");
        memcpy(m + D160 + VIEW_DISP_SLOT, &(uint32_t){0x3f800000u}, 4);
        CHECK(far160_through(m, 0, 0.0f) == 0.0f && far160_through(m, 0, -5.0f) == -5.0f, "1.6.0 far exit, K = 1: 0 and -5 as the game gives them");
    }
    memcpy(m + D160 + VIEW_DISP_SLOT, &(uint32_t){0x3f800000u}, 4);
    /* Depth of field */
    CHECK(view_dof160_apply(m, BASE, T160, D160, I160, 0, &found) == 0, "1.6.0 dof: done");
    view_dof160.build(w, BASE, DOF160_AT, D160);
    CHECK(word_at(m, view_dof160.site) == fov_branch(view_dof160.site, DOF160_AT) && !memcmp(m + DOF160_AT, w, 4 * view_dof160.words) && word_at(m, D160 + VIEW_DOF_SLOT) == 0, "1.6.0 dof: the branch, the stub, on");
    const uint32_t enable[] = {0, 1, 2, 0xff};
    for (unsigned i = 0; i < 4; i++) CHECK(dof160_through(m, enable[i]) == (enable[i] != 0), "1.6.0 on: the game's own test for %u", enable[i]);
    for (uint32_t flag = 1; flag < 4; flag++) {
        memcpy(m + D160 + VIEW_DOF_SLOT, &flag, 4);
        for (unsigned i = 0; i < 4; i++) CHECK(dof160_through(m, enable[i]) == 0, "1.6.0 off (%u): skips the draw for %u", flag, enable[i]);
    }
    memset(m + D160 + VIEW_DOF_SLOT, 0, 4);
    CHECK(dof160_through(m, 1) == 1, "1.6.0: back on");
    CHECK(bytes_changed(m, shipped) <= 4 * (view_far160.sites + 1) + 4 * (view_far160.words + view_dof160.words) + 4 + 4, "1.6.0: nothing else written (%u bytes)", bytes_changed(m, shipped));
    /* A second pass refuses at the site and writes nothing. */
    uint8_t *again = copy_of(m);
    CHECK(view_far160_apply(m, BASE, T160, D160, I160, 1.0, &found) == 3 && found == fov_branch(VIEW_TRAV160_SITE, FAR160_AT) &&
          view_dof160_apply(m, BASE, T160, D160, I160, 0, NULL) == 3 && !memcmp(m, again, I160), "1.6.0: a second pass refuses and writes nothing");
    free(again); free(m); free(shipped);

    /* Refusing: a differing guard, a taken padding or slot byte, no page, no room, a bad factor. far160 has no build (NULL):
       its own apply, at K = 1. */
#define APPLY160(v, t, te, data, size, f) ((v) == &view_far160 ? view_far160_apply(t, BASE, te, data, size, 1.0, f) : view_apply(v, t, BASE, te, data, size, 0, f))
    for (unsigned p = 0; p < 2; p++) {
        const view_patch *v = ps[p];
        for (unsigned bad = 0; bad < v->guards; bad++) {
            uint8_t *t = fake160(); uint32_t other = v->guard[bad].original ^ 0x20u; memcpy(t + v->guard[bad].off, &other, 4);
            uint8_t *before = copy_of(t); found = 0;
            CHECK(APPLY160(v, t, T160, D160, I160, &found) == (int)bad + 1 && found == other && !memcmp(t, before, I160), "1.6.0 patch %u guard %u differs: refused, nothing written", p, bad);
            free(before); free(t);
        }
        const uint32_t taken[] = {(uint32_t)view_stub_at(v, T160), (uint32_t)view_stub_at(v, T160) + 4 * v->words - 1, D160 + v->slot, D160 + v->slot + 3};
        for (unsigned i = 0; i < 4; i++) {
            uint8_t *t = fake160(); t[taken[i]] = 1; uint8_t *before = copy_of(t);
            CHECK(APPLY160(v, t, T160, D160, I160, NULL) == -1 && !memcmp(t, before, I160), "1.6.0 patch %u: main+%#x taken", p, taken[i]);
            free(before); free(t);
        }
        uint8_t *t = fake160(), *before = copy_of(t);
        CHECK(APPLY160(v, t, T160, D160, D160, NULL) == -1 && APPLY160(v, t, T160, D160 + 8, I160, NULL) == -1 &&
              APPLY160(v, t, 0x2129fe0u, D160, I160, NULL) == -1 && !memcmp(t, before, I160), "1.6.0 patch %u: no page, unaligned page, no room", p);
        CHECK(APPLY160(v, t, T160, D160, I160, NULL) == 0, "1.6.0 patch %u: and then it goes in", p);
        free(before); free(t);
    }
#undef APPLY160
    uint8_t *t = fake160(), *before = copy_of(t);
    const double off[] = {0, -1, NAN, 0.09, 11};
    for (unsigned i = 0; i < 5; i++) CHECK(view_far160_apply(t, BASE, T160, D160, I160, off[i], NULL) == -2 && !memcmp(t, before, I160), "1.6.0 far factor %g", off[i]);
    free(before); free(t);

    /* With the sound-link veneer (+0, 16 bytes) and the camera blend's (+16, 24 bytes), in either order. */
    uint8_t *a = fake160(), *b = fake160(), pad[40]; for (unsigned i = 0; i < sizeof pad; i++) pad[i] = (uint8_t)(0xa0 + i);
    memcpy(a + A160, pad, sizeof pad); memcpy(b + A160, pad, sizeof pad);
    CHECK(view_far160_apply(a, BASE, T160, D160, I160, 1.5, NULL) == 0 && view_dof160_apply(a, BASE, T160, D160, I160, 1, NULL) == 0 && !memcmp(a + A160, pad, sizeof pad), "1.6.0: both after the veneers, theirs untouched");
    CHECK(view_dof160_apply(b, BASE, T160, D160, I160, 1, NULL) == 0 && view_far160_apply(b, BASE, T160, D160, I160, 1.5, NULL) == 0 && !memcmp(a, b, I160), "1.6.0: the same main in the other order");
    CHECK(word_at(a, D160 + VIEW_DISP_SLOT) == bits_of(1.5f) && word_at(a, D160 + VIEW_DOF_SLOT) == 1 && word_at(a, D160 + VIEW_LOD_SLOT) == 0, "1.6.0: the slots; none for the model detail distance");
    free(a); free(b);
    g_image = IMAGE;
}

/* ---- 1.6.0's model detail distance: 32 sites, one 6-word stub each at +192 + 24 i. */
#define LOD160_AT(i) (A160 + 192 + 24 * (i))
#define SITE160(i) (view_lod160.guard[5 * (i) + 2].off)
#define FADD160(i) (view_lod160.guard[5 * (i) + 2].original)
/* Site i: the fadd's first source (sn) = z, second (sm) = bias; the stub must give sd = z x c + bias at site + 4, and leave
   x16, sp, the stack above it and every other register as it was (only s31 is the stub's). */
static float lod160_through(uint8_t *m, unsigned i, float z, float bias) {
    const uint32_t fadd = FADD160(i), site = SITE160(i), rd = fadd & 31, rn = fadd >> 5 & 31, rm = fadd >> 16 & 31;
    Regs r, before; poison(&r); r.x[31] = SP_TOP; r.s[rn] = z; r.s[rm] = bias; before = r;
    if (run(m, &r, site, site + 4, site + 4) != site + 4) return NAN;
    kept(&r, &before, 0, 1u << rd | 1u << 31, "lod160");
    return r.s[rd];
}
static void lod160(void) {
    g_image = I160;
    const view_patch *p = &view_lod160;
    uint32_t found = 1;
    CHECK(p->guards == 5 * VIEW_LOD160_SITES && p->sites == VIEW_LOD160_SITES && p->words == VIEW_LOD160_WORDS && p->slot == VIEW_LOD_SLOT, "1.6.0 lod: 32 sites, 5 words each, 6-word stubs, the LOD slot");
    CHECK(view_stub_at(p, T160) == LOD160_AT(0), "1.6.0 lod: the first stub's place");
    /* After the FOV stubs (+48 .. +180, the 1.6.0 rows' branch), before the depth of field stub (+960), inside the page
       (aligned end 0x2129bb0, page end 0x212a000). */
    CHECK(p->skip >= 180 && p->skip + 4 * p->words * p->sites <= view_dof160.skip, "1.6.0 lod: the stubs fit between the others (%u bytes)", p->skip + 4 * p->words * p->sites);
    for (unsigned i = 0; i < VIEW_LOD160_SITES; i++) {
        const uint32_t fadd = FADD160(i);
        CHECK((fadd & 0xffe0fc00u) == 0x1e202800u && (fadd & 31) == (fadd >> 5 & 31) && ((fadd >> 16 & 31) == (fadd & 31) + 1), "1.6.0 lod site %u: fadd sd, sd, s(d+1)", i);
        CHECK(i == 0 || SITE160(i) >= SITE160(i - 1) + 0x20, "1.6.0 lod site %u: sorted, apart", i);
        CHECK(p->guard[5 * i].off + 8 == SITE160(i) && p->guard[5 * i + 4].off == SITE160(i) + 8, "1.6.0 lod site %u: -8 .. +8", i);
    }
    CHECK(p->site == SITE160(0) && (fov_branch(SITE160(31), LOD160_AT(31)) != 0), "1.6.0 lod: the first site; the last branch builds");

    uint8_t *m = fake160(), *shipped = copy_of(m);
    CHECK(view_lod160_apply(m, BASE, T160, D160, I160, 1.0, &found) == 0 && found == 1, "1.6.0 lod: done");
    CHECK(word_at(m, D160 + VIEW_LOD_SLOT) == bits_of(1.0f), "1.6.0 lod: c = 1");
    /* As the system assembler gives them (clang -c, otool -t): str x16, [sp, #-16]!; ldr s31, [x16, #0x60]; ldr x16, [sp], #16; fmadd. */
    for (unsigned i = 0; i < VIEW_LOD160_SITES; i++) {
        const uint32_t at = LOD160_AT(i), site = SITE160(i);
        CHECK(word_at(m, at) == 0xf81f0ff0u && word_at(m, at + 8) == 0xbd40621fu && word_at(m, at + 12) == 0xf84107f0u, "1.6.0 lod stub %u: x16 saved, s31 loaded, x16 restored", i);
        CHECK(word_at(m, at + 4) == fov_adrp(16, BASE + at + 4, BASE + D160) && word_at(m, at + 20) == fov_branch(at + 20, site + 4) && word_at(m, site) == fov_branch(site, at), "1.6.0 lod stub %u: adrp x16, back to site + 4, the branch", i);
        CHECK(word_at(m, at + 16) == (FADD160(i) == 0x1e222821u ? 0x1f1f0821u : 0x1f1f18a5u), "1.6.0 lod stub %u: fmadd s1, s1, s31, s2 / s5, s5, s31, s6", i);
    }
    CHECK(FADD160(0) == 0x1e222821u && FADD160(2) == 0x1e2628a5u && word_at(m, LOD160_AT(2) + 16) == 0x1f1f18a5u, "1.6.0 lod: both register forms are in the table");
    /* Only the slot, the 32 stubs and the 32 site words changed. */
    unsigned other = 0;
    for (uint32_t o = 0; o < I160; o++) {
        int allowed = (o >= D160 + VIEW_LOD_SLOT && o < D160 + VIEW_LOD_SLOT + 4) || (o >= LOD160_AT(0) && o < LOD160_AT(32));
        for (unsigned i = 0; i < VIEW_LOD160_SITES; i++) allowed |= o >= SITE160(i) && o < SITE160(i) + 4;
        other += !allowed && m[o] != shipped[o];
    }
    CHECK(other == 0, "1.6.0 lod: nothing else written (%u bytes)", other);
    /* K = 1: the game's sum, bit for bit, at every site. */
    const float z[] = {-1.0f, -0.001f, -137.25f, -5000.0f, -1e6f, 0.0f, -0.0f, 3.5f, 1e30f, -1e30f, INFINITY, -INFINITY, 1e-30f, 123456.789f};
    const float bias[] = {0.0f, 1.0f, -2.5f, 0.3f, -100.0f, 1e-20f, 7777.7f, -INFINITY};
    for (unsigned i = 0; i < VIEW_LOD160_SITES; i++)
        for (unsigned a = 0; a < sizeof z / sizeof *z; a++)
            for (unsigned b = 0; b < sizeof bias / sizeof *bias; b++) {
                volatile float game = z[a] + bias[b];
                float got = lod160_through(m, i, z[a], bias[b]);
                CHECK(isnan(game) ? isnan(got) : bits_of(got) == bits_of(game), "1.6.0 lod site %u, K = 1: z %g bias %g: %08x, the game's %08x", i, z[a], bias[b], bits_of(got), bits_of(game));
            }
    /* K = 2 (live, no new install): z x 0.5 + bias; z alone is scaled, never the bias. Other K as the 1.9.0 test. */
    { uint32_t c2 = slot_bits(2, 1); memcpy(m + D160 + VIEW_LOD_SLOT, &c2, 4); }
    for (unsigned i = 0; i < VIEW_LOD160_SITES; i++)
        CHECK(lod160_through(m, i, -100.0f, 3.0f) == -100.0f * 0.5f + 3.0f && lod160_through(m, i, -7.0f, -1.0f) == -3.5f - 1.0f, "1.6.0 lod site %u, K = 2: z x 0.5 + bias", i);
    const double ks[] = {0.1, 1.5, 4, 100, 1e6};
    for (unsigned k = 0; k < 5; k++) {
        { uint32_t ck = slot_bits(ks[k], 1); memcpy(m + D160 + VIEW_LOD_SLOT, &ck, 4); }
        float c = (float)(1.0 / ks[k]);
        for (unsigned n = 0; n < 3; n++) {
            const unsigned i = (unsigned[]){0, 2, 31}[n];
            CHECK(bits_of(lod160_through(m, i, -250.0f, 12.0f)) == bits_of(fmaf(-250.0f, c, 12.0f)), "1.6.0 lod site %u, K = %g", i, ks[k]);
        }
    }
    /* A second pass refuses at the first site (a branch now) and writes nothing. */
    uint8_t *again = copy_of(m); found = 0;
    CHECK(view_lod160_apply(m, BASE, T160, D160, I160, 1.0, &found) == 3 && found == fov_branch(SITE160(0), LOD160_AT(0)) && !memcmp(m, again, I160), "1.6.0 lod: a second pass refuses and writes nothing");
    free(again); free(m);

    /* Refusing: any one of the 160 words different, a taken padding or slot byte, no page, no room, a bad factor. */
    for (unsigned bad = 0; bad < p->guards; bad++) {
        uint8_t *t = fake160(); uint32_t other_word = p->guard[bad].original ^ 0x20u; memcpy(t + p->guard[bad].off, &other_word, 4);
        uint8_t *before = copy_of(t); found = 0;
        CHECK(view_lod160_apply(t, BASE, T160, D160, I160, 1.0, &found) == (int)bad + 1 && found == other_word && !memcmp(t, before, I160), "1.6.0 lod guard %u differs: refused, nothing written", bad);
        free(before); free(t);
    }
    const uint32_t taken[] = {LOD160_AT(0), LOD160_AT(31) + 23, LOD160_AT(17) + 5, D160 + VIEW_LOD_SLOT, D160 + VIEW_LOD_SLOT + 3};
    for (unsigned i = 0; i < 5; i++) {
        uint8_t *t = fake160(); t[taken[i]] = 1; uint8_t *before = copy_of(t);
        CHECK(view_lod160_apply(t, BASE, T160, D160, I160, 1.0, NULL) == -1 && !memcmp(t, before, I160), "1.6.0 lod: main+%#x taken", taken[i]);
        free(before); free(t);
    }
    uint8_t *t = fake160(), *before = copy_of(t);
    CHECK(view_lod160_apply(t, BASE, T160, D160, D160, 1.0, NULL) == -1 && view_lod160_apply(t, BASE, T160, D160 + 8, I160, 1.0, NULL) == -1 &&
          view_lod160_apply(t, BASE, 0x2129c60u, D160, I160, 1.0, NULL) == -1 && !memcmp(t, before, I160), "1.6.0 lod: no page, unaligned page, no room");
    const double off[] = {0, -1, NAN, 0.09, 1e7};
    for (unsigned i = 0; i < 5; i++) CHECK(view_lod160_apply(t, BASE, T160, D160, I160, off[i], NULL) == -2 && !memcmp(t, before, I160), "1.6.0 lod factor %g", off[i]);
    CHECK(view_lod160_apply(t, BASE, T160, D160, I160, 1e6, NULL) == 0 && word_at(t, D160 + VIEW_LOD_SLOT) == bits_of((float)1e-6), "1.6.0 lod: and then it goes in (max)");
    free(before); free(t);

    /* With the veneers (+0, +16) and the other two 1.6.0 stubs, in either order. */
    uint8_t *a = fake160(), *b = fake160(), pad[40]; for (unsigned i = 0; i < sizeof pad; i++) pad[i] = (uint8_t)(0xa0 + i);
    memcpy(a + A160, pad, sizeof pad); memcpy(b + A160, pad, sizeof pad);
    CHECK(view_lod160_apply(a, BASE, T160, D160, I160, 2.0, NULL) == 0 && view_far160_apply(a, BASE, T160, D160, I160, 1.5, NULL) == 0 && view_dof160_apply(a, BASE, T160, D160, I160, 1, NULL) == 0, "1.6.0 lod: all three after the veneers");
    CHECK(view_dof160_apply(b, BASE, T160, D160, I160, 1, NULL) == 0 && view_far160_apply(b, BASE, T160, D160, I160, 1.5, NULL) == 0 && view_lod160_apply(b, BASE, T160, D160, I160, 2.0, NULL) == 0, "1.6.0 lod: the other order");
    CHECK(!memcmp(a, b, I160) && !memcmp(a + A160, pad, sizeof pad), "1.6.0 lod: the same main either way; the veneers untouched");
    CHECK(word_at(a, D160 + VIEW_LOD_SLOT) == bits_of(0.5f) && word_at(a, D160 + VIEW_DISP_SLOT) == bits_of(1.5f) && word_at(a, D160 + VIEW_DOF_SLOT) == 1, "1.6.0 lod: the three slots");
    free(a); free(b); free(shipped);
    g_image = IMAGE;
}

int main(void) {
    words(); parsing(); patching(); refusing(); with_the_others(); v160(); lod160();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("botw view patches ok\n");
    return 0;
}
