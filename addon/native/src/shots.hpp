// Shots: the edit is a row of clips on the replay timeline. Each shot has a type (how the camera behaves), a
// time range, a driver (car slot) where it needs one, and optional keys that animate it. A cut happens where
// one shot meets the next; gaps show the game's own camera. No game dependencies (tests/seq_test.cpp).
//
// Every shot works in ONE frame: the world (free path, track cam) or its driver's car (onboard, chase, orbit;
// chase and orbit use a level version of the car's frame so the horizon doesn't shake). Keys store their
// position and rotation in that frame, so a key never switches anchors.
//
// Motion is continuous through the whole shot: positions, rotations (quaternion components in one
// hemisphere), FOV and roll each follow a natural cubic spline through every key of the shot, with the key
// times as knots, so speed AND acceleration carry through every key. "Constant speed" re-times the path by
// arc length; ease in / out only touch the shot's ends.
#pragma once

#include "freecam.hpp"

#include <string>
#include <vector>

namespace cine {

// transition: fills the gap between two shots, gliding from the one before to the one after (transition_view).
enum class ShotType { free_path, onboard, chase, track, orbit, transition };

inline const char *shot_type_name(ShotType t)
{
    switch (t) {
    case ShotType::free_path: return "Free path";
    case ShotType::onboard: return "Onboard";
    case ShotType::chase: return "Chase";
    case ShotType::track: return "Track cam";
    case ShotType::orbit: return "Orbit";
    case ShotType::transition: return "Transition";
    }
    return "?";
}
inline const char *shot_type_id(ShotType t)
{
    switch (t) {
    case ShotType::free_path: return "free";
    case ShotType::onboard: return "onboard";
    case ShotType::chase: return "chase";
    case ShotType::track: return "track";
    case ShotType::orbit: return "orbit";
    case ShotType::transition: return "transition";
    }
    return "free";
}
inline ShotType shot_type_from(std::string_view s)
{
    if (s == "onboard") return ShotType::onboard;
    if (s == "chase") return ShotType::chase;
    if (s == "track") return ShotType::track;
    if (s == "orbit") return ShotType::orbit;
    if (s == "transition") return ShotType::transition;
    return ShotType::free_path;
}

// Mount points on a car (right, up, forward from the car's origin, which sits near the nose; metres) and the
// view from them. Measured in the game where noted in docs; the rest are first estimates.
struct Mount {
    const char *id, *name;
    V3 offset;
    float yaw, pitch, fov;
};
// Offsets in the car frame (right, up, forward) from the car's origin, which sits about mid-car (between the axles,
// nearer the rear). Tuned live by the owner on 2026-11-04 (Austria replay, Gasly's car), except the wheels FR, RL and
// RR; the wheels were then set from screenshots: just outside the tyre, the front ones looking
// forward, the rear ones ahead of the tyre looking back, the tyre at the frame's edge.
inline const Mount kMounts[] = {
    {"cockpit", "Cockpit", {0, 0.43f, 0.25f}, 0, -3, 120},
    {"tcam", "T-cam", {0, 0.82f, -0.95f}, 0, -6, 75},
    {"nose", "Nose", {0, 0.17f, 2.45f}, 0, -2, 80},
    {"front_wing", "Front wing", {0.35f, 0.01f, 2.15f}, 0, -1, 80},
    {"rear_wing", "Rear wing", {0, 0.75f, -2.11f}, 0, -8, 75},
    {"sidepod_l", "Sidepod L", {-0.48f, 0.4f, -0.9f}, 0, -3, 75},
    {"sidepod_r", "Sidepod R", {0.48f, 0.4f, -0.9f}, 0, -3, 75},
    {"wheel_fl", "Wheel FL", {-1.2f, 0.4f, 1.0f}, 0, -3, 70},
    {"wheel_fr", "Wheel FR", {1.2f, 0.4f, 1.0f}, 0, -3, 70},
    {"wheel_rl", "Wheel RL", {-1.15f, 0.35f, -1.1f}, 180, -3, 70},
    {"wheel_rr", "Wheel RR", {1.15f, 0.35f, -1.1f}, 180, -3, 70},
};

// What track / chase / orbit shots aim at and circle: about the cockpit, in the car frame.
inline constexpr V3 kCarCentre{0, 0.45f, -0.4f};
inline const Mount *find_mount(std::string_view id)
{
    for (auto &m : kMounts)
        if (id == m.id)
            return &m;
    return nullptr;
}

// Chase presets: behind the car in its level frame, looking at the car.
struct ChasePreset {
    const char *id, *name;
    float distance, height, fov;
};
inline const ChasePreset kChases[] = {
    {"close", "Close", 6, 1.4f, 65},
    {"far", "Far", 12, 2.4f, 55},
    {"high", "High", 12, 7, 55},
    {"heli", "Helicopter", 35, 40, 40},
};

struct ShotKey {
    float t = 0;   // replay time
    V3 pos;        // in the shot's frame (right, up, forward)
    Quat rel;      // rotation relative to the shot frame's looking-forward rotation
    float fov = 60;
    V3 bend;       // path shaping: pushes the middle of the segment to the next key by this much (shot frame)
    // the segment to the next key: how the camera moves along it, and an optional corkscrew around it
    int seg = 0;              // kSegSmooth .. kSegEaseInOut
    float twist_turns = 0;    // corkscrew: turns around the line to the next key (+ / - = direction)
    float twist_radius = 0;   // corkscrew: metres
    V3 orig;                  // where it was before it was first dragged in 3D (has_orig): "Reset position"
    bool has_orig = false;
    bool selected = false;  // editor only
};
enum SegMode { kSegSmooth = 0, kSegHold, kSegEaseIn, kSegEaseOut, kSegEaseInOut, kSegCount };
inline const char *seg_name(int m)
{
    static const char *const n[] = {"Smooth", "Hold", "Ease in", "Ease out", "Ease in-out"};
    return m >= 0 && m < kSegCount ? n[m] : "Smooth";
}
// A segment's timing: progress s (0..1) through it -> progress along its path.
inline float seg_warp(int mode, float s)
{
    switch (mode) {
    case kSegHold: return 0;
    case kSegEaseIn: return s * s;
    case kSegEaseOut: return 1 - (1 - s) * (1 - s);
    case kSegEaseInOut: return s * s * (3 - 2 * s);
    default: return s;
    }
}

// The bend's weight along a segment (s = 0..1): 0 at both keys, 1 in the middle, and its slope and curvature are 0
// at the keys, so a bend never jerks the camera at a key.
inline float bend_weight(float s)
{
    if (s <= 0 || s >= 1)
        return 0;
    float a = s * (1 - s);
    return 64 * a * a * a;
}

// A keyframed slider value: (time, value) keys, played on a natural cubic spline (held before the first and
// after the last key).
// A speed curve over a move, as in video editors' graph editors: anchors from (0, 0) to (1, 1), joined by cubic
// Bezier segments, each anchor with an in and an out handle (absolute positions). x is time through the move; y is
// how far along it (0 = the start value, 1 = the end value), and may go below 0 or above 1 (overshoot, a dip).
// The first and last anchors are fixed at (0, 0) and (1, 1); anchors between them can go anywhere in between.
struct CurvePt {
    float x = 0, y = 0;
    float ix = 0, iy = 0;  // in handle (towards the anchor before)
    float ox = 0, oy = 0;  // out handle (towards the anchor after)
};
struct Curve {
    std::vector<CurvePt> pts{{0, 0, 0, 0, 0.42f, 0}, {1, 1, 0.58f, 1, 1, 1}};  // smooth (ease in-out)

