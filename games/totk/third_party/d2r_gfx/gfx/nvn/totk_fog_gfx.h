/* Tears of the Kingdom 1.4.3 fog-of-war map, Metal side (local mod; port of BOTW local patch 0004's botw_fog_map.h).
   Included at the end of nvn_mtl.m for every TOTK build on the Mac (not Breath of the Wild, not the Switch 2 games,
   not iOS). It stays idle until the 1.4.3 runtime (games/totk/runtime/fog_map_v143.h) calls totk_fog_gfx_start; the
   Metal objects are made on first use, once the game made its device.

   - Upload hook (FOG_TP_UPLOADED in nvn_obj.h, replay thread): a texture of a map tile's shape (NVN 0x94 at 3000 px,
     0x8E at 840 / 300 / 135 / 27 px; square, one level, 2D, sRGB ASTC) gets a tile object, its NvPkg's `fog`. A serial
     queue keys the image (tfog_key, totk_fog_core.h) and looks it up in totk_fog.bin; an unknown or ambiguous key gets
     no fog. A known tile is deswizzled into a non-sRGB ASTC copy (sampled values = the encoded values).
   - Build (own command queue): kernel totk_fog_bc1 (totk_fog_kernel.h) darkens the copy by the layer's grid snapshot
     into BC1 blocks; a blit puts them into a private BC1 sRGB texture with the guest texture's swizzle; the completion
     handler publishes it as `next` and bumps the serial, so the texture-pool slots that view the tile whole pick it
     (FOG_TP_BASE / FOG_TP_VIEW -> totk_fog_pick). A tile rebuilds at most every 2 s, only when its grid changed.
   - Lifetime: the tile dies with its NvPkg (ARC) and releases its picked texture through defer_obj_release (which also
     takes it out of the residency set). A re-upload retires the old tile at once. Threads that walk the weak tile table
     do it inside an autorelease pool (BOTW 0004, 2026-10-27: without one no tile was ever freed). The 30 s log line
     counts live tiles. */
#pragma once
#include "../../../../runtime/totk_fog_core.h"
#include "totk_fog_kernel.h"
#include <os/lock.h>

int g_totk_fog;   /* 0 idle (every build until the 1.4.3 runtime starts it), 1 on */
static tfog_table g_tf_tab;
static float g_tf_dark = 0.3f;
static int g_tf_log;
static dispatch_queue_t g_tf_q;   /* identify, deswizzle, encode builds, 1 Hz kicks */
static dispatch_source_t g_tf_timer;
static id<MTLCommandQueue> g_tf_cq;
static id<MTLComputePipelineState> g_tf_ps;
static os_unfair_lock g_tf_lk = OS_UNFAIR_LOCK_INIT;   /* tile table and tile state, grid buffers, counters */
static NSHashTable *g_tf_tiles;   /* live tiles (weak) */
static id<MTLBuffer> g_tf_grid[TFOG_LAYERS];   /* the latest grid of each layer (never written after it is set) */
static int g_tf_ready;   /* Metal objects: 0 not yet made, 1 ready, -1 failed */
static int tf_metal_ready(void);
static void tf_tick(void);
static uint64_t g_tf_n_up, g_tf_n_known, g_tf_n_unknown, g_tf_n_amb, g_tf_n_built, g_tf_n_retired, g_tf_n_fail;

@interface TotkFogTile : NSObject { @public
    uint64_t ser;
    uint32_t w, h;
    int zoom, layer, col, row, known, dead, busy, dirty;
    double t_built;
    MTLTextureSwizzleChannels swz;
    id<MTLTexture> src;         /* non-sRGB ASTC copy of the guest image (g_tf_q) */
    id<MTLTexture> next, cur;   /* next: built, not yet picked (g_tf_lk); cur: in use (replay thread) */
} @end
@implementation TotkFogTile
- (void)dealloc { if (cur) defer_obj_release(cur); }
@end

static double tf_now(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }
static int tf_zoom_of(const tex_t *t) {
    for (int z = 0; z < TFOG_ZOOMS; z++)
        if (t->d.width == tfog_zoom_px[z]) return t->d.format == (z ? 0x8E : 0x94) ? z : -1;
    return -1;
}

