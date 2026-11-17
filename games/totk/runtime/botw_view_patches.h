/* Breath of the Wild 1.9.0's model detail distance, render distance and depth of field (the settings panel's effects page,
   graphics-lod.txt / graphics-far.txt / graphics-dof.txt, TOTK_LOD_SCALE / TOTK_FAR_SCALE; Mac only), without the runtime,
   so tests/test_botw_view_patches.c can check them. Each patch is a branch from one game instruction to a stub in main's
   text padding (after the text end, 16-byte aligned, behind the stubs of botw_fov.h and the camera blend), and each
   stub reads its word on the data page after main (+0x60, +0x64, +0x68) at every call, so a change acts at once. Always
   installed on the Mac: at 1x / on the game gets its own result bit for bit. Nothing is written unless every check holds;
   a mismatch leaves the game's code as shipped. The sites of 1, 2 and 3 below are 1.9.0's; 1.6.0's are listed after them.
   1. Model detail distance. gsys::ModelViewInfo::calcLodLevel (main+0x10575a4; x0 the view info, whose +0x18 holds the
      view-space depth z, negative in front; w1 the level count; x2 the model's LOD table: [0] a bias, [2] k; s0 the
      threshold) takes the level from v = z + bias: none while v >= s0, else one level for each 1/k of depth. The sum
      (main+0x10575e8, fadd s1, s1, s2) becomes fmadd s1, s1, s3, s2 with s3 = c = 1/K from the page (+0x60), so the level
      at depth d is the one the game picks at d / K. K = 1: c is 1.0 and the fused sum is the game's, bit for bit.
      "max" (K = 1e6): the level a model has up close. x11 and s3 are free (a leaf; s3 is set before its next use).
   2. Render distance. The camera's far plane is 25000 m, more than the whole map (main+0xd55f6c), so scaling it shows
      nothing; here the object and actor display distance scales instead. ksys::map::Object::getDispDistance
      (main+0x10a4640) returns (distance > 0 ? distance : 100) x ActorDebug::mDispDistanceMultiplier; the multiply
      (main+0x10a4688, fmul s0, s0, s1) gets one more, by K from the page (+0x64): d x m x K, so K = 1 is the game's
      word. The display distance drives the placement's spawn and keep radius (more actors alive: the actor heaps) and
      the _Far model swap. x8 and s1 are free after the multiply, x9 and x10 too (the function returns after it).
      One caller keeps the game's value: the placement check main+0x10a4d20 calls it at main+0x10a6e98 for its "within
      display distance" flag and distance, which only the placement loop main+0x1545618 reads (main+0x15457a8: the
      object's near or far list). Scaled there, the floors of the F-3 Bokoblin camp vanished at 1.5x (2026-11-05, owner's
      save; a caller bisect: only that call). The stub compares the return address the function saved ([sp, #8]) with
      base + 0x10a6e9c on the data page (+0x70, written at install) and skips K for it.
   3. Depth of field. The draw entry of gsys's DepthOfField (main+0xfe09e4; x0 the object, the "enable" byte at +0x1310)
      returns at main+0xfe09ec (cbz w9, main+0xfe0a38, a plain ret) while the byte is 0. The stub also returns while the
      page's flag (+0x68) is not 0: no draw, the pass left out; an event camera or the DOF manager only writes the
      parameters, so cutscenes go on. A page of zeros means "on". x10 is free at the entry.
   Breath of the Wild 1.6.0 (the same pages and slots; tables view_lod160, view_far160 and view_dof160 below):
   2. Render distance. getDispDistance is inlined at 26 places there, each multiplying by ActorDebug's field (+0x310 of
      the instance at main+0x2cc43b0), but the game never makes that instance (its pointer stayed 0 in a whole 1.6.0 run,
      2026-11-04), so every place takes its null path: m = 1.0. 21 places take d from getTraverseDist (main+0x1561f80;
      its only callers, as 1.9.0's twin main+0x10a46d4 has getDispDistance alone); its exit (main+0x1562038, the
      `ldp x29, x30, [sp, #0x30]` that every path reaches with d in s0) gets (d > 0 ? d : 100) x K, K from the page (+0x64):
      the 21 callers apply their own `d > 0 ? d : 100` (100 = [main+0x260e280]) after the call, so a plain d x K would leave
      a 0 at 0 and the game's 100 m at any K. The 5 others compute d inline; their null path's `fmov sN, #1.0` (just before
      an `adrp x8`, so x8 is free) loads K instead. Each place then gets (d > 0 ? d : 100) x K, as 1.9.0's stub gives.
      K = 1: the exit returns d untouched (0 and negative values included; the callers' fallback is the game's) and the
      null paths load 1.0, the game's values bit for bit. x8, x9, s1 and s2 are free at the exit (no caller reads them
      before it writes them again; x9 and s2 are the stub's scratch).
   3. Depth of field. The draw entry of gsys's DepthOfField (main+0xfe09e4; x0 the object, the "enable" byte at +0x1310)
      returns at main+0xfe09ec (cbz w9, main+0xfe0a38, a plain ret) while the byte is 0. The stub also returns while the
      page's flag (+0x68) is not 0: no draw, the pass left out; an event camera or the DOF manager only writes the
      parameters, so cutscenes go on. A page of zeros means "on". x10 is free at the entry.
   Breath of the Wild 1.6.0 (the same pages and slots; tables view_lod160, view_far160 and view_dof160 below):
   2. Render distance. getDispDistance is inlined at 26 places there, each multiplying by ActorDebug's field (+0x310 of
      the instance at main+0x2cc43b0), but the game never makes that instance (its pointer stayed 0 in a whole 1.6.0 run,
      2026-11-04), so every place takes its null path: m = 1.0. 21 places take d from getTraverseDist (main+0x1561f80;
      its only callers, as 1.9.0's twin main+0x10a46d4 has getDispDistance alone); its exit (main+0x1562038, the
      `ldp x29, x30, [sp, #0x30]` that every path reaches with d in s0) gets d x K, K from the page (+0x64). The 5 others
      compute d inline; their null path's `fmov sN, #1.0` (just before an `adrp x8`, so x8 is free) loads K instead.
      Each place then gets d x K, as 1.9.0's stub gives (an object without a distance keeps the game's 100 at the 21).
      K = 1: the multiply by 1.0 and the word 0x3f800000, the game's values bit for bit. x8 and s1 are free at the exit.
   3. Depth of field. agl::pfx::DepthOfField::draw (main+0x14140b0; the "enable" byte at +0x1350): the entry's
      `cbz w8, main+0x1416508` (main+0x14140e8; the target is the epilogue, not a ret) gets the same flag stub (x10, the page's
      +0x68; local branches, the cbz and cbnz do not reach from the padding).
   1. Model detail distance. calcLodLevel is inlined into 32 gsys draw functions (main+0x14e2c30..0x14ecb90), each with
      the same sum v = z + bias as 1.9.0's. There the first source is the depth z (just stored at view info +0x18 or at
      unit +0x50) and the second the unit's bias ([unit+8], units 0x58 bytes apart), so z is the one scaled. 30 sites are
      `fadd s1, s1, s2`, 2 are `fadd s5, s5, s6`; each becomes `fmadd sd, sn, s31, sm` with s31 = c = 1/K from the page
      (+0x60), K = 1 the game's sum bit for bit. s31 is read and written in none of the 32 functions. The data page is
      out of ldr-literal range of the padding, so the stub gets its address through x16, which the game does use (its madd at +12 of
      7 sites and a store after 2 more read it, so x16 is saved, not assumed free): `str x16, [sp, #-16]!; adrp x16; ldr s31, [x16, #0x60]; ldr x16, [sp], #16; fmadd;
      b site+4` (6 words, 24 bytes, at +192 + 24 * the site's number; the 5 words around each site are checked, 160 in all).
   In the game (2026-11-04, plateau-ruin-legs): 1.6.0's render distance at 0.1x hides the Bokoblin and the chest by the
   ruin wall that 1x shows, at the same fps; 1.9.0's 2x and 3x crashed the world load (VIEW_DISP_MAX). 1.9.0's model detail
   distance runs (about 15k calls/s, the coarser level for 60% / 92% / 0% of units at 1x / 0.1x / max) but shows no
   visible change there. Not tried yet: the actor count and heap use at 1.5x, cutscenes, depth of field on far scenery. */
