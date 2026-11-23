    // ---------------- world lines (world_lines.hpp): the pass's data ----------------
    {
        using namespace cine::wl;
        auto u32 = [](const std::string &b, size_t at) {
            uint32_t w;
            memcpy(&w, &b[at], 4);
            return w;
        };
        auto f32 = [](const std::string &b, size_t at) {
            float f;
            memcpy(&f, &b[at], 4);
            return f;
        };
        // identity rotation looks along -z; fov 90 over 1280 px: f = 640
        View v{{0, 0, 0}, Quat{}, 90.0f, 1280.0f, 720.0f, 0.05f};
        uint32_t white = 0xffffffffu;

        // a segment straight ahead, 10 m away: through the centre, depth near / 10
        Packed one = pack({{{-1, 0, -10}, {1, 0, -10}, 6, white}}, v, 64 * 1024);
        CHECK(!one.bytes.empty() && u32(one.bytes, 0) == kMagic, "world lines: packed with the magic");
        CHECK(u32(one.bytes, 12) == 40 && u32(one.bytes, 16) == 23 && u32(one.bytes, 20) == 32,
              "world lines: 40 x 23 tiles of 32 px (got %u x %u)", u32(one.bytes, 12), u32(one.bytes, 16));
        CHECK(u32(one.bytes, 24) == 1 && one.shown == 1 && one.kept == 1, "world lines: one segment");
        size_t seg = u32(one.bytes, 28);
        CHECK(seg % 16 == 0, "world lines: segments 16-byte aligned");
        CHECK(std::fabs(f32(one.bytes, seg) - 576) < 0.01f && std::fabs(f32(one.bytes, seg + 8) - 704) < 0.01f &&
                  std::fabs(f32(one.bytes, seg + 4) - 360) < 0.01f,
              "world lines: x from 576 to 704 at y 360 (got %.2f..%.2f at %.2f)", f32(one.bytes, seg),
              f32(one.bytes, seg + 8), f32(one.bytes, seg + 4));
        CHECK(std::fabs(f32(one.bytes, seg + 16) - 0.005f) < 1e-7f, "world lines: depth near / distance");
        CHECK(u32(one.bytes, seg + 28) == 3u * 16u, "world lines: half width 3 px, no outline");
        // its tiles: row 360 / 32 = 11, columns 576..704 (+4 px) / 32 = 17..22; a tile elsewhere has none
        auto count = [&](const std::string &b, uint32_t tx, uint32_t ty) { return u32(b, (kHeaderWords + ty * u32(b, 12) + tx) * 4) >> 16; };
        CHECK(count(one.bytes, 17, 11) == 1 && count(one.bytes, 22, 11) == 1 && count(one.bytes, 20, 11) == 1,
              "world lines: in the tiles it crosses");
        CHECK(count(one.bytes, 16, 11) == 0 && count(one.bytes, 20, 10) == 0 && count(one.bytes, 0, 0) == 0,
              "world lines: not in the tiles beside it");

        // behind the camera, or off to the side: nothing
        Packed none = pack({{{-1, 0, 10}, {1, 0, 10}, 6, white}, {{500, 0, -10}, {502, 0, -10}, 6, white}}, v, 64 * 1024);
        CHECK(u32(none.bytes, 24) == 0 && none.shown == 0 && none.kept == 2, "world lines: culled behind and off screen");

        // from behind the camera to ahead: cut at the near plane, kept
        Packed cut = pack({{{0, -1, 5}, {0, -1, -10}, 4, white}}, v, 64 * 1024);
        seg = u32(cut.bytes, 28);
        CHECK(u32(cut.bytes, 24) == 1 && std::fabs(f32(cut.bytes, seg + 16) - 0.05f / kNear) < 1e-5f,
              "world lines: cut at the near plane (depth %.4f)", u32(cut.bytes, 24) ? f32(cut.bytes, seg + 16) : -1.f);

        // far away: faded as the overlay fades; a polyline's second segment starts where the first ends (dashes)
        Packed far = pack({{{-10, 0, -400}, {10, 0, -400}, 4, 0xff0000ffu}, {{10, 0, -400}, {30, 0, -400}, 4, 0xff0000ffu, 0, true}},
                          v, 64 * 1024);
        seg = u32(far.bytes, 28);
        uint32_t a = u32(far.bytes, seg + 24) & 0xff;
        CHECK(a == uint32_t(fade(400) * 255 + 0.5f), "world lines: faded with distance (alpha %u)", a);
        float len0 = f32(far.bytes, seg + 8) - f32(far.bytes, seg);
        CHECK(std::fabs(f32(far.bytes, seg + 40 + 32) - len0) < 1e-3f, "world lines: dashes run on along the line");

        // too many for the cap: the first ones kept, within the bytes allowed
        std::vector<Seg> many;
        for (int i = 0; i < 4000; i++)
            many.push_back({{-5.0f + i * 0.0025f, -2, -10}, {-5.0f + i * 0.0025f, 2, -10}, 3, white});
        Packed capped = pack(many, v, 64 * 1024);
        CHECK(!capped.bytes.empty() && capped.bytes.size() <= 64 * 1024 && capped.kept > 100 && capped.kept < many.size(),
              "world lines: capped at 64 KB (%zu bytes, %zu of %zu kept)", capped.bytes.size(), capped.kept, many.size());
        CHECK(u32(capped.bytes, 24) == capped.shown, "world lines: header counts what was kept");

        // a crowded tile: the pass reads at most kMaxPerTile segments a tile, so the tile lists its first ones
        std::vector<Seg> crowd;
        for (int i = 0; i < 600; i++)
            crowd.push_back({{-0.01f, 0.0001f * i, -10}, {0.01f, 0.0001f * i, -10}, 2, white});
        Packed crowded = pack(crowd, v, 64 * 1024);
        CHECK(crowded.shown == crowd.size(), "world lines: the crowd fits the bytes (%zu shown)", crowded.shown);
        uint32_t tw = u32(crowded.bytes, (kHeaderWords + 11 * 40 + 20) * 4);
        size_t idx = u32(crowded.bytes, 32), first = tw & 0xffff;
        auto u16 = [](const std::string &b, size_t at) {
            uint16_t w;
            memcpy(&w, &b[at], 2);
            return size_t(w);
        };
        CHECK((tw >> 16) == kMaxPerTile && u16(crowded.bytes, idx + first * 2) == 0 &&
                  u16(crowded.bytes, idx + (first + kMaxPerTile - 1) * 2) == kMaxPerTile - 1,
              "world lines: a crowded tile lists its first %zu segments (got %u)", kMaxPerTile, tw >> 16);
    }
