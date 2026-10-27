/* Breath of the Wild 1.9.0: fog-of-war map (local build only). Places Link has been are clear on the mini map and the
   big map, every other place is darker. Included at the end of nvn_mtl.m after botw_texpack.h, whose hook it uses.

   - Map tiles: UI/MapTex/MainField/MapTex{,1,2,3}_<col>-<row>[_<variant>] (3000², 840², 300², 135² BC1 sRGB, one
     level). When the game opens one (romfs.c), a queue reads it, decodes Yaz0 + BNTX and keys its image by pkg_hash:
     the key of the texture the game uploads from it. A key two cells share (blank edge cells) gets no fog.
   - A mirrored texture of a tile's shape (botw_tp_uploaded) gets a tile object: the guest bytes deswizzled once into
     linear BC1 blocks. A build darkens a copy block by block from the visited grid (botw_fog_core.h) into a new BC1
     texture; botw_tp_pick samples it in place of the guest's copy for every slot that views the texture whole, so the
     mini map and the big map both show it. The game's own reveal mask still applies on top.
   - Position: [main+0x1e13ed0] is PlayerInfo (its getPlayerPos at main+0xcc7598 returns this+0x88, a float[3]);
     StageInfo::sCurrentMapType at main+0x1dae3a0 (FixedSafeString, char * at +8) reads "MainField" in the field. A
     thread reads both once a second; field places paint the grid (shrines use their own coordinates and do not).
   - Save slots: the game reading save:/N/game_data.sav loads slot N's grid (<profile>/fog-map/<user>/slotN.grid);
     the game writing it saves the live grid to slot N. Every load also paints Hero's Path into the grid
     (save/<user>/tracker/trackblock{,_hard}NN.sav; slots 6-7 are Master Mode), so a slot without a grid starts from
     it, and a grid older than its save (the save written by a run without the fog map) gets back the trail walked
     since (owner, 2026-10-27: a Hateno trail walked before its tower was gone after the unlock, because the loaded
     slot's grid was 2 h older than its save). The game's save files are only read.
   - A re-upload of a tile retires the old tile and releases its fog texture at once. The position thread and the save
     hook walk the weak tile table inside an autorelease pool: each weak read autoreleases the tile, and without a pool
     on these threads no tile was ever freed (owner's 4.9 h run, 2026-10-27: 1399 fog textures, 746 MB, GPU budget abort).
   - BOTW_FOG=0 off; BOTW_FOG_DARK (0.3: what is left of unvisited colour), BOTW_FOG_RADIUS (48 m, plus a 32 m soft edge); BOTW_FOG_LOG=1
     logs the position once a second. */
#include "botw_fog_core.h"
#include <mach/mach.h>
#include "mach_vm_compat.h"   /* nvn/include */

enum { FOG_PINFO = 0x1e13ed0, FOG_PINFO_POS = 0x88, FOG_MAPTYPE = 0x1dae3a0, FOG_EDGE = 2 * FOG_CELL, FOG_GAP = 250 };
static int g_fog = -1;   /* -1 not yet set up, 0 off, 1 on */
static float g_fog_dark = 0.3f, g_fog_radius = 48;
static int g_fog_log;
static uintptr_t g_fog_main;
static dispatch_queue_t g_fog_dq, g_fog_bq;   /* decode, build */
static os_unfair_lock g_fog_lk = OS_UNFAIR_LOCK_INIT;   /* table, tiles, tile state, counters */
static NSMutableDictionary<NSNumber *, NSNumber *> *g_fog_keys;   /* hash -> cell (level << 16 | (col + 2) << 8 | row, never 0; -1 shared) */
static NSMutableSet<NSString *> *g_fog_seen;   /* tile files opened */
static NSHashTable *g_fog_tiles;   /* live tiles (weak) */
static uint64_t g_fog_n_open[4], g_fog_n_key[4], g_fog_n_up[4], g_fog_n_built[4], g_fog_n_retired;
static pthread_mutex_t g_fog_gmu = PTHREAD_MUTEX_INITIALIZER;   /* grid, slot */
static uint8_t g_fog_grid[FOG_GW * FOG_GH];
static uint64_t g_fog_ggen, g_fog_epoch;   /* grid generation; slot loads */
static int g_fog_slot = -1, g_fog_have_prev;
static float g_fog_prev[2];

