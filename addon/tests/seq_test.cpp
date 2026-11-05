// Offline test of sequence.hpp: a fake car on a circle, a sequence mixing world, car and look-at keys.
// Build and run: bash tests/run.sh
#include "../native/src/shots.hpp"

#include <swofty/json.hpp>

#include <cstdio>
#include <cstdlib>

using namespace cine;

static int g_fail;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            g_fail++;                                                                                                 \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                          \
            std::printf(__VA_ARGS__);                                                                                 \
            std::printf("\n");                                                                                        \
        }                                                                                                             \
    } while (0)

// Car 0 (and the followed car, -1) drive a circle of radius 200 m at 60 m/s, anticlockwise seen from above.
// Car 1 is 10 m behind car 0 on the same circle.
struct FakeCars : Cars {
    float t = 0;
    std::optional<Frame> frame(int car) const override
    {
        if (car > 1)
            return std::nullopt;
        float behind = car == 1 ? 10.0f : 0.0f;
        float a = (60 * t - behind) / 200;
        Frame f;
        f.p = {200 * std::cos(a), 0, 200 * std::sin(a)};
        f.f = norm(V3{-std::sin(a), 0, std::cos(a)});  // direction of travel
        f.u = {0, 1, 0};
        f.r = cross(f.f, f.u);  // the game's handedness: forward x up = right
        return f;
    }
};

static const char *kSeq = R"({
  "name": "test",
  "keys": [
    { "t": 10, "pos": { "anchor": "world", "p": [0, 5, 0] }, "look": { "type": "car", "car": 0 }, "fov": 40 },
    { "t": 14, "path": "smooth", "pos": { "anchor": "world", "p": [10, 5, 10] }, "look": { "type": "car", "car": 0 }, "fov": 50 },
    { "t": 18, "path": "smooth", "pos": { "anchor": "car", "car": 0, "offset": [-1.1, 0.25, 1.6] }, "look": { "type": "dir" }, "fov": 70,
      "speed": 0.25 },
    { "t": 22, "cut": true, "pos": { "anchor": "car", "car": 1, "offset": [0, 1, -4] },
      "look": { "type": "car", "car": 0, "offset": [0, 0.5, 0] }, "fov": 60, "speed": 1 },
    { "t": 26, "path": "smooth", "pos": { "anchor": "car", "car": 1, "offset": [0, 1, -4] }, "look": { "type": "car", "car": 0 } }
  ]
})";

