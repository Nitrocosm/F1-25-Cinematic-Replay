// Camera sequences: keyframes on the replay timeline, evaluated per frame into one camera.
// No game dependencies (tested offline by tests/seq_test.cpp); cinecam.cpp supplies car frames.
//
// A sequence is a JSON file:
// {
//   "name": "battle at turn 3",
//   "keys": [
//     { "t": 30.0,                                    // replay time (s)
//       "pos":  { "anchor": "car", "car": -1, "offset": [-1.1, 0.25, -1.6] },  // or { "anchor": "world", "p": [x,y,z] }
//       "look": { "type": "dir", "yaw": 0, "pitch": -3 },     // relative to the pos anchor's frame (car or world)
//            // or { "type": "car", "car": 5, "offset": [0, 0.5, 0] }  look at a point on a car
//            // or { "type": "point", "p": [x,y,z] }               look at a world point
//       "fov": 70, "roll": 0,
//       "speed": 1.0,                                  // replay speed at this key (ramps linearly to the next)
//       "cut": false,                                  // true: jump here (no blend from the previous key)
//       "end": false,                                  // true: this key ends its shot; the next key starts a new one
//       "path": "smooth" },                            // the segment that ends here: straight (default) | smooth
//     ...
//   ]
// }
// car: -1 = the car the replay follows; 0..N = car slot.
// Car offsets are (right, up, forward) in metres in the car's frame. Yaw: positive turns right; pitch:
// positive looks up; roll: rotation of the camera around its view (degrees; its on-screen direction is
// checked live).
//
// Every key resolves, at the current time, to an eye point and either a rotation (direction keys) or a
// look-at point (anchors move with their cars). Eye and look-at points are blended with a Hermite spline
// over the neighbouring keys, so a path can hand off smoothly from a trackside point to a point riding on
// a car. Between two direction keys the rotations are blended with slerp, so big pans turn smoothly.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace cine {

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a)
{
    float l = len(a);
    return l > 1e-6f ? a * (1 / l) : a;
}
inline V3 rot(V3 v, V3 k, float ang)  // rotate v around unit axis k (Rodrigues)
{
    float c = std::cos(ang), s = std::sin(ang);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}
constexpr float kDeg = 3.14159265f / 180;

// ----- rotations -----------------------------------------------------------------------------------
// A quaternion (x, y, z, w) of the rotation whose columns are (right, up, back): exactly the game's
// camera quaternion (its camera looks along local -Z, live check 2026-11-03).
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Quat quat_from_cols(V3 r, V3 u, V3 b)
{
    float m00 = r.x, m01 = u.x, m02 = b.x, m10 = r.y, m11 = u.y, m12 = b.y, m20 = r.z, m21 = u.z, m22 = b.z;
    float t = m00 + m11 + m22;
    Quat q;
    if (t > 0) {
        float s = std::sqrt(t + 1) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return q;
}

inline void cols_from_quat(Quat q, V3 &r, V3 &u, V3 &b)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    r = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
    u = {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)};
    b = {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
}

