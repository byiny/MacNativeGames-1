/* Tears of the Kingdom 1.4.3: sharp text (local patch 0026; BOTW's TOTK_FONT_RES, ported to the game's own text code).
   1.4.3 rasterizes every font at its fcpx size for a 1920x1080 layout (Normal 45, NormalS 30, Caption 27, Special 111, ...),
   so a 3456x1944 canvas magnifies the glyphs 1.35x to 1.8x and the edges are soft. With k = start canvas height / 1080 the
   raster becomes k times finer and the drawn text keeps its size, line breaks and typing speed. Design and evidence:
   dev-data/totk/scratch/m2-2026-11-12/re-text/FINDINGS.md. TOTK_FONT_RES=0 keeps the shipped raster; not on iOS.

   Sites (main offsets; each stub is a `bl` from the site, so only x30 and the registers named below change):
   1. 0xc63064 fcvtzs w8, s0 (the font row's size to the ScalableFont's integer size, at font+0x18): S_SIZE returns w8 =
      min(255, round(s0 x k)) and records the shipped size at T[w8] (T on the data page, T[i] = i before). x9 stays.
   2. 0xc63070 ldr w8, [x28, #8] (the row's line feed offset): S_LFO returns round(value x k). x9 stays.
   3. 0xc62398 bl TextureCache::Initialize 0xc65800: S_FACE first scales the copy of the font table that x2 points to (per
      font i < [x2+0x16a18], per face j < [x2+0x16a1c+4i]: border u8 +0x2818+16i+j, bearing x s16 +0x4618+32i+2j, mono width
      u16 +0x4e18+32i+2j, baseline s32 +0x5218+64i+4j; the face metrics are in raster pixels), then branches to Initialize
      with x0-x5 and x30 as they were.
   4. 0x60b248 and 0x60b268 scvtf s0, w0 (the text box's relative size = ratio x the font's GetWidth / GetHeight): S_REL
      gives s0 = w0 x T[fs'] / fs' (fs' = font+0x18), the shipped layout size again. s8 stays (the ratio).
   5. 0x28f7928 scvtf s0, w0 (the typewriter's per-character width): S_TYPE does the same for the font of the typewriter's
      text box ([[x19,#0x28],#0xf0]), so a typed line takes the shipped number of frames. s8 stays.
   6. 0xd3f9c0 movk x8, #0x1000, lsl #32 (the glyph cache's height, 2048x4096 R8): written only for cache_h 8192 / 16384, which
      need a larger "UI" heap (8192 rows emptied it: UIHeap null, crash at the save load). Default: the shipped 4096.
   S_REL and S_TYPE treat a font as scalable when its vtable's GetWidth slot (+0x20) is main+0x10bf438, the function that
   returns font+0x18; any other font (and a size outside 1..511) takes the plain scvtf of the shipped code.
   Stubs use x10-x17 and s1-s7 only. The caller's flags are not live at sites 1 and 2.
   Data page (the page after main, patch_realtime): k as a float at +0x1a0, T at +0x1000 (512 floats). Stubs sit in the
   rewriter's patch area tail after the depth-of-field stubs (file_size + 496), below the VR branches at (data - 64) & ~15.

   The builder (font_raster_build) only encodes words from offsets, so tests can run it without the game
   (dev-data/totk/keep/local-patches/0026-totk-sharp-text-emu.py). */
#pragma once
#include <stdint.h>
#include <string.h>
#ifndef FONT_RASTER_PURE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#endif

enum {
    FR_K_SLOT = 0x1a0,      /* k, a float, on the data page */
    FR_T_SLOT = 0x1000,     /* T[0..511], floats: the shipped size of each raster size */
    FR_T_COUNT = 512,
    FR_STUB_OFF = 496,      /* from (file_size + 15) & ~15: after the depth-of-field stubs */
    FR_WORDS = 144,         /* the stubs' room, in words */
    FR_GETWIDTH = 0x10bf438 /* ScalableFont / Font GetWidth: ldr w0, [x0, #0x18] */
};