@interface BotwFogTile : NSObject { @public
    uint64_t hash, ser;
    uintptr_t lo, hi;   /* the guest memory the tile was uploaded from */
    uint32_t w, h; int level, cell, dead, busy, dirty, warned;
    double t_up, t_built;
    uint8_t *blocks; MTLPixelFormat pf; MTLTextureSwizzleChannels swz;
    id<MTLTexture> next, cur;   /* next: built, not yet picked (g_fog_lk); cur: in use (replay thread) */
} @end
@implementation BotwFogTile
- (void)dealloc { free(blocks); if (cur) defer_obj_release(cur); }
@end

static double fog_now(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }
static int fog_on(void) {
    if (g_fog >= 0) return g_fog;
    const char *e = getenv("BOTW_FOG"), *d = getenv("BOTW_FOG_DARK"), *r = getenv("BOTW_FOG_RADIUS"), *l = getenv("BOTW_FOG_LOG");
    g_fog = !e || atoi(e) ? 1 : 0;
    if (d && *d) { float v = strtof(d, NULL); if (v >= 0 && v <= 1) g_fog_dark = v; }
    if (r && *r) { float v = strtof(r, NULL); if (v >= 10 && v <= 2000) g_fog_radius = v; }
    g_fog_log = l && atoi(l);
    if (g_fog) {
        g_fog_dq = dispatch_queue_create("botw.fog.decode", DISPATCH_QUEUE_SERIAL);
        g_fog_bq = dispatch_queue_create("botw.fog.build", DISPATCH_QUEUE_SERIAL);
        g_fog_keys = [NSMutableDictionary dictionary]; g_fog_seen = [NSMutableSet set]; g_fog_tiles = [NSHashTable weakObjectsHashTable];
    }
    return g_fog;
}
static int fog_level_of(uint32_t w) { for (int l = 0; l < 4; l++) if (fog_level_size[l] == w) return l; return -1; }

