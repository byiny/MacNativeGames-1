/* Tears of the Kingdom 1.4.3 fog-of-war map and sensor shrines (local mod, port of BOTW local patches 0004 and 0011):
   the plain C part. fog_map_v143.h (runtime), gfx/nvn/totk_fog_gfx.h (Metal) and tests/test_totk_fog.c include it.

   - Grids: 3 layers (0 sky, 1 surface, 2 depths) of TFOG_GW x TFOG_GH bytes, TFOG_CELL m per cell, over x -6000..6000
     and z -5000..5000 (the map tiles: 12 x 10 cells of 1 km). 0 = never seen, 255 = clear; painting only raises values.
   - Map tile (col, row): world x = -6000 + 1000 col, z = -5000 + 1000 row; pixel row 0 = the smallest z.
   - totk_fog.bin (dev-data/totk/keep/local-patches/tools/make_totk_fog_table.py): tile keys (first 8 bytes of the
     SHA-256 of a tile's image, as a little-endian u64) -> layer, cell, zoom; the shrines and towers; one tower region
     byte per surface cell (255: none or sea).
   - Hero's Path, save:/slot_NN/footprint.sav: (u32 hash, u32 value) header pairs on 8-byte steps (0x08e8c243 = write
     index, -1 empty; 0x37dc6f6b = wrapped), u32 data offset at +8; at the data: ring size u32 at +0xc, u32 words from
     +0x10. Word: bits 3-4 layer, bit 5 x sign, bits 6-18 |x|, bit 19 z sign, bits 20-31 |z| (metres).
   - Slot state (tfog_state): points from the Hero's Path writer hook go through a small ring with the slot epoch they
     were recorded in; the fog thread drains them into the live grids. A load bumps the epoch (older points are
     dropped), loads the slot's grid file and paints the slot's footprint.sav into it; a save drains the ring first and
     then writes the grid file. */
#pragma once
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

enum {
    TFOG_CELL = 16, TFOG_X0 = -6000, TFOG_Z0 = -5000, TFOG_GW = 12000 / TFOG_CELL, TFOG_GH = 10000 / TFOG_CELL,
    TFOG_LAYERS = 3, TFOG_ZOOMS = 5, TFOG_NCELLS = TFOG_GW * TFOG_GH,
    TFOG_SHRINE_NEAR = 24,     /* m: a shrine shows once a cell this close to it is > 0 (about 1.25 x the fog circle) */
    TFOG_RING = 256, TFOG_TEXT_CAP = 41,
};
static const uint32_t tfog_zoom_px[TFOG_ZOOMS] = {3000, 840, 300, 135, 27};

typedef struct { int x0, z0, x1, z1; } tfog_box;   /* cells, inclusive; x0 > x1 = empty */
static const tfog_box tfog_box_empty = {1, 0, 0, 0};
static const tfog_box tfog_box_all = {0, 0, TFOG_GW - 1, TFOG_GH - 1};
static inline void tfog_box_add(tfog_box *b, tfog_box a) {
    if (a.x0 > a.x1) return;
    if (b->x0 > b->x1) { *b = a; return; }
    if (a.x0 < b->x0) b->x0 = a.x0;
    if (a.z0 < b->z0) b->z0 = a.z0;
    if (a.x1 > b->x1) b->x1 = a.x1;
    if (a.z1 > b->z1) b->z1 = a.z1;
}

