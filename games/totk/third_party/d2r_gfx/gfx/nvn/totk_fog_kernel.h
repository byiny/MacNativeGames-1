/* Tears of the Kingdom 1.4.3 fog-of-war map: the Metal kernel that darkens a map tile and encodes it as BC1, and the
   helper that encodes one dispatch. Used by totk_fog_gfx.h and by the local Metal test
   (dev-data/totk/keep/local-patches/0024-totk-fog-map-metal-test.m).

   One thread per 4 x 4 block (edge blocks of the 135 and 27 px tiles repeat the last row and column): each texel is read
   from the tile (the encoded sRGB values: the source texture is a non-sRGB copy), multiplied by dark + (1 - dark) x the
   grid's clearness at the texel's world place (bilinear over cell centres, as tfog_sample in totk_fog_core.h), and the
   block is encoded: endpoints at the ends of the colours' principal axis (power iteration on the block covariance), inset by 1/16, endpoints rounded to 565, c0 > c1 (4-colour mode; equal endpoints read
   as one colour with indices 0), indices by projection onto the endpoint line. */
#pragma once
#import <Metal/Metal.h>

static const char g_totk_fog_msl[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct TfParams { uint w, h, bw, bh; float x0, z0, dark, pad; };\n"
    "static float tf_at(const device uchar *g, int i, int j) { return i < 0 || j < 0 || i >= 750 || j >= 625 ? 0.0f : float(g[j * 750 + i]) / 255.0f; }\n"
    "static float tf_clear(const device uchar *g, float x, float z) {\n"
    "    float fx = (x + 6000.0f) / 16.0f - 0.5f, fz = (z + 5000.0f) / 16.0f - 0.5f;\n"
    "    if (!(fx > -1.0f && fz > -1.0f && fx < 750.0f && fz < 625.0f)) return 0.0f;\n"
    "    int i = int(floor(fx)), j = int(floor(fz)); float ax = fx - float(i), az = fz - float(j);\n"
    "    return (tf_at(g, i, j) * (1 - ax) + tf_at(g, i + 1, j) * ax) * (1 - az) + (tf_at(g, i, j + 1) * (1 - ax) + tf_at(g, i + 1, j + 1) * ax) * az;\n"
    "}\n"
    "static ushort tf_565(float3 c) { uint3 q = uint3(clamp(c, 0.0f, 1.0f) * float3(31, 63, 31) + 0.5f); return ushort(q.r << 11 | q.g << 5 | q.b); }\n"
    "static float3 tf_rgb(ushort v) { return float3(float(v >> 11) / 31.0f, float(v >> 5 & 63) / 63.0f, float(v & 31) / 31.0f); }\n"
    "kernel void totk_fog_bc1(texture2d<float, access::read> src [[texture(0)]], const device uchar *grid [[buffer(0)]],\n"
    "                         device uint2 *out [[buffer(1)]], constant TfParams &p [[buffer(2)]], uint2 b [[thread_position_in_grid]]) {\n"
    "    if (b.x >= p.bw || b.y >= p.bh) return;\n"
    "    float3 px[16], lo = float3(1), hi = float3(0);\n"
    "    for (uint k = 0; k < 16; k++) {\n"
    "        uint x = min(b.x * 4 + (k & 3), p.w - 1), y = min(b.y * 4 + (k >> 2), p.h - 1);\n"
    "        float wx = p.x0 + (float(x) + 0.5f) * 1000.0f / float(p.w), wz = p.z0 + (float(y) + 0.5f) * 1000.0f / float(p.h);\n"
    "        float f = p.dark + (1.0f - p.dark) * tf_clear(grid, wx, wz);\n"
    "        float3 c = clamp(src.read(uint2(x, y)).rgb * f, 0.0f, 1.0f);\n"
    "        px[k] = c; lo = min(lo, c); hi = max(hi, c);\n"
    "    }\n"
    "    float3 mean = float3(0);\n"
    "    for (uint k = 0; k < 16; k++) mean += px[k] / 16.0f;\n"
    "    float3 cx = float3(0), cy = float3(0), cz = float3(0);\n"
    "    for (uint k = 0; k < 16; k++) { float3 d = px[k] - mean; cx += d * d.x; cy += d * d.y; cz += d * d.z; }\n"
    "    float3 axis = hi - lo;\n"
    "    for (uint it = 0; it < 6; it++) { float3 n = float3(dot(cx, axis), dot(cy, axis), dot(cz, axis)); float m = max(max(abs(n.x), abs(n.y)), abs(n.z)); if (m < 1e-12f) break; axis = n / m; }\n"
    "    float al = length(axis);\n"
    "    float3 e_hi = hi, e_lo = lo;\n"
    "    if (al > 1e-6f) {\n"
    "        axis /= al; float tmin = 1e9f, tmax = -1e9f;\n"
    "        for (uint k = 0; k < 16; k++) { float t = dot(px[k] - mean, axis); tmin = min(tmin, t); tmax = max(tmax, t); }\n"
    "        float inset = (tmax - tmin) / 16.0f;\n"
    "        e_hi = mean + (tmax - inset) * axis; e_lo = mean + (tmin + inset) * axis;\n"
    "    }\n"
    "    ushort c0 = tf_565(e_hi), c1 = tf_565(e_lo);\n"
    "    if (c0 < c1) { ushort t = c0; c0 = c1; c1 = t; }\n"
    "    uint idx = 0;\n"
    "    if (c0 != c1) {\n"
    "        float3 e0 = tf_rgb(c0), e1 = tf_rgb(c1), d = e0 - e1; float dd = dot(d, d);\n"
    "        for (uint k = 0; k < 16; k++) {\n"
    "            float t = dot(px[k] - e1, d) / dd;\n"
    "            uint i = t >= 5.0f / 6.0f ? 0u : t >= 0.5f ? 2u : t >= 1.0f / 6.0f ? 3u : 1u;\n"
    "            idx |= i << (2 * k);\n"
    "        }\n"
    "    }\n"
    "    out[b.y * p.bw + b.x] = uint2(uint(c0) | uint(c1) << 16, idx);\n"
    "}\n";

typedef struct { uint32_t w, h, bw, bh; float x0, z0, dark, pad; } totk_fog_params;

/* Compute pass: src (w x h, any readable colour format) darkened by grid (750 x 625 bytes) into out (bw x bh x 8 bytes).
   x0, z0: the tile's world origin. */
static void totk_fog_encode(id<MTLCommandBuffer> cb, id<MTLComputePipelineState> ps, id<MTLTexture> src, id<MTLBuffer> grid,
                            id<MTLBuffer> out, uint32_t w, uint32_t h, float x0, float z0, float dark) {
    totk_fog_params p = {w, h, (w + 3) / 4, (h + 3) / 4, x0, z0, dark, 0};
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:ps];
    [ce setTexture:src atIndex:0];
    [ce setBuffer:grid offset:0 atIndex:0];
    [ce setBuffer:out offset:0 atIndex:1];
    [ce setBytes:&p length:sizeof p atIndex:2];
    [ce dispatchThreads:MTLSizeMake(p.bw, p.bh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [ce endEncoding];
}
/* Blit pass: the BC1 blocks into a BC1 texture of the same size. */
static void totk_fog_blit(id<MTLCommandBuffer> cb, id<MTLBuffer> out, id<MTLTexture> dst, uint32_t w, uint32_t h) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromBuffer:out sourceOffset:0 sourceBytesPerRow:(NSUInteger)bw * 8 sourceBytesPerImage:(NSUInteger)bw * bh * 8
            sourceSize:MTLSizeMake(w, h, 1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [be endEncoding];
}
static id<MTLComputePipelineState> totk_fog_pipeline(id<MTLDevice> dev, NSError **err) {
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(g_totk_fog_msl) options:nil error:err];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"totk_fog_bc1"] : nil;
    return fn ? [dev newComputePipelineStateWithFunction:fn error:err] : nil;
}
