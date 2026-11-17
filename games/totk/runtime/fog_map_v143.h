/* Tears of the Kingdom 1.4.3: fog-of-war map and sensor shrines (local mod; port of BOTW local patches 0004 and 0011).
   Places Link has been are clear on the map, the rest is darker (gfx/nvn/totk_fog_gfx.h darkens the map tiles); a
   shrine shows on the map once Link passed near it or its tower region is visited to TOTK_FOG_SHRINE_PCT; the big map
   shows a tower region's visited share after the tower's name. Not on iOS, not in the iOS export.

   - Table: TOTK_FOG_TABLE=<file>, else <TOTK_PROFILE_DIR or user-data>/fog-map/totk_fog.bin, else Resources/totk_fog.bin
     in a folder above the working one (dev-data/totk/keep/local-patches/tools/make_totk_fog_table.py). No table: the
     mod is off and nothing is patched.
   - Hooks: one 16-byte veneer at PAD_V143_FOG (text_padding_v143.h) branches to a host-stack wrapper of
     totk_fog_hook_entry, which passes x19, x20, x22 and the guest return address (x30 when it is in main's text, else
     [x29 + 8]: the frame record of _runtime_hle_switch, hle_stack.S) to totk_fog_hook. Each site's word is checked
     first; a wrong word turns that hook off (L off: the whole mod off).
     L 0xc8e51c `bl 0x401ad0` (Hero's Path writer: layer of the recorded point, w1 = 1): the point goes to the fog.
     A 0x20f6fe4 `bl 0x1b02f8` (map icon query: IsVisitLocation bool of hash w3): a revealed shrine reads as visited
     (gated shrines only once DungeonState is not Hidden); its icon's root pane alpha +0x59 is 0x70 until it is visited.
     B 0x2156208 `bl 0x20f61a8` (big-map hover name, type [x19 + 0x308] == 5 and tower vtable at [x22]): the name gets
     " NN%" (the tower region's visited share); the text id changes with the share so the game sets it again.
   - Fog thread (1 Hz): drains the Hero's Path points into the grids (totk_fog_core.h tfog_state), pushes the changed
     boxes to the Metal side, every 2 s counts the region shares and the shrine reveal bytes.
   - Slots (fs_host.c open_file): reading save:/slot_NN/progress.sav loads <profile>/fog-map/<user>/slot_NN.grid and
     paints that slot's footprint.sav into it; writing it saves the grids as slot NN. Known limit: a new game started
     after a load in the same run keeps the loaded slot's grid (restart the app before a new game).
   - TOTK_FOG=0 off; TOTK_FOG_RADIUS (60 m, plus a 32 m soft edge); TOTK_FOG_DARK (0.3: what is left of unvisited colour);
     TOTK_FOG_LOG=1 logs every point; TOTK_FOG_SHRINES=0 leaves the shrines and tower names alone; TOTK_FOG_SHRINE_PCT
     (65; 0 = no region reveal); TOTK_FOG_SHRINE_DIM=0 keeps revealed icons at full alpha. */
#pragma once
#include "runtime.h"
#include "text_padding_v143.h"
#include <TargetConditionals.h>

#if !TARGET_OS_IPHONE
#include "totk_fog_core.h"
#include <errno.h>
#include <sys/stat.h>

int totk_fog_gfx_start(const void *table, size_t n, float dark, int log);   /* gfx/nvn/totk_fog_gfx.h */
int totk_fog_gfx_grid(int layer, const uint8_t *grid, int x0, int z0, int x1, int z1);
const char *runtime_fs_profile_dir(void);   /* fs_host.c */
const char *runtime_fs_save_root(void);