#pragma once
#include "botw_fov.h"   /* fov_branch, fov_adrp, fov_number */

#define VIEW_LOD_SLOT 0x60u      /* the data page: 1.9.0 uses +0 .. +0x20, the FOV's +0x48 / +0x4c (botw_fov.h) and the arrow log's +0x58 (8 bytes, game_patches_botw.c); the placement check's kept return address is at +0x70 (8 bytes) */
#define VIEW_DISP_SLOT 0x64u
#define VIEW_DOF_SLOT 0x68u
#define VIEW_DISP_KEEP_SLOT 0x70u   /* 8 bytes: base + VIEW_DISP_KEEP_RET, the caller that keeps the game's distance */
#define VIEW_DISP_KEEP_CALL 0x10a6e98u   /* bl getDispDistance in the placement check main+0x10a4d20 */
#define VIEW_DISP_KEEP_RET 0x10a6e9cu
#define VIEW_LOD_MIN 0.1
#define VIEW_LOD_ENV_MAX 100.0   /* TOTK_LOD_SCALE, as Tears of the Kingdom's */
#define VIEW_LOD_MAX 1e6         /* "always the finest": the panel's "max" */
#define VIEW_DISP_MIN 0.1
#define VIEW_DISP_MAX 1.5       /* 2x and 3x crashed the 1.9.0 world load (2026-11-04, the _Far actor packs streaming in) */
#define VIEW_MAX_WORDS 9