/* A disc of radius r (m) fully clear, fading to 0 over edge m; changed cells grow *dirty. Returns 1 if a cell rose. */
static inline int tfog_paint_disc(uint8_t *g, float x, float z, float r, float edge, tfog_box *dirty) {
    if (!isfinite(x) || !isfinite(z) || !(r >= 0) || !(edge > 0)) return 0;
    float far = r + edge, cx = (x - TFOG_X0) / TFOG_CELL, cz = (z - TFOG_Z0) / TFOG_CELL;
    if (cx < -far || cz < -far || cx > TFOG_GW + far || cz > TFOG_GH + far) return 0;
    int i0 = (int)floorf(cx - far / TFOG_CELL), i1 = (int)ceilf(cx + far / TFOG_CELL);
    int j0 = (int)floorf(cz - far / TFOG_CELL), j1 = (int)ceilf(cz + far / TFOG_CELL);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > TFOG_GW - 1) i1 = TFOG_GW - 1;
    if (j1 > TFOG_GH - 1) j1 = TFOG_GH - 1;
    tfog_box b = tfog_box_empty;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        float dx = (i + 0.5f - cx) * TFOG_CELL, dz = (j + 0.5f - cz) * TFOG_CELL, d = sqrtf(dx * dx + dz * dz);
        if (d >= far) continue;
        int v = d <= r ? 255 : (int)(255.0f * (far - d) / edge + 0.5f);
        uint8_t *c = &g[(size_t)j * TFOG_GW + i];
        if (v > *c) { *c = (uint8_t)v; tfog_box_add(&b, (tfog_box){i, j, i, j}); }
    }
    if (dirty) tfog_box_add(dirty, b);
    return b.x0 <= b.x1;
}
/* Discs along a step shorter than max_gap m (a longer one is a warp: only its end is painted). */
static inline int tfog_paint_line(uint8_t *g, float xa, float za, float xb, float zb, float r, float edge, float max_gap, tfog_box *dirty) {
    float dx = xb - xa, dz = zb - za, len = sqrtf(dx * dx + dz * dz);
    if (!(len < max_gap)) return tfog_paint_disc(g, xb, zb, r, edge, dirty);
    int n = (int)ceilf(len / (TFOG_CELL * 0.5f)), changed = 0;
    if (n < 1) n = 1;
    for (int k = 1; k <= n; k++) { float t = (float)k / n; changed |= tfog_paint_disc(g, xa + dx * t, za + dz * t, r, edge, dirty); }
    return changed;
}
/* Bilinear clearness 0..1 at a world place (cell centres hold the values; outside the grid = 0). The Metal kernel
   (totk_fog_kernel.h) samples the same way. */
static inline float tfog_sample(const uint8_t *g, float x, float z) {
    float fx = (x - TFOG_X0) / TFOG_CELL - 0.5f, fz = (z - TFOG_Z0) / TFOG_CELL - 0.5f;
    if (!(fx > -1 && fz > -1 && fx < TFOG_GW && fz < TFOG_GH)) return 0;
    int i = (int)floorf(fx), j = (int)floorf(fz);
    float ax = fx - i, az = fz - j;
#define TFOG_AT(a, b) ((a) < 0 || (b) < 0 || (a) >= TFOG_GW || (b) >= TFOG_GH ? 0.0f : g[(size_t)(b) * TFOG_GW + (a)] / 255.0f)
    float v = (TFOG_AT(i, j) * (1 - ax) + TFOG_AT(i + 1, j) * ax) * (1 - az) + (TFOG_AT(i, j + 1) * (1 - ax) + TFOG_AT(i + 1, j + 1) * ax) * az;
#undef TFOG_AT
    return v;
}

static inline uint32_t tfog_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t tfog_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline float tfog_f32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

/* ---------- Hero's Path ---------- */
typedef struct { float x, z; int layer; } tfog_point;
enum { TFOG_FP_INDEX = 0x08e8c243, TFOG_FP_WRAPPED = 0x37dc6f6b };
/* Word -> point; 0 for an empty word or a layer outside 0..2. */
static inline int tfog_fp_word(uint32_t w, tfog_point *p) {
    int layer = (int)(w >> 3 & 3);
    if (!w || layer > 2) return 0;
    int ax = (int)(w >> 6 & 0x1fff), az = (int)(w >> 20);
    p->x = (float)(w >> 5 & 1 ? -ax : ax); p->z = (float)(w >> 19 & 1 ? -az : az); p->layer = layer;
    return 1;
}
/* The points of a footprint.sav, oldest first, into out (at most cap); returns the count, -1 on a bad file. */
static inline int tfog_fp_parse(const uint8_t *d, size_t n, tfog_point *out, int cap) {
    if (n < 16 || tfog_u32(d) != 0x01020304) return -1;
    uint32_t data = tfog_u32(d + 8);
    if (data < 16 || data > n || n - data < 16) return -1;
    int have_idx = 0, wrapped = 0; int32_t idx = -1;
    for (size_t o = 16; o + 8 <= data; o += 8) {
        uint32_t h = tfog_u32(d + o), v = tfog_u32(d + o + 4);
        if (h == TFOG_FP_INDEX) { idx = (int32_t)v; have_idx = 1; }
        else if (h == TFOG_FP_WRAPPED) wrapped = v & 1;
    }
    uint32_t ring = tfog_u32(d + data + 0xc);
    if (!have_idx || !ring || ring > (n - data - 16) / 4 || idx < -1 || idx >= (int64_t)ring) return -1;
    if (idx < 0) return 0;
    const uint8_t *w = d + data + 16;
    uint32_t first = wrapped ? (uint32_t)idx + 1 : 0, count = wrapped ? ring : (uint32_t)idx + 1;
    int k = 0;
    for (uint32_t i = 0; i < count && k < cap; i++) {
        tfog_point p;
        if (tfog_fp_word(tfog_u32(w + 4 * (size_t)((first + i) % ring)), &p)) out[k++] = p;
    }
    return k;
}

