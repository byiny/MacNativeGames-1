/* Breath of the Wild 1.9.0: Sheikah Sensor and shrine map mod, host side (local build only; the guest caves are in
   game_patches_botw.c patch_sensor_shrines, the map in docs/botw-v1.9.0-sensor.md). Included at the end of
   botw_fog_map.h: the fog position thread calls shrines_tick once a second with Link's position and reuses the live grid.

   - botw_shrines.bin (dev-data/botw/keep/local-patches/tools/make_shrine_table.py; <app>/Contents/Resources, or
     TOTK_BOTW_SHRINES=<file>): the 136 shrine positions and tower regions, the region of each fog cell, and the shrine
     icon's Color.bflan. Without it the caves stay out and the game is as shipped.
   - Data page after main (upstream uses +0x0 the frame step, +0x8 the pad tick, +0x10..0x1f the output size, +0x20 the ultrawide
     ratio, +0x48 / +0x4c the FOV and +0x1000 the font table; checked 2026-11-04, nothing below overlaps):
     +0x100 u32 seqlock over the tower texts, +0x10c u32 icon update request (host), +0x200 one reveal byte per shrine
     (Location_DungeonNNN), +0x300 + 32 NN the text after tower NN's name ({u16 length; u16 text[15]}). The guest's view
     (+0x5f0) and compose buffer (+0x600..+0xa00) are written only by the game's UI thread.
   - A shrine is revealed while Link is within 1.25 times the fog's reveal circle of it (owner, 2026-10-29: 1.25 x (48 m +
     32 m soft edge) = 115 m with the default BOTW_FOG_RADIUS of 60 m) in the field, or while its
     tower region's visited share (fog cells >= 128 / cells, as fog_ratio.py) is at least TOTK_BOTW_SHRINE_REVEAL_PCT
     (default 65 since 2026-10-30, was 70; 0 = no region reveal). Shares are counted every 2 s; the tower name shows its region's share (" 61%").
   - Grey: the shrine, DLC shrine and tower icons share one Color.bflan (frames 0 orange, 1 half, 2 blue). After the
     first field position the thread finds its copies in the guest heap (exact bytes, read with mach_vm_read, retried
     every 10 s, at most 5 tries) and writes curve keys so frame 0.5 is grey while frames 0, 1 and 2 keep their values
     (nn::ui2d Hermite; asm/grey.py in the task folder models it). P_Icon_00's first keys move from frame 1 to 0 with
     equal slopes; one frame of frame 1 may read past 255 / below 0 between its two slope stores.
   - The sensor text is the game's own (owner, 2026-10-29: the height text is gone).
   - Quest gate (owner, 2026-10-28): 31 base shrines and the 16 DLC shrines exist only once a quest flag is on (the
     map unit's LinkTag with that SaveFlag has a Create link to the shrine entrance; gate.py in the task folder lists
     them). The host reveals such a shrine only while its flag is on. Bool flags: [[main+0x1e3b188] +0xbe0] = trigger
     param; +0x8 count, +0x10 flag pointers sorted by CRC32 of the name (0x113226c); flag +0xb value, +0xc hash.
   - No save flag is written: the reveal only sets the icon's draw bit (0x80) while its discovered bit (0x8000) is clear; the
     game runs the icons' update after a reveal change because the host sets +0x10c (the trig cave). */
#include <mach/mach.h>

enum { SHR_N = 136, SHR_REG = 15, SHR_BFLAN = 680 };
static _Atomic(uint8_t *) g_shr_page;   /* set once the caves are written */
static struct { float x, y, z; int32_t region; } g_shr[SHR_N];
static uint32_t g_shr_cells[SHR_REG];
static int8_t *g_shr_cellmap;   /* FOG_GW * FOG_GH */
static uint8_t g_shr_bflan[SHR_BFLAN], g_shr_near[SHR_N], g_shr_reg_on[SHR_REG];
static int g_shr_pct = 65, g_shr_grey_tries;
static double g_shr_t_share, g_shr_t_grey;
static uint32_t g_shr_seq;