    Curve() = default;
    Curve(float x1, float y1, float x2, float y2)  // the two-anchor curve with these handles
        : pts{{0, 0, 0, 0, x1, y1}, {1, 1, x2, y2, 1, 1}}
    {
    }
    // keeps the anchors in time order, the ends at (0, 0) / (1, 1), and each handle's time within its segment
    // (so time only moves forward)
    void tidy()
    {
        if (pts.size() < 2)
            *this = Curve{};
        std::stable_sort(pts.begin() + 1, pts.end() - 1, [](const CurvePt &a, const CurvePt &b) { return a.x < b.x; });
        pts.front().x = 0, pts.front().y = 0;
        pts.back().x = 1, pts.back().y = 1;
        for (size_t i = 1; i + 1 < pts.size(); i++)
            pts[i].x = std::clamp(pts[i].x, pts[i - 1].x + 1e-3f, 1 - 1e-3f);
        for (size_t i = 0; i + 1 < pts.size(); i++) {
            pts[i].ox = std::clamp(pts[i].ox, pts[i].x, pts[i + 1].x);
            pts[i + 1].ix = std::clamp(pts[i + 1].ix, pts[i].x, pts[i + 1].x);
        }
    }
    float at(float x) const
    {
        x = std::clamp(x, 0.0f, 1.0f);
        if (pts.size() < 2)
            return x;
        size_t i = 0;
        while (i + 2 < pts.size() && x > pts[i + 1].x)
            i++;
        const CurvePt &a = pts[i], &b = pts[i + 1];
        float ax = std::clamp(a.ox, a.x, b.x), bx = std::clamp(b.ix, a.x, b.x);
        auto bez = [](float p0, float p1, float p2, float p3, float s) {
            float m = 1 - s;
            return m * m * m * p0 + 3 * m * m * s * p1 + 3 * m * s * s * p2 + s * s * s * p3;
        };
        float lo = 0, hi = 1;  // x(s) is monotonic with the handles inside the segment: bisect for s
        for (int k = 0; k < 30; k++) {
            float mid = (lo + hi) / 2;
            (bez(a.x, ax, bx, b.x, mid) < x ? lo : hi) = mid;
        }
        return bez(a.y, a.oy, b.iy, b.y, (lo + hi) / 2);
    }
    // A new anchor on the curve at time x: the segment there is split exactly (de Casteljau), so the curve keeps
    // its shape; the new anchor's handles and the neighbours' facing handles come from the split.
    void add_anchor(float x)
    {
        x = std::clamp(x, 0.01f, 0.99f);
        size_t i = 0;
        while (i + 2 < pts.size() && x > pts[i + 1].x)
            i++;
        CurvePt &a = pts[i], &b = pts[i + 1];
        float p0x = a.x, p1x = std::clamp(a.ox, a.x, b.x), p2x = std::clamp(b.ix, a.x, b.x), p3x = b.x;
        float p0y = a.y, p1y = a.oy, p2y = b.iy, p3y = b.y;
        auto bez = [](float q0, float q1, float q2, float q3, float s) {
            float m = 1 - s;
            return m * m * m * q0 + 3 * m * m * s * q1 + 3 * m * s * s * q2 + s * s * s * q3;
        };
        float lo = 0, hi = 1;
        for (int k = 0; k < 30; k++) {
            float mid = (lo + hi) / 2;
            (bez(p0x, p1x, p2x, p3x, mid) < x ? lo : hi) = mid;
        }
        float s = (lo + hi) / 2;
        auto lerp = [s](float u, float v) { return u + (v - u) * s; };
        float ax1 = lerp(p0x, p1x), ay1 = lerp(p0y, p1y), bx1 = lerp(p1x, p2x), by1 = lerp(p1y, p2y);
        float cx1 = lerp(p2x, p3x), cy1 = lerp(p2y, p3y);
        float ax2 = lerp(ax1, bx1), ay2 = lerp(ay1, by1), bx2 = lerp(bx1, cx1), by2 = lerp(by1, cy1);
        float mx = lerp(ax2, bx2), my = lerp(ay2, by2);
        a.ox = ax1, a.oy = ay1;
        b.ix = cx1, b.iy = cy1;
        pts.insert(pts.begin() + long(i + 1), CurvePt{mx, my, ax2, ay2, bx2, by2});
        tidy();
    }
};
inline Curve curve_preset(int i)  // 0 smooth, 1 linear, 2 ease in, 3 ease out
{
    switch (i) {
    case 1: return {1.0f / 3, 1.0f / 3, 2.0f / 3, 2.0f / 3};
    case 2: return {0.42f, 0, 1, 1};
    case 3: return {0, 0, 0.58f, 1};
    }
    return {};
}

// JSON: [[x, y, ix, iy, ox, oy], ...]; the older [x1, y1, x2, y2] (two anchors) still reads.
inline std::string curve_to_json(const Curve &c)
{
    std::string o = "[";
    char b[128];
    for (size_t i = 0; i < c.pts.size(); i++) {
        const CurvePt &p = c.pts[i];
        snprintf(b, sizeof b, "%s[%.4f, %.4f, %.4f, %.4f, %.4f, %.4f]", i ? ", " : "", p.x, p.y, p.ix, p.iy, p.ox, p.oy);
        o += b;
    }
    return o + "]";
}
template <class J> bool curve_from_json(const J &j, Curve &c)
{
    if (!j.is_array())
        return false;
    if (j.size() == 4 && !j[0].is_array()) {
        c = Curve(float(j[0].as_double(0.42)), float(j[1].as_double(0)), float(j[2].as_double(0.58)),
                  float(j[3].as_double(1)));
        return true;
    }
    if (j.size() < 2)
        return false;
    Curve out;
    out.pts.clear();
    for (size_t i = 0; i < j.size(); i++) {
        const auto &p = j[i];
        if (!p.is_array() || p.size() != 6)
            return false;
        out.pts.push_back({float(p[0].as_double()), float(p[1].as_double()), float(p[2].as_double()),
                           float(p[3].as_double()), float(p[4].as_double()), float(p[5].as_double())});
    }
    out.tidy();
    c = out;
    return true;
}

// A keyframed slider value. `curved`: the segment from this key to the next follows `curve` (smooth by default:
// it eases out of this key and into the next); otherwise (keys from before 1.0.0) the values follow a spline
// through all the keys.
struct PKey {
    float t = 0, v = 0;
    bool curved = false;
    Curve curve;
    PKey() = default;
    PKey(float t_, float v_, bool curved_ = false, Curve c = {}) : t(t_), v(v_), curved(curved_), curve(c) {}
};
struct Track {
    std::string param;  // one of kParams
    std::vector<PKey> keys;
};

// A transition's curves: how fast the camera covers each axis of the move in the destination car's frame (level:
// forward along the car, side across it, height), and the turn of the view with the lens.
enum TransitionAxis { kAxForward, kAxSide, kAxHeight, kAxLook, kAxCount };

struct Shot {
    ShotType type = ShotType::free_path;
    Curve curves[kAxCount];  // transition only
    float t0 = 0, t1 = 5;
    int car = -1;          // car slot (-1: the car the replay follows)
    std::string driver;    // the driver's name when it was picked (display; the slot is what plays)
    // onboard: a mount plus a nudge; chase: distance / height behind; both: look offsets and FOV
    std::string mount = "cockpit";
    V3 nudge;
    float distance = 6, height = 1.4f;
    float yaw = 0, pitch = 0, roll = 0, fov = 70;
    V3 point;              // track cam: where it stands (world)
    float radius = 8, period = 10, start_angle = 0;  // orbit (height shared with chase)
    float around = 0;      // chase: swings the camera around the car's centre (degrees, about the vertical)
    bool reverse = false;  // orbit: turn the other way
    float speed = 1;       // replay speed during the shot
    bool constant_speed = false, ease_in = false, ease_out = false;
    std::vector<ShotKey> keys;  // free path: the path; onboard / chase: optional animation in the car frame
    std::vector<Track> tracks;  // keyframed slider values (override the plain values while they have keys)
    bool selected = false;      // editor only

