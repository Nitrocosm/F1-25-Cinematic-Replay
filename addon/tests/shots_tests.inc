    // ---------------- shots (shots.hpp) ----------------

    // path shaping: a bend pushes a segment's middle, leaves the keys where they are and doesn't jerk at them
    {
        cars.t = 0;
        Shot s;
        s.type = ShotType::free_path;
        s.t0 = 0;
        s.t1 = 6;
        for (int i = 0; i < 4; i++) {
            ShotKey k;
            k.t = 2.0f * i;
            k.pos = {float(i) * 20, 5, 0};
            k.rel = qconj(frame_quat(Frame{}));
            s.keys.push_back(k);
        }
        Shot b = s;
        b.keys[1].bend = {0, 3, 0};  // the segment from key 1 (t 2) to key 2 (t 4)
        float key_moved = 0;
        for (float kt : {0.0f, 2.0f, 4.0f, 6.0f})
            key_moved = std::max(key_moved, len(shot_view(b, 0, kt, cars)->eye - shot_view(s, 0, kt, cars)->eye));
        float mid_push = len(shot_view(b, 0, 3, cars)->eye - shot_view(s, 0, 3, cars)->eye);
        float other = len(shot_view(b, 0, 1, cars)->eye - shot_view(s, 0, 1, cars)->eye);
        float worst_ratio = 1;
        for (float kt : {2.0f, 4.0f}) {
            V3 a0 = shot_view(b, 0, kt - 0.01f, cars)->eye, a1 = shot_view(b, 0, kt, cars)->eye,
               a2 = shot_view(b, 0, kt + 0.01f, cars)->eye;
            float v0 = len(a1 - a0), v1 = len(a2 - a1);
            worst_ratio = std::max(worst_ratio, std::max(v0, v1) / std::max(1e-6f, std::min(v0, v1)));
        }
        CHECK(key_moved < 1e-3f && std::fabs(mid_push - 3) < 1e-3f && other < 1e-3f && worst_ratio < 1.02f,
              "a bend: keys stay (%.4f), the middle moves 3 m (%.4f), other segments stay (%.4f), no jerk at the "
              "keys (%.4f)",
              key_moved, mid_push, other, worst_ratio);
        PathSample ps = sample_path(b, 0, cars);
        CHECK(ps.handles.size() == 3 && ps.handles[1].key == 1 && std::fabs(len(ps.handles[1].at - ps.handles[1].rest) - 3) < 1e-3f,
              "a handle per segment, at the bent middle");
        // JSON
        Edit e;
        e.shots = {b};
        swofty::json::Error je;
        std::string er;
        Edit back = edit_from_json(swofty::json::parse(edit_to_json(e), &je), er);
        CHECK(je.ok() && len(back.shots[0].keys[1].bend - V3{0, 3, 0}) < 1e-3f && len(back.shots[0].keys[0].bend) < 1e-6f,
              "bends survive a save");
    }
    // segment timings and the corkscrew
    {
        cars.t = 0;
        Shot s;
        s.type = ShotType::free_path;
        s.t0 = 0;
        s.t1 = 4;
        for (int i = 0; i < 3; i++) {
            ShotKey k;
            k.t = 2.0f * i;
            k.pos = {float(i) * 20, 5, 0};
            k.rel = qconj(frame_quat(Frame{}));
            s.keys.push_back(k);
        }
        Shot h = s;
        h.keys[0].seg = kSegHold;
        auto a = shot_view(h, 0, 1.5f, cars), b = shot_view(h, 0, 2.5f, cars), b0 = shot_view(s, 0, 2.5f, cars);
        CHECK(len(a->eye - V3{0, 5, 0}) < 1e-3f && len(b->eye - b0->eye) < 1e-3f,
              "hold: the camera stays on the key until the next one, the next segment unchanged");
        Shot e = s;
        e.keys[0].seg = kSegEaseInOut;
        float v_start = len(shot_view(e, 0, 0.02f, cars)->eye - shot_view(e, 0, 0.0f, cars)->eye);
        float v_plain = len(shot_view(s, 0, 0.02f, cars)->eye - shot_view(s, 0, 0.0f, cars)->eye);
        CHECK(v_start < v_plain * 0.1f && len(shot_view(e, 0, 1, cars)->eye - shot_view(s, 0, 1, cars)->eye) < 1,
              "ease in-out: slow off the key, through the same path");
        Shot c = s;
        c.keys[0].twist_turns = 2;
        c.keys[0].twist_radius = 3;
        float off_keys = len(shot_view(c, 0, 0, cars)->eye - shot_view(s, 0, 0, cars)->eye) +
                         len(shot_view(c, 0, 2, cars)->eye - shot_view(s, 0, 2, cars)->eye);
        float off_mid = 0;
        for (float t = 0.6f; t < 1.4f; t += 0.05f)
            off_mid = std::max(off_mid, len(shot_view(c, 0, t, cars)->eye - shot_view(s, 0, t, cars)->eye));
        CHECK(off_keys < 1e-3f && off_mid > 5.5f && off_mid < 6.01f, "corkscrew: on the keys, a 3 m spiral between "
              "(widest %.2f m)", off_mid);
        Edit ed;
        ed.shots = {c};
        ed.shots[0].keys[1].seg = kSegEaseOut;
        swofty::json::Error je;
        std::string er;
        Edit back = edit_from_json(swofty::json::parse(edit_to_json(ed), &je), er);
        CHECK(je.ok() && back.shots[0].keys[1].seg == kSegEaseOut && std::fabs(back.shots[0].keys[0].twist_radius - 3) < 1e-3f,
              "timings and corkscrews survive a save");
    }

    // anchors: a key inserted on the path keeps the camera where it was at that moment, bend or not
    {
        cars.t = 0;
        Shot s;
        s.type = ShotType::free_path;
        s.t0 = 0;
        s.t1 = 6;
        for (int i = 0; i < 4; i++) {
            ShotKey k;
            k.t = 2.0f * i;
            k.pos = {float(i) * 20, 5, float(i * i)};
            k.rel = qnorm({0, 0.2f * i, 0, 1});
            k.fov = 50 + 5 * i;
            s.keys.push_back(k);
        }
        s.keys[1].bend = {0, 3, 0};
        auto before = shot_view(s, 0, 3.3f, cars);
        int at = insert_key(s, 3.3f);
        auto after = shot_view(s, 0, 3.3f, cars);
        CHECK(at == 2 && s.keys.size() == 5 && len(after->eye - before->eye) < 1e-3f &&
                  std::fabs(qdot(after->q, before->q)) > 0.99999f && std::fabs(after->fov - before->fov) < 1e-3f &&
                  len(s.keys[1].bend) < 1e-6f,
              "an anchor keeps the view where it was (off %.5f m)", len(after->eye - before->eye));
        CHECK(insert_key(s, 3.32f) == -1 && insert_key(s, 7) == -1, "no anchor next to a key or outside the keys");
    }
    // a world drag through a handle's axes moves the handle by that drag, also on a swung, car-relative shot
    {
        cars.t = 3;
        Shot c;
        c.type = ShotType::chase;
        c.car = 0;
        c.t0 = 0;
        c.t1 = 6;
        c.around = 40;
        for (int i = 0; i < 3; i++) {
            ShotKey k;
            k.t = 1.0f + 2 * i;
            k.pos = {float(i), 2, -8.0f + 3 * i};
            k.rel = qnorm({0, 0.1f * i, 0, 1});
            c.keys.push_back(k);
        }
        PathSample before = sample_path(c, 0, cars);
        const PathHandle &h = before.handles[0];
        V3 drag{1, 0.5f, -2};
        c.keys[size_t(h.key)].bend = h.bend + V3{dot(drag, h.bx), dot(drag, h.by), dot(drag, h.bz)};
        PathSample after = sample_path(c, 0, cars);
        CHECK(len(after.handles[0].at - (h.at + drag)) < 1e-3f, "a drag through the handle's axes moves it by the drag "
              "(off %.5f m)", len(after.handles[0].at - (h.at + drag)));
    }

    // the drawn path is exactly what plays: sample_path (curves built once) against shot_view per time
    {
        cars.t = 2;
        Shot s;
        s.type = ShotType::chase;
        s.car = 0;
        s.t0 = 1;
        s.t1 = 4.5f;
        s.constant_speed = true;
        s.tracks = {{"around", {{1, 0}, {4, 90}}}};
        for (int i = 0; i < 4; i++) {
            ShotKey k;
            k.t = 1 + i;
            k.pos = {float(i), 2.0f + i * 0.3f, -8.0f + i};
            k.rel = qnorm({0, 0.1f * i, 0, 1});
            k.fov = 60;
            k.selected = i == 2;
            s.keys.push_back(k);
        }
        PathSample ps = sample_path(s, 0, cars);
        float worst = 0;
        int n = int(ps.pts.size()) - 1;
        for (int i = 0; i <= n; i++) {
            auto v = shot_view(s, 0, s.t0 + (s.t1 - s.t0) * float(i) / float(n), cars);
            worst = std::max(worst, len(v->eye - ps.pts[size_t(i)]));
        }
        CHECK(n > 16 && worst < 1e-4f && ps.ticks.size() == 4 && ps.keys.size() == 4 && ps.keys[2].selected &&
                  len(ps.keys[1].eye - shot_view(s, 0, 2, cars)->eye) < 1e-4f,
              "the drawn path matches playback (worst %.6f m over %d points)", worst, n + 1);
    }

    // the jerk test: many keys in quick succession, unevenly timed, on a path flown at a steady 30 m/s along a
    // gentle curve. The camera must move at a steady speed with no lurch at any key.
    {
        Shot s;
        s.type = ShotType::free_path;
        s.t0 = 0;
        s.t1 = 4;
        const float times[] = {0, 0.1f, 0.15f, 0.4f, 0.45f, 0.5f, 1.3f, 1.35f, 2.2f, 2.25f, 2.3f, 3.0f, 4.0f};
        Frame w;
        for (float tk : times) {
            ShotKey k;
            k.t = tk;
            float a = tk * 0.2f;  // a curve of radius 150 m at 30 m/s
            k.pos = w.local(V3{150 * std::sin(a), 5, 150 * (1 - std::cos(a))});
            k.rel = qconj(frame_quat(w));  // identity in the world
            s.keys.push_back(k);
        }
        cars.t = 0;
        float worst = 0, vmin = 1e9f, vmax = 0, pv = -1;
        V3 prev{};
        for (int i = 1; i <= 240; i++) {
            float t = i / 60.0f;
            auto v = shot_view(s, 0, t, cars);
            if (i > 1) {
                float sp = len(v->eye - prev) * 60;
                vmin = std::min(vmin, sp);
                vmax = std::max(vmax, sp);
                if (pv >= 0)
                    worst = std::max(worst, std::fabs(sp - pv));
                pv = sp;
            }
            prev = v->eye;
        }
        CHECK(worst < 0.5f && vmin > 28 && vmax < 32,
              "uneven keys on a steady path: speed %.2f..%.2f m/s, largest frame-to-frame change %.3f m/s", vmin, vmax,
              worst);
        std::printf("jerk test: speed %.2f..%.2f m/s, largest change %.3f m/s per frame\n", vmin, vmax, worst);
    }

    // a natural spline: speed carries through keys even when they are unevenly spaced in space and time
    {
        Shot s;
        s.t0 = 0;
        s.t1 = 3;
        Frame w;
        V3 pts[] = {{0, 5, 0}, {4, 5, 1}, {6, 6, 6}, {20, 5, 10}, {22, 5, 30}};
        float tt[] = {0, 0.3f, 0.6f, 1.8f, 3};
        for (int i = 0; i < 5; i++) {
            ShotKey k;
            k.t = tt[i];
            k.pos = w.local(pts[i]);
            k.rel = qconj(frame_quat(w));
            s.keys.push_back(k);
        }
        float worst_ratio = 1;
        for (float kt : {0.3f, 0.6f, 1.8f}) {
            auto a = shot_view(s, 0, kt - 0.01f, cars), b = shot_view(s, 0, kt, cars),
                 c = shot_view(s, 0, kt + 0.01f, cars);
            float v0 = len(b->eye - a->eye), v1 = len(c->eye - b->eye);
            worst_ratio = std::max(worst_ratio, std::max(v0, v1) / std::max(1e-6f, std::min(v0, v1)));
        }
        CHECK(worst_ratio < 1.05f, "speed is continuous through each key (worst ratio %.3f)", worst_ratio);
        // constant speed: even spacing along the path whatever the key times
        s.constant_speed = true;
        float smin = 1e9f, smax = 0;
        V3 prev = shot_view(s, 0, 0, cars)->eye;
        for (int i = 1; i <= 180; i++) {
            V3 e = shot_view(s, 0, i / 60.0f, cars)->eye;
            float d = len(e - prev);
            smin = std::min(smin, d);
            smax = std::max(smax, d);
            prev = e;
        }
        CHECK(smax / smin < 1.05f, "constant speed: even steps along the path (%.4f..%.4f m)", smin, smax);
    }

    // shot types around a moving car
    {
        cars.t = 5;
        auto car = *cars.frame(0);
        Edit e;
        Shot on;
        on.type = ShotType::onboard;
        on.car = 0;
        on.mount = "cockpit";
        on.t0 = 0;
        on.t1 = 10;
        Shot tr;
        tr.type = ShotType::track;
        tr.car = 0;
        tr.point = {0, 20, 0};
        tr.t0 = 10;
        tr.t1 = 20;
        Shot ob;
        ob.type = ShotType::orbit;
        ob.car = 0;
        ob.t0 = 20;
        ob.t1 = 30;
        ob.radius = 8;
        ob.height = 2;
        Shot ch;
        ch.type = ShotType::chase;
        ch.car = 0;
        ch.t0 = 30;
        ch.t1 = 40;
        e.shots = {on, tr, ob, ch};
        auto v = evaluate_edit(e, 5, cars);
        const Mount *m = find_mount("cockpit");
        V3 r_, u_, b_;
        cols_from_quat(v->q, r_, u_, b_);
        CHECK(v && v->shot == 0 && len(v->eye - car.at(m->offset)) < 1e-3f && dot(b_ * -1, car.f) > 0.99f,
              "onboard sits at the mount and looks along the car");
        cars.t = 15;
        car = *cars.frame(0);
        v = evaluate_edit(e, 15, cars);
        cols_from_quat(v->q, r_, u_, b_);
        CHECK(v && v->shot == 1 && len(v->eye - V3{0, 20, 0}) < 1e-3f &&
                  dot(b_ * -1, norm(car.at(kCarCentre) - v->eye)) > 0.9999f,
              "track cam stands still and looks at the car");
        cars.t = 25;
        car = *cars.frame(0);
        v = evaluate_edit(e, 25, cars);
        V3 centre = car.at({0, 0, kCarCentre.z});
        V3 d = v->eye - centre;
        CHECK(v && v->shot == 2 && std::fabs(std::sqrt(d.x * d.x + d.z * d.z) - 8) < 0.05f && std::fabs(d.y - 2) < 0.05f,
              "orbit keeps its radius and height around the car");
        {
            Shot o1 = ob, o2 = ob;
            o2.reverse = true;
            Frame lf2 = level_frame(car);
            auto a1 = shot_view(o1, 2, 22, cars), a2 = shot_view(o2, 2, 22, cars);
            float s1 = dot(a1->eye - centre, lf2.r), s2 = dot(a2->eye - centre, lf2.r);
            CHECK(s1 * s2 < 0 && std::fabs(s1 + s2) < 0.05f, "a reversed orbit turns the other way (%.2f vs %.2f)", s1, s2);
        }
        cars.t = 35;
        car = *cars.frame(0);
        v = evaluate_edit(e, 35, cars);
        Frame lf = level_frame(car);
        CHECK(v && v->shot == 3 && dot(v->eye - lf.p, lf.f) < -5 && v->eye.y > car.p.y + 1,
              "chase sits behind and above the car");
        CHECK(!evaluate_edit(e, 45, cars), "after the last shot: the game's camera");
        // an onboard shot with keys: the key made from a view plays back that view, riding the car
        Shot ok = on;
        cars.t = 3;
        car = *cars.frame(0);
        V3 eye = car.at({1, 1, -2});
        Quat q = look_quat(car, 20, -5, 3);
        auto key = key_from_view(ok, 3, eye, q, 55, cars);
        ok.keys = {*key};
        cars.t = 7;  // the car moved on
        auto car7 = *cars.frame(0);
        auto kv = shot_view(ok, 0, 7, cars);
        V3 r2, u2, b2, r3, u3, b3;
        cols_from_quat(kv->q, r2, u2, b2);
        cols_from_quat(look_quat(car7, 20, -5, 3), r3, u3, b3);
        CHECK(kv && len(kv->eye - car7.at({1, 1, -2})) < 1e-3f && dot(b2, b3) > 0.9999f && std::fabs(kv->fov - 55) < 1e-3f,
              "a key on an onboard shot rides the car with the same view");
    }

    // sliders on keyed shots: roll / around / nudge adjust the key path, and agree with the unkeyed chase
    {
        cars.t = 4;
        Shot ch;
        ch.type = ShotType::chase;
        ch.car = 0;
        ch.t0 = 0;
        ch.t1 = 10;
        auto base = shot_view(ch, 0, 4, cars);  // the unkeyed preset view
        Shot kd = ch;
        kd.keys = {*key_from_view(kd, 4, base->eye, base->q, base->fov, cars)};
        auto same = shot_view(kd, 0, 4, cars);
        CHECK(len(same->eye - base->eye) < 1e-3f && std::fabs(qdot(same->q, base->q)) > 0.99999f,
              "a key from the chase preset plays the preset view");
        ch.roll = kd.roll = 15;
        ch.around = kd.around = 70;
        ch.yaw = kd.yaw = 12;
        ch.pitch = kd.pitch = -7;
        auto u = shot_view(ch, 0, 4, cars), k = shot_view(kd, 0, 4, cars);
        CHECK(len(u->eye - k->eye) < 1e-3f && std::fabs(qdot(u->q, k->q)) > 0.9999f,
              "roll and around do the same on keyed and unkeyed chase (eye off %.4f, q dot %.6f)", len(u->eye - k->eye),
              std::fabs(qdot(u->q, k->q)));
        CHECK(len(k->eye - same->eye) > 1, "around moves a keyed chase camera");
        // a key made with adjustments on plays back the view it was made from
        kd.nudge = {0.3f, -0.2f, 1.1f};
        cars.t = 6;
        V3 eye = cars.frame(0)->at({2, 3, -9});
        Quat q = qnorm({0.1f, 0.7f, 0.05f, 0.7f});
        kd.keys = {*key_from_view(kd, 6, eye, q, 50, cars)};
        auto back = shot_view(kd, 0, 6, cars);
        CHECK(len(back->eye - eye) < 1e-3f && std::fabs(qdot(back->q, q)) > 0.99999f,
              "a key made with nudge / around / roll on plays back its view");
    }

    // keyframed slider values (tracks)
    {
        cars.t = 3;
        Shot ch;
        ch.type = ShotType::chase;
        ch.car = 0;
        ch.t0 = 0;
        ch.t1 = 10;
        Shot an = ch;
        an.tracks = {{"roll", {{2, 0}, {4, 30}}}, {"around", {{2, 0}, {4, 60}}}};
        Shot fixed = ch;
        fixed.roll = 15;
        fixed.around = 30;
        auto a = shot_view(an, 0, 3, cars), f = shot_view(fixed, 0, 3, cars);
        CHECK(len(a->eye - f->eye) < 1e-3f && std::fabs(qdot(a->q, f->q)) > 0.9999f,
              "a roll / around track halfway between its keys plays the halfway values");
        auto before = shot_view(an, 0, 1, cars), plain = shot_view(ch, 0, 1, cars);
        CHECK(len(before->eye - plain->eye) < 1e-3f, "before a track's first key: its first value");
        // keys made under an animated adjustment play back their view
        Shot kd = an;
        V3 eye = cars.frame(0)->at({1, 2, -8});
        Quat q = qnorm({0.05f, 0.6f, 0.1f, 0.8f});
        kd.keys = {*key_from_view(kd, 3, eye, q, 55, cars)};
        auto back = shot_view(kd, 0, 3, cars);
        CHECK(len(back->eye - eye) < 1e-3f && std::fabs(qdot(back->q, q)) > 0.99999f,
              "a key made under animated values plays back its view");
        // speed animates too, and tracks survive a save
        Edit e;
        an.tracks.push_back({"speed", {{1, 1}, {5, 0.25f}}});
        e.shots = {an};
        CHECK(std::fabs(shot_view(an, 0, 6, cars)->speed - 0.25f) < 1e-4f, "a speed track sets the replay speed");
        swofty::json::Error e6;
        std::string er;
        Edit back2 = edit_from_json(swofty::json::parse(edit_to_json(e), &e6), er);
        CHECK(e6.ok() && back2.shots.size() == 1 && back2.shots[0].tracks.size() == 3 &&
                  back2.shots[0].tracks[0].param == "roll" && back2.shots[0].tracks[0].keys.size() == 2 &&
                  std::fabs(back2.shots[0].tracks[0].keys[1].v - 30) < 1e-3f,
              "tracks survive a save");
    }

    // splitting a shot keeps the camera where it was at the cut
    {
        Shot s;
        s.type = ShotType::free_path;
        s.t0 = 0;
        s.t1 = 6;
        Frame w;
        V3 pts[] = {{0, 5, 0}, {10, 6, 3}, {25, 4, 10}, {40, 8, 30}};
        float tt[] = {0, 2, 4, 6};
        for (int i = 0; i < 4; i++) {
            ShotKey k;
            k.t = tt[i];
            k.pos = pts[i];
            k.rel = qnorm({0.0f, std::sin(i * 0.2f), 0.0f, std::cos(i * 0.2f)});
            k.fov = 50 + i * 5;
            s.keys.push_back(k);
        }
        s.tracks = {{"roll", {{1, 0}, {5, 40}}}};
        Edit e;
        e.shots = {s};
        auto before = shot_view(s, 0, 3, cars);
        CHECK(split_shot(e, 0, 3) && e.shots.size() == 2 && std::fabs(e.shots[0].t1 - 3) < 1e-4f &&
                  std::fabs(e.shots[1].t0 - 3) < 1e-4f,
              "split makes two shots that meet at the cut");
        auto first = shot_view(e.shots[0], 0, 3, cars), second = shot_view(e.shots[1], 1, 3, cars);
        CHECK(len(first->eye - before->eye) < 1e-3f && len(second->eye - before->eye) < 1e-3f &&
                  std::fabs(qdot(second->q, before->q)) > 0.99999f && std::fabs(second->fov - before->fov) < 1e-3f,
              "both halves are where the camera was at the cut");
        CHECK(e.shots[0].keys.size() == 3 && e.shots[1].keys.size() == 3 && e.shots[0].tracks.size() == 1 &&
                  std::fabs(track_at(e.shots[1].tracks[0], 3) - track_at(s.tracks[0], 3)) < 1e-3f,
              "keys and value keys go to their side, with a key at the cut on both");
        CHECK(!split_shot(e, 0, 0.1f), "no split within 0.25 s of an end");
    }

    // JSON round trip and conversion from version 1
    {
        Edit e;
        e.name = "t";
        Shot a;
        a.type = ShotType::onboard;
        a.car = 21;
        a.driver = "BOTTAS";
        a.mount = "tcam";
        a.t0 = 1;
        a.t1 = 4;
        a.speed = 0.5f;
        a.ease_in = true;
        Shot b;
        b.type = ShotType::free_path;
        b.t0 = 4;
        b.t1 = 9;
        b.constant_speed = true;
        ShotKey k;
        k.t = 5;
        k.pos = {1, 2, 3};
        k.rel = qnorm({0.1f, 0.2f, 0.3f, 0.9f});
        k.fov = 44;
        b.keys = {k};
        e.shots = {a, b};
        std::string txt = edit_to_json(e);
        swofty::json::Error e5;
        std::string er;
        Edit back = edit_from_json(swofty::json::parse(txt, &e5), er);
        CHECK(e5.ok() && er.empty() && back.shots.size() == 2 && back.shots[0].type == ShotType::onboard &&
                  back.shots[0].driver == "BOTTAS" && back.shots[0].mount == "tcam" && back.shots[0].ease_in &&
                  std::fabs(back.shots[0].speed - 0.5f) < 1e-4f && back.shots[1].constant_speed &&
                  back.shots[1].keys.size() == 1 && len(back.shots[1].keys[0].pos - V3{1, 2, 3}) < 1e-3f &&
                  std::fabs(back.shots[1].keys[0].rel.w - k.rel.w) < 1e-4f,
              "shots survive a save");
        const char *v1 = R"({ "name": "old", "keys": [
          { "t": 1, "pos": { "anchor": "world", "p": [0, 5, 0] }, "look": { "type": "dir", "yaw": 10 } },
          { "t": 3, "end": true, "pos": { "anchor": "world", "p": [10, 5, 0] }, "look": { "type": "dir" } },
          { "t": 3.02, "pos": { "anchor": "car", "car": 21, "offset": [0, 0.8, -3] }, "look": { "type": "dir" } },
          { "t": 6, "pos": { "anchor": "car", "car": 21, "offset": [0, 0.8, -3] }, "look": { "type": "dir" } } ] })";
        er.clear();
        Edit conv = edit_from_json(swofty::json::parse(v1, &e5), er);
        CHECK(conv.shots.size() == 2 && conv.shots[0].type == ShotType::free_path && conv.shots[0].keys.size() == 2 &&
                  conv.shots[1].type == ShotType::onboard && conv.shots[1].car == 21 &&
                  std::fabs(conv.shots[1].t0 - 3.02f) < 1e-3f,
              "version 1 converts: a world path, then an onboard shot");

        // the replay an edit belongs to: saved, read back, compared; an edit without one isn't "the same"
        Edit tagged = e;
        tagged.replay.length = 1338.76f;
        tagged.replay.drivers = "NORRIS,PIASTRI,BOTTAS";
        er.clear();
        Edit tb = edit_from_json(swofty::json::parse(edit_to_json(tagged), &e5), er);
        ReplayId other = tagged.replay;
        other.length += 1;
        CHECK(e5.ok() && tb.replay.same(tagged.replay) && !tb.replay.same(other) && !back.replay.known() &&
                  !back.replay.same(tagged.replay),
              "an edit keeps its replay; another replay or none doesn't match");
        ReplayId fa = tagged.replay, fb = tagged.replay, nofile = tagged.replay;
        fa.file = "aa11";
        fb.file = "bb22";
        tagged.replay.file = "aa11";
        er.clear();
        Edit tf = edit_from_json(swofty::json::parse(edit_to_json(tagged), &e5), er);
        CHECK(tf.replay.file == "aa11" && tf.replay.same(fa) && !tf.replay.same(fb) && tf.replay.same(nofile),
              "replay files: the same file matches, another file doesn't (even with the same length and drivers), "
              "no file falls back to length and drivers");
    }

    // keyed slider values: a curved segment eases out of one key and into the next; old keys stay a spline
    {
        Track tr{"around", {}};
        tr.keys.push_back({0, -30, true, curve_preset(0)});
        tr.keys.push_back({1, 10, true, curve_preset(0)});
        float a = track_at(tr, 0.05f) - track_at(tr, 0), mid = track_at(tr, 0.5f), b = track_at(tr, 1) - track_at(tr, 0.95f);
        Track old{"around", {{0, -30, false, {}}, {1, 10, false, {}}}};
        float la = track_at(old, 0.05f) - track_at(old, 0);
        CHECK(std::fabs(mid + 10) < 1e-3f && a < la * 0.2f && b < la * 0.2f && std::fabs(la - 2) < 1e-3f,
              "slider keys: smooth segments start and stop gently (old keys: linear, unchanged)");
        Shot sh;
        sh.type = ShotType::chase;
        sh.tracks.push_back(tr);
        Edit ce;
        ce.shots = {sh};
        std::string cerr;
        swofty::json::Error cj;
        Edit cb = edit_from_json(swofty::json::parse(edit_to_json(ce), &cj), cerr);
        CHECK(cj.ok() && cb.shots.size() == 1 && cb.shots[0].tracks.size() == 1 &&
                  cb.shots[0].tracks[0].keys[0].curved &&
                  std::fabs(cb.shots[0].tracks[0].keys[0].curve.pts[0].ox - 0.42f) < 1e-3f,
              "slider keys: their curves survive a save");
    }

    // transition: an overhead orbit on car 0, then a gap filled by a transition, then a cockpit onboard on car 0
    {
        Edit ed;
        Shot top;
        top.type = ShotType::orbit;
        top.car = 0;
        top.t0 = 0;
        top.t1 = 4;
        top.radius = 2;
        top.height = 30;
        Shot tx;
        tx.type = ShotType::transition;
        tx.t0 = 4;
        tx.t1 = 6;
        for (auto &cv : tx.curves)  // linear, for the middle check
            cv = curve_preset(1);
        Shot ob;
        ob.type = ShotType::onboard;
        ob.car = 0;
        ob.mount = "cockpit";
        ob.t0 = 6;
        ob.t1 = 10;
        ed.shots = {top, tx, ob};
        auto at = [&](float t) {
            cars.t = t;
            return evaluate_edit(ed, t, cars);
        };
        auto from = [&](const Shot &s, int i, float t) {
            cars.t = t;
            return shot_view(s, i, t, cars);
        };
        auto t0 = at(4), t1 = at(6 - 1e-4f), mid = at(5);
        auto a0 = from(top, 0, 4), b1 = from(ob, 2, 6 - 1e-4f), a5 = from(top, 0, 5), b5 = from(ob, 2, 5);
        CHECK(t0 && t1 && mid && a0 && b1 && a5 && b5 && len(t0->eye - a0->eye) < 1e-3f &&
                  len(t1->eye - b1->eye) < 0.05f && len(mid->eye - (a5->eye + b5->eye) * 0.5f) < 1e-3f &&
                  t0->shot == 1,
              "transition: starts on the shot before, ends on the shot after, both followed at the same moment");
        auto fwd = [](Quat q) {  // the view looks along local -Z
            V3 r, u, back;
            cols_from_quat(q, r, u, back);
            return back * -1.0f;
        };
        // overhead -> cockpit: the gaze turns smoothly (no swing), from down at the car to ahead of it
        float worst = 0;
        for (float t = 4; t < 6 - 0.05f; t += 0.05f) {
            auto v0 = at(t), v1 = at(t + 0.05f);
            worst = std::max(worst, std::acos(std::clamp(dot(fwd(v0->q), fwd(v1->q)), -1.0f, 1.0f)) / kDeg);
        }
        CHECK(worst < 6, "transition: overhead to cockpit turns smoothly (largest turn per 0.05 s under 6 deg)");
        // per axis, in the car's level frame: Forward done early, Height late -> at a quarter of the way, the camera
        // has covered most of its forward distance to the car but has hardly come down
        auto local = [&](float t) {
            auto v = at(t);
            return level_frame(*cars.frame(0)).local(v->eye);
        };
        V3 start = local(4), end = local(6 - 1e-4f);
        ed.shots[1].curves[kAxForward] = {0, 0.9f, 0.2f, 1};  // fast at first
        ed.shots[1].curves[kAxHeight] = {0.8f, 0, 1, 0.1f};   // late
        V3 q = local(4.5f);
        float fz = (q.z - start.z) / (end.z - start.z), fy = (q.y - start.y) / (end.y - start.y);
        CHECK(fz > 0.7f && fy < 0.1f, "transition: each axis follows its own curve (forward early, height late)");
        Curve lin = curve_preset(1), smooth = curve_preset(0);
        CHECK(std::fabs(lin.at(0.3f) - 0.3f) < 1e-3f && smooth.at(0.25f) < 0.25f && smooth.at(0.75f) > 0.75f &&
                  std::fabs(smooth.at(0.5f) - 0.5f) < 1e-3f && std::fabs(smooth.at(1) - 1) < 1e-4f,
              "curves: linear is the identity, smooth starts slow and ends slow");
        // the curves survive a save
        std::string err2;
        swofty::json::Error je2;
        Edit back = edit_from_json(swofty::json::parse(edit_to_json(ed), &je2), err2);
        const Curve &bc = back.shots[1].curves[kAxHeight];
        CHECK(je2.ok() && back.shots[1].type == ShotType::transition && std::fabs(bc.pts[0].ox - 0.8f) < 1e-3f &&
                  std::fabs(bc.pts[1].iy - 0.1f) < 1e-3f,
              "transition: its type and curves survive a save");
        // anchors: one added on a curve leaves its shape; moved, the curve goes through it, below 0 if asked
        Curve ac = curve_preset(0);
        float before = ac.at(0.3f), before2 = ac.at(0.7f);
        ac.add_anchor(0.5f);
        bool same = ac.pts.size() == 3 && std::fabs(ac.at(0.3f) - before) < 0.02f && std::fabs(ac.at(0.7f) - before2) < 0.02f;
        float dy = -0.4f - ac.pts[1].y;  // drag the anchor (and its handles) down to -0.4
        ac.pts[1].y += dy, ac.pts[1].iy += dy, ac.pts[1].oy += dy;
        CHECK(same && std::fabs(ac.at(0.5f) + 0.4f) < 1e-3f && std::fabs(ac.at(0) ) < 1e-4f && std::fabs(ac.at(1) - 1) < 1e-4f,
              "curves: an added anchor keeps the shape; dragged, the curve passes through it (here below 0)");
        // a key's position before it was first dragged survives a save (for "Reset position")
        Edit oe;
        Shot fp;
        fp.type = ShotType::free_path;
        ShotKey ok1;
        ok1.pos = {1, 2, 3};
        ok1.orig = {4, 5, 6};
        ok1.has_orig = true;
        ShotKey ok2;
        ok2.t = 2;
        fp.keys = {ok1, ok2};
        oe.shots = {fp};
        std::string oerr;
        swofty::json::Error oj;
        Edit oback = edit_from_json(swofty::json::parse(edit_to_json(oe), &oj), oerr);
        CHECK(oj.ok() && oback.shots.size() == 1 && oback.shots[0].keys.size() == 2 && oback.shots[0].keys[0].has_orig &&
                  len(oback.shots[0].keys[0].orig - V3{4, 5, 6}) < 1e-3f && !oback.shots[0].keys[1].has_orig,
              "keys: the position to reset to survives a save");
        Curve rt;
        swofty::json::Error je3;
        CHECK(curve_from_json(swofty::json::parse(curve_to_json(ac), &je3), rt) && rt.pts.size() == 3 &&
                  std::fabs(rt.at(0.5f) + 0.4f) < 1e-3f,
              "curves: anchors survive a save");
    }