/* g_tf_q: one build of a known tile from its layer's current grid */
static void tf_build(TotkFogTile *tl) {
    os_unfair_lock_lock(&g_tf_lk);
    id<MTLBuffer> grid = g_tf_grid[tl->layer];
    os_unfair_lock_unlock(&g_tf_lk);
    uint32_t bw = (tl->w + 3) / 4, bh = (tl->h + 3) / 4;
    id<MTLBuffer> out = [g_dev newBufferWithLength:(NSUInteger)bw * bh * 8 options:MTLResourceStorageModePrivate];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA_sRGB width:tl->w height:tl->h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; d.swizzle = tl->swz;
    id<MTLTexture> dst = [g_dev newTextureWithDescriptor:d];
    id<MTLCommandBuffer> cb = grid && out && dst ? [g_tf_cq commandBuffer] : nil;
    if (!cb) { os_unfair_lock_lock(&g_tf_lk); tl->busy = 0; g_tf_n_fail++; os_unfair_lock_unlock(&g_tf_lk); return; }
    totk_fog_encode(cb, g_tf_ps, tl->src, grid, out, tl->w, tl->h, -6000.0f + 1000 * tl->col, -5000.0f + 1000 * tl->row, g_tf_dark);
    totk_fog_blit(cb, out, dst, tl->w, tl->h);
    double t0 = tf_now();
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        int ok = done.status == MTLCommandBufferStatusCompleted, live;
        uint64_t n = 0;
        os_unfair_lock_lock(&g_tf_lk);
        live = ok && !tl->dead;
        if (live) { tl->next = dst; if (!tl->t_built) n = ++g_tf_n_built; tl->t_built = tf_now(); }
        if (!ok) g_tf_n_fail++;
        tl->busy = 0;
        os_unfair_lock_unlock(&g_tf_lk);
        if (live) ser_bump(tl->ser);   // the slots of this serial validate again and pick the new texture
        if (!ok) MLOG("【红】[fog] tile build failed: %s", done.error.description.UTF8String);
        else if (live && ((n && n <= 3) || g_tf_log))
            MLOG("[fog] tile %c %02d-%02d zoom %d %ux%u %s in %.1f ms", "SGU"[tl->layer], tl->col, tl->row, tl->zoom, tl->w, tl->h, n ? "built" : "rebuilt", (tf_now() - t0) * 1e3);
    }];
    [cb commit];
}
/* g_tf_lk held: start a build when the tile is known and none runs; now = 0 keeps the 2 s gap and needs dirty */
static void tf_kick_locked(TotkFogTile *tl, int now) {
    if (tl->dead || tl->busy || !tl->known) return;
    if (!now && (!tl->dirty || tf_now() - tl->t_built < 2)) return;
    tl->busy = 1; tl->dirty = 0;
    dispatch_async(g_tf_q, ^{ @autoreleasepool { tf_build(tl); } });
}

/* replay thread (the only one that touches cur): the tile is no longer used */
static void tf_retire(TotkFogTile *tl) {
    os_unfair_lock_lock(&g_tf_lk); tl->dead = 1; tl->next = nil; g_tf_n_retired++; os_unfair_lock_unlock(&g_tf_lk);
    if (tl->cur) { defer_obj_release(tl->cur); tl->cur = nil; }
}

/* nvn_obj.h FOG_TP_UPLOADED (replay thread): a first upload or a re-upload of a mirrored texture */
void totk_fog_uploaded(void *pk_, const tex_t *t) {
    NvPkg *pk = (__bridge NvPkg *)pk_;
    if (pk->fog) { tf_retire(pk->fog); pk->fog = nil; }
    int zoom = tf_zoom_of(t);
    if (zoom < 0) return;
    uint32_t isz = g_tf_tab.img_size[zoom];
    if (t->d.height != t->d.width || t->d.levels > 1 || (t->d.target != 1 && t->d.target != 7) || !t->mem || !isz || t->L.size < isz
        || !pk->tex || pk->tex.mipmapLevelCount != 1 || pk->tex.textureType != MTLTextureType2D) return;
    uint8_t *raw = malloc(t->L.size);
    if (!raw) return;
    if (!guarded_memcpy(raw, t->mem, t->L.size, "地图迷雾贴图")) { free(raw); return; }   // the game may have taken the pages away since the upload
    TotkFogTile *tl = [TotkFogTile new];
    tl->ser = t->serial; tl->w = t->d.width; tl->h = t->d.height; tl->zoom = zoom; tl->swz = pk->tex.swizzle;
    pk->fog = tl;
    nvn_texdesc desc = t->d;
    os_unfair_lock_lock(&g_tf_lk); [g_tf_tiles addObject:tl]; g_tf_n_up++; os_unfair_lock_unlock(&g_tf_lk);
    dispatch_async(g_tf_q, ^{ @autoreleasepool {
        if (!tf_metal_ready()) { free(raw); return; }
        tfog_tile e;
        int found = tfog_table_find(&g_tf_tab, tfog_key(raw, isz), &e), usable = found && !e.ambiguous && e.zoom == zoom;
        if (!usable) {
            free(raw);
            os_unfair_lock_lock(&g_tf_lk);
            uint64_t n = found && e.ambiguous ? ++g_tf_n_amb : ++g_tf_n_unknown;
            os_unfair_lock_unlock(&g_tf_lk);
            if (n <= 3 || g_tf_log) MLOG("[fog] map-shaped texture %ux%u (zoom %d): %s, no fog on it", desc.width, desc.height, zoom,
                                         !found ? "not in the table" : e.ambiguous ? "its image is in more than one cell" : "zoom differs from the table");
            return;
        }
        int bwx, bhx;
        nvn_fmt_bpb(desc.format, &bwx, &bhx);
        size_t pitch = (size_t)((desc.width + bwx - 1) / bwx) * 16, nb = pitch * ((desc.height + bhx - 1) / bhx);
        uint8_t *lin = malloc(nb);
        int bad = !lin || nvn_tex_is_linear(&desc) || nvn_deswizzle(&desc, 0, 0, raw, lin, pitch);
        free(raw);
        id<MTLTexture> src = nil;
        if (!bad) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:zoom ? MTLPixelFormatASTC_8x8_LDR : MTLPixelFormatASTC_12x12_LDR
                                                                                       width:desc.width height:desc.height mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
            src = [g_dev newTextureWithDescriptor:d];
            [src replaceRegion:MTLRegionMake2D(0, 0, desc.width, desc.height) mipmapLevel:0 withBytes:lin bytesPerRow:pitch];
        }
        free(lin);
        os_unfair_lock_lock(&g_tf_lk);
        if (!src) g_tf_n_fail++;
        else { tl->src = src; tl->layer = e.layer; tl->col = e.col; tl->row = e.row; tl->known = 1; g_tf_n_known++; tf_kick_locked(tl, 1); }
        os_unfair_lock_unlock(&g_tf_lk);
        if (!src) MLOG("【红】[fog] map tile %ux%u: deswizzle failed", desc.width, desc.height);
    }});
}