    float dur() const { return t1 - t0; }
    bool uses_car() const { return type != ShotType::free_path && type != ShotType::transition; }
    bool keyed() const { return type == ShotType::free_path || type == ShotType::onboard || type == ShotType::chase; }
    void sort_keys()
    {
        std::stable_sort(keys.begin(), keys.end(), [](const ShotKey &a, const ShotKey &b) { return a.t < b.t; });
    }
};

// The slider values a track can animate.
inline constexpr const char *kParams[] = {"nudge_x", "nudge_y", "nudge_z", "yaw",    "pitch",  "roll",
                                          "fov",     "distance", "height", "around", "radius", "speed"};
inline float *param_ptr(Shot &s, std::string_view p)
{
    if (p == "nudge_x") return &s.nudge.x;
    if (p == "nudge_y") return &s.nudge.y;
    if (p == "nudge_z") return &s.nudge.z;
    if (p == "yaw") return &s.yaw;
    if (p == "pitch") return &s.pitch;
    if (p == "roll") return &s.roll;
    if (p == "fov") return &s.fov;
    if (p == "distance") return &s.distance;
    if (p == "height") return &s.height;
    if (p == "around") return &s.around;
    if (p == "radius") return &s.radius;
    if (p == "speed") return &s.speed;
    return nullptr;
}
inline Track *find_track(Shot &s, std::string_view p)
{
    for (auto &tr : s.tracks)
        if (tr.param == p)
            return &tr;
    return nullptr;
}
inline void sort_track(Track &tr)
{
    std::stable_sort(tr.keys.begin(), tr.keys.end(), [](const PKey &a, const PKey &b) { return a.t < b.t; });
}

// The replay an edit was made on. `file`: the replay file's header MAC (bytes 160..191 of the .frr, hex), unique
// per saved replay; empty for a replay that isn't a file (a session's own replay). `length` and `drivers` (by car
// slot, comma separated) identify it otherwise. Nothing known = an edit from before 1.0.0, or a new one before the
// replay was identified.
struct ReplayId {
    std::string file;
    float length = 0;
    std::string drivers;
    bool known() const { return !file.empty() || (length > 0 && !drivers.empty()); }
    bool same(const ReplayId &o) const
    {
        if (!file.empty() && !o.file.empty())
            return file == o.file;
        return length > 0 && !drivers.empty() && std::fabs(length - o.length) < 0.05f && drivers == o.drivers;
    }
};

struct Edit {
    std::string name;
    ReplayId replay;          // which replay it belongs to
    std::vector<Shot> shots;  // sorted by t0, not overlapping (the editor keeps it so)
    void sort()
    {
        std::stable_sort(shots.begin(), shots.end(), [](const Shot &a, const Shot &b) { return a.t0 < b.t0; });
    }
    int shot_at(float t) const
    {
        for (int i = int(shots.size()) - 1; i >= 0; i--)
            if (t >= shots[size_t(i)].t0 && t < shots[size_t(i)].t1)
                return i;
        return -1;
    }
    float start() const { return shots.empty() ? 0 : shots.front().t0; }
    float end() const
    {
        float e = 0;
        for (auto &s : shots)
            e = std::max(e, s.t1);
        return e;
    }
};

// ----- splines -------------------------------------------------------------------------------------

// A natural cubic spline through (x_i, y_i): continuous value, slope and curvature. Two points: a line.
struct Spline {
    std::vector<float> x, y, m;  // m: second derivatives
    void fit(const std::vector<float> &xs, const std::vector<float> &ys)
    {
        x = xs;
        y = ys;
        size_t n = x.size();
        m.assign(n, 0);
        if (n < 3)
            return;
        std::vector<float> a(n), b(n), c(n), d(n);
        for (size_t i = 1; i + 1 < n; i++) {
            float h0 = x[i] - x[i - 1], h1 = x[i + 1] - x[i];
            a[i] = h0;
            b[i] = 2 * (h0 + h1);
            c[i] = h1;
            d[i] = 6 * ((y[i + 1] - y[i]) / h1 - (y[i] - y[i - 1]) / h0);
        }
        // Thomas algorithm over rows 1..n-2 (m[0] = m[n-1] = 0)
        for (size_t i = 2; i + 1 < n; i++) {
            float w = a[i] / b[i - 1];
            b[i] -= w * c[i - 1];
            d[i] -= w * d[i - 1];
        }
        m[n - 2] = d[n - 2] / b[n - 2];
        for (size_t i = n - 2; i-- > 1;)
            m[i] = (d[i] - c[i] * m[i + 1]) / b[i];
    }
    float at(float t) const
    {
        size_t n = x.size();
        if (n == 0)
            return 0;
        if (n == 1 || t <= x[0])
            return y[0];
        if (t >= x[n - 1])
            return y[n - 1];
        size_t i = 0;
        while (i + 2 < n && t > x[i + 1])
            i++;
        float h = x[i + 1] - x[i];
        if (h <= 1e-6f)
            return y[i + 1];
        float A = (x[i + 1] - t) / h, B = (t - x[i]) / h;
        return A * y[i] + B * y[i + 1] + ((A * A * A - A) * m[i] + (B * B * B - B) * m[i + 1]) * h * h / 6;
    }
};

// A shot's keys as continuous curves.
struct KeyCurves {
    Spline px, py, pz, qx, qy, qz, qw, fov;
    float k0 = 0, k1 = 0;              // first and last key time
    std::vector<float> arc_t, arc_s;   // constant speed: time -> distance along the path
    std::vector<float> knot;           // the key times used (duplicates dropped)
    std::vector<V3> bends;             // per knot: the bend of the segment that leaves it
    std::vector<int> key_index;        // per knot: its index in the shot's keys
    std::vector<int> segs;             // per knot: the timing of the segment that leaves it (SegMode)
    std::vector<float> twist_n, twist_r;  // per knot: corkscrew turns / radius of the segment that leaves it
    std::vector<V3> knot_pos;          // per knot: the key's position (the corkscrew's axis runs between them)
    bool any_bend = false, any_seg = false, any_twist = false;
    void build(const std::vector<ShotKey> &keys, bool constant_speed)
    {
        std::vector<float> ts, x, y, z, a, b, c, d, f;
        Quat prev{};
        bends.clear();
        key_index.clear();
        segs.clear();
        twist_n.clear();
        twist_r.clear();
        knot_pos.clear();
        any_bend = any_seg = any_twist = false;
        for (size_t i = 0; i < keys.size(); i++) {
            const ShotKey &k = keys[i];
            if (!ts.empty() && k.t <= ts.back() + 1e-4f)
                continue;  // two keys at the same time: keep the first
            Quat q = k.rel;
            if (!ts.empty() && q.x * prev.x + q.y * prev.y + q.z * prev.z + q.w * prev.w < 0)
                q = {-q.x, -q.y, -q.z, -q.w};  // stay in one hemisphere so the curve takes the short way
            prev = q;
            bends.push_back(k.bend);
            key_index.push_back(int(i));
            any_bend = any_bend || len(k.bend) > 1e-5f;
            segs.push_back(k.seg);
            twist_n.push_back(k.twist_turns);
            twist_r.push_back(k.twist_radius);
            knot_pos.push_back(k.pos);
            any_seg = any_seg || k.seg != kSegSmooth;
            any_twist = any_twist || (std::fabs(k.twist_turns) > 1e-4f && k.twist_radius > 1e-4f);
            ts.push_back(k.t);
            x.push_back(k.pos.x);
            y.push_back(k.pos.y);
            z.push_back(k.pos.z);
            a.push_back(q.x);
            b.push_back(q.y);
            c.push_back(q.z);
            d.push_back(q.w);
            f.push_back(k.fov);
        }
        px.fit(ts, x);
        py.fit(ts, y);
        pz.fit(ts, z);
        qx.fit(ts, a);
        qy.fit(ts, b);
        qz.fit(ts, c);
        qw.fit(ts, d);
        fov.fit(ts, f);
        k0 = ts.empty() ? 0 : ts.front();
        k1 = ts.empty() ? 0 : ts.back();
        knot = ts;
        arc_t.clear();
        arc_s.clear();
        if (constant_speed && ts.size() >= 2) {
            const int N = 256;
            float s = 0;
            V3 last = pos(k0);
            for (int i = 0; i <= N; i++) {
                float t = k0 + (k1 - k0) * i / N;
                V3 p = pos(t);
                s += len(p - last);
                last = p;
                arc_t.push_back(t);
                arc_s.push_back(s);
            }
        }
    }
    // the segment curve time t is in, and the progress through it (false outside the keys)
    bool segment_of(float t, size_t &i, float &s) const
    {
        if (knot.size() < 2 || t <= knot.front() || t >= knot.back())
            return false;
        i = 0;
        while (i + 2 < knot.size() && t >= knot[i + 1])
            i++;
        s = (t - knot[i]) / (knot[i + 1] - knot[i]);
        return true;
    }
    V3 pos(float t) const
    {
        V3 p{px.at(t), py.at(t), pz.at(t)};
        size_t i;
        float s;
        if ((any_bend || any_twist) && segment_of(t, i, s)) {
            if (any_bend)
                p = p + bends[i] * bend_weight(s);
            if (any_twist && std::fabs(twist_n[i]) > 1e-4f && twist_r[i] > 1e-4f) {
                // a corkscrew around the line to the next key; it grows in over the first fifth of the segment and
                // out over the last, with zero slope at both keys
                V3 dir = knot_pos[i + 1] - knot_pos[i];
                float L = len(dir);
                dir = L > 1e-4f ? dir * (1 / L) : V3{0, 0, 1};
                V3 ax = cross(dir, V3{0, 1, 0});
                if (len(ax) < 1e-3f)
                    ax = cross(dir, V3{1, 0, 0});
                ax = norm(ax);
                V3 ay = cross(ax, dir);
                auto e = [](float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3 - 2 * x); };
                float w = e(s / 0.2f) * e((1 - s) / 0.2f), th = 2 * 3.14159265f * twist_n[i] * s;
                p = p + (ay * std::cos(th) + ax * std::sin(th) - ay) * (twist_r[i] * w);  // starts on the line
            }
        }
        return p;
    }
    // the curve time after the segment timings (hold, eases); identity when every segment is smooth
    float warp(float t) const
    {
        size_t i;
        float s;
        if (!any_seg || !segment_of(t, i, s))
            return t;
        return knot[i] + (knot[i + 1] - knot[i]) * seg_warp(segs[i], s);
    }
    // the curve time of the middle of segment i (between knot i and i + 1)
    float segment_mid(size_t i) const { return (knot[i] + knot[i + 1]) / 2; }
    Quat rot(float t) const { return qnorm(Quat{qx.at(t), qy.at(t), qz.at(t), qw.at(t)}); }
    // the curve time to sample at shot-local progress u (0..1 between the first and last key)
    float curve_time(float u) const
    {
        if (arc_s.size() < 2 || arc_s.back() < 1e-4f)
            return k0 + (k1 - k0) * u;
        float target = arc_s.back() * u;
        size_t i = 1;
        while (i + 1 < arc_s.size() && arc_s[i] < target)
            i++;
        float s0 = arc_s[i - 1], s1 = arc_s[i];
        float w = s1 > s0 ? (target - s0) / (s1 - s0) : 0;
        return arc_t[i - 1] + (arc_t[i] - arc_t[i - 1]) * w;
    }
};