enum { FM_SITE_L = 0xc8e51c, FM_FN_L = 0x401ad0, FM_SITE_A = 0x20f6fe4, FM_FN_A = 0x1b02f8, FM_SITE_B = 0x2156208, FM_FN_B = 0x20f61a8,
       FM_GATE_REF = 0x3cffe8, FM_GATE_GET = 0x735698, FM_ICON_VT = 0x371dcc8, FM_TOWER_VT = 0x371dc30, FM_HIDDEN = 0x04a35d72,
       FM_DUNGEON_STATE = (int)0xee7517dc, FM_MAX_SHRINES = 256 };
typedef struct { uint16_t *str; int32_t len; uint32_t pad; uint64_t id; } FmText;   /* the game's text object */
typedef struct { uint64_t a; uint32_t b; uint8_t c; uint8_t pad[3]; } FmRef;   /* game data handle, zeroed */

uint64_t g_totk_fog_text[2] = {0, 0};   /* main's text [lo, hi): read by totk_fog_hook_entry */
static struct {
    uint8_t *base;
    uintptr_t ret[3];                   /* guest return addresses of L, A, B */
    int on, on_a, on_b, dim, log, pct;
    uint32_t (*layer_fn)(const float *pos, uint32_t force);
    uint32_t (*visit_fn)(void *mgr, uint8_t *out, void *ref, uint32_t hash);
    void (*name_fn)(void *info, FmText *text);
    uint32_t (*gate_ref)(void *mgr, FmRef *ref, uint32_t hash);
    uint32_t (*gate_get)(void *mgr, uint32_t *out, FmRef *ref, uint32_t hash);
    tfog_table tab;
    tfog_state st;
    _Atomic uint8_t reveal[FM_MAX_SHRINES], pct_of[256];
    uint64_t hits[3];
} fm;

