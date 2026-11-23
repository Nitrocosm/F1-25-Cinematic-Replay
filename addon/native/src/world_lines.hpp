// The editor's 3D lines (camera paths, key frustums) drawn inside the game's picture by the addon's post-process
// pass (fx/world_lines.hlsl), behind whatever the game drew in front of them, instead of over everything as the
// overlay draws. Here: the segments are projected with the camera the game renders this frame, cut at the near
// plane, culled off screen, and packed for the pass (swofty::postfx::set_data) with a tile index, so a pixel tests
// only the segments that pass near it. No game code: unit-tested in tests/world_lines_tests.inc.
#pragma once

#include "shots.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace cine::wl {

// A world-space segment, in drawing order (later ones over earlier ones). width: pixels; outline: the dark border's
// extra width on each side (0: none); rgba: 0xRRGGBBAA, display-encoded; cont: it continues the previous segment
// (one polyline: hidden stretches are dashed along it).
struct Seg {
    V3 a, b;
    float width;
    uint32_t rgba;
    float outline = 0;
    bool cont = false;
};

// The camera the game renders with: eye, rotation, horizontal field of view (degrees), the picture's size in
// pixels, and the near plane of its projection (reverse Z: depth = near_plane / distance along the view).
struct View {
    V3 eye;
    Quat q;
    float fov;
    float W, H;
    float near_plane;
};

// The pass's data (little-endian 32-bit words): a header of kHeaderWords, the tile table (one word a tile:
// count << 16 | first index), the segments (kSegWords each), then the index (16-bit segment numbers, two a word).
constexpr uint32_t kMagic = 0x31575243;  // "CRW1"
constexpr uint32_t kHeaderWords = 12, kSegWords = 10;
constexpr size_t kMaxPerTile = 256;  // the pass reads at most this many segments a tile (fx/world_lines.hlsl)
constexpr float kNear = 0.2f;  // lines are cut this close to the camera, as the overlay cuts them
enum : uint32_t { kFlagDepth = 1 };

struct Packed {
    std::string bytes;
    size_t kept = 0;  // segments drawn (the first `kept`; the rest didn't fit)
    size_t shown = 0; // of those, on screen
};

// The overlay's fade with distance (depth_alpha in cinecam.cpp).
inline float fade(float z) { return std::clamp(1.2f - z / 300.0f, 0.3f, 1.0f); }

namespace detail {

struct Proj {
    V3 eye, r, up, back;
    float f, cx, cy;
    explicit Proj(const View &v)
    {
        eye = v.eye;
        cols_from_quat(v.q, r, up, back);
        f = (v.W / 2) / std::tan(std::clamp(v.fov, 1.0f, 170.0f) * kDeg / 2);
        cx = v.W / 2;
        cy = v.H / 2;
    }
    V3 cam(V3 p) const
    {
        V3 d = p - eye;
        return {dot(d, r), dot(d, up), -dot(d, back)};
    }
};

struct Screen {  // a projected segment
    float x0, y0, x1, y1, d0, d1, s0, half, outline;
    uint32_t rgba;
};

inline void put(std::string &out, size_t at, uint32_t w) { memcpy(&out[at], &w, 4); }
inline void putf(std::string &out, size_t at, float f) { memcpy(&out[at], &f, 4); }

// Tiles a segment touches, row by row: in each tile row, the segment's x range there, widened by pad.
template <class Fn> void tiles_of(const Screen &s, float pad, int tile, int tx_n, int ty_n, Fn fn)
{
    float ymin = std::min(s.y0, s.y1) - pad, ymax = std::max(s.y0, s.y1) + pad;
    int ty0 = std::max(0, int(std::floor(ymin / float(tile)))), ty1 = std::min(ty_n - 1, int(std::floor(ymax / float(tile))));
    for (int ty = ty0; ty <= ty1; ty++) {
        float ra = float(ty * tile) - pad, rb = float((ty + 1) * tile) + pad;
        float dy = s.y1 - s.y0, t0 = 0, t1 = 1;
        if (std::fabs(dy) > 1e-6f) {
            float ta = (ra - s.y0) / dy, tb = (rb - s.y0) / dy;
            t0 = std::max(0.0f, std::min(ta, tb));
            t1 = std::min(1.0f, std::max(ta, tb));
            if (t0 > t1)
                continue;
        }
        float xa = s.x0 + (s.x1 - s.x0) * t0, xb = s.x0 + (s.x1 - s.x0) * t1;
        int tx0 = std::max(0, int(std::floor((std::min(xa, xb) - pad) / float(tile))));
        int tx1 = std::min(tx_n - 1, int(std::floor((std::max(xa, xb) + pad) / float(tile))));
        for (int tx = tx0; tx <= tx1; tx++)
            fn(ty * tx_n + tx);
    }
}

}