inline float ease(float u, bool in, bool out)
{
    u = std::clamp(u, 0.0f, 1.0f);
    if (in && out)
        return u * u * (3 - 2 * u);
    if (in)
        return u * u;
    if (out)
        return 1 - (1 - u) * (1 - u);
    return u;
}

inline float track_at(const Track &tr, float t)
{
    const auto &ks = tr.keys;
    for (size_t i = 0; i + 1 < ks.size(); i++)  // a curved segment: its own curve from this key's value to the next's
        if (ks[i].curved && t >= ks[i].t && t <= ks[i + 1].t && ks[i + 1].t - ks[i].t > 1e-4f)
            return ks[i].v + (ks[i + 1].v - ks[i].v) * ks[i].curve.at((t - ks[i].t) / (ks[i + 1].t - ks[i].t));
    std::vector<float> xs, ys;
    for (const auto &k : ks) {
        if (!xs.empty() && k.t - xs.back() < 1e-4f)
            continue;  // equal times would divide by zero
        xs.push_back(k.t);
        ys.push_back(k.v);
    }
    Spline sp;
    sp.fit(xs, ys);
    return sp.at(t);
}

// The shot with its keyframed values at time t.
inline void apply_tracks(Shot &s, float t)
{
    for (const auto &tr : s.tracks)
        if (!tr.keys.empty())
            if (float *v = param_ptr(s, tr.param))
                *v = track_at(tr, t);
}

// ----- evaluation -----------------------------------------------------------------------------------

// A level frame on a car: its position and heading, world up (chase / orbit: no shaking horizon).
inline Frame level_frame(const Frame &car)
{
    Frame f;
    f.p = car.p;
    V3 h = car.f - V3{0, 1, 0} * car.f.y;
    f.f = len(h) > 1e-3f ? norm(h) : V3{0, 0, 1};
    f.u = {0, 1, 0};
    f.r = norm(cross(f.f, f.u));
    return f;
}

// The frame a shot's keys live in, at the current moment.
inline std::optional<Frame> shot_frame(const Shot &s, const Cars &cars)
{
    switch (s.type) {
    case ShotType::free_path:
    case ShotType::track:
    case ShotType::transition:
        return Frame{};
    case ShotType::onboard:
        return cars.frame(s.car);
    case ShotType::chase:
    case ShotType::orbit: {
        auto c = cars.frame(s.car);
        if (!c)
            return std::nullopt;
        return level_frame(*c);
    }
    }
    return std::nullopt;
}

inline Quat look_at_quat(V3 eye, V3 target, float roll)
{
    Camera c{};
    c.eye = eye;
    c.target = target;
    c.up_hint = {0, 0, 1};
    c.roll = roll;
    orient(c);
    return c.q;
}