typedef struct { uint32_t off, original; } view_guard;
/* The first of n words in main that is not as shipped: its 1-based index and the word found; 0 = all as shipped. */
static inline int verify_words(const uint8_t *main, const view_guard *g, unsigned n, uint32_t *found) {
    for (unsigned i = 0; i < n; i++) {
        uint32_t have; memcpy(&have, main + g[i].off, 4);
        if (have != g[i].original) { if (found) *found = have; return (int)i + 1; }
    }
    return 0;
}
typedef struct {
    const view_guard *guard; unsigned guards;   /* the words checked before anything is written */
    uint32_t site;                              /* the word that becomes a branch to the stub */
    int32_t skip;                               /* the stub's place: the text end, 16-byte aligned, + skip (1.9.0's model detail distance and render distance stubs sit in the 12 bytes before it too) */
    unsigned words;                             /* its length */
    uint32_t slot;                              /* its word on the data page */
    int (*build)(uint32_t *w, uint64_t base, uint64_t stub, uint64_t data);   /* 0 = a branch or the adrp is out of range */
    unsigned sites;                             /* 1; the 1.6.0 model detail distance has VIEW_LOD160_SITES (view_lod160_apply) */
} view_patch;

/* The stubs (1.9.0), counted from the text end rounded up to 16 ("pad"; main+0x15a1c70). The page's room is 912 bytes
   (pad .. main+0x15a2000) and the other users fill it: botw_fov.h's FOV stub +32 .. +76, the camera blend's veneer +80, the
   arrow gravity veneer +104 .. +128 (game_patches_botw.c), the VR camera veneer +128 .. +200 (botw_vr_camera.h, VR only),
   upstream's heap veneers +256 and +272 (botw_heap_patch.h, native 5K or HD heaps only) and, here, the sensor caves
   (0011) +200 .. +884. So these three go where nothing else fits: the render distance (9 words) at -12, in the 12 bytes
   between the text end and pad (zero like the rest), the depth of field (6 words) at +884, after the caves, and the model detail distance (4 words) at +128, in the VR
   camera veneer's room (+128 .. +200: that veneer is installed only when the VR labo is on, and then finds its room taken
   and stops the start); each is checked empty before it is written. */
#define VIEW_LOD_BACK 0x10575ecu
#define VIEW_DISP_BACK 0x10a468cu
#define VIEW_DOF_BACK 0xfe09f0u
#define VIEW_DOF_RET 0xfe0a38u
static const view_guard view_lod_guard[] = {
    {0x10575a4u, 0x39404408u},   /* ldrb w8, [x0, #0x11]: the entry */
    {0x10575d4u, 0x2a1f03e9u},   /* mov w9, wzr */
    {0x10575e0u, 0xbd401801u},   /* ldr s1, [x0, #0x18]: the depth z */
    {0x10575e4u, 0xbd400042u},   /* ldr s2, [x2]: the bias */
    {0x10575e8u, 0x1e222821u},   /* fadd s1, s1, s2: the site */
    {VIEW_LOD_BACK, 0x1e202020u},   /* fcmp s1, s0 */
    {0x10575f0u, 0x54000185u},   /* b.pl */
    {0x1057620u, 0x39000109u},   /* strb w9, [x8] */
};
static const view_guard view_disp_guard[] = {
    {0x10a4640u, 0xb9400028u},   /* ldr w8, [x1]: the entry */
    {0x10a4670u, 0xb4000128u},   /* cbz x8: no multiplier object */
    {0x10a4674u, 0xbd431101u},   /* ldr s1, [x8, #0x310]: the multiplier */
    {0x10a4678u, 0x52a85908u},   /* mov w8, #0x42c80000: 100.0 */
    {0x10a467cu, 0x1e202008u},   /* fcmp s0, #0.0 */
    {0x10a4680u, 0x1e270102u},   /* fmov s2, w8 */
    {0x10a4684u, 0x1e22cc00u},   /* fcsel s0, s0, s2, gt */
    {0x10a4688u, 0x1e210800u},   /* fmul s0, s0, s1: the site */
    {VIEW_DISP_BACK, 0xa8c17bfdu},   /* ldp x29, x30, [sp], #0x10 */
    {VIEW_DISP_KEEP_CALL, 0x97fff5eau},   /* bl main+0x10a4640: the caller that keeps the game's distance */
    {VIEW_DISP_KEEP_RET, 0xbd000280u},    /* str s0, [x20]: its distance out */
};
static const view_guard view_dof_guard[] = {
    {0xfe09e4u, 0x52826209u},    /* mov w9, #0x1310 */
    {0xfe09e8u, 0x38696809u},    /* ldrb w9, [x0, x9]: the enable byte */
    {0xfe09ecu, 0x34000269u},    /* cbz w9, main+0xfe0a38: the site */
    {VIEW_DOF_BACK, 0xd10243ffu},   /* sub sp, sp, #0x90 */
    {VIEW_DOF_RET, 0xd65f03c0u},    /* ret */
};

