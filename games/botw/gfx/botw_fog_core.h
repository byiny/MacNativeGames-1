/* Breath of the Wild 1.9.0 fog-of-war map: the plain C part (no Objective-C, so tests/test_botw_fog.c includes it).
   botw_fog_map.h holds the game and Metal side.

   - Visited grid: FOG_GW x FOG_GH bytes, FOG_CELL m per cell, over x FOG_X0.. and z FOG_Z0.. (the map tiles Z..K x
     0..9, 1 km each). 0 = never seen, 255 = clear. Painting only raises values (max), so a place never goes dark again.
   - BC1: a block is darkened by scaling both 565 endpoints; the block keeps its mode (4-colour c0 > c1, 3-colour with
     transparency c0 <= c1), swapping the endpoints and remapping the indices when rounding turns the order round.
   - Hero's Path (save folder tracker/trackblock{,_hard}NN.sav): a word per point from 0x50; bit 31 main field,
     bits 0-11 |z| (sign bit 12), bits 13-25 |x| (sign bit 26), metres. Header and shrine words have bit 31 clear.
   - Yaz0 and the BNTX mip-0 image of a UI/MapTex .sbmaptex file (one texture, one level). */
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { FOG_CELL = 16, FOG_X0 = -6000, FOG_Z0 = -5000, FOG_GW = 12000 / FOG_CELL, FOG_GH = 10000 / FOG_CELL };

typedef struct { int x0, z0, x1, z1; } fog_box;   /* cells, inclusive; x0 > x1 = empty */
static inline void fog_box_add(fog_box *b, int x0, int z0, int x1, int z1) {
    if (b->x0 > b->x1) { *b = (fog_box){x0, z0, x1, z1}; return; }
    if (x0 < b->x0) b->x0 = x0; if (z0 < b->z0) b->z0 = z0; if (x1 > b->x1) b->x1 = x1; if (z1 > b->z1) b->z1 = z1;
}

/* A disc of radius r (m) fully clear, fading to 0 over edge m; changed cells grow *dirty. Returns 1 if a cell rose. */
static int fog_paint_disc(uint8_t *g, float x, float z, float r, float edge, fog_box *dirty) {
    if (!isfinite(x) || !isfinite(z)) return 0;
    float far = r + edge, cx = (x - FOG_X0) / FOG_CELL, cz = (z - FOG_Z0) / FOG_CELL;
    int i0 = (int)floorf(cx - far / FOG_CELL), i1 = (int)ceilf(cx + far / FOG_CELL), j0 = (int)floorf(cz - far / FOG_CELL), j1 = (int)ceilf(cz + far / FOG_CELL);
    if (i0 < 0) i0 = 0; if (j0 < 0) j0 = 0; if (i1 > FOG_GW - 1) i1 = FOG_GW - 1; if (j1 > FOG_GH - 1) j1 = FOG_GH - 1;
    int changed = 0, bx0 = FOG_GW, bz0 = FOG_GH, bx1 = -1, bz1 = -1;
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        float dx = (i + 0.5f - cx) * FOG_CELL, dz = (j + 0.5f - cz) * FOG_CELL, d = sqrtf(dx * dx + dz * dz);
        if (d >= far) continue;
        int v = d <= r ? 255 : (int)(255.0f * (far - d) / edge + 0.5f);
        uint8_t *c = &g[(size_t)j * FOG_GW + i];
        if (v > *c) { *c = (uint8_t)v; changed = 1; if (i < bx0) bx0 = i; if (i > bx1) bx1 = i; if (j < bz0) bz0 = j; if (j > bz1) bz1 = j; }
    }
    if (changed && dirty) fog_box_add(dirty, bx0, bz0, bx1, bz1);
    return changed;
}
/* Discs along a step shorter than max_gap m (a longer one is a warp: only its end is painted). */
static int fog_paint_line(uint8_t *g, float xa, float za, float xb, float zb, float r, float edge, float max_gap, fog_box *dirty) {
    float dx = xb - xa, dz = zb - za, len = sqrtf(dx * dx + dz * dz);
    if (!(len < max_gap)) return fog_paint_disc(g, xb, zb, r, edge, dirty);
    int n = (int)ceilf(len / (FOG_CELL * 0.5f)), changed = 0;
    if (n < 1) n = 1;
    for (int k = 1; k <= n; k++) { float t = (float)k / n; changed |= fog_paint_disc(g, xa + dx * t, za + dz * t, r, edge, dirty); }
    return changed;
}
/* Bilinear clearness 0..1 at a world place (cell centres hold the values; outside the grid = 0). */
static float fog_sample(const uint8_t *g, float x, float z) {
    float fx = (x - FOG_X0) / FOG_CELL - 0.5f, fz = (z - FOG_Z0) / FOG_CELL - 0.5f;
    if (!(fx > -1 && fz > -1 && fx < FOG_GW && fz < FOG_GH)) return 0;
    int i = (int)floorf(fx), j = (int)floorf(fz); float ax = fx - i, az = fz - j;
    #define FOG_AT(a, b) ((a) < 0 || (b) < 0 || (a) >= FOG_GW || (b) >= FOG_GH ? 0.0f : g[(size_t)(b) * FOG_GW + (a)] / 255.0f)
    float v = (FOG_AT(i, j) * (1 - ax) + FOG_AT(i + 1, j) * ax) * (1 - az) + (FOG_AT(i, j + 1) * (1 - ax) + FOG_AT(i + 1, j + 1) * ax) * az;
    #undef FOG_AT
    return v;
}