/* quest gate per shrine (NULL: always there); Dungeon106 opens with either flag */
static const struct { uint8_t n; const char *a, *b; } g_shr_gate_def[] = {
    {8, "RitoSongMystery_PanelOn"}, {12, "Rito_BrosRock_Dungeon"}, {14, "CursedStatue_StatueBroken"}, {20, "TwoWheels_Dungeon"},
    {21, "FourJewel_Dungeon"}, {24, "Animal_Forest_step1"}, {25, "GoronsRock_Break"}, {30, "Gaman_DungeonArrival"},
    {34, "SearchStone_Dungeon"}, {36, "NightStoneDungeonAppear"}, {76, "Shadow_Sign_Dungeon"}, {78, "BloodyMoonReliefFirstTime"},
    {80, "Remains_Fancier_Appearance"}, {84, "Enemy_Dragon_Fire_DoorOpen"}, {89, "CastleHarbor_DungeonAppear"}, {90, "SecretofObject_Step1"},
    {93, "SunazarashiRace_Dungeon"}, {94, "SandwormDef_Dungeon"}, {95, "GodTree_Dis_Tree2"}, {97, "NakedIsland_AppearDungeon"},
    {98, "FirstOhenro_Activated"}, {100, "SeekerEye_Step1"}, {101, "StatueofZora_Dungeon"}, {106, "ShieldofKolog_Finish", "ShieldofKolog_Minigame"},
    {108, "DarkWoods_Dungeon"}, {109, "Dungeon109_Appear"}, {110, "KnightDoll_Dungeon"}, {111, "giant_dungeon"},
    {112, "StolenBook_DungeonGenerate"}, {113, "Enemy_Dragon_Electric_DoorOpen"}, {114, "Relief_Landing_Act"}, {117, "Enemy_Dragon_Ice_DoorOpen"},
    {120, "Lock_OneHitDungeon001"}, {121, "BalladOfHeroGerudo_AppearDungeon03"}, {122, "Lock_OneHitDungeon002"}, {123, "Lock_OneHitDungeon003"},
    {124, "BalladOfHeroZora_AppearDungeon01"}, {125, "BalladOfHeroZora_AppearDungeon02"}, {126, "BalladOfHeroZora_AppearDungeon03"},
    {127, "BalladOfHeroRito_AppearDungeon01"}, {128, "BalladOfHeroRito_AppearDungeon03"}, {129, "BalladOfHeroRito_AppearDungeon02"},
    {130, "BalladOfHeroGoron_AppearDungeon02"}, {131, "BalladOfHeroGoron_AppearDungeon01"}, {132, "BalladOfHeroGoron_AppearDungeon03"},
    {133, "BalladOfHeroGerudo_AppearDungeon02"}, {134, "BalladOfHeroGerudo_AppearDungeon01"}, {135, "Lock_OneHitDungeon004"},
};
enum { SHR_GATES = sizeof g_shr_gate_def / sizeof *g_shr_gate_def, SHR_GDT = 0x1e3b188 };
static struct { uintptr_t flag[2]; uint8_t on, missing; } g_shr_gate[SHR_GATES];
static int8_t g_shr_gate_of[SHR_N];   /* index into g_shr_gate + 1, 0 = no gate */