/* ---------- table ---------- */
enum { TFOG_HEAD = 80, TFOG_TILE_REC = 16, TFOG_SHRINE_REC = 16, TFOG_TOWER_REC = 12, TFOG_NO_TOWER = 255 };
typedef struct {
    const uint8_t *d; size_t n;
    uint32_t ntiles, nshrines, ntowers, img_size[TFOG_ZOOMS];
    const uint8_t *tiles, *shrines, *towers, *region;
} tfog_table;
typedef struct { uint64_t key; int layer, col, row, zoom, ambiguous; } tfog_tile;
typedef struct { uint32_t hash; float x, z; int layer, gated, tower; } tfog_shrine;
typedef struct { uint32_t hash; float x, z; } tfog_tower;

static inline int tfog_sect_ok(size_t n, uint32_t off, uint64_t len) { return off >= TFOG_HEAD && off <= n && len <= n - off; }
/* 0 ok, else the reason. Records stay in d (caller keeps it alive). */
static inline const char *tfog_table_parse(tfog_table *t, const void *data, size_t n) {
    const uint8_t *d = data;
    memset(t, 0, sizeof *t);
    if (!d || n < TFOG_HEAD || memcmp(d, "TOTKFOGT", 8)) return "not a fog table";
    if (tfog_u32(d + 8) != 1) return "table version is not 1";
    if (tfog_u32(d + 68) != n) return "table size does not match its header";
    t->d = d; t->n = n; t->ntiles = tfog_u32(d + 12); t->nshrines = tfog_u32(d + 16); t->ntowers = tfog_u32(d + 20);
    uint32_t to = tfog_u32(d + 24), so = tfog_u32(d + 28), wo = tfog_u32(d + 32), ro = tfog_u32(d + 36);
    if (tfog_u32(d + 40) != TFOG_GW || tfog_u32(d + 44) != TFOG_GH) return "region map size is not 750 x 625";
    if (t->ntowers > 254) return "too many towers";
    if (!tfog_sect_ok(n, to, (uint64_t)t->ntiles * TFOG_TILE_REC) || !tfog_sect_ok(n, so, (uint64_t)t->nshrines * TFOG_SHRINE_REC)
        || !tfog_sect_ok(n, wo, (uint64_t)t->ntowers * TFOG_TOWER_REC) || !tfog_sect_ok(n, ro, TFOG_NCELLS)) return "a section runs past the file";
    for (int z = 0; z < TFOG_ZOOMS; z++) t->img_size[z] = tfog_u32(d + 48 + 4 * z);
    t->tiles = d + to; t->shrines = d + so; t->towers = d + wo; t->region = d + ro;
    for (uint32_t i = 1; i < t->ntiles; i++)
        if (tfog_u64(t->tiles + (size_t)(i - 1) * TFOG_TILE_REC) >= tfog_u64(t->tiles + (size_t)i * TFOG_TILE_REC)) return "tile keys are not sorted";
    for (uint32_t i = 0; i < t->ntiles; i++) {
        const uint8_t *r = t->tiles + (size_t)i * TFOG_TILE_REC;
        if (r[8] > 2 || r[9] > 11 || r[10] > 9 || r[11] >= TFOG_ZOOMS) return "a tile record is out of range";
    }
    for (uint32_t i = 0; i < t->nshrines; i++) {
        const uint8_t *r = t->shrines + (size_t)i * TFOG_SHRINE_REC;
        if (r[12] > 2 || (r[14] != TFOG_NO_TOWER && r[14] >= t->ntowers)) return "a shrine record is out of range";
    }
    for (size_t i = 0; i < TFOG_NCELLS; i++)
        if (t->region[i] != TFOG_NO_TOWER && t->region[i] >= t->ntowers) return "a region cell names no tower";
    return NULL;
}
static inline tfog_tile tfog_tile_at(const tfog_table *t, uint32_t i) {
    const uint8_t *r = t->tiles + (size_t)i * TFOG_TILE_REC;
    return (tfog_tile){tfog_u64(r), r[8], r[9], r[10], r[11], r[12]};
}
/* 1 and *out when the key is in the table */
static inline int tfog_table_find(const tfog_table *t, uint64_t key, tfog_tile *out) {
    uint32_t lo = 0, hi = t->ntiles;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint64_t k = tfog_u64(t->tiles + (size_t)mid * TFOG_TILE_REC);
        if (k == key) { *out = tfog_tile_at(t, mid); return 1; }
        if (k < key) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static inline tfog_shrine tfog_shrine_at(const tfog_table *t, uint32_t i) {
    const uint8_t *r = t->shrines + (size_t)i * TFOG_SHRINE_REC;
    return (tfog_shrine){tfog_u32(r), tfog_f32(r + 4), tfog_f32(r + 8), r[12], r[13], r[14]};
}
static inline tfog_tower tfog_tower_at(const tfog_table *t, uint32_t i) {
    const uint8_t *r = t->towers + (size_t)i * TFOG_TOWER_REC;
    return (tfog_tower){tfog_u32(r), tfog_f32(r + 4), tfog_f32(r + 8)};
}
/* Tile key: the first 8 bytes of the SHA-256 of the image, little endian (the builder's key). */
static inline uint64_t tfog_key(const void *p, size_t n) {
    unsigned char h[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(p, (CC_LONG)n, h);
    return tfog_u64(h);
}

/* ---------- shrines and towers ---------- */
/* Visited share of each tower region: surface cells >= 128 / land cells (the region map holds land only). */
static inline void tfog_shares(const tfog_table *t, const uint8_t *surface, double *share) {
    uint32_t vis[256] = {0}, land[256] = {0};
    for (size_t i = 0; i < TFOG_NCELLS; i++) {
        uint8_t r = t->region[i];
        if (r == TFOG_NO_TOWER) continue;
        land[r]++;
        if (surface[i] >= 128) vis[r]++;
    }
    for (uint32_t r = 0; r < t->ntowers; r++) share[r] = land[r] ? (double)vis[r] / land[r] : 0;
}
/* 1 when a cell of the shrine's layer within TFOG_SHRINE_NEAR m of it is > 0, or its region is at pct % (pct > 0). */
static inline int tfog_shrine_revealed(const uint8_t *grids, const tfog_shrine *s, const double *share, int pct) {
    if (pct > 0 && s->tower != TFOG_NO_TOWER && share[s->tower] * 100 >= pct) return 1;
    if (!isfinite(s->x) || !isfinite(s->z) || s->layer < 0 || s->layer > 2) return 0;
    const uint8_t *g = grids + (size_t)s->layer * TFOG_NCELLS;
    float cx = (s->x - TFOG_X0) / TFOG_CELL, cz = (s->z - TFOG_Z0) / TFOG_CELL, reach = (float)TFOG_SHRINE_NEAR / TFOG_CELL;
    int i0 = (int)floorf(cx - reach), i1 = (int)ceilf(cx + reach), j0 = (int)floorf(cz - reach), j1 = (int)ceilf(cz + reach);
    for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < TFOG_GH; j++) for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < TFOG_GW; i++) {
        float dx = (i + 0.5f - cx) * TFOG_CELL, dz = (j + 0.5f - cz) * TFOG_CELL;
        if (dx * dx + dz * dz <= (float)TFOG_SHRINE_NEAR * TFOG_SHRINE_NEAR && g[(size_t)j * TFOG_GW + i]) return 1;
    }
    return 0;
}
/* name + " NN%" in UTF-16 (at most TFOG_TEXT_CAP units, no terminator counted); the name alone when it does not fit. */
static inline int tfog_text_suffix(const uint16_t *name, int len, int pct, uint16_t *out, int cap) {
    if (cap > TFOG_TEXT_CAP) cap = TFOG_TEXT_CAP;
    if (len < 0) len = 0;
    if (len > cap) len = cap;
    memcpy(out, name, (size_t)len * 2);
    char s[8];
    int k = snprintf(s, sizeof s, " %d%%", pct < 0 ? 0 : pct > 100 ? 100 : pct);
    if (k <= 0 || len + k > cap) return len;
    for (int i = 0; i < k; i++) out[len + i] = (uint8_t)s[i];
    return len + k;
}