static void fog_kick_locked(BotwFogTile *tl, int now);
/* Build queue: darken a copy of the tile's blocks from a grid snapshot into a new texture; the next pick takes it. */
static void fog_build(BotwFogTile *tl) {
    dispatch_async(g_fog_bq, ^{
        double t0 = fog_now();
        uint8_t *g = malloc(sizeof g_fog_grid); size_t nb = (size_t)((tl->w + 3) / 4) * ((tl->h + 3) / 4) * 8; uint8_t *b = malloc(nb);
        if (!g || !b) { free(g); free(b); os_unfair_lock_lock(&g_fog_lk); tl->busy = 0; os_unfair_lock_unlock(&g_fog_lk); return; }
        pthread_mutex_lock(&g_fog_gmu); memcpy(g, g_fog_grid, sizeof g_fog_grid); uint64_t gen = g_fog_ggen, epoch = g_fog_epoch; pthread_mutex_unlock(&g_fog_gmu);
        memcpy(b, tl->blocks, nb);
        int col = (tl->cell >> 8 & 0xff) - 2, row = tl->cell & 0xff;
        fog_bc1_tile(b, tl->w, tl->h, -5000.0f + 1000 * col, -5000.0f + 1000 * row, g, g_fog_dark);
        free(g);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tl->pf width:tl->w height:tl->h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared; d.swizzle = tl->swz;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
        if (t) [t replaceRegion:MTLRegionMake2D(0, 0, tl->w, tl->h) mipmapLevel:0 withBytes:b bytesPerRow:(NSUInteger)((tl->w + 3) / 4) * 8];
        free(b);
        pthread_mutex_lock(&g_fog_gmu); int stale = epoch != g_fog_epoch;   // a slot loaded meanwhile; held until published (lock order: grid, then tiles)
        os_unfair_lock_lock(&g_fog_lk);
        int live = !tl->dead && t && !stale; uint64_t n = 0;
        if (live) { tl->next = t; if (!tl->t_built) n = ++g_fog_n_built[tl->level]; tl->t_built = fog_now(); }
        tl->busy = 0;
        if (stale && !tl->dead) fog_kick_locked(tl, 1);
        os_unfair_lock_unlock(&g_fog_lk);
        pthread_mutex_unlock(&g_fog_gmu);
        if (!live) return;
        ser_bump(tl->ser);   // the slots of this serial validate again and pick the new texture
        if (!n && g_fog_log) MLOG("[fog] tile %ux%u cell %c-%d rebuilt in %.1f ms (grid generation %llu)", tl->w, tl->h, "ZABCDEFGHIJK"[col + 1], row, (fog_now() - t0) * 1e3, (unsigned long long)gen);
        if (n && (n <= 3 || g_fog_log)) MLOG("[fog] tile %ux%u cell %c-%d built in %.1f ms (%llu at this size)", tl->w, tl->h, "ZABCDEFGHIJK"[col + 1], row, (fog_now() - t0) * 1e3, (unsigned long long)n);
    });
}
/* g_fog_lk held: start a build when the tile knows its cell and none runs; rebuilds at most every 2 s */
static void fog_kick_locked(BotwFogTile *tl, int now) {
    if (tl->dead || tl->busy || tl->cell <= 0 || !tl->blocks) return;
    if (!now && (!tl->dirty || fog_now() - tl->t_built < 2)) return;
    tl->busy = 1; tl->dirty = 0; fog_build(tl);
}

/* romfs.c runtime_romfs_open (any thread): the game opens a map tile file */
void *runtime_romfs_slurp(const char *path, size_t *n);
void botw_fog_rom_opened(const char *path) {
    fog_cell c;
    if (!fog_on() || !fog_parse_tile(path, &c)) return;
    NSString *name = @(path);
    os_unfair_lock_lock(&g_fog_lk); int fresh = ![g_fog_seen containsObject:name]; if (fresh) { [g_fog_seen addObject:name]; g_fog_n_open[c.level]++; } os_unfair_lock_unlock(&g_fog_lk);
    if (!fresh) return;
    char *p = strdup(path);
    dispatch_async(g_fog_dq, ^{
        size_t rn = 0, dn = 0, io, is; uint32_t w, h; uint8_t *raw = runtime_romfs_slurp(p, &rn), *d = raw ? fog_yaz0(raw, rn, &dn) : NULL;
        free(raw);
        if (!d || !fog_bntx_image(d, dn, &io, &is, &w, &h) || w != fog_level_size[c.level]) { MLOG("【红】[fog] %s: not a map tile (Yaz0 BNTX)", p); free(d); free(p); return; }
        NSNumber *key = @(pkg_hash(d + io, is)); free(d); free(p);
        int cell = c.level << 16 | (c.col + 2) << 8 | c.row;
        os_unfair_lock_lock(&g_fog_lk);
        NSNumber *had = g_fog_keys[key];
        if (!had) { g_fog_keys[key] = @(cell); g_fog_n_key[c.level]++; }
        else if (had.intValue != cell) g_fog_keys[key] = @(-1);
        for (BotwFogTile *tl in g_fog_tiles) if (!tl->cell && tl->hash == key.unsignedLongLongValue) { tl->cell = g_fog_keys[key].intValue; fog_kick_locked(tl, 1); }
        os_unfair_lock_unlock(&g_fog_lk);
    });
}