/* BC1: both endpoints times f (0..1), in place. */
static uint16_t fog_565_scale(uint16_t c, float f) {
    int r = (int)((c >> 11) * f + 0.5f), g = (int)(((c >> 5) & 63) * f + 0.5f), b = (int)((c & 31) * f + 0.5f);
    return (uint16_t)(r << 11 | g << 5 | b);
}
static void fog_bc1_block(uint8_t *blk, float f) {
    uint16_t c0 = (uint16_t)(blk[0] | blk[1] << 8), c1 = (uint16_t)(blk[2] | blk[3] << 8);
    uint32_t idx = (uint32_t)blk[4] | (uint32_t)blk[5] << 8 | (uint32_t)blk[6] << 16 | (uint32_t)blk[7] << 24;
    uint16_t n0 = fog_565_scale(c0, f), n1 = fog_565_scale(c1, f);
    if (c0 > c1) {                       /* 4-colour */
        if (n0 < n1) { uint16_t t = n0; n0 = n1; n1 = t; idx ^= 0x55555555u; }   /* 0<->1, 2<->3 */
        else if (n0 == n1) idx = 0;      /* would read as 3-colour: one solid colour */
    } else if (n0 > n1) {                /* 3-colour turned round: swap, 0<->1 only (2 = midpoint, 3 = transparent) */
        uint16_t t = n0; n0 = n1; n1 = t; idx ^= ~(idx >> 1) & 0x55555555u;
    }
    blk[0] = (uint8_t)n0; blk[1] = (uint8_t)(n0 >> 8); blk[2] = (uint8_t)n1; blk[3] = (uint8_t)(n1 >> 8);
    blk[4] = (uint8_t)idx; blk[5] = (uint8_t)(idx >> 8); blk[6] = (uint8_t)(idx >> 16); blk[7] = (uint8_t)(idx >> 24);
}
/* A map tile's linear BC1 blocks (w x h texels) darkened by the grid: tile origin (x0, z0), 1000 m across. */
static void fog_bc1_tile(uint8_t *blocks, uint32_t w, uint32_t h, float x0, float z0, const uint8_t *g, float dark) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++) {
        float z = z0 + (by * 4 + 2.0f) * 1000.0f / h;
        for (uint32_t bx = 0; bx < bw; bx++) {
            float c = fog_sample(g, x0 + (bx * 4 + 2.0f) * 1000.0f / w, z);
            if (c < 0.999f) fog_bc1_block(blocks + ((size_t)by * bw + bx) * 8, dark + (1 - dark) * c);
        }
    }
}

/* Map cell of a tile file name "…/MapTex<level>_<col>-<row>[_<variant>].sbmaptex": level 0 (3000), 1 (840), 2 (300),
   3 (135); col 'Z' = -1, 'A'..'K' = 0..10; row '0'..'9'. Tile origin x = -5000 + 1000 col, z = -5000 + 1000 row. */
typedef struct { int level, col, row; } fog_cell;
static int fog_parse_tile(const char *path, fog_cell *o) {
    const char *p = path ? strstr(path, "MapTex/MainField/MapTex") : NULL;
    if (!p) return 0;
    p += strlen("MapTex/MainField/MapTex");
    int level = 0;
    if (*p >= '1' && *p <= '3') level = *p++ - '0';
    if (*p++ != '_') return 0;
    int col = *p == 'Z' ? -1 : *p >= 'A' && *p <= 'K' ? *p - 'A' : -2;
    if (col == -2 || p[1] != '-' || p[2] < '0' || p[2] > '9' || (p[3] != '_' && p[3] != '.')) return 0;
    *o = (fog_cell){level, col, p[2] - '0'};
    return 1;
}
static const uint32_t fog_level_size[4] = {3000, 840, 300, 135};