/* ---------- hooks (guest threads) ---------- */
static int fm_shrine_index(uint32_t hash) {
    for (uint32_t k = 0; k < fm.tab.nshrines && k < FM_MAX_SHRINES; k++) if (tfog_shrine_at(&fm.tab, k).hash == hash) return (int)k;
    return -1;
}
static int fm_tower_index(uint32_t hash) {
    for (uint32_t k = 0; k < fm.tab.ntowers; k++) if (tfog_tower_at(&fm.tab, k).hash == hash) return (int)k;
    return -1;
}
static uint64_t fm_rd64(uint64_t a) { uint64_t v = 0; if (a) memcpy(&v, (const void *)a, 8); return v; }
/* the icon's root pane alpha byte, or NULL when the entry's wrapper is not a shrine icon */
static uint8_t *fm_icon_alpha(uint64_t entry) {
    uint64_t w = fm_rd64(entry + 8);
    if (!w || fm_rd64(w) != (uint64_t)(fm.base + FM_ICON_VT)) return NULL;
    uint64_t pane = fm_rd64(fm_rd64(w + 0x20) + 0x18);
    return pane ? (uint8_t *)(pane + 0x59) : NULL;
}
static uint64_t fm_layer_hook(uint64_t pos, uint64_t force) {
    uint32_t layer = fm.layer_fn((const float *)pos, (uint32_t)force);
    float p[3];
    memcpy(p, (const void *)pos, sizeof p);
    tfog_push(&fm.st, (int)layer, p[0], p[2]);
    fm.hits[0]++;
    if (fm.log) fprintf(stderr, "[totk-mod] fog: point %.0f %.0f %.0f layer %u\n", p[0], p[1], p[2], layer);
    return layer;
}
static uint64_t fm_visit_hook(uint64_t mgr, uint64_t out, uint64_t ref, uint64_t hash, uint64_t entry) {
    uint32_t r = fm.visit_fn((void *)mgr, (uint8_t *)out, (void *)ref, (uint32_t)hash);
    fm.hits[1]++;
    if (!(r & 1) || !out) return r;
    int k = fm_shrine_index((uint32_t)hash);
    if (k < 0) return r;
    uint8_t *alpha = fm.dim ? fm_icon_alpha(entry) : NULL;
    if (*(uint8_t *)out) { if (alpha && *alpha == 0x70) *alpha = 0xff; return r; }   /* visited: full alpha again */
    if (!atomic_load_explicit(&fm.reveal[k], memory_order_relaxed)) return r;
    if (tfog_shrine_at(&fm.tab, (uint32_t)k).gated) {   /* a hidden entrance shows once its quest made it appear */
        FmRef h = {0};
        uint32_t v = 0;
        if (!(fm.gate_ref((void *)mgr, &h, (uint32_t)FM_DUNGEON_STATE) & 1) || !(fm.gate_get((void *)mgr, &v, &h, (uint32_t)hash) & 1) || v == FM_HIDDEN) return r;
    }
    *(uint8_t *)out = 1;
    if (alpha && *alpha == 0xff) *alpha = 0x70;
    return r;
}
static uint64_t fm_name_hook(uint64_t info, uint64_t text, uint64_t x19, uint64_t x22) {
    fm.hits[2]++;
    if (x19 && x22 && text) {
        uint32_t type, th;
        memcpy(&type, (const void *)(x19 + 0x308), 4);
        if (type == 5 && fm_rd64(x22) == (uint64_t)(fm.base + FM_TOWER_VT)) {
            memcpy(&th, (const void *)(x22 + 0x88), 4);
            int t = fm_tower_index(th);
            const FmText *src = (const FmText *)text;
            if (t >= 0 && src->str) {
                static uint16_t buf[48];   /* the game's UI thread only; setText copies it */
                static FmText tx;
                int pct = atomic_load_explicit(&fm.pct_of[t], memory_order_relaxed);
                int n = tfog_text_suffix(src->str, src->len, pct, buf, TFOG_TEXT_CAP);
                buf[n] = 0;
                tx = (FmText){buf, n, 0, src->id ^ (0x5f0f000000000000ull + (uint64_t)pct)};
                fm.name_fn((void *)info, &tx);
                return 0;
            }
        }
    }
    fm.name_fn((void *)info, (FmText *)text);
    return 0;
}
/* totk_fog_hook_entry: x0-x3 as at the call, x4 = x19, x5 = x20, x6 = x22, x7 = the guest return address */
__attribute__((used)) uint64_t totk_fog_hook(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x19, uint64_t x20, uint64_t x22, uint64_t ra) {
    if (ra == fm.ret[0]) return fm_layer_hook(x0, x1);
    if (ra == fm.ret[1]) return fm_visit_hook(x0, x1, x2, x3, x20);
    if (ra == fm.ret[2]) return fm_name_hook(x0, x1, x19, x22);
    runtime_fail("fog map hook: called from an unknown site");
}
void totk_fog_hook_entry(void);
__asm__(".text\n.p2align 2\n.globl _totk_fog_hook_entry\n_totk_fog_hook_entry:\n"
        "    mov x4, x19\n    mov x5, x20\n    mov x6, x22\n    mov x7, x30\n"
        "    adrp x9, _g_totk_fog_text@PAGE\n    add x9, x9, _g_totk_fog_text@PAGEOFF\n    ldp x10, x11, [x9]\n"
        "    cmp x30, x10\n    b.lo 1f\n    cmp x30, x11\n    b.lo 2f\n"
        "1:  ldr x7, [x29, #8]\n"   /* switched to the host stack: the guest's x30 is in _runtime_hle_switch's frame record */
        "2:  b _totk_fog_hook\n");