// The adjustments the inspector's sliders make on top of a shot's base view (its keys, or a chase preset):
// nudge (car / level frame), then a swing of `around` degrees about the vertical through the car's centre
// (chase), then roll about the view axis (keyed shots; unkeyed ones roll in their look-at). inverse undoes them,
// so a key made from a view plays that view back.
// Look offsets in the camera's own axes: yaw (positive: right), then pitch (positive: up).
inline Quat look_offset(const Shot &s) { return qmul(qaxis({0, 1, 0}, -s.yaw), qaxis({1, 0, 0}, s.pitch)); }
inline Quat roll_offset(float roll) { return qaxis({0, 0, 1}, -roll); }  // about the view axis, like look_at_quat

inline void adjust_view(const Shot &s, const Frame &fr, V3 centre, V3 &eye, Quat &q, bool roll, bool inverse)
{
    Quat sw = qaxis(fr.u, s.type == ShotType::chase ? s.around : 0);
    Quat rl = roll ? qmul(look_offset(s), roll_offset(s.roll)) : Quat{};  // keyed: look offsets and roll
    if (!inverse) {
        eye = eye + fr.dir(s.nudge);
        eye = centre + qrot(sw, eye - centre);
        q = qnorm(qmul(qmul(sw, q), rl));
    } else {
        q = qnorm(qmul(qmul(qconj(sw), q), qconj(rl)));
        eye = centre + qrot(qconj(sw), eye - centre);
        eye = eye - fr.dir(s.nudge);
    }
}

struct ShotView {
    V3 eye;
    Quat q;
    float fov;
    float speed;
    int shot;
};

// The view of shot `s` at replay time t (t inside the shot). `curves`, when given, is s.keys already built with
// s.constant_speed (sampling many times of one shot builds them once).
inline std::optional<ShotView> shot_view_at(const Shot &s, int index, float t, const Cars &cars,
                                            const KeyCurves *curves = nullptr);

inline std::optional<ShotView> shot_view(const Shot &s, int index, float t, const Cars &cars,
                                         const KeyCurves *curves = nullptr)
{
    if (s.tracks.empty())
        return shot_view_at(s, index, t, cars, curves);
    Shot cur = s;
    apply_tracks(cur, t);
    return shot_view_at(cur, index, t, cars, curves);
}

// shot_view for a shot whose keyframed values are already applied.
inline std::optional<ShotView> shot_view_at(const Shot &s, int index, float t, const Cars &cars, const KeyCurves *curves)
{
    auto fr = shot_frame(s, cars);
    if (!fr)
        return std::nullopt;
    ShotView v{};
    v.shot = index;
    v.speed = s.speed;
    v.fov = s.fov;
    std::optional<Frame> car;
    if (s.uses_car())
        car = cars.frame(s.car);
    V3 car_pt = car ? car->at(kCarCentre) : V3{};

    if (s.keyed() && !s.keys.empty()) {  // keyed: the curves through the keys, in the shot's frame
        KeyCurves built;
        if (!curves)
            built.build(s.keys, s.constant_speed);
        const KeyCurves &kc = curves ? *curves : built;
        float span = std::max(1e-4f, kc.k1 - kc.k0);
        float u = (t - kc.k0) / span;
        float ct = kc.warp(kc.curve_time(ease(u, s.ease_in, s.ease_out)));
        if (kc.k1 <= kc.k0)
            ct = kc.k0;
        v.eye = fr->at(kc.pos(ct));
        v.q = qnorm(qmul(frame_quat(*fr), kc.rot(ct)));
        v.fov = kc.fov.at(ct);
        if (s.type != ShotType::free_path)
            adjust_view(s, *fr, car_pt, v.eye, v.q, true, false);
        return v;
    }
    switch (s.type) {
    case ShotType::free_path:
        return std::nullopt;  // a free path needs keys
    case ShotType::transition:
        return std::nullopt;  // needs its neighbours: transition_view
    case ShotType::onboard: {
        const Mount *m = find_mount(s.mount);
        V3 off = (m ? m->offset : V3{}) + s.nudge;
        v.eye = fr->at(off);
        v.q = look_quat(*fr, (m ? m->yaw : 0) + s.yaw, (m ? m->pitch : 0) + s.pitch, s.roll);
        return v;
    }
    case ShotType::chase: {
        v.eye = fr->at({0, s.height, kCarCentre.z - s.distance});
        Quat unused{};
        adjust_view(s, *fr, car_pt, v.eye, unused, false, false);
        v.q = qnorm(qmul(qmul(look_at_quat(v.eye, car_pt, 0), look_offset(s)), roll_offset(s.roll)));
        return v;
    }
    case ShotType::track:
        v.eye = s.point;
        v.q = look_at_quat(v.eye, car_pt, s.roll);
        return v;
    case ShotType::orbit: {
        float a = (s.start_angle + (s.reverse ? -360.0f : 360.0f) * (t - s.t0) / std::max(0.5f, s.period)) * kDeg;
        V3 off{std::sin(a) * s.radius, s.height, std::cos(a) * s.radius};
        v.eye = fr->at(off) + fr->dir({0, 0, kCarCentre.z});
        v.q = look_at_quat(v.eye, car_pt, s.roll);
        return v;
    }
    }
    return std::nullopt;
}

// A shot's camera path for drawing: points along it, one tick per whole replay second, and each key's pose, all in
// the world with the cars where they are now (shots on a car are a snapshot around it). The curves are built once.
struct PathKey {
    V3 eye;
    Quat q;
    float fov;
    bool selected;
    float t;          // the key's time (identifies it when the keys change under an edit)
    V3 bx, by, bz;    // the shot frame's axes in the world at the key: a world drag d moves key.pos by
                      // (dot(d, bx), dot(d, by), dot(d, bz))
};
// A path-shaping handle at the middle of the segment leaving key `key`. bx, by, bz are the world directions of the
// shot frame's x, y, z there (with the chase swing), so a world drag d becomes the bend change
// (dot(d, bx), dot(d, by), dot(d, bz)). rest = where the middle is without the bend.
struct PathHandle {
    V3 at, rest, bx, by, bz, bend;
    int key;
    float key_t;  // the time of the key the segment leaves
};
struct PathSample {
    std::vector<V3> pts, ticks;
    std::vector<float> pt_t;  // the replay time of each point
    // per point, filled by the plugin from the game's collision: 0 clear, 1 within 30 cm of the ground,
    // 2 inside the track / a barrier or passing through one (on the way from the previous point)
    std::vector<uint8_t> clip;
    std::vector<PathKey> keys;
    std::vector<PathHandle> handles;
};

// The shot frame's axes in the world at time t, with the chase swing (how a key's position moves its eye).
inline bool frame_axes(const Shot &s, float t, const Cars &cars, V3 &bx, V3 &by, V3 &bz)
{
    Shot cur = s;
    apply_tracks(cur, t);
    auto fr = shot_frame(cur, cars);
    if (!fr)
        return false;
    Quat sw = qaxis(fr->u, cur.type == ShotType::chase ? cur.around : 0);
    bx = qrot(sw, fr->r);
    by = qrot(sw, fr->u);
    bz = qrot(sw, fr->f);
    return true;
}

// A key for shot s at time t on its current path (position, rotation and lens as played; in the shot frame).
// Inserting it leaves the path where it was there (the spline then passes through it).
inline ShotKey key_at(const Shot &s, float t);

