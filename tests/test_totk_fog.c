/* games/totk/third_party/d2r_gfx/gfx/nvn/include/totk_fog_core.h: grid paint and sample, footprint.sav parse, fog table, shares and shrine
   reveal, the tower text suffix, the grid file and the slot order (load, save, epochs). Synthetic data only. */
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/include/totk_fog_core.h"
#include <stdio.h>
#include <sys/stat.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t *cell(uint8_t *g, float x, float z) { return &g[(size_t)((z - TFOG_Z0) / TFOG_CELL) * TFOG_GW + (size_t)((x - TFOG_X0) / TFOG_CELL)]; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void putf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static uint32_t fp_word(int layer, int x, int z) {
    return (uint32_t)layer << 3 | (uint32_t)(x < 0) << 5 | (uint32_t)abs(x) << 6 | (uint32_t)(z < 0) << 19 | (uint32_t)abs(z) << 20;
}
/* footprint.sav: data at 0x40, ring of `ring` words */
static size_t make_fp(uint8_t *d, int32_t idx, int wrapped, uint32_t ring, const uint32_t *words) {
    size_t n = 0x40 + 16 + 4 * (size_t)ring;
    memset(d, 0, n);
    put32(d, 0x01020304); put32(d + 8, 0x40);
    put32(d + 0x20, TFOG_FP_WRAPPED); put32(d + 0x24, (uint32_t)wrapped);
    put32(d + 0x38, TFOG_FP_INDEX); put32(d + 0x3c, (uint32_t)idx);
    put32(d + 0x40 + 0xc, ring);
    for (uint32_t i = 0; i < ring; i++) put32(d + 0x40 + 16 + 4 * i, words[i]);
    return n;
}
/* a table with 3 tiles, 2 shrines, 1 tower; region: tower 0 on the cells of x 0..160, z 0..160 */
static uint8_t *make_table(size_t *n_out) {
    size_t to = 80, so = to + 3 * 16, wo = so + 2 * 16, ro = (wo + 12 + 7) & ~(size_t)7, n = ro + TFOG_NCELLS;
    uint8_t *d = calloc(1, n);
    memcpy(d, "TOTKFOGT", 8); put32(d + 8, 1); put32(d + 12, 3); put32(d + 16, 2); put32(d + 20, 1);
    put32(d + 24, (uint32_t)to); put32(d + 28, (uint32_t)so); put32(d + 32, (uint32_t)wo); put32(d + 36, (uint32_t)ro);
    put32(d + 40, TFOG_GW); put32(d + 44, TFOG_GH);
    for (int z = 0; z < 5; z++) put32(d + 48 + 4 * z, 1000u + (uint32_t)z);
    put32(d + 68, (uint32_t)n);
    const uint64_t keys[3] = {5, 77, 0xfeedfacecafebeefull};
    for (int i = 0; i < 3; i++) { uint8_t *r = d + to + 16 * i; put64(r, keys[i]); r[8] = 1; r[9] = (uint8_t)(6 + i); r[10] = 5; r[11] = (uint8_t)i; r[12] = i == 1; }
    put32(d + so, 0x1111); putf(d + so + 4, 2000); putf(d + so + 8, 2000); d[so + 12] = 1; d[so + 13] = 0; d[so + 14] = 0;
    put32(d + so + 16, 0x2222); putf(d + so + 20, -3000); putf(d + so + 24, 100); d[so + 28] = 0; d[so + 29] = 1; d[so + 30] = 255;
    put32(d + wo, 0x3333); putf(d + wo + 4, 80); putf(d + wo + 8, 80);
    memset(d + ro, 255, TFOG_NCELLS);
    for (int j = 0; j < 10; j++) for (int i = 0; i < 10; i++) d[ro + (size_t)(5000 / 16 + j) * TFOG_GW + 6000 / 16 + i] = 0;
    *n_out = n;
    return d;
}