/* Breath of the Wild 1.6.0: the 32 model detail distance stubs (+192 .. +960, below), the depth of field entry (a stub of
   the same 6 words at +960) and the render distance (getTraverseDist's exit, 12 words, and 5 null paths, 3 words each:
   +984 .. +1092). Below +192 the 1.6.0 text padding is the sound-link veneer's (+0, 4 words), the camera blend's (+16, 6
   words, game_patches_botw.c) and the field of view stubs' (+48 .. +180, the 1.6.0 rows' branch). The text ends at
   main+0x2129ba4, the padded file at main+0x2e0e988 (the data page main+0x2e10000). */
#define VIEW_DOF160_BACK 0x14140ecu
#define VIEW_DOF160_RET 0x1416508u
#define VIEW_TRAV160_SITE 0x1562038u   /* getTraverseDist's exit: ldp x29, x30, [sp, #0x30] */
#define VIEW_TRAV160_BACK 0x156203cu
#define VIEW_FAR160_NULLS 5u
#define VIEW_FAR160_EXIT 12u   /* the exit stub's words, then each null path's 3 */
#define VIEW_FAR160_WORDS (VIEW_FAR160_EXIT + 3u * VIEW_FAR160_NULLS)
static const view_guard view_dof160_guard[] = {
    {0x14140e0u, 0x52826a08u},   /* mov w8, #0x1350 */
    {0x14140e4u, 0x38686808u},   /* ldrb w8, [x0, x8]: the enable byte */
    {0x14140e8u, 0x34012108u},   /* cbz w8, main+0x1416508: the site */
    {VIEW_DOF160_BACK, 0xb9400809u},   /* ldr w9, [x0, #8] */
    {VIEW_DOF160_RET, 0x910743ffu},    /* add sp, sp, #0x1d0: the epilogue */
};
/* getTraverseDist's 4 words, then each null path's 5: cbz x8 (-12), ldr sN, [x8, #0x310] (-8), b (-4), the site's
   fmov sN, #1.0, adrp x8 (+4). */
#define VIEW_NULL(s, ld, fmov, adrp) {(s) - 12, 0xb4000068u}, {(s) - 8, ld}, {(s) - 4, 0x14000002u}, {(s), fmov}, {(s) + 4, adrp}
static const view_guard view_far160_guard[] = {
    {0x1561f80u, 0xd10103ffu},   /* sub sp, sp, #0x40: getTraverseDist's entry */
    {0x1562034u, 0x1e212800u},   /* fadd s0, s0, s1: d */
    {VIEW_TRAV160_SITE, 0xa9437bfdu},   /* ldp x29, x30, [sp, #0x30]: the exit, site 0 */
    {VIEW_TRAV160_BACK, 0xf9400bf5u},   /* ldr x21, [sp, #0x10] */
    VIEW_NULL(0x38a14u, 0xbd431102u, 0x1e2e1002u, 0xd0012ea8u),     /* s2 */
    VIEW_NULL(0x15656f8u, 0xbd431101u, 0x1e2e1001u, 0xb0008548u),   /* s1 */
    VIEW_NULL(0x1565948u, 0xbd431101u, 0x1e2e1001u, 0xb0008548u),
    VIEW_NULL(0x1566b70u, 0xbd431101u, 0x1e2e1001u, 0x90008548u),
    VIEW_NULL(0x1c1da74u, 0xbd431101u, 0x1e2e1001u, 0xb0004f88u),
};
#undef VIEW_NULL

/* Breath of the Wild 1.6.0's model detail distance: the 32 sites, each with the words at -8, -4, 0 (the fadd), +4, +8
   (the rewriter changes the words at -16 and +12 of 6 of them, so those are left out). Sorted; the stubs from +192, 24
   bytes for each site, before the depth of field's (+960) and the render distance's (+984). */