/* nvn_mtl.m tex_by_id (replay thread, g_rmu held): the fog texture of this slot's texture, or base */
static int tf_slot_ok(const treg_t *rp) {   /* the slot views the whole texture in its own format, one 2D level and layer */
    if (!rp->has_v) return 1;
    const view_t *v = &rp->v;
    return !(v->set_format && v->format != rp->t.d.format) && !(v->set_levels && (v->base_level || v->nlevels > 1)) && !(v->set_layers && (v->min_layer || v->nlayers > 1))
        && !(v->set_target && ttype(v->target, MTLTextureType2D) != MTLTextureType2D);
}
void *totk_fog_pick(void *rp_, void *base_) {
    if (g_totk_fog <= 0 || !base_) return base_;
    treg_t *rp = rp_;
    __unsafe_unretained id o = serial_get(rp->t.serial);
    if (![o isKindOfClass:[NvPkg class]]) return base_;
    NvPkg *pk = o;
    __unsafe_unretained TotkFogTile *tl = pk->fog;
    if (!tl || (__bridge void *)pk->tex != base_ || pk->owned) return base_;   // no tile, a rendering read in its place, or the GPU renders into it
    os_unfair_lock_lock(&g_tf_lk); id<MTLTexture> nt = tl->next; tl->next = nil; os_unfair_lock_unlock(&g_tf_lk);
    if (nt) { if (tl->cur) defer_obj_release(tl->cur); tl->cur = nt; res_add(nt); }
    return tl->cur && tf_slot_ok(rp) ? (__bridge void *)tl->cur : base_;
}

/* fog_map_v143.h fog thread: a layer's grid changed in the box of cells (inclusive); x0 > x1 = all. 0: Metal is not
   ready yet (no device): the caller keeps the box and pushes again. */
int totk_fog_gfx_grid(int layer, const uint8_t *grid, int x0, int z0, int x1, int z1) { @autoreleasepool {
    if (g_totk_fog <= 0 || layer < 0 || layer >= TFOG_LAYERS) return 1;
    if (!__atomic_load_n(&g_tf_ready, __ATOMIC_ACQUIRE)) { if (!g_dev) return 0; dispatch_sync(g_tf_q, ^{ tf_metal_ready(); }); }
    if (g_tf_ready < 0) return 1;
    id<MTLBuffer> b = [g_dev newBufferWithBytes:grid length:TFOG_NCELLS options:MTLResourceStorageModeShared];
    if (!b) return 0;
    int all = x0 > x1;
    /* + 1 cell: the bilinear reach of a changed cell */
    float bx0 = TFOG_X0 + (x0 - 1) * (float)TFOG_CELL, bx1 = TFOG_X0 + (x1 + 2) * (float)TFOG_CELL, bz0 = TFOG_Z0 + (z0 - 1) * (float)TFOG_CELL, bz1 = TFOG_Z0 + (z1 + 2) * (float)TFOG_CELL;
    os_unfair_lock_lock(&g_tf_lk);
    g_tf_grid[layer] = b;
    for (TotkFogTile *tl in g_tf_tiles) {
        if (tl->dead || !tl->known || tl->layer != layer) continue;
        float tx0 = -6000.0f + 1000 * tl->col, tz0 = -5000.0f + 1000 * tl->row;
        if (all) { tl->dirty = 1; tl->t_built = 0; }   // a slot load: rebuild at the next tick
        else if (bx1 > tx0 && bx0 < tx0 + 1000 && bz1 > tz0 && bz0 < tz0 + 1000) tl->dirty = 1;
    }
    os_unfair_lock_unlock(&g_tf_lk);
    return 1;
}}