// The replay time at which shot s plays curve time ct (inverts ease and constant speed; u is monotonic).
inline float time_of_curve(const Shot &s, const KeyCurves &kc, float ct)
{
    float span = std::max(1e-4f, kc.k1 - kc.k0), lo = 0, hi = 1;
    for (int i = 0; i < 40; i++) {
        float mid = (lo + hi) / 2;
        (kc.curve_time(ease(mid, s.ease_in, s.ease_out)) < ct ? lo : hi) = mid;
    }
    return kc.k0 + span * (lo + hi) / 2;
}

inline PathSample sample_path(const Shot &s, int index, const Cars &cars, int max_points = 300)
{
    PathSample out;
    KeyCurves kc;
    const KeyCurves *pre = nullptr;
    if (s.keyed() && !s.keys.empty()) {
        kc.build(s.keys, s.constant_speed);
        pre = &kc;
    }
    float dur = std::max(0.05f, s.t1 - s.t0);
    int n = std::clamp(int(dur * 30), 16, std::max(16, max_points));
    for (int i = 0; i <= n; i++) {
        float t = s.t0 + dur * float(i) / float(n);
        cars.at_time(t);
        if (auto v = shot_view(s, index, t, cars, pre)) {
            out.pts.push_back(v->eye);
            out.pt_t.push_back(t);
        }
    }
    for (float t = std::ceil(s.t0); t < s.t1; t += 1) {
        cars.at_time(t);
        if (auto v = shot_view(s, index, t, cars, pre))
            out.ticks.push_back(v->eye);
    }
    if (s.keyed())
        for (const auto &k : s.keys) {
            PathKey pk{};
            cars.at_time(k.t);
            auto v = shot_view(s, index, k.t, cars, pre);
            if (!v || !frame_axes(s, k.t, cars, pk.bx, pk.by, pk.bz))
                continue;
            pk.eye = v->eye;
            pk.q = v->q;
            pk.fov = v->fov;
            pk.selected = k.selected;
            pk.t = k.t;
            out.keys.push_back(pk);
        }
    if (pre)
        for (size_t i = 0; i + 1 < kc.knot.size(); i++) {
            float t = time_of_curve(s, kc, kc.segment_mid(i));
            cars.at_time(t);
            auto v = shot_view(s, index, t, cars, pre);
            PathHandle h;
            if (!v || !frame_axes(s, t, cars, h.bx, h.by, h.bz))
                continue;
            h.at = v->eye;
            h.bend = kc.bends[i];
            h.rest = h.at - (h.bx * h.bend.x + h.by * h.bend.y + h.bz * h.bend.z);
            h.key = kc.key_index[i];
            h.key_t = kc.knot[i];
            out.handles.push_back(h);
        }
    cars.at_time(-1);
    return out;
}

// A transition (shot i) glides from the shot before it to the shot after it. At each moment both neighbours are
// evaluated at that same time, so each keeps following its car (or path) while the view blends from one to the
// other: at the start it is the shot before at its end, at the end the shot after at its start. The move is done
// in the destination's car frame (level: forward, side, up; the source's car, else the world, when it has none),
// each axis with its own speed curve (s.curves), so e.g. the camera can come in level first and drop late. The
// view turns (slerp) and the lens changes on the Look curve. With only one neighbour it holds that one.
inline std::optional<ShotView> transition_view(const Edit &e, int i, float t, const Cars &cars)
{
    const Shot &s = e.shots[size_t(i)];
    int n = int(e.shots.size()), a = i - 1, b = i + 1;
    while (a >= 0 && e.shots[size_t(a)].type == ShotType::transition)
        a--;
    while (b < n && e.shots[size_t(b)].type == ShotType::transition)
        b++;
    std::optional<ShotView> va, vb;
    if (a >= 0)
        va = shot_view(e.shots[size_t(a)], a, t, cars);
    if (b < n)
        vb = shot_view(e.shots[size_t(b)], b, t, cars);
    if (!va && !vb)
        return std::nullopt;
    float u = (t - s.t0) / std::max(0.05f, s.t1 - s.t0);
    ShotView v = va ? *va : *vb;
    if (va && vb) {
        Frame fr;  // the move's frame: the destination's car (level), else the source's, else the world
        for (int idx : {b, a}) {
            const Shot &o = e.shots[size_t(idx)];
            if (o.uses_car())
                if (auto f = cars.frame(o.car)) {
                    fr = level_frame(*f);
                    break;
                }
        }
        V3 pa = fr.local(va->eye), pb = fr.local(vb->eye), d = pb - pa;
        V3 p{pa.x + d.x * s.curves[kAxSide].at(u), pa.y + d.y * s.curves[kAxHeight].at(u),
             pa.z + d.z * s.curves[kAxForward].at(u)};
        v.eye = fr.at(p);
        float wl = s.curves[kAxLook].at(u);
        v.q = slerp(va->q, vb->q, std::clamp(wl, 0.0f, 1.0f));
        v.fov = va->fov + (vb->fov - va->fov) * wl;
    }
    v.shot = i;
    v.speed = s.speed;
    return v;
}

// The shot on screen at t: its view (a transition blends its neighbours).
inline std::optional<ShotView> edit_shot_view(const Edit &e, int i, float t, const Cars &cars)
{
    const Shot &s = e.shots[size_t(i)];
    return s.type == ShotType::transition ? transition_view(e, i, t, cars) : shot_view(s, i, t, cars);
}

inline std::optional<ShotView> evaluate_edit(const Edit &e, float t, const Cars &cars)
{
    int i = e.shot_at(t);
    if (i < 0)
        return std::nullopt;
    return edit_shot_view(e, i, t, cars);
}

// A transition's camera path for drawing (points and whole-second ticks; no keys or handles).
inline PathSample sample_transition(const Edit &e, int i, const Cars &cars)
{
    PathSample out;
    const Shot &s = e.shots[size_t(i)];
    float dur = std::max(0.05f, s.t1 - s.t0);
    int n = std::clamp(int(dur * 30), 16, 300);
    for (int k = 0; k <= n; k++) {
        float t = s.t0 + dur * float(k) / float(n);
        cars.at_time(t);
        if (auto v = transition_view(e, i, t, cars)) {
            out.pts.push_back(v->eye);
            out.pt_t.push_back(t);
        }
    }
    for (float t = std::ceil(s.t0); t < s.t1; t += 1) {
        cars.at_time(t);
        if (auto v = transition_view(e, i, t, cars))
            out.ticks.push_back(v->eye);
    }
    cars.at_time(-1);
    return out;
}

// A key for shot s from a world view (eye, rotation, fov), in the shot's frame at this moment.
inline std::optional<ShotKey> key_from_view(const Shot &s_in, float t, V3 eye, Quat q, float fov, const Cars &cars)
{
    Shot s = s_in;
    apply_tracks(s, t);  // undo the adjustments as they are at t
    auto fr = shot_frame(s, cars);
    if (!fr)
        return std::nullopt;
    if (s.type != ShotType::free_path && s.type != ShotType::track) {
        auto car = cars.frame(s.car);
        if (!car)
            return std::nullopt;
        adjust_view(s, *fr, car->at(kCarCentre), eye, q, true, true);
    }
    ShotKey k;
    k.t = t;
    k.pos = fr->local(eye);
    k.rel = qnorm(qmul(qconj(frame_quat(*fr)), q));
    k.fov = fov;
    return k;
}