#define VIEW_FIVE(s, a, b, c, d, e) {(s) - 8, a}, {(s) - 4, b}, {(s), c}, {(s) + 4, d}, {(s) + 8, e}
static const view_guard view_lod160_guard[] = {
    VIEW_FIVE(0x14e2eac, 0xbd4009c2, 0xbd400b00, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e3348, 0xf9400fec, 0xbd400980, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e36b4, 0xbd400225, 0xbd400186, 0x1e2628a5, 0xbd400864, 0x1e2420a0),
    VIEW_FIVE(0x14e3990, 0xbd4009e2, 0xbd400b00, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e3fa8, 0xf9400fec, 0xbd400980, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e4314, 0xbd400225, 0xbd400186, 0x1e2628a5, 0xbd400864, 0x1e2420a0),
    VIEW_FIVE(0x14e4854, 0xbd400942, 0xbd400b20, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e4df8, 0xbd400181, 0xbd400162, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e5274, 0xbd4002e1, 0xbd400162, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e55a4, 0xbd400942, 0xbd400b20, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e5ce8, 0xbd400181, 0xbd400162, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e6164, 0xbd4002e1, 0xbd400162, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e6700, 0xbd400942, 0xbd400b00, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e6c70, 0xbd400321, 0xbd400162, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e70d8, 0xbd400301, 0xbd400162, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e7668, 0xbd400942, 0xbd400b00, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e7c30, 0xbd400321, 0xbd400162, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e8098, 0xbd400301, 0xbd400162, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e8684, 0xbd400942, 0xbd400b20, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e8d04, 0xf9400bea, 0xbd400142, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e9238, 0xbd4002a1, 0xbd400142, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e98cc, 0xbd400942, 0xbd400b20, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14e9f84, 0xf9400bea, 0xbd400142, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14ea4b8, 0xbd4002a1, 0xbd400142, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14ea88c, 0xbd4009c2, 0xbd400b00, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14eaab0, 0xbd4009e2, 0xbd400b00, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14eafcc, 0xbd4009c2, 0xbd400b20, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14eb1f0, 0xbd4009e2, 0xbd400b20, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14eb8f0, 0xbd400942, 0xbd400b00, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14ebe78, 0xbd400942, 0xbd400b00, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14ec47c, 0xbd400942, 0xbd400b20, 0x1e222821, 0x1e202020, 0x54000145),
    VIEW_FIVE(0x14eca04, 0xbd400942, 0xbd400b20, 0x1e222821, 0x1e202020, 0x54000145),
};
#undef VIEW_FIVE
#define VIEW_LOD160_SITES 32u
#define VIEW_LOD160_WORDS 6u

static inline uint32_t view_ldr(uint32_t op, unsigned rt, unsigned rn, uint32_t slot) {   /* ldr <rt>, [<rn>, #slot]; op: 0xbd400000 s, 0xb9400000 w */
    return op | ((slot / 4) << 10) | (rn << 5) | rt;
}
static inline int view_lod_stub_words(uint32_t *w, uint64_t base, uint64_t stub, uint64_t data) {
    w[0] = fov_adrp(11, base + stub, base + data);          /* adrp x11, <data page> */
    w[1] = view_ldr(0xbd400000u, 3, 11, VIEW_LOD_SLOT);     /* ldr s3, [x11, #0x60]: c = 1/K */
    w[2] = 0x1f030821u;                                     /* fmadd s1, s1, s3, s2: z x c + bias */
    w[3] = fov_branch(stub + 12, VIEW_LOD_BACK);            /* b main+0x10575ec */
    return w[0] && w[3];
}
static inline int view_disp_stub_words(uint32_t *w, uint64_t base, uint64_t stub, uint64_t data) {
    w[0] = 0x1e210800u;                                     /* fmul s0, s0, s1: the game's */
    w[1] = fov_adrp(8, base + stub + 4, base + data);       /* adrp x8, <data page> */
    w[2] = 0xf94007e9u;                                     /* ldr x9, [sp, #8]: the caller's return address */
    w[3] = 0xf940390au;                                     /* ldr x10, [x8, #0x70]: base + 0x10a6e9c */
    w[4] = 0xeb0a013fu;                                     /* cmp x9, x10 */
    w[5] = 0x54000060u;                                     /* b.eq w[8]: that caller keeps the game's value */
    w[6] = view_ldr(0xbd400000u, 1, 8, VIEW_DISP_SLOT);     /* ldr s1, [x8, #0x64]: K */
    w[7] = 0x1e210800u;                                     /* fmul s0, s0, s1 */
    w[8] = fov_branch(stub + 32, VIEW_DISP_BACK);           /* b main+0x10a468c */
    return w[1] && w[8];
}
static inline int view_dof_stub_words(uint32_t *w, uint64_t base, uint64_t stub, uint64_t data) {
    w[0] = fov_adrp(10, base + stub, base + data);          /* adrp x10, <data page> */
    w[1] = view_ldr(0xb9400000u, 10, 10, VIEW_DOF_SLOT);    /* ldr w10, [x10, #0x68]: off? */
    w[2] = 0x3500006au;                                     /* cbnz w10, w[5] */
    w[3] = 0x34000049u;                                     /* cbz w9, w[5]: the game's own test */
    w[4] = fov_branch(stub + 16, VIEW_DOF_BACK);            /* b main+0xfe09f0: draw */
    w[5] = fov_branch(stub + 20, VIEW_DOF_RET);             /* b main+0xfe0a38: ret */
    return w[0] && w[4] && w[5];
}
static inline int view_dof160_stub_words(uint32_t *w, uint64_t base, uint64_t stub, uint64_t data) {
    w[0] = fov_adrp(10, base + stub, base + data);          /* adrp x10, <data page> */
    w[1] = view_ldr(0xb9400000u, 10, 10, VIEW_DOF_SLOT);    /* ldr w10, [x10, #0x68]: off? */
    w[2] = 0x3500006au;                                     /* cbnz w10, w[5] */
    w[3] = 0x34000048u;                                     /* cbz w8, w[5]: the game's own test */
    w[4] = fov_branch(stub + 16, VIEW_DOF160_BACK);         /* b main+0x14140ec: draw */
    w[5] = fov_branch(stub + 20, VIEW_DOF160_RET);          /* b main+0x1416508: the epilogue */
    return w[0] && w[4] && w[5];
}
static const view_patch view_lod = {view_lod_guard, sizeof view_lod_guard / sizeof *view_lod_guard, 0x10575e8u, 128, 4, VIEW_LOD_SLOT, view_lod_stub_words, 1};
static const view_patch view_disp = {view_disp_guard, sizeof view_disp_guard / sizeof *view_disp_guard, 0x10a4688u, -12, 9, VIEW_DISP_SLOT, view_disp_stub_words, 1};
static const view_patch view_dof = {view_dof_guard, sizeof view_dof_guard / sizeof *view_dof_guard, 0xfe09ecu, 884, 6, VIEW_DOF_SLOT, view_dof_stub_words, 1};