/* g_tf_q, once a second: start due builds; every 30 s a count line */
static void tf_tick(void) {
    static uint64_t tick;
    os_unfair_lock_lock(&g_tf_lk);
    unsigned long live = 0, known = 0, dead = 0;
    uint64_t kb = 0;
    for (TotkFogTile *tl in g_tf_tiles) {
        if (tl->dead) { dead++; continue; }
        live++;
        if (tl->known) { known++; kb += ((uint64_t)((tl->w + 3) / 4) * ((tl->h + 3) / 4) * 8 + (tl->src ? 16 * (uint64_t)((tl->w + 7) / 8) * ((tl->h + 7) / 8) : 0)) >> 10; }
        tf_kick_locked(tl, 0);
    }
    if (++tick % 30 == 0)
        MLOG("[fog] tiles: uploads %llu, identified %llu, not in the table %llu, ambiguous %llu, built %llu, failed %llu; live %lu (%lu with fog, about %llu MB), retired %llu (%lu not yet freed)",
             (unsigned long long)g_tf_n_up, (unsigned long long)g_tf_n_known, (unsigned long long)g_tf_n_unknown, (unsigned long long)g_tf_n_amb,
             (unsigned long long)g_tf_n_built, (unsigned long long)g_tf_n_fail, live, known, (unsigned long long)(kb >> 10), (unsigned long long)g_tf_n_retired, dead);
    os_unfair_lock_unlock(&g_tf_lk);
}

/* g_tf_q: the Metal objects, made on first use (the device may not exist yet when the runtime starts the mod) */
static int tf_metal_ready(void) {
    if (g_tf_ready || !g_dev) return g_tf_ready > 0;
    NSError *err = nil;
    g_tf_ps = totk_fog_pipeline(g_dev, &err);
    g_tf_cq = [g_dev newCommandQueue];
    if (!g_tf_ps || !g_tf_cq) { g_tf_ready = -1; MLOG("【红】[fog] kernel: %s: no fog on the map", err.description.UTF8String); return 0; }
    g_tf_cq.label = @"totk.fog";
    static uint8_t zero[TFOG_NCELLS];
    os_unfair_lock_lock(&g_tf_lk);
    for (int l = 0; l < TFOG_LAYERS; l++) if (!g_tf_grid[l]) g_tf_grid[l] = [g_dev newBufferWithBytes:zero length:sizeof zero options:MTLResourceStorageModeShared];
    os_unfair_lock_unlock(&g_tf_lk);
    g_tf_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_tf_q);
    dispatch_source_set_timer(g_tf_timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(g_tf_timer, ^{ @autoreleasepool { tf_tick(); } });
    dispatch_resume(g_tf_timer);
    g_tf_ready = 1;
    return 1;
}
/* fog_map_v143.h: the table bytes (copied), what unvisited places keep of their colour, log every build. 1 when on. */
int totk_fog_gfx_start(const void *table, size_t n, float dark, int log) {
    if (g_totk_fog > 0) return 1;
    void *copy = malloc(n ? n : 1);
    const char *why = copy ? (memcpy(copy, table, n), tfog_table_parse(&g_tf_tab, copy, n)) : "out of memory";
    if (why) { free(copy); MLOG("【红】[fog] fog table: %s: fog map off", why); return 0; }
    g_tf_dark = dark; g_tf_log = log;
    g_tf_tiles = [NSHashTable weakObjectsHashTable];
    g_tf_q = dispatch_queue_create("totk.fog", dispatch_queue_attr_make_with_autorelease_frequency(DISPATCH_QUEUE_SERIAL, DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM));
    __atomic_store_n(&g_totk_fog, 1, __ATOMIC_RELEASE);
    MLOG("[fog] map tiles: %u keys (%u shrines, %u towers in the table); unvisited places keep %.0f%% of their colour", g_tf_tab.ntiles, g_tf_tab.nshrines, g_tf_tab.ntowers, dark * 100);
    return 1;
}