/* replay thread (the only one that touches cur): the tile is no longer used; its textures go now, not with its package */
static void fog_retire(BotwFogTile *tl) {
    os_unfair_lock_lock(&g_fog_lk); tl->dead = 1; tl->next = nil; os_unfair_lock_unlock(&g_fog_lk);   // next: built, never picked, never resident
    if (tl->cur) { defer_obj_release(tl->cur); tl->cur = nil; }
    g_fog_n_retired++;
}
/* botw_texpack.h botw_tp_uploaded (replay thread): a first upload or a re-upload of a mirrored texture */
static void fog_uploaded(NvPkg *pk, const tex_t *t) {
    if (pk->fog) { fog_retire(pk->fog); pk->fog = nil; }
    int level = fog_level_of(t->d.width);
    if (level < 0 || t->d.height != t->d.width || t->d.format != 0x46 || (t->d.target != 1 && t->d.target != 7) || t->d.levels > 1 || !t->mem || !t->L.size
        || !pk->tex || pk->tex.pixelFormat != MTLPixelFormatBC1_RGBA_sRGB || pk->tex.mipmapLevelCount != 1) return;
    double t0 = fog_now();
    BotwFogTile *tl = [BotwFogTile new];
    tl->hash = pkg_hash(t->mem, t->L.size); tl->ser = t->serial; tl->w = t->d.width; tl->h = t->d.height; tl->level = level;
    tl->pf = pk->tex.pixelFormat; tl->swz = pk->tex.swizzle; tl->t_up = t0; tl->lo = (uintptr_t)t->mem; tl->hi = tl->lo + t->L.size;
    uint8_t *raw = malloc(t->L.size); if (!raw) return;
    memcpy(raw, t->mem, t->L.size);
    nvn_texdesc desc = t->d;
    pk->fog = tl;
    os_unfair_lock_lock(&g_fog_lk);
    [g_fog_tiles addObject:tl]; uint64_t n = ++g_fog_n_up[level];
    NSNumber *cell = g_fog_keys[@(tl->hash)]; if (cell) tl->cell = cell.intValue;
    os_unfair_lock_unlock(&g_fog_lk);
    double ms = (fog_now() - t0) * 1e3;
    if (n <= 3 || g_fog_log || ms > 4) MLOG("[fog] map tile %ux%u uploaded (serial %llu, %s, hash + copy %.1f ms)", tl->w, tl->h, (unsigned long long)tl->ser, cell ? "cell known" : "waiting for its file", ms);
    dispatch_async(g_fog_bq, ^{
        size_t pitch = (size_t)((desc.width + 3) / 4) * 8, nb = pitch * ((desc.height + 3) / 4);
        uint8_t *b = malloc(nb);
        if (b && (nvn_tex_is_linear(&desc) || nvn_deswizzle(&desc, 0, 0, raw, b, pitch))) { free(b); b = NULL; }   // the tiles are block-linear
        free(raw);
        if (!b) { MLOG("【红】[fog] map tile %ux%u: deswizzle failed", desc.width, desc.height); return; }
        os_unfair_lock_lock(&g_fog_lk); tl->blocks = b; fog_kick_locked(tl, 1); os_unfair_lock_unlock(&g_fog_lk);
    });
}

/* botw_texpack.h botw_tp_pick (replay thread, g_rmu held): the fog texture of this texture, or nil */
static id<MTLTexture> fog_pick(NvPkg *pk) {
    BotwFogTile *tl = pk->fog;
    if (!tl) return nil;
    os_unfair_lock_lock(&g_fog_lk); id<MTLTexture> nt = tl->next; tl->next = nil; os_unfair_lock_unlock(&g_fog_lk);
    if (nt) { if (tl->cur) defer_obj_release(tl->cur); tl->cur = nt; res_add(nt); }
    return tl->cur;
}