int main()
{
    swofty::json::Error e;
    auto doc = swofty::json::parse(kSeq, &e);
    CHECK(e.ok(), "json: %s", e.message.c_str());
    std::string err;
    Sequence s = from_json(doc, err);
    CHECK(err.empty(), "from_json: %s", err.c_str());
    CHECK(s.keys.size() == 5, "5 keys, got %zu", s.keys.size());
    CHECK(s.keys[3].cut && !s.keys[2].cut, "cut flags");

    FakeCars cars;
    // outside the sequence: nothing
    cars.t = 9.9f;
    CHECK(!evaluate(s, cars.t, cars), "before the first key");
    cars.t = 26.1f;
    CHECK(!evaluate(s, cars.t, cars), "after the last key");

    // exactly on a key: the key itself
    cars.t = 10;
    auto c = evaluate(s, 10, cars);
    CHECK(c && len(c->eye - V3{0, 5, 0}) < 1e-3f, "key 0 eye");
    auto car0 = *cars.frame(0);
    CHECK(c && dot(c->fwd, norm(car0.p - c->eye)) > 0.9999f, "key 0 looks at car 0");
    CHECK(c && std::fabs(c->fov - 40) < 1e-3f, "key 0 fov");

    // the car-anchored key: front-left of the car, facing its direction of travel
    cars.t = 18;
    c = evaluate(s, 18, cars);
    car0 = *cars.frame(0);
    V3 want = car0.at({-1.1f, 0.25f, 1.6f});
    CHECK(c && len(c->eye - want) < 1e-3f, "key 2 eye at the car offset (%.3f m off)", c ? len(c->eye - want) : -1.f);
    CHECK(c && dot(c->fwd, car0.f) > 0.9999f, "key 2 looks along the car");
    CHECK(c && dot(c->eye - car0.p, car0.r) < 0, "key 2 is on the car's left");
    CHECK(c && std::fabs(c->speed - 0.25f) < 1e-4f, "key 2 speed");

    // segment 2 -> 3 is a cut: key 2 is held (riding on car 0) until t = 22, then key 3
    cars.t = 21.9f;
    c = evaluate(s, cars.t, cars);
    car0 = *cars.frame(0);
    CHECK(c && len(c->eye - car0.at({-1.1f, 0.25f, 1.6f})) < 1e-3f, "held shot keeps riding on car 0");
    CHECK(c && c->segment == 2, "segment 2 before the cut");
    cars.t = 22;
    c = evaluate(s, cars.t, cars);
    CHECK(c && c->segment == 3, "segment 3 at the cut");
    auto car1 = *cars.frame(1);
    CHECK(c && len(c->eye - car1.at({0, 1, -4})) < 1e-3f, "key 3 rides behind car 1");

    // continuity: sample at 60 Hz, the eye may only jump at the cut (t = 22)
    float worst = 0, worst_t = 0, prev_step = -1;
    V3 prev{};
    bool have = false;
    int seg_prev = -1;
    for (int n = 0; n <= 16 * 60; n++) {
        cars.t = 10 + n / 60.0f;
        auto cc = evaluate(s, cars.t, cars);
        CHECK(cc.has_value(), "inside the sequence at t %.3f", cars.t);
        if (!cc)
            continue;
        CHECK(std::isfinite(cc->eye.x) && std::isfinite(cc->fwd.x), "finite at t %.3f", cars.t);
        CHECK(std::fabs(dot(cc->fwd, cc->right)) < 1e-4f && std::fabs(len(cc->up) - 1) < 1e-4f, "orthonormal");
        bool cut_here = cc->segment != seg_prev && cc->segment > 0 && jump_after(s.keys, size_t(cc->segment - 1));
        if (have && !cut_here) {
            // a jump shows up as a step much bigger than the one before it (speed may be high, but smooth)
            float step = len(cc->eye - prev);
            if (prev_step >= 0 && std::fabs(step - prev_step) > worst) {
                worst = std::fabs(step - prev_step);
                worst_t = cars.t;
            }
            prev_step = step;
        } else {
            prev_step = -1;
        }
        prev = cc->eye;
        have = true;
        seg_prev = cc->segment;
    }
    // change of the per-frame step under 5 cm per frame (= 180 m/s^2): no jumps
    CHECK(worst < 0.05f, "largest change of the eye step %.4f m at t %.3f (cut excluded)", worst, worst_t);
    std::printf("largest change of the eye step between frames: %.4f m at t %.3f\n", worst, worst_t);

    // speed ramps linearly from key 1 (1.0) to key 2 (0.25)
    cars.t = 16;
    c = evaluate(s, 16, cars);
    CHECK(c && std::fabs(c->speed - 0.625f) < 1e-3f, "speed ramp midpoint %.3f", c ? c->speed : -1.f);

    // a hold (two equal keys after a different one) must not overshoot (the in-game bug of 2026-11-03:
    // Catmull-Rom pushed the camera 0.3 m into the bodywork)
    {
        Sequence h;
        Key a;
        a.t = 0;
        a.pos = {-5, 1.2f, 0};
        Key b = a;
        b.t = 5;
        b.pos = {-1.1f, 0.25f, 1.6f};
        Key c2 = b;
        c2.t = 8;
        h.keys = {a, b, c2};
        float worst_hold = 0;
        for (float t = 5; t <= 8; t += 0.05f) {
            cars.t = t;
            auto hc = evaluate(h, t, cars);
            auto f = *cars.frame(-1);
            if (hc)
                worst_hold = std::max(worst_hold, len(hc->eye - f.at(b.pos)));
        }
        CHECK(worst_hold < 1e-3f, "held shot drifts %.4f m from its key", worst_hold);
    }

    // straight-down shot above a car: the car's nose points up the screen (camera up ~ car forward)
    {
        Sequence o;
        Key a;
        a.t = 0;
        a.pos = {0, 40, -0.01f};
        a.look = Key::car;
        a.look_car_id = 0;
        a.look_v = {0, 0, 0};
        Key b = a;
        b.t = 10;
        o.keys = {a, b};
        for (float t : {1.0f, 4.0f, 7.0f}) {
            cars.t = t;
            auto oc = evaluate(o, t, cars);
            auto f = *cars.frame(0);
            CHECK(oc && dot(oc->fwd, V3{0, -1, 0}) > 0.999f, "looks straight down");
            CHECK(oc && dot(oc->up, f.f) > 0.99f, "car nose up the screen at t %.1f (up.f %.3f)", t,
                  oc ? dot(oc->up, f.f) : -2.f);
        }
    }

    // yaw / pitch / roll of direction keys, on a car and in the world
    {
        cars.t = 3;
        auto f = *cars.frame(0);
        Key k;
        k.pos_car_id = 0;
        k.yaw = 90;
        auto r = resolve(k, cars);
        Camera c{};
        c.q = r->q;
        orient_q(c);
        CHECK(dot(c.fwd, f.r) > 0.999f, "car key yaw +90 looks to the car's right");
        k.yaw = 0;
        k.pitch = 30;
        r = resolve(k, cars);
        c.q = r->q;
        orient_q(c);
        CHECK(std::fabs(dot(c.fwd, f.u) - 0.5f) < 1e-3f, "pitch +30 looks up 30 degrees");
        k.pitch = 0;
        k.roll = 20;
        r = resolve(k, cars);
        c.q = r->q;
        orient_q(c);
        CHECK(std::fabs(roll_of(f, c.right, c.fwd) - 20) < 0.05f, "roll 20 reads back as %.2f", roll_of(f, c.right, c.fwd));
        Key w;
        w.pos_car = false;
        w.pos = {0, 10, 0};
        w.yaw = 90;
        r = resolve(w, cars);
        c.q = r->q;
        orient_q(c);
        Frame world;
        CHECK(dot(c.fwd, world.r) > 0.999f && dot(c.fwd, V3{-1, 0, 0}) > 0.999f, "world yaw +90 looks along -X");
        float yw, pt;
        local_angles(world, c.fwd, yw, pt);
        CHECK(std::fabs(yw - 90) < 0.05f && std::fabs(pt) < 0.05f, "local_angles reads back yaw %.2f pitch %.2f", yw, pt);
    }

    // a 180 degree pan between two direction keys turns smoothly (slerp), never flipping in one frame
    {
        Sequence p;
        Key a;
        a.pos_car = false;
        a.pos = {0, 5, 0};
        a.t = 0;
        Key b = a;
        b.t = 4;
        b.yaw = 179;
        p.keys = {a, b};
        float worst = 0;
        V3 prev{};
        for (int n = 0; n <= 240; n++) {
            float t = n / 60.0f;
            auto pc = evaluate(p, t, cars);
            CHECK(pc.has_value(), "pan at %.2f", t);
            if (n && pc)
                worst = std::max(worst, std::acos(std::clamp(dot(prev, pc->fwd), -1.0f, 1.0f)) / kDeg);
            if (pc)
                prev = pc->fwd;
        }
        CHECK(worst < 2.5f, "pan step per frame %.2f degrees", worst);
    }

    // JSON round trip
    {
        std::string text = to_json(s);
        swofty::json::Error e2;
        auto d2 = swofty::json::parse(text, &e2);
        std::string err2;
        Sequence s2 = from_json(d2, err2);
        CHECK(e2.ok() && err2.empty() && s2.keys.size() == s.keys.size(), "round trip parses");
        bool same = true;
        for (size_t i = 0; i < s.keys.size() && i < s2.keys.size(); i++) {
            const Key &x = s.keys[i], &y = s2.keys[i];
            same = same && std::fabs(x.t - y.t) < 1e-3f && x.pos_car == y.pos_car && x.pos_car_id == y.pos_car_id &&
                   len(x.pos - y.pos) < 1e-3f && x.look == y.look && x.look_car_id == y.look_car_id &&
                   std::fabs(x.fov - y.fov) < 0.06f && std::fabs(x.speed - y.speed) < 1e-3f && x.cut == y.cut;
        }
        CHECK(same, "round trip keeps every key");
        Sequence ins = s2;
        Key mid;
        mid.t = 15;
        ins.insert(mid);
        CHECK(ins.keys.size() == s2.keys.size() + 1 && ins.keys[2].t == 15, "insert keeps time order");
    }

    // free camera: fly a bit, ride a car, and the keys it makes play back the same view
    {
        FreeCam fc;
        Frame world;
        cars.t = 5;
        fc.set_from_world(world, V3{10, 4, -20}, look_quat(world, 30, -10, 5), 55);
        float fy, fp, frl;
        fc.angles(world, fy, fp, frl);
        CHECK(len(fc.eye(world) - V3{10, 4, -20}) < 1e-3f && std::fabs(fy - 30) < 0.01f && std::fabs(fp + 10) < 0.01f &&
                  std::fabs(frl - 5) < 0.05f,
              "set_from_world reads the view back (yaw %.2f pitch %.2f roll %.2f)", fy, fp, frl);
        fc.level(world);
        fc.angles(world, fy, fp, frl);
        CHECK(std::fabs(frl) < 0.05f && std::fabs(fy - 30) < 0.05f && std::fabs(fp + 10) < 0.05f,
              "level removes the roll and keeps the view (yaw %.2f pitch %.2f roll %.2f)", fy, fp, frl);
        fc.set_from_world(world, V3{10, 4, -20}, look_quat(world, 30, 0, 0), 55);
        FreeInput in;
        in.move_fwd = 1;
        in.yaw = 2;
        for (int n = 0; n < 60; n++)
            fc.update(in, 1 / 60.0f, world);
        fc.angles(world, fy, fp, frl);
        CHECK(std::fabs(std::remainder(fy - 150, 360.0f)) < 0.05f, "turned 120 degrees (yaw %.2f)", fy);
        Key wk = fc.key(5, world);
        auto wc = evaluate(Sequence{"", {wk, [&] { Key k2 = wk; k2.t = 6; return k2; }()}}, 5.5f, cars);
        V3 r_, u_, b_;
        cols_from_quat(fc.rotation(world), r_, u_, b_);
        CHECK(wc && len(wc->eye - fc.eye(world)) < 1e-3f && dot(wc->fwd, b_ * -1) > 0.9999f &&
                  dot(wc->right, r_) > 0.9999f,
              "a world key from the free camera plays back the same view");
        // ride car 0: rebase keeps the view, then the key follows the car
        auto car0 = *cars.frame(0);
        V3 before = fc.eye(world);
        fc.ride = true;
        fc.ride_car = 0;
        fc.rebase(world, car0);
        CHECK(len(fc.eye(car0) - before) < 1e-3f, "rebase onto the car keeps the eye");
        Key ck = fc.key(5, car0);
        cars.t = 7;  // two seconds later the car has moved on; the riding key moves with it
        auto car0b = *cars.frame(0);
        auto cc = evaluate(Sequence{"", {ck, [&] { Key k2 = ck; k2.t = 9; return k2; }()}}, 7, cars);
        CHECK(cc && len(cc->eye - car0b.at(fc.pos)) < 1e-3f, "a riding key moves with its car");
    }

    // a rolled camera moves and turns in its own axes
    {
        FreeCam fc;
        Frame world;
        fc.set_from_world(world, V3{0, 10, 0}, look_quat(world, 0, 0, 90), 60);
        V3 r0, u0, b0;
        cols_from_quat(fc.rotation(world), r0, u0, b0);
        CHECK(std::fabs(dot(r0, V3{0, 1, 0})) > 0.999f, "rolled 90: the camera's right points along world up/down");
        FreeInput mv;
        mv.move_right = 1;
        V3 before = fc.eye(world);
        fc.update(mv, 0.5f, world);  // 12 m/s * 0.5 s = 6 m
        V3 step = fc.eye(world) - before;
        CHECK(len(step) > 5.9f && dot(norm(step), r0) > 0.999f, "move right goes along the camera's own right");
        FreeInput lk;
        lk.yaw = 10;  // turn "right" on screen
        V3 f0 = b0 * -1;
        fc.update(lk, 0.016f, world);
        V3 r1, u1, b1;
        cols_from_quat(fc.rotation(world), r1, u1, b1);
        V3 f1 = b1 * -1;
        CHECK(dot(f1 - f0, r0) > 0.1f && std::fabs(dot(f1 - f0, u0)) < 0.01f,
              "yaw turns toward the camera's own right, not the world's");
    }

    // align: look along a car's heading, level in its frame, keeping the position
    {
        cars.t = 3;
        auto car = *cars.frame(0);
        FreeCam fc;
        Frame world;
        fc.set_from_world(world, V3{5, 3, 5}, look_quat(world, 77, 12, 30), 60);
        fc.align(world, car);
        V3 r2, u2, b2;
        cols_from_quat(fc.rotation(world), r2, u2, b2);
        CHECK(dot(b2 * -1, car.f) > 0.9999f && dot(u2, car.u) > 0.9999f && len(fc.eye(world) - V3{5, 3, 5}) < 1e-3f,
              "align looks exactly along the car, level, and stays put");
    }

    // the track surface: a banked, sloped normal; flat_on lies flat on it, level_to only matches the roll
    {
        V3 up = norm(V3{0.2f, 1, 0.1f});  // banked and sloped
        FreeCam fc;
        Frame world;
        fc.set_from_world(world, V3{0, 2, 0}, look_quat(world, 40, 15, 25), 60);
        V3 r0, u0, b0;
        cols_from_quat(fc.rotation(world), r0, u0, b0);
        fc.level_to(world, up);
        V3 r1, u1, b1;
        cols_from_quat(fc.rotation(world), r1, u1, b1);
        CHECK(dot(b1, b0) > 0.9999f && std::fabs(dot(r1, up)) < 1e-4f,
              "level_to keeps the view and lays the camera's right in the surface");
        fc.flat_on(world, up);
        V3 r2, u2, b2;
        cols_from_quat(fc.rotation(world), r2, u2, b2);
        V3 f0 = b0 * -1, fp = norm(f0 - up * dot(f0, up));
        CHECK(dot(u2, up) > 0.9999f && dot(b2 * -1, fp) > 0.9999f && len(fc.eye(world) - V3{0, 2, 0}) < 1e-4f,
              "flat_on looks along the heading in the surface, with the surface's up, and stays put");
    }

    // shot ends: shot A rides car 0 and pans, ends at t 5; shot B (cockpit-like, riding car 1) starts one frame
    // later. Until 5 the path is A's; from 5 + 1/60 it is B's; nothing blends between them.
    {
        Sequence sh;
        Key a1;
        a1.t = 2;
        a1.pos_car_id = 0;
        a1.pos = {-4, 1.5f, 0};
        a1.look = Key::car;
        a1.look_car_id = 0;
        Key a2 = a1;
        a2.t = 5;
        a2.pos = {-4, 1.5f, 6};
        a2.end = true;
        Key b1;
        b1.t = 5 + 1.0f / 60;
        b1.pos_car_id = 1;
        b1.pos = {0, 0.9f, 0.4f};
        b1.look = Key::dir;
        b1.fov = 85;
        Key b2 = b1;
        b2.t = 9;
        sh.keys = {a1, a2, b1, b2};
        cars.t = 5;
        auto at_end = evaluate(sh, 5.0f, cars);
        auto c0 = *cars.frame(0), c1 = *cars.frame(1);
        CHECK(at_end && at_end->segment == 1 && len(at_end->eye - c0.at(a2.pos)) < 1e-3f, "shot A holds its last key");
        float tb = 5 + 1.0f / 60;
        cars.t = tb;
        auto at_b = evaluate(sh, tb, cars);
        c1 = *cars.frame(1);
        CHECK(at_b && at_b->segment == 2 && len(at_b->eye - c1.at(b1.pos)) < 1e-3f && std::fabs(at_b->fov - 85) < 1e-3f,
              "one frame later shot B is fully there (no blend)");
        CHECK(jump_after(sh.keys, 1) && !jump_after(sh.keys, 0) && !jump_after(sh.keys, 2), "only the shot end jumps");
        // A's path does not bend toward B: halfway through A it is the plain A path
        cars.t = 3.5f;
        Sequence only_a;
        only_a.keys = {a1, a2};
        only_a.keys[1].end = false;
        auto p1 = evaluate(sh, 3.5f, cars), p2 = evaluate(only_a, 3.5f, cars);
        CHECK(p1 && p2 && len(p1->eye - p2->eye) < 1e-4f, "shot B does not bend shot A's path");
        std::string txt = to_json(sh);
        swofty::json::Error e3;
        auto back = from_json(swofty::json::parse(txt, &e3), txt);
        CHECK(back.keys.size() == 4 && back.keys[1].end && !back.keys[0].end, "end survives a save");
    }

    // straight is the default: two world keys, the camera is exactly halfway at half time, at constant speed
    {
        Sequence st;
        Key a;
        a.pos_car = false;
        a.pos = {0, 5, 0};
        a.t = 0;
        Key b = a;
        b.t = 4;
        b.pos = {40, 5, 20};
        CHECK(b.linear, "a new key is straight");
        st.keys = {a, b};
        auto m = evaluate(st, 2, cars), q1 = evaluate(st, 1, cars);
        CHECK(m && len(m->eye - V3{20, 5, 10}) < 1e-3f && q1 && len(q1->eye - V3{10, 5, 5}) < 1e-3f,
              "straight: halfway at half time, a quarter at a quarter (no easing)");
        std::string txt = to_json(st);
        swofty::json::Error e4;
        std::string er;
        auto back = from_json(swofty::json::parse(txt, &e4), er);
        CHECK(back.keys.size() == 2 && back.keys[1].linear && txt.find("smooth") == std::string::npos,
              "straight saves as the default (no path field)");
    }

    // a smooth path keeps moving through a key (no stop): the speed just before and after the key is similar
    {
        Sequence sm;
        Key a;
        a.pos_car = false;
        a.pos = {0, 5, 0};
        a.t = 0;
        a.linear = false;
        Key b = a, c2 = a;
        b.t = 2;
        b.pos = {20, 5, 0};
        c2.t = 4;
        c2.pos = {40, 5, 0};
        sm.keys = {a, b, c2};
        auto p0 = evaluate(sm, 1.95f, cars), p1 = evaluate(sm, 2.0f, cars), p2 = evaluate(sm, 2.05f, cars);
        float v0 = len(p1->eye - p0->eye) / 0.05f, v1 = len(p2->eye - p1->eye) / 0.05f;
        CHECK(v0 > 8 && v1 > 8 && std::fabs(v0 - v1) < 1.0f, "smooth: no stop at the middle key (%.2f / %.2f m/s)", v0, v1);
    }

#include "shots_tests.inc"
    // an anchor that can't resolve gives nothing (no garbage camera)
    Sequence bad = s;
    bad.keys[0].look_car_id = 7;
    cars.t = 10;
    CHECK(!evaluate(bad, 10, cars), "unresolvable car");

    std::printf(g_fail ? "%d check(s) FAILED\n" : "all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