/* ---------- slots (fs_host.c open_file, guest threads) ---------- */
static int fm_slot_of(const char *path) {
    static const char pre[] = "save:/slot_";
    if (!path || strncmp(path, pre, sizeof pre - 1)) return -1;
    const char *s = path + sizeof pre - 1;
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9' || strcmp(s + 2, "/progress.sav")) return -1;
    return (s[0] - '0') * 10 + s[1] - '0';
}
static int fm_grid_path(int slot, char *out, size_t cap, int make) {
    const char *prof = runtime_fs_profile_dir(), *root = runtime_fs_save_root(), *user = root ? strrchr(root, '/') : NULL;
    if (!prof || !*prof || !user) return 0;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/fog-map", prof); if (make) mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/fog-map%s", prof, user); if (make) mkdir(dir, 0700);
    return snprintf(out, cap, "%s/slot_%02d.grid", dir, slot) < (int)cap;
}
void totk_fog_save_opened(const char *path, int write) {
    int slot = fm.on ? fm_slot_of(path) : -1;
    if (slot < 0) return;
    char grid[1200];
    if (!fm_grid_path(slot, grid, sizeof grid, write)) return;
    if (write) {
        if (!tfog_on_save(&fm.st, slot, grid)) fprintf(stderr, "【红】[totk-mod] fog: cannot write %s\n", grid);
        else if (fm.log) fprintf(stderr, "[totk-mod] fog: slot %02d grid saved\n", slot);
        return;
    }
    char fpp[1200];
    size_t n = 0;
    snprintf(fpp, sizeof fpp, "%s/slot_%02d/footprint.sav", runtime_fs_save_root(), slot);
    uint8_t *fp = tfog_slurp(fpp, 16 << 20, &n);
    int points = 0, had = tfog_on_load(&fm.st, slot, grid, fp, n, &points);
    free(fp);
    if (had >= 0) fprintf(stderr, "[totk-mod] fog: slot %02d loaded: %s, %d Hero's Path point(s)\n", slot, had ? "its fog grid" : "new grid", points);
}

/* ---------- fog thread ---------- */
static void *fm_loop(void *arg) {
    (void)arg;
    pthread_setname_np("totk.fog");
    static uint8_t copy[TFOG_NCELLS];
    static double share[256];
    tfog_box pend[TFOG_LAYERS] = {tfog_box_empty, tfog_box_empty, tfog_box_empty};
    uint8_t region_on[256] = {0};
    for (uint64_t tick = 0;; tick++) {
        sleep(1);
        for (int l = 0; l < TFOG_LAYERS; l++) {
            pthread_mutex_lock(&fm.st.mu);
            if (l == 0) tfog_drain_locked(&fm.st);
            tfog_box_add(&pend[l], fm.st.dirty[l]);
            fm.st.dirty[l] = tfog_box_empty;
            tfog_box b = pend[l];
            if (b.x0 <= b.x1) memcpy(copy, fm.st.grid[l], sizeof copy);
            pthread_mutex_unlock(&fm.st.mu);
            if (b.x0 > b.x1) continue;
            int all = b.x0 == 0 && b.z0 == 0 && b.x1 == TFOG_GW - 1 && b.z1 == TFOG_GH - 1;
            if (totk_fog_gfx_grid(l, copy, all ? 1 : b.x0, b.z0, all ? 0 : b.x1, b.z1)) pend[l] = tfog_box_empty;   /* else Metal is not ready: again next tick */
        }
        if (!fm.on_a && !fm.on_b) continue;
        if (tick % 2) continue;
        pthread_mutex_lock(&fm.st.mu);
        tfog_shares(&fm.tab, fm.st.grid[1], share);
        for (uint32_t k = 0; k < fm.tab.nshrines && k < FM_MAX_SHRINES; k++) {
            tfog_shrine s = tfog_shrine_at(&fm.tab, k);
            uint8_t on = (uint8_t)tfog_shrine_revealed(&fm.st.grid[0][0], &s, share, fm.pct);
            if (on != atomic_load_explicit(&fm.reveal[k], memory_order_relaxed) && fm.log) fprintf(stderr, "[totk-mod] fog: shrine %u %s\n", k, on ? "revealed" : "hidden");
            atomic_store_explicit(&fm.reveal[k], on, memory_order_relaxed);
        }
        pthread_mutex_unlock(&fm.st.mu);
        for (uint32_t t = 0; t < fm.tab.ntowers; t++) {
            int pct = (int)floor(share[t] * 100);
            atomic_store_explicit(&fm.pct_of[t], (uint8_t)pct, memory_order_relaxed);
            uint8_t on = fm.pct > 0 && pct >= fm.pct;
            if (on != region_on[t]) fprintf(stderr, "[totk-mod] fog: tower region %02u at %d%%: its shrines %s\n", t + 1, pct, on ? "show" : "follow the fog");
            region_on[t] = on;
        }
        if (tick % 30 == 0) fprintf(stderr, "[totk-mod] fog: %llu points (%llu dropped, %u lost), hook calls L %llu A %llu B %llu\n", (unsigned long long)fm.st.points,
                                    (unsigned long long)fm.st.dropped, fm.st.ring_lost, (unsigned long long)fm.hits[0], (unsigned long long)fm.hits[1], (unsigned long long)fm.hits[2]);
    }
    return NULL;
}