/* fs_host.c open_file: the game reads (loads) or writes (saves) save:/N/game_data.sav */
const char *runtime_fs_profile_dir(void);
const char *runtime_fs_save_root(void);
static int fog_grid_path(int slot, char *out, size_t cap, int make) {
    const char *prof = runtime_fs_profile_dir(), *root = runtime_fs_save_root(), *user = root ? strrchr(root, '/') : NULL;
    if (!prof || !*prof || !user) return 0;
    char dir[1200]; snprintf(dir, sizeof dir, "%s/fog-map", prof); if (make) mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/fog-map%s", prof, user); if (make) mkdir(dir, 0700);
    return snprintf(out, cap, "%s/slot%d.grid", dir, slot) < (int)cap;
}
static const char g_fog_magic[8] = {'B', 'O', 'T', 'W', 'F', 'O', 'G', '1'};
static int fog_grid_load(int slot) {   /* g_fog_gmu held */
    char p[1300]; if (!fog_grid_path(slot, p, sizeof p, 0)) return 0;
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    char m[8]; uint32_t dims[3]; int ok = fread(m, 1, 8, f) == 8 && !memcmp(m, g_fog_magic, 8) && fread(dims, 4, 3, f) == 3 && dims[0] == FOG_GW && dims[1] == FOG_GH && dims[2] == FOG_CELL
        && fread(g_fog_grid, 1, sizeof g_fog_grid, f) == sizeof g_fog_grid;
    fclose(f);
    if (!ok) { memset(g_fog_grid, 0, sizeof g_fog_grid); MLOG("【红】[fog] %s is not a fog grid of this version: starting from Hero's Path", p); }
    return ok;
}
static void fog_grid_save(int slot) {   /* g_fog_gmu held */
    char p[1300], tmp[1320]; if (!fog_grid_path(slot, p, sizeof p, 1)) return;
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", p, (int)getpid());
    FILE *f = fopen(tmp, "wb"); uint32_t dims[3] = {FOG_GW, FOG_GH, FOG_CELL};
    int ok = f && fwrite(g_fog_magic, 1, 8, f) == 8 && fwrite(dims, 4, 3, f) == 3 && fwrite(g_fog_grid, 1, sizeof g_fog_grid, f) == sizeof g_fog_grid;
    if (f && fclose(f)) ok = 0;
    if (!ok || rename(tmp, p)) { unlink(tmp); MLOG("【红】[fog] cannot write %s", p); return; }
    if (g_fog_log) MLOG("[fog] slot %d grid saved", slot);
}
static int fog_import_track(int hard) {   /* g_fog_gmu held: Hero's Path of this mode into the grid */
    const char *root = runtime_fs_save_root(); int points = 0, files = 0;
    for (int k = 0; root && *root && k < 100; k++) {
        char p[1300]; snprintf(p, sizeof p, "%s/tracker/trackblock%s%02d.sav", root, hard ? "_hard" : "", k);
        FILE *f = fopen(p, "rb"); if (!f) break;
        uint8_t *d = malloc(1 << 20); size_t n = d ? fread(d, 1, 1 << 20, f) : 0; fclose(f);
        if (d) { points += fog_paint_track(g_fog_grid, d, n, g_fog_radius, FOG_EDGE, FOG_GAP); files++; }
        free(d);
    }
    MLOG("[fog] Hero's Path (%s): %d points from %d file(s)", hard ? "Master Mode" : "normal", points, files);
    return points;
}
static void fog_mark_all_dirty(void) { @autoreleasepool {   // a guest thread: no pool of its own (see the top)
    os_unfair_lock_lock(&g_fog_lk); for (BotwFogTile *tl in g_fog_tiles) { tl->dirty = 1; fog_kick_locked(tl, 1); } os_unfair_lock_unlock(&g_fog_lk);
} }
void botw_fog_save_opened(const char *path, int write) {
    static const char pre[] = "save:/";
    if (g_fog <= 0 || !path || strncmp(path, pre, sizeof pre - 1)) return;
    const char *s = path + sizeof pre - 1;
    if (s[0] < '0' || s[0] > '9' || strcmp(s + 1, "/game_data.sav")) return;
    int slot = s[0] - '0';
    pthread_mutex_lock(&g_fog_gmu);
    if (write) { fog_grid_save(slot); g_fog_slot = slot; pthread_mutex_unlock(&g_fog_gmu); return; }
    if (slot == g_fog_slot) { g_fog_have_prev = 0; pthread_mutex_unlock(&g_fog_gmu); return; }   // the live slot read again (a check after a save, a retry): the live grid is newer; no line to the respawn
    memset(g_fog_grid, 0, sizeof g_fog_grid);
    int had = fog_grid_load(slot);
    fog_import_track(slot >= 6);   // painting only raises: a stale or missing grid gets the trail the game recorded
    g_fog_slot = slot; g_fog_have_prev = 0; g_fog_ggen++; g_fog_epoch++;
    pthread_mutex_unlock(&g_fog_gmu);
    MLOG("[fog] slot %d loaded (%s): %s", slot, slot >= 6 ? "Master Mode" : "normal", had ? "its fog grid + Hero's Path" : "new grid from Hero's Path");
    fog_mark_all_dirty();
}