// Projects and packs segs for the pass, keeping the first segments that fit in max_bytes. xray: the alpha of a
// line's stretches hidden behind the game's picture; dash: their dash period in pixels; depth: whether the pass
// should test against the game's depth at all.
inline Packed pack(const std::vector<Seg> &segs, const View &v, size_t max_bytes, int tile = 32, float xray = 0.35f,
                   float dash = 10.0f, bool depth = true)
{
    Packed res;
    if (v.W < 16 || v.H < 16 || tile < 8)
        return res;
    detail::Proj pr(v);
    std::vector<detail::Screen> scr;
    std::vector<size_t> from;  // the seg each screen segment came from
    scr.reserve(segs.size());
    float s_run = 0;
    for (size_t i = 0; i < segs.size(); i++) {
        const Seg &g = segs[i];
        if (!g.cont)
            s_run = 0;
        V3 ca = pr.cam(g.a), cb = pr.cam(g.b);
        if (ca.z < kNear && cb.z < kNear)
            continue;
        if (ca.z < kNear)
            ca = ca + (cb - ca) * ((kNear - ca.z) / (cb.z - ca.z));
        else if (cb.z < kNear)
            cb = cb + (ca - cb) * ((kNear - cb.z) / (ca.z - cb.z));
        detail::Screen s{};
        s.x0 = pr.cx + ca.x / ca.z * pr.f;
        s.y0 = pr.cy - ca.y / ca.z * pr.f;
        s.x1 = pr.cx + cb.x / cb.z * pr.f;
        s.y1 = pr.cy - cb.y / cb.z * pr.f;
        s.d0 = v.near_plane / ca.z;
        s.d1 = v.near_plane / cb.z;
        s.half = std::max(0.5f, g.width / 2);
        s.outline = std::max(0.0f, g.outline);
        float a = float(g.rgba & 0xff) / 255.0f * fade(std::min(ca.z, cb.z));
        s.rgba = (g.rgba & 0xffffff00u) | uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        s.s0 = s_run;
        float len2d = std::hypot(s.x1 - s.x0, s.y1 - s.y0);
        s_run += len2d;
        float pad = s.half + s.outline + 1;
        if (std::max(s.x0, s.x1) < -pad || std::min(s.x0, s.x1) > v.W + pad || std::max(s.y0, s.y1) < -pad ||
            std::min(s.y0, s.y1) > v.H + pad)
            continue;
        scr.push_back(s);
        from.push_back(i);
    }
    int tx_n = int(std::ceil(v.W / float(tile))), ty_n = int(std::ceil(v.H / float(tile)));
    size_t tiles = size_t(tx_n) * size_t(ty_n);
    size_t fixed = (kHeaderWords + tiles) * 4;
    if (fixed + 64 > max_bytes)
        return res;
    // the most segments (from the front) whose packing fits
    size_t n = std::min(scr.size(), size_t(0xffff));
    std::vector<std::vector<uint16_t>> lists;
    size_t entries = 0;
    for (;;) {
        lists.assign(tiles, {});
        entries = 0;
        for (size_t i = 0; i < n; i++)
            detail::tiles_of(scr[i], scr[i].half + scr[i].outline + 1, tile, tx_n, ty_n, [&](int t) {
                if (lists[size_t(t)].size() < kMaxPerTile) {  // a crowded tile keeps its first segments
                    lists[size_t(t)].push_back(uint16_t(i));
                    entries++;
                }
            });
        size_t seg_at = (fixed + 15) / 16 * 16;
        size_t total = seg_at + n * kSegWords * 4 + (entries + 1) / 2 * 4;
        if (total <= max_bytes || n == 0)
            break;
        n = std::min(n - 1, size_t(double(n) * double(max_bytes) / double(total) * 0.95));
    }
    if (entries > 0xffff)
        return res;
    size_t seg_at = (fixed + 15) / 16 * 16, idx_at = seg_at + n * kSegWords * 4;
    std::string &out = res.bytes;
    out.assign(idx_at + (entries + 1) / 2 * 4, '\0');
    detail::put(out, 0, kMagic);
    detail::putf(out, 4, v.W);
    detail::putf(out, 8, v.H);
    detail::put(out, 12, uint32_t(tx_n));
    detail::put(out, 16, uint32_t(ty_n));
    detail::put(out, 20, uint32_t(tile));
    detail::put(out, 24, uint32_t(n));
    detail::put(out, 28, uint32_t(seg_at));
    detail::put(out, 32, uint32_t(idx_at));
    detail::putf(out, 36, xray);
    detail::putf(out, 40, dash);
    detail::put(out, 44, depth ? kFlagDepth : 0u);
    uint32_t first = 0;
    std::vector<uint16_t> index;
    index.reserve(entries);
    for (size_t t = 0; t < tiles; t++) {
        uint32_t count = uint32_t(lists[t].size());
        detail::put(out, (kHeaderWords + t) * 4, count << 16 | first);
        index.insert(index.end(), lists[t].begin(), lists[t].end());
        first += count;
    }
    for (size_t i = 0; i < n; i++) {
        const detail::Screen &s = scr[i];
        size_t at = seg_at + i * kSegWords * 4;
        float f[6] = {s.x0, s.y0, s.x1, s.y1, s.d0, s.d1};
        for (int k = 0; k < 6; k++)
            detail::putf(out, at + size_t(k) * 4, f[k]);
        detail::put(out, at + 24, s.rgba);
        uint32_t hw = uint32_t(std::min(s.half * 16.0f, 65535.0f)), ow = uint32_t(std::min(s.outline * 16.0f, 65535.0f));
        detail::put(out, at + 28, ow << 16 | hw);
        detail::putf(out, at + 32, s.s0);
    }
    if (!index.empty())
        memcpy(&out[idx_at], index.data(), index.size() * 2);
    res.shown = n;
    res.kept = n < scr.size() ? (n ? from[n - 1] + 1 : 0) : segs.size();
    return res;
}

}
