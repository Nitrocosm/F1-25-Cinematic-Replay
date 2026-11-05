// The free camera: flown with the pad or keyboard and mouse, in the world or riding on a car. Pure logic
// (no game reads), tested offline by tests/seq_test.cpp. cinecam.cpp feeds it input and the car frames.
//
// The orientation is a quaternion relative to the frame (the world, or the car it rides on), and every
// control works in the camera's own axes: looking turns around the camera's up and right, rolling around
// its view, and moving goes along its view, right and up. So a rolled camera still flies and turns the way
// the picture shows.
#pragma once

#include "sequence.hpp"

namespace cine {

inline Quat qmul(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat qconj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
inline float qdot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline Quat qaxis(V3 axis, float deg)
{
    float h = deg * kDeg / 2, s = std::sin(h);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
}
inline Quat qnorm(Quat q)
{
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return n > 0 ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}
inline V3 qrot(Quat q, V3 v)
{
    Quat p = qmul(qmul(q, Quat{v.x, v.y, v.z, 0}), qconj(q));
    return {p.x, p.y, p.z};
}
// The rotation of a frame looking along its forward: camera columns (right, up, back).
inline Quat frame_quat(const Frame &fr) { return quat_from_cols(fr.r, fr.u, fr.f * -1); }

// One frame's controls, already merged from pad and keyboard/mouse.
struct FreeInput {
    float move_right = 0, move_fwd = 0, move_up = 0;  // -1..1, in the camera's own axes
    float yaw = 0, pitch = 0;                         // degrees to turn this frame (already scaled)
    float roll_rate = 0, fov_rate = 0;                // -1..1
    float speed_mul = 1;                              // Shift / Ctrl
    float fine = 1;                                   // precision: scales look, roll, zoom (and movement)
};

struct FreeCam {
    bool ride = false;  // riding on a car: pos and rotation are relative to that car's frame
    int ride_car = -1;
    V3 pos;             // in the frame's terms (right, up, forward), metres
    Quat rel;           // the rotation relative to the frame's own looking-forward rotation
    float fov = 60;
    float speed = 12;  // flying speed, m/s (mouse wheel / presets)
    static constexpr float kPresets[3] = {3, 12, 40};

    // The next preset above the current speed (wrapping), for the pad's right-stick click.
    void next_preset()
    {
        for (float p : kPresets)
            if (p > speed * 1.01f) {
                speed = p;
                return;
            }
        speed = kPresets[0];
    }
    void scale_speed(float k) { speed = std::clamp(speed * k, 0.25f, 150.0f); }

    Quat world_rot(const Frame &fr) const { return qnorm(qmul(frame_quat(fr), rel)); }
    V3 eye(const Frame &fr) const { return fr.at(pos); }
    Quat rotation(const Frame &fr) const { return world_rot(fr); }

    // Start from a world camera (eye + rotation), in `fr` (the world frame, or a car's when riding).
    void set_from_world(const Frame &fr, V3 eye_w, Quat q, float fov_deg)
    {
        pos = fr.local(eye_w);
        rel = qnorm(qmul(qconj(frame_quat(fr)), q));
        fov = fov_deg;
    }

    // Yaw / pitch / roll of the view in the frame's terms (what a key stores).
    void angles(const Frame &fr, float &yaw, float &pitch, float &roll) const
    {
        V3 r, u, b;
        cols_from_quat(world_rot(fr), r, u, b);
        V3 f = b * -1;
        local_angles(fr, f, yaw, pitch);
        roll = roll_of(fr, r, f);
    }

    void update(const FreeInput &in, float dt, const Frame &fr)
    {
        (void)fr;
        // turn in the camera's own axes: yaw around its up (y), pitch around its right (x), roll around its
        // view (-z). Positive yaw turns right, positive pitch looks up, positive roll matches roll_of.
        Quat d = qmul(qmul(qaxis({0, 1, 0}, -in.yaw), qaxis({1, 0, 0}, in.pitch)),
                      qaxis({0, 0, 1}, -in.roll_rate * 45 * in.fine * dt));
        rel = qnorm(qmul(rel, d));
        fov = std::clamp(fov + in.fov_rate * 25 * in.fine * dt * (fov / 60), 5.0f, 120.0f);
        // move in the camera's own axes; `rel` maps camera axes (x right, y up, z back) to the frame's
        // (right, up, back)
        V3 v = qrot(rel, V3{in.move_right, in.move_up, -in.move_fwd});
        float s = speed * in.speed_mul * in.fine * dt;
        pos = pos + V3{v.x, v.y, -v.z} * s;
    }

    // Level the horizon in the frame, keeping where the camera looks.
    void level(const Frame &fr)
    {
        float y, p, r;
        angles(fr, y, p, r);
        rel = qnorm(qmul(qconj(frame_quat(fr)), look_quat(fr, y, p, 0)));
    }

    // Look exactly along another frame's forward (a car's heading), level in that frame, keeping the position.
    void align(const Frame &fr, const Frame &to)
    {
        rel = qnorm(qmul(qconj(frame_quat(fr)), frame_quat(to)));
    }

    // Roll to a surface (its up vector, world terms), keeping where the camera looks.
    void level_to(const Frame &fr, V3 up)
    {
        V3 r, u, b;
        cols_from_quat(world_rot(fr), r, u, b);
        V3 f = b * -1;
        V3 rn = cross(f, up);
        if (len(rn) < 1e-3f)
            return;  // looking straight along the normal: no roll to choose
        rn = norm(rn);
        rel = qnorm(qmul(qconj(frame_quat(fr)), quat_from_cols(rn, cross(rn, f), b)));
    }

    // Lie flat on a surface: look along the current heading projected onto it, level (pitch and roll 0).
    void flat_on(const Frame &fr, V3 up)
    {
        V3 r, u, b;
        cols_from_quat(world_rot(fr), r, u, b);
        V3 f = b * -1, fp = f - up * dot(f, up);
        if (len(fp) < 1e-3f)
            fp = u - up * dot(u, up);  // looking straight down: keep the screen's up as the heading
        if (len(fp) < 1e-3f)
            return;
        fp = norm(fp);
        V3 rn = norm(cross(fp, up));
        rel = qnorm(qmul(qconj(frame_quat(fr)), quat_from_cols(rn, up, fp * -1)));
    }

    // Switch frames (world <-> riding a car) without moving the view.
    void rebase(const Frame &from, const Frame &to)
    {
        V3 e = eye(from);
        Quat q = world_rot(from);
        set_from_world(to, e, q, fov);
    }

    // The key that reproduces this view at replay time t.
    Key key(float t, const Frame &fr) const
    {
        Key k;
        k.t = t;
        k.look = Key::dir;
        k.fov = fov;
        angles(fr, k.yaw, k.pitch, k.roll);
        if (ride) {
            k.pos_car = true;
            k.pos_car_id = ride_car;
            k.pos = pos;
        } else {
            k.pos_car = false;
            k.pos = eye(fr);  // world keys store world coordinates
        }
        return k;
    }
};

} // namespace cine