/* ---------- grid file: "TOTKFOG1", u32 GW, GH, CELL, LAYERS, then the grids ---------- */
enum { TFOG_FILE_HEAD = 24, TFOG_FILE_SIZE = TFOG_FILE_HEAD + TFOG_LAYERS * TFOG_NCELLS };
static inline void tfog_grid_pack(uint8_t *out, const uint8_t *grids) {
    uint32_t dims[4] = {TFOG_GW, TFOG_GH, TFOG_CELL, TFOG_LAYERS};
    memcpy(out, "TOTKFOG1", 8); memcpy(out + 8, dims, sizeof dims);
    memcpy(out + TFOG_FILE_HEAD, grids, (size_t)TFOG_LAYERS * TFOG_NCELLS);
}
static inline int tfog_grid_unpack(const uint8_t *in, size_t n, uint8_t *grids) {
    uint32_t dims[4];
    if (n != TFOG_FILE_SIZE || memcmp(in, "TOTKFOG1", 8)) return 0;
    memcpy(dims, in + 8, sizeof dims);
    if (dims[0] != TFOG_GW || dims[1] != TFOG_GH || dims[2] != TFOG_CELL || dims[3] != TFOG_LAYERS) return 0;
    memcpy(grids, in + TFOG_FILE_HEAD, (size_t)TFOG_LAYERS * TFOG_NCELLS);
    return 1;
}
/* whole small file -> malloc'd bytes, NULL if missing or larger than max */
static inline uint8_t *tfog_slurp(const char *path, size_t max, size_t *n) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    uint8_t *b = NULL;
    long len = fseek(f, 0, SEEK_END) ? -1 : ftell(f);
    if (len >= 0 && (size_t)len <= max && !fseek(f, 0, SEEK_SET) && (b = malloc(len ? (size_t)len : 1)) && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    if (b) *n = (size_t)len;
    return b;
}
static inline int tfog_write_file(const char *path, const void *d, size_t n) {
    char tmp[1200];
    if (snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid()) >= (int)sizeof tmp) return 0;
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(d, 1, n, f) == n;
    if (f && fclose(f)) ok = 0;
    if (!ok || rename(tmp, path)) { unlink(tmp); return 0; }
    return 1;
}

