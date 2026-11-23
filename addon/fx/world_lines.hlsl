// Cinematic Replay: the editor's 3D lines (camera paths, key frustums) inside the game's picture. The addon's code
// projects them with the camera the game renders this frame and sends them every frame (world_lines.hpp: header,
// a tile table, segments, a 16-bit index). Where the game drew something in front of a line, the line is drawn
// faint and dashed instead: its depth (near / distance, linear on screen) against the game's depth buffer.
// Without data (editor closed, paths off) the picture is untouched.
#include "swofty/postfx.hlsli"

static const uint kMagic = 0x31575243u;  // "CRW1"
static const uint kSegBytes = 40u;
// The data is only ever read within these limits, whatever it says (world_lines.hpp packs within them), so bad bytes
// draw a wrong picture, never a GPU hang.
static const uint kMaxPerTile = 256u, kMaxTiles = 1u << 16;

float4 rgba_of(uint w)
{
    return float4((w >> 24) & 255u, (w >> 16) & 255u, (w >> 8) & 255u, w & 255u) / 255.0;
}

float4 effect(float2 uv, uint2 pixel)
{
    float4 c = scene_at(int2(pixel));
    if (data_size() < 48u || data_uint(0) != kMagic)
        return c;
    float W = data_float(4), H = data_float(8);
    uint tx_n = data_uint(12), ty_n = data_uint(16), tile = max(data_uint(20), 8u);
    uint seg_at = data_uint(28), idx_at = data_uint(32);
    float xray = saturate(data_float(36)), dash = clamp(data_float(40), 2.0, 1000.0);
    bool depth_on = (data_uint(44) & 1u) != 0 && has_depth();
    if (tx_n == 0u || ty_n == 0u || tx_n > kMaxTiles / ty_n || 48u + 4u * tx_n * ty_n > data_size() ||
        !(W >= 16.0 && W <= 32768.0 && H >= 16.0 && H <= 32768.0))
        return c;
    float2 p = (float2(pixel) + 0.5) * float2(W, H) / float2(swofty_size);
    uint tx = min(uint(max(p.x, 0.0)) / tile, tx_n - 1u), ty = min(uint(max(p.y, 0.0)) / tile, ty_n - 1u);
    uint t = data_uint(48u + 4u * (ty * tx_n + tx));
    uint first = t & 0xffffu, count = min(t >> 16, kMaxPerTile);
    if (count == 0u)
        return c;
    float scene_d = depth_on ? scene_depth(uv) : 0.0;
    float3 col = to_linear(c.rgb);
    // two rounds, as the overlay draws: every dark outline first, then the lines over them
    [loop] for (uint round = 0u; round < 2u; round++) {
        [loop] for (uint i = 0u; i < count; i++) {
            uint e = first + i;
            uint pair = data_uint(idx_at + (e >> 1) * 4u);
            uint s = (e & 1u) != 0u ? pair >> 16 : pair & 0xffffu;
            uint o = seg_at + s * kSegBytes;
            float4 ab = data_float4(o);
            float d0 = data_float(o + 16u), d1 = data_float(o + 20u);
            uint wid = data_uint(o + 28u);
            float half_w = float(wid & 0xffffu) / 16.0, out_w = float(wid >> 16) / 16.0;
            float2 a = ab.xy, b = ab.zw, ba = b - a;
            float len2 = max(dot(ba, ba), 1e-6);
            float h = saturate(dot(p - a, ba) / len2);
            float dist = length(p - (a + ba * h));
            float reach = round == 0u ? half_w + out_w : half_w;
            float cover = saturate(reach + 0.5 - dist);
            if (cover <= 0.0 || (round == 0u && out_w <= 0.0))
                continue;
            float dl = lerp(d0, d1, h);
            bool hidden = depth_on && dl < scene_d * 0.999;
            if (hidden) {
                if (round == 0u)
                    continue;  // no outline behind things
                float along = data_float(o + 32u) + h * sqrt(len2);
                if (frac(along / dash) >= 0.5)
                    continue;
            }
            float4 lc = rgba_of(data_uint(o + 24u));
            if (round == 0u)
                col = lerp(col, 0.0, cover * 0.63 * lc.a);
            else
                col = lerp(col, srgb_to_linear(lc.rgb), cover * lc.a * (hidden ? xray : 1.0));
        }
    }
    return float4(from_linear(col), c.a);
}