/* Position thread */
static int fog_read(uintptr_t a, void *out, size_t n) {
    mach_vm_size_t got = 0;
    return a && mach_vm_read_overwrite(mach_task_self(), a, n, (uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}
static void *fog_loop(void *arg) {
    (void)arg;
    pthread_setname_np("botw.fog.pos");
    for (uint64_t tick = 0;; tick++) @autoreleasepool {   // each weak read of a tile autoreleases it (see the top)
        sleep(1);
        pthread_mutex_lock(&g_fog_gmu); uint64_t epoch = g_fog_epoch; pthread_mutex_unlock(&g_fog_gmu);
        uintptr_t pi = 0, ts = 0; float pos[3] = {0}; char type[16] = {0};
        int ok = fog_read(g_fog_main + FOG_PINFO, &pi, 8) && pi && fog_read(pi + FOG_PINFO_POS, pos, sizeof pos);
        int field = fog_read(g_fog_main + FOG_MAPTYPE + 8, &ts, 8) && ts && fog_read(ts, type, sizeof type - 1) && !strcmp(type, "MainField");
        int valid = ok && isfinite(pos[0]) && isfinite(pos[2]) && (pos[0] || pos[2]) && pos[0] >= FOG_X0 && pos[0] < FOG_X0 + 12000 && pos[2] >= FOG_Z0 && pos[2] < FOG_Z0 + 10000;
        fog_box box = {1, 0, 0, 0}; int slot;
        pthread_mutex_lock(&g_fog_gmu);
        slot = g_fog_slot;
        if (epoch != g_fog_epoch) ;   // a slot loaded while this sample was read: it belongs to the old one
        else if (slot >= 0 && field && valid) {
            int ch = g_fog_have_prev ? fog_paint_line(g_fog_grid, g_fog_prev[0], g_fog_prev[1], pos[0], pos[2], g_fog_radius, FOG_EDGE, FOG_GAP, &box)
                                     : fog_paint_disc(g_fog_grid, pos[0], pos[2], g_fog_radius, FOG_EDGE, &box);
            if (ch) g_fog_ggen++;
            g_fog_prev[0] = pos[0]; g_fog_prev[1] = pos[2]; g_fog_have_prev = 1;
        } else if (!field) g_fog_have_prev = 0;
        pthread_mutex_unlock(&g_fog_gmu);
        if (g_fog_log) MLOG("[fog] pos %.1f %.1f %.1f map '%s' slot %d%s", pos[0], pos[1], pos[2], type, slot, box.x0 <= box.x1 ? " painted" : "");
        /* tiles whose 1 km cell meets the painted cells */
        double now = fog_now();
        os_unfair_lock_lock(&g_fog_lk);
        for (BotwFogTile *tl in g_fog_tiles) {
            if (tl->dead) continue;
            if (box.x0 <= box.x1 && tl->cell > 0) {
                int col = (tl->cell >> 8 & 0xff) - 2, row = tl->cell & 0xff;
                float tx0 = -5000.0f + 1000 * col, tz0 = -5000.0f + 1000 * row;
                float bx0 = FOG_X0 + (box.x0 - 1) * (float)FOG_CELL, bx1 = FOG_X0 + (box.x1 + 2) * (float)FOG_CELL, bz0 = FOG_Z0 + (box.z0 - 1) * (float)FOG_CELL, bz1 = FOG_Z0 + (box.z1 + 2) * (float)FOG_CELL;   // + the bilinear reach
                if (bx1 > tx0 && bx0 < tx0 + 1000 && bz1 > tz0 && bz0 < tz0 + 1000) tl->dirty = 1;
            }
            fog_kick_locked(tl, 0);
            if (!tl->cell && !tl->warned && now - tl->t_up > 5) { tl->warned = 1; MLOG("【红】[fog] map tile %ux%u (serial %llu) has no known file after 5 s: no fog on it", tl->w, tl->h, (unsigned long long)tl->ser); }
        }
        NSArray<BotwFogTile *> *all = tick % 30 == 29 ? g_fog_tiles.allObjects : nil;
        if (all) MLOG("[fog] tiles 3000/840/300/135: files %llu/%llu/%llu/%llu, keys %llu/%llu/%llu/%llu, uploads %llu/%llu/%llu/%llu, built %llu/%llu/%llu/%llu, live %lu",
            g_fog_n_open[0], g_fog_n_open[1], g_fog_n_open[2], g_fog_n_open[3], g_fog_n_key[0], g_fog_n_key[1], g_fog_n_key[2], g_fog_n_key[3],
            g_fog_n_up[0], g_fog_n_up[1], g_fog_n_up[2], g_fog_n_up[3], g_fog_n_built[0], g_fog_n_built[1], g_fog_n_built[2], g_fog_n_built[3], (unsigned long)all.count);
        os_unfair_lock_unlock(&g_fog_lk);
        if (all) {   // what holds the live tiles: their serial finalised, still in the serial table, memory initialised over by a newer tile
            unsigned long live = 0, dead = 0, fin = 0, intab = 0, over = 0; uint64_t mb = 0;
            for (BotwFogTile *tl in all) {
                if (tl->dead) { dead++; continue; }
                live++; mb += (uint64_t)((tl->w + 3) / 4) * ((tl->h + 3) / 4) * 8;
                if (serial_is_dead(tl->ser)) fin++;
                if (serial_get(tl->ser)) intab++;
                for (BotwFogTile *o in all) if (o != tl && o->ser > tl->ser && o->lo < tl->hi && tl->lo < o->hi) { over++; break; }
            }
            MLOG("[fog] tile objects: %lu in use (%llu MB of fog textures; serial finalised %lu, in the serial table %lu, memory used by a newer tile %lu), %lu retired not yet freed; %llu retired so far",
                live, (unsigned long long)(mb >> 20), fin, intab, over, dead, (unsigned long long)g_fog_n_retired);
        }
    }
    return NULL;
}
/* game_patches_botw.c runtime_game_patches: the main module is loaded */
void botw_fog_init(uintptr_t main_base, uint64_t main_size) {
    if (!fog_on()) { MLOG("[fog] off (BOTW_FOG=0)"); return; }
    if (main_size < FOG_PINFO + 8) { g_fog = 0; MLOG("【红】[fog] main module too small: off"); return; }
    g_fog_main = main_base;
    pthread_t th; if (pthread_create(&th, NULL, fog_loop, NULL)) { g_fog = 0; return; }
    pthread_detach(th);
    MLOG("[fog] on: unvisited places keep %.0f%% of their colour, reveal radius %.0f m (BOTW_FOG=0 off)", g_fog_dark * 100, g_fog_radius);
}