// The key a keyed shot's curves pass through at time t (in the shot's frame), as shot_view plays it.
inline ShotKey key_at(const Shot &s, float t)
{
    KeyCurves kc;
    kc.build(s.keys, s.constant_speed);
    float span = std::max(1e-4f, kc.k1 - kc.k0);
    float ct = kc.k1 <= kc.k0 ? kc.k0 : kc.warp(kc.curve_time(ease((t - kc.k0) / span, s.ease_in, s.ease_out)));
    ShotKey k;
    k.t = t;
    k.pos = kc.pos(ct);
    k.rel = kc.rot(ct);
    k.fov = kc.fov.at(ct);
    return k;
}

// Inserts an anchor: a key at time t where the shot's path is (position, rotation and lens as played), for shaping
// the path by dragging it. The bend of the segment it lands in is dropped: the new key sits on the bent curve.
// Returns the new key's index, or -1 (not a keyed shot with keys, t outside them, or a key within 0.05 s).
inline int insert_key(Shot &s, float t)
{
    if (!s.keyed() || s.keys.size() < 2 || t <= s.keys.front().t || t >= s.keys.back().t)
        return -1;
    for (const auto &k : s.keys)
        if (std::fabs(k.t - t) < 0.05f)
            return -1;
    ShotKey k = key_at(s, t);
    k.bend = {};
    for (auto &o : s.keys)
        o.selected = false;
    for (int i = int(s.keys.size()) - 1; i >= 0; i--)
        if (s.keys[size_t(i)].t < t) {
            s.keys[size_t(i)].bend = {};
            break;
        }
    k.selected = true;
    s.keys.push_back(k);
    s.sort_keys();
    for (int i = 0; i < int(s.keys.size()); i++)
        if (s.keys[size_t(i)].selected)
            return i;
    return -1;
}

// Splits shot i at time t into two shots that meet there. Keys and value keys go to the side they're on, and
// both sides get a key at t where the curves were, so the camera is where it was at the cut. Ease in stays
// on the first part, ease out on the second. False when t is within 0.25 s of either end.
inline bool split_shot(Edit &e, int i, float t)
{
    if (i < 0 || i >= int(e.shots.size()))
        return false;
    const Shot s = e.shots[size_t(i)];
    if (t < s.t0 + 0.25f || t > s.t1 - 0.25f)
        return false;
    Shot a = s, b = s;
    a.t1 = t;
    b.t0 = t;
    a.ease_out = false;
    b.ease_in = false;
    a.keys.clear();
    b.keys.clear();
    if (!s.keys.empty()) {
        ShotKey mid = key_at(s, t);
        for (const auto &k : s.keys)
            if (k.t < t - 1e-3f)
                a.keys.push_back(k);
        a.keys.push_back(mid);
        b.keys.push_back(mid);
        for (const auto &k : s.keys)
            if (k.t > t + 1e-3f)
                b.keys.push_back(k);
    }
    a.tracks.clear();
    b.tracks.clear();
    for (const auto &tr : s.tracks) {
        if (tr.keys.empty())
            continue;
        PKey mid{t, track_at(tr, t), false, {}};
        for (const auto &k : tr.keys)  // the segment it splits: keeps its kind of curve on both sides
            if (k.t <= t)
                mid.curved = k.curved, mid.curve = k.curve;
        Track ta{tr.param, {}}, tb{tr.param, {}};
        for (const auto &k : tr.keys)
            if (k.t < t - 1e-3f)
                ta.keys.push_back(k);
        ta.keys.push_back(mid);
        tb.keys.push_back(mid);
        for (const auto &k : tr.keys)
            if (k.t > t + 1e-3f)
                tb.keys.push_back(k);
        a.tracks.push_back(ta);
        b.tracks.push_back(tb);
    }
    e.shots[size_t(i)] = a;
    e.shots.insert(e.shots.begin() + i + 1, b);
    return true;
}

// ----- JSON (version 2) ------------------------------------------------------------------------------

inline std::string edit_to_json(const Edit &e)
{
    auto f3 = [](V3 v) {
        char b[96];
        snprintf(b, sizeof b, "[%.4f, %.4f, %.4f]", v.x, v.y, v.z);
        return std::string(b);
    };
    std::string o = "{\n  \"version\": 2,\n  \"name\": \"";
    for (char ch : e.name)
        o += (ch == '"' || ch == '\\') ? '_' : ch;
    o += "\",\n";
    if (e.replay.known()) {
        char lb[48];
        snprintf(lb, sizeof lb, "%.3f", e.replay.length);
        o += "  \"replay\": { \"file\": \"" + e.replay.file + "\", \"length\": " + std::string(lb) + ", \"drivers\": \"";
        for (char ch : e.replay.drivers)
            o += (ch == '"' || ch == '\\') ? '_' : ch;
        o += "\" },\n";
    }
    o += "  \"shots\": [\n";
    for (size_t i = 0; i < e.shots.size(); i++) {
        const Shot &s = e.shots[i];
        char b[1024];
        snprintf(b, sizeof b,
                 "    { \"type\": \"%s\", \"t0\": %.3f, \"t1\": %.3f, \"car\": %d, \"driver\": \"%s\", \"mount\": \"%s\", "
                 "\"nudge\": %s, \"distance\": %.3f, \"height\": %.3f, \"yaw\": %.3f, \"pitch\": %.3f, \"roll\": %.3f, "
                 "\"fov\": %.2f, \"point\": %s, \"radius\": %.3f, \"period\": %.3f, \"start_angle\": %.2f, \"around\": %.2f, \"reverse\": %s, \"speed\": %.3f, "
                 "\"timing\": \"%s\", \"ease_in\": %s, \"ease_out\": %s,\n      \"keys\": [",
                 shot_type_id(s.type), s.t0, s.t1, s.car, s.driver.c_str(), s.mount.c_str(), f3(s.nudge).c_str(),
                 s.distance, s.height, s.yaw, s.pitch, s.roll, s.fov, f3(s.point).c_str(), s.radius, s.period,
                 s.start_angle, s.around, s.reverse ? "true" : "false", s.speed, s.constant_speed ? "constant" : "time", s.ease_in ? "true" : "false",
                 s.ease_out ? "true" : "false");
        std::string line = b;
        if (s.type == ShotType::transition) {  // forward, side, height, look (curve_to_json each), before "keys"
            std::string cs = "\"curves\": [";
            for (int c = 0; c < kAxCount; c++)
                cs += (c ? ", " : "") + curve_to_json(s.curves[c]);
            cs += "],\n      ";
            line.insert(line.rfind("\"keys\""), cs);
        }
        o += line;
        for (size_t j = 0; j < s.keys.size(); j++) {
            const ShotKey &k = s.keys[j];
            snprintf(b, sizeof b, "%s\n        { \"t\": %.4f, \"pos\": %s, \"rot\": [%.6f, %.6f, %.6f, %.6f], \"fov\": %.2f",
                     j ? "," : "", k.t, f3(k.pos).c_str(), k.rel.x, k.rel.y, k.rel.z, k.rel.w, k.fov);
            o += b;
            if (len(k.bend) > 1e-5f)
                o += ", \"bend\": " + f3(k.bend);
            if (k.seg != kSegSmooth)
                o += ", \"seg\": " + std::to_string(k.seg);
            if (std::fabs(k.twist_turns) > 1e-5f || k.twist_radius > 1e-5f) {
                snprintf(b, sizeof b, ", \"twist\": [%.3f, %.3f]", k.twist_turns, k.twist_radius);
                o += b;
            }
            if (k.has_orig)
                o += ", \"orig\": " + f3(k.orig);
            o += " }";
        }
        o += s.keys.empty() ? "]" : "\n      ]";
        if (!s.tracks.empty()) {
            o += ",\n      \"tracks\": [";
            for (size_t j = 0; j < s.tracks.size(); j++) {
                const Track &tr = s.tracks[j];
                o += (j ? ", " : "") + std::string("{ \"param\": \"") + tr.param + "\", \"keys\": [";
                for (size_t k = 0; k < tr.keys.size(); k++) {
                    const PKey &pk = tr.keys[k];
                    snprintf(b, sizeof b, "%s[%.4f, %.4f", k ? ", " : "", pk.t, pk.v);
                    o += b;
                    if (pk.curved)  // [t, v, curve]: the curve of the segment leaving this key
                        o += ", " + curve_to_json(pk.curve);
                    o += "]";
                }
                o += "] }";
            }
            o += "]";
        }
        o += " }";
        o += i + 1 < e.shots.size() ? ",\n" : "\n";
    }
    return o + "  ]\n}\n";
}

