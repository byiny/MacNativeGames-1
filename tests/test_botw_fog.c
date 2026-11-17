/* games/botw/gfx/botw_fog_core.h: BC1 darkening (checked against the bcdec decoder), grid, Hero's Path words, Yaz0,
   BNTX lookup and tile names. cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all */
#define BCDEC_IMPLEMENTATION
#include "../games/totk/third_party/d2r_gfx/third_party/bcdec/bcdec.h"
#include "../games/botw/gfx/botw_fog_core.h"
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* every texel of the darkened block is f times the original (565 steps and the 1/3 points allow 12 of 255), alpha kept */
static void check_block(const uint8_t blk[8], float f) {
    uint8_t a[64], b[64], d[8]; memcpy(d, blk, 8);
    bcdec_bc1(blk, a, 16); fog_bc1_block(d, f); bcdec_bc1(d, b, 16);
    for (int i = 0; i < 16; i++) {
        CHECK(a[i * 4 + 3] == b[i * 4 + 3], "alpha texel %d: %d -> %d (f %.2f)", i, a[i * 4 + 3], b[i * 4 + 3], f);
        if (!a[i * 4 + 3]) continue;
        for (int c = 0; c < 3; c++) {
            float want = a[i * 4 + c] * f, got = b[i * 4 + c];
            CHECK(fabsf(want - got) <= 12, "texel %d ch %d: %d * %.2f -> %d (block %02x%02x %02x%02x)", i, c, a[i * 4 + c], f, b[i * 4 + c], blk[1], blk[0], blk[3], blk[2]);
        }
    }
    if (f == 1.0f) CHECK(!memcmp(d, blk, 8), "f = 1 changes the block");
}

