/* Breath of the Wild 1.9.0 resource size table (System/Resource/ResourceSizeTable.product.srsizetable): pure transforms that
   romfs.c uses to give each model archive more room (local patch 0025, after the mechanism of Savage Wind Recomp #524).
   The game sets aside the bytes this table lists for a resource; a model that needs more than that fails to load and the
   actor is left without its model. The decoded table is little-endian: "RSTB", u32 CRC entries, u32 name entries, the
   CRC entries (CRC32 of the resource name, size) sorted by CRC, then the name entries (char[128], size) for the names
   whose CRC32 collides. Resource names drop the Yaz0 "s": Model/X.sbfres is listed as Model/X.bfres. No allocation
   leaks and no global state, so tests can call every function. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BOTW_RSTB_PATH "System/Resource/ResourceSizeTable.product.srsizetable"
enum { BOTW_RSTB_NAME = 128, BOTW_RSTB_HEAD = 12, BOTW_RSTB_CRC_ENTRY = 8, BOTW_RSTB_NAME_ENTRY = BOTW_RSTB_NAME + 4 };

static uint32_t botw_rstb_crc32(const char *s) {   /* zlib's CRC32 */
    uint32_t c = 0xffffffffu;
    for (; *s; s++) { c ^= (uint8_t)*s; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xedb88320u & (0u - (c & 1u))); }
    return ~c;
}
static uint32_t botw_rstb_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void botw_rstb_put(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* The table's name for RomFS path p when p is a model archive (Model/....sbfres): Model/....bfres in out. 0 otherwise. */
static int botw_rstb_model_name(const char *p, char *out, size_t cap) {
    size_t n = strlen(p);
    if (strncmp(p, "Model/", 6) || n < 6 + 8 || strcmp(p + n - 7, ".sbfres") || n > cap) return 0;   /* the name needs n bytes */
    memcpy(out, p, n - 6); memcpy(out + n - 6, "bfres", 6);   /* ".sbfres" -> ".bfres" */
    return 1;
}

/* Yaz0 -> malloc'd bytes (*out_n), NULL on a bad stream. */
static uint8_t *botw_rstb_yaz0_decode(const uint8_t *in, size_t n, size_t *out_n) {
    if (n < 16 || memcmp(in, "Yaz0", 4)) return NULL;
    size_t size = (size_t)in[4] << 24 | (size_t)in[5] << 16 | (size_t)in[6] << 8 | in[7], o = 0, i = 16;
    uint8_t *out = malloc(size ? size : 1);
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

/* d as Yaz0 with literal runs only (the game decodes it as any Yaz0; no search, so it takes no time), the header's
   bytes 8..15 copied from head (the source file's). NULL if d is over 4 GiB or out of memory. */
static uint8_t *botw_rstb_yaz0_store(const uint8_t head[16], const uint8_t *d, size_t n, size_t *out_n) {
    if (n > 0xffffffffu) return NULL;
    size_t total = 16 + n + (n + 7) / 8, o = 16;
    uint8_t *out = malloc(total);
    if (!out) return NULL;
    memcpy(out, "Yaz0", 4); out[4] = (uint8_t)(n >> 24); out[5] = (uint8_t)(n >> 16); out[6] = (uint8_t)(n >> 8); out[7] = (uint8_t)n;
    memcpy(out + 8, head + 8, 8);
    for (size_t i = 0; i < n; i += 8) { size_t k = n - i < 8 ? n - i : 8; out[o++] = 0xff; memcpy(out + o, d + i, k); o += k; }
    *out_n = o; return out;
}

typedef struct { size_t grown, missed, crc_entries, name_entries; } BotwRstbGrow;
/* Adds `add` bytes to the entry of each of the `count` resource names (a name entry when the table has one for it, else its
   CRC entry; an entry two names share grows once). A name the table lacks counts as missed. Returns 0 and leaves t
   unchanged when t is not a whole table (size, header, unsorted CRCs, an unterminated name) or an entry would pass 4 GiB. */
static int botw_rstb_grow(uint8_t *t, size_t n, const char *const *names, size_t count, uint32_t add, BotwRstbGrow *r) {
    memset(r, 0, sizeof *r);
    if (n < BOTW_RSTB_HEAD || memcmp(t, "RSTB", 4)) return 0;
    const uint64_t crcs = botw_rstb_u32(t + 4), nnames = botw_rstb_u32(t + 8);
    if ((uint64_t)n != BOTW_RSTB_HEAD + crcs * BOTW_RSTB_CRC_ENTRY + nnames * BOTW_RSTB_NAME_ENTRY) return 0;
    uint8_t *crc = t + BOTW_RSTB_HEAD, *nam = crc + crcs * BOTW_RSTB_CRC_ENTRY;
    for (uint64_t k = 1; k < crcs; k++) if (botw_rstb_u32(crc + k * 8 - 8) >= botw_rstb_u32(crc + k * 8)) return 0;
    for (uint64_t k = 0; k < nnames; k++) if (!memchr(nam + k * BOTW_RSTB_NAME_ENTRY, 0, BOTW_RSTB_NAME)) return 0;
    r->crc_entries = (size_t)crcs; r->name_entries = (size_t)nnames;
    uint8_t *hit = calloc((size_t)(crcs + nnames) + 1, 1);   /* entries to grow: CRC entries first, then name entries */
    if (!hit) return 0;
    for (size_t i = 0; i < count; i++) {
        const char *s = names[i]; uint64_t at = UINT64_MAX;
        if (strlen(s) < BOTW_RSTB_NAME)
            for (uint64_t k = 0; k < nnames && at == UINT64_MAX; k++) if (!strcmp((const char *)nam + k * BOTW_RSTB_NAME_ENTRY, s)) at = crcs + k;
        if (at == UINT64_MAX) {
            uint32_t c = botw_rstb_crc32(s); uint64_t lo = 0, hi = crcs;
            while (lo < hi) { uint64_t m = lo + (hi - lo) / 2; if (botw_rstb_u32(crc + m * 8) < c) lo = m + 1; else hi = m; }
            if (lo < crcs && botw_rstb_u32(crc + lo * 8) == c) at = lo;
        }
        if (at == UINT64_MAX) { r->missed++; continue; }
        hit[at] = 1;
    }
    for (uint64_t k = 0; k < crcs + nnames; k++) {
        if (!hit[k]) continue;
        const uint8_t *sz = k < crcs ? crc + k * 8 + 4 : nam + (k - crcs) * BOTW_RSTB_NAME_ENTRY + BOTW_RSTB_NAME;
        if (botw_rstb_u32(sz) > 0xffffffffu - add) { free(hit); memset(r, 0, sizeof *r); return 0; }
    }
    for (uint64_t k = 0; k < crcs + nnames; k++) {
        if (!hit[k]) continue;
        uint8_t *sz = k < crcs ? crc + k * 8 + 4 : nam + (k - crcs) * BOTW_RSTB_NAME_ENTRY + BOTW_RSTB_NAME;
        botw_rstb_put(sz, botw_rstb_u32(sz) + add); r->grown++;
    }
    free(hit); return 1;
}