template <class J> Edit edit_from_json(const J &doc, std::string &err)
{
    Edit e;
    e.name = std::string(doc["name"].as_string("unnamed"));
    e.replay.file = std::string(doc["replay"]["file"].as_string(""));
    e.replay.length = float(doc["replay"]["length"].as_double(0));
    e.replay.drivers = std::string(doc["replay"]["drivers"].as_string(""));
    if (!doc["shots"].is_array()) {
        // version 1 (a line of keys): convert. Keys split into shots at "end" / "cut"; a run of keys riding one
        // car becomes an onboard shot with keys, anything else a free path (keys riding cars are dropped there,
        // with a note in err).
        Sequence old = from_json(doc, err);
        if (!err.empty())
            return e;
        size_t i = 0;
        int dropped = 0;
        while (i < old.keys.size()) {
            size_t j = i;
            while (j + 1 < old.keys.size() && !jump_after(old.keys, j))
                j++;
            Shot s;
            s.t0 = old.keys[i].t;
            s.t1 = std::max(old.keys[j].t, s.t0 + 0.5f);
            bool one_car = old.keys[i].pos_car;
            for (size_t k = i; k <= j; k++)
                one_car = one_car && old.keys[k].pos_car && old.keys[k].pos_car_id == old.keys[i].pos_car_id;
            s.type = one_car ? ShotType::onboard : ShotType::free_path;
            s.car = one_car ? old.keys[i].pos_car_id : -1;
            Frame w;
            for (size_t k = i; k <= j; k++) {
                const Key &o = old.keys[k];
                if (!one_car && o.pos_car) {
                    dropped++;
                    continue;
                }
                ShotKey nk;
                nk.t = o.t;
                nk.pos = one_car ? o.pos : w.local(o.pos);
                nk.rel = qnorm(qmul(qconj(frame_quat(w)), look_quat(w, o.yaw, o.pitch, o.roll)));
                nk.fov = o.fov;
                s.keys.push_back(nk);
            }
            if (!s.keys.empty() || s.type != ShotType::free_path)
                e.shots.push_back(s);
            i = j + 1;
        }
        if (dropped)
            err = "converted from version 1; " + std::to_string(dropped) + " key(s) riding a car inside a world path were dropped";
        e.sort();
        return e;
    }
    const auto &shots = doc["shots"];
    for (size_t i = 0; i < shots.size(); i++) {
        const auto &j = shots[i];
        Shot s;
        s.type = shot_type_from(j["type"].as_string("free"));
        s.t0 = float(j["t0"].as_double(0));
        s.t1 = float(j["t1"].as_double(s.t0 + 5));
        s.car = int(j["car"].as_int(-1));
        s.driver = std::string(j["driver"].as_string(""));
        s.mount = std::string(j["mount"].as_string("cockpit"));
        s.nudge = v3(j["nudge"], V3{});
        s.distance = float(j["distance"].as_double(6));
        s.height = float(j["height"].as_double(1.4));
        s.yaw = float(j["yaw"].as_double(0));
        s.pitch = float(j["pitch"].as_double(0));
        s.roll = float(j["roll"].as_double(0));
        s.fov = float(j["fov"].as_double(70));
        s.point = v3(j["point"], V3{});
        s.radius = float(j["radius"].as_double(8));
        s.period = float(j["period"].as_double(10));
        s.start_angle = float(j["start_angle"].as_double(0));
        s.around = float(j["around"].as_double(0));
        s.reverse = j["reverse"].as_bool(false);
        s.speed = float(j["speed"].as_double(1));
        s.constant_speed = j["timing"].as_string("time") == "constant";
        s.ease_in = j["ease_in"].as_bool(false);
        s.ease_out = j["ease_out"].as_bool(false);
        const auto &cj = j["curves"];
        for (size_t c = 0; c < cj.size() && c < size_t(kAxCount); c++)
            curve_from_json(cj[c], s.curves[c]);
        const auto &ks = j["keys"];
        for (size_t k = 0; k < ks.size(); k++) {
            const auto &jk = ks[k];
            ShotKey nk;
            nk.t = float(jk["t"].as_double(s.t0));
            nk.pos = v3(jk["pos"], V3{});
            const auto &r = jk["rot"];
            if (r.is_array() && r.size() == 4)
                nk.rel = qnorm({float(r[0].as_double()), float(r[1].as_double()), float(r[2].as_double()),
                                float(r[3].as_double(1))});
            nk.fov = float(jk["fov"].as_double(s.fov));
            nk.bend = v3(jk["bend"], V3{});
            nk.seg = std::clamp(int(jk["seg"].as_double(0)), 0, int(kSegCount) - 1);
            const auto &tw = jk["twist"];
            if (tw.is_array() && tw.size() == 2) {
                nk.twist_turns = float(tw[0].as_double());
                nk.twist_radius = std::max(0.0f, float(tw[1].as_double()));
            }
            if (jk["orig"].is_array()) {
                nk.orig = v3(jk["orig"], nk.pos);
                nk.has_orig = true;
            }
            s.keys.push_back(nk);
        }
        s.sort_keys();
        const auto &tj = j["tracks"];
        for (size_t k = 0; k < tj.size(); k++) {
            Track tr;
            tr.param = std::string(tj[k]["param"].as_string(""));
            if (!param_ptr(s, tr.param))
                continue;
            const auto &kk = tj[k]["keys"];
            for (size_t m = 0; m < kk.size(); m++)  // [t, v] (spline) or [t, v, curve]
                if (kk[m].is_array() && kk[m].size() >= 2) {
                    PKey pk;
                    pk.t = float(kk[m][0].as_double());
                    pk.v = float(kk[m][1].as_double());
                    if (kk[m].size() == 3)
                        pk.curved = curve_from_json(kk[m][2], pk.curve);
                    else if (kk[m].size() == 6)  // [t, v, x1, y1, x2, y2]
                        pk.curved = true, pk.curve = Curve(float(kk[m][2].as_double()), float(kk[m][3].as_double()),
                                                           float(kk[m][4].as_double()), float(kk[m][5].as_double()));
                    tr.keys.push_back(pk);
                }
            sort_track(tr);
            if (!tr.keys.empty())
                s.tracks.push_back(tr);
        }
        e.shots.push_back(s);
    }
    e.sort();
    return e;
}

} // namespace cine