static const struct { uint64_t off; uint32_t word; } font_raster_guards[] = {
    {0xc63064, 0x1e380008},   /* fcvtzs w8, s0 (site 1) */
    {0xc63070, 0xb9400b88},   /* ldr w8, [x28, #8] (site 2) */
    {0xc62398, 0x94000d1a},   /* bl 0xc65800 (site 3) */
    {0x60b248, 0x1e220000},   /* scvtf s0, w0 (site 4) */
    {0x60b268, 0x1e220000},   /* scvtf s0, w0 (site 4) */
    {0x28f7928, 0x1e220000},  /* scvtf s0, w0 (site 5) */
    {0xd3f9c0, 0xf2c20008},   /* movk x8, #0x1000, lsl #32 (site 6) */
    {0x10bf438, 0xb9401800},  /* ldr w0, [x0, #0x18]: GetWidth */
    {0x60b22c, 0x37600088},   /* tbnz w8, #12: the relative-size flag */
    {0x28f78bc, 0x1f000820},  /* fmadd s0, s1, s0, s2: the typewriter's budget */
    {0xc6304c, 0xbd400780},   /* ldr s0, [x28, #4]: the size S_SIZE reads in s0 */
};

/* Encoders (only what the stubs use). */
static inline uint32_t fr_adrp(unsigned reg, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}
static inline uint32_t fr_b(uint64_t pc, uint64_t target) { return 0x14000000u | (((uint32_t)(((int64_t)target - (int64_t)pc) >> 2)) & 0x3ffffff); }
static inline uint32_t fr_bcond(unsigned cond, int words) { return 0x54000000u | (((uint32_t)words & 0x7ffff) << 5) | cond; }
static inline uint32_t fr_cbz(unsigned t, int words) { return 0xb4000000u | (((uint32_t)words & 0x7ffff) << 5) | t; }
static inline uint32_t fr_ldr_s(unsigned t, unsigned n, unsigned off) { return 0xbd400000u | ((off / 4) << 10) | (n << 5) | t; }
static inline uint32_t fr_ldr_w(unsigned t, unsigned n, unsigned off) { return 0xb9400000u | ((off / 4) << 10) | (n << 5) | t; }
static inline uint32_t fr_ldr_x(unsigned t, unsigned n, unsigned off) { return 0xf9400000u | ((off / 8) << 10) | (n << 5) | t; }
static inline uint32_t fr_ldrb(unsigned t, unsigned n, unsigned off) { return 0x39400000u | (off << 10) | (n << 5) | t; }
static inline uint32_t fr_strb(unsigned t, unsigned n, unsigned off) { return 0x39000000u | (off << 10) | (n << 5) | t; }
static inline uint32_t fr_ldrh(unsigned t, unsigned n, unsigned off) { return 0x79400000u | ((off / 2) << 10) | (n << 5) | t; }
static inline uint32_t fr_strh(unsigned t, unsigned n, unsigned off) { return 0x79000000u | ((off / 2) << 10) | (n << 5) | t; }
static inline uint32_t fr_ldrsh_w(unsigned t, unsigned n, unsigned off) { return 0x79c00000u | ((off / 2) << 10) | (n << 5) | t; }
static inline uint32_t fr_str_w(unsigned t, unsigned n, unsigned off) { return 0xb9000000u | ((off / 4) << 10) | (n << 5) | t; }
static inline uint32_t fr_ldr_w_idx(unsigned t, unsigned n, unsigned m) { return 0xb8607800u | (m << 16) | (n << 5) | t; }   /* ldr wt, [xn, xm, lsl #2] */
static inline uint32_t fr_ldr_s_idx(unsigned t, unsigned n, unsigned m) { return 0xbc607800u | (m << 16) | (n << 5) | t; }   /* ldr st, [xn, xm, lsl #2] */
static inline uint32_t fr_str_s_uxtw(unsigned t, unsigned n, unsigned m) { return 0xbc205800u | (m << 16) | (n << 5) | t; } /* str st, [xn, wm, uxtw #2] */
static inline uint32_t fr_fmul(unsigned d, unsigned n, unsigned m) { return 0x1e200800u | (m << 16) | (n << 5) | d; }
static inline uint32_t fr_fdiv(unsigned d, unsigned n, unsigned m) { return 0x1e201800u | (m << 16) | (n << 5) | d; }
static inline uint32_t fr_scvtf(unsigned d, unsigned w) { return 0x1e220000u | (w << 5) | d; }
static inline uint32_t fr_fcvtas(unsigned w, unsigned s) { return 0x1e240000u | (s << 5) | w; }
static inline uint32_t fr_fcvtzs(unsigned w, unsigned s) { return 0x1e380000u | (s << 5) | w; }
static inline uint32_t fr_frintz(unsigned d, unsigned n) { return 0x1e25c000u | (n << 5) | d; }
static inline uint32_t fr_movw(unsigned d, unsigned imm) { return 0x52800000u | (imm << 5) | d; }
static inline uint32_t fr_cmpw(unsigned n, unsigned m) { return 0x6b00001fu | (m << 16) | (n << 5); }
static inline uint32_t fr_cmpw_i(unsigned n, unsigned imm) { return 0x7100001fu | (imm << 10) | (n << 5); }
static inline uint32_t fr_cmpx(unsigned n, unsigned m) { return 0xeb00001fu | (m << 16) | (n << 5); }
static inline uint32_t fr_csel(unsigned d, unsigned n, unsigned m, unsigned cond) { return 0x1a800000u | (m << 16) | (cond << 12) | (n << 5) | d; }
static inline uint32_t fr_add_i(unsigned d, unsigned n, unsigned imm, unsigned shift12) { return 0x91000000u | (shift12 << 22) | (imm << 10) | (n << 5) | d; }
static inline uint32_t fr_add_lsl(unsigned d, unsigned n, unsigned m, unsigned s) { return 0x8b000000u | (m << 16) | (s << 10) | (n << 5) | d; }
static inline uint32_t fr_sub_w_i(unsigned d, unsigned n, unsigned imm) { return 0x51000000u | (imm << 10) | (n << 5) | d; }
static inline uint32_t fr_mov_xzr(unsigned d) { return 0xaa1f03e0u | d; }
enum { FR_RET = 0xd65f03c0u, FR_NOP = 0xd503201fu };
enum { FR_EQ = 0, FR_NE = 1, FR_HS = 2, FR_HI = 8, FR_LS = 9, FR_LT = 11, FR_LE = 13 };