static uint32_t shr_crc32(const char *s) {   /* sead HashCRC32 (zlib CRC-32) */
    uint32_t c = 0xffffffff;
    for (; *s; s++) { c ^= (uint8_t)*s; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320 & -(c & 1)); }
    return ~c;
}
/* the bool flag's object, by binary search over the hashes as the game does; 0 if not there */
static uintptr_t shr_flag_find(const char *name) {
    uintptr_t mgr = 0, p1 = 0, tp = 0, arr = 0; int32_t n = 0; uint32_t h = shr_crc32(name);
    if (!fog_read(g_fog_main + SHR_GDT, &mgr, 8) || !mgr || !fog_read(mgr + 0xbe0, &p1, 8) || !p1 || !fog_read(p1, &tp, 8) || !tp
        || !fog_read(tp + 8, &n, 4) || !fog_read(tp + 0x10, &arr, 8) || !arr || n <= 0 || n > 1 << 20) return 0;
    for (int32_t lo = 0, hi = n - 1; lo <= hi;) {
        int32_t mid = lo + (hi - lo) / 2; uintptr_t f = 0; uint32_t fh = 0;
        if (!fog_read(arr + 8 * (uintptr_t)mid, &f, 8) || !f || !fog_read(f + 0xc, &fh, 4)) return 0;
        if (fh == h) return f;
        if (fh < h) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
/* 1 while the flag is on; the cached object is checked by its hash and found again when it moved */
static int shr_flag_on(uintptr_t *f, const char *name, uint8_t *missing) {
    uint8_t b[8]; uint32_t h = shr_crc32(name);
    for (int pass = 0; pass < 2; pass++) {
        if (*f && fog_read(*f + 8, b, 8) && !memcmp(b + 4, &h, 4)) { *missing = 0; return b[3] & 1; }
        if (pass || !(*f = shr_flag_find(name))) break;
    }
    if (!*missing) { *missing = 1; MLOG("【红】[shrine] quest flag %s not found: its shrine stays hidden", name); }
    return 0;
}
static void shr_gates(void) {
    for (int g = 0; g < SHR_GATES; g++) {
        int on = shr_flag_on(&g_shr_gate[g].flag[0], g_shr_gate_def[g].a, &g_shr_gate[g].missing)
              || (g_shr_gate_def[g].b && shr_flag_on(&g_shr_gate[g].flag[1], g_shr_gate_def[g].b, &g_shr_gate[g].missing));
        if (on != g_shr_gate[g].on) MLOG("[shrine] Dungeon%03d quest flag %s: %s", g_shr_gate_def[g].n, g_shr_gate_def[g].a, on ? "on, may show" : "off, hidden");
        g_shr_gate[g].on = (uint8_t)on;
    }
}

/* game_patches_botw.c patch_sensor_shrines: 1 when the table is loaded and the caves may go in */
int botw_shrines_load(void) {
    const char *e = getenv("TOTK_BOTW_SHRINES"), *p = getenv("TOTK_BOTW_SHRINE_REVEAL_PCT");
    if (p && *p) { char *end; long v = strtol(p, &end, 10); if (!*end && v >= 0 && v <= 100) g_shr_pct = (int)v; else MLOG("【红】[shrine] TOTK_BOTW_SHRINE_REVEAL_PCT=%s is not 0..100: using 65", p); }
    NSString *path = e && *e ? @(e) : nil;
    for (NSString *dir = NSBundle.mainBundle.resourcePath; !path && dir.length > 1; dir = dir.stringByDeletingLastPathComponent) {   /* the game runs from <app>/Contents/Resources/totk/build */
        NSString *f = [dir stringByAppendingPathComponent:@"botw_shrines.bin"];
        if ([NSFileManager.defaultManager fileExistsAtPath:f] || [dir.lastPathComponent isEqualToString:@"Resources"]) path = f;
    }
    NSData *d = [NSData dataWithContentsOfFile:path];
    const size_t head = 32 + 16 * SHR_N + 4 * SHR_REG, cells = (size_t)FOG_GW * FOG_GH;
    uint32_t h[6] = {0}, bl = 0;
    if (d.length >= head + cells + 4) { memcpy(h, (const uint8_t *)d.bytes + 8, sizeof h); memcpy(&bl, (const uint8_t *)d.bytes + head + cells, 4); }
    if (d.length != head + cells + 4 + SHR_BFLAN || memcmp(d.bytes, "BOTWSHR1", 8) || h[0] != SHR_N || h[1] != FOG_GW || h[2] != FOG_GH || h[3] != FOG_CELL || h[4] != SHR_REG || bl != SHR_BFLAN) {
        MLOG("【红】[shrine] %s: %s: sensor and shrine map mod off", path.UTF8String, d ? "not a shrine table of this version" : "missing");
        return 0;
    }
    const uint8_t *b = d.bytes;
    memcpy(g_shr, b + 32, sizeof g_shr); memcpy(g_shr_cells, b + 32 + sizeof g_shr, sizeof g_shr_cells);
    g_shr_cellmap = malloc(cells); if (!g_shr_cellmap) return 0;
    memcpy(g_shr_cellmap, b + head, cells); memcpy(g_shr_bflan, b + head + cells + 4, SHR_BFLAN);
    for (int g = 0; g < SHR_GATES; g++) g_shr_gate_of[g_shr_gate_def[g].n] = (int8_t)(g + 1);
    MLOG("[shrine] table loaded (%s): reveal within 1.25 x the fog circle and at %d%% of a tower region%s", path.lastPathComponent.UTF8String, g_shr_pct, g_shr_pct ? "" : " (off)");
    return 1;
}
void botw_shrines_start(uint8_t *page) { atomic_store(&g_shr_page, page); }

/* tower texts under the seqlock (the guest reads them on its UI thread) */
static void shr_towers(const double *share) {
    uint8_t *pg = atomic_load(&g_shr_page);
    _Atomic uint32_t *seq = (_Atomic uint32_t *)(void *)(pg + 0x100);
    atomic_store_explicit(seq, ++g_shr_seq * 2 - 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    for (int nn = 1; nn <= SHR_REG; nn++) {
        char s[16]; int n = snprintf(s, sizeof s, " %d%%", (int)floor(share[nn - 1] * 100));
        uint16_t u[16] = {(uint16_t)n}; for (int i = 0; i < n; i++) u[1 + i] = (uint8_t)s[i];
        memcpy(pg + 0x300 + 32 * nn, u, 32);
    }
    atomic_store_explicit(seq, g_shr_seq * 2, memory_order_release);
}

/* grey frame 0.5: curve keys of the Color.bflan (file offset, vanilla float, new float), in store order */
static const struct { uint16_t off; float was, now; } g_shr_keys[] = {
    {0x0cc, 0.0f, -1400.0f},   /* P_Glow_00 R key 0 slope (frame 0.5: 80) */
    {0x12c, 0.0f, 640.0f},     /* P_Glow_00 B key 0 slope (80; G is 80) */
    {0x18c, 1.0f, 0.0f},       /* P_Icon_00 R key 0 frame 1 -> 0 (255 on 0..2 with zero slopes) */
    {0x1a0, 0.0f, -560.0f},    /* P_Icon_00 R key 1 slope */
    {0x194, 0.0f, -560.0f},    /* P_Icon_00 R key 0 slope (150 at 0.5, 255 at 1) */
    {0x1d4, 1.0f, 0.0f},       /* P_Icon_00 B key 0 frame */
    {0x1e8, 0.0f, 800.0f},     /* P_Icon_00 B key 1 slope */
    {0x1dc, 0.0f, 800.0f},     /* P_Icon_00 B key 0 slope (150 at 0.5, 0 at 1) */
    {0x244, 0.0f, -840.0f},    /* P_Icon_01 R key 0 slope (150) */
    {0x28c, 0.0f, 1200.0f},    /* P_Icon_01 B key 0 slope (150; G is 150) */
};
uint64_t runtime_heap_base(void);
void runtime_guest_committed(uint64_t *heap_bytes, uint64_t *alias_mapped);
static int shr_grey(void) {   /* copies found and patched */
    uint64_t base = runtime_heap_base(), size = 0, alias = 0; runtime_guest_committed(&size, &alias);
    enum { CH = 4 << 20 };
    uint8_t *buf = malloc(CH + SHR_BFLAN); if (!buf || !base || !size) { free(buf); return 0; }
    double t0 = fog_now(); int found = 0, done = 0;
    for (uint64_t at = 0; at < size && found < 8; at += CH) {
        size_t n = (size_t)(size - at < CH + SHR_BFLAN ? size - at : CH + SHR_BFLAN);
        if (!fog_read(base + at, buf, n)) continue;
        for (size_t i = 0; i < CH && i + SHR_BFLAN <= n && found < 8; i += 4) {   /* SARC data is aligned; a copy across the chunk end is in the overlap */
            if (memcmp(buf + i, g_shr_bflan, SHR_BFLAN)) continue;
            uintptr_t f = base + at + i; found++; int ok = 1;
            for (size_t k = 0; k < sizeof g_shr_keys / sizeof *g_shr_keys && ok; k++) {
                float was; memcpy(&was, g_shr_bflan + g_shr_keys[k].off, 4);
                ok = was == g_shr_keys[k].was && mach_vm_write(mach_task_self(), f + g_shr_keys[k].off, (vm_offset_t)&g_shr_keys[k].now, 4) == KERN_SUCCESS;
            }
            done += ok;
            MLOG("%s[shrine] shrine icon colours at guest heap +%#llx: %s", ok ? "" : "【红】", (unsigned long long)(at + i), ok ? "frame 0.5 is grey" : "write failed");
        }
    }
    free(buf);
    MLOG("[shrine] colour scan: %d cop%s of the shrine icon colours in %.0f MB of heap (%.0f ms)", found, found == 1 ? "y" : "ies", size / 1048576.0, (fog_now() - t0) * 1e3);
    return done;
}

/* fog position thread, once a second: Link's position (pos valid and in the field: ok) */
static void shrines_tick(const float *pos, int ok) {
    uint8_t *pg = atomic_load(&g_shr_page);
    if (!pg) return;
    double now = fog_now();
    if (ok && g_shr_grey_tries < 5 && now - g_shr_t_grey >= 10) {
        g_shr_t_grey = now;
        if (shr_grey()) g_shr_grey_tries = 5;
        else if (++g_shr_grey_tries == 5) MLOG("【红】[shrine] the shrine icon colours were not found in 5 scans: mod icons keep the orange colour");
    }
    if (now - g_shr_t_share >= 2) {
        g_shr_t_share = now;
        uint32_t vis[SHR_REG] = {0};
        pthread_mutex_lock(&g_fog_gmu);
        for (size_t i = 0; i < (size_t)FOG_GW * FOG_GH; i++) { int r = g_shr_cellmap[i]; if (r >= 0 && g_fog_grid[i] >= 128) vis[r]++; }
        pthread_mutex_unlock(&g_fog_gmu);
        double share[SHR_REG];
        for (int r = 0; r < SHR_REG; r++) {
            share[r] = g_shr_cells[r] ? (double)vis[r] / g_shr_cells[r] : 0;
            int on = g_shr_pct > 0 && share[r] * 100 >= g_shr_pct;
            if (on != g_shr_reg_on[r]) MLOG("[shrine] tower region %02d at %.1f%%: its shrines %s", r + 1, share[r] * 100, on ? "show" : "hide");
            g_shr_reg_on[r] = (uint8_t)on;
        }
        shr_towers(share);
        if (ok) shr_gates();   /* every 2 s, one 8-byte read per quest flag; outside the field the gates keep their value */
    }
    int changed = 0;
    const float near = 1.25f * (g_fog_radius + FOG_EDGE);   /* 1.25 x the fog's reveal circle (owner): 115 m by default */
    for (int k = 0; k < SHR_N; k++) {
        if (ok && isfinite(g_shr[k].x)) {
            float dx = pos[0] - g_shr[k].x, dz = pos[2] - g_shr[k].z; uint8_t nr = dx * dx + dz * dz <= near * near;
            if (nr != g_shr_near[k]) { g_shr_near[k] = nr; MLOG("[shrine] Dungeon%03d %s (%.0f m)", k, nr ? "near: shown" : "left", sqrtf(dx * dx + dz * dz)); }
        }
        int r = g_shr[k].region;
        int gate = g_shr_gate_of[k];
        uint8_t on = (uint8_t)((!gate || g_shr_gate[gate - 1].on) && (g_shr_near[k] || (r >= 0 && r < SHR_REG && g_shr_reg_on[r])));   /* a quest shrine only once its quest made it appear (owner) */
        changed |= on != pg[0x200 + k];
        atomic_store_explicit((_Atomic uint8_t *)(pg + 0x200 + k), on, memory_order_relaxed);
    }
    /* the map manager updates the shrine icons only in a frame with a game flag change: the trig cave also runs it while
       +0x10c is 1, here for one tick (about a second) after a reveal byte changed */
    atomic_store_explicit((_Atomic uint32_t *)(void *)(pg + 0x10c), (uint32_t)changed, memory_order_relaxed);
}