int main(void) {
    /* key: SHA-256("abc") = ba7816bf8f01cfea..., first 8 bytes as little endian */
    CHECK(tfog_key("abc", 3) == 0xeacf018fbf1678baull);

    /* paint and sample */
    static uint8_t g[TFOG_NCELLS];
    tfog_box box = tfog_box_empty;
    CHECK(tfog_paint_disc(g, 100, 200, 60, 32, &box));
    CHECK(*cell(g, 100, 200) == 255 && *cell(g, 100 + 70, 200) > 0 && *cell(g, 100 + 70, 200) < 255 && *cell(g, 100 + 120, 200) == 0);
    CHECK(box.x0 <= box.x1 && box.x0 <= (100 - TFOG_X0) / 16 && box.x1 >= (100 - TFOG_X0) / 16);
    CHECK(!tfog_paint_disc(g, 100, 200, 60, 32, NULL));   /* only raises */
    CHECK(tfog_sample(g, 100, 200) > 0.99f && tfog_sample(g, 1000, 1000) == 0 && tfog_sample(g, -7000, 0) == 0);
    CHECK(!tfog_paint_disc(g, NAN, 0, 60, 32, NULL) && !tfog_paint_disc(g, 1e30f, 0, 60, 32, NULL));
    tfog_paint_disc(g, 5990, 4990, 60, 32, NULL);   /* the corner cell */
    CHECK(g[TFOG_NCELLS - 1] == 255 && tfog_sample(g, 5999, 4999) > 0.25f);
    memset(g, 0, sizeof g);
    tfog_paint_line(g, -1000, 0, -700, 0, 60, 32, 400, NULL);
    CHECK(*cell(g, -850, 0) == 255);
    memset(g, 0, sizeof g);
    tfog_paint_line(g, -1000, 0, -500, 0, 60, 32, 400, NULL);   /* a 500 m step is a warp */
    CHECK(*cell(g, -750, 0) == 0 && *cell(g, -500, 0) == 255 && *cell(g, -1000, 0) == 0);

    /* footprint.sav */
    static uint8_t fp[0x40 + 16 + 4 * 8];
    static tfog_point pts[16];
    uint32_t words[8];
    for (int i = 0; i < 8; i++) words[i] = fp_word(i % 3, -100 * i, 50 * i - 200);
    size_t n = make_fp(fp, 2, 0, 8, words);
    CHECK(tfog_fp_parse(fp, n, pts, 16) == 3);
    CHECK(pts[0].x == 0 && pts[0].z == -200 && pts[0].layer == 0 && pts[2].x == -200 && pts[2].z == -100 && pts[2].layer == 2);
    n = make_fp(fp, 2, 1, 8, words);
    CHECK(tfog_fp_parse(fp, n, pts, 16) == 8 && pts[0].x == -300 && pts[0].layer == 0 && pts[7].x == -200);
    words[4] = 0; words[5] = 3u << 3;   /* empty word, layer 3: skipped */
    n = make_fp(fp, 2, 1, 8, words);
    CHECK(tfog_fp_parse(fp, n, pts, 16) == 6);
    CHECK(tfog_fp_parse(fp, n, pts, 2) == 2);
    n = make_fp(fp, -1, 0, 8, words);
    CHECK(tfog_fp_parse(fp, n, pts, 16) == 0);
    n = make_fp(fp, 8, 0, 8, words);
    CHECK(tfog_fp_parse(fp, n, pts, 16) == -1);   /* index past the ring */
    n = make_fp(fp, 1, 0, 8, words);
    CHECK(tfog_fp_parse(fp, n - 4, pts, 16) == -1);   /* ring past the file */
    fp[0] = 0;
    CHECK(tfog_fp_parse(fp, n, pts, 16) == -1);
    tfog_point p;
    CHECK(tfog_fp_word(fp_word(1, 4999, -3999), &p) && p.x == 4999 && p.z == -3999 && p.layer == 1);

    /* table */
    size_t tn;
    uint8_t *tb = make_table(&tn);
    tfog_table t;
    CHECK(!tfog_table_parse(&t, tb, tn));
    CHECK(t.ntiles == 3 && t.nshrines == 2 && t.ntowers == 1 && t.img_size[4] == 1004);
    tfog_tile tile;
    CHECK(tfog_table_find(&t, 77, &tile) && tile.ambiguous && tile.col == 7 && tile.zoom == 1);
    CHECK(tfog_table_find(&t, 0xfeedfacecafebeefull, &tile) && !tile.ambiguous && tile.col == 8 && tile.row == 5 && tile.layer == 1);
    CHECK(tfog_table_find(&t, 5, &tile) && !tfog_table_find(&t, 6, &tile) && !tfog_table_find(&t, 0, &tile) && !tfog_table_find(&t, ~0ull, &tile));
    tfog_shrine s0 = tfog_shrine_at(&t, 0), s1 = tfog_shrine_at(&t, 1);
    CHECK(s0.hash == 0x1111 && s0.tower == 0 && s0.layer == 1 && s1.gated && s1.tower == 255 && tfog_tower_at(&t, 0).hash == 0x3333);
    CHECK(tfog_table_parse(&t, tb, tn - 1));                       /* size */
    put64(tb + 80, 100); CHECK(tfog_table_parse(&t, tb, tn)); put64(tb + 80, 5);   /* unsorted */
    tb[tn - 1] = 3; CHECK(tfog_table_parse(&t, tb, tn)); tb[tn - 1] = 255;          /* region names no tower */
    tb[0] = 'X'; CHECK(tfog_table_parse(&t, tb, tn)); tb[0] = 'T';
    CHECK(!tfog_table_parse(&t, tb, tn));

    /* shares and shrine reveal */
    static uint8_t grids[3][TFOG_NCELLS];
    double share[1];
    tfog_shares(&t, grids[1], share);
    CHECK(share[0] == 0);
    for (int j = 0; j < 7; j++) for (int i = 0; i < 10; i++) grids[1][(size_t)(5000 / 16 + j) * TFOG_GW + 6000 / 16 + i] = 200;
    tfog_shares(&t, grids[1], share);
    CHECK(fabs(share[0] - 0.7) < 1e-9);
    tfog_shrine near = {1, 2000, 2000, 1, 0, 255};
    CHECK(!tfog_shrine_revealed(&grids[0][0], &near, share, 65));
    *cell(grids[1], 2020, 2000) = 1;                                /* 20 m away, other layer first */
    CHECK(tfog_shrine_revealed(&grids[0][0], &near, share, 65));
    near.layer = 0;
    CHECK(!tfog_shrine_revealed(&grids[0][0], &near, share, 65));
    *cell(grids[1], 2020, 2000) = 0; *cell(grids[1], 2040, 2000) = 255; near.layer = 1;
    CHECK(!tfog_shrine_revealed(&grids[0][0], &near, share, 65));   /* 40 m: too far */
    near.tower = 0;
    CHECK(tfog_shrine_revealed(&grids[0][0], &near, share, 65) && !tfog_shrine_revealed(&grids[0][0], &near, share, 71) && !tfog_shrine_revealed(&grids[0][0], &near, share, 0));

    /* tower text */
    uint16_t name[48], out[48];
    for (int i = 0; i < 48; i++) name[i] = (uint16_t)('A' + i % 26);
    CHECK(tfog_text_suffix(name, 3, 61, out, 42) == 7 && out[3] == ' ' && out[4] == '6' && out[5] == '1' && out[6] == '%' && out[0] == 'A');
    CHECK(tfog_text_suffix(name, 37, 100, out, 42) == 37);           /* " 100%" does not fit */
    CHECK(tfog_text_suffix(name, 37, 99, out, 42) == 41);
    CHECK(tfog_text_suffix(name, 40, 5, out, 42) == 40 && tfog_text_suffix(name, 45, 5, out, 48) == 41);

    /* grid file */
    static uint8_t file[TFOG_FILE_SIZE], back[3][TFOG_NCELLS];
    tfog_grid_pack(file, &grids[0][0]);
    CHECK(tfog_grid_unpack(file, sizeof file, &back[0][0]) && !memcmp(back, grids, sizeof grids));
    CHECK(!tfog_grid_unpack(file, sizeof file - 1, &back[0][0]));
    file[12] = 99; CHECK(!tfog_grid_unpack(file, sizeof file, &back[0][0]));

    /* slot order */
    char dir[] = "/tmp/test_totk_fog.XXXXXX", p1[256], p2[256];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(p1, sizeof p1, "%s/slot_01.grid", dir); snprintf(p2, sizeof p2, "%s/slot_02.grid", dir);
    static tfog_state st;
    tfog_state_init(&st, 60, 32, 400);
    tfog_push(&st, 1, 1000, 1000);                                   /* a new game: no load */
    pthread_mutex_lock(&st.mu); CHECK(tfog_drain_locked(&st) == 1); pthread_mutex_unlock(&st.mu);
    CHECK(*cell(st.grid[1], 1000, 1000) == 255 && st.dirty[1].x0 <= st.dirty[1].x1);
    tfog_push(&st, 0, 1500, 1000);                                   /* queued at the save */
    CHECK(tfog_on_save(&st, 1, p1) && st.slot == 1);
    size_t fn = 0; uint8_t *f = tfog_slurp(p1, TFOG_FILE_SIZE, &fn);
    CHECK(f && tfog_grid_unpack(f, fn, &back[0][0]) && back[0][(size_t)((1000 - TFOG_Z0) / 16) * TFOG_GW + (1500 - TFOG_X0) / 16] == 255);
    free(f);
    tfog_push(&st, 1, -2000, 0);                                     /* recorded before the load of slot 2 */
    words[0] = fp_word(2, 300, 300); words[1] = fp_word(2, 340, 300);
    n = make_fp(fp, 1, 0, 8, words);
    int k = -9;
    CHECK(tfog_on_load(&st, 2, p2, fp, n, &k) == 0 && k == 2);
    CHECK(*cell(st.grid[1], 1000, 1000) == 0 && *cell(st.grid[2], 320, 300) == 255 && !st.have_prev[2]);
    pthread_mutex_lock(&st.mu); CHECK(tfog_drain_locked(&st) == 0 && st.dropped == 1); pthread_mutex_unlock(&st.mu);
    CHECK(*cell(st.grid[1], -2000, 0) == 0);
    tfog_push(&st, 1, -2000, 0);
    CHECK(tfog_on_load(&st, 2, p2, NULL, 0, NULL) == -1);            /* the live slot read again: kept */
    pthread_mutex_lock(&st.mu); CHECK(tfog_drain_locked(&st) == 1); pthread_mutex_unlock(&st.mu);
    CHECK(*cell(st.grid[1], -2000, 0) == 255);
    CHECK(tfog_on_load(&st, 1, p1, NULL, 0, NULL) == 1 && *cell(st.grid[1], 1000, 1000) == 255 && *cell(st.grid[0], 1500, 1000) == 255 && *cell(st.grid[1], -2000, 0) == 0);
    CHECK(st.dirty[0].x0 == 0 && st.dirty[0].x1 == TFOG_GW - 1);
    for (int i = 0; i < TFOG_RING + 5; i++) tfog_push(&st, 1, (float)i, 0);
    CHECK(st.ring_n == TFOG_RING && st.ring_lost == 5);
    /* lines: none across a trip to the depths; surface - sky - surface keeps its line */
    static tfog_state ls;
    tfog_state_init(&ls, 60, 32, 400);
    tfog_push(&ls, 1, 0, 2000); tfog_push(&ls, 2, 150, 2000); tfog_push(&ls, 1, 300, 2000);
    tfog_push(&ls, 1, 0, 3000); tfog_push(&ls, 0, 150, 3000); tfog_push(&ls, 1, 300, 3000);
    pthread_mutex_lock(&ls.mu); CHECK(tfog_drain_locked(&ls) == 6); pthread_mutex_unlock(&ls.mu);
    CHECK(*cell(ls.grid[1], 150, 2000) == 0 && *cell(ls.grid[1], 300, 2000) == 255 && *cell(ls.grid[2], 150, 2000) == 255);
    CHECK(*cell(ls.grid[1], 150, 3000) == 255 && *cell(ls.grid[0], 150, 3000) == 255);
    tfog_push(&ls, 1, 0, 4000); tfog_push(&ls, 2, 150, 4000); tfog_push(&ls, 0, 300, 4000); tfog_push(&ls, 1, 300, 4000);   /* out by a warp to the sky */
    tfog_push(&ls, 0, 0, 5000); tfog_push(&ls, 2, 150, 5000); tfog_push(&ls, 1, 300, 5000); tfog_push(&ls, 0, 300, 5000);
    pthread_mutex_lock(&ls.mu); CHECK(tfog_drain_locked(&ls) == 8); pthread_mutex_unlock(&ls.mu);
    CHECK(*cell(ls.grid[1], 150, 4000) == 0 && *cell(ls.grid[0], 150, 5000) == 0 && *cell(ls.grid[1], 300, 4000) == 255);
    unlink(p1); rmdir(dir);
    free(tb);

    if (fails) { fprintf(stderr, "test_totk_fog: %d failure(s)\n", fails); return 1; }
    printf("test_totk_fog: ok\n");
    return 0;
}