/* Hero's Path word → place; 0 for headers, shrine points and markers. */
static int fog_track_point(uint32_t w, float *x, float *z) {
    if (!(w >> 31)) return 0;
    int az = (int)(w & 0xfff), ax = (int)((w >> 13) & 0x1fff);
    float px = (float)((w >> 26) & 1 ? -ax : ax), pz = (float)((w >> 12) & 1 ? -az : az);
    if (px < FOG_X0 || px >= FOG_X0 + 12000 || pz < FOG_Z0 || pz >= FOG_Z0 + 10000 || ax >= 6000) return 0;
    *x = px; *z = pz; return 1;
}
/* Paints a trackblock file; returns the number of points. */
static int fog_paint_track(uint8_t *g, const uint8_t *d, size_t n, float r, float edge, float max_gap) {
    int count = 0, have = 0; float px = 0, pz = 0;
    for (size_t o = 0x50; o + 4 <= n; o += 4) {
        uint32_t w = (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24;
        float x, z;
        if (!fog_track_point(w, &x, &z)) continue;
        if (have) fog_paint_line(g, px, pz, x, z, r, edge, max_gap, NULL); else fog_paint_disc(g, x, z, r, edge, NULL);
        px = x; pz = z; have = 1; count++;
    }
    return count;
}

/* Yaz0 → malloc'd bytes (*out_n), NULL on a bad stream. */
static uint8_t *fog_yaz0(const uint8_t *in, size_t n, size_t *out_n) {
    if (n < 16 || memcmp(in, "Yaz0", 4)) return NULL;
    size_t size = (size_t)in[4] << 24 | (size_t)in[5] << 16 | (size_t)in[6] << 8 | in[7], o = 0, i = 16;
    uint8_t *out = size ? malloc(size) : NULL;
    if (!out) return NULL;
    while (o < size) {
        if (i >= n) goto bad;
        uint8_t code = in[i++];
        for (int k = 0; k < 8 && o < size; k++) {
            if (code & (0x80 >> k)) { if (i >= n) goto bad; out[o++] = in[i++]; continue; }
            if (i + 1 >= n) goto bad;
            size_t b1 = in[i], b2 = in[i + 1], dist = ((b1 & 0xf) << 8 | b2) + 1, len = b1 >> 4; i += 2;
            if (!len) { if (i >= n) goto bad; len = (size_t)in[i++] + 0x12; } else len += 2;
            if (dist > o || len > size - o) goto bad;
            for (size_t j = 0; j < len; j++, o++) out[o] = out[o - dist];
        }
    }
    *out_n = size; return out;
bad:
    free(out); return NULL;
}
static uint64_t fog_rd(const uint8_t *d, size_t off, int bytes) { uint64_t v = 0; for (int k = bytes - 1; k >= 0; k--) v = v << 8 | d[off + k]; return v; }
/* The first BNTX texture's mip-0 image in d: offset and size, width and height. 0 if not found or out of range. */
static int fog_bntx_image(const uint8_t *d, size_t n, size_t *off, size_t *size, uint32_t *w, uint32_t *h) {
    const uint8_t *b = n >= 4 ? memmem(d, n, "BNTX", 4) : NULL;
    if (!b) return 0;
    size_t base = (size_t)(b - d), bn = n - base;
    const uint8_t *ti = memmem(d + base, bn, "BRTI", 4), *td = memmem(d + base, bn, "BRTD", 4);
    if (!ti || !td) return 0;
    size_t o = (size_t)(ti - d) - base, bd = (size_t)(td - d) - base;
    if (o + 0x88 > bn) return 0;
    uint32_t mips = (uint32_t)fog_rd(b, o + 0x16, 2), tw = (uint32_t)fog_rd(b, o + 0x24, 4), th = (uint32_t)fog_rd(b, o + 0x28, 4), isz = (uint32_t)fog_rd(b, o + 0x50, 4);
    uint64_t ptrs = fog_rd(b, o + 0x80, 8);
    if (!mips || ptrs + 8 > bn) return 0;
    uint64_t m0 = fog_rd(b, (size_t)ptrs, 8), img = m0 >= bd + 16 ? m0 : bd + 16 + m0;
    uint64_t sz = mips > 1 && ptrs + 16 <= bn ? fog_rd(b, (size_t)ptrs + 8, 8) - m0 : isz;
    if (!sz || img > bn || sz > bn - img) return 0;
    *off = base + (size_t)img; *size = (size_t)sz; *w = tw; *h = th; return 1;
}