typedef struct { unsigned size, lfo, face, type, rel, end; } FontRasterOffsets;   /* byte offsets in the stub area; end = its length */

/* Writes the stubs for a main mapped at base, the stub area at main offset stub, the data page at data. Returns the number
   of words (<= FR_WORDS) and the entry offsets. */
static unsigned font_raster_build(uint32_t *w, uint64_t base, uint64_t stub, uint64_t data, FontRasterOffsets *at) {
    unsigned n = 0;
#define EMIT(x) do { uint32_t emit_ = (x); w[n++] = emit_; } while (0)
#define PC() (base + stub + 4ull * n)
    at->size = n * 4;                                       /* S_SIZE: w8 = min(255, round(s0 x k)); T[w8] = shipped size */
    EMIT(fr_adrp(10, PC(), base + data));
    EMIT(fr_ldr_s(1, 10, FR_K_SLOT));                       /* ldr s1, [x10, #0x1a0]: k */
    EMIT(fr_fmul(2, 0, 1));
    EMIT(fr_fcvtas(8, 2));                                  /* round to nearest, half away from zero */
    EMIT(fr_movw(11, 255));
    EMIT(fr_cmpw(8, 11));
    EMIT(fr_csel(8, 8, 11, FR_LE));                         /* signed: min(w8, 255) */
    EMIT(fr_cmpw_i(8, 1));
    unsigned size_lt = n; EMIT(0);                          /* b.lt orig: a size below 1 keeps the shipped fcvtzs */
    EMIT(fr_frintz(3, 0));                                  /* the shipped size is the truncated one */
    EMIT(fr_add_i(11, 10, 1, 1));                           /* add x11, x10, #0x1000: T */
    EMIT(fr_str_s_uxtw(3, 11, 8));                          /* str s3, [x11, w8, uxtw #2] */
    EMIT(FR_RET);
    w[size_lt] = fr_bcond(FR_LT, (int)(n - size_lt));
    EMIT(fr_fcvtzs(8, 0));
    EMIT(FR_RET);

    at->lfo = n * 4;                                        /* S_LFO: w8 = round([x28,#8] x k) */
    EMIT(fr_ldr_w(8, 28, 8));
    EMIT(fr_adrp(10, PC(), base + data));
    EMIT(fr_ldr_s(1, 10, FR_K_SLOT));
    EMIT(fr_scvtf(2, 8));
    EMIT(fr_fmul(2, 2, 1));
    EMIT(fr_fcvtas(8, 2));
    EMIT(FR_RET);

    at->face = n * 4;                                       /* S_FACE: scale the face metrics at x2, then Initialize */
    EMIT(fr_adrp(10, PC(), base + data));
    EMIT(fr_ldr_s(7, 10, FR_K_SLOT));                       /* s7 = k */
    EMIT(fr_add_i(16, 2, 0x16, 1));                         /* x16 = x2 + 0x16000 */
    EMIT(fr_ldr_w(12, 16, 0xa18));                          /* w12 = the number of fonts */
    EMIT(fr_add_i(11, 16, 0xa1c, 0));                       /* x11 = the faces of each font */
    EMIT(fr_cmpw_i(12, 32));
    unsigned f_hi = n; EMIT(0);                             /* b.hi tail: more than 32 fonts is not the table this expects */
    EMIT(fr_mov_xzr(10));                                   /* i = 0 */
    unsigned f_i = n;
    EMIT(fr_cmpw(10, 12));
    unsigned f_end = n; EMIT(0);                            /* b.hs tail */
    EMIT(fr_ldr_w_idx(13, 11, 10));                         /* w13 = faces of font i */
    EMIT(fr_cmpw_i(13, 16));
    EMIT(fr_movw(14, 16));
    EMIT(fr_csel(13, 13, 14, FR_LS));                       /* at most 16 */
    EMIT(fr_mov_xzr(15));                                   /* j = 0 */
    unsigned f_j = n;
    EMIT(fr_cmpw(15, 13));
    unsigned f_next = n; EMIT(0);                           /* b.hs next font */
    EMIT(fr_add_lsl(16, 2, 10, 4));                         /* border: u8 at x2 + 0x2818 + 16i + j */
    EMIT(fr_add_lsl(16, 16, 15, 0));
    EMIT(fr_add_i(16, 16, 2, 1));                           /* + 0x2000 */
    EMIT(fr_ldrb(17, 16, 0x818));
    EMIT(fr_scvtf(1, 17));
    EMIT(fr_fmul(1, 1, 7));
    EMIT(fr_fcvtas(17, 1));
    EMIT(fr_movw(14, 255));
    EMIT(fr_cmpw(17, 14));
    EMIT(fr_csel(17, 17, 14, FR_LS));
    EMIT(fr_strb(17, 16, 0x818));
    EMIT(fr_add_lsl(14, 2, 10, 5));                         /* x2 + 32i + 2j: bearing x s16 +0x4618, mono width u16 +0x4e18 */
    EMIT(fr_add_lsl(14, 14, 15, 1));
    EMIT(fr_add_i(14, 14, 4, 1));                           /* + 0x4000 */
    EMIT(fr_ldrsh_w(17, 14, 0x618));
    EMIT(fr_scvtf(1, 17));
    EMIT(fr_fmul(1, 1, 7));
    EMIT(fr_fcvtas(17, 1));
    EMIT(fr_strh(17, 14, 0x618));
    EMIT(fr_ldrh(17, 14, 0xe18));
    EMIT(fr_scvtf(1, 17));
    EMIT(fr_fmul(1, 1, 7));
    EMIT(fr_fcvtas(17, 1));
    EMIT(fr_strh(17, 14, 0xe18));
    EMIT(fr_add_lsl(14, 2, 10, 6));                         /* x2 + 64i + 4j: baseline s32 +0x5218 */
    EMIT(fr_add_lsl(14, 14, 15, 2));
    EMIT(fr_add_i(14, 14, 5, 1));                           /* + 0x5000 */
    EMIT(fr_ldr_w(17, 14, 0x218));
    EMIT(fr_scvtf(1, 17));
    EMIT(fr_fmul(1, 1, 7));
    EMIT(fr_fcvtas(17, 1));
    EMIT(fr_str_w(17, 14, 0x218));
    EMIT(fr_add_i(15, 15, 1, 0));                           /* j++ */
    EMIT(fr_b(PC(), base + stub + 4ull * f_j));
    unsigned f_nexti = n;
    EMIT(fr_add_i(10, 10, 1, 0));                           /* i++ */
    EMIT(fr_b(PC(), base + stub + 4ull * f_i));
    unsigned f_tail = n;
    EMIT(fr_b(PC(), base + 0xc65800));                      /* TextureCache::Initialize, x0-x5 and x30 as at the site */
    w[f_hi] = fr_bcond(FR_HI, (int)(f_tail - f_hi));
    w[f_end] = fr_bcond(FR_HS, (int)(f_tail - f_end));
    w[f_next] = fr_bcond(FR_HS, (int)(f_nexti - f_next));

    at->type = n * 4;                                       /* S_TYPE: the typewriter's text box font, then S_REL's body */
    EMIT(fr_ldr_x(10, 19, 0x28));
    EMIT(fr_ldr_x(10, 10, 0xf0));
    unsigned t_b = n; EMIT(0);                              /* b core */
    at->rel = n * 4;                                        /* S_REL: the text box's font */
    EMIT(fr_ldr_x(10, 28, 0xf0));
    unsigned core = n;
    EMIT(fr_scvtf(0, 0));                                   /* the shipped instruction */
    unsigned r_nofont = n; EMIT(0);                         /* cbz x10, done */
    EMIT(fr_ldr_x(11, 10, 0));                              /* the vtable */
    EMIT(fr_ldr_x(11, 11, 0x20));                           /* its GetWidth */
    EMIT(fr_adrp(12, PC(), base + FR_GETWIDTH));
    EMIT(fr_add_i(12, 12, (unsigned)(FR_GETWIDTH & 0xfff), 0));
    EMIT(fr_cmpx(11, 12));
    unsigned r_other = n; EMIT(0);                          /* b.ne done: not a scalable font */
    EMIT(fr_ldr_w(11, 10, 0x18));                           /* fs' */
    EMIT(fr_sub_w_i(12, 11, 1));
    EMIT(fr_cmpw_i(12, FR_T_COUNT - 2));                    /* fs' - 1 > 510: fs' above 511 */
    unsigned r_range = n; EMIT(0);                          /* b.hi done: outside T */
    EMIT(fr_adrp(13, PC(), base + data));
    EMIT(fr_add_i(13, 13, 1, 1));                           /* T */
    EMIT(fr_ldr_s_idx(1, 13, 11));                          /* s1 = T[fs'] */
    EMIT(fr_scvtf(2, 11));
    EMIT(fr_fmul(0, 0, 1));
    EMIT(fr_fdiv(0, 0, 2));                                 /* w0 x T[fs'] / fs': exact for w0 = fs' */
    unsigned done = n;
    EMIT(FR_RET);
    w[t_b] = fr_b(base + stub + 4ull * t_b, base + stub + 4ull * core);
    w[r_nofont] = fr_cbz(10, (int)(done - r_nofont));
    w[r_other] = fr_bcond(FR_NE, (int)(done - r_other));
    w[r_range] = fr_bcond(FR_HI, (int)(done - r_range));
    at->end = n * 4;
#undef EMIT
#undef PC
    return n;
}