static const view_patch view_dof160 = {view_dof160_guard, sizeof view_dof160_guard / sizeof *view_dof160_guard, 0x14140e8u, 960, 6, VIEW_DOF_SLOT, view_dof160_stub_words, 1};
static const view_patch view_far160 = {view_far160_guard, sizeof view_far160_guard / sizeof *view_far160_guard, VIEW_TRAV160_SITE, 984, VIEW_FAR160_WORDS, VIEW_DISP_SLOT, NULL, 1 + VIEW_FAR160_NULLS};

static const view_patch view_lod160 = {view_lod160_guard, sizeof view_lod160_guard / sizeof *view_lod160_guard, 0x14e2eacu, 192, VIEW_LOD160_WORDS, VIEW_LOD_SLOT, NULL, VIEW_LOD160_SITES};

static inline uint64_t view_stub_at(const view_patch *p, uint64_t text_end) { return (uint64_t)((int64_t)((text_end + 15) & ~15ULL) + p->skip); }

/* Install: the guards are checked first, then the stub's padding (zero, inside the 4 KiB page after the text), the data
   page and the slot (zero). Only when all hold are the slot's word, the stub and, last, the branch written. 0 = done;
   1..n = that guard is not as shipped (its word in *found); -1 = no room. Nothing is written unless 0 is returned. */
static inline int view_apply(const view_patch *p, uint8_t *main, uint64_t base, uint64_t text_end, uint64_t data, uint64_t size,
                             uint32_t slot_word, uint32_t *found) {
    { int bad = verify_words(main, p->guard, p->guards, found); if (bad) return bad; }
    const uint64_t stub = view_stub_at(p, text_end);
    if (stub < text_end || stub + 4 * p->words > ((text_end + 0xfff) & ~0xfffULL) || data & 0xfff || data + p->slot + 4 > size) return -1;
    for (unsigned i = 0; i < 4 * p->words; i++) if (main[stub + i]) return -1;
    for (unsigned i = 0; i < 4; i++) if (main[data + p->slot + i]) return -1;
    uint32_t w[VIEW_MAX_WORDS], jump = fov_branch(p->site, stub);
    if (!jump || !p->build(w, base, stub, data)) return -1;
    memcpy(main + data + p->slot, &slot_word, 4);   /* first: no call may read an empty word */
    memcpy(main + stub, w, 4 * p->words);
    memcpy(main + p->site, &jump, 4);
    return 0;
}

/* The words the slots hold. LOD: c = 1/K as a float, K 0.1..1e6. Render distance: K as a float, 0.1..10. 0 = not valid. */
static inline int view_lod_bits(double k, uint32_t *bits) {
    if (!isfinite(k) || k < VIEW_LOD_MIN || k > VIEW_LOD_MAX) return 0;
    float c = (float)(1.0 / k); memcpy(bits, &c, 4); return 1;
}
static inline int view_disp_bits(double k, uint32_t *bits) {
    if (!isfinite(k) || k < VIEW_DISP_MIN || k > VIEW_DISP_MAX) return 0;
    float f = (float)k; memcpy(bits, &f, 4); return 1;
}
static inline int view_lod_apply(uint8_t *main, uint64_t base, uint64_t text_end, uint64_t data, uint64_t size, double k, uint32_t *found) {
    uint32_t bits; if (!view_lod_bits(k, &bits)) return -2;   /* -2: the factor is not valid; nothing written */
    return view_apply(&view_lod, main, base, text_end, data, size, bits, found);
}
/* 1.6.0's model detail distance: the same order as view_apply over the 32 sites (every guard, the stubs' padding as one
   run and the slot first; then the slot, the stubs, last the branches). Site i's fadd is guard 5i+2, whose registers give
   the stub's fmadd. Return values as view_apply's; -2 = the factor is not valid. */