/* ---------- live state ---------- */
typedef struct { float x, z; int layer; uint32_t epoch; } tfog_ring_pt;
typedef struct {
    pthread_mutex_t mu;               /* grids, slot, epoch, prev, dirty; then ring_mu inside it */
    uint8_t grid[TFOG_LAYERS][TFOG_NCELLS];
    _Atomic uint32_t epoch;           /* written under mu, read by the hook without it */
    int slot;                         /* -1: none loaded or saved yet */
    int last_layer;                   /* layer of the last painted point, -1 none */
    int have_prev[TFOG_LAYERS];
    float prev[TFOG_LAYERS][2];
    tfog_box dirty[TFOG_LAYERS];
    float radius, edge, gap;
    pthread_mutex_t ring_mu;
    tfog_ring_pt ring[TFOG_RING];
    unsigned ring_head, ring_n, ring_lost;
    uint64_t points, dropped;
} tfog_state;
static inline void tfog_state_init(tfog_state *s, float radius, float edge, float gap) {
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->mu, NULL); pthread_mutex_init(&s->ring_mu, NULL);
    s->slot = -1; s->last_layer = -1; s->radius = radius; s->edge = edge; s->gap = gap;
    for (int l = 0; l < TFOG_LAYERS; l++) s->dirty[l] = tfog_box_empty;
}
/* any thread (the Hero's Path writer hook): one recorded point */
static inline void tfog_push(tfog_state *s, int layer, float x, float z) {
    if (layer < 0 || layer > 2 || !isfinite(x) || !isfinite(z)) return;
    uint32_t e = atomic_load(&s->epoch);
    pthread_mutex_lock(&s->ring_mu);
    if (s->ring_n == TFOG_RING) { s->ring_head = (s->ring_head + 1) % TFOG_RING; s->ring_n--; s->ring_lost++; }
    s->ring[(s->ring_head + s->ring_n++) % TFOG_RING] = (tfog_ring_pt){x, z, layer, e};
    pthread_mutex_unlock(&s->ring_mu);
}
/* mu held: one point into its layer, a line from that layer's previous point. No line across a trip to or from the
   depths (2): Link cannot see one from the other. Surface and sky keep their lines (he sees the ground he glides over). */