#ifndef FONT_RASTER_PURE
/* k: the start canvas height / 1080 (> 1); cache_h: 4096 (as shipped, site 6 not written), 8192 or 16384. Returns 1 when installed. */
static int font_raster_install(Module *m, double k, unsigned cache_h) {
#if TARGET_OS_IPHONE
    (void)m; (void)k; (void)cache_h;
    return 0;
#else
    const uint64_t data = (m->file_size + 0x3fff) & ~0x3fffULL, stub = ((m->file_size + 15) & ~15ULL) + FR_STUB_OFF,
        vr = (data - 64) & ~15ULL;   /* the VR head look's two branches (vr_headlook_install) */
    for (unsigned i = 0; i < sizeof font_raster_guards / sizeof *font_raster_guards; i++) {
        uint32_t word;
        memcpy(&word, m->base + font_raster_guards[i].off, 4);
        if (word != font_raster_guards[i].word) {
            fprintf(stderr, "[totk] 1.4.3 font raster: main+%#llx holds %08x, expected %08x; text stays as shipped\n",
                    (unsigned long long)font_raster_guards[i].off, word, font_raster_guards[i].word);
            return 0;
        }
    }
    uint32_t words[FR_WORDS];
    FontRasterOffsets at;
    unsigned n = font_raster_build(words, (uint64_t)(uintptr_t)m->base, stub, data, &at);
    if (stub < m->patch_off || stub + 4ull * n > vr || data + FR_T_SLOT + 4 * FR_T_COUNT > m->size) {
        fprintf(stderr, "[totk] 1.4.3 font raster: no room at main+%#llx (%u words) before the VR branches / data page; text stays as shipped\n",
                (unsigned long long)stub, n);
        return 0;
    }
    for (uint64_t i = 0; i < 4ull * n; i++)
        if (m->base[stub + i]) { fprintf(stderr, "[totk] 1.4.3 font raster: the stub area at main+%#llx is in use; text stays as shipped\n", (unsigned long long)stub); return 0; }
    for (uint64_t i = 0; i < 4; i++) if (m->base[data + FR_K_SLOT + i]) { fprintf(stderr, "[totk] 1.4.3 font raster: data page +%#x is in use; text stays as shipped\n", FR_K_SLOT); return 0; }
    for (uint64_t i = 0; i < 4ull * FR_T_COUNT; i++)
        if (m->base[data + FR_T_SLOT + i]) { fprintf(stderr, "[totk] 1.4.3 font raster: data page +%#x is in use; text stays as shipped\n", FR_T_SLOT); return 0; }
    const uint32_t cache = cache_h == 16384 ? 0xf2c80008u : 0xf2c40008u;   /* movk x8, #0x4000 / #0x2000, lsl #32 */
    float kf = (float)k, table[FR_T_COUNT];
    for (unsigned i = 0; i < FR_T_COUNT; i++) table[i] = (float)i;
    memcpy(m->base + data + FR_T_SLOT, table, sizeof table);
    memcpy(m->base + data + FR_K_SLOT, &kf, 4);
    memcpy(m->base + stub, words, 4ull * n);
    const struct { uint64_t site, entry; } sites[] = {
        {0xc63064, stub + at.size}, {0xc63070, stub + at.lfo}, {0xc62398, stub + at.face},
        {0x60b248, stub + at.rel}, {0x60b268, stub + at.rel}, {0x28f7928, stub + at.type}};
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        uint32_t bl = 0x94000000u | (fr_b(sites[i].site, sites[i].entry) & 0x3ffffff);
        memcpy(m->base + sites[i].site, &bl, 4);
    }
    if (cache_h > 4096) memcpy(m->base + 0xd3f9c0, &cache, 4);
    fprintf(stderr, "[totk] 1.4.3 font raster x%.3f (start canvas %.0f p / 1080; fonts rasterized k times finer, layout, line feeds and typing as shipped), glyph cache 2048x%u%s, stubs main+%#llx (%u words), data page +%#x / +%#x\n",
            k, k * 1080, cache_h > 4096 ? (cache_h == 16384 ? 16384u : 8192u) : 4096u, cache_h > 4096 ? "" : " (as shipped)", (unsigned long long)stub, n, FR_K_SLOT, FR_T_SLOT);
    return 1;
#endif
}
#endif