static inline int view_lod160_apply(uint8_t *main, uint64_t base, uint64_t text_end, uint64_t data, uint64_t size, double k, uint32_t *found) {
    const view_patch *p = &view_lod160; uint32_t bits;
    if (!view_lod_bits(k, &bits)) return -2;
    { int bad = verify_words(main, p->guard, p->guards, found); if (bad) return bad; }
    const uint64_t stub = view_stub_at(p, text_end), bytes = 4ull * p->words;
    if (stub + bytes * p->sites > ((text_end + 0xfff) & ~0xfffULL) || data & 0xfff || data + p->slot + 4 > size) return -1;
    for (uint64_t i = 0; i < bytes * p->sites; i++) if (main[stub + i]) return -1;
    for (unsigned i = 0; i < 4; i++) if (main[data + p->slot + i]) return -1;
    uint32_t w[VIEW_LOD160_SITES][VIEW_LOD160_WORDS], jump[VIEW_LOD160_SITES];
    for (unsigned i = 0; i < p->sites; i++) {
        const uint32_t site = p->guard[5 * i + 2].off, fadd = p->guard[5 * i + 2].original, at = (uint32_t)(stub + bytes * i);
        w[i][0] = 0xf81f0ff0u;                                         /* str x16, [sp, #-16]!: x16 is the game's */
        w[i][1] = fov_adrp(16, base + at + 4, base + data);            /* adrp x16, <data page> */
        w[i][2] = view_ldr(0xbd400000u, 31, 16, VIEW_LOD_SLOT);        /* ldr s31, [x16, #0x60]: c = 1/K */
        w[i][3] = 0xf84107f0u;                                         /* ldr x16, [sp], #16 */
        w[i][4] = 0x1f1f0000u | ((fadd >> 16 & 31) << 10) | (fadd & 0x3ffu);   /* fmadd sd, sn, s31, sm for fadd sd, sn, sm */
        w[i][5] = fov_branch(at + 20, site + 4);                       /* b site+4 */
        jump[i] = fov_branch(site, at);
        if (!w[i][1] || !w[i][5] || !jump[i]) return -1;
    }
    memcpy(main + data + p->slot, &bits, 4);
    memcpy(main + stub, w, sizeof w);
    for (unsigned i = 0; i < p->sites; i++) memcpy(main + p->guard[5 * i + 2].off, &jump[i], 4);
    return 0;
}
static inline int view_disp_apply(uint8_t *main, uint64_t base, uint64_t text_end, uint64_t data, uint64_t size, double k, uint32_t *found) {
    uint32_t bits; if (!view_disp_bits(k, &bits)) return -2;
    { int bad = verify_words(main, view_disp.guard, view_disp.guards, found); if (bad) return bad; }   /* the guards first, as view_apply: a second pass refuses at the site */
    if (data + VIEW_DISP_KEEP_SLOT + 8 > size) return -1;
    for (unsigned i = 0; i < 8; i++) if (main[data + VIEW_DISP_KEEP_SLOT + i]) return -1;
    const uint64_t keep = base + VIEW_DISP_KEEP_RET;
    memcpy(main + data + VIEW_DISP_KEEP_SLOT, &keep, 8);   /* before the branch: the stub reads it at every call */
    int r = view_apply(&view_disp, main, base, text_end, data, size, bits, found);
    if (r) memset(main + data + VIEW_DISP_KEEP_SLOT, 0, 8);   /* refused: nothing stays written */
    return r;
}
/* 1.6.0's render distance: the same order as view_apply (every guard, the stubs' padding and the slot; then the slot, the
   stubs, last the 6 branches). Site 0 is getTraverseDist's exit; null path i's fmov is guard 4 + 5i + 3, whose register
   the stub loads. Return values as view_apply's; -2 = the factor is not valid. */