inline Quat slerp(Quat a, Quat b, float s)
{
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {  // the short way round
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    float ka = 1 - s, kb = s;
    if (d < 0.9995f) {
        float th = std::acos(d), sn = std::sin(th);
        ka = std::sin((1 - s) * th) / sn;
        kb = std::sin(s * th) / sn;
    }
    Quat q{a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb, a.w * ka + b.w * kb};
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

// A frame in the game's convention (live check 2026-11-03): Y is world up and forward x up = right.
// The world frame faces +Z with right = -X.
struct Frame {
    V3 p, r{-1, 0, 0}, u{0, 1, 0}, f{0, 0, 1};
    V3 at(V3 o) const { return p + r * o.x + u * o.y + f * o.z; }
    V3 dir(V3 o) const { return r * o.x + u * o.y + f * o.z; }
    V3 local(V3 w) const  // a world point in this frame's (right, up, forward) terms
    {
        V3 d = w - p;
        return {dot(d, r), dot(d, u), dot(d, f)};
    }
};

// Yaw (positive = right) and pitch (positive = up) of a world direction, in a frame's terms (degrees).
inline void local_angles(const Frame &fr, V3 dir, float &yaw, float &pitch)
{
    yaw = std::atan2(dot(dir, fr.r), dot(dir, fr.f)) / kDeg;
    pitch = std::asin(std::clamp(dot(dir, fr.u), -1.0f, 1.0f)) / kDeg;
}

// The view direction yaw/pitch away from a frame's forward (degrees).
inline V3 look_dir(const Frame &fr, float yaw, float pitch)
{
    float cy = std::cos(yaw * kDeg), sy = std::sin(yaw * kDeg);
    float cp = std::cos(pitch * kDeg), sp = std::sin(pitch * kDeg);
    return norm(fr.dir(V3{sy * cp, sp, cy * cp}));
}

// The camera rotation looking yaw/pitch away from a frame's forward, rolled around the view (degrees).
inline Quat look_quat(const Frame &fr, float yaw, float pitch, float roll)
{
    V3 f = look_dir(fr, yaw, pitch);
    float cy = std::cos(yaw * kDeg), sy = std::sin(yaw * kDeg);
    V3 r = std::fabs(dot(f, fr.u)) < 0.999f ? norm(cross(f, fr.u)) : norm(fr.r * cy - fr.f * sy);
    V3 u = cross(r, f);
    if (roll) {
        r = rot(r, f, roll * kDeg);
        u = rot(u, f, roll * kDeg);
    }
    return quat_from_cols(r, u, f * -1);
}

// Roll (degrees) of a camera basis relative to level in a frame (right lying in the frame's horizontal).
inline float roll_of(const Frame &fr, V3 r, V3 f)
{
    V3 level = cross(f, fr.u);
    if (len(level) < 1e-4f)
        return 0;
    level = norm(level);
    return std::atan2(dot(cross(level, r), f), dot(level, r)) / kDeg;
}

// The host resolves a car id (-1 = followed car) to its frame at the current time.
struct Cars {
    virtual std::optional<Frame> frame(int car) const = 0;
    // Drawing a path: frames at replay time t from now on (t < 0: the current moment again). Cars that only know
    // the current moment ignore it, and paths on cars are drawn around where the cars are now.
    virtual void at_time(float) const {}
    virtual ~Cars() = default;
};

struct Key {
    float t = 0;
    // position
    bool pos_car = true;
    int pos_car_id = -1;
    V3 pos{-1.1f, 0.25f, -1.6f};  // car offset or world point
    // look
    enum Look { dir, car, point } look = dir;
    float yaw = 0, pitch = 0;
    int look_car_id = -1;
    V3 look_v{0, 0.5f, 0};  // car offset (look = car) or world point (look = point)
    float fov = 70, roll = 0, speed = 1;
    bool cut = false;
    bool linear = true;  // the segment into this key: a straight line at constant speed (default) or a smooth curve
    bool end = false;
    bool selected = false;  // editor selection (not saved)  // ends its shot: no path into the next key (the next key is a jump, as with its `cut`)
};

struct Sequence {
    std::string name;
    std::vector<Key> keys;  // sorted by t
    bool empty() const { return keys.empty(); }
    float start() const { return keys.empty() ? 0 : keys.front().t; }
    float end() const { return keys.empty() ? 0 : keys.back().t; }
    void insert(const Key &k)
    {
        auto it = std::upper_bound(keys.begin(), keys.end(), k.t, [](float t, const Key &a) { return t < a.t; });
        keys.insert(it, k);
    }
};

// What a key looks like at the current time.
struct Resolved {
    V3 eye, target;
    V3 up_hint;           // screen-up when looking (almost) straight down: the anchor car's forward
    bool has_q = false;   // direction keys carry a full rotation (blended with slerp)
    Quat q;
    float fov, roll, speed;
};

inline std::optional<Resolved> resolve(const Key &k, const Cars &cars)
{
    Resolved o;
    o.up_hint = {0, 0, 1};
    Frame base;  // world frame unless the position rides on a car
    if (k.pos_car) {
        auto f = cars.frame(k.pos_car_id);
        if (!f)
            return std::nullopt;
        base = *f;
        o.eye = base.at(k.pos);
        o.up_hint = base.f;
    } else {
        base.p = {};
        o.eye = k.pos;
    }
    switch (k.look) {
    case Key::dir:
        o.target = o.eye + look_dir(base, k.yaw, k.pitch) * 10;
        o.has_q = true;
        o.q = look_quat(base, k.yaw, k.pitch, k.roll);
        break;
    case Key::car: {
        auto f = cars.frame(k.look_car_id);
        if (!f)
            return std::nullopt;
        o.target = f->at(k.look_v);
        if (!k.pos_car)
            o.up_hint = f->f;  // a world camera looking down on a car: keep that car's nose up
        break;
    }
    case Key::point:
        o.target = k.look_v;
        break;
    }
    o.fov = k.fov;
    o.roll = k.roll;
    o.speed = k.speed;
    return o;
}

struct Camera {
    V3 eye, target, up_hint, fwd, right, up;
    bool has_q = false;
    Quat q;
    float fov, roll, speed;
    int segment;  // index of the key the segment starts at; a change across a jump = hard cut
    bool cut;     // this frame starts a new shot (jump): reset TAA / motion blur
};

// Cubic Hermite between p1 and p2 (keys at times t0..t3, p0/p3 the neighbours). The tangents are
// Catmull-Rom's, scaled for uneven key spacing, and zero next to a held stretch (two equal points) so a
// path never overshoots into or out of a hold.
inline V3 spline(V3 p0, V3 p1, V3 p2, V3 p3, float t0, float t1, float t2, float t3, float s)
{
    const float eps = 1e-3f;
    float d12 = t2 - t1;
    V3 m1{}, m2{};
    bool hold = len(p2 - p1) < eps;
    if (!hold && len(p1 - p0) > eps && t2 - t0 > 1e-6f)
        m1 = (p2 - p0) * (d12 / (t2 - t0));
    if (!hold && len(p3 - p2) > eps && t3 - t1 > 1e-6f)
        m2 = (p3 - p1) * (d12 / (t3 - t1));
    float s2 = s * s, s3 = s2 * s;
    float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
    return p1 * h00 + m1 * h10 + p2 * h01 + m2 * h11;
}

// Orientation from a rotation (direction keys).
inline void orient_q(Camera &c)
{
    V3 b;
    cols_from_quat(c.q, c.right, c.up, b);
    c.fwd = b * -1;
}

// Orientation from eye -> target with world up, then roll around forward. Looking steeply down (or up),
// world up stops defining a screen "up", so it blends into the up hint (the anchor car's heading): an
// overhead shot keeps the car's nose up the screen.
inline void orient(Camera &c)
{
    V3 f = norm(c.target - c.eye);
    if (len(c.target - c.eye) < 1e-4f)
        f = {0, 0, 1};
    V3 wu{0, 1, 0};
    float steep = std::fabs(dot(f, wu));           // 1 = straight down or up
    float w = std::clamp((steep - 0.85f) / 0.13f, 0.0f, 1.0f);
    w = w * w * (3 - 2 * w);
    V3 hint = c.up_hint - f * dot(c.up_hint, f);   // the hint's part across the view
    if (w > 0 && len(hint) > 1e-3f) {
        V3 h = norm(hint) * (dot(f, wu) < 0 ? 1.0f : -1.0f);  // looking down: car forward = screen up
        wu = norm(wu * (1 - w) + h * w);
    }
    if (std::fabs(dot(f, wu)) > 0.9999f)
        wu = {0, 0, 1};                            // no hint: fall back to world axes
    V3 r = norm(cross(f, wu));                     // right = forward x up (the game's handedness)
    V3 u = cross(r, f);
    if (c.roll) {
        r = rot(r, f, c.roll * kDeg);
        u = rot(u, f, c.roll * kDeg);
    }
    c.fwd = f;
    c.right = r;
    c.up = u;
    c.q = quat_from_cols(r, u, f * -1);
}

// The camera at replay time t, or nothing outside the sequence (or when an anchor can't resolve).
// True when the camera jumps from key i to key i+1 (no path between them).
inline bool jump_after(const std::vector<Key> &k, size_t i) { return i + 1 < k.size() && (k[i + 1].cut || k[i].end); }

inline std::optional<Camera> evaluate(const Sequence &seq, float t, const Cars &cars)
{
    const auto &k = seq.keys;
    if (k.empty() || t < k.front().t || t > k.back().t)
        return std::nullopt;
    size_t i = 0;
    while (i + 1 < k.size() && t >= k[i + 1].t)
        i++;
    Camera c{};
    c.segment = int(i);
    if (i + 1 >= k.size() || jump_after(k, i)) {  // last key, or a shot held until the jump
        auto a = resolve(k[i], cars);
        if (!a)
            return std::nullopt;
        c.eye = a->eye;
        c.target = a->target;
        c.up_hint = a->up_hint;
        c.has_q = a->has_q;
        c.q = a->q;
        c.fov = a->fov;
        c.roll = a->roll;
        c.speed = a->speed;
    } else {
        // neighbours for the spline: stop at cuts (a cut starts a separate path)
        size_t i0 = (i > 0 && !jump_after(k, i - 1)) ? i - 1 : i;
        size_t i3 = (i + 2 < k.size() && !jump_after(k, i + 1)) ? i + 2 : i + 1;
        auto r0 = resolve(k[i0], cars), r1 = resolve(k[i], cars), r2 = resolve(k[i + 1], cars),
             r3 = resolve(k[i3], cars);
        if (!r0 || !r1 || !r2 || !r3)
            return std::nullopt;
        float span = k[i + 1].t - k[i].t;
        float u = span > 1e-6f ? std::clamp((t - k[i].t) / span, 0.0f, 1.0f) : 1.0f;
        // constant speed through each segment: no easing at the keys (easing stopped the camera at every key)
        float s = u;
        // neighbour times: a missing neighbour (path start / end / cut) stands at its own key
        float t0 = i0 == i ? k[i].t - span : k[i0].t, t3 = i3 == i + 1 ? k[i + 1].t + span : k[i3].t;
        if (k[i + 1].linear) {  // straight
            c.eye = r1->eye + (r2->eye - r1->eye) * s;
            c.target = r1->target + (r2->target - r1->target) * s;
        } else {
            c.eye = spline(r0->eye, r1->eye, r2->eye, r3->eye, t0, k[i].t, k[i + 1].t, t3, s);
            c.target = spline(r0->target, r1->target, r2->target, r3->target, t0, k[i].t, k[i + 1].t, t3, s);
        }
        c.up_hint = norm(r1->up_hint * (1 - s) + r2->up_hint * s);
        c.fov = r1->fov + (r2->fov - r1->fov) * s;
        c.roll = r1->roll + (r2->roll - r1->roll) * s;
        c.speed = r1->speed + (r2->speed - r1->speed) * u;  // speed ramps with replay time, not eased
        if (r1->has_q && r2->has_q) {  // two direction keys: blend the rotations (big pans turn smoothly)
            c.has_q = true;
            c.q = slerp(r1->q, r2->q, s);
        }
    }
    if (c.has_q) {
        orient_q(c);  // the key's roll is already inside its rotation
        c.target = c.eye + c.fwd * 10;
    } else {
        orient(c);
    }
    c.cut = false;
    return c;
}

// ----- JSON ----------------------------------------------------------------------------------------

template <class J> V3 v3(const J &a, V3 fb)
{
    if (!a.is_array() || a.size() < 3)
        return fb;
    return {float(a[0].as_double(fb.x)), float(a[1].as_double(fb.y)), float(a[2].as_double(fb.z))};
}

template <class J> Sequence from_json(const J &doc, std::string &err)
{
    Sequence s;
    s.name = std::string(doc["name"].as_string("unnamed"));
    const auto &keys = doc["keys"];
    if (!keys.is_array()) {
        err = "no \"keys\" list";
        return s;
    }
    for (size_t i = 0; i < keys.size(); i++) {
        const auto &j = keys[i];
        Key k;
        if (!j["t"].is_number()) {
            err = "key " + std::to_string(i) + " has no time \"t\"";
            return Sequence{};
        }
        k.t = float(j["t"].as_double());
        const auto &pos = j["pos"];
        if (pos.is_object()) {
            k.pos_car = pos["anchor"].as_string("car") != "world";
            k.pos_car_id = int(pos["car"].as_int(-1));
            k.pos = k.pos_car ? v3(pos["offset"], k.pos) : v3(pos["p"], V3{});
        }
        const auto &look = j["look"];
        if (look.is_object()) {
            auto type = look["type"].as_string("dir");
            k.look = type == "car" ? Key::car : type == "point" ? Key::point : Key::dir;
            k.yaw = float(look["yaw"].as_double(0));
            k.pitch = float(look["pitch"].as_double(0));
            k.look_car_id = int(look["car"].as_int(-1));
            k.look_v = k.look == Key::point ? v3(look["p"], V3{}) : v3(look["offset"], k.look_v);
        }
        k.fov = float(j["fov"].as_double(70));
        k.roll = float(j["roll"].as_double(0));
        k.speed = float(j["speed"].as_double(1));
        k.cut = j["cut"].as_bool(false);
        k.end = j["end"].as_bool(false);
        k.linear = j["path"].as_string("straight") != "smooth";
        s.keys.push_back(k);
    }
    std::stable_sort(s.keys.begin(), s.keys.end(), [](const Key &a, const Key &b) { return a.t < b.t; });
    return s;
}

inline std::string fmt_v3(V3 v)
{
    char b[96];
    snprintf(b, sizeof b, "[%.3f, %.3f, %.3f]", v.x, v.y, v.z);
    return b;
}

inline std::string key_json(const Key &k)
{
    char b[512];
    std::string o = "{ ";
    snprintf(b, sizeof b, "\"t\": %.3f, ", k.t);
    o += b;
    if (k.cut)
        o += "\"cut\": true, ";
    if (k.end)
        o += "\"end\": true, ";
    if (k.pos_car)
        snprintf(b, sizeof b, "\"pos\": { \"anchor\": \"car\", \"car\": %d, \"offset\": %s }, ", k.pos_car_id,
                 fmt_v3(k.pos).c_str());
    else
        snprintf(b, sizeof b, "\"pos\": { \"anchor\": \"world\", \"p\": %s }, ", fmt_v3(k.pos).c_str());
    o += b;
    if (k.look == Key::dir)
        snprintf(b, sizeof b, "\"look\": { \"type\": \"dir\", \"yaw\": %.2f, \"pitch\": %.2f }, ", k.yaw, k.pitch);
    else if (k.look == Key::car)
        snprintf(b, sizeof b, "\"look\": { \"type\": \"car\", \"car\": %d, \"offset\": %s }, ", k.look_car_id,
                 fmt_v3(k.look_v).c_str());
    else
        snprintf(b, sizeof b, "\"look\": { \"type\": \"point\", \"p\": %s }, ", fmt_v3(k.look_v).c_str());
    o += b;
    snprintf(b, sizeof b, "\"fov\": %.1f, \"roll\": %.2f", k.fov, k.roll);
    o += b;
    if (k.speed != 1) {
        snprintf(b, sizeof b, ", \"speed\": %.3f", k.speed);
        o += b;
    }
    if (!k.linear)
        o += ", \"path\": \"smooth\"";
    return o + " }";
}

inline std::string to_json(const Sequence &s)
{
    std::string o = "{\n  \"name\": \"";
    for (char ch : s.name)
        o += (ch == '"' || ch == '\\') ? '_' : ch;
    o += "\",\n  \"keys\": [\n";
    for (size_t i = 0; i < s.keys.size(); i++)
        o += "    " + key_json(s.keys[i]) + (i + 1 < s.keys.size() ? ",\n" : "\n");
    return o + "  ]\n}\n";
}

} // namespace cine