/* ---------- install ---------- */
static uint32_t fm_bl(size_t from, size_t to) { return 0x94000000u | ((uint32_t)(((int64_t)to - (int64_t)from) / 4) & 0x03ffffff); }
static float fm_envf(const char *name, float def, float lo, float hi) {
    const char *e = getenv(name);
    if (!e || !*e) return def;
    char *end; float v = strtof(e, &end);
    if (*end || !(v >= lo && v <= hi)) { fprintf(stderr, "【红】[totk-mod] fog: %s=%s is not %g..%g: using %g\n", name, e, lo, hi, def); return def; }
    return v;
}
static uint8_t *fm_find_table(size_t *n, char *path, size_t cap) {
    const char *e = getenv("TOTK_FOG_TABLE"), *prof = getenv("TOTK_PROFILE_DIR");
    if (e && *e) { snprintf(path, cap, "%s", e); return tfog_slurp(path, 64 << 20, n); }
    snprintf(path, cap, "%s/fog-map/totk_fog.bin", prof && *prof ? prof : "user-data");
    uint8_t *d = tfog_slurp(path, 64 << 20, n);
    char dir[1024];
    if (d || !getcwd(dir, sizeof dir)) return d;
    for (char *slash; (slash = strrchr(dir, '/')) && slash != dir; *slash = 0) {   /* <app>/Contents/Resources/totk/... */
        if (strcmp(slash, "/Resources")) continue;
        snprintf(path, cap, "%s/totk_fog.bin", dir);
        return tfog_slurp(path, 64 << 20, n);
    }
    return NULL;
}
/* 0 installed; else why not (nothing patched) */
static const char *fm_install_at(Module *m, size_t stub) {
    uint8_t *b = m->base;
    if (stub + PAD_V143_FOG_LEN > m->ro_off) return "no veneer space";
    for (unsigned i = 0; i < PAD_V143_FOG_LEN; i++) if (b[stub + i]) return "veneer space occupied";
    uint32_t w;
    memcpy(&w, b + FM_SITE_L, 4);
    if (w != fm_bl(FM_SITE_L, FM_FN_L)) return "unexpected word at the Hero's Path site";
    memcpy(&w, b + FM_SITE_A, 4); if (fm.on_a && w != fm_bl(FM_SITE_A, FM_FN_A)) { fm.on_a = 0; fprintf(stderr, "【红】[totk-mod] fog: unexpected word at the shrine icon site: shrines off\n"); }
    memcpy(&w, b + FM_SITE_B, 4); if (fm.on_b && w != fm_bl(FM_SITE_B, FM_FN_B)) { fm.on_b = 0; fprintf(stderr, "【红】[totk-mod] fog: unexpected word at the tower name site: tower shares off\n"); }
    fm.base = b;
    fm.ret[0] = (uintptr_t)(b + FM_SITE_L + 4); fm.ret[1] = (uintptr_t)(b + FM_SITE_A + 4); fm.ret[2] = (uintptr_t)(b + FM_SITE_B + 4);
    fm.layer_fn = (void *)(b + FM_FN_L); fm.visit_fn = (void *)(b + FM_FN_A); fm.name_fn = (void *)(b + FM_FN_B);
    fm.gate_ref = (void *)(b + FM_GATE_REF); fm.gate_get = (void *)(b + FM_GATE_GET);
    g_totk_fog_text[0] = (uint64_t)b; g_totk_fog_text[1] = (uint64_t)(b + m->text_end);
    uint32_t code[4] = {0x58000050, 0xd61f0200, 0, 0};   /* ldr x16, #8 ; br x16 ; .quad hook */
    void *hook = runtime_hle_wrap_named((void *)totk_fog_hook_entry, "fog map hooks");
    memcpy(code + 2, &hook, sizeof hook); memcpy(b + stub, code, sizeof code);
    w = fm_bl(FM_SITE_L, stub); memcpy(b + FM_SITE_L, &w, 4);
    if (fm.on_a) { w = fm_bl(FM_SITE_A, stub); memcpy(b + FM_SITE_A, &w, 4); }
    if (fm.on_b) { w = fm_bl(FM_SITE_B, stub); memcpy(b + FM_SITE_B, &w, 4); }
    return NULL;
}
static void fog_map_install(Module *m) {
    const char *e = getenv("TOTK_FOG");
    if (getenv("TOTK_EXPORT_IOS")) return;
    if (e && !strcmp(e, "0")) { fprintf(stderr, "[totk-mod] fog map off (TOTK_FOG=0)\n"); return; }
    char path[1200];
    size_t n = 0;
    uint8_t *d = fm_find_table(&n, path, sizeof path);
    const char *why = d ? tfog_table_parse(&fm.tab, d, n) : "missing";
    if (why) { fprintf(stderr, "[totk-mod] fog map off: table %s: %s\n", path, why); free(d); return; }
    const char *sh = getenv("TOTK_FOG_SHRINES"), *dim = getenv("TOTK_FOG_SHRINE_DIM"), *log = getenv("TOTK_FOG_LOG");
    fm.on_a = fm.on_b = !(sh && !strcmp(sh, "0"));
    fm.dim = !(dim && !strcmp(dim, "0"));
    fm.log = log && atoi(log);
    fm.pct = (int)fm_envf("TOTK_FOG_SHRINE_PCT", 65, 0, 100);
    float radius = fm_envf("TOTK_FOG_RADIUS", 60, 10, 2000), dark = fm_envf("TOTK_FOG_DARK", 0.3f, 0, 1);
    tfog_state_init(&fm.st, radius, 2 * TFOG_CELL, 400);
    why = fm_install_at(m, ((m->text_end + 15) & ~15ULL) + PAD_V143_FOG);
    if (!why && !totk_fog_gfx_start(d, n, dark, fm.log)) why = "the map side did not start";   /* the veneer stays, the sites go back */
    if (why) {
        if (fm.base) {
            uint32_t w = fm_bl(FM_SITE_L, FM_FN_L); memcpy(m->base + FM_SITE_L, &w, 4);
            if (fm.on_a) { w = fm_bl(FM_SITE_A, FM_FN_A); memcpy(m->base + FM_SITE_A, &w, 4); }
            if (fm.on_b) { w = fm_bl(FM_SITE_B, FM_FN_B); memcpy(m->base + FM_SITE_B, &w, 4); }
        }
        fprintf(stderr, "【红】[totk-mod] fog map off: %s\n", why);
        free(d); memset(&fm.tab, 0, sizeof fm.tab);
        return;
    }
    fm.on = 1;
    pthread_t th;
    if (!pthread_create(&th, NULL, fm_loop, NULL)) pthread_detach(th);
    fprintf(stderr, "[totk-mod] fog map on (1.4.3, table %s): reveal radius %.0f m, unvisited keeps %.0f%%; shrines %s at %d%% of a region%s (TOTK_FOG=0 off)\n",
            path, radius, dark * 100, fm.on_a ? "on" : "off", fm.pct, fm.on_b ? ", tower shares on the big map" : "");
}
#else
static void fog_map_install(Module *m) { (void)m; }
#endif