static inline void tfog_paint_point_locked(tfog_state *s, int layer, float x, float z) {
    uint8_t *g = s->grid[layer];
    if (s->last_layer >= 0 && (layer == 2) != (s->last_layer == 2)) for (int l = 0; l < TFOG_LAYERS; l++) s->have_prev[l] = 0;
    s->last_layer = layer;
    if (s->have_prev[layer]) tfog_paint_line(g, s->prev[layer][0], s->prev[layer][1], x, z, s->radius, s->edge, s->gap, &s->dirty[layer]);
    else tfog_paint_disc(g, x, z, s->radius, s->edge, &s->dirty[layer]);
    s->prev[layer][0] = x; s->prev[layer][1] = z; s->have_prev[layer] = 1;
}
/* mu held: the ring into the grids; points of an older epoch are dropped. Returns the points painted. */
static inline int tfog_drain_locked(tfog_state *s) {
    tfog_ring_pt buf[TFOG_RING];
    pthread_mutex_lock(&s->ring_mu);
    unsigned n = s->ring_n;
    for (unsigned i = 0; i < n; i++) buf[i] = s->ring[(s->ring_head + i) % TFOG_RING];
    s->ring_head = (s->ring_head + n) % TFOG_RING; s->ring_n = 0;
    pthread_mutex_unlock(&s->ring_mu);
    uint32_t e = atomic_load(&s->epoch);
    int painted = 0;
    for (unsigned i = 0; i < n; i++) {
        if (buf[i].epoch != e) { s->dropped++; continue; }
        tfog_paint_point_locked(s, buf[i].layer, buf[i].x, buf[i].z); painted++; s->points++;
    }
    return painted;
}
/* The game reads slot's progress.sav (a load). grid: the slot's grid file (may be missing), fp: its footprint.sav
   bytes (may be NULL). Returns 1 when the grid file was used, 0 when the slot starts from Hero's Path, -1 when this is
   the live slot read again (the live grid is newer; only the line to the next point is cut). */
static inline int tfog_on_load(tfog_state *s, int slot, const char *grid_path, const uint8_t *fp, size_t fp_n, int *fp_points) {
    pthread_mutex_lock(&s->mu);
    if (slot == s->slot) {
        for (int l = 0; l < TFOG_LAYERS; l++) s->have_prev[l] = 0;
        s->last_layer = -1;
        pthread_mutex_unlock(&s->mu);
        return -1;
    }
    atomic_store(&s->epoch, atomic_load(&s->epoch) + 1);
    size_t n = 0;
    uint8_t *f = tfog_slurp(grid_path, TFOG_FILE_SIZE, &n);
    int had = f && tfog_grid_unpack(f, n, &s->grid[0][0]);
    free(f);
    if (!had) memset(s->grid, 0, sizeof s->grid);
    for (int l = 0; l < TFOG_LAYERS; l++) s->have_prev[l] = 0;
    s->last_layer = -1;
    int k = 0;
    if (fp) {
        tfog_point *pts = malloc(sizeof *pts * 160000);
        k = pts ? tfog_fp_parse(fp, fp_n, pts, 160000) : -1;
        for (int i = 0; i < k; i++) tfog_paint_point_locked(s, pts[i].layer, pts[i].x, pts[i].z);
        free(pts);
        for (int l = 0; l < TFOG_LAYERS; l++) s->have_prev[l] = 0;   /* no line from the last recorded point */
        s->last_layer = -1;
    }
    if (fp_points) *fp_points = k;
    for (int l = 0; l < TFOG_LAYERS; l++) s->dirty[l] = tfog_box_all;
    s->slot = slot;
    pthread_mutex_unlock(&s->mu);
    return had;
}
/* The game writes slot's progress.sav (a save): queued points first, then the grids as that slot. 1 when written. */
static inline int tfog_on_save(tfog_state *s, int slot, const char *grid_path) {
    static uint8_t buf[TFOG_FILE_SIZE];   /* under mu */
    pthread_mutex_lock(&s->mu);
    tfog_drain_locked(s);
    tfog_grid_pack(buf, &s->grid[0][0]);
    int ok = grid_path && tfog_write_file(grid_path, buf, sizeof buf);
    s->slot = slot;
    pthread_mutex_unlock(&s->mu);
    return ok;
}