static inline int view_far160_apply(uint8_t *main, uint64_t base, uint64_t text_end, uint64_t data, uint64_t size, double k, uint32_t *found) {
    const view_patch *p = &view_far160; uint32_t bits;
    if (!view_disp_bits(k, &bits)) return -2;
    { int bad = verify_words(main, p->guard, p->guards, found); if (bad) return bad; }
    const uint64_t stub = view_stub_at(p, text_end);
    if (stub + 4 * p->words > ((text_end + 0xfff) & ~0xfffULL) || data & 0xfff || data + p->slot + 4 > size) return -1;
    for (unsigned i = 0; i < 4 * p->words; i++) if (main[stub + i]) return -1;
    for (unsigned i = 0; i < 4; i++) if (main[data + p->slot + i]) return -1;
    uint32_t w[VIEW_FAR160_WORDS], site[1 + VIEW_FAR160_NULLS], jump[1 + VIEW_FAR160_NULLS];
    w[0] = 0xa9437bfdu;                                     /* ldp x29, x30, [sp, #0x30]: the game's */
    w[1] = fov_adrp(8, base + stub + 4, base + data);       /* adrp x8, <data page> */
    w[2] = view_ldr(0xbd400000u, 1, 8, VIEW_DISP_SLOT);     /* ldr s1, [x8, #0x64]: K */
    w[3] = 0x1e2e1002u;                                     /* fmov s2, #1.0 */
    w[4] = 0x1e222020u;                                     /* fcmp s1, s2 */
    w[5] = 0x540000c0u;                                     /* b.eq w[11]: K = 1, d is the game's own */
    w[6] = 0x1e202008u;                                     /* fcmp s0, #0.0 */
    w[7] = 0x5400006cu;                                     /* b.gt w[10]: d > 0 */
    w[8] = 0x52a85909u;                                     /* mov w9, #0x42c80000: 100.0, the callers' fallback */
    w[9] = 0x1e270120u;                                     /* fmov s0, w9 */
    w[10] = 0x1e210800u;                                    /* fmul s0, s0, s1: (d > 0 ? d : 100) x K */
    w[11] = fov_branch(stub + 44, VIEW_TRAV160_BACK);       /* b main+0x156203c */
    site[0] = VIEW_TRAV160_SITE; jump[0] = fov_branch(site[0], stub);
    for (unsigned i = 0; i < VIEW_FAR160_NULLS; i++) {
        const view_guard *g = &p->guard[4 + 5 * i + 3]; const uint64_t at = stub + 4 * VIEW_FAR160_EXIT + 12 * i;
        w[VIEW_FAR160_EXIT + 3 * i] = fov_adrp(8, base + at, base + data);                          /* adrp x8, <data page> */
        w[VIEW_FAR160_EXIT + 1 + 3 * i] = view_ldr(0xbd400000u, g->original & 31, 8, VIEW_DISP_SLOT);  /* ldr sN, [x8, #0x64]: K for 1.0 */
        w[VIEW_FAR160_EXIT + 2 + 3 * i] = fov_branch(at + 8, g->off + 4);                               /* b site+4: the adrp x8 */
        site[1 + i] = g->off; jump[1 + i] = fov_branch(g->off, at);
    }
    for (unsigned i = 0; i < p->words; i++) if (!w[i]) return -1;
    for (unsigned i = 0; i < p->sites; i++) if (!jump[i]) return -1;
    memcpy(main + data + p->slot, &bits, 4);
    memcpy(main + stub, w, sizeof w);
    for (unsigned i = 0; i < p->sites; i++) memcpy(main + site[i], &jump[i], 4);
    return 0;
}
static inline int view_dof160_apply(uint8_t *main, uint64_t base, uint64_t text_end, uint64_t data, uint64_t size, int off, uint32_t *found) {
    return view_apply(&view_dof160, main, base, text_end, data, size, off ? 1u : 0u, found);
}
static inline int view_dof_apply(uint8_t *main, uint64_t base, uint64_t text_end, uint64_t data, uint64_t size, int off, uint32_t *found) {
    return view_apply(&view_dof, main, base, text_end, data, size, off ? 1u : 0u, found);
}

/* graphics-lod.txt: the factor x100, 100..10000 (the panel's), or "max" (a word that starts so, as 1.4.3's restore reads
   it); the factor, 0 = not valid. TOTK_LOD_SCALE: 0.1..100, or "max". */
static inline double view_lod_parse_file(const char *text) {
    while (*text == ' ' || *text == '\t') text++;
    if (!strncmp(text, "max", 3)) return VIEW_LOD_MAX;
    double d; return fov_number(text, &d) && d >= 100 && d <= 10000 ? d / 100 : 0;
}
static inline double view_lod_parse_env(const char *text) {
    while (*text == ' ' || *text == '\t') text++;
    if (!strncmp(text, "max", 3)) return VIEW_LOD_MAX;
    double d; return fov_number(text, &d) && d >= VIEW_LOD_MIN && d <= VIEW_LOD_ENV_MAX ? d : 0;
}
/* graphics-far.txt: percent as 1.4.3 writes it (the panel offers 100..150; a file of 10..150 is taken); TOTK_FAR_SCALE:
   0.1..10. The factor, 0 = not valid. */
static inline double view_disp_parse_file(const char *text) {
    double d; return fov_number(text, &d) && d >= VIEW_DISP_MIN * 100 && d <= VIEW_DISP_MAX * 100 ? d / 100 : 0;
}
static inline double view_disp_parse_env(const char *text) {
    double d; return fov_number(text, &d) && d >= VIEW_DISP_MIN && d <= VIEW_DISP_MAX ? d : 0;
}