int main(void) {
    /* BC1: random blocks of both modes, and the 3-colour case whose endpoints turn round at 0.3 */
    uint32_t s = 12345;
    const float fs[] = {1.0f, 0.9f, 0.5f, 0.3f, 0.1f, 0.0f};
    for (int n = 0; n < 20000; n++) {
        uint8_t blk[8];
        for (int k = 0; k < 8; k++) { s = s * 1103515245u + 12345u; blk[k] = (uint8_t)(s >> 16); }
        if (n % 7 == 0) { blk[2] = blk[0]; blk[3] = blk[1]; }   /* equal endpoints */
        if (n % 5 == 0) { blk[1] = (uint8_t)(blk[3] ^ 1); }     /* close endpoints: rounding may turn them round */
        check_block(blk, fs[n % 6]);
    }
    const uint8_t turn[8] = {0xff, 0x07, 0x00, 0x08, 0xe4, 0xe4, 0xe4, 0xe4};   /* c0 0x07ff <= c1 0x0800: 3-colour */
    check_block(turn, 0.3f);
    { uint8_t d[8]; memcpy(d, turn, 8); fog_bc1_block(d, 0.3f); CHECK((d[0] | d[1] << 8) <= (d[2] | d[3] << 8), "3-colour block stays 3-colour"); }

    /* grid */
    static uint8_t g[FOG_GW * FOG_GH];
    fog_box box = {1, 0, 0, 0};
    CHECK(fog_paint_disc(g, -1262, 1633, 120, 32, &box), "disc paints");
    CHECK(fog_sample(g, -1262, 1633) > 0.99f, "centre clear: %.3f", fog_sample(g, -1262, 1633));
    CHECK(fog_sample(g, -1262 + 100, 1633) > 0.99f, "inside radius clear");
    CHECK(fog_sample(g, -1262 + 200, 1633) == 0, "outside dark");
    float mid = fog_sample(g, -1262 + 135, 1633);
    CHECK(mid > 0.05f && mid < 0.95f, "soft edge: %.3f", mid);
    CHECK(box.x0 <= box.x1 && (box.x0 + 0.5f) * FOG_CELL + FOG_X0 < -1262 - 100 && (box.x1 + 0.5f) * FOG_CELL + FOG_X0 > -1262 + 100, "dirty box covers the disc");
    CHECK(!fog_paint_disc(g, -1262, 1633, 120, 32, NULL), "second paint changes nothing");
    fog_paint_line(g, 0, 0, 200, 0, 20, 16, 250, NULL);
    CHECK(fog_sample(g, 100, 0) > 0.99f, "line between near samples");
    fog_paint_line(g, 1000, 1000, 1000, 2000, 20, 16, 250, NULL);
    CHECK(fog_sample(g, 1000, 1500) == 0 && fog_sample(g, 1000, 2000) > 0.99f, "a warp paints its end only");
    CHECK(fog_sample(g, -7000, 0) == 0 && fog_sample(g, 0, 6000) == 0, "outside the grid");

    /* BC1 tile: a darkened 8x8 tile keeps the clear block and darkens the far one */
    static uint8_t gt[FOG_GW * FOG_GH];
    fog_paint_disc(gt, -1000, 1000, 400, 16, NULL);   /* block 0 centre is 354 m from the corner, block 3 1060 m */
    uint8_t tile[4 * 8]; for (int k = 0; k < 4; k++) { const uint8_t w[8] = {0xff, 0xff, 0x00, 0x00, 0, 0, 0, 0}; memcpy(tile + k * 8, w, 8); }
    fog_bc1_tile(tile, 8, 8, -1000, 1000, gt, 0.3f);
    CHECK(tile[0] == 0xff && tile[1] == 0xff, "clear block untouched");
    CHECK(tile[3 * 8 + 1] < 0xff, "far block darkened");

    /* Hero's Path words */
    float x, z;
    CHECK(fog_track_point(0x848d877du, &x, &z) && x == -1132 && z == 1917, "plateau start: %.0f %.0f", x, z);
    CHECK(!fog_track_point(0xfbffdfffu, &x, &z), "start marker");
    CHECK(!fog_track_point(0x0000025au, &x, &z), "chunk header");
    CHECK(!fog_track_point(0x04004103u, &x, &z), "shrine point (bit 31 clear)");
    uint32_t w = 1u << 31 | 1u << 12 | 2000u << 13 | 300u;   /* x +2000, z -300 */
    CHECK(fog_track_point(w, &x, &z) && x == 2000 && z == -300, "signs: %.0f %.0f", x, z);
    uint8_t track[0x60] = {0}; for (int k = 0; k < 4; k++) track[0x54 + k] = (uint8_t)(0x848d877du >> (8 * k));
    memset(g, 0, sizeof g);
    CHECK(fog_paint_track(g, track, sizeof track, 120, 32, 250) == 1 && fog_sample(g, -1132, 1917) > 0.99f, "track paints");

    /* Yaz0 */
    const uint8_t y[] = {'Y', 'a', 'z', '0', 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0xe0, 'a', 'b', 'c', 0x10, 0x02};
    size_t yn = 0; uint8_t *yo = fog_yaz0(y, sizeof y, &yn);
    CHECK(yo && yn == 6 && !memcmp(yo, "abcabc", 6), "yaz0 back-reference");
    free(yo);
    CHECK(!fog_yaz0(y, sizeof y - 1, &yn), "truncated yaz0 refused");

    /* BNTX */
    static uint8_t bn[0x400];
    memcpy(bn, "BNTX", 4); memcpy(bn + 0x20, "BRTI", 4); memcpy(bn + 0x300, "BRTD", 4);
    bn[0x20 + 0x16] = 1; bn[0x20 + 0x24] = 8; bn[0x20 + 0x28] = 8; bn[0x20 + 0x50] = 32; bn[0x20 + 0x81] = 0x02; bn[0x200] = 0x10; bn[0x201] = 0x03;
    size_t off = 0, sz = 0; uint32_t tw = 0, th = 0;
    CHECK(fog_bntx_image(bn, sizeof bn, &off, &sz, &tw, &th) && off == 0x310 && sz == 32 && tw == 8 && th == 8, "bntx image %zx %zu %u", off, sz, tw);
    CHECK(!fog_bntx_image(bn, 0x300, &off, &sz, &tw, &th), "bntx without BRTD refused");

    /* tile names */
    fog_cell c;
    CHECK(fog_parse_tile("UI/MapTex/MainField/MapTex1_D-6_64.sbmaptex", &c) && c.level == 1 && c.col == 3 && c.row == 6, "MapTex1_D-6_64");
    CHECK(fog_parse_tile("content:/UI/MapTex/MainField/MapTex_Z-0.sbmaptex", &c) && c.level == 0 && c.col == -1 && c.row == 0, "MapTex_Z-0");
    CHECK(fog_parse_tile("UI/MapTex/MainField/MapTex3_K-9_6144.sbmaptex", &c) && c.level == 3 && c.col == 10 && c.row == 9, "MapTex3_K-9_6144");
    CHECK(!fog_parse_tile("UI/MapOpen/MapOpen_00.sbmaptex", &c), "not a tile");

    printf(fails ? "test_botw_fog: %d failures\n" : "test_botw_fog: ok\n", fails);
    return fails != 0;
}
