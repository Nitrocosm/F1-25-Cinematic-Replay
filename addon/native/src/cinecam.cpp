// Cinematic Replay (cinecam.dll): keyframed camera sequences in F1 25 replays.
//
// During a replay, replaces the view camera with our own: a single car-anchored shot (mode 1) or a
// keyframed sequence on the replay timeline (mode 2, see sequence.hpp), with replay speed ramps.
//
// How (addresses are for one game build, found by static analysis and checked live):
// - ViewportManager::Update (vtable slot 9) turns the per-viewport camera state into the render
//   proxy messages every frame. We take that vtable slot and, before the game's code runs, write our
//   camera into viewport 0's state (entry + 0x20): eye, focus point, quaternion (x, y, z, w), near,
//   lens width/height, cut flag. Culling, LOD, shadows, DOF and TAA all derive from it (L3).
// - The followed car comes from the active camera's target object: T + 0x90 is its world matrix, rows
//   (left, up, forward, translation) (L3, settled 2026-11-03 with a camera 12 m along +row2 = ahead).
// - The camera quaternion is (x, y, z, w) of the basis local x = right, y = up, z = BACKWARD: the
//   game's camera looks along its local -Z (L3). Handedness: forward x up = right.
// - Replay (GameReplaySystem RS): state +0x20, mode +0x3c, time [**(RS+0x98)] + 0xd8, seek +0x7c/+0x7a,
//   base speed multiplier +0x24 (L3).
//
// Files live in %LOCALAPPDATA%\Swofty\cinematic-replay\ (outside the game folder, so editing them never
// trips Swofty's install journal):
//   cinecam.cfg            live settings, re-read when it changes (copied from the addon's on first run)
//   sequences\<name>.json  sequences (mode 2 plays `sequence = <name>`)
//   sequences\_captured.json  keys captured with `capture` (see below)
//
// One game build: the globals and slots below are fixed RVAs of build d873a1c8 (Steam build 25251167), only
// trusted when ViewportManager::Update's signature sits at its known RVA (= that build); otherwise the plugin
// stays off. The vtable slots are taken directly (not Swofty bindings).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shellapi.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <swofty/json.hpp>
#include <swofty/mem.hpp>
#include <swofty/mixin.hpp>
#include <swofty/events.hpp>
#include <swofty/overlay_ui.hpp>

#include "shots.hpp"
#include "render.hpp"
#include "audio_capture.hpp"

// The per-car audio update (0x1409fa9f0, vtable slot base+0x5666da0 on d873a1c8) is caught by a stub that only
// remembers `this` and jumps on, so its arguments (unknown, maybe floats in xmm) pass through untouched. The
// camera thread then finds the pass-by system from that object (see passby_tick).
extern "C" {
void *g_audio_this;
void *g_audio_orig;
__attribute__((naked)) void audio_update_stub()
{
    asm volatile("movq %rcx, g_audio_this(%rip)\n\t"
                 "jmpq *g_audio_orig(%rip)");
}

// The ReplayDirector's system update (0x142c08360, vtable 0x1457ce670 slot 25; arguments unknown): this stub calls
// director_tick(this) on the director's own thread, keeping every argument register (rcx, rdx, r8, r9, xmm0-3),
// then jumps to the original.
void *g_director_orig;
void director_tick(void *director);
__attribute__((naked)) void director_update_stub()
{
    asm volatile("pushq %rcx\n\t"
                 "pushq %rdx\n\t"
                 "pushq %r8\n\t"
                 "pushq %r9\n\t"
                 "subq $0x68, %rsp\n\t"  // 32 shadow + 64 for xmm0-3; rsp 16-aligned for the call
                 "movdqu %xmm0, 0x20(%rsp)\n\t"
                 "movdqu %xmm1, 0x30(%rsp)\n\t"
                 "movdqu %xmm2, 0x40(%rsp)\n\t"
                 "movdqu %xmm3, 0x50(%rsp)\n\t"
                 "callq director_tick\n\t"
                 "movdqu 0x20(%rsp), %xmm0\n\t"
                 "movdqu 0x30(%rsp), %xmm1\n\t"
                 "movdqu 0x40(%rsp), %xmm2\n\t"
                 "movdqu 0x50(%rsp), %xmm3\n\t"
                 "addq $0x68, %rsp\n\t"
                 "popq %r9\n\t"
                 "popq %r8\n\t"
                 "popq %rdx\n\t"
                 "popq %rcx\n\t"
                 "jmpq *g_director_orig(%rip)");
}
}

namespace {

namespace mem = swofty::mem;
using cine::V3;

// ViewportManager::Update 0x14440a4c0 (unique in .xtls on d873a1c8)
constexpr const char *kVpmUpdatePattern =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 38 01 00 00 C5 78 29 BC 24 20 01 00 00 C5 78 29 B4 24 10 01 00 00 "
    "C5 78 29 AC 24 00 01 00 00 C5 78 29 A4 24 F0 00 00 00 C5 78 29 9C 24 E0 00 00 00 C5 78 29 94 24 D0 00 00 00 "
    "C5 79 7F 8C 24 C0 00 00 00"; // the first 37 bytes alone match 13 functions
constexpr uintptr_t kVpmUpdateRva = 0x440a4c0;
constexpr int kVpmUpdateSlot = 9;
// globals of build d873a1c8
constexpr uintptr_t kReplaySystemRva = 0x6ce5178;  // GameReplaySystem*
constexpr uintptr_t kGameManagerRva = 0x604c2c8;   // g_Game_Manager
constexpr uintptr_t kVehicleCountRva = 0x6ce86dc;  // g_Sess_VehicleCount (u8)
constexpr uintptr_t kVehicleTableRva = 0x6ce86e0;  // g_Sess_VehicleTable[N]
constexpr uintptr_t kVtCarPhysics2 = 0x5bf79b0, kVtSafetyCar = 0x58552f0, kVtVehicle = 0x5bf73a0;
// Trackside pass-by sounds (checked live): the per-car
// audio update reaches the audio manager as *(*(this+0xdf0)+0xa0); the manager's pass-by system is at +0xc10
// (it points back: *(sys+0x48)+0xc10 == sys) and keeps Audio.passbySystem_enabled in byte +1.
constexpr uintptr_t kCarAudioUpdateRva = 0x9fa9f0;
// AudioCameraListener: vtable 0x145a8a230. Slot 17 (0x1443ea6a0,
// (listener, ctx)) runs inside Camera::Update, reads the camera's working state ctx[2]: eye at +0x120, quat at
// +0x140, builds the listener matrix and hands it to the audio manager right away. We wrap that slot and swap our
// view into the working state for the call.
constexpr uintptr_t kListenerUpdateSlotRva = 0x5a8a2b8, kListenerUpdateRva = 0x43ea6a0;
// The replay's own camera follows the car of the shot on screen, so the game's sound (listener and mix) is that
// car's: ReplayDirector update slot 25, and "switch to car N"
// 0x1406ae650(helper = director+0x70, u8 N, u8 mode 1 = car camera, cam 0). helper+0x14 = focus car, +0x15 = mode.
constexpr uintptr_t kDirectorUpdateSlotRva = 0x57ce738, kDirectorUpdateRva = 0x2c08360, kSwitchCarRva = 0x6ae650;

constexpr size_t kCam = 0x20; // camera state inside a viewport entry

using Update_t = uint64_t (*)(void *, void *, void *, void *);
Update_t g_orig;
void **g_slot;
uintptr_t g_base;
bool g_globals_ok;
std::string g_dir;       // %LOCALAPPDATA%\Swofty\cinematic-replay
std::string g_addon_dir; // the addon's folder in the game

template <class T> bool rd(uintptr_t at, T &out) { return at && mem::read(at, out); }

// ----- config ----------------------------------------------------------------------------------

struct Config {
    int mode = 0;                 // outside the editor: 0 the game's own cameras, 1 single shot, 2 play the edit
    bool in_photo_mode = false;   // also override while photo mode is open
    // mode 1: the shot, in the followed car's frame (right, up, forward), degrees
    float ox = -1.1f, oy = 0.25f, oz = -1.6f, yaw = 0, pitch = -3, roll = 0, fov = 70;
    float near_ = 0.05f, focus = 6;
    std::string sequence;         // mode 2: sequences\<name>.json
    float restart = 0;            // changed: seek to the sequence's first key
    float capture = 0;            // changed: append the current camera as a key to sequences\_captured.json
    float seek = -1;              // >= 0 and changed: seek there
    float select_shot = -1;       // changed (>= 0): select shot N in the editor (for tests without a mouse)
    float dump_tracks = 0;        // changed: write the recorded car positions to tracks.csv (planning shots offline)
    float base_speed = 0;         // != 0: replay base speed multiplier (RS+0x24); 0 = leave it to the sequence / 1.0
    float list_cars = 0;          // changed: log every car slot with its distance to the followed car
    float freecam = 0;            // changed: open / close the editor (like F9)
    bool recorded_passby = true;  // keep the game's recorded trackside pass-by sounds in replays (0: live engine,
                                  // out of sync with our cameras until the listener follows them)
    bool move_listener = true;    // hear the replay from our camera (the game's audio listener follows it)
    bool sound_follows_shot = true; // keep the replay's own camera in car-camera mode (live engines, no TV pass-bys)
    bool editor_hides_hud = true;   // the game's replay HUD is hidden while the editor is open
    int key_curve = 0;              // the curve new slider keys get: 0 smooth, 1 linear (Options)
    bool check_clipping = true;     // the 3D path shows where it goes into the track or a barrier (Options)
    float final_cut = 0;          // changed: start Final Cut (like its button)
    float render = 0;             // changed: start a render (like its button)
    float speed_trace = 0;        // changed: log the followed car's speed 5x a second for the next 120 s of replay
    float letterbox = 0;          // Final Cut: bars to this aspect ratio (e.g. 2.39); 0 = none
    float fade = 0;               // Final Cut only (never in a render; fades are done in post): fade from / to black
    float preroll = 1.0f;         // Final Cut: start this long before the first shot (black), so the game settles
    int render_fps = 60;          // Render: output frames per second
    int render_settle = 12;       // Render: rendered frames each output frame is held for before it is captured
    int render_crf = 16;          // Render: x264 quality (lower = better, bigger)
    int render_format = 0;        // Render: 0 H.264 (.mp4), 1 ProRes 422 HQ (.mov), 2 ProRes 4444 XQ (.mov), 3 H.265 (.mp4)
    bool render_audio = true;     // Render: a real-time sound pass after the video, joined into the file
    int render_blur = 1;          // Render: motion blur samples per frame (1 = off; 4, 8, 16)
    bool render_screen_capture = false; // Render: capture the screen instead of the window (the game must stay in front)
    float render_shutter = 180;   // Render: motion blur shutter angle in degrees (180 = half the frame time)
    float render_audio_wait = 3;  // Render: seconds the sound pass holds at its start (loading) before it records
    float render_keep_parts = 0;  // Render: 1 keeps the video-only file, the raw sound and the sync trace too
    float render_audio_offset = 0;     // Render: seconds to shift the sound (+ = earlier). The 450 ms once needed
                                       // was the video starting 0.5 s late (the stepped seek), fixed in 0.15.4
    std::string render_dir;       // Render: output folder (empty: Videos\Cinematic Replay)
    std::string ffmpeg;           // Render: ffmpeg.exe (empty: PATH or WinGet)
    int log_every = 600;          // frames between diagnostic lines; 0 = off
};
Config g_cfg;
FILETIME g_cfg_time{};
bool g_cfg_first = true;
float g_last_seek = -1, g_last_restart = 0, g_last_capture = 0, g_last_list = 0, g_last_freecam = 0;
float g_last_final = 0, g_last_render = 0, g_last_trace = 0;

std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool file_time(const std::string &path, FILETIME &out)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a))
        return false;
    out = a.ftLastWriteTime;
    return true;
}

bool read_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

void load_config()
{
    std::string path = g_dir + "\\cinecam.cfg";
    FILETIME ft{};
    if (!file_time(path, ft))
        return;
    if (!g_cfg_first && CompareFileTime(&ft, &g_cfg_time) == 0)
        return;
    g_cfg_time = ft;
    std::string text;
    if (!read_file(path, text))
        return;
    Config c;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        line = trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
            continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        float f = float(atof(v.c_str()));
        if (k == "mode") c.mode = int(f);
        else if (k == "enabled") c.mode = f != 0 ? std::max(c.mode, 1) : 0;  // the PoC's key
        else if (k == "in_photo_mode") c.in_photo_mode = f != 0;
        else if (k == "ox") c.ox = f;
        else if (k == "oy") c.oy = f;
        else if (k == "oz") c.oz = f;
        else if (k == "yaw") c.yaw = f;
        else if (k == "pitch") c.pitch = f;
        else if (k == "roll") c.roll = f;
        else if (k == "fov") c.fov = f;
        else if (k == "near") c.near_ = f;
        else if (k == "focus") c.focus = f;
        else if (k == "sequence") c.sequence = v;
        else if (k == "restart") c.restart = f;
        else if (k == "capture") c.capture = f;
        else if (k == "seek") c.seek = f;
        else if (k == "base_speed") c.base_speed = f;
        else if (k == "log_every") c.log_every = int(f);
        else if (k == "list_cars") c.list_cars = f;
        else if (k == "freecam") c.freecam = f;
        else if (k == "recorded_passby") c.recorded_passby = f != 0;
        else if (k == "move_listener") c.move_listener = f != 0;
        else if (k == "sound_follows_shot") c.sound_follows_shot = f != 0;
        else if (k == "editor_hides_hud") c.editor_hides_hud = f != 0;
        else if (k == "key_curve") c.key_curve = f >= 0.5f ? 1 : 0;
        else if (k == "check_clipping") c.check_clipping = f != 0;
        else if (k == "select_shot") c.select_shot = f;
        else if (k == "dump_tracks") c.dump_tracks = f;
        else if (k == "final_cut") c.final_cut = f;
        else if (k == "render") c.render = f;
        else if (k == "speed_trace") c.speed_trace = f;
        else if (k == "letterbox") c.letterbox = f;
        else if (k == "fade") c.fade = std::max(0.0f, f);
        else if (k == "preroll") c.preroll = std::max(0.0f, f);
        else if (k == "render_fps") c.render_fps = std::clamp(int(f), 10, 240);
        // at least 8: with 4 (the old default), some frames still showed the cars twice, half-blended with where
        // they were before the step (live test 2026-11-05: 8 and 12 clean, about the same render time)
        else if (k == "render_settle") c.render_settle = std::clamp(int(f), 8, 60);
        else if (k == "render_crf") c.render_crf = std::clamp(int(f), 0, 51);
        else if (k == "render_format") c.render_format = std::clamp(int(f), 0, 3);
        else if (k == "render_dir") c.render_dir = v;
        else if (k == "render_audio") c.render_audio = f != 0;
        else if (k == "render_blur") c.render_blur = std::clamp(int(f), 1, 32);
        else if (k == "render_screen_capture") c.render_screen_capture = f != 0;
        else if (k == "render_shutter") c.render_shutter = std::clamp(f, 10.0f, 360.0f);
        else if (k == "render_audio_wait") c.render_audio_wait = std::clamp(f, 0.5f, 30.0f);
        else if (k == "render_keep_parts") c.render_keep_parts = f;
        else if (k == "render_audio_offset") c.render_audio_offset = std::clamp(f, -2.0f, 2.0f);
        else if (k == "ffmpeg") c.ffmpeg = v;
    }
    if (g_cfg_first) {  // counters already in the file don't fire at load
        g_last_restart = c.restart;
        g_last_capture = c.capture;
        g_last_seek = c.seek;
        g_last_list = c.list_cars;
        g_last_freecam = c.freecam;
        g_last_final = c.final_cut;
        g_last_render = c.render;
        g_last_trace = c.speed_trace;
        g_cfg_first = false;
    }
    g_cfg = c;
    swofty::log("[cinecam] config: mode %d sequence \"%s\" shot (%.2f, %.2f, %.2f) yaw %.1f pitch %.1f fov %.1f "
                "base_speed %.2f",
                c.mode, c.sequence.c_str(), c.ox, c.oy, c.oz, c.yaw, c.pitch, c.fov, c.base_speed);
}

// ----- edit file (version 2: shots; version 1 files convert on load) --------------------------------

cine::Edit g_edit;
std::string g_seq_loaded;  // name
FILETIME g_seq_time{};
bool g_hist_reset = true;  // the edit was (re)loaded: undo history starts over (guarded by g_seq_mx)

void load_sequence()
{
    if (g_cfg.sequence.empty())
        return;
    std::string path = g_dir + "\\sequences\\" + g_cfg.sequence + ".json";
    FILETIME ft{};
    if (!file_time(path, ft)) {
        if (g_seq_loaded != g_cfg.sequence) {
            swofty::log("[cinecam] edit \"%s\": no file %s", g_cfg.sequence.c_str(), path.c_str());
            g_edit = {};
            g_hist_reset = true;
            g_seq_loaded = g_cfg.sequence;
            g_seq_time = {};
        }
        return;
    }
    if (g_seq_loaded == g_cfg.sequence && CompareFileTime(&ft, &g_seq_time) == 0)
        return;
    g_seq_loaded = g_cfg.sequence;
    g_seq_time = ft;
    std::string text;
    read_file(path, text);
    swofty::json::Error e;
    auto doc = swofty::json::parse(text, &e);
    std::string err;
    cine::Edit ed;
    if (!e.ok()) {
        err = "JSON line " + std::to_string(e.line) + ": " + e.message;
    } else {
        ed = cine::edit_from_json(doc, err);
        if (text.find("\"shots\"") == std::string::npos) {  // a version 1 file: keep the original
            std::string v1 = g_dir + "\\sequences\\" + g_cfg.sequence + ".v1.json";
            CopyFileA(path.c_str(), v1.c_str(), TRUE);
            swofty::log("[cinecam] edit \"%s\": converted from version 1 (original kept as %s)%s%s",
                        g_cfg.sequence.c_str(), v1.c_str(), err.empty() ? "" : ": ", err.c_str());
            err.clear();  // conversion notes are not fatal
        }
    }
    if (!err.empty()) {
        swofty::log("[cinecam] edit \"%s\" not loaded: %s", g_cfg.sequence.c_str(), err.c_str());
        g_edit = {};
        g_hist_reset = true;
        return;
    }
    g_edit = ed;
    g_hist_reset = true;
    swofty::log("[cinecam] edit \"%s\" (%s): %zu shot(s), %.2f .. %.2f s", g_cfg.sequence.c_str(), ed.name.c_str(),
                ed.shots.size(), ed.start(), ed.end());
}

// ----- game reads ------------------------------------------------------------------------------

// The followed car's camera target object for viewport 0, or 0.
uintptr_t followed_target()
{
    uintptr_t gm, rcs, comp, vec, t;
    if (!rd(g_base + kGameManagerRva, gm) || !rd(gm + 0x98, rcs) || !rd(rcs + 0x98, comp) ||
        !rd(comp + 0x2a0, vec) || !rd(vec + 8, t))
        return 0;
    return t;
}

// A target object's world frame (T+0x90: rows left, up, forward, translation).
std::optional<cine::Frame> target_frame(uintptr_t T)
{
    float m[16];
    if (!T || !mem::read(T + 0x90, m))
        return std::nullopt;
    cine::Frame f;
    f.r = cine::norm(V3{-m[0], -m[1], -m[2]});
    f.u = cine::norm(V3{m[4], m[5], m[6]});
    f.f = cine::norm(V3{m[8], m[9], m[10]});
    f.p = V3{m[12], m[13], m[14]};
    if (!std::isfinite(f.p.x) || cine::dot(f.f, f.f) < 0.5f || cine::dot(f.u, f.u) < 0.5f)
        return std::nullopt;
    return f;
}

// Car slot N's entity (the same object the cameras target), or 0. Every link is checked: vehicle id back-check,
// component back-pointer, component vtable.
uintptr_t slot_target(int n, bool *safety = nullptr)
{
    uint8_t count = 0;
    if (n < 0 || n >= 32 || !rd(g_base + kVehicleCountRva, count) || n >= count)
        return 0;
    uintptr_t car = 0, comp = 0, back = 0, vt = 0, T = 0;
    uint8_t id = 0xff;
    if (!rd(g_base + kVehicleTableRva + size_t(n) * 8, car) || !car || !rd(car + 0xe8, id) || id != n ||
        !rd(car + 0xe0, comp) || !comp || !rd(comp + 0x58, back) || back != car || !rd(comp, vt) ||
        !rd(comp + 0x08, T) || !T)
        return 0;
    vt -= g_base;
    if (vt != kVtCarPhysics2 && vt != kVtSafetyCar && vt != kVtVehicle)
        return 0;
    if (safety)
        *safety = vt == kVtSafetyCar;
    return T;
}

int slot_count()
{
    uint8_t count = 0;
    rd(g_base + kVehicleCountRva, count);
    return count;
}

// Car frames for this frame (cached per frame). -1 = the followed car; 0..N = car slot.
struct GameCars : cine::Cars {
    mutable std::map<int, std::optional<cine::Frame>> cache;
    std::optional<cine::Frame> frame(int car) const override
    {
        auto it = cache.find(car);
        if (it != cache.end())
            return it->second;
        std::optional<cine::Frame> f;
        f = target_frame(car < 0 ? followed_target() : slot_target(car));
        cache[car] = f;
        return f;
    }
};

// True paths: every car's frame recorded as the replay plays (20 per replay second, camera thread only), so a
// path on a car can be drawn where the car really drives instead of around where it is now.
struct CarTrack {
    static constexpr float kRate = 20;
    struct Sample {
        float p[3], r[3], u[3], f[3];
        bool ok;
    };
    std::vector<Sample> s[32];
    float end = -1;  // the replay these are for (its length)

    void record(float t, float replay_end, const cine::Cars &cars, int slots)
    {
        if (std::fabs(replay_end - end) > 0.01f) {  // another replay: start over
            for (auto &v : s)
                v.clear();
            end = replay_end;
        }
        int b = int(std::lround(t * kRate));
        if (b < 0 || replay_end <= 0 || b > int(replay_end * kRate) + 2)
            return;
        for (int i = 0; i < std::min(slots, 32); i++) {
            auto f = cars.frame(i);
            if (!f)
                continue;
            auto &v = s[i];
            if (v.empty())
                v.resize(size_t(replay_end * kRate) + 4);
            if (size_t(b) >= v.size())
                continue;
            v[size_t(b)] = {{f->p.x, f->p.y, f->p.z}, {f->r.x, f->r.y, f->r.z}, {f->u.x, f->u.y, f->u.z},
                            {f->f.x, f->f.y, f->f.z}, true};
        }
    }
    // Writes every recorded sample to `path` as CSV (t, slot, position, right, up, forward), for planning shots
    // offline (cfg dump_tracks). Returns the number of rows.
    size_t dump(const std::string &path) const
    {
        FILE *fp = fopen(path.c_str(), "wb");
        if (!fp)
            return 0;
        fprintf(fp, "t,slot,px,py,pz,rx,ry,rz,ux,uy,uz,fx,fy,fz\n");
        size_t rows = 0;
        for (int i = 0; i < 32; i++)
            for (size_t b = 0; b < s[i].size(); b++) {
                const Sample &q = s[i][b];
                if (!q.ok)
                    continue;
                fprintf(fp, "%.2f,%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", float(b) / kRate, i,
                        q.p[0], q.p[1], q.p[2], q.r[0], q.r[1], q.r[2], q.u[0], q.u[1], q.u[2], q.f[0], q.f[1], q.f[2]);
                rows++;
            }
        fclose(fp);
        return rows;
    }
    // the recorded fraction of [t0, t1] for car i
    float coverage(int i, float t0, float t1) const
    {
        if (i < 0 || i >= 32 || s[i].empty())
            return 0;
        int b0 = std::max(0, int(t0 * kRate)), b1 = std::min(int(s[i].size()) - 1, int(t1 * kRate));
        if (b1 <= b0)
            return 0;
        int n = 0;
        for (int b = b0; b <= b1; b++)
            n += s[i][size_t(b)].ok;
        return float(n) / float(b1 - b0 + 1);
    }
    // car i at time t, between the recorded samples around it (none recorded within 0.5 s: nullopt)
    std::optional<cine::Frame> at(int i, float t) const
    {
        if (i < 0 || i >= 32 || s[i].empty())
            return std::nullopt;
        auto &v = s[i];
        float x = t * kRate;
        int b0 = int(std::floor(x));
        auto ok = [&](int b) { return b >= 0 && b < int(v.size()) && v[size_t(b)].ok; };
        int lo = b0, hi = b0 + 1;
        while (lo > b0 - 10 && !ok(lo))
            lo--;
        while (hi < b0 + 11 && !ok(hi))
            hi++;
        if (!ok(lo) && !ok(hi))
            return std::nullopt;
        if (!ok(lo))
            lo = hi;
        if (!ok(hi))
            hi = lo;
        float w = hi > lo ? std::clamp((x - float(lo)) / float(hi - lo), 0.0f, 1.0f) : 0;
        auto mix = [&](const float *a, const float *c) {
            return V3{a[0] + (c[0] - a[0]) * w, a[1] + (c[1] - a[1]) * w, a[2] + (c[2] - a[2]) * w};
        };
        const Sample &A = v[size_t(lo)], &C = v[size_t(hi)];
        cine::Frame f;
        f.p = mix(A.p, C.p);
        f.f = cine::norm(mix(A.f, C.f));
        f.u = cine::norm(mix(A.u, C.u));
        f.r = cine::norm(mix(A.r, C.r));
        return f;
    }
} g_tracks;

// Cars for drawing a path: at the asked time from the recorded tracks when they cover the shot, else live.
struct TrackCars : cine::Cars {
    const cine::Cars &live;
    bool use;      // the tracks cover this shot's car well enough
    int followed;  // the slot of "the followed car" (-1)
    mutable float t = -1;
    TrackCars(const cine::Cars &l, bool u, int fol) : live(l), use(u), followed(fol) {}
    void at_time(float x) const override { t = x; }
    std::optional<cine::Frame> frame(int car) const override
    {
        if (use && t >= 0)
            if (auto f = g_tracks.at(car < 0 ? followed : car, t))
                return f;
        return live.frame(car);
    }
};

struct Replay {
    bool ok;
    uint32_t state, mode;
    float t, end;
    uintptr_t rs;
};
Replay replay()
{
    Replay r{};
    uintptr_t streams, st0;
    if (!rd(g_base + kReplaySystemRva, r.rs) || !rd(r.rs + 0x20, r.state) || !rd(r.rs + 0x3c, r.mode) ||
        !rd(r.rs + 0x98, streams) || !rd(streams, st0) || !rd(st0 + 0xd8, r.t))
        return r;
    rd(st0 + 0xe8, r.end);
    r.ok = true;
    return r;
}

// ----- quaternions -----------------------------------------------------------------------------

// The basis columns (local x, y, z) of a quaternion (x, y, z, w). For a game camera: right, up, -view.
void basis_from_quat(const float q[4], V3 &r, V3 &u, V3 &f)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    r = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
    u = {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)};
    f = {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
}

// ----- capture ---------------------------------------------------------------------------------

// Append the camera in viewport 0 as a key to sequences\_captured.json (one JSON object per line, ready
// to paste into a sequence's "keys"): world-anchored, plus the same shot relative to the followed car.
void capture(uintptr_t entry, const Replay &r, GameCars &cars)
{
    float eye[4], q[4], w = 0;
    memcpy(eye, reinterpret_cast<void *>(entry + kCam), 16);
    memcpy(q, reinterpret_cast<void *>(entry + kCam + 0x20), 16);
    memcpy(&w, reinterpret_cast<void *>(entry + kCam + 0x64), 4);
    V3 R, U, Z;
    basis_from_quat(q, R, U, Z);
    V3 F = Z * -1;  // the view direction
    V3 e{eye[0], eye[1], eye[2]};
    float fov = w > 0 ? 2 * std::atan(w / 2) / cine::kDeg : 70;
    float yaw = std::atan2(F.x, F.z) / cine::kDeg, pitch = std::asin(std::clamp(F.y, -1.0f, 1.0f)) / cine::kDeg;
    // roll: angle of the camera's right vector against the level right
    V3 level_r = cine::norm(cine::cross(V3{0, 1, 0}, F));
    float roll = std::atan2(cine::dot(cine::cross(level_r, R), F), cine::dot(level_r, R)) / cine::kDeg;
    char line[1024];
    int n = snprintf(line, sizeof line,
                     "{ \"t\": %.3f, \"pos\": { \"anchor\": \"world\", \"p\": [%.3f, %.3f, %.3f] }, \"look\": { \"type\": "
                     "\"dir\", \"yaw\": %.2f, \"pitch\": %.2f }, \"fov\": %.1f, \"roll\": %.2f",
                     r.t, e.x, e.y, e.z, yaw, pitch, fov, roll);
    if (auto car = cars.frame(-1)) {
        V3 d = e - car->p;
        V3 off{cine::dot(d, car->r), cine::dot(d, car->u), cine::dot(d, car->f)};
        V3 lf{cine::dot(F, car->r), cine::dot(F, car->u), cine::dot(F, car->f)};
        float cyaw = std::atan2(lf.x, lf.z) / cine::kDeg, cpitch = std::asin(std::clamp(lf.y, -1.0f, 1.0f)) / cine::kDeg;
        n += snprintf(line + n, sizeof line - n,
                      ",\n  \"car_relative\": { \"pos\": { \"anchor\": \"car\", \"car\": -1, \"offset\": [%.3f, %.3f, "
                      "%.3f] }, \"look\": { \"type\": \"dir\", \"yaw\": %.2f, \"pitch\": %.2f } }",
                      off.x, off.y, off.z, cyaw, cpitch);
    }
    snprintf(line + n, sizeof line - n, " },\n");
    std::string path = g_dir + "\\sequences\\_captured.json";
    if (FILE *f = fopen(path.c_str(), "ab")) {
        fputs(line, f);
        fclose(f);
        swofty::log("[cinecam] captured a key at t %.3f (state %u) to %s", r.t, r.state, path.c_str());
    } else {
        swofty::log("[cinecam] capture: could not write %s", path.c_str());
    }
}

// ----- car slots -------------------------------------------------------------------------------

int g_followed_slot = -2;  // last logged

// Which slot the replay follows (its entity equals the camera target), or -1.
int followed_slot()
{
    uintptr_t T = followed_target();
    int n = slot_count();
    for (int i = 0; T && i < n && i < 32; i++)
        if (slot_target(i) == T)
            return i;
    return -1;
}

void list_cars(GameCars &cars)
{
    auto me = cars.frame(-1);
    int n = slot_count();
    swofty::log("[cinecam] %d car slot(s); the replay follows slot %d", n, followed_slot());
    for (int i = 0; i < n && i < 32; i++) {
        bool sc = false;
        uintptr_t T = slot_target(i, &sc);
        auto f = target_frame(T);
        if (!f) {
            swofty::log("[cinecam]   slot %2d: no valid entity", i);
            continue;
        }
        if (me) {
            V3 d = f->p - me->p;
            swofty::log("[cinecam]   slot %2d%s: %7.1f m from the followed car (%+.1f m along its heading)", i,
                        sc ? " (safety car)" : "", cine::len(d), cine::dot(d, me->f));
        } else {
            swofty::log("[cinecam]   slot %2d%s: at (%.1f, %.1f, %.1f)", i, sc ? " (safety car)" : "", f->p.x,
                        f->p.y, f->p.z);
        }
    }
}

bool write_ptr(void **at, void *value);

// ----- pad: the game's XInputGetState import -----------------------------------------------------
// The game reads pads through xinput1_4.dll ordinal 2 (as Swofty's test agent notes). We take its import
// slot: every call stores the real state for the free camera, and while the free camera is on the game
// gets a centred, released pad (wheels use DirectInput and are not touched).

struct PadState {
    bool ok = false;
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
struct XInputState {
    DWORD packet;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
using XGet_t = DWORD(WINAPI *)(DWORD, XInputState *);
XGet_t g_xget_orig;
std::mutex g_pad_mx;
PadState g_pad[4];
std::atomic<bool> g_block_pad{false};

// Presses of the replay's own hide-HUD button (Y), for Final Cut: the game cycles its replay HUD through four
// states on Y (all shown, bar hidden, bar + tower hidden, everything hidden; L3 by screenshots after each press),
// so 3 presses from the normal view hide it all and 1 more brings it back. Each press is 120 ms down, 120 ms up on
// controller 1; with no XInput controller there, it reports an idle one only while pressing. hud_tick decides the
// presses (hud_request), checking the HUD's state between them.
std::atomic<int> g_y_presses{0};
ULONGLONG g_y_phase_at;        // xget_hook only
std::atomic<int> g_y_phase{0}; // 0 idle, 1 down, 2 up

// What Final Cut wants of the replay HUD: 0 nothing pending, 1 all shown, 2 everything hidden.
std::atomic<int> g_hud_goal{0};
// The HUD is hidden by us (the editor, Final Cut, a render) or not: a second hide while hidden, or a show while
// it isn't, does nothing (pressing again would lose the user's own state, which the first hide noted).
std::atomic<bool> g_hud_hidden_by_us{false};
void hud_request(bool hide)
{
    if (g_hud_hidden_by_us.exchange(hide) == hide)
        return;
    g_hud_goal = hide ? 2 : 1;
}
bool hud_busy() { return g_hud_goal.load() != 0 || g_y_phase.load() != 0 || g_y_presses.load() > 0; }

// Director thread, every frame. ReplayDirector+0x401 is 1 only in the HUD's first state (all shown) and 0 in the
// other three (L3: a memory diff of the director over four presses; nothing else in its first 0x4000 bytes
// follows Y). So: show = press until it reads 1. Hide = show first (from whatever state the user left), press once
// and check it took (0), then the last two presses. A press that didn't land is pressed again, up to 6 tries.
// The user's own state is put back: hiding counts the presses that brought it to all shown (n from state s means
// s = (4 - n) % 4), and showing presses that many more after reaching all shown.
void hud_tick(uintptr_t director)
{
    static int stage = 0, tries = 0, to_first = 0, user_state = 0;
    static ULONGLONG at = 0;
    int goal = g_hud_goal.load();
    if (!goal) {
        stage = tries = to_first = 0;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (g_y_presses.load() > 0 || g_y_phase.load() != 0) {
        at = now;  // a press still going: wait for it, then a moment for the HUD to follow
        return;
    }
    if (now - at < 150)
        return;
    bool shown = *reinterpret_cast<volatile uint8_t *>(director + 0x401) != 0;
    auto press = [&](int n) {
        g_y_presses += n;
        at = now;
    };
    auto done = [&](const char *what) {
        if (what)
            swofty::log("[cinecam] replay HUD: %s", what);
        g_hud_goal = 0;
        stage = tries = to_first = 0;
    };
    if (tries >= 6) {
        done(goal == 1 ? "could not bring it back (6 presses)" : "could not hide it (6 presses)");
        return;
    }
    if (goal == 1 || stage == 0) {  // to all shown
        if (shown) {
            if (goal == 1) {
                if (user_state > 0)  // back to how the user had it
                    press(user_state);
                user_state = 0;
                done(nullptr);
            } else {
                user_state = (4 - to_first % 4) % 4;
                stage = 1;  // hiding: from the first state now
            }
        } else {
            press(1);
            tries++;
            to_first++;
        }
        if (goal == 1 || stage == 0)
            return;
    }
    if (stage == 1) {  // first hide press
        press(1);
        stage = 2;
    } else if (stage == 2) {
        if (shown) {  // it didn't land
            press(1);
            tries++;
        } else {
            press(2);  // bar + tower, then everything
            done(nullptr);
        }
    }
}

bool y_injection_down()
{
    ULONGLONG now = GetTickCount64();
    if (g_y_phase == 0) {
        if (g_y_presses.load() <= 0)
            return false;
        g_y_presses--;
        g_y_phase = 1;
        g_y_phase_at = now;
    }
    if (now - g_y_phase_at >= 120) {
        g_y_phase_at = now;
        g_y_phase = g_y_phase == 1 ? 2 : 0;
    }
    return g_y_phase == 1;
}

DWORD WINAPI xget_hook(DWORD user, XInputState *s)
{
    DWORD r = g_xget_orig(user, s);
    if (user == 0 && s) {
        bool active = g_y_phase != 0 || g_y_presses.load() > 0;
        if (active) {
            bool down = y_injection_down();
            if (r != ERROR_SUCCESS) {  // no controller: an idle one while pressing
                memset(s, 0, sizeof *s);
                r = ERROR_SUCCESS;
            }
            static DWORD packet = 0x40000000;
            if (down)
                s->buttons |= 0x8000;  // Y
            s->packet = ++packet;
            return r;  // not blocked, not recorded as the user's pad
        }
    }
    if (user < 4) {
        std::lock_guard<std::mutex> lk(g_pad_mx);
        PadState &p = g_pad[user];
        p.ok = r == ERROR_SUCCESS && s;
        if (p.ok) {
            p.buttons = s->buttons;
            p.lt = s->lt;
            p.rt = s->rt;
            p.lx = s->lx;
            p.ly = s->ly;
            p.rx = s->rx;
            p.ry = s->ry;
        }
    }
    if (r == ERROR_SUCCESS && s && g_block_pad.load()) {
        s->buttons = 0;
        s->lt = s->rt = 0;
        s->lx = s->ly = s->rx = s->ry = 0;
    }
    return r;
}

// The import slot for xinput1_4.dll ordinal 2 (XInputGetState) in the game's import table.
void **xinput_slot(uintptr_t base)
{
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return nullptr;
    HMODULE x = GetModuleHandleA("xinput1_4.dll");
    void *fn = x ? reinterpret_cast<void *>(GetProcAddress(x, MAKEINTRESOURCEA(2))) : nullptr;
    for (auto *d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; d++) {
        const char *name = reinterpret_cast<const char *>(base + d->Name);
        if (_stricmp(name, "xinput1_4.dll") != 0)
            continue;
        auto *iat = reinterpret_cast<uintptr_t *>(base + d->FirstThunk);
        auto *ilt = d->OriginalFirstThunk ? reinterpret_cast<const uintptr_t *>(base + d->OriginalFirstThunk) : nullptr;
        for (size_t i = 0; iat[i]; i++) {
            bool by_ord = ilt && (ilt[i] & IMAGE_ORDINAL_FLAG64) && IMAGE_ORDINAL64(ilt[i]) == 2;
            bool by_name = false;
            if (ilt && !(ilt[i] & IMAGE_ORDINAL_FLAG64)) {
                auto *ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + ilt[i]);
                by_name = strcmp(reinterpret_cast<const char *>(ibn->Name), "XInputGetState") == 0;
            }
            if (by_ord || by_name || (fn && iat[i] == reinterpret_cast<uintptr_t>(fn)))
                return reinterpret_cast<void **>(&iat[i]);
        }
    }
    return nullptr;
}

// The game's import slot for `fn` (by name, from whichever DLL), or nullptr.
void **import_slot(uintptr_t base, const char *fn)
{
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return nullptr;
    for (auto *d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; d++) {
        if (!d->OriginalFirstThunk)
            continue;
        auto *iat = reinterpret_cast<uintptr_t *>(base + d->FirstThunk);
        auto *ilt = reinterpret_cast<const uintptr_t *>(base + d->OriginalFirstThunk);
        for (size_t i = 0; iat[i]; i++)
            if (!(ilt[i] & IMAGE_ORDINAL_FLAG64) &&
                strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + ilt[i])->Name, fn) == 0)
                return reinterpret_cast<void **>(&iat[i]);
    }
    return nullptr;
}

// ----- which replay file is playing: the game's CreateFileW import ------------------------------------------
// Every .frr the game opens is noted (its path and when); the replay that starts next is the last one opened.
// Nothing else is touched: the call goes through unchanged.
using CreateFileW_t = HANDLE(WINAPI *)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileW_t g_cfw_orig;
std::mutex g_frr_mx;
std::wstring g_frr_path;  // the last .frr opened (guarded by g_frr_mx)
ULONGLONG g_frr_at;       // when (GetTickCount64)
std::atomic<int> g_frr_logged{0};

HANDLE WINAPI cfw_hook(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                       HANDLE tmpl)
{
    HANDLE h = g_cfw_orig(name, access, share, sa, disp, flags, tmpl);
    if (name && h != INVALID_HANDLE_VALUE && (access & GENERIC_READ) && !(access & GENERIC_WRITE)) {
        size_t n = wcslen(name);
        if (n > 4 && _wcsicmp(name + n - 4, L".frr") == 0) {
            {
                std::lock_guard<std::mutex> lk(g_frr_mx);
                g_frr_path = name;
                g_frr_at = GetTickCount64();
            }
            if (g_frr_logged.fetch_add(1) < 40) {  // how the game opens replays (list vs play), for the log
                char path[600];
                WideCharToMultiByte(CP_UTF8, 0, name, -1, path, sizeof path, nullptr, nullptr);
                swofty::log("[cinecam] replay file opened: %s (access 0x%lx, flags 0x%lx)", path,
                            (unsigned long)access, (unsigned long)flags);
            }
        }
    }
    return h;
}

void install_file_hook()
{
    void **slot = import_slot(g_base, "CreateFileW");
    if (!slot || !*slot) {
        swofty::log("[cinecam] replay files: no CreateFileW import; edits match replays by length and drivers");
        return;
    }
    g_cfw_orig = reinterpret_cast<CreateFileW_t>(*slot);
    if (!write_ptr(slot, reinterpret_cast<void *>(&cfw_hook))) {
        g_cfw_orig = nullptr;
        swofty::log("[cinecam] replay files: could not write the CreateFileW import slot");
    }
}

// ----- the game's physics raycast (0x1440c0f10, build d873a1c8; static L1: docs/re notes in PROJECT.md) --------
// ray(world, from (vec4), to (vec4), u8 layer, float *fraction, vec4 *normal, 16-byte extra) -> hit object or 0.
// Photo mode's free camera uses layers 13 and 5 to stay out of geometry; the AI uses 14. The world isn't reachable
// from here, so the function's entry is detoured: every call notes its world (and which layers the game uses),
// then goes on unchanged through a trampoline holding the 16 bytes the jump replaced. Our own rays call the
// trampoline. The entry's bytes are checked first, so on any other build nothing is patched.
using Raycast_t = uint64_t (*)(void *world, const float *from, const float *to, uint8_t layer, float *fraction,
                               float *normal, void *extra);
constexpr uintptr_t kRaycastRva = 0x40c0f10;
Raycast_t g_ray_tramp;
std::atomic<void *> g_ray_world{nullptr};
std::atomic<uint64_t> g_ray_calls{0}, g_ray_layers{0};

uint64_t ray_hook(void *world, const float *from, const float *to, uint8_t layer, float *fraction, float *normal,
                  void *extra)
{
    if (world)
        g_ray_world.store(world, std::memory_order_relaxed);
    g_ray_calls.fetch_add(1, std::memory_order_relaxed);
    if (layer < 64)
        g_ray_layers.fetch_or(1ull << layer, std::memory_order_relaxed);
    return g_ray_tramp(world, from, to, layer, fraction, normal, extra);
}

void install_ray_hook()
{
    static const uint8_t expect[16] = {0x56, 0x48, 0x81, 0xec, 0xf0, 0x00, 0x00, 0x00,   // push rsi; sub rsp,0xf0
                                       0x4c, 0x8b, 0x94, 0x24, 0x28, 0x01, 0x00, 0x00};  // mov r10,[rsp+0x128]
    uint8_t *fn = reinterpret_cast<uint8_t *>(g_base + kRaycastRva);
    if (memcmp(fn, expect, sizeof expect) != 0) {
        swofty::log("[cinecam] raycast: entry bytes differ (another build): no clipping checks");
        return;
    }
    auto *tr = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tr)
        return;
    memcpy(tr, fn, 16);  // the replaced instructions (no RIP-relative operands), then jmp [rip+0] -> fn + 16
    tr[16] = 0xff, tr[17] = 0x25;
    memset(tr + 18, 0, 4);
    uintptr_t back = reinterpret_cast<uintptr_t>(fn + 16);
    memcpy(tr + 22, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);
    g_ray_tramp = reinterpret_cast<Raycast_t>(tr);
    uint8_t patch[16] = {0xff, 0x25, 0, 0, 0, 0};  // jmp [rip+0] -> ray_hook, then 2 nops
    uintptr_t to = reinterpret_cast<uintptr_t>(&ray_hook);
    memcpy(patch + 6, &to, 8);
    patch[14] = patch[15] = 0x90;
    DWORD old = 0;
    if (!VirtualProtect(fn, 16, PAGE_EXECUTE_READWRITE, &old)) {
        g_ray_tramp = nullptr;
        return;
    }
    memcpy(fn, patch, 16);
    VirtualProtect(fn, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 16);
    swofty::log("[cinecam] raycast: entry at base+0x%llx detoured (clipping checks)", (unsigned long long)kRaycastRva);
}

// One of our rays from a to b on `layer` (the game's thread only): the fraction of the way to the first hit
// (1 = none) and the hit's normal.
float raycast(V3 a, V3 b, uint8_t layer, V3 *normal = nullptr)
{
    void *w = g_ray_world.load();
    if (!g_ray_tramp || !w)
        return 1;
    alignas(16) float from[4] = {a.x, a.y, a.z, 0}, to[4] = {b.x, b.y, b.z, 0}, nrm[4] = {}, extra[4] = {};
    float frac = 1;
    uint64_t hit = g_ray_tramp(w, from, to, layer, &frac, nrm, extra);
    if (!hit)
        return 1;
    if (normal)
        *normal = {nrm[0], nrm[1], nrm[2]};
    return std::clamp(frac, 0.0f, 1.0f);
}

// Where a sampled camera path goes into the track or a barrier, from the game's collision (layer 13, the photo
// mode camera's: the world without the cars, so onboard shots inside a car are fine). Per point (ps.clip):
// - 2 when the way from the previous point crosses a surface (a ray forward, and one backward, between them),
//   and every point between entering a surface and leaving it again (inside something thick);
// - 2 when a surface lies within 3 m above the point (under the track);
// - 1 when the ground is within 30 cm below it (skimming).
// The game's thread only (the rays run where the game's own camera rays do).
constexpr uint8_t kCamLayer = 13;
void clip_check(cine::PathSample &ps)
{
    ps.clip.assign(ps.pts.size(), 0);
    if (!g_ray_tramp || !g_ray_world.load())
        return;
    bool inside = false;
    for (size_t i = 0; i < ps.pts.size(); i++) {
        V3 p = ps.pts[i];
        uint8_t c = 0;
        if (raycast(p + V3{0, 3, 0}, p, kCamLayer) < 1)  // something above it, within 3 m: under the surface
            c = 2;
        else if (raycast(p, p + V3{0, -0.3f, 0}, kCamLayer) < 1)
            c = 1;
        if (i > 0 && cine::len(p - ps.pts[i - 1]) > 1e-3f) {
            V3 q = ps.pts[i - 1];
            bool enters = raycast(q, p, kCamLayer) < 1, leaves = raycast(p, q, kCamLayer) < 1;
            if (enters || leaves)
                c = 2;
            if (enters && !leaves)
                inside = true;
            else if (leaves && !enters)
                inside = false;
        }
        if (inside)
            c = 2;
        ps.clip[i] = c;
    }
}

// The id of the replay file opened in the last `within_ms`: the hex of its header MAC (bytes 160..191), or "".
// `stem`: its file name without .frr (the game names them track_team_mode_date_time).
std::string recent_replay_file(ULONGLONG within_ms, std::string &stem)
{
    std::wstring path;
    {
        std::lock_guard<std::mutex> lk(g_frr_mx);
        if (g_frr_path.empty() || GetTickCount64() - g_frr_at > within_ms)
            return {};
        path = g_frr_path;
    }
    {
        size_t s = path.find_last_of(L"\\/");
        std::wstring w = path.substr(s == std::wstring::npos ? 0 : s + 1);
        w.resize(w.size() >= 4 ? w.size() - 4 : w.size());
        char b[260];
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof b, nullptr, nullptr);
        stem = b;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    uint8_t b[192];
    DWORD got = 0;
    bool ok = ReadFile(f, b, sizeof b, &got, nullptr) && got == sizeof b;
    CloseHandle(f);
    if (!ok)
        return {};
    std::string hex;
    char x[3];
    for (int i = 160; i < 192; i++) {
        snprintf(x, sizeof x, "%02x", b[i]);
        hex += x;
    }
    return hex;
}

void install_pad_hook()
{
    void **slot = xinput_slot(g_base);
    if (!slot || !*slot) {
        swofty::log("[cinecam] pad: no XInputGetState import found; the free camera uses keyboard and mouse only");
        return;
    }
    g_xget_orig = reinterpret_cast<XGet_t>(*slot);
    if (!write_ptr(slot, reinterpret_cast<void *>(&xget_hook))) {
        g_xget_orig = nullptr;
        swofty::log("[cinecam] pad: could not write the XInputGetState import slot");
        return;
    }
    swofty::log("[cinecam] pad: XInputGetState import at base+0x%llx taken",
                (unsigned long long)(uintptr_t(slot) - g_base));
}

// ----- shared state: UI (overlay paint thread) <-> camera (ViewportManager hook, worker threads) ----

namespace ov = swofty::overlay;
namespace oui = swofty::overlay::ui;

// g_edit and its bookkeeping are shared: the editor edits, the camera evaluates. g_seq_mx guards g_edit,
// g_seq_loaded, g_seq_time and the dirty flag.
std::recursive_mutex g_seq_mx;
bool g_dirty;
ULONGLONG g_dirty_at;

void mark_dirty()
{
    g_dirty = true;
    g_dirty_at = GetTickCount64();
}

// Flight controls and actions from the keyboard and mouse (paint -> camera).
struct KbState {
    bool w, a, s, d, q, e, z, c, r, f, shift, ctrl, rmb, up, down, left, right;
    float mdx, mdy;
    float step;     // replay seconds to step ([ / ])
    float wheel;    // mouse-wheel notches outside the timeline: fly speed
    int speed_dir;  // - / =
    int toggle, add, freeze, level, ride, align, flat, level_surface, fly, orbit;
};
std::mutex g_kb_mx;
KbState g_kb{};
std::atomic<int> g_device{0};       // the input device used last: 0 keyboard / mouse, 1 pad
std::atomic<bool> g_guides{false};  // crosshair, thirds and the true horizon (G)
std::atomic<bool> g_help{false};    // the controls table (H)
std::atomic<bool> g_hide_ui{false}; // U: preview without the editor's UI (the editor stays live)

// What the UI shows (camera -> paint).
struct Status {
    bool in_replay, editor, fly, orbit, preview, frozen, ride, playing;
    int ride_car, followed, slots, shot;
    float t, end, fov, speed, play_speed, fly_speed;
    bool final_cut;               // Final Cut is playing
    float edit_t0, edit_t1;       // the edit's first and last moment
    float letterbox, fade;        // Final Cut settings
    bool rendering, render_show;  // a render is running; its progress line may show this frame (not near a capture)
    int render_frame, render_total, render_phase;
    // the settings, for the Options pane
    int o_fps, o_settle, o_crf, o_blur, o_format;
    float o_shutter;
    bool o_audio, o_listener, o_follow, o_passby, o_hidehud, o_clip;
    int o_keycurve;
    float o_offset, o_wait, o_preroll;
    char o_dir[260];  // where renders go (UTF-8)
    char seq[64];
    // precise readout of the view: world angles, and relative to the nearest car
    float yaw, pitch, wroll;
    int near_car;
    float c_yaw, c_pitch, c_roll, c_height, c_dist;
};
std::mutex g_status_mx;
Status g_status{};
std::deque<std::string> g_toasts;  // guarded by g_status_mx

// The camera path drawn in 3D (camera thread -> paint): sampled paths of the selected shot and the shot under the
// playhead, and the view they are projected through (what is on screen).
struct PathView {
    V3 eye;
    cine::Quat q;
    float fov;
};
struct PathDraw {
    cine::PathSample ps;
    cine::ShotType type;
    bool selected;
    int shot;
    bool true_path;  // drawn along the car's recorded drive (else around where the car is now)
};
std::mutex g_path_mx;
std::vector<PathDraw> g_path_draw;  // guarded by g_path_mx
PathView g_path_view;               // guarded by g_path_mx
bool g_path_valid;                  // guarded by g_path_mx: we drive the camera and the editor is open
std::atomic<int> g_path_shot{-1};   // paint -> camera: the selected shot
std::atomic<bool> g_show_path{true}; // J

void toast(std::string s)
{
    std::lock_guard<std::mutex> lk(g_status_mx);
    if (g_toasts.size() < 8)
        g_toasts.push_back(std::move(s));
}

// Commands (paint -> camera): anything that needs the game (seek, car frames, the live view).
enum class CmdKind { SwitchEdit, ToggleFly, Seek, Freeze, PlaySpeed, PlayFromStart, FinalCut, Render, StopPreview, AddShot, AddKey, SetKeyFromView, PlaceTrack };
struct Cmd {
    CmdKind kind;
    float t = 0;
    int shot = -1, key = -1;
    int type = 0;
    std::string name = {};  // SwitchEdit: the edit; type 0 open, 1 new (empty), 2 duplicate of the current one
    float t2 = 0;           // Render: the range's end (t = its start; t2 <= t: the whole edit)
};
std::mutex g_cmd_mx;
std::vector<Cmd> g_cmds;

void post(Cmd c)
{
    std::lock_guard<std::mutex> lk(g_cmd_mx);
    if (g_cmds.size() < 64)
        g_cmds.push_back(c);
}

std::atomic<bool> g_editor{false};   // the editor is open
std::atomic<bool> g_fly{false};      // in the editor: flying the free camera (else watching the edit)
std::atomic<bool> g_preview{false};  // "Play all": the edit drives the camera outside the editor
std::atomic<bool> g_final{false};    // Final Cut: Play all with every HUD hidden, fades and bars
float g_final_return = -1;           // camera thread: where the editor's playhead was when Final Cut started
bool g_preview_inside;               // camera thread: Play all / Final Cut has played inside the edit (its start
                                     // seek has landed), so passing the edit's end really is the end
std::atomic<int> g_select_shot{-2};  // camera -> UI: select this shot on the next paint (-2: none)

std::string seq_name() { return g_cfg.sequence.empty() ? std::string("my_edit") : g_cfg.sequence; }

void save_sequence_locked()
{
    std::string path = g_dir + "\\sequences\\" + seq_name() + ".json";
    if (g_edit.name.empty())
        g_edit.name = seq_name();
    std::string text = cine::edit_to_json(g_edit);
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
        file_time(path, g_seq_time);  // our own write: no reload
        g_seq_loaded = seq_name();
        g_dirty = false;
    } else {
        swofty::log("[cinecam] could not write %s", path.c_str());
    }
}

std::string mmss(float t)
{
    char b[32];
    if (t < 0)
        t = 0;
    int m = int(t / 60);
    snprintf(b, sizeof b, "%d:%05.2f", m, t - m * 60.0f);
    return b;
}

// ----- driver names: the game's view model (#session.cars[N].driver.second_name) -----------------------
// Read the way show_all_drivers.dll reads them (MVCD: node = *handle, value holder at node+0x58, holder vtable
// +0x38 returns the value, an engine string with its characters at +9), on Swofty's frame tick (the flow thread
// that runs the view models), about once a second while the editor is open. The camera and the UI only read
// the cache. The index is the car slot (the vehicle id), as in show_all_drivers.

using Resolve_t = void *(void *registry, const char *path, const void *base, void *create);
using Value_t = void *(void *holder);
Resolve_t *g_vm_resolve;
void **g_vm_registry;
void *g_vm_missing;
std::mutex g_names_mx;
std::string g_names[32];  // guarded by g_names_mx
uint64_t g_name_ticks;

void *vm_lookup(const char *path)
{
    void *reg = g_vm_registry ? *g_vm_registry : nullptr;
    uint8_t *root = nullptr;
    if (!reg || !rd(reinterpret_cast<uintptr_t>(reg) + 8, root) || !root)
        return nullptr;
    void *base = root + 8;
    if (!g_vm_missing)
        g_vm_missing = g_vm_resolve(reg, "#cinecam_probe.no_such_node", base, nullptr);
    void *r = g_vm_resolve(reg, path, base, nullptr);
    if (!r || r == base || r == g_vm_missing || mem::in_image(r))
        return nullptr;
    return r;
}

bool vm_string(const char *path, std::string &out)
{
    void *handle = vm_lookup(path);
    uint8_t *node = nullptr, *holder = nullptr, *value_box = nullptr, *str = nullptr;
    void **vt = nullptr;
    Value_t *get = nullptr;
    if (!handle || !rd(reinterpret_cast<uintptr_t>(handle), node) || !node || mem::in_image(node) ||
        !rd(reinterpret_cast<uintptr_t>(node) + 0x58, holder) || !holder ||
        !rd(reinterpret_cast<uintptr_t>(holder), vt) || !vt || !mem::in_image(vt) ||
        !rd(reinterpret_cast<uintptr_t>(vt) + 0x38, get) || !get || !mem::in_image(reinterpret_cast<void *>(get)))
        return false;
    value_box = static_cast<uint8_t *>(get(holder));
    if (!value_box || !rd(reinterpret_cast<uintptr_t>(value_box), str) || !str)
        return false;
    std::string sname;
    if (!mem::c_string(str + 9, sname, 64) || sname.empty() || !mem::printable_utf8(sname))
        return false;
    out = std::move(sname);
    return true;
}

void name_tick(double, void *)
{
    if (!g_vm_resolve || !g_editor.load() || (++g_name_ticks % 60) != 1)
        return;
    int n = std::min(slot_count(), 32);
    std::string got[32];
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof path, "#session.cars[%d].driver.second_name", i);
        vm_string(path, got[i]);
    }
    std::lock_guard<std::mutex> lk(g_names_mx);
    for (int i = 0; i < 32; i++)
        g_names[i] = i < n ? got[i] : std::string();
}

std::string car_name(int slot)
{
    if (slot < 0 || slot >= 32)
        return {};
    std::lock_guard<std::mutex> lk(g_names_mx);
    return g_names[slot];
}

// "BOTTAS (21)" when the name is known, else "car 21"; -1 is the car the replay follows.
std::string car_label(int car)
{
    if (car < 0)
        return "followed car";
    std::string n = car_name(car);
    char b[80];
    if (!n.empty())
        snprintf(b, sizeof b, "%s (%d)", n.c_str(), car);
    else
        snprintf(b, sizeof b, "car %d", car);
    return b;
}

// A slider key counts as "at the playhead" within this (s): a jump to a key can land a frame off it.
constexpr float kKeyNear = 0.05f;

// The curve a new slider key gets for the segment after it (Options: key_curve).
cine::Curve new_key_curve() { return cine::curve_preset(g_cfg.key_curve == 1 ? 1 : 0); }

// Which preset a curve is (0 smooth, 1 linear, 2 ease in, 3 ease out), or -1 (shaped by hand).
int curve_preset_of(const cine::Curve &c)
{
    for (int i = 0; i < 4; i++) {
        cine::Curve pc = cine::curve_preset(i);
        if (pc.pts.size() != c.pts.size())
            continue;
        bool same = true;
        for (size_t j = 0; j < c.pts.size() && same; j++) {
            const auto &a = pc.pts[j], &b = c.pts[j];
            same = std::fabs(a.ix - b.ix) + std::fabs(a.iy - b.iy) + std::fabs(a.ox - b.ox) + std::fabs(a.oy - b.oy) < 2e-3f;
        }
        if (same)
            return i;
    }
    return -1;
}
float g_props_w = 0;  // the properties panel's width while shown (0: hidden), for the 3D view's click routing

// The previous (dir -1) or next (dir 1) key of the whole edit from time t. Selects that key alone and its shot
// (on the next paint). Caller holds g_seq_mx.
std::optional<float> jump_key(cine::Edit &ed, float t, int dir)
{
    int bs = -1, bk = -1;
    float bt = 0;
    for (int i = 0; i < int(ed.shots.size()); i++) {
        const auto &keys = ed.shots[size_t(i)].keys;
        for (int j = 0; j < int(keys.size()); j++) {
            float kt = keys[size_t(j)].t;
            bool ahead = dir > 0 ? kt > t + 0.02f : kt < t - 0.02f;
            if (ahead && (bs < 0 || (dir > 0 ? kt < bt : kt > bt)))
                bs = i, bk = j, bt = kt;
        }
    }
    for (int i = 0; i < int(ed.shots.size()); i++)  // keyframed slider values
        for (const auto &tr : ed.shots[size_t(i)].tracks)
            for (const auto &pk : tr.keys) {
                bool ahead = dir > 0 ? pk.t > t + 0.02f : pk.t < t - 0.02f;
                if (ahead && (bs < 0 || (dir > 0 ? pk.t < bt : pk.t > bt)))
                    bs = i, bk = -1, bt = pk.t;
            }
    if (bs < 0)
        return std::nullopt;
    for (auto &sh : ed.shots)
        for (auto &k : sh.keys)
            k.selected = false;
    if (bk >= 0)
        ed.shots[size_t(bs)].keys[size_t(bk)].selected = true;
    g_select_shot = bs;
    return bt;
}

std::string shot_label(const cine::Shot &s)
{
    std::string l = cine::shot_type_name(s.type);
    if (s.uses_car()) {
        std::string n = car_name(s.car);
        l += " · " + (!n.empty() ? n : !s.driver.empty() ? s.driver : car_label(s.car));
    }
    if (s.type == cine::ShotType::onboard)
        if (const cine::Mount *m = cine::find_mount(s.mount))
            l += std::string(" · ") + m->name;
    if (s.keyed() && !s.keys.empty())
        l += " · " + std::to_string(s.keys.size()) + " key" + (s.keys.size() == 1 ? "" : "s");
    if (std::fabs(s.speed - 1) > 1e-3f) {
        char b[24];
        snprintf(b, sizeof b, " · %.2fx", s.speed);
        l += b;
    }
    return l;
}

uint32_t shot_colour(cine::ShotType t)
{
    switch (t) {
    case cine::ShotType::onboard: return 0x2f7fd8d8;
    case cine::ShotType::chase: return 0x2fa86cd8;
    case cine::ShotType::track: return 0xc8862ad8;
    case cine::ShotType::orbit: return 0x8a4fd0d8;
    case cine::ShotType::free_path: return 0x5a5a6ad8;
    case cine::ShotType::transition: return 0x3a9aa0d8;
    }
    return 0x5a5a6ad8;
}

// The graph editor for a speed curve (transitions' axes, keyed values' segments). Time runs across, progress up
// (0 = the start value, 1 = the end value). Drag an anchor (its handles move with it) or a handle; double-click the
// curve to add an anchor there, double-click an anchor between the ends to remove it. The vertical range fits the
// curve (at least 0..1, room to go below and above) and holds still while dragging. `faint`: other curves drawn
// behind, for comparison. `playhead`: 0..1 where the playhead is in the move, or < 0. Returns true when it changed.
bool curve_editor(oui::Ui &u, SwoftyPaint *p, ov::Rect g, cine::Curve &cv, uint32_t col, float playhead,
                  const cine::Curve *const *faint = nullptr, const uint32_t *faint_col = nullptr, int n_faint = 0)
{
    static const void *drag_curve = nullptr;
    static int drag_pt = -1, drag_part = 0;  // part: 0 anchor, 1 in handle, 2 out handle
    static float lo = -0.25f, hi = 1.25f;
    static ULONGLONG last_click;
    static float last_x, last_y;
    const auto &in = u.in;
    bool dragging = drag_curve == &cv;
    if (!dragging) {  // fit the range to this curve (with room around it)
        float a = 0, b = 1;
        for (const auto &q : cv.pts)
            a = std::min({a, q.y, q.iy, q.oy}), b = std::max({b, q.y, q.iy, q.oy});
        lo = a - 0.25f, hi = b + 0.25f;
    }
    float span = std::max(0.1f, hi - lo);
    auto sx = [&](float x) { return g.x + x * g.w; };
    auto sy = [&](float v) { return g.y + g.h * (hi - v) / span; };
    auto vx = [&](float px) { return (px - g.x) / std::max(1.0f, g.w); };
    auto vy = [&](float py) { return hi - (py - g.y) / std::max(1.0f, g.h) * span; };
    ov::rect_rounded(p, g.x - u.u(4), g.y, g.w + u.u(8), g.h, u.u(6), 0x00000060);
    ov::line(p, g.x, sy(0), g.x + g.w, sy(0), 1, 0xffffff30);
    ov::line(p, g.x, sy(1), g.x + g.w, sy(1), 1, 0xffffff30);
    if (playhead >= 0 && playhead <= 1)
        ov::line(p, sx(playhead), g.y, sx(playhead), g.y + g.h, 1, 0xe1060080);
    auto draw_curve = [&](const cine::Curve &c, uint32_t cl, float w) {
        float px0 = sx(0), py0 = sy(c.at(0));
        for (int k = 1; k <= 64; k++) {
            float xx = float(k) / 64, px1 = sx(xx), py1 = sy(c.at(xx));
            ov::line(p, px0, py0, px1, py1, w, cl);
            px0 = px1, py0 = py1;
        }
    };
    for (int i = 0; i < n_faint; i++)
        if (faint[i] && faint[i] != &cv)
            draw_curve(*faint[i], (faint_col[i] & 0xffffff00u) | 0x50, u.u(1.5f));
    draw_curve(cv, col, u.u(3));
    // handles and anchors
    float grab = u.u(11);
    int hot_pt = -1, hot_part = 0;
    float best = grab;
    for (int i = 0; i < int(cv.pts.size()); i++) {
        const auto &q = cv.pts[size_t(i)];
        struct Cand {
            int part;
            float x, y;
        } cands[3] = {{0, q.x, q.y}, {1, q.ix, q.iy}, {2, q.ox, q.oy}};
        for (const auto &c : cands) {
            if ((c.part == 1 && i == 0) || (c.part == 2 && i + 1 == int(cv.pts.size())))
                continue;  // the ends have one handle each
            if (c.part == 0 && (i == 0 || i + 1 == int(cv.pts.size())))
                continue;  // the ends don't move
            float d = std::hypot(in.x - sx(c.x), in.y - sy(c.y));
            if (d < best)
                best = d, hot_pt = i, hot_part = c.part;
        }
    }
    bool inside = in.x >= g.x - u.u(6) && in.x <= g.x + g.w + u.u(6) && in.y >= g.y && in.y <= g.y + g.h;
    bool press = (in.pressed & ov::mouse::left) && inside;
    bool dbl = press && GetTickCount64() - last_click < 400 && std::hypot(in.x - last_x, in.y - last_y) < u.u(8);
    if (press)
        last_click = dbl ? 0 : GetTickCount64(), last_x = in.x, last_y = in.y;
    bool changed = false;
    if (dbl) {
        if (hot_pt > 0 && hot_part == 0 && hot_pt + 1 < int(cv.pts.size())) {  // remove an anchor
            cv.pts.erase(cv.pts.begin() + hot_pt);
            changed = true;
        } else if (hot_pt < 0) {  // add one where the curve is
            float x = std::clamp(vx(in.x), 0.0f, 1.0f);
            if (std::fabs(in.y - sy(cv.at(x))) < u.u(14)) {
                cv.add_anchor(x);
                changed = true;
            }
        }
        drag_curve = nullptr;
    } else if (press && hot_pt >= 0) {
        drag_curve = &cv, drag_pt = hot_pt, drag_part = hot_part;
    }
    if (drag_curve == &cv) {
        if (!in.held() || drag_pt >= int(cv.pts.size())) {
            drag_curve = nullptr;
        } else {
            auto &q = cv.pts[size_t(drag_pt)];
            float nx = std::clamp(vx(in.x), 0.0f, 1.0f), ny = std::clamp(vy(in.y), -3.0f, 4.0f);
            if (drag_part == 0) {  // the anchor, its handles with it
                float dx = nx - q.x, dy = ny - q.y;
                q.x += dx, q.ix += dx, q.ox += dx;
                q.y += dy, q.iy += dy, q.oy += dy;
            } else if (drag_part == 1) {
                q.ix = nx, q.iy = ny;
            } else {
                q.ox = nx, q.oy = ny;
            }
            cv.tidy();
            changed = true;
        }
    }
    for (int i = 0; i < int(cv.pts.size()); i++) {
        const auto &q = cv.pts[size_t(i)];
        bool first = i == 0, last = i + 1 == int(cv.pts.size());
        auto handle = [&](int part, float hx, float hy) {
            bool hot = (drag_curve == &cv && drag_pt == i && drag_part == part) || (hot_pt == i && hot_part == part);
            ov::line(p, sx(q.x), sy(q.y), sx(hx), sy(hy), 1, 0xffffff90);
            ov::circle(p, sx(hx), sy(hy), u.u(hot ? 7 : 5), 0x000000c0);
            ov::circle(p, sx(hx), sy(hy), u.u(hot ? 5 : 3.5f), col);
        };
        if (!first)
            handle(1, q.ix, q.iy);
        if (!last)
            handle(2, q.ox, q.oy);
        bool hot = (drag_curve == &cv && drag_pt == i && drag_part == 0) || (hot_pt == i && hot_part == 0);
        float r = u.u(hot ? 7 : 5.5f);  // anchors are squares, handles round
        ov::rect_rounded(p, sx(q.x) - r, sy(q.y) - r, 2 * r, 2 * r, u.u(1.5f), first || last ? 0xffffff80 : 0xffffffff);
    }
    return changed;
}

// ----- the editor UI (overlay paint thread) ----------------------------------------------------------

oui::State g_ui;
bool g_focus;
int g_shot = -1;              // selected shot
float g_v0 = 0, g_v1 = 40;    // timeline view (replay seconds)
bool g_view_init;
ULONGLONG g_view_touched;     // the last time the user moved the timeline view
enum class Drag { none, scrub, shot_move, shot_left, shot_right, key_move, marquee, shot_marquee, sbar, pan };
Drag g_drag = Drag::none;
float g_drag_x0 = 0, g_drag_dt = 0, g_drag_a = 0, g_drag_b = 0;
float g_scrub_t = 0, g_marq_x = 0, g_sbar_grab = 0, g_pan_x = 0;
std::vector<cine::Shot> g_clip;  // copied shots
enum class Clip { none, shots, keys } g_clip_kind = Clip::none;
std::vector<cine::ShotKey> g_key_clip;  // copied camera keys (times relative to the first)
cine::ShotType g_key_clip_type;
std::optional<cine::Shot> g_look_clip;  // Copy look: a shot's settings and value keys
std::atomic<bool> g_snap{true};         // snap drags to the playhead and shot edges (N)
float g_snap_line = -1;                 // where the last drag frame snapped (drawn), -1 none
float g_drag_keyt;                      // the grabbed key's time (key drags snap it)
bool g_pick_driver, g_edits_menu;       // the driver list / edits menu are open
bool g_render_menu, g_options;          // the Render menu / Options pane are open
bool g_play_menu;                       // the Play menu (Play all, Final Cut, Render...) is open
float g_play_menu_x;                    // where its button is (the menu opens above it)
float g_render_in = -1, g_render_out = -1;  // the render range's in / out points (-1: not set), shown on the ruler
std::vector<cine::Edit> g_undo, g_redo;  // guarded by g_seq_mx (paint thread)
cine::Edit g_prev;                       // the edit as of the last history step
constexpr float kTimelineH = 214;

constexpr uint32_t kAccent = 0xe10600ff, kText = 0xffffffff, kDim = 0xffffffa0, kLane = 0xffffff14;

void draw_diamond(SwoftyPaint *p, float x, float y, float r, uint32_t rgba)
{
    float xy[8] = {x, y - r, x + r, y, x, y + r, x - r, y};
    ov::polygon(p, xy, 4, rgba);
}

// Keep shot i inside its neighbours (shots never overlap) and at least 0.25 s long.
void fit_shot(cine::Edit &e, int i)
{
    if (i < 0 || i >= int(e.shots.size()))
        return;
    cine::Shot &s = e.shots[size_t(i)];
    float lo = i > 0 ? e.shots[size_t(i - 1)].t1 : 0;
    float hi = i + 1 < int(e.shots.size()) ? e.shots[size_t(i + 1)].t0 : 1e9f;
    s.t0 = std::clamp(s.t0, lo, hi - 0.25f);
    s.t1 = std::clamp(s.t1, s.t0 + 0.25f, hi);
}

// The precise readout (top centre) and, with G, composition guides: centre cross, thirds and the true
// horizon projected from the camera's exact angles.
// ----- the camera path in 3D ------------------------------------------------------------------------------------
// Projected through the view on screen (the game's camera looks along -Z of its (right, up, back) basis). The
// overlay has no depth test, so the path also shows through walls; it fades with distance.

struct Projector {
    V3 eye, r, up, back;
    float f, cx, cy, aspect;
    explicit Projector(const PathView &v, float W, float H)
    {
        eye = v.eye;
        cine::cols_from_quat(v.q, r, up, back);
        f = (W / 2) / std::tan(std::clamp(v.fov, 1.0f, 170.0f) * cine::kDeg / 2);
        cx = W / 2;
        cy = H / 2;
        aspect = W > 0 ? H / W : 0.5625f;
    }
    V3 cam(V3 p) const  // camera space: x right, y up, z forward (depth)
    {
        V3 d = p - eye;
        return {cine::dot(d, r), cine::dot(d, up), -cine::dot(d, back)};
    }
    void screen(V3 c, float &sx, float &sy) const
    {
        sx = cx + c.x / c.z * f;
        sy = cy - c.y / c.z * f;
    }
};

constexpr float kPathNear = 0.2f;

uint32_t with_alpha(uint32_t rgba, float a) { return (rgba & 0xffffff00u) | uint32_t(std::clamp(a, 0.0f, 1.0f) * 255); }
float depth_alpha(float z) { return std::clamp(1.2f - z / 300.0f, 0.3f, 1.0f); }

// A 3D segment, cut at the near plane.
void line3(SwoftyPaint *p, const Projector &pr, V3 a, V3 b, float width, uint32_t rgba)
{
    V3 ca = pr.cam(a), cb = pr.cam(b);
    if (ca.z < kPathNear && cb.z < kPathNear)
        return;
    if (ca.z < kPathNear)
        ca = ca + (cb - ca) * ((kPathNear - ca.z) / (cb.z - ca.z));
    else if (cb.z < kPathNear)
        cb = cb + (ca - cb) * ((kPathNear - cb.z) / (ca.z - cb.z));
    float x0, y0, x1, y1;
    pr.screen(ca, x0, y0);
    pr.screen(cb, x1, y1);
    ov::line(p, x0, y0, x1, y1, width, with_alpha(rgba, depth_alpha(std::min(ca.z, cb.z)) * (rgba & 0xff) / 255.0f));
}

void dot3(SwoftyPaint *p, const Projector &pr, V3 a, float radius, uint32_t rgba)
{
    V3 c = pr.cam(a);
    if (c.z < kPathNear)
        return;
    float x, y;
    pr.screen(c, x, y);
    ov::circle(p, x, y, radius, with_alpha(rgba, depth_alpha(c.z) * (rgba & 0xff) / 255.0f));
}

// A key: a small camera frustum where it stands, looking where it looks.
void frustum3(SwoftyPaint *p, const Projector &pr, const cine::PathKey &k, float width, uint32_t rgba)
{
    V3 r, up, back;
    cine::cols_from_quat(k.q, r, up, back);
    const float L = 1.6f;
    float hw = std::tan(std::clamp(k.fov, 5.0f, 140.0f) * cine::kDeg / 2) * L, hh = hw * pr.aspect;
    V3 c = k.eye + back * -L;
    V3 corner[4] = {c + r * hw + up * hh, c - r * hw + up * hh, c - r * hw - up * hh, c + r * hw - up * hh};
    for (int i = 0; i < 4; i++) {
        line3(p, pr, k.eye, corner[i], width, rgba);
        line3(p, pr, corner[i], corner[(i + 1) % 4], width, rgba);
    }
    line3(p, pr, corner[0] + up * (hh * 0.6f), corner[1] + up * (hh * 0.6f), width, rgba);  // which way is up
}

// Path shaping on the selected shot, in the 3D view (one drag = one undo step):
// - a key's dot: drag moves the key (the path follows); Ctrl+drag also pulls its neighbours with a soft falloff
//   (by time, 1.5 s); double-click goes to it;
// - a blue handle (a segment's middle): drag bends that segment; double-click straightens it;
// - the line itself: double-click adds an anchor (a key there, path unchanged) to drag.
// Drag = across the screen, Shift+drag = towards / away from the camera.
struct SoftKey {
    float t, w;
    V3 pos0, bx, by, bz;
};
struct BendDrag {
    bool on, is_key;
    int shot;
    float key_t;               // which key (by time; indices change when keys are added)
    V3 v0, bx, by, bz;         // the bend or position at the press, the shot frame's axes in the world there
    V3 cr, cu, cf;             // the camera's right, up, forward at the press
    float mx0, my0, depth, f;  // the press, the point's depth, the focal length (pixels)
    std::vector<SoftKey> soft; // Ctrl+drag on a key: the others, with their weights
    int axis = -1;             // an arrow drag: 0 / 1 / 2 = the shot frame's x (right), y (up), z (forward)
    float sdx = 0, sdy = 0;    // arrow drag: the arrow's direction on screen (unit)
    float px_per_m = 1;        // arrow drag: pixels per metre along it at the press
} g_bend;

// The bend handle picked last on the selected shot (its arrows show there), by its segment's key time.
float g_sel_handle_t = -1;
int g_sel_handle_shot = -1;

int key_by_time(const cine::Shot &s, float t)
{
    for (int i = 0; i < int(s.keys.size()); i++)
        if (std::fabs(s.keys[size_t(i)].t - t) < 1e-3f)
            return i;
    return -1;
}

void draw_paths(oui::Ui &u, SwoftyPaint *p, cine::Edit &ed)
{
    std::vector<PathDraw> paths;
    PathView view;
    {
        std::lock_guard<std::mutex> lk(g_path_mx);
        if (!g_path_valid || !g_show_path.load()) {
            g_bend.on = false;
            return;
        }
        paths = g_path_draw;
        view = g_path_view;
    }
    float W = 0, H = 0;
    ov::size(p, &W, &H);
    Projector pr(view, W, H);
    const auto &in = u.in;
    ov::Rect safe = ov::safe_area(p);
    float timeline_top = safe.y + safe.h - u.u(kTimelineH);
    bool over_ui = in.y >= timeline_top || (in.x >= safe.x + safe.w - u.u(510) && in.y >= safe.y + u.u(80)) ||
                   (g_props_w > 0 && in.x <= safe.x + g_props_w && in.y >= safe.y + u.u(80)) ||
                   g_pick_driver || g_edits_menu || g_ui.active || g_drag != Drag::none;

    // what is under the mouse on the selected shot's path: a key's dot or a handle (nearest within 14 px)
    const PathDraw *sel_path = nullptr;
    for (const auto &pd : paths)
        if (pd.selected && pd.shot >= 0 && pd.shot < int(ed.shots.size()))
            sel_path = &pd;
    const cine::PathHandle *hover = nullptr;
    const cine::PathKey *hover_key = nullptr;
    auto screen_d = [&](V3 at, float &d) {
        V3 c = pr.cam(at);
        if (c.z < kPathNear)
            return false;
        float sx, sy;
        pr.screen(c, sx, sy);
        d = std::hypot(in.x - sx, in.y - sy);
        return true;
    };
    float best = u.u(14), d = 0;
    if (sel_path) {
        for (const auto &k : sel_path->ps.keys)
            if (screen_d(k.eye, d) && d < best)
                best = d, hover_key = &k, hover = nullptr;
        for (const auto &h : sel_path->ps.handles)
            if (screen_d(h.at, d) && d < best)
                best = d, hover = &h, hover_key = nullptr;
    }
    // The arrows of the selected key (else the picked bend handle): one per axis of the shot frame, about the same
    // size on screen whatever the distance. Dragging one moves the key (or bend) along that axis only.
    struct Arrow {
        V3 from, to;
        int axis;
    } arrows[3];
    int n_arrows = 0, hover_arrow = -1;
    V3 arrow_o{}, arrow_ax[3];
    float arrow_len = 0, arrow_t = -1;  // arrow_t: the key's time, or the handle's segment's key time
    bool arrow_on_key = false;
    if (sel_path) {
        V3 o{}, ax[3];
        bool have = false;
        for (const auto &k : sel_path->ps.keys)
            if (k.selected)
                o = k.eye, ax[0] = k.bx, ax[1] = k.by, ax[2] = k.bz, have = true, arrow_on_key = true, arrow_t = k.t;
        if (!have && g_sel_handle_shot == sel_path->shot)
            for (const auto &h : sel_path->ps.handles)
                if (std::fabs(h.key_t - g_sel_handle_t) < 1e-3f)
                    o = h.at, ax[0] = h.bx, ax[1] = h.by, ax[2] = h.bz, have = true, arrow_t = h.key_t;
        V3 oc = pr.cam(o);
        if (have && oc.z > kPathNear) {
            float len = oc.z * 90.0f / pr.f;  // about 90 px long
            arrow_o = o, arrow_len = len;
            for (int a = 0; a < 3; a++)
                arrow_ax[a] = ax[a];
            float best_a = u.u(9);
            for (int a = 0; a < 3; a++) {
                arrows[n_arrows++] = {o + ax[a] * (len * 0.12f), o + ax[a] * len, a};
                V3 c0 = pr.cam(arrows[a].from), c1 = pr.cam(arrows[a].to);
                if (c0.z < kPathNear || c1.z < kPathNear)
                    continue;
                float x0, y0, x1, y1;
                pr.screen(c0, x0, y0);
                pr.screen(c1, x1, y1);
                float vx = x1 - x0, vy = y1 - y0, l2 = vx * vx + vy * vy;
                float s = l2 > 1 ? std::clamp(((in.x - x0) * vx + (in.y - y0) * vy) / l2, 0.0f, 1.0f) : 0;
                float dd = std::hypot(in.x - (x0 + vx * s), in.y - (y0 + vy * s));
                if (dd < best_a)
                    best_a = dd, hover_arrow = a;
            }
        }
    }
    if (hover_arrow >= 0)  // the arrows win over the dots and handles under them
        hover = nullptr, hover_key = nullptr;
    bool press = !over_ui && !g_bend.on && sel_path && (in.pressed & ov::mouse::left);
    static ULONGLONG last_click;
    static float last_x, last_y;
    bool dbl = press && GetTickCount64() - last_click < 400 && std::hypot(in.x - last_x, in.y - last_y) < u.u(8);
    if (press) {
        last_click = dbl ? 0 : GetTickCount64();
        last_x = in.x;
        last_y = in.y;
    }
    if (press && sel_path) {
        cine::Shot &s = ed.shots[size_t(sel_path->shot)];
        auto start = [&](bool is_key, float key_t, V3 v0, V3 at, V3 bx, V3 by, V3 bz) {
            V3 c = pr.cam(at);
            g_bend = {true, is_key, sel_path->shot, key_t, v0, bx, by, bz, pr.r, pr.up, pr.back * -1,
                      in.x, in.y, c.z, pr.f, {}};
        };
        if (hover_arrow >= 0) {  // an arrow: move along that axis only
            int i = key_by_time(s, arrow_t);
            if (i >= 0) {
                cine::ShotKey &k = s.keys[size_t(i)];
                if (arrow_on_key) {
                    if (!k.has_orig)
                        k.orig = k.pos, k.has_orig = true;
                    start(true, arrow_t, k.pos, arrow_o, arrow_ax[0], arrow_ax[1], arrow_ax[2]);
                } else {
                    start(false, arrow_t, k.bend, arrow_o, arrow_ax[0], arrow_ax[1], arrow_ax[2]);
                }
                float x0, y0, x1, y1;
                pr.screen(pr.cam(arrow_o), x0, y0);
                pr.screen(pr.cam(arrow_o + arrow_ax[hover_arrow] * arrow_len), x1, y1);
                float l = std::max(1.0f, std::hypot(x1 - x0, y1 - y0));
                g_bend.axis = hover_arrow;
                g_bend.sdx = (x1 - x0) / l, g_bend.sdy = (y1 - y0) / l;
                g_bend.px_per_m = l / std::max(1e-3f, arrow_len);
            }
        } else if (hover && dbl) {  // straighten
            if (int i = key_by_time(s, hover->key_t); i >= 0) {
                s.keys[size_t(i)].bend = {};
                mark_dirty();
                toast("Segment straightened");
            }
        } else if (hover) {
            for (auto &k : s.keys)  // the handle is picked now (its arrows), not a key
                k.selected = false;
            g_sel_handle_t = hover->key_t, g_sel_handle_shot = sel_path->shot;
            start(false, hover->key_t, hover->bend, hover->at, hover->bx, hover->by, hover->bz);
        } else if (hover_key && dbl) {  // go to the key
            post({CmdKind::Seek, hover_key->t});
        } else if (hover_key) {
            int i = key_by_time(s, hover_key->t);
            if (i >= 0) {
                for (auto &k : s.keys)
                    k.selected = false;
                s.keys[size_t(i)].selected = true;
                g_sel_handle_t = -1;
                if (!s.keys[size_t(i)].has_orig)  // remember where it was, for "Reset position"
                    s.keys[size_t(i)].orig = s.keys[size_t(i)].pos, s.keys[size_t(i)].has_orig = true;
                start(true, hover_key->t, s.keys[size_t(i)].pos, hover_key->eye, hover_key->bx, hover_key->by,
                      hover_key->bz);
                if (in.key_down(VK_CONTROL))  // soft: the neighbours follow, less the further they are in time
                    for (const auto &o : sel_path->ps.keys) {
                        int j = key_by_time(s, o.t);
                        float w = std::exp(-std::pow((o.t - hover_key->t) / 1.5f, 2.0f));
                        if (j >= 0 && j != i && w > 0.02f)
                            g_bend.soft.push_back({o.t, w, s.keys[size_t(j)].pos, o.bx, o.by, o.bz});
                    }
            }
        } else if (dbl) {  // on the line: add an anchor there
            const auto &pts = sel_path->ps.pts;
            float line_best = u.u(10), at_t = -1;
            for (size_t i = 0; i < pts.size() && i < sel_path->ps.pt_t.size(); i++)
                if (screen_d(pts[i], d) && d < line_best)
                    line_best = d, at_t = sel_path->ps.pt_t[i];
            if (at_t >= 0) {
                int i = cine::insert_key(s, at_t);
                if (i >= 0) {
                    mark_dirty();
                    toast("Anchor added at " + mmss(at_t) + ": drag its dot to shape the path");
                } else {
                    toast("No anchor there (too near a key, or outside the keys)");
                }
            }
        }
    }
    if (g_bend.on) {
        bool alive = g_bend.shot >= 0 && g_bend.shot < int(ed.shots.size());
        int i = alive ? key_by_time(ed.shots[size_t(g_bend.shot)], g_bend.key_t) : -1;
        if (!in.held() || i < 0) {
            g_bend.on = false;
            mark_dirty();  // saved (and one history step) once the drag is over
        } else {
            cine::Shot &s = ed.shots[size_t(g_bend.shot)];
            float k = g_bend.depth / g_bend.f, dx = in.x - g_bend.mx0, dy = in.y - g_bend.my0;
            V3 wd = in.key_down(VK_SHIFT) ? g_bend.cf * (-dy * k) : (g_bend.cr * dx - g_bend.cu * dy) * k;
            auto local = [&](V3 bx, V3 by, V3 bz) { return V3{cine::dot(wd, bx), cine::dot(wd, by), cine::dot(wd, bz)}; };
            V3 v = g_bend.v0 + local(g_bend.bx, g_bend.by, g_bend.bz);
            if (g_bend.axis >= 0) {  // along the arrow only: the mouse's travel along it on screen
                float m = (dx * g_bend.sdx + dy * g_bend.sdy) / g_bend.px_per_m;
                v = g_bend.v0;
                (g_bend.axis == 0 ? v.x : g_bend.axis == 1 ? v.y : v.z) += m;
            }
            if (g_bend.is_key)
                s.keys[size_t(i)].pos = v;
            else
                s.keys[size_t(i)].bend = v;
            for (const auto &sk : g_bend.soft)
                if (int j = key_by_time(s, sk.t); j >= 0)
                    s.keys[size_t(j)].pos = sk.pos0 + local(sk.bx, sk.by, sk.bz) * sk.w;
        }
    }

    for (const auto &pd : paths) {
        uint32_t col = pd.selected ? 0xffffffff : (shot_colour(pd.type) | 0xff);
        const auto &pts = pd.ps.pts;
        float w = u.u(pd.selected ? 7 : 5);
        for (size_t i = 1; i < pts.size(); i++)  // a dark outline first, so the line reads on bright track and sky
            line3(p, pr, pts[i - 1], pts[i], w + u.u(4), 0x000000a0);
        const auto &clip = pd.ps.clip;
        for (size_t i = 1; i < pts.size(); i++) {  // into the track / a barrier: red; skimming the ground: orange
            uint8_t cl = i < clip.size() ? clip[i] : 0;
            line3(p, pr, pts[i - 1], pts[i], cl ? w + u.u(2) : w, cl == 2 ? 0xff3b30ff : cl == 1 ? 0xffa726ff : col);
        }
        for (size_t i = 1; i < pts.size() && i < clip.size(); i++)  // where each red stretch begins
            if (clip[i] == 2 && clip[i - 1] != 2) {
                dot3(p, pr, pts[i], u.u(11), 0x000000c0);
                dot3(p, pr, pts[i], u.u(8), 0xff3b30ff);
            }
        for (V3 t : pd.ps.ticks) {
            dot3(p, pr, t, u.u(7), 0x000000b0);
            dot3(p, pr, t, u.u(5), 0xffffffff);
        }
        for (const auto &k : pd.ps.keys) {  // bold, on a dark outline like the path
            float fw = u.u(k.selected ? 5.0f : 3.5f);
            frustum3(p, pr, k, fw + u.u(3), 0x000000a0);
            frustum3(p, pr, k, fw, k.selected ? kAccent : 0xe8e8f0ff);
        }
        if (!pd.selected)
            continue;
        for (const auto &k : pd.ps.keys) {  // the keys' dots, to drag
            bool active = (g_bend.on && g_bend.is_key && g_bend.shot == pd.shot && std::fabs(g_bend.key_t - k.t) < 1e-3f) ||
                          &k == hover_key;
            dot3(p, pr, k.eye, u.u(active ? 10 : 8), 0x000000c0);
            dot3(p, pr, k.eye, u.u(active ? 8 : 6), active ? kAccent : 0xffffffff);
        }
        for (const auto &h : pd.ps.handles) {  // path-shaping handles: a ring, and where the middle was if bent
            bool active =
                (g_bend.on && !g_bend.is_key && g_bend.shot == pd.shot && std::fabs(g_bend.key_t - h.key_t) < 1e-3f) ||
                &h == hover;
            if (cine::len(h.bend) > 0.01f) {
                line3(p, pr, h.rest, h.at, u.u(2), 0xffffff90);
                dot3(p, pr, h.rest, u.u(3), 0xffffffb0);
            }
            dot3(p, pr, h.at, u.u(active ? 11 : 9), 0x000000c0);
            dot3(p, pr, h.at, u.u(active ? 9 : 7), active ? kAccent : 0x4fc3f7ff);
            dot3(p, pr, h.at, u.u(3), 0xffffffff);
        }
    }
    // the arrows (on top): red right, green up, blue forward, in the shot's frame
    const uint32_t acol[3] = {0xff5252ff, 0x69f0aeff, 0x448affff};
    for (int a = 0; a < n_arrows; a++) {
        bool hot = (g_bend.on && g_bend.axis == a) || (!g_bend.on && hover_arrow == a);
        V3 c0 = pr.cam(arrows[a].from), c1 = pr.cam(arrows[a].to);
        if (c0.z < kPathNear || c1.z < kPathNear)
            continue;
        float x0, y0, x1, y1;
        pr.screen(c0, x0, y0);
        pr.screen(c1, x1, y1);
        float vx = x1 - x0, vy = y1 - y0, l = std::max(1.0f, std::hypot(vx, vy));
        vx /= l, vy /= l;
        float hw = u.u(hot ? 9 : 7), hl = u.u(hot ? 16 : 13), w = u.u(hot ? 5 : 3.5f);
        float bx = x1 - vx * hl, by = y1 - vy * hl;  // the head: a triangle at the tip
        ov::line(p, x0, y0, bx, by, w + u.u(3), 0x000000a0);
        ov::line(p, x0, y0, bx, by, w, hot ? 0xffffffff : acol[a]);
        for (int side : {-1, 1})
            ov::line(p, x1, y1, bx - vy * hw * float(side), by + vx * hw * float(side), w, hot ? 0xffffffff : acol[a]);
    }
}

void readout(oui::Ui &u, SwoftyPaint *p, const Status &st)
{
    float W = 0, H = 0;
    ov::size(p, &W, &H);
    ov::Rect safe = ov::safe_area(p);
    if (g_guides.load()) {
        uint32_t g = 0xffffff40;
        for (int i = 1; i < 3; i++) {
            ov::line(p, W * i / 3, 0, W * i / 3, H, 1, g);
            ov::line(p, 0, H * i / 3, W, H * i / 3, 1, g);
        }
        float cx = W / 2, cy = H / 2, cr = u.u(14);
        ov::line(p, cx - cr, cy, cx + cr, cy, u.u(2), 0xffffffc0);
        ov::line(p, cx, cy - cr, cx, cy + cr, u.u(2), 0xffffffc0);
        float focal = (W / 2) / std::tan(st.fov * cine::kDeg / 2);
        float off = std::tan(st.pitch * cine::kDeg) * focal;
        float a = -st.wroll * cine::kDeg;
        float dx = std::cos(a), dy = std::sin(a);
        float hx = cx - std::sin(a) * off, hy = cy + std::cos(a) * off;
        bool level = std::fabs(st.wroll) < 0.05f;
        ov::line(p, hx - dx * W, hy - dy * W, hx + dx * W, hy + dy * W, u.u(2), level ? 0x50ff78c0 : 0xffd23cc0);
    }
    char l1[220], l2[220];
    snprintf(l1, sizeof l1, "%s   Yaw %8.2f   Pitch %7.2f   Roll %7.2f   FOV %6.2f   fly %.1f m/s",
             st.orbit ? "ORBITING" : st.fly ? "FLYING" : "WATCHING", st.yaw, st.pitch, st.wroll, st.fov, st.fly_speed);
    if (st.near_car >= -1)
        snprintf(l2, sizeof l2, "vs %s:  yaw %+7.2f  pitch %+6.2f  roll %+6.2f   height %5.2f m   dist %6.1f m",
                 car_label(st.near_car).c_str(), st.c_yaw, st.c_pitch, st.c_roll, st.c_height, st.c_dist);
    else
        snprintf(l2, sizeof l2, "no car within 300 m");
    float w = u.u(900), h = u.u(62);
    ov::Rect box{safe.x + (safe.w - w) / 2, safe.y + u.u(8), w, h};
    ov::rect_rounded(p, box.x, box.y, box.w, box.h, u.u(8), 0x0d0d14d8);
    ov::TextStyle ts;
    ts.size = u.u(18);
    ts.rgba = std::fabs(st.wroll) < 0.05f ? 0x9dffadff : 0xffffffff;
    ts.align = ov::align::center | ov::align::top;
    ov::text_in(p, {box.x, box.y + u.u(7), box.w, u.u(24)}, ts, l1);
    ts.size = u.u(16);
    ts.rgba = std::fabs(st.c_roll) < 0.05f && st.near_car >= -1 ? 0x9dffadff : 0xffffffa8;
    ov::text_in(p, {box.x, box.y + u.u(34), box.w, u.u(22)}, ts, l2);
}

void help_panel(oui::Ui &u, SwoftyPaint *p)
{
    static const char *const kb[][2] = {
        {"EDITOR", nullptr},
        {"F", "fly the camera / watch the edit"},
        {"K", "add a key here (a new Free path if no shot)"},
        {"H / G", "this help / guides (thirds, horizon)"},
        {"U", "hide / show the editor's UI (preview)"},
        {"J", "show / hide the camera path in 3D"},
        {"PATH (selected shot, J)", nullptr},
        {"Drag a key's white dot", "move the key (Ctrl: neighbours follow)"},
        {"Drag a blue handle", "bend that segment"},
        {"Double-click the line", "add an anchor (a key) there"},
        {"Double-click handle / dot", "straighten / go to the key"},
        {"Shift while dragging", "nearer / farther instead of across"},
        {"Arrows on the picked key / handle", "drag: along that axis only"},
        {"Red / orange path", "into the track or a barrier / skims it"},
        {"F9", "open / close the editor (stops Final Cut, a render)"},
        {"FLY", nullptr},
        {"W A S D, Q / E", "move (the camera's own axes)"},
        {"Right mouse", "hold and move to look"},
        {"Shift / Ctrl", "faster / fine"},
        {"Mouse wheel", "fly speed"},
        {"Z / C, R / V", "roll, zoom in / out"},
        {"L / Home", "align to the car / level horizon"},
        {"Shift+L / Shift+Home", "flat on / roll to the track"},
        {"T", "ride / leave the followed car"},
        {"O", "orbit the car: look at it, move around it"},
        {",  .", "previous / next key (or double-click a key)"},
        {"REPLAY", nullptr},
        {"Space / P", "freeze / run"},
        {"[  ]", "-/+ 0.1 s (Shift 1 s, Ctrl 1 frame)"},
        {"-  =", "replay speed down / up"},
        {"TIMELINE", nullptr},
        {"Click / drag a shot", "select / move it, drag its edges to trim"},
        {"Edge two shots share", "trims the shot on the side you grab"},
        {"Keys lane", "click, drag, box-select the shot's keys"},
        {"Blue dots (slider keys)", "double-click: go to that key"},
        {"Shift+click / drag on SHOTS", "select several shots"},
        {"Ctrl+C / Ctrl+V, Del", "copy / paste keys or shots, delete"},
        {"Ctrl+Z / Ctrl+Y", "undo / redo"},
        {"B / N (Alt: no snap)", "split at the playhead / snap on-off"},
        {"Wheel / Ctrl+wheel", "scroll / zoom the timeline"},
        {"SLIDERS", nullptr},
        {"Diamond left of a slider", "key the value here / remove that key"},
        {"PROPERTIES (left)", "a row of keys per value; click between two"},
        {"", "keys: shape that segment's curve below"},
        {"Key in a row", "double-click: go to it; right-click: remove"},
        {"Ctrl + click the diamond", "clear the value's keys"},
        {"Ctrl + drag", "fine (20x slower)"},
        {"Wheel / Shift+wheel", "one step (1 cm, 0.5 deg) / ten"},
    };
    static const char *const pad[][2] = {
        {"EDITOR", nullptr},
        {"Start", "fly the camera / watch the edit"},
        {"A", "add a key here"},
        {"L3 + R3", "open / close the editor"},
        {"FLY", nullptr},
        {"Left stick, LT / RT", "move, down / up"},
        {"Right stick", "look"},
        {"Hold L-stick in", "fine"},
        {"Right stick click", "fly speed preset"},
        {"LB / RB", "roll"},
        {"LB + RB together", "flat on the track surface"},
        {"D-pad up / down", "zoom"},
        {"Back", "level horizon"},
        {"X", "ride / leave the followed car"},
        {"B", "orbit the car: look at it, move around it"},
        {"LB + D-pad left/right", "previous / next key"},
        {"REPLAY", nullptr},
        {"Y", "freeze / run"},
        {"D-pad left / right", "-/+ 0.1 s"},
        {"Y + D-pad left/right", "replay speed"},
    };
    bool use_pad = g_device.load() == 1;
    const auto *rows = use_pad ? pad : kb;
    size_t count = use_pad ? std::size(pad) : std::size(kb);
    // two columns, split at the section heading nearest the middle, so the table fits above the timeline
    size_t split = count;
    for (int off = 0, mid = int(count / 2); off <= mid && split == count; off++)
        for (int i : {mid + off, mid - off})
            if (i > 0 && i < int(count) && !rows[size_t(i)][1]) {
                split = size_t(i);
                break;
            }
    auto height_of = [&](size_t a, size_t b) {
        float hh = 0;
        for (size_t i = a; i < b; i++)
            hh += rows[i][1] ? 1.0f : 1.2f;
        return hh;
    };
    ov::Rect safe = ov::safe_area(p);
    float col_w = u.u(560), gap = u.u(30), w = col_w * 2 + gap, small = u.u(24), key_w = u.u(250);
    float h = u.u(56) + small * std::max(height_of(0, split), height_of(split, count)) + u.u(10);
    float top = safe.y + u.u(84), room = safe.y + safe.h - u.u(kTimelineH) - u.u(10) - top;
    if (h > room)  // still too tall: shrink the rows to fit
        small *= room / h, h = room;
    ov::Rect c = oui::panel(u, {safe.x + (safe.w - w) / 2, top, w, h}, use_pad ? "CONTROLS (CONTROLLER)" : "CONTROLS");
    for (int col = 0; col < 2; col++) {
        float x = c.x + col * (col_w + gap), y = c.y;
        for (size_t i = col == 0 ? 0 : split; i < (col == 0 ? split : count); i++) {
            if (!rows[i][1]) {
                y += small * 0.2f;
                oui::label(u, {x, y, col_w, small}, rows[i][0], kAccent);
                y += small;
                continue;
            }
            oui::label(u, {x, y, key_w, small}, rows[i][0], kText);
            oui::label(u, {x + key_w, y, col_w - key_w, small}, rows[i][1], kDim);
            y += small;
        }
    }
}

int g_want_undo;     // the transport's Undo (1) / Redo (2), done by paint
bool g_want_split;   // the transport's Split

void timeline(oui::Ui &u, SwoftyPaint *p, const Status &st, cine::Edit &ed)
{
    const auto &in = u.in;
    ov::Rect safe = ov::safe_area(p);
    float H = u.u(kTimelineH);
    ov::Rect box{safe.x, safe.y + safe.h - H, safe.w, H};
    ov::rect_rounded(p, box.x, box.y, box.w, box.h, u.u(10), 0x0d0d14ea);
    float pad = u.u(10), row = u.u(34);
    ULONGLONG now_ms = GetTickCount64();
    int ns = int(ed.shots.size());
    cine::Shot *sel = g_shot >= 0 && g_shot < ns ? &ed.shots[size_t(g_shot)] : nullptr;

    // transport row
    float bx = box.x + pad, by = box.y + pad, bw = u.u(52);
    auto btn = [&](const char *label, float w = 0) {
        float ww = w > 0 ? w : bw;
        bool hit = oui::button(u, {bx, by, ww, row}, label);
        bx += ww + u.u(6);
        return hit;
    };
    if (btn("|<") && ns) {  // the previous shot's start
        int i = ns - 1;
        while (i > 0 && ed.shots[size_t(i)].t0 >= st.t - 0.05f)
            i--;
        post({CmdKind::Seek, ed.shots[size_t(i)].t0});
        g_shot = i;
    }
    if (btn("<<"))
        post({CmdKind::Seek, st.t - 5});
    if (btn(st.frozen ? "Run" : "Freeze", u.u(80)))
        post({CmdKind::Freeze});
    if (btn(">>"))
        post({CmdKind::Seek, st.t + 5});
    if (btn(">|") && ns) {  // the next shot's start
        int i = 0;
        while (i < ns - 1 && ed.shots[size_t(i)].t0 <= st.t + 0.05f)
            i++;
        post({CmdKind::Seek, ed.shots[size_t(i)].t0});
        g_shot = i;
    }
    // a button that is lit while `on`
    auto lit = [&](const char *label, float w, bool on) {
        if (on)
            ov::rect_rounded(p, bx - u.u(2), by - u.u(2), w + u.u(4), row + u.u(4), u.u(6), kAccent);
        return btn(label, w);
    };
    bx += u.u(8);
    {  // one button cycles the replay speed (also - / =)
        const float speeds[] = {0.1f, 0.25f, 0.5f, 1};
        char sl[32];
        snprintf(sl, sizeof sl, "Speed %gx", double(st.play_speed));
        if (btn(sl, u.u(112))) {
            int i = 0;
            while (i < 3 && speeds[i] < st.play_speed - 1e-3f)
                i++;
            post({CmdKind::PlaySpeed, speeds[(i + 1) % 4]});
        }
    }
    bx += u.u(8);
    if (lit(st.fly ? "Flying" : "Fly", u.u(76), st.fly))
        post({CmdKind::ToggleFly});
    g_play_menu_x = bx;
    if (lit("Play  v", u.u(80), g_play_menu || g_render_menu))
        g_play_menu = !g_play_menu, g_render_menu = g_options = g_edits_menu = g_pick_driver = false;
    bx += u.u(8);
    if (btn("Undo", u.u(60)))
        g_want_undo = 1;
    if (btn("Redo", u.u(60)))
        g_want_undo = 2;
    bx += u.u(8);
    if (lit("Options", u.u(84), g_options))
        g_options = !g_options, g_play_menu = g_render_menu = g_edits_menu = g_pick_driver = false;
    bx += u.u(8);
    // the right end: the time readout in a fixed slot, the edit's button before it (its name clipped to fit)
    float time_w = u.u(270), right = box.x + box.w - pad;
    float ew = std::max(u.u(120), right - time_w - u.u(8) - bx);
    ew = std::min(ew, u.u(240));
    {
        std::string en = std::string("Edit: ") + st.seq;
        size_t fit = size_t(std::max(4.0f, (ew - u.u(20)) / u.u(9)));
        if (en.size() > fit)
            en = en.substr(0, fit - 2) + "..";
        if (lit(en.c_str(), ew, g_edits_menu))
            g_edits_menu = !g_edits_menu, g_play_menu = g_render_menu = g_options = g_pick_driver = false;
    }
    // the time, plus only what isn't normal: frozen, or a speed other than 1x
    char line[160], extra[32] = "";
    if (st.frozen)
        snprintf(extra, sizeof extra, "  frozen");
    else if (std::fabs(st.speed - 1) > 1e-3f)
        snprintf(extra, sizeof extra, "  %gx", double(st.speed));
    snprintf(line, sizeof line, "%s / %s%s", mmss(st.t).c_str(), mmss(st.end).c_str(), extra);
    oui::label(u, {right - time_w, by, time_w, row}, line, kText, ov::align::right | ov::align::middle);

    // lanes and the scrollbar
    float L = box.x + u.u(64), R = box.x + box.w - pad;
    float ruler_y = by + row + u.u(8), ruler_h = u.u(22);
    float shot_y = ruler_y + ruler_h + u.u(4), shot_h = u.u(48);
    float key_y = shot_y + shot_h + u.u(4), key_h = u.u(34);
    float lanes_bottom = key_y + key_h;
    float sb_y = lanes_bottom + u.u(6), sb_h = u.u(12);
    oui::label(u, {box.x + pad, shot_y, u.u(54), shot_h}, "SHOTS", kDim);
    oui::label(u, {box.x + pad, key_y, u.u(54), key_h}, "KEYS", kDim);
    ov::rect(p, L, shot_y, R - L, shot_h, kLane);
    ov::rect(p, L, key_y, R - L, key_h, kLane);

    // the view: follows the playhead only while the replay runs and the timeline hasn't been touched for 3 s
    float span = std::max(1.0f, g_v1 - g_v0);
    float full = std::max({st.end, g_v1, 60.0f});
    if (!g_view_init) {
        g_v0 = std::max(0.0f, st.t - 10);
        g_v1 = g_v0 + 40;
        g_view_init = true;
    }
    if (g_drag == Drag::none && !st.frozen && now_ms - g_view_touched > 3000 && (st.t < g_v0 || st.t > g_v1)) {
        g_v0 = std::max(0.0f, st.t - span * 0.25f);
        g_v1 = g_v0 + span;
    }
    auto tx = [&](float t) { return L + (t - g_v0) / (g_v1 - g_v0) * (R - L); };
    auto xt = [&](float x) { return g_v0 + (x - L) / (R - L) * (g_v1 - g_v0); };
    auto set_view = [&](float v0, float sp) {
        g_v0 = std::clamp(v0, 0.0f, std::max(0.0f, full - sp));
        g_v1 = g_v0 + sp;
        span = sp;
        g_view_touched = now_ms;
    };
    bool over_lanes = in.x >= L && in.x <= R && in.y >= ruler_y && in.y <= lanes_bottom;
    bool over_sb = in.x >= L && in.x <= R && in.y >= sb_y - u.u(3) && in.y <= sb_y + sb_h + u.u(3);
    if ((over_lanes || over_sb) && in.wheel != 0) {
        if (in.key_down(VK_CONTROL)) {
            float at = xt(in.x), k = std::pow(1.25f, -in.wheel);
            float nsp = std::clamp(span * k, 2.0f, std::max(60.0f, full));
            set_view(at - (at - g_v0) * nsp / span, nsp);
        } else {
            set_view(g_v0 - in.wheel * span * 0.15f, span);
        }
    }

    // ruler
    const float steps[] = {0.25f, 0.5f, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
    float step = 600;
    for (float s : steps)
        if ((R - L) / (span / s) >= u.u(70)) {
            step = s;
            break;
        }
    ov::push_clip(p, L, ruler_y, R - L, lanes_bottom - ruler_y);
    ov::TextStyle ts;
    ts.size = u.u(14);
    ts.rgba = kDim;
    for (float t = std::floor(g_v0 / step) * step; t <= g_v1; t += step) {
        float x = tx(t);
        ov::line(p, x, ruler_y + ruler_h * 0.55f, x, lanes_bottom, 1, 0xffffff18);
        ov::text(p, x + u.u(3), ruler_y, ts, mmss(t));
    }

    // shots (drawn where a drag would put them)
    auto shot_span = [&](int i, float &a, float &b) {
        a = ed.shots[size_t(i)].t0;
        b = ed.shots[size_t(i)].t1;
        if (g_drag == Drag::shot_move && (i == g_shot || ed.shots[size_t(i)].selected))  // the group moves
            a += g_drag_dt, b += g_drag_dt;
        if (i == g_shot) {
            if (g_drag == Drag::shot_left)
                a = g_drag_a;
            if (g_drag == Drag::shot_right)
                b = g_drag_b;
        }
    };
    int hover_shot = -1, hover_edge = 0;  // edge: -1 left, 1 right
    ov::TextStyle lt;
    lt.size = u.u(15);
    lt.rgba = 0xffffffff;
    lt.align = ov::align::left | ov::align::middle;
    for (int i = 0; i < ns; i++) {
        float a, b;
        shot_span(i, a, b);
        float x0 = tx(a), x1 = tx(b);
        const cine::Shot &s = ed.shots[size_t(i)];
        ov::rect_rounded(p, x0 + 1, shot_y + u.u(3), std::max(2.0f, x1 - x0 - 2), shot_h - u.u(6), u.u(5),
                         shot_colour(s.type));
        if (i == g_shot)
            ov::rect_rounded_outline(p, x0 + 1, shot_y + u.u(3), std::max(2.0f, x1 - x0 - 2), shot_h - u.u(6), u.u(5),
                                     u.u(2), kAccent);
        else if (s.selected)
            ov::rect_rounded_outline(p, x0 + 1, shot_y + u.u(3), std::max(2.0f, x1 - x0 - 2), shot_h - u.u(6), u.u(5),
                                     u.u(2), 0xffffffd0);
        float lx = std::max(x0, L);  // the label stays inside the lane when the shot starts off to the left
        ov::push_clip(p, lx + u.u(6), shot_y, std::max(0.0f, std::min(x1, R) - lx - u.u(12)), shot_h);
        ov::text_in(p, {lx + u.u(8), shot_y, std::max(0.0f, x1 - lx - u.u(12)), shot_h}, lt, shot_label(s));
        ov::pop_clip(p);
        if (in.y >= shot_y && in.y <= shot_y + shot_h && in.x >= x0 - u.u(4) && in.x <= x1 + u.u(4)) {
            hover_shot = i;
            hover_edge = std::fabs(in.x - x0) < u.u(7) ? -1 : std::fabs(in.x - x1) < u.u(7) ? 1 : 0;
        }
    }
    {  // where the shown paths go into the track or a barrier: red ticks along the bottom of their shots
        std::lock_guard<std::mutex> lk(g_path_mx);
        for (const auto &pd : g_path_draw)
            for (size_t i = 0; i < pd.ps.clip.size() && i < pd.ps.pt_t.size(); i++)
                if (pd.ps.clip[i] == 2) {
                    float x = tx(pd.ps.pt_t[i]);
                    if (x >= L && x <= R)
                        ov::rect(p, x - 1, shot_y + shot_h - u.u(9), 3, u.u(6), 0xff3b30ff);
                }
    }
    if (hover_shot >= 0 && hover_edge != 0) {  // show the trim handle
        float a, b;
        shot_span(hover_shot, a, b);
        float x = tx(hover_edge < 0 ? a : b);
        ov::line(p, x, shot_y + u.u(2), x, shot_y + shot_h - u.u(2), u.u(3), 0xffffffff);
    }

    // the selected shot's keys
    int hover_key = -1;
    float ky = key_y + key_h / 2;
    if (sel && sel->keyed()) {
        for (int i = 0; i < int(sel->keys.size()); i++) {
            const auto &k = sel->keys[size_t(i)];
            float t = k.t + (g_drag == Drag::key_move && k.selected ? g_drag_dt : 0) +
                      (g_drag == Drag::shot_move ? g_drag_dt : 0);
            float x = tx(t);
            bool near_ = std::fabs(in.x - x) < u.u(9) && std::fabs(in.y - ky) < u.u(13);
            if (near_)
                hover_key = i;
            draw_diamond(p, x, ky, u.u(k.selected ? 10 : 8), k.selected ? kAccent : near_ ? 0xffffffff : 0xd0d0e0ff);
        }
    } else if (sel && sel->tracks.empty()) {
        ov::TextStyle ht = lt;
        ht.rgba = kDim;
        ov::text_in(p, {L + u.u(8), key_y, R - L, key_h}, ht,
                    sel->type == cine::ShotType::track        ? "Track cam: no keys (place it in the inspector)"
                    : sel->type == cine::ShotType::transition ? "Transition: no keys (shape its curves in the inspector)"
                                                              : "Orbit: no keys (radius, height and period in the inspector)");
    }
    if (sel) {  // the keyed slider values' keys: dots under the lane; double-click one to go to it
        static ULONGLONG dot_click;
        static float dot_x;
        bool on_lane = in.y >= key_y && in.y <= key_y + key_h && in.x >= L && in.x <= R;
        bool pressed = on_lane && (in.pressed & ov::mouse::left);
        bool dbl = pressed && GetTickCount64() - dot_click < 400 && std::fabs(in.x - dot_x) < u.u(8);
        if (pressed)
            dot_click = dbl ? 0 : GetTickCount64(), dot_x = in.x;
        float best = u.u(10), best_t = -1;
        for (const auto &tr : sel->tracks)
            for (const auto &pk : tr.keys) {
                float t = pk.t + (g_drag == Drag::shot_move ? g_drag_dt : 0);
                bool hot = on_lane && std::fabs(in.x - tx(t)) < u.u(10);
                ov::circle(p, tx(t), key_y + key_h - u.u(5), u.u(hot ? 5.0f : 3.5f), 0x4fc3f7ff);
                if (std::fabs(in.x - tx(t)) < best)
                    best = std::fabs(in.x - tx(t)), best_t = pk.t;
            }
        if (dbl && best_t >= 0)
            post({CmdKind::Seek, best_t});
    }
    if (g_drag == Drag::marquee || g_drag == Drag::shot_marquee) {
        float a = std::min(g_marq_x, in.x), b = std::max(g_marq_x, in.x);
        float my = g_drag == Drag::marquee ? key_y : shot_y, mh = g_drag == Drag::marquee ? key_h : shot_h;
        ov::rect(p, a, my, b - a, mh, 0xe1060030);
        ov::rect_outline(p, a, my, b - a, mh, 1, 0xe10600c0);
    }
    if (g_snap_line >= 0 && (g_drag == Drag::shot_move || g_drag == Drag::shot_left || g_drag == Drag::shot_right ||
                             g_drag == Drag::key_move))
        ov::line(p, tx(g_snap_line), ruler_y, tx(g_snap_line), lanes_bottom, u.u(2), 0xffffffff);

    // the render range: a band between the in and out points, with brackets
    if (g_render_in >= 0 || g_render_out >= 0) {
        float a = g_render_in >= 0 ? g_render_in : g_v0, b = g_render_out >= 0 ? g_render_out : g_v1;
        if (b > a)
            ov::rect(p, tx(a), ruler_y, tx(b) - tx(a), lanes_bottom - ruler_y, 0xffc83c1c);
        for (int side = 0; side < 2; side++) {
            float t = side == 0 ? g_render_in : g_render_out;
            if (t < 0)
                continue;
            float x = tx(t), d = side == 0 ? u.u(8) : -u.u(8);
            ov::line(p, x, ruler_y, x, lanes_bottom, u.u(2), 0xffc83cff);
            ov::line(p, x, ruler_y, x + d, ruler_y, u.u(3), 0xffc83cff);
            ov::line(p, x, lanes_bottom, x + d, lanes_bottom, u.u(3), 0xffc83cff);
        }
    }
    // playhead
    float ph = g_drag == Drag::scrub ? g_scrub_t : st.t;
    float px = tx(ph);
    ov::line(p, px, ruler_y, px, lanes_bottom, u.u(2), kAccent);
    float tri[6] = {px - u.u(6), ruler_y, px + u.u(6), ruler_y, px, ruler_y + u.u(8)};
    ov::polygon(p, tri, 3, kAccent);
    ov::pop_clip(p);

    // scrollbar
    ov::rect_rounded(p, L, sb_y, R - L, sb_h, sb_h / 2, 0xffffff12);
    float th0 = L + g_v0 / full * (R - L), th1 = std::max(L + g_v1 / full * (R - L), L + g_v0 / full * (R - L) + u.u(16));
    ov::rect_rounded(p, th0, sb_y, th1 - th0, sb_h, sb_h / 2, g_drag == Drag::sbar ? 0xffffffa0 : 0xffffff60);
    float pmark = L + std::clamp(st.t / full, 0.0f, 1.0f) * (R - L);
    ov::line(p, pmark, sb_y, pmark, sb_y + sb_h, u.u(2), kAccent);

    // mouse
    bool lpress = in.pressed & ov::mouse::left, lheld = in.buttons & ov::mouse::left;
    bool rpress = in.pressed & ov::mouse::right, rheld = in.buttons & ov::mouse::right;
    bool shift = in.key_down(VK_SHIFT);
    if (lpress && g_drag == Drag::none) {
        if (over_sb) {
            g_drag = Drag::sbar;
            g_sbar_grab = in.x >= th0 && in.x <= th1 ? in.x - th0 : (th1 - th0) / 2;
        } else if (over_lanes && in.y < shot_y) {
            g_drag = Drag::scrub;
        } else if (over_lanes && in.y < key_y) {  // shots lane
            if (hover_shot >= 0 && shift) {  // add to / take from the selection
                cine::Shot &s = ed.shots[size_t(hover_shot)];
                if (g_shot >= 0 && g_shot < ns)
                    ed.shots[size_t(g_shot)].selected = true;
                s.selected = !s.selected;
                if (s.selected) {
                    g_shot = hover_shot;
                } else if (hover_shot == g_shot) {  // the next selected shot becomes the one in the inspector
                    g_shot = -1;
                    for (int i = 0; i < ns && g_shot < 0; i++)
                        if (ed.shots[size_t(i)].selected)
                            g_shot = i;
                }
            } else if (hover_shot >= 0) {
                cine::Shot &s = ed.shots[size_t(hover_shot)];
                if (!s.selected)
                    for (auto &o : ed.shots)
                        o.selected = false;
                s.selected = true;
                g_shot = hover_shot;
                g_drag = hover_edge < 0 ? Drag::shot_left : hover_edge > 0 ? Drag::shot_right : Drag::shot_move;
                g_drag_x0 = xt(in.x);
                g_drag_dt = 0;
                g_drag_a = s.t0;
                g_drag_b = s.t1;
                // an edge another shot touches: trim the shot on the cursor's side of it (it can get shorter, and
                // longer only into a gap: the neighbour is never cut)
                int nb = hover_edge < 0 ? hover_shot - 1 : hover_edge > 0 ? hover_shot + 1 : -1;
                if (nb >= 0 && nb < ns) {
                    float edge = hover_edge < 0 ? s.t0 : s.t1;
                    const cine::Shot &o = ed.shots[size_t(nb)];
                    bool cursor_left = in.x < tx(edge);
                    bool other_side = hover_edge < 0 ? cursor_left : !cursor_left;
                    if (std::fabs((hover_edge < 0 ? o.t1 : o.t0) - edge) < 1e-3f && other_side) {
                        for (auto &o2 : ed.shots)
                            o2.selected = false;
                        ed.shots[size_t(nb)].selected = true;
                        g_shot = nb;
                        g_drag = hover_edge < 0 ? Drag::shot_right : Drag::shot_left;
                        g_drag_a = o.t0;
                        g_drag_b = o.t1;
                    }
                }
            } else {
                g_drag = Drag::shot_marquee;
                g_marq_x = in.x;
            }
        } else if (over_lanes && sel && sel->keyed()) {  // keys lane
            if (hover_key >= 0) {
                auto &k = sel->keys[size_t(hover_key)];
                static ULONGLONG last_click;
                static int last_key = -1, last_shot = -1;
                if (hover_key == last_key && g_shot == last_shot && now_ms - last_click < 400)
                    post({CmdKind::Seek, k.t});
                last_click = now_ms;
                last_key = hover_key;
                last_shot = g_shot;
                if (shift) {
                    k.selected = !k.selected;
                } else {
                    if (!k.selected) {
                        for (auto &kk : sel->keys)
                            kk.selected = false;
                        k.selected = true;
                    }
                    g_drag = Drag::key_move;
                    g_drag_x0 = xt(in.x);
                    g_drag_dt = 0;
                    g_drag_keyt = k.t;
                }
            } else {
                g_drag = Drag::marquee;
                g_marq_x = in.x;
            }
        }
    }
    float lo = g_shot > 0 ? ed.shots[size_t(g_shot - 1)].t1 : 0;
    float hi = g_shot >= 0 && g_shot + 1 < ns ? ed.shots[size_t(g_shot + 1)].t0 : std::max(st.end, 1.0f);
    // Snapping (N; hold Alt to drag freely): to the playhead and the other shots' edges, within 8 pixels.
    bool snapping = g_snap.load() && !in.key_down(VK_MENU);
    float snap_thr = u.u(8) * (g_v1 - g_v0) / std::max(1.0f, R - L);
    g_snap_line = -1;
    // The offset that brings one of the times in `ts` onto a target, if one is near enough (else 0).
    auto snap_by = [&](std::initializer_list<float> ts, bool shot_edges) {
        float best = snap_thr, off = 0;
        auto try_target = [&](float target) {
            for (float t : ts)
                if (std::fabs(target - t) < best)
                    best = std::fabs(target - t), off = target - t, g_snap_line = target;
        };
        if (!snapping)
            return 0.0f;
        try_target(st.t);
        if (shot_edges)
            for (int i = 0; i < ns; i++)
                if (i != g_shot && !(g_drag == Drag::shot_move && ed.shots[size_t(i)].selected))
                    try_target(ed.shots[size_t(i)].t0), try_target(ed.shots[size_t(i)].t1);
        return off;
    };
    switch (g_drag) {
    case Drag::scrub:
        g_scrub_t = std::clamp(xt(in.x), 0.0f, std::max(st.end, 1.0f));
        if (!lheld) {
            post({CmdKind::Seek, g_scrub_t});
            g_drag = Drag::none;
        }
        break;
    case Drag::shot_move:
        if (sel) {
            // the group: the selected shots plus the one grabbed. It may not cross a shot outside it, go below 0
            // or past the replay's end.
            auto in_group = [&](int i) { return i == g_shot || ed.shots[size_t(i)].selected; };
            float dmin = -1e9f, dmax = 1e9f, gmin = 1e9f, gmax = -1e9f;
            for (int i = 0; i < ns; i++) {
                if (!in_group(i))
                    continue;
                const auto &gs = ed.shots[size_t(i)];
                gmin = std::min(gmin, gs.t0);
                gmax = std::max(gmax, gs.t1);
                dmin = std::max(dmin, -gs.t0);
                dmax = std::min(dmax, std::max(st.end, 1.0f) - gs.t1);
                for (int j = 0; j < ns; j++) {
                    if (in_group(j))
                        continue;
                    const auto &o = ed.shots[size_t(j)];
                    if (o.t1 <= gs.t0 + 1e-4f)
                        dmin = std::max(dmin, o.t1 - gs.t0);
                    else if (o.t0 >= gs.t1 - 1e-4f)
                        dmax = std::min(dmax, o.t0 - gs.t1);
                }
            }
            g_drag_dt = xt(in.x) - g_drag_x0;
            g_drag_dt += snap_by({sel->t0 + g_drag_dt, sel->t1 + g_drag_dt, gmin + g_drag_dt, gmax + g_drag_dt}, true);
            g_drag_dt = std::clamp(g_drag_dt, std::min(dmin, 0.0f), std::max(dmax, 0.0f));
            if (!lheld) {
                for (int i = 0; i < ns; i++) {
                    if (!in_group(i))
                        continue;
                    auto &gs = ed.shots[size_t(i)];
                    gs.t0 += g_drag_dt;
                    gs.t1 += g_drag_dt;
                    for (auto &k : gs.keys)
                        k.t += g_drag_dt;
                    for (auto &tr : gs.tracks)
                        for (auto &pk : tr.keys)
                            pk.t += g_drag_dt;
                }
                if (std::fabs(g_drag_dt) > 1e-4f) {
                    mark_dirty();
                } else {  // a click without a drag: just this shot
                    for (int i = 0; i < ns; i++)
                        ed.shots[size_t(i)].selected = i == g_shot;
                }
                g_drag = Drag::none;
                g_drag_dt = 0;
            }
        }
        break;
    case Drag::shot_left:
        if (sel) {
            g_drag_a = xt(in.x);
            g_drag_a = std::clamp(g_drag_a + snap_by({g_drag_a}, true), lo, sel->t1 - 0.25f);
            if (!lheld) {
                sel->t0 = g_drag_a;
                mark_dirty();
                g_drag = Drag::none;
            }
        }
        break;
    case Drag::shot_right:
        if (sel) {
            g_drag_b = xt(in.x);
            g_drag_b = std::clamp(g_drag_b + snap_by({g_drag_b}, true), sel->t0 + 0.25f, hi);
            if (!lheld) {
                sel->t1 = g_drag_b;
                mark_dirty();
                g_drag = Drag::none;
            }
        }
        break;
    case Drag::key_move:
        if (sel) {
            float mn = 1e9f, mx = -1e9f;
            for (auto &k : sel->keys)
                if (k.selected)
                    mn = std::min(mn, k.t), mx = std::max(mx, k.t);
            g_drag_dt = xt(in.x) - g_drag_x0;
            g_drag_dt += snap_by({g_drag_keyt + g_drag_dt}, false);
            g_drag_dt = std::clamp(g_drag_dt, sel->t0 - mn, sel->t1 - mx);
            if (!lheld) {
                for (auto &k : sel->keys)
                    if (k.selected)
                        k.t += g_drag_dt;
                sel->sort_keys();
                if (std::fabs(g_drag_dt) > 1e-4f)
                    mark_dirty();
                g_drag = Drag::none;
                g_drag_dt = 0;
            }
        }
        break;
    case Drag::marquee:
        if (!lheld) {
            float a = std::min(g_marq_x, in.x), b = std::max(g_marq_x, in.x);
            if (sel) {
                if (!shift)
                    for (auto &k : sel->keys)
                        k.selected = false;
                if (b - a >= u.u(4))
                    for (auto &k : sel->keys)
                        if (k.t >= xt(a) && k.t <= xt(b))
                            k.selected = true;
            }
            g_drag = Drag::none;
        }
        break;
    case Drag::shot_marquee:
        if (!lheld) {
            float a = std::min(g_marq_x, in.x), b = std::max(g_marq_x, in.x);
            if (!shift)
                for (auto &o : ed.shots)
                    o.selected = false;
            if (b - a >= u.u(4)) {
                int first = -1;
                for (int i = 0; i < ns; i++) {
                    auto &o = ed.shots[size_t(i)];
                    if (o.t1 > xt(a) && o.t0 < xt(b)) {
                        o.selected = true;
                        if (first < 0)
                            first = i;
                    }
                }
                if (first >= 0 && (g_shot < 0 || !ed.shots[size_t(g_shot)].selected))
                    g_shot = first;
            } else if (!shift) {
                g_shot = -1;
            }
            g_drag = Drag::none;
        }
        break;
    case Drag::sbar:
        if (lheld)
            set_view((in.x - g_sbar_grab - L) / (R - L) * full, span);
        else
            g_drag = Drag::none;
        break;
    case Drag::pan:
        if (rheld) {
            set_view(g_v0 - (in.x - g_pan_x) / (R - L) * span, span);
            g_pan_x = in.x;
        } else {
            g_drag = Drag::none;
        }
        break;
    case Drag::none:
        break;
    }
    if (rpress && over_lanes && g_drag == Drag::none) {
        g_drag = Drag::pan;
        g_pan_x = in.x;
    }
}

float g_prev_mx;   // the mouse x on the last paint (fine slider drags)
float g_insp_scroll;  // the shot inspector's scroll (pixels)
uint32_t g_fine_id;  // the slider in a fine (Ctrl) drag

void inspector(oui::Ui &u, SwoftyPaint *p, const Status &st, cine::Edit &ed)
{
    ov::Rect safe = ov::safe_area(p);
    float w = u.u(500);
    float top = safe.y + u.u(84), bottom = safe.y + safe.h - u.u(kTimelineH) - u.u(10);
    int ns = int(ed.shots.size());
    if (g_shot >= ns)
        g_shot = -1;
    float row = u.u(30), gap = u.u(5), sl = u.u(40);
    char b[200];

    if (g_shot < 0) {  // nothing selected: add a shot
        ov::Rect c = oui::panel(u, {safe.x + safe.w - w, top, w, std::min(bottom - top, u.u(470))}, "ADD A SHOT AT THE PLAYHEAD");
        float y = c.y;
        struct Item {
            cine::ShotType t;
            const char *what;
        };
        const Item items[] = {
            {cine::ShotType::onboard, "ride a car: cockpit, T-cam, wings, wheels"},
            {cine::ShotType::chase, "follow behind a driver, level horizon"},
            {cine::ShotType::track, "a fixed camera that turns to follow a driver"},
            {cine::ShotType::orbit, "circle around a driver"},
            {cine::ShotType::free_path, "fly and drop keys (F, then K)"},
            {cine::ShotType::transition, "glide between two shots (in a gap)"},
        };
        for (auto &it : items) {
            if (oui::button(u, {c.x, y, u.u(140), row}, cine::shot_type_name(it.t)))
                post({CmdKind::AddShot, 0, -1, -1, int(it.t)});
            oui::label(u, {c.x + u.u(150), y, c.w - u.u(150), row}, it.what, kDim);
            y += row + gap;
        }
        y += gap;
        snprintf(b, sizeof b, "Driver for new shots: %s", car_label(st.followed).c_str());
        oui::label(u, {c.x, y, c.w, row}, b);
        y += row;
        oui::label(u, {c.x, y, c.w, row}, "(the car the replay follows; change it per shot)", kDim);
        y += row + gap;
        oui::label(u, {c.x, y, c.w, row}, "Click a shot on the timeline to edit it.", kDim);
        y += row;
        oui::label(u, {c.x, y, c.w, row}, "F fly   K add key   H all controls   F9 close", kDim);
        return;
    }

    cine::Shot &s = ed.shots[size_t(g_shot)];
    snprintf(b, sizeof b, "SHOT %d / %d   %s", g_shot + 1, ns, cine::shot_type_name(s.type));
    static int scrolled_shot = -1;  // a newly selected shot starts at the top
    if (scrolled_shot != g_shot)
        g_insp_scroll = 0, scrolled_shot = g_shot;
    ov::Rect c = oui::panel(u, {safe.x + safe.w - w, top, w, bottom - top}, b);
    // The contents scroll inside the panel (wheel over anything but a slider) and are clipped to it. Widgets
    // scrolled out of view must not take clicks, so the mouse is hidden from them unless it is inside the view.
    bool wheel_used = false;
    float mouse_x = u.in.x, mouse_y = u.in.y, mouse_wheel = u.in.wheel;
    bool mouse_in = u.in.inside(c.x - u.u(8), c.y, c.w + u.u(16), c.h);
    if (!mouse_in && !g_ui.active)
        u.in.x = u.in.y = -1e6f;
    ov::push_clip(p, c.x - u.u(8), c.y, c.w + u.u(16), c.h);
    float y = c.y - g_insp_scroll;
    float third = (c.w - 2 * gap) / 3;
    auto changed = [&] { mark_dirty(); };

    // type (a transition stays one: it only makes sense in its gap)
    if (s.type != cine::ShotType::transition) {
        float q = (c.w - 4 * gap) / 5;
        const cine::ShotType types[] = {cine::ShotType::onboard, cine::ShotType::chase, cine::ShotType::track,
                                        cine::ShotType::orbit, cine::ShotType::free_path};
        const char *short_names[] = {"Onboard", "Chase", "Track", "Orbit", "Free"};
        for (int i = 0; i < 5; i++) {
            float x = c.x + i * (q + gap);
            if (s.type == types[i])
                ov::rect_rounded(p, x - u.u(2), y - u.u(2), q + u.u(4), row + u.u(4), u.u(6), kAccent);
            if (oui::button(u, {x, y, q, row}, short_names[i]) && s.type != types[i]) {
                s.type = types[i];
                if (s.type == cine::ShotType::track)
                    post({CmdKind::PlaceTrack, 0, g_shot});  // stand where the camera is now
                changed();
            }
        }
        y += row + gap;
    }
    // driver
    if (s.uses_car()) {
        if (oui::button(u, {c.x, y, u.u(44), row}, "<")) {
            s.car = s.car <= 0 ? st.slots - 1 : s.car - 1;
            s.driver = car_name(s.car);
            changed();
        }
        snprintf(b, sizeof b, "Driver: %s", car_label(s.car).c_str());
        if (oui::button(u, {c.x + u.u(50), y, c.w - u.u(210), row}, b))  // the list of drivers
            g_pick_driver = !g_pick_driver, g_edits_menu = false;
        if (oui::button(u, {c.x + c.w - u.u(154), y, u.u(44), row}, ">")) {
            s.car = s.car + 1 >= st.slots ? 0 : s.car + 1;
            s.driver = car_name(s.car);
            changed();
        }
        if (oui::button(u, {c.x + c.w - u.u(104), y, u.u(104), row}, "Followed")) {  // the car the replay follows
            s.car = st.followed;
            s.driver = car_name(s.car);
            changed();
        }
        y += row + gap;
    }
    // time
    auto time_row = [&](const char *name, float &v, bool is_start) {
        snprintf(b, sizeof b, "%s %s", name, mmss(v).c_str());
        oui::label(u, {c.x, y, u.u(130), row}, b);
        float q = (c.w - u.u(136) - 3 * gap) / 4;
        const float d[3] = {-0.1f, 0.1f, 0};
        const char *names[3] = {"-0.1", "+0.1", "Here"};
        for (int i = 0; i < 3; i++)
            if (oui::button(u, {c.x + u.u(136) + i * (q + gap), y, q, row}, names[i])) {
                float nv = i < 2 ? v + d[i] : st.t;
                if (is_start)
                    s.t0 = nv;
                else
                    s.t1 = nv;
                fit_shot(ed, g_shot);
                changed();
            }
        if (oui::button(u, {c.x + u.u(136) + 3 * (q + gap), y, q, row}, "Go"))
            post({CmdKind::Seek, is_start ? s.t0 : std::max(s.t0, s.t1 - 0.5f)});
        y += row + gap;
    };
    time_row("Start", s.t0, true);
    time_row("End", s.t1, false);

    // oui::slider, plus fine control: Ctrl + drag moves 20x slower (relative to where the knob is), the wheel
    // over the slider steps by `step` (Shift: 10 steps) instead of 5% of the range.
    // With a param, a key button sits left of the slider: click keys the value at the playhead (or removes the
    // key there), Ctrl+click clears the value's keys. While a value has keys the slider shows it at the playhead
    // and moving it keys the new value at the playhead.
    auto slider = [&](const char *name, float &v, float lo, float hi, const char *fmt, float step,
                      const char *param = nullptr) {
        uint32_t id = oui::id_of(name) ^ 0x2545f491u;  // oui::slider's widget id
        cine::Track *tr = param ? cine::find_track(s, param) : nullptr;
        bool animated = tr && !tr->keys.empty();
        float x = animated ? cine::track_at(*tr, st.t) : v;
        float before = x;
        int here = -1;
        if (animated)
            for (int i = 0; i < int(tr->keys.size()); i++)
                if (std::fabs(tr->keys[size_t(i)].t - st.t) < kKeyNear)
                    here = i;
        float kb_w = param ? u.u(30) : 0;
        ov::Rect rr{c.x + kb_w, y, c.w - kb_w, sl};
        if (param) {
            float kx = c.x + u.u(11), ky = y + sl * 0.68f, kr = u.u(here >= 0 ? 8 : 7);
            ov::Rect hit{c.x, y + sl * 0.3f, u.u(26), sl * 0.7f};
            bool over = u.over(hit);
            uint32_t col = here >= 0 ? kAccent : animated ? 0x4fc3f7ff : over ? 0xffffffc0 : 0xffffff50;
            draw_diamond(p, kx, ky, kr, col);
            if (!animated && !over)
                draw_diamond(p, kx, ky, kr - u.u(2.5f), 0x16161eff);  // hollow: not keyed
            if (over && (u.in.pressed & ov::mouse::left)) {
                bool inside_shot = st.t >= s.t0 - 0.02f && st.t <= s.t1 + 0.02f;
                if (u.in.key_down(VK_CONTROL)) {
                    std::erase_if(s.tracks, [&](const cine::Track &t2) { return t2.param == param; });
                    toast(std::string(name) + ": keys cleared");
                } else if (here >= 0) {
                    tr->keys.erase(tr->keys.begin() + here);
                    if (tr->keys.empty())
                        std::erase_if(s.tracks, [&](const cine::Track &t2) { return t2.param == param; });
                } else if (!inside_shot) {
                    toast("Move the playhead into the shot to key a value");
                } else {
                    if (!tr) {
                        s.tracks.push_back({param, {}});
                        tr = &s.tracks.back();
                    }
                    tr->keys.push_back({std::clamp(st.t, s.t0, s.t1), x, true, new_key_curve()});
                    cine::sort_track(*tr);
                    if (tr->keys.size() == 1)
                        toast(std::string(name) + " is keyed: moving it now keys the value at the playhead");
                }
                changed();
                tr = cine::find_track(s, param);  // the vector may have moved
                animated = tr && !tr->keys.empty();
            }
        }
        if (g_ui.active == id && u.in.held() && u.in.key_down(VK_CONTROL))
            g_fine_id = id;
        if (g_fine_id == id) {
            if (!u.in.held()) {
                g_fine_id = 0;
            } else {
                x = std::clamp(x + (u.in.x - g_prev_mx) * (hi - lo) / std::max(1.0f, c.w) * 0.05f, lo, hi);
                g_ui.active = 0;  // oui would put the knob under the mouse
            }
        }
        float wheel = u.in.wheel;
        if (wheel != 0 && u.over(rr)) {
            x = std::clamp(x + wheel * step * (u.in.key_down(VK_SHIFT) ? 10.0f : 1.0f), lo, hi);
            u.in.wheel = 0;
            wheel_used = true;
        }
        oui::slider(u, rr, name, x, lo, hi, fmt);
        u.in.wheel = wheel;
        if (animated && x != before) {  // key the new value at the playhead
            bool inside_shot = st.t >= s.t0 - 0.02f && st.t <= s.t1 + 0.02f;
            if (!inside_shot) {
                if (!g_ui.active)
                    toast("Move the playhead into the shot to key a value");
            } else {
                float kt = std::clamp(st.t, s.t0, s.t1);
                auto it = std::find_if(tr->keys.begin(), tr->keys.end(),
                                       [&](const cine::PKey &k) { return std::fabs(k.t - kt) < kKeyNear; });
                if (it != tr->keys.end())
                    it->v = x;
                else
                    tr->keys.push_back({kt, x, true, new_key_curve()});
                cine::sort_track(*tr);
                changed();
            }
        } else if (!animated && x != v) {
            v = x;
            changed();
        }
        y += sl + gap;
    };

    // The panel's end, for every shot: Deselect / Delete, then the scrolling and its bar.
    auto finish = [&] {
        if (oui::button(u, {c.x, y, (c.w - gap) / 2, row}, "Deselect"))
            g_shot = -1;
        if (oui::button(u, {c.x + (c.w + gap) / 2, y, (c.w - gap) / 2, row}, "Delete shot")) {
            ed.shots.erase(ed.shots.begin() + g_shot);
            g_shot = -1;
            changed();
        }
        y += row + gap;
        ov::pop_clip(p);
        u.in.x = mouse_x;
        u.in.y = mouse_y;
        u.in.wheel = mouse_wheel;
        float content = y + g_insp_scroll - c.y, max_scroll = std::max(0.0f, content - c.h);
        if (mouse_in && mouse_wheel != 0 && !wheel_used)
            g_insp_scroll -= mouse_wheel * u.u(60);
        g_insp_scroll = std::clamp(g_insp_scroll, 0.0f, max_scroll);
        if (max_scroll > 0) {  // a thin scrollbar on the panel's right edge
            float bx = c.x + c.w + u.u(10), th = c.h * c.h / content,
                  ty = c.y + (c.h - th) * g_insp_scroll / max_scroll;
            ov::rect_rounded(p, bx, c.y, u.u(4), c.h, u.u(2), 0xffffff18);
            ov::rect_rounded(p, bx, ty, u.u(4), th, u.u(2), 0xffffff80);
        }
    };

    switch (s.type) {
    case cine::ShotType::onboard: {
        int cols = 4;
        float q = (c.w - (cols - 1) * gap) / cols;
        int i = 0;
        for (auto &m : cine::kMounts) {
            float x = c.x + (i % cols) * (q + gap), yy = y + (i / cols) * (row + gap);
            if (s.mount == m.id)
                ov::rect_rounded(p, x - u.u(2), yy - u.u(2), q + u.u(4), row + u.u(4), u.u(6), kAccent);
            if (oui::button(u, {x, yy, q, row}, m.name)) {
                s.mount = m.id;
                s.nudge = {};
                s.yaw = s.pitch = s.roll = 0;
                s.fov = m.fov;
                changed();
            }
            i++;
        }
        y += ((i + cols - 1) / cols) * (row + gap);
        slider("Nudge right", s.nudge.x, -3, 3, "%.2f m", 0.01f, "nudge_x");
        slider("Nudge up", s.nudge.y, -2, 3, "%.2f m", 0.01f, "nudge_y");
        slider("Nudge forward", s.nudge.z, -8, 8, "%.2f m", 0.01f, "nudge_z");
        slider("Look yaw", s.yaw, -180, 180, "%.1f", 0.5f, "yaw");
        slider("Look pitch", s.pitch, -60, 60, "%.1f", 0.5f, "pitch");
        break;
    }
    case cine::ShotType::chase: {
        float q = (c.w - 3 * gap) / 4;
        int i = 0;
        for (auto &cp : cine::kChases) {
            if (oui::button(u, {c.x + i * (q + gap), y, q, row}, cp.name)) {
                s.distance = cp.distance;
                s.height = cp.height;
                s.fov = cp.fov;
                changed();
            }
            i++;
        }
        y += row + gap;
        if (s.keys.empty()) {
            slider("Distance", s.distance, 1, 80, "%.1f m", 0.1f, "distance");
            slider("Height", s.height, -1, 60, "%.1f m", 0.05f, "height");
        } else {  // keys place the camera; these move the whole key path
            oui::label(u, {c.x, y, c.w, row}, "Keys place the camera; these shift the whole path:", kDim);
            y += row;
            slider("Nudge right", s.nudge.x, -10, 10, "%.2f m", 0.01f, "nudge_x");
            slider("Nudge up", s.nudge.y, -5, 20, "%.2f m", 0.01f, "nudge_y");
            slider("Nudge forward", s.nudge.z, -20, 20, "%.2f m", 0.01f, "nudge_z");
        }
        slider("Around the car", s.around, -180, 180, "%.1f", 0.5f, "around");
        slider("Look yaw", s.yaw, -60, 60, "%.1f", 0.5f, "yaw");
        slider("Look pitch", s.pitch, -45, 45, "%.1f", 0.5f, "pitch");
        break;
    }
    case cine::ShotType::track:
        if (oui::button(u, {c.x, y, c.w, row}, "Place it where the camera is now"))
            post({CmdKind::PlaceTrack, 0, g_shot});
        y += row + gap;
        snprintf(b, sizeof b, "Stands at %.1f  %.1f  %.1f", s.point.x, s.point.y, s.point.z);
        oui::label(u, {c.x, y, c.w, row}, b, kDim);
        y += row + gap;
        break;
    case cine::ShotType::orbit:
        slider("Radius", s.radius, 2, 80, "%.1f m", 0.1f, "radius");
        slider("Height", s.height, -1, 50, "%.1f m", 0.05f, "height");
        slider("Seconds per turn", s.period, 2, 60, "%.1f s", 0.1f);
        slider("Start angle", s.start_angle, -180, 180, "%.0f", 1);
        {
            bool rv = s.reverse;
            if (oui::toggle(u, {c.x, y, c.w / 2, row}, "Turn the other way", rv)) {
                s.reverse = rv;
                changed();
            }
            y += row + gap;
        }
        break;
    case cine::ShotType::free_path:
        oui::label(u, {c.x, y, c.w, row}, "Fly (F) to a spot, press K: a key at the playhead.", kDim);
        y += row + gap;
        break;
    case cine::ShotType::transition: {
        // the shots it glides between (the nearest non-transition shots either side)
        int a = g_shot - 1, bb = g_shot + 1;
        while (a >= 0 && ed.shots[size_t(a)].type == cine::ShotType::transition)
            a--;
        while (bb < ns && ed.shots[size_t(bb)].type == cine::ShotType::transition)
            bb++;
        snprintf(b, sizeof b, "From: %s", a >= 0 ? shot_label(ed.shots[size_t(a)]).c_str() : "(no shot before)");
        oui::label(u, {c.x, y, c.w, row}, b, a >= 0 ? kText : kAccent);
        y += row;
        snprintf(b, sizeof b, "To: %s", bb < ns ? shot_label(ed.shots[size_t(bb)]).c_str() : "(no shot after)");
        oui::label(u, {c.x, y, c.w, row}, b, bb < ns ? kText : kAccent);
        y += row + gap;
        // The speed curves: one per axis of the move (in the destination car's frame) and one for the look. The
        // graph shows time across and progress up (0 = the shot before, 1 = the shot after); drag the two handles.
        static int axis = cine::kAxForward;
        const char *an[cine::kAxCount] = {"Forward", "Side", "Height", "Look"};
        const uint32_t acol[cine::kAxCount] = {0x4fc3f7ff, 0x81c784ff, 0xffb74dff, 0xe0e0e0ff};
        float q = (c.w - 3 * gap) / 4;
        for (int i = 0; i < cine::kAxCount; i++) {
            float x = c.x + i * (q + gap);
            if (axis == i)
                ov::rect_rounded(p, x - u.u(2), y - u.u(2), q + u.u(4), row + u.u(4), u.u(6), acol[i]);
            if (oui::button(u, {x, y, q, row}, an[i]))
                axis = i;
        }
        y += row + gap;
        cine::Curve &cv = s.curves[axis];
        const cine::Curve *others[cine::kAxCount];
        for (int i = 0; i < cine::kAxCount; i++)
            others[i] = &s.curves[i];
        float ph = st.t > s.t0 && st.t < s.t1 ? (st.t - s.t0) / std::max(0.05f, s.t1 - s.t0) : -1;
        float gh = u.u(220);
        if (curve_editor(u, p, {c.x + u.u(8), y, c.w - u.u(16), gh}, cv, acol[axis], ph, others, acol,
                         cine::kAxCount))
            changed();
        y += gh + gap;
        oui::label(u, {c.x, y, c.w, row}, "Double-click the curve: add an anchor (again: remove)", kDim);
        y += row;
        // presets for the axis being edited
        const char *pn[4] = {"Smooth", "Linear", "Ease in", "Ease out"};
        for (int i = 0; i < 4; i++)
            if (oui::button(u, {c.x + i * (q + gap), y, q, row}, pn[i])) {
                cv = cine::curve_preset(i);
                changed();
            }
        y += row + gap;
        if (oui::button(u, {c.x, y, c.w, row}, "Same curve on every axis")) {
            for (auto &o : s.curves)
                o = cv;
            changed();
        }
        y += row + gap;
        oui::label(u, {c.x, y, c.w, row}, "Forward early, Height late: it comes in level, then drops.", kDim);
        y += row + gap;
        slider("Replay speed", s.speed, 0.05f, 2, "%.2fx", 0.01f);
        break;
    }
    }
    if (s.type == cine::ShotType::transition) {  // the rest (lens, roll, ease, keys) comes from its two shots
        finish();
        return;
    }
    if (!(s.keyed() && !s.keys.empty()))  // keyed shots take the FOV from their keys
        slider("FOV", s.fov, 5, 140, "%.1f", 0.5f, "fov");
    if (s.type != cine::ShotType::free_path)  // a free path's roll is in its keys
        slider("Roll", s.roll, -180, 180, "%.1f", 0.5f, "roll");
    slider("Replay speed", s.speed, 0.05f, 2, "%.2fx", 0.01f, "speed");
    {
        bool cs = s.constant_speed, ei = s.ease_in, eo = s.ease_out;
        if (oui::toggle(u, {c.x, y, third, row}, "Constant", cs))
            s.constant_speed = cs, changed();
        if (oui::toggle(u, {c.x + third + gap, y, third, row}, "Ease in", ei))
            s.ease_in = ei, changed();
        if (oui::toggle(u, {c.x + 2 * (third + gap), y, third, row}, "Ease out", eo))
            s.ease_out = eo, changed();
        y += row + gap;
    }
    if (s.keyed()) {
        int nk = int(s.keys.size()), nsel = 0, first_sel = -1;
        for (int i = 0; i < nk; i++)
            if (s.keys[size_t(i)].selected) {
                nsel++;
                if (first_sel < 0)
                    first_sel = i;
            }
        snprintf(b, sizeof b, "Keys: %d%s", nk, nsel ? ("   selected " + std::to_string(nsel)).c_str() : "");
        oui::label(u, {c.x, y, c.w, row}, b);
        y += row;
        // the selected keys' FOV (several: each moves by the same amount, so their differences stay)
        if (nsel) {
            float f = s.keys[size_t(first_sel)].fov, f0 = f;
            slider(nsel > 1 ? "Key FOV (first selected)" : "Key FOV", f, 5, 140, "%.1f", 0.5f);
            if (f != f0)
                for (auto &k : s.keys)
                    if (k.selected)
                        k.fov = std::clamp(k.fov + (f - f0), 5.0f, 140.0f);
        } else if (nk) {
            oui::label(u, {c.x, y, c.w, row}, "Select a key to change its FOV", kDim);
            y += row;
        }
        if (oui::button(u, {c.x, y, third, row}, "+ Key (K)"))
            post({CmdKind::AddKey});
        if (oui::button(u, {c.x + third + gap, y, third, row}, "Set from view") && first_sel >= 0)
            post({CmdKind::SetKeyFromView, 0, g_shot, first_sel});
        if (oui::button(u, {c.x + 2 * (third + gap), y, third, row}, "Delete keys") && nsel) {
            std::erase_if(s.keys, [](const cine::ShotKey &k) { return k.selected; });
            changed();
        }
        y += row + gap;
        if (oui::button(u, {c.x, y, third, row}, "< Key (,)"))
            if (auto kt = jump_key(ed, st.t, -1))
                post({CmdKind::Seek, *kt});
        if (oui::button(u, {c.x + third + gap, y, third, row}, "Go to key") && first_sel >= 0)
            post({CmdKind::Seek, s.keys[size_t(first_sel)].t});
        if (oui::button(u, {c.x + 2 * (third + gap), y, third, row}, "Key (.) >"))
            if (auto kt = jump_key(ed, st.t, 1))
                post({CmdKind::Seek, *kt});
        y += row + gap;
        // back to where they were before they were dragged in 3D; the picked bend handle: straighten its segment
        {
            int moved = 0;
            for (const auto &k : s.keys)
                moved += k.selected && k.has_orig && cine::len(k.pos - k.orig) > 1e-4f;
            int hk = g_sel_handle_shot == g_shot ? key_by_time(s, g_sel_handle_t) : -1;
            bool bent = hk >= 0 && cine::len(s.keys[size_t(hk)].bend) > 1e-4f;
            float half = (c.w - gap) / 2;
            if (oui::button(u, {c.x, y, half, row}, moved ? "Reset position" : "Reset position (not moved)") && moved) {
                for (auto &k : s.keys)
                    if (k.selected && k.has_orig)
                        k.pos = k.orig;
                changed();
                toast(std::to_string(moved) + " key(s) back where they were");
            }
            if (oui::button(u, {c.x + half + gap, y, half, row}, "Straighten bend") && bent) {
                s.keys[size_t(hk)].bend = {};
                changed();
                toast("Segment straightened");
            }
            y += row + gap;
        }
        // the segment after the selected key(s): timing and corkscrew (every selected key that has a next key)
        if (nsel > 0 && first_sel + 1 < nk) {
            cine::ShotKey &k = s.keys[size_t(first_sel)];
            oui::label(u, {c.x, y, c.w, row}, nsel == 1 ? "To the next key:" : "To the next key (all selected):",
                       kDim);
            y += row;
            float q5 = (c.w - 4 * gap) / 5;
            const char *names[] = {"Smooth", "Hold", "Ease in", "Ease out", "In-out"};
            for (int m = 0; m < 5; m++) {
                float x = c.x + m * (q5 + gap);
                if (k.seg == m)
                    ov::rect_rounded(p, x - u.u(2), y - u.u(2), q5 + u.u(4), row + u.u(4), u.u(6), kAccent);
                if (oui::button(u, {x, y, q5, row}, names[m])) {
                    for (int i = 0; i + 1 < nk; i++)
                        if (s.keys[size_t(i)].selected)
                            s.keys[size_t(i)].seg = m;
                    changed();
                }
            }
            y += row + gap;
            float turns = k.twist_turns, radius = k.twist_radius;
            bool ch = oui::slider(u, {c.x, y, c.w, sl}, "Corkscrew turns (- = the other way)", turns, -6, 6, "%.2f");
            y += sl + gap;
            ch |= oui::slider(u, {c.x, y, c.w, sl}, "Corkscrew radius", radius, 0, 15, "%.2f m");
            y += sl + gap;
            if (ch) {
                for (int i = 0; i + 1 < nk; i++)
                    if (s.keys[size_t(i)].selected) {
                        s.keys[size_t(i)].twist_turns = std::round(turns * 4) / 4;  // quarter turns
                        s.keys[size_t(i)].twist_radius = radius;
                    }
                changed();
            }
        }
    }
    if (oui::button(u, {c.x, y, c.w, row}, "Split at the playhead (B)"))
        g_want_split = true;
    y += row + gap;
    if (oui::button(u, {c.x, y, (c.w - gap) / 2, row}, "Copy look")) {  // settings and value keys
        g_look_clip = s;
        toast("Copied the look of shot " + std::to_string(g_shot + 1));
    }
    if (oui::button(u, {c.x + (c.w + gap) / 2, y, (c.w - gap) / 2, row}, "Paste look") && g_look_clip) {
        const cine::Shot &f = *g_look_clip;
        float d = s.t0 - f.t0;
        s.mount = f.mount;
        s.nudge = f.nudge;
        s.distance = f.distance;
        s.height = f.height;
        s.yaw = f.yaw;
        s.pitch = f.pitch;
        s.roll = f.roll;
        s.fov = f.fov;
        s.around = f.around;
        s.radius = f.radius;
        s.period = f.period;
        s.start_angle = f.start_angle;
        s.reverse = f.reverse;
        s.speed = f.speed;
        s.constant_speed = f.constant_speed;
        s.ease_in = f.ease_in;
        s.ease_out = f.ease_out;
        s.tracks = f.tracks;
        for (auto &tr : s.tracks)
            for (auto &pk : tr.keys)
                pk.t += d;
        changed();
        toast("Pasted the look");
    }
    y += row + gap;
    finish();
}

// ----- the properties panel (left): the selected shot's keyable values, their keys over the shot, and the
// speed curve of the segment picked between two keys. The inspector (right) still sets the values themselves.
struct PropInfo {
    const char *param, *name, *fmt;
};
// The values a shot of this type can key, in the inspector's order.
std::vector<PropInfo> shot_props(const cine::Shot &s)
{
    static const PropInfo nx{"nudge_x", "Nudge right", "%.2f m"}, ny{"nudge_y", "Nudge up", "%.2f m"},
        nz{"nudge_z", "Nudge forward", "%.2f m"}, yaw{"yaw", "Look yaw", "%.1f"}, pitch{"pitch", "Look pitch", "%.1f"},
        roll{"roll", "Roll", "%.1f"}, fov{"fov", "FOV", "%.1f"}, dist{"distance", "Distance", "%.1f m"},
        height{"height", "Height", "%.1f m"}, around{"around", "Around the car", "%.1f"},
        radius{"radius", "Radius", "%.1f m"}, speed{"speed", "Replay speed", "%.2fx"};
    switch (s.type) {
    case cine::ShotType::onboard: return {nx, ny, nz, yaw, pitch, roll, fov, speed};
    case cine::ShotType::chase:
        if (!s.keys.empty())
            return {nx, ny, nz, around, yaw, pitch, roll, speed};
        return {dist, height, around, yaw, pitch, roll, fov, speed};
    case cine::ShotType::track: return {roll, fov, speed};
    case cine::ShotType::orbit: return {radius, height, roll, fov, speed};
    case cine::ShotType::free_path: return {speed};
    case cine::ShotType::transition: return {};
    }
    return {};
}

void props_panel(oui::Ui &u, SwoftyPaint *p, const Status &st, cine::Edit &ed)
{
    g_props_w = 0;
    int ns = int(ed.shots.size());
    if (g_shot < 0 || g_shot >= ns)
        return;
    cine::Shot &s = ed.shots[size_t(g_shot)];
    auto props = shot_props(s);
    if (props.empty())
        return;
    static int for_shot = -1, sel_key = -1;
    static std::string sel_param;
    if (for_shot != g_shot)
        for_shot = g_shot, sel_param.clear(), sel_key = -1;
    ov::Rect safe = ov::safe_area(p);
    float w = u.u(480), top = safe.y + u.u(84), bottom = safe.y + safe.h - u.u(kTimelineH) - u.u(10);
    float row = u.u(30), gap = u.u(5);
    g_props_w = w + u.u(20);
    ov::Rect c = oui::panel(u, {safe.x + u.u(10), top, w, bottom - top}, "PROPERTIES");
    float y = c.y;
    char b[160];
    float name_w = u.u(140), val_w = u.u(80), dia_w = u.u(28);
    // the lane: the shot's time across. Inset by a key's half width at both ends, so a key at the shot's start
    // doesn't sit on the row's key button (a click there hit both)
    float pad = u.u(10);
    float lx = c.x + name_w + val_w + dia_w + pad, lw = c.x + c.w - pad - lx;
    auto lane_x = [&](float t) { return lx + (t - s.t0) / std::max(0.05f, s.t1 - s.t0) * lw; };
    bool inside_shot = st.t >= s.t0 - 0.02f && st.t <= s.t1 + 0.02f;
    for (const auto &pi : props) {
        float *pv = cine::param_ptr(s, pi.param);
        if (!pv)
            continue;
        cine::Track *tr = cine::find_track(s, pi.param);
        bool animated = tr && !tr->keys.empty();
        float val = animated ? cine::track_at(*tr, st.t) : *pv;
        bool row_sel = sel_param == pi.param;
        if (row_sel)
            ov::rect_rounded(p, c.x - u.u(4), y - u.u(2), c.w + u.u(8), row + u.u(4), u.u(5), 0xffffff12);
        oui::label(u, {c.x, y, name_w, row}, pi.name, animated ? kText : kDim);
        snprintf(b, sizeof b, pi.fmt, val);
        oui::label(u, {c.x + name_w, y, val_w, row}, b, kDim);
        // the key button: key the value at the playhead, or remove the key there
        int here = -1;
        if (animated)
            for (int i = 0; i < int(tr->keys.size()); i++)
                if (std::fabs(tr->keys[size_t(i)].t - st.t) < kKeyNear)
                    here = i;
        float kx = c.x + name_w + val_w + dia_w / 2, ky = y + row / 2;
        ov::Rect hit{kx - dia_w / 2, y, dia_w, row};
        bool over = u.over(hit);
        draw_diamond(p, kx, ky, u.u(here >= 0 ? 8 : 7), here >= 0 ? kAccent : over ? 0xffffffc0 : 0xffffff60);
        if (here < 0)
            draw_diamond(p, kx, ky, u.u(4.5f), 0x16161eff);
        if (over && (u.in.pressed & ov::mouse::left)) {
            if (here >= 0) {
                tr->keys.erase(tr->keys.begin() + here);
                if (tr->keys.empty())
                    std::erase_if(s.tracks, [&](const cine::Track &t2) { return t2.param == pi.param; });
            } else if (!inside_shot) {
                toast("Move the playhead into the shot to key a value");
            } else {
                if (!tr) {
                    s.tracks.push_back({pi.param, {}});
                    tr = &s.tracks.back();
                }
                tr->keys.push_back({std::clamp(st.t, s.t0, s.t1), val, true, new_key_curve()});
                cine::sort_track(*tr);
            }
            mark_dirty();
            tr = cine::find_track(s, pi.param);
            animated = tr && !tr->keys.empty();
        }
        // the lane: the shot's span, its keys, the playhead; click between two keys: that segment's curve
        ov::rect_rounded(p, lx, y + row * 0.35f, lw, row * 0.3f, u.u(3), 0xffffff14);
        if (inside_shot)
            ov::line(p, lane_x(st.t), y, lane_x(st.t), y + row, 1, 0xe1060090);
        if (animated) {
            for (int i = 0; i + 1 < int(tr->keys.size()); i++) {
                float x0 = lane_x(tr->keys[size_t(i)].t), x1 = lane_x(tr->keys[size_t(i + 1)].t);
                bool seg_sel = row_sel && sel_key == i;
                uint32_t col = seg_sel ? kAccent : tr->keys[size_t(i)].curved ? 0x4fc3f7b0 : 0xffffff60;
                ov::line(p, x0, ky, x1, ky, u.u(seg_sel ? 4 : 2), col);
                ov::Rect seg{x0 + u.u(6), y, std::max(0.0f, x1 - x0 - u.u(12)), row};
                if (u.over(seg) && (u.in.pressed & ov::mouse::left))
                    sel_param = pi.param, sel_key = i;
            }
            for (int i = 0; i < int(tr->keys.size()); i++) {
                float x = lane_x(tr->keys[size_t(i)].t);
                ov::Rect kh{x - u.u(7), y, u.u(14), row};
                bool kover = u.over(kh);
                draw_diamond(p, x, ky, u.u(kover ? 7 : 6), i == here ? kAccent : 0xffffffe0);
                if (kover && (u.in.pressed & ov::mouse::left)) {  // a key: pick the segment after it; double: go to it
                    static ULONGLONG kclick;
                    static float kclick_t = -1;
                    float kt = tr->keys[size_t(i)].t;
                    bool dbl = GetTickCount64() - kclick < 400 && std::fabs(kclick_t - kt) < 1e-4f;
                    kclick = dbl ? 0 : GetTickCount64(), kclick_t = kt;
                    if (dbl)
                        post({CmdKind::Seek, kt});
                    sel_param = pi.param;
                    sel_key = std::min(i, int(tr->keys.size()) - 2);
                } else if (kover && (u.in.pressed & ov::mouse::right)) {  // right-click: remove it, from anywhere
                    tr->keys.erase(tr->keys.begin() + i);
                    if (tr->keys.empty())
                        std::erase_if(s.tracks, [&](const cine::Track &t2) { return t2.param == pi.param; });
                    sel_key = -1;
                    mark_dirty();
                    toast(std::string(pi.name) + ": key removed");
                    break;
                }
            }
        }
        y += row + gap;
    }
    // the picked segment's curve
    y += gap;
    ov::line(p, c.x, y, c.x + c.w, y, 1, 0xffffff20);
    y += gap * 2;
    cine::Track *tr = sel_param.empty() ? nullptr : cine::find_track(s, sel_param);
    if (!tr || sel_key < 0 || sel_key + 1 >= int(tr->keys.size())) {
        oui::label(u, {c.x, y, c.w, row}, "Key a value twice (the diamonds), then click", kDim);
        y += row;
        oui::label(u, {c.x, y, c.w, row}, "between its two keys to shape how it moves.", kDim);
        return;
    }
    const char *pname = sel_param.c_str(), *pfmt = "%.2f";
    for (const auto &pi : props)
        if (sel_param == pi.param)
            pname = pi.name, pfmt = pi.fmt;
    cine::PKey &k0 = tr->keys[size_t(sel_key)];
    const cine::PKey &k1 = tr->keys[size_t(sel_key + 1)];
    char v0[32], v1[32];
    snprintf(v0, sizeof v0, pfmt, k0.v);
    snprintf(v1, sizeof v1, pfmt, k1.v);
    snprintf(b, sizeof b, "%s   %s  ->  %s", pname, v0, v1);
    oui::label(u, {c.x, y, c.w, row}, b);
    y += row;
    snprintf(b, sizeof b, "%s - %s", mmss(k0.t).c_str(), mmss(k1.t).c_str());
    oui::label(u, {c.x, y, c.w, row}, b, kDim);
    y += row + gap;
    float q = (c.w - 3 * gap) / 4;
    float gh = std::max(u.u(120), std::min(u.u(240), c.y + c.h - y - 3 * (row + gap)));
    if (k0.curved) {
        float ph = st.t > k0.t && st.t < k1.t ? (st.t - k0.t) / std::max(1e-3f, k1.t - k0.t) : -1;
        if (curve_editor(u, p, {c.x + u.u(8), y, c.w - u.u(16), gh}, k0.curve, 0x4fc3f7ff, ph))
            mark_dirty();
    } else {
        oui::label(u, {c.x, y + gh / 2 - row, c.w, row}, "Older keys: a plain spline through them.", kDim);
        oui::label(u, {c.x, y + gh / 2, c.w, row}, "Pick a curve below to shape this segment.", kDim);
    }
    y += gh + gap;
    const char *pn[4] = {"Smooth", "Linear", "Ease in", "Ease out"};
    int cur = k0.curved ? curve_preset_of(k0.curve) : -2;  // -1: shaped by hand, -2: older spline
    for (int i = 0; i < 4; i++) {
        float x = c.x + i * (q + gap);
        if (cur == i)  // the one this segment uses
            ov::rect_rounded(p, x - u.u(2), y - u.u(2), q + u.u(4), row + u.u(4), u.u(6), kAccent);
        if (oui::button(u, {x, y, q, row}, pn[i])) {
            k0.curved = true;
            k0.curve = cine::curve_preset(i);
            mark_dirty();
        }
    }
    y += row + gap;
    oui::label(u, {c.x, y, c.w, row},
               cur == -1 ? "Custom curve (shaped by hand)" : cur == -2 ? "Older keys: spline" : "", kDim);
}

// The list of drivers for the selected shot, left of the inspector.
void driver_picker(oui::Ui &u, SwoftyPaint *p, const Status &st, cine::Edit &ed)
{
    if (g_shot < 0 || g_shot >= int(ed.shots.size()) || !ed.shots[size_t(g_shot)].uses_car()) {
        g_pick_driver = false;
        return;
    }
    cine::Shot &s = ed.shots[size_t(g_shot)];
    ov::Rect safe = ov::safe_area(p);
    int n = std::clamp(st.slots, 0, 32);
    float row = u.u(30), gap = u.u(5), w = u.u(470);
    int rows = (n + 1) / 2 + 1;
    float h = u.u(60) + rows * (row + gap);
    ov::Rect c = oui::panel(u, {safe.x + safe.w - u.u(500) - w - u.u(12), safe.y + u.u(84), w, h}, "DRIVER");
    float half = (c.w - gap) / 2;
    for (int i = 0; i < n; i++) {
        float x = c.x + (i % 2) * (half + gap), y = c.y + (i / 2) * (row + gap);
        if (s.car == i)
            ov::rect_rounded(p, x - u.u(2), y - u.u(2), half + u.u(4), row + u.u(4), u.u(6), kAccent);
        if (oui::button(u, {x, y, half, row}, car_label(i))) {
            s.car = i;
            s.driver = car_name(i);
            mark_dirty();
            g_pick_driver = false;
        }
    }
    float y = c.y + ((n + 1) / 2) * (row + gap);
    if (oui::button(u, {c.x, y, half, row}, "The followed car")) {
        s.car = st.followed;
        s.driver = car_name(s.car);
        mark_dirty();
        g_pick_driver = false;
    }
    if (oui::button(u, {c.x + half + gap, y, half, row}, "Close"))
        g_pick_driver = false;
}

// The edits in the sequences folder (not backups), sorted.
std::vector<std::string> list_edits()
{
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_dir + "\\sequences\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        std::string n = fd.cFileName;
        if (n.size() < 6 || n.find(".v1.") != std::string::npos || n.find(".before") != std::string::npos)
            continue;
        out.push_back(n.substr(0, n.size() - 5));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// The Play menu, above its button: Play all, Final Cut, Render... (which opens the Render menu).
void play_menu(oui::Ui &u, SwoftyPaint *p)
{
    ov::Rect safe = ov::safe_area(p);
    float row = u.u(34), gap = u.u(6), w = u.u(380), h = u.u(56) + 3 * (row + gap);
    ov::Rect c = oui::panel(u, {g_play_menu_x - u.u(10), safe.y + safe.h - u.u(kTimelineH) - h - u.u(10), w, h}, "PLAY");
    float y = c.y;
    auto item = [&](const char *name, const char *what) {
        bool hit = oui::button(u, {c.x, y, u.u(120), row}, name);
        oui::label(u, {c.x + u.u(128), y, c.w - u.u(128), row}, what, kDim);
        y += row + gap;
        return hit;
    };
    if (item("Play all", "the edit, editor hidden")) {
        post({CmdKind::PlayFromStart});
        g_play_menu = false;
    }
    if (item("Final Cut", "clean: every HUD hidden")) {
        post({CmdKind::FinalCut});
        g_play_menu = false;
    }
    if (item("Render...", "to a video file")) {
        g_play_menu = false;
        g_render_menu = true;
    }
}

// ffmpeg for renders: found (render::find_ffmpeg, re-checked every 2 s while the Render menu is open), or
// installed from the menu with WinGet (Gyan.FFmpeg, a portable zip WinGet unpacks for the user; no console
// window; output in ffmpeg-install.log).
std::wstring widen(const std::string &s);
enum FfInstall { kFfIdle, kFfRunning, kFfDone, kFfFailed, kFfNoWinget };
std::atomic<int> g_ff_install{kFfIdle};
std::atomic<int> g_ff_install_code{0};

bool ffmpeg_ready()
{
    static bool found = false;
    static ULONGLONG at = 0;
    ULONGLONG now = GetTickCount64();
    if (at == 0 || now - at > 2000) {  // again every 2 s while the menu is open: it can also go away
        at = now;
        found = !render::find_ffmpeg(widen(g_cfg.ffmpeg)).empty();
    }
    return found;
}

void install_ffmpeg()
{
    int idle = kFfIdle, failed = kFfFailed, none = kFfNoWinget;
    if (!g_ff_install.compare_exchange_strong(idle, kFfRunning) &&
        !g_ff_install.compare_exchange_strong(failed, kFfRunning) &&
        !g_ff_install.compare_exchange_strong(none, kFfRunning))
        return;
    std::thread([] {
        wchar_t winget[MAX_PATH];
        if (!SearchPathW(nullptr, L"winget.exe", nullptr, MAX_PATH, winget, nullptr)) {
            g_ff_install = kFfNoWinget;
            swofty::log("[cinecam] ffmpeg install: winget not found");
            return;
        }
        std::wstring log = widen(g_dir) + L"\\ffmpeg-install.log";
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE lg = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
        std::wstring cmd = L"\"" + std::wstring(winget) +
                           L"\" install --id Gyan.FFmpeg --exact --source winget "
                           L"--accept-source-agreements --accept-package-agreements --disable-interactivity";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = lg != INVALID_HANDLE_VALUE ? lg : nullptr;
        PROCESS_INFORMATION pi{};
        DWORD code = DWORD(-1);
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 15 * 60 * 1000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (lg != INVALID_HANDLE_VALUE)
            CloseHandle(lg);
        g_ff_install_code = int(code);
        bool ok = !render::find_ffmpeg(widen(g_cfg.ffmpeg)).empty();
        g_ff_install = ok ? kFfDone : kFfFailed;
        swofty::log("[cinecam] ffmpeg install: winget exit 0x%08lx, ffmpeg %s", (unsigned long)code,
                    ok ? "found" : "still not found");
        toast(ok ? "ffmpeg installed: rendering is ready" : "ffmpeg install failed (see ffmpeg-install.log)");
    }).detach();
}

// The Render menu: the whole edit, the selected shots, or an in / out range set at the playhead.
void render_menu(oui::Ui &u, SwoftyPaint *p, const Status &st, cine::Edit &ed)
{
    ov::Rect safe = ov::safe_area(p);
    bool have_ff = ffmpeg_ready();
    float row = u.u(32), gap = u.u(6), w = u.u(560), h = u.u(60) + (have_ff ? 6 : 8) * (row + gap);
    ov::Rect c = oui::panel(u, {safe.x + u.u(520), safe.y + safe.h - u.u(kTimelineH) - h - u.u(10), w, h}, "RENDER");
    float y = c.y, bw = u.u(120), lw = c.w - bw - gap;
    char b[160];
    auto go = [&](float a, float z) {
        Cmd cmd{CmdKind::Render};
        cmd.t = a;
        cmd.t2 = z;
        post(cmd);
        g_render_menu = false;
    };
    if (!have_ff) {  // ffmpeg missing: offer to install it
        int fs = g_ff_install.load();
        const char *msg = fs == kFfRunning    ? "Installing ffmpeg with WinGet... (a minute or two)"
                          : fs == kFfFailed   ? "The install failed: see ffmpeg-install.log, or try again"
                          : fs == kFfNoWinget ? "WinGet isn't on this PC: install ffmpeg from ffmpeg.org"
                          : fs == kFfDone     ? "Installed: finding it..."
                                              : "Rendering needs ffmpeg (free), not found on this PC";
        oui::label(u, {c.x, y, lw, row}, msg, fs == kFfFailed || fs == kFfNoWinget ? kAccent : kText);
        if (fs != kFfRunning && fs != kFfDone && oui::button(u, {c.x + lw + gap, y, bw, row}, "Install"))
            install_ffmpeg();
        y += row + gap;
        oui::label(u, {c.x, y, c.w, row}, "Or: winget install Gyan.FFmpeg, or set ffmpeg = <path> in cinecam.cfg",
                   kDim);
        y += row + gap * 2;
    }
    // the whole edit (label column, times column, Render)
    float tcol = c.x + u.u(150);
    oui::label(u, {c.x, y, u.u(145), row}, "Whole edit");
    snprintf(b, sizeof b, "%s - %s", mmss(st.edit_t0).c_str(), mmss(st.edit_t1).c_str());
    oui::label(u, {tcol, y, c.x + lw - tcol, row}, b, kDim);
    if (oui::button(u, {c.x + lw + gap, y, bw, row}, "Render"))
        go(0, 0);
    y += row + gap;
    // the selected shots
    float s0 = 1e9f, s1 = -1;
    int nsel = 0;
    for (int i = 0; i < int(ed.shots.size()); i++)
        if (ed.shots[size_t(i)].selected || i == g_shot) {
            s0 = std::min(s0, ed.shots[size_t(i)].t0);
            s1 = std::max(s1, ed.shots[size_t(i)].t1);
            nsel++;
        }
    oui::label(u, {c.x, y, u.u(145), row}, nsel > 1 ? "Selected shots" : "Selected shot", nsel ? kText : kDim);
    if (nsel) {
        snprintf(b, sizeof b, "%s - %s", mmss(s0).c_str(), mmss(s1).c_str());
        oui::label(u, {tcol, y, c.x + lw - tcol, row}, b, kDim);
        if (oui::button(u, {c.x + lw + gap, y, bw, row}, "Render"))
            go(s0, s1);
    } else {
        oui::label(u, {tcol, y, c.x + c.w - tcol, row}, "none (click shots on the timeline)", kDim);
    }
    y += row + gap * 2;
    // a range: In / Out at the playhead (the ruler shows them)
    float q = (c.w - 3 * gap) / 4;
    snprintf(b, sizeof b, "In  %s", g_render_in >= 0 ? mmss(g_render_in).c_str() : "-");
    oui::label(u, {c.x, y, q, row}, b);
    if (oui::button(u, {c.x + q + gap, y, q, row}, "Set In"))
        g_render_in = st.t;
    snprintf(b, sizeof b, "Out  %s", g_render_out >= 0 ? mmss(g_render_out).c_str() : "-");
    oui::label(u, {c.x + 2 * (q + gap), y, q, row}, b);
    if (oui::button(u, {c.x + 3 * (q + gap), y, q, row}, "Set Out"))
        g_render_out = st.t;
    y += row + gap;
    bool range_ok = g_render_in >= 0 && g_render_out > g_render_in + 0.05f;
    bool any_mark = g_render_in >= 0 || g_render_out >= 0;
    oui::label(u, {c.x, y, u.u(145), row}, "Range", range_ok ? kText : kDim);
    if (range_ok)
        snprintf(b, sizeof b, "%s - %s  (%.1f s)", mmss(g_render_in).c_str(), mmss(g_render_out).c_str(),
                 double(g_render_out - g_render_in));
    else
        snprintf(b, sizeof b, "%s", any_mark ? "Out must be after In" : "set In, then Out");
    float cw = u.u(84);  // Clear, left of Render, only once something is set
    oui::label(u, {tcol, y, c.x + lw - tcol - (any_mark ? cw + gap : 0), row}, b, kDim);
    if (any_mark && oui::button(u, {c.x + lw - cw, y, cw, row}, "Clear"))
        g_render_in = g_render_out = -1;
    if (range_ok && oui::button(u, {c.x + lw + gap, y, bw, row}, "Render"))
        go(g_render_in, g_render_out);
    y += row + gap * 2;
    {
        char q2[32];
        bool crf_fmt = st.o_format == 0 || st.o_format == 3;
        if (crf_fmt)
            snprintf(q2, sizeof q2, "%s q%d", st.o_format == 3 ? "H.265" : "H.264", st.o_crf);
        snprintf(b, sizeof b, "%d fps, %s, sound %s  (change in Options)", st.o_fps,
                 crf_fmt ? q2 : st.o_format == 1 ? "ProRes HQ" : "ProRes 4444", st.o_audio ? "on" : "off");
    }
    oui::label(u, {c.x, y, c.w, row}, b, kDim);
    y += row;
    oui::label(u, {c.x, y, c.w, row}, "Keep the game open, not minimized. F9 cancels.", kDim);
}

// The Options pane: the settings of cinecam.cfg, saved to it shortly after a change. Called every editor paint:
// drawn while `open`; when closed it only saves a change still pending and re-reads the file next time it opens.
void set_cfg_value(const std::string &key, float v);
std::wstring wide_of(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(std::max(1, n)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    w.resize(wcslen(w.c_str()));
    return w;
}

void options_pane(oui::Ui &u, SwoftyPaint *p, const Status &st, bool open)
{
    struct Vals {
        float fps, settle, crf, offset_ms, wait, preroll, letterbox, fade, blur, shutter;
        bool audio, listener, follow, passby;
        float format;
        bool hidehud;
        float keycurve;
        bool clip;
    };
    static Vals v, saved;
    static bool fresh = true;
    static ULONGLONG changed_at;
    auto flush = [&] {
        auto put = [](const char *k, float a, float b2) {
            if (std::fabs(a - b2) > 1e-4f)
                set_cfg_value(k, a);
        };
        put("render_fps", std::round(v.fps), saved.fps);
        put("render_settle", std::round(v.settle), saved.settle);
        put("render_crf", std::round(v.crf), saved.crf);
        put("render_format", std::round(v.format), saved.format);
        put("render_blur", std::round(v.blur), saved.blur);
        put("render_shutter", std::round(v.shutter), std::round(saved.shutter));
        put("render_audio_offset", std::round(v.offset_ms) / 1000, std::round(saved.offset_ms) / 1000);
        put("render_audio_wait", v.wait, saved.wait);
        put("preroll", v.preroll, saved.preroll);
        put("letterbox", v.letterbox, saved.letterbox);
        put("fade", v.fade, saved.fade);
        put("render_audio", v.audio, saved.audio);
        put("move_listener", v.listener, saved.listener);
        put("sound_follows_shot", v.follow, saved.follow);
        put("recorded_passby", v.passby, saved.passby);
        put("editor_hides_hud", v.hidehud, saved.hidehud);
        put("key_curve", std::round(v.keycurve), saved.keycurve);
        put("check_clipping", v.clip, saved.clip);
        saved = v;
        changed_at = 0;
    };
    if (!open) {
        if (changed_at)
            flush();
        fresh = true;
        return;
    }
    if (fresh) {  // the file's values each time the pane opens
        v = {float(st.o_fps), float(st.o_settle), float(st.o_crf), st.o_offset * 1000, st.o_wait, st.o_preroll,
             st.letterbox, st.fade, float(st.o_blur), st.o_shutter, st.o_audio, st.o_listener, st.o_follow,
             st.o_passby, float(st.o_format), st.o_hidehud, float(st.o_keycurve), st.o_clip};
        saved = v;
        fresh = false;
    }
    ov::Rect safe = ov::safe_area(p);
    float row = u.u(30), gap = u.u(5), sl = u.u(40), colgap = u.u(30), w = u.u(1180);
    float top = safe.y + u.u(84), h = safe.y + safe.h - u.u(kTimelineH) - u.u(10) - top;
    ov::Rect c = oui::panel(u, {safe.x + (safe.w - w) / 2, top, w, h}, "OPTIONS");
    float colw = (c.w - colgap) / 2;
    float cx = c.x, cw = colw, y = c.y;  // the column being laid out
    auto section = [&](const char *name) {
        y += gap;
        oui::label(u, {cx, y, cw, row}, name, kAccent);
        y += row;
    };
    auto slider = [&](const char *name, float &x, float lo, float hi, const char *fmt) {
        if (oui::slider(u, {cx, y, cw, sl}, name, x, lo, hi, fmt))
            changed_at = GetTickCount64();
        y += sl + gap;
    };
    auto choice = [&](const char *name, float &x, std::initializer_list<std::pair<const char *, float>> opts) {
        oui::label(u, {cx, y, u.u(150), row}, name, kDim);
        float bx = cx + u.u(155), bw = (cw - u.u(155) - gap * float(opts.size() - 1)) / float(opts.size());
        for (auto &o : opts) {
            if (std::fabs(x - o.second) < 1e-3f)
                ov::rect_rounded(p, bx - u.u(2), y - u.u(2), bw + u.u(4), row + u.u(4), u.u(6), kAccent);
            if (oui::button(u, {bx, y, bw, row}, o.first))
                x = o.second, changed_at = GetTickCount64();
            bx += bw + gap;
        }
        y += row + gap;
    };
    auto toggle = [&](const char *name, bool &b) {  // a full-width row
        if (oui::toggle(u, {cx, y, cw, row}, name, b))
            changed_at = GetTickCount64();
        y += row + gap;
    };

    // left column: rendering
    section("RENDER");
    choice("Frame rate", v.fps, {{"24", 24}, {"30", 30}, {"60", 60}, {"120", 120}});
    slider("Settle frames (more = cleaner frames, slower)", v.settle, 8, 24, "%.0f");
    choice("Format", v.format, {{"H.264", 0}, {"H.265", 3}, {"ProRes HQ", 1}, {"ProRes 4444", 2}});
    if (v.format < 0.5f || v.format > 2.5f)
        slider("Quality (lower = better, bigger file)", v.crf, 0, 35, "%.0f");
    choice("Motion blur", v.blur, {{"Off", 1}, {"4", 4}, {"8", 8}, {"16", 16}});
    if (v.blur > 1)
        slider("Shutter (180 = film look; takes blur x longer)", v.shutter, 30, 360, "%.0f deg");
    toggle("Record sound", v.audio);
    slider("Sound offset (+ = earlier)", v.offset_ms, -1000, 1000, "%.0f ms");
    slider("Sound pass: load wait before recording", v.wait, 1, 10, "%.1f s");
    {
        std::string dir = st.o_dir[0] ? std::string(st.o_dir) : std::string("Videos\\Cinematic Replay");
        oui::label(u, {cx, y, cw - u.u(140), row}, ("Output: " + dir).c_str(), kDim);
        if (oui::button(u, {cx + cw - u.u(134), y, u.u(134), row}, "Open folder"))
            if (st.o_dir[0]) {
                CreateDirectoryW(wide_of(st.o_dir).c_str(), nullptr);
                ShellExecuteW(nullptr, L"open", wide_of(st.o_dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        y += row + gap;
    }

    // right column: editor, Final Cut, sound
    cx = c.x + colw + colgap;
    y = c.y;
    section("EDITOR");
    {
        bool snap = g_snap.load(), path = g_show_path.load();
        if (oui::toggle(u, {cx, y, cw, row}, "Snap to playhead / edges (N)", snap))
            g_snap = snap;
        y += row + gap;
        if (oui::toggle(u, {cx, y, cw, row}, "Camera path in 3D (J)", path))
            g_show_path = path;
        y += row + gap;
    }
    toggle("Hide the game's HUD while editing", v.hidehud);
    choice("New slider keys", v.keycurve, {{"Smooth", 0}, {"Linear", 1}});  // only keys made from now on
    toggle("Path: show where it clips into the track", v.clip);
    section("FINAL CUT");
    slider("Run-up before the first shot (black)", v.preroll, 0, 5, "%.1f s");
    choice("Letterbox", v.letterbox, {{"Off", 0}, {"1.85", 1.85f}, {"2.00", 2.0f}, {"2.39", 2.39f}});
    slider("Fade in / out (Final Cut only)", v.fade, 0, 3, "%.1f s");
    section("SOUND");
    toggle("Hear from our camera *", v.listener);
    toggle("Live engines (car-camera mix)", v.follow);
    toggle("Recorded trackside pass-bys *", v.passby);
    oui::label(u, {cx, y, cw, row}, "* from the next game start. Saved automatically.", kDim);

    // save what changed, once the controls have been still for a moment
    if (changed_at && GetTickCount64() - changed_at > 300 && !g_ui.active)
        flush();
}

cine::ReplayId current_replay();
cine::ReplayId edit_replay(const std::string &name);
bool edit_fits(const cine::Edit &e, const cine::ReplayId &id);

// `s` shortened in the middle with "..." so it fits `max_w` at the UI's body text size (keeps the start and the
// end, where the game's replay names carry the date).
std::string fit_text(oui::Ui &u, const std::string &s, float max_w)
{
    auto width = [&](const std::string &t) {
        float w = 0;
        ov::measure(u.p, u.style(u.th.body, 0, 0), t, &w, nullptr);
        return w;
    };
    if (width(s) <= max_w)
        return s;
    for (size_t cut = 1; cut < s.size(); cut++) {
        size_t keep = s.size() - cut, head = (keep + 1) / 2, tail = keep - head;
        std::string t = s.substr(0, head) + "..." + s.substr(s.size() - tail);
        if (width(t) <= max_w)
            return t;
    }
    return "...";
}

// Whether edit `name` belongs to the replay on screen: made on it, or older (no replay recorded) with shots that
// fit it (each shot's driver in its car slot, within the replay's length).
bool edit_is_here(const std::string &name, const cine::ReplayId &now)
{
    cine::ReplayId id = edit_replay(name);
    if (id.known())
        return id.same(now);
    std::string text, err;
    if (!now.known() || !read_file(g_dir + "\\sequences\\" + name + ".json", text))
        return false;
    swofty::json::Error e;
    auto doc = swofty::json::parse(text, &e);
    if (!e.ok())
        return false;
    return edit_fits(cine::edit_from_json(doc, err), now);
}

void edits_menu(oui::Ui &u, SwoftyPaint *p, const Status &st)
{
    static std::vector<std::string> names;
    static std::vector<bool> here;  // names[i] was made on the replay on screen
    static ULONGLONG listed;
    if (GetTickCount64() - listed > 1000) {
        names = list_edits();
        cine::ReplayId now_id = current_replay();
        here.clear();
        for (const auto &n : names)
            here.push_back(n == st.seq || edit_is_here(n, now_id));
        listed = GetTickCount64();
    }
    auto unused = [&](const std::string &base) {
        for (int i = base == "edit" ? 1 : 2;; i++) {
            std::string n = base + "_" + std::to_string(i);
            if (std::find(names.begin(), names.end(), n) == names.end())
                return n;
        }
    };
    ov::Rect safe = ov::safe_area(p);
    float row = u.u(30), gap = u.u(5), w = u.u(460);
    // only this replay's edits (14 rows at most)
    std::vector<int> order;
    for (int i = 0; i < int(names.size()); i++)
        if (size_t(i) < here.size() && here[size_t(i)])
            order.push_back(i);
    int shown = std::min(int(order.size()), 14);
    float h = u.u(60) + (shown + 4) * (row + gap);
    ov::Rect c = oui::panel(u, {safe.x + u.u(470), safe.y + safe.h - u.u(kTimelineH) - h - u.u(10), w, h},
                            "EDITS FOR THIS REPLAY");
    float y = c.y;
    for (int k = 0; k < shown; k++) {
        const std::string &nm = names[size_t(order[size_t(k)])];
        if (nm == st.seq)
            ov::rect_rounded(p, c.x - u.u(2), y - u.u(2), c.w + u.u(4), row + u.u(4), u.u(6), kAccent);
        if (oui::button(u, {c.x, y, c.w, row}, fit_text(u, nm, c.w - u.u(16))) && nm != st.seq) {
            Cmd cmd{CmdKind::SwitchEdit};
            cmd.name = nm;
            post(cmd);
            g_edits_menu = false;
        }
        y += row + gap;
    }
    float third = (c.w - 2 * gap) / 3;
    y += gap;
    if (oui::button(u, {c.x, y, third, row}, "New")) {
        Cmd cmd{CmdKind::SwitchEdit};
        cmd.name = unused("edit");
        cmd.type = 1;
        post(cmd);
        g_edits_menu = false;
        listed = 0;
    }
    if (oui::button(u, {c.x + third + gap, y, third, row}, "Duplicate")) {
        Cmd cmd{CmdKind::SwitchEdit};
        cmd.name = unused(st.seq);
        cmd.type = 2;
        post(cmd);
        g_edits_menu = false;
        listed = 0;
    }
    if (oui::button(u, {c.x + 2 * (third + gap), y, third, row}, "Close"))
        g_edits_menu = false;
    y += row + gap * 3;

    // rename the current edit
    static std::string new_name, for_edit;
    if (for_edit != st.seq) {
        new_name = st.seq;
        for_edit = st.seq;
    }
    float bw = u.u(110);
    oui::text_field(u, {c.x, y, c.w - bw - gap, row}, "edit_rename", new_name, 48, "new name");
    std::string clean;  // letters, digits, space, - and _ only (it is a file name)
    for (char ch : new_name)
        if (isalnum(uint8_t(ch)) || ch == '_' || ch == '-' || ch == ' ')
            clean += ch;
    while (!clean.empty() && clean.back() == ' ')
        clean.pop_back();
    if (oui::button(u, {c.x + c.w - bw, y, bw, row}, "Rename") && !clean.empty() && clean != st.seq) {
        Cmd cmd{CmdKind::SwitchEdit};
        cmd.name = clean;
        cmd.type = 3;
        post(cmd);
        listed = 0;
    }
    y += row + gap;

    // delete the current edit: a second click within 3 s confirms
    static ULONGLONG armed;
    bool confirm = GetTickCount64() - armed < 3000;
    std::string label = confirm ? "Click again to delete " + std::string(st.seq) : "Delete this edit";
    if (confirm)
        ov::rect_rounded(p, c.x - u.u(2), y - u.u(2), c.w + u.u(4), row + u.u(4), u.u(6), 0xc0392bff);
    if (oui::button(u, {c.x, y, c.w, row}, label)) {
        if (!confirm) {
            armed = GetTickCount64();
        } else {
            std::string next;  // another edit of this replay, else a new empty one
            for (size_t i = 0; i < names.size() && i < here.size(); i++)
                if (here[i] && names[i] != st.seq) {
                    next = names[i];
                    break;
                }
            Cmd cmd{CmdKind::SwitchEdit};
            cmd.name = next;
            cmd.type = 4;
            post(cmd);
            armed = 0;
            listed = 0;
        }
    }
}

// ----- undo / redo (paint thread, g_seq_mx held) ------------------------------------------------------
// A history step is taken when a change is saved (400 ms after the last edit), so one slider drag or one
// timeline drag is one step.

void history_commit()
{
    if (g_hist_reset) {
        g_undo.clear();
        g_redo.clear();
        g_prev = g_edit;
        g_hist_reset = false;
        return;
    }
    g_undo.push_back(std::move(g_prev));
    if (g_undo.size() > 100)
        g_undo.erase(g_undo.begin());
    g_redo.clear();
    g_prev = g_edit;
}

void undo_redo(bool redo)
{
    if (g_drag != Drag::none)
        return;
    if (g_dirty) {
        history_commit();
        save_sequence_locked();
    }
    auto &from = redo ? g_redo : g_undo;
    auto &to = redo ? g_undo : g_redo;
    if (from.empty()) {
        toast(redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    to.push_back(g_edit);
    g_edit = std::move(from.back());
    from.pop_back();
    g_prev = g_edit;
    save_sequence_locked();
    if (g_shot >= int(g_edit.shots.size()))
        g_shot = -1;
    // the snapshot carries the selection of its time; keep it to the inspected shot so no stale flag lingers
    for (int i = 0; i < int(g_edit.shots.size()); i++)
        g_edit.shots[size_t(i)].selected = i == g_shot;
    toast(redo ? "Redo" : "Undo");
}

// Pastes shots at time t, overwriting what is there (shots across the range's ends are split there).
void paste_shots(cine::Edit &ed, float t)
{
    if (g_clip.empty())
        return;
    float d = t - g_clip.front().t0, p0 = t, p1 = g_clip.back().t1 + d;
    for (float edge : {p0, p1}) {
        int i = ed.shot_at(edge);
        if (i >= 0 && !cine::split_shot(ed, i, edge)) {  // too near an end to split: trim instead
            auto &o = ed.shots[size_t(i)];
            if (edge - o.t0 < o.t1 - edge)
                o.t0 = edge;
            else
                o.t1 = edge;
        }
    }
    std::erase_if(ed.shots, [&](const cine::Shot &o) { return o.t0 >= p0 - 1e-3f && o.t1 <= p1 + 1e-3f; });
    std::erase_if(ed.shots, [](const cine::Shot &o) { return o.t1 - o.t0 < 0.05f; });
    for (auto &o : ed.shots)
        o.selected = false;
    for (cine::Shot s : g_clip) {
        s.t0 += d;
        s.t1 += d;
        for (auto &k : s.keys)
            k.t += d;
        for (auto &tr : s.tracks)
            for (auto &pk : tr.keys)
                pk.t += d;
        s.selected = true;
        ed.shots.push_back(s);
    }
    ed.sort();
    g_shot = -1;
    for (int i = 0; i < int(ed.shots.size()); i++)
        if (ed.shots[size_t(i)].selected && g_shot < 0)
            g_shot = i;
    mark_dirty();
}

bool g_looking;  // right mouse held: the cursor is pinned while it looks
POINT g_look_center, g_look_restore;

// The game window's client centre in screen coordinates (when the game is in front).
bool game_center(POINT &out)
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    RECT rc;
    if (pid != GetCurrentProcessId() || !GetClientRect(w, &rc))
        return false;
    out = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    return ClientToScreen(w, &out);
}

void paint(SwoftyPaint *p, void *)
{
    oui::Ui u(p, g_ui);
    const auto &in = u.in;
    bool open = g_editor.load();
    if (open && in.key_pressed(VK_SPACE) && !in.key_pressed(VK_RETURN))
        u.accept = false;  // Space plays / pauses (below), it doesn't press the highlighted button
    ov::Rect safe = ov::safe_area(p);
    float timeline_top = safe.y + safe.h - u.u(kTimelineH);
    {
        std::lock_guard<std::mutex> lk(g_kb_mx);
        KbState &k = g_kb;
        if (in.key_pressed(VK_F9))
            k.toggle++;
        bool fly = open && !g_ui.edit;
        bool ctrl = in.key_down(VK_CONTROL);
        k.w = fly && in.key_down('W');
        k.a = fly && in.key_down('A');
        k.s = fly && in.key_down('S');
        k.d = fly && in.key_down('D');
        k.q = fly && in.key_down('Q');
        k.e = fly && in.key_down('E');
        k.z = fly && in.key_down('Z') && !ctrl;
        k.c = fly && in.key_down('C') && !ctrl;
        k.r = fly && in.key_down('R');
        k.f = fly && in.key_down('V') && !ctrl;  // zoom out (F toggles fly)
        k.up = fly && in.key_down(VK_UP);
        k.down = fly && in.key_down(VK_DOWN);
        k.left = fly && in.key_down(VK_LEFT);
        k.right = fly && in.key_down(VK_RIGHT);
        k.shift = fly && in.key_down(VK_SHIFT);
        k.ctrl = fly && ctrl;
        bool rmb = fly && in.held(ov::mouse::right) && g_drag != Drag::pan;
        if (rmb && !g_looking && (in.pressed & ov::mouse::right) && (in.y < timeline_top || g_hide_ui.load())) {
            POINT c;
            if (game_center(c)) {
                GetCursorPos(&g_look_restore);
                g_look_center = c;
                SetCursorPos(c.x, c.y);
                g_looking = true;
            }
        } else if (g_looking && rmb) {
            POINT cur;
            GetCursorPos(&cur);
            k.mdx += float(cur.x - g_look_center.x);
            k.mdy += float(cur.y - g_look_center.y);
            SetCursorPos(g_look_center.x, g_look_center.y);
        } else if (g_looking) {
            SetCursorPos(g_look_restore.x, g_look_restore.y);
            g_looking = false;
        }
        k.rmb = g_looking;
        if (fly) {
            if (in.key_down('W') || in.key_down('A') || in.key_down('S') || in.key_down('D') || g_looking || in.pressed)
                g_device = 0;
            if (in.key_pressed('K'))
                k.add++;
            if (in.key_pressed('F'))
                k.fly++;
            if (in.key_pressed('O'))
                k.orbit++;
            if (in.key_pressed('T'))
                k.ride++;
            if (in.key_pressed('P') || in.key_pressed(VK_SPACE))  // freeze / run
                k.freeze++;
            if (in.key_pressed('L'))
                (in.key_down(VK_SHIFT) ? k.flat : k.align)++;
            if (in.key_pressed('G'))
                g_guides = !g_guides.load();
            if (in.key_pressed('H'))
                g_help = !g_help.load();
            float stp = in.key_down(VK_SHIFT) ? 1.0f : ctrl ? 1.0f / 60 : 0.1f;
            if (in.key_typed(VK_OEM_4))
                k.step -= stp;
            if (in.key_typed(VK_OEM_6))
                k.step += stp;
            if (in.key_pressed(VK_HOME))
                (in.key_down(VK_SHIFT) ? k.level_surface : k.level)++;
            if (in.key_pressed(VK_OEM_MINUS))
                k.speed_dir--;
            if (in.key_pressed(VK_OEM_PLUS))
                k.speed_dir++;
            bool over_panel = in.x >= safe.x + safe.w - u.u(500) ||  // the inspector's sliders take the wheel
                              (g_props_w > 0 && in.x <= safe.x + g_props_w);
            if (in.wheel != 0 && ((in.y < timeline_top && !over_panel) || g_hide_ui.load()))
                k.wheel += in.wheel;
        }
    }
    if (open != g_focus) {  // focus keeps keyboard and mouse from the game while the editor is open
        ov::focus(open);
        g_focus = open;
    }

    Status st;
    {
        std::lock_guard<std::mutex> lk(g_status_mx);
        st = g_status;
        while (!g_toasts.empty()) {
            oui::toast(g_ui, g_toasts.front(), 2.5f);
            g_toasts.pop_front();
        }
    }
    if (st.in_replay && open) {
        std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
        cine::Edit &ed = g_edit;
        if (int want = g_select_shot.exchange(-2); want >= -1) {
            // a shot picked by the program (a new shot, a jump to a key, select_shot): it alone is selected. Left
            // as it was, a shot clicked earlier stayed selected: drags moved both (held by the other's
            // neighbours) and clicks didn't clear it (owner's bug, cleared by a Shift-click)
            g_shot = want;
            for (int i = 0; i < int(ed.shots.size()); i++)
                ed.shots[size_t(i)].selected = i == want;
        }
        if (g_hist_reset && !g_dirty)
            history_commit();  // a fresh edit: history starts here
        bool keys_ok = !g_ui.edit;
        bool ctrl = in.key_down(VK_CONTROL), shift = in.key_down(VK_SHIFT);
        if (keys_ok && ctrl && (in.key_pressed('Z') || in.key_pressed('Y')))
            g_want_undo = in.key_pressed('Y') || shift ? 2 : 1;
        if (g_want_undo) {
            undo_redo(g_want_undo == 2);
            g_want_undo = 0;
        }
        if (keys_ok && in.key_pressed('N')) {
            g_snap = !g_snap.load();
            toast(g_snap.load() ? "Snap on" : "Snap off");
        }
        if ((keys_ok && in.key_pressed('B')) || g_want_split) {
            g_want_split = false;
            int at = ed.shot_at(st.t);
            if (at < 0) {
                toast("No shot under the playhead to split");
            } else if (cine::split_shot(ed, at, st.t)) {
                for (auto &o : ed.shots)
                    o.selected = false;
                g_shot = at + 1;
                ed.shots[size_t(g_shot)].selected = true;
                mark_dirty();
                toast("Split shot " + std::to_string(at + 1) + " at " + mmss(st.t));
            } else {
                toast("Too near the shot's start or end to split");
            }
        }
        // Delete: the selected shot's selected keys, else every selected shot
        if (keys_ok && in.key_pressed(VK_DELETE) && g_shot >= 0 && g_shot < int(ed.shots.size())) {
            auto &keys = ed.shots[size_t(g_shot)].keys;
            size_t before = keys.size();
            std::erase_if(keys, [](const cine::ShotKey &k) { return k.selected; });
            if (keys.size() == before) {
                ed.shots[size_t(g_shot)].selected = true;
                std::erase_if(ed.shots, [](const cine::Shot &o) { return o.selected; });
                g_shot = -1;
            }
            mark_dirty();
        }
        if (!g_ui.edit && (in.key_pressed(VK_OEM_COMMA) || in.key_pressed(VK_OEM_PERIOD))) {
            int dir = in.key_pressed(VK_OEM_PERIOD) ? 1 : -1;
            static float last_target = -1;  // repeated presses before the clock catches up go on from the last key
            float from = std::fabs(st.t - last_target) < 0.5f ? last_target : st.t;
            if (auto kt = jump_key(ed, from, dir)) {
                post({CmdKind::Seek, *kt});
                last_target = *kt;
            } else {
                toast(dir > 0 ? "No key after this" : "No key before this");
            }
        }
        // Ctrl+C: the selected keys of the selected shot if any, else the selected shots
        if (keys_ok && ctrl && in.key_pressed('C') && g_shot >= 0 && g_shot < int(ed.shots.size())) {
            const cine::Shot &cur = ed.shots[size_t(g_shot)];
            std::vector<cine::ShotKey> ks;
            for (const auto &k : cur.keys)
                if (k.selected)
                    ks.push_back(k);
            if (!ks.empty()) {
                for (auto &k : ks)
                    k.t -= ks.front().t;
                g_key_clip = ks;
                g_key_clip_type = cur.type;
                g_clip_kind = Clip::keys;
                toast("Copied " + std::to_string(ks.size()) + " key(s)");
            } else {
                g_clip.clear();
                for (int i = 0; i < int(ed.shots.size()); i++)
                    if (ed.shots[size_t(i)].selected || i == g_shot)
                        g_clip.push_back(ed.shots[size_t(i)]);
                g_clip_kind = Clip::shots;
                toast("Copied " + std::to_string(g_clip.size()) + " shot(s)");
            }
        }
        if (keys_ok && ctrl && in.key_pressed('V')) {
            if (g_clip_kind == Clip::shots && !g_clip.empty()) {
                paste_shots(ed, st.t);
                toast("Pasted " + std::to_string(g_clip.size()) + " shot(s) at " + mmss(st.t));
            } else if (g_clip_kind == Clip::keys && !g_key_clip.empty()) {
                int at = ed.shot_at(st.t);
                if (at < 0 || ed.shots[size_t(at)].type != g_key_clip_type) {
                    toast(std::string("Keys paste into a ") + cine::shot_type_name(g_key_clip_type) +
                          " shot under the playhead");
                } else {
                    cine::Shot &dst = ed.shots[size_t(at)];
                    for (auto &k : dst.keys)
                        k.selected = false;
                    for (cine::ShotKey k : g_key_clip) {
                        k.t += st.t;
                        if (k.t > dst.t1)
                            break;
                        std::erase_if(dst.keys, [&](const cine::ShotKey &o) { return std::fabs(o.t - k.t) < 1.0f / 60; });
                        k.selected = true;
                        dst.keys.push_back(k);
                    }
                    dst.sort_keys();
                    g_shot = at;
                    mark_dirty();
                    toast("Pasted " + std::to_string(g_key_clip.size()) + " key(s)");
                }
            }
        }
        g_path_shot = g_shot;
        if (keys_ok && in.key_pressed('J')) {
            g_show_path = !g_show_path.load();
            toast(g_show_path.load() ? "Camera path shown" : "Camera path hidden");
        }
        if (keys_ok && in.key_pressed('U'))
            g_hide_ui = !g_hide_ui.load();
        if (!g_hide_ui.load()) {  // hidden: nothing drawn and nothing to click; the shortcuts still work
            draw_paths(u, p, ed);  // under the panels; also the path-shaping handles
            readout(u, p, st);
            inspector(u, p, st, ed);
            props_panel(u, p, st, ed);
            timeline(u, p, st, ed);
            if (g_pick_driver)
                driver_picker(u, p, st, ed);
            if (g_edits_menu)
                edits_menu(u, p, st);
            if (g_play_menu)
                play_menu(u, p);
            if (g_render_menu)
                render_menu(u, p, st, ed);
            options_pane(u, p, st, g_options);  // also saves a change pending when it closes
            if (keys_ok && in.key_pressed(VK_ESCAPE))
                g_pick_driver = g_edits_menu = g_render_menu = g_options = g_play_menu = false;
            if (g_help.load())
                help_panel(u, p);
        }
        if (g_dirty && GetTickCount64() - g_dirty_at > 400 && g_drag == Drag::none && !g_bend.on) {
            history_commit();
            save_sequence_locked();
        }
    } else if (st.in_replay && st.final_cut) {
        // Final Cut: nothing of ours but black, for the run-up before the first shot, the fades and the bars
        float W = 0, H = 0;
        ov::size(p, &W, &H);
        float t = st.t, a = 0, fade = st.rendering ? 0 : st.fade;  // renders: no fades (done in post)
        if (t < st.edit_t0)
            a = 1;
        else if (fade > 0 && t < st.edit_t0 + fade)
            a = 1 - (t - st.edit_t0) / fade;
        else if (fade > 0 && t > st.edit_t1 - fade)
            a = std::min(1.0f, (t - (st.edit_t1 - fade)) / fade);
        if (st.letterbox > 0.5f && W > 0) {
            float bar = std::max(0.0f, (H - W / st.letterbox) / 2);
            if (bar > 0) {
                ov::rect(p, 0, 0, W, bar, 0x000000ff);
                ov::rect(p, 0, H - bar, W, bar, 0x000000ff);
            }
        }
        if (a > 0)
            ov::rect(p, 0, 0, W, H, uint32_t(std::clamp(a, 0.0f, 1.0f) * 255));
        if (st.rendering && st.render_show) {  // never on a captured frame (see render_show)
            ov::TextStyle ts;
            ts.size = u.u(16);
            ts.rgba = 0xffffffc0;
            ts.align = ov::align::left | ov::align::top;
            char line[96];
            if (st.render_phase == 3)
                snprintf(line, sizeof line, "Recording the sound for %d frames   (F9 keeps the video without it)",
                         st.render_frame);
            else
                snprintf(line, sizeof line, "Rendering %d / ~%d   (F9 cancels)", st.render_frame,
                         st.render_total);
            ov::text(p, u.u(12), u.u(8), ts, line);
        }
    } else if (st.in_replay && st.playing) {
        ov::TextStyle ts;
        ts.size = u.u(18);
        ts.rgba = 0xffffffd0;
        ts.align = ov::align::right | ov::align::top;
        char line[128];
        snprintf(line, sizeof line, "Cinematic: %s  shot %d", st.seq, st.shot + 1);
        ov::text_in(p, {safe.x, safe.y + u.u(110), safe.w, u.u(24)}, ts, line);
    }
    if (!(open && g_hide_ui.load()) && !st.final_cut)
        oui::draw_toasts(u);
    g_prev_mx = in.x;
}

// ----- the camera (ViewportManager hook) -----------------------------------------------------------

uint64_t g_frames, g_overrides;
bool g_overriding, g_speed_written;
int g_last_shot = -1;

cine::FreeCam g_fc;
float g_play_speed = 1;  // the replay's speed while editing (camera thread)
constexpr float kPlaySpeeds[] = {0.05f, 0.1f, 0.25f, 0.5f, 1, 2};
bool g_frozen, g_fc_init;  // camera thread only
uint16_t g_prev_buttons;
bool g_prev_y;
LARGE_INTEGER g_qpf, g_last_qpc;

float next_play_speed(float cur, int dir)
{
    int i = 0;
    while (i + 1 < int(std::size(kPlaySpeeds)) && kPlaySpeeds[i] < cur - 1e-3f)
        i++;
    return kPlaySpeeds[std::clamp(i + dir, 0, int(std::size(kPlaySpeeds)) - 1)];
}

// The view we last put on screen. Between our write and our next frame the game writes its own replay
// camera (often a trackside TV camera) into the same entry, so reading the entry at the start of a frame
// gives the game's camera, not ours: K and Fly use this instead while we drive the camera.
struct Shown {
    V3 eye;
    cine::Quat q;
    float fov;
} g_shown;

void write_camera(uintptr_t entry, V3 eye, V3 target, cine::Quat q, float fov, float near_, bool cut)
{
    uint8_t *cam = reinterpret_cast<uint8_t *>(entry + kCam);
    float stock_w = 0, stock_h = 0;
    memcpy(&stock_w, cam + 0x64, 4);
    memcpy(&stock_h, cam + 0x68, 4);
    float q4[4] = {q.x, q.y, q.z, q.w};  // (right, up, back) columns: the game's camera looks along -Z
    float eye4[4] = {eye.x, eye.y, eye.z, 1}, focus4[4] = {target.x, target.y, target.z, 1};
    memcpy(cam + 0x00, eye4, 16);
    memcpy(cam + 0x10, focus4, 16);
    memcpy(cam + 0x20, q4, 16);
    memcpy(cam + 0x5c, &near_, 4);
    if (fov > 1 && fov < 170 && stock_w > 0 && stock_h > 0) {
        float w = 2 * std::tan(fov * cine::kDeg / 2), h = w * (stock_h / stock_w);
        memcpy(cam + 0x64, &w, 4);
        memcpy(cam + 0x68, &h, 4);
    }
    if (cut)
        cam[0xa6] = 1;  // hard cut: TAA and motion-blur history start over
    g_shown = {eye, q, fov > 1 && fov < 170 ? fov : (stock_w > 0 ? 2 * std::atan(stock_w / 2) / cine::kDeg : 60)};
}

void write_view(uintptr_t entry, V3 eye, cine::Quat q, float fov, bool cut)
{
    V3 r, uu, b;
    cine::cols_from_quat(q, r, uu, b);
    write_camera(entry, eye, eye + b * -10.0f, q, fov, 0.05f, cut);
}

void set_speed(const Replay &r, float speed)
{
    *reinterpret_cast<float *>(r.rs + 0x24) = speed;
    g_speed_written = speed != 1.0f;
}

void seek(const Replay &r, float t)
{
    *reinterpret_cast<float *>(r.rs + 0x7c) = std::max(0.0f, t);
    *reinterpret_cast<uint8_t *>(r.rs + 0x7a) = 1;
}

// The camera currently in viewport 0 (as the game or we left it).
void read_view(uintptr_t entry, V3 &eye, cine::Quat &q, float &fov)
{
    float e[4], qq[4], w = 0;
    memcpy(e, reinterpret_cast<void *>(entry + kCam), 16);
    memcpy(qq, reinterpret_cast<void *>(entry + kCam + 0x20), 16);
    memcpy(&w, reinterpret_cast<void *>(entry + kCam + 0x64), 4);
    eye = {e[0], e[1], e[2]};
    q = {qq[0], qq[1], qq[2], qq[3]};
    fov = w > 0 ? 2 * std::atan(w / 2) / cine::kDeg : 60;
}

float axis(int16_t v, float dead = 0.18f)
{
    float f = std::max(-1.0f, v / 32767.0f);
    if (std::fabs(f) < dead)
        return 0;
    return (f - (f > 0 ? dead : -dead)) / (1 - dead);
}

// The car nearest to a point (slot and frame), within 300 m; -2 when none.
int nearest_car(GameCars &cars, V3 at, cine::Frame &out)
{
    int best = -2;
    float bd = 300;
    int n = std::min(slot_count(), 32);
    for (int i = 0; i < n; i++)
        if (auto f = cars.frame(i)) {
            float d = cine::len(f->p - at);
            if (d < bd) {
                bd = d;
                best = i;
                out = *f;
            }
        }
    return best;
}

// What is on screen: our last write while we drive the camera, else the game's camera.
void shown_view(uintptr_t entry, V3 &eye, cine::Quat &q, float &fov)
{
    if (g_overriding) {
        eye = g_shown.eye;
        q = g_shown.q;
        fov = g_shown.fov;
    } else {
        read_view(entry, eye, q, fov);
    }
}

// The view the editor stands on: the free camera while flying, else whatever is on screen.
void editor_view(uintptr_t entry, GameCars &cars, V3 &eye, cine::Quat &q, float &fov)
{
    if (g_fly.load() && g_fc_init) {
        auto rf = g_fc.ride ? cars.frame(g_fc.ride_car) : std::nullopt;
        cine::Frame fr = rf ? *rf : cine::Frame{};
        eye = g_fc.eye(fr);
        q = g_fc.rotation(fr);
        fov = g_fc.fov;
    } else {
        shown_view(entry, eye, q, fov);
    }
}

// A key at the playhead into the shot under it (a new Free path shot when there is none).
void add_key(const Replay &r, GameCars &cars, uintptr_t entry)
{
    V3 eye;
    cine::Quat q;
    float fov;
    editor_view(entry, cars, eye, q, fov);
    std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
    cine::Edit &ed = g_edit;
    int at = ed.shot_at(r.t);
    if (at < 0) {
        cine::Shot s;
        s.type = cine::ShotType::free_path;
        s.t0 = r.t;
        s.t1 = r.t + 5;
        ed.shots.push_back(s);
        ed.sort();
        for (int i = 0; i < int(ed.shots.size()); i++)
            if (ed.shots[size_t(i)].t0 == r.t)
                at = i;
        fit_shot(ed, at);
        toast("New Free path shot at " + mmss(r.t));
    }
    cine::Shot &s = ed.shots[size_t(at)];
    if (!s.keyed()) {
        toast(std::string(cine::shot_type_name(s.type)) + " shots have no keys: use the inspector");
        return;
    }
    auto k = cine::key_from_view(s, r.t, eye, q, fov, cars);
    if (!k) {
        toast("The shot's driver is not in this replay");
        return;
    }
    // a key within a frame of another replaces it
    std::erase_if(s.keys, [&](const cine::ShotKey &o) { return std::fabs(o.t - r.t) < 1.0f / 60; });
    for (auto &o : s.keys)
        o.selected = false;
    k->selected = true;
    s.keys.push_back(*k);
    s.sort_keys();
    g_select_shot = at;
    mark_dirty();
    toast("Key " + std::to_string(s.keys.size()) + " in shot " + std::to_string(at + 1) + " at " + mmss(r.t));
}

// Sets `key = value` in cinecam.cfg (replacing the key's line, or appending it). The camera thread re-reads the
// file when it changes. Any thread.
std::mutex g_cfg_file_mx;
void set_cfg_value(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lk(g_cfg_file_mx);
    std::string path = g_dir + "\\cinecam.cfg", text, out;
    read_file(path, text);
    bool done = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos + 1);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        size_t k = line.find_first_not_of(" \t"), eq = line.find('=');
        if (!done && k != std::string::npos && line[k] != '#' && eq != std::string::npos &&
            trim(line.substr(k, eq - k)) == key) {
            line = key + " = " + value + "\n";
            done = true;
        }
        out += line;
    }
    if (!done)
        out += (out.empty() || out.back() == '\n' ? "" : "\n") + key + " = " + value + "\n";
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
}
void set_cfg_value(const std::string &key, float v)
{
    char b[32];
    snprintf(b, sizeof b, "%g", double(v));
    set_cfg_value(key, std::string(b));
}

// Points cinecam.cfg's "sequence" at another edit (camera thread: it owns g_cfg).
void set_cfg_sequence(const std::string &name) { set_cfg_value("sequence", name); }

// Opens edit `name` (mode 0), starts it empty (1) or as a copy of the current edit (2).
// Renames the current edit to `name` (its file too). False (with a toast) if the name is taken.
bool rename_edit(const std::string &name)
{
    std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
    std::string from = g_dir + "\\sequences\\" + seq_name() + ".json", to = g_dir + "\\sequences\\" + name + ".json";
    if (GetFileAttributesA(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
        toast("There is already an edit called " + name);
        return false;
    }
    if (g_dirty)
        save_sequence_locked();
    if (GetFileAttributesA(from.c_str()) != INVALID_FILE_ATTRIBUTES && !MoveFileA(from.c_str(), to.c_str())) {
        toast("Could not rename the edit's file");
        return false;
    }
    std::string old = seq_name();
    set_cfg_sequence(name);
    g_cfg.sequence = name;
    g_edit.name = name;
    g_seq_loaded = name;
    file_time(to, g_seq_time);
    mark_dirty();  // saves the new name inside the file too
    toast("Renamed " + old + " to " + name);
    return true;
}

// Deletes the current edit: its file moves to sequences\deleted (recoverable), then `next` opens (or a new, empty
// edit when `next` is empty).
void delete_edit(const std::string &next)
{
    std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
    std::string name = seq_name(), dir = g_dir + "\\sequences\\deleted";
    CreateDirectoryA(dir.c_str(), nullptr);
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute,
             tm.wSecond);
    std::string from = g_dir + "\\sequences\\" + name + ".json", to = dir + "\\" + name + "_" + stamp + ".json";
    if (g_dirty)
        save_sequence_locked();
    MoveFileA(from.c_str(), to.c_str());
    g_dirty = false;
    std::string open = next.empty() ? "edit_1" : next;
    set_cfg_sequence(open);
    g_cfg.sequence = open;
    g_seq_loaded.clear();
    g_edit = {};
    load_sequence();
    if (g_seq_loaded != open) {  // no file: a new, empty edit for the replay on screen
        g_edit = {};
        g_edit.name = open;
        g_edit.replay = current_replay();
        g_seq_loaded = open;
    }
    g_hist_reset = true;
    g_select_shot = -1;
    toast("Deleted " + name + " (kept in sequences\\deleted). Now: " + open);
}

cine::ReplayId current_replay();
bool edit_fits(const cine::Edit &e, const cine::ReplayId &id);
void claim_edit(const cine::ReplayId &id, const char *why);

void switch_edit(const std::string &name, int mode)
{
    if (mode == 3) {
        rename_edit(name);
        return;
    }
    if (mode == 4) {
        delete_edit(name);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
    if (g_dirty)
        save_sequence_locked();  // the current edit first, under its own name
    cine::Edit keep = g_edit;
    set_cfg_sequence(name);
    g_cfg.sequence = name;
    cine::ReplayId here = current_replay();
    if (mode == 0) {
        g_seq_loaded.clear();
        load_sequence();
        if (!g_edit.replay.known() && here.known() && edit_fits(g_edit, here))  // an older edit opened here
            claim_edit(here, "opened in it, its shots fit");
    } else {
        g_edit = mode == 2 ? keep : cine::Edit{};
        g_edit.name = name;
        if (mode == 1)
            g_edit.replay = here;  // a new edit belongs to the replay on screen
        g_seq_loaded = name;
        g_seq_time = {};
        mark_dirty();
    }
    g_hist_reset = true;
    g_select_shot = -1;
    toast(std::string(mode == 1 ? "New edit: " : mode == 2 ? "Copied to: " : "Edit: ") + name);
}

// ----- edits per replay ----------------------------------------------------------------------------------
// Each edit records the replay it was made on (cine::ReplayId: the replay's length and its drivers by car slot).
// When the editor is open in a replay, replay_check picks the edit for it: the open edit if it belongs, else the
// newest edit made on this replay, else a new empty one. An edit from before 1.0.0 (no replay recorded) is
// adopted when its shots fit this replay (within its length, and each shot's stored driver in that car slot).
// The check runs once per replay, so an edit the user opens by hand afterwards stays open.
std::mutex g_rid_mx;
cine::ReplayId g_rid;  // the replay on screen (guarded by g_rid_mx); not known until its drivers were read

cine::ReplayId current_replay()
{
    std::lock_guard<std::mutex> lk(g_rid_mx);
    return g_rid;
}

bool edit_fits(const cine::Edit &e, const cine::ReplayId &id)
{
    std::vector<std::string> names;
    for (size_t a = 0, b; a <= id.drivers.size(); a = b + 1) {
        b = id.drivers.find(',', a);
        if (b == std::string::npos)
            b = id.drivers.size();
        names.push_back(id.drivers.substr(a, b - a));
    }
    for (const auto &s : e.shots) {
        if (s.t1 > id.length + 0.5f)
            return false;
        if (s.car >= 0 && !s.driver.empty() && (size_t(s.car) >= names.size() || names[size_t(s.car)] != s.driver))
            return false;
    }
    return true;
}

// The open edit, now known to be this replay's (g_seq_mx held): recorded on its next save.
void claim_edit(const cine::ReplayId &id, const char *why)
{
    g_edit.replay = id;
    mark_dirty();
    swofty::log("[cinecam] edit \"%s\" belongs to this replay (%s)", seq_name().c_str(), why);
}

// The replay recorded in an edit's file (empty when none or unreadable).
cine::ReplayId edit_replay(const std::string &name)
{
    std::string text;
    if (!read_file(g_dir + "\\sequences\\" + name + ".json", text))
        return {};
    swofty::json::Error e;
    auto doc = swofty::json::parse(text, &e);
    cine::ReplayId id;
    if (!e.ok())
        return id;
    id.file = std::string(doc["replay"]["file"].as_string(""));
    id.length = float(doc["replay"]["length"].as_double(0));
    id.drivers = std::string(doc["replay"]["drivers"].as_string(""));
    return id;
}

void replay_check(const Replay &r)
{
    static float seen_end = -1;
    static std::string decided;  // the replay already handled (its file id, else length|drivers)
    static std::string file_id;  // the replay file of the replay on screen ("" = not a file, or not seen)
    static std::string file_stem;
    if (!r.ok || r.end <= 0)
        return;
    if (std::fabs(r.end - seen_end) > 0.01f) {  // another replay: its file and drivers are found anew
        seen_end = r.end;
        file_stem.clear();
        file_id = recent_replay_file(120000, file_stem);  // opened while it loaded; a session's own replay has none
        swofty::log("[cinecam] replay of %.1f s: %s", r.end,
                    file_id.empty() ? "not from a file (or not seen)" : ("file " + file_id.substr(0, 16)).c_str());
        std::lock_guard<std::mutex> lk(g_names_mx);
        for (auto &n : g_names)
            n.clear();
    }
    cine::ReplayId id;
    id.file = file_id;
    id.length = r.end;
    {
        std::lock_guard<std::mutex> lk(g_names_mx);
        int n = std::min(slot_count(), 32), named = 0;
        for (int i = 0; i < n; i++) {
            id.drivers += (i ? "," : "") + g_names[i];
            named += !g_names[i].empty();
        }
        if (n == 0 || named * 2 < n)  // not read yet
            id.drivers.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_rid_mx);
        g_rid = id;
    }
    if (id.drivers.empty() || !g_editor.load())  // the drivers too: an older edit is claimed by them
        return;
    char lb[32];
    snprintf(lb, sizeof lb, "%.3f|", id.length);
    std::string key = !id.file.empty() ? id.file : lb + id.drivers;
    static std::string pending;  // the names arrive over a second or two: act once the same key is read twice
    if (key == decided)
        return;
    if (key != pending) {
        pending = key;
        return;
    }
    decided = key;
    std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
    if (g_edit.replay.same(id))
        return;
    if (!g_edit.replay.known() && edit_fits(g_edit, id)) {
        claim_edit(id, g_edit.shots.empty() ? "empty" : "its shots fit");
        return;
    }
    // the newest edit made on this replay
    std::string best;
    FILETIME best_t{};
    for (const auto &name : list_edits()) {
        if (name == seq_name() || !edit_replay(name).same(id))
            continue;
        FILETIME t{};
        if (file_time(g_dir + "\\sequences\\" + name + ".json", t) && (best.empty() || CompareFileTime(&t, &best_t) > 0))
            best = name, best_t = t;
    }
    if (!best.empty()) {
        switch_edit(best, 0);
        toast("This replay's edit: " + best);
        return;
    }
    // none yet: a new, empty edit for this replay, named after its file (else the date)
    std::string base;
    for (char ch : file_id.empty() ? std::string() : file_stem)  // a file name: letters, digits, - and _ only
        if (isalnum(uint8_t(ch)) || ch == '_' || ch == '-')
            base += ch;
    if (base.empty()) {
        SYSTEMTIME tm;
        GetLocalTime(&tm);
        char b[40];
        snprintf(b, sizeof b, "replay_%04d-%02d-%02d", tm.wYear, tm.wMonth, tm.wDay);
        base = b;
    }
    auto names = list_edits();
    std::string name = base;
    for (int i = 2; std::find(names.begin(), names.end(), name) != names.end(); i++)
        name = base + "_" + std::to_string(i);
    std::string was = seq_name();
    switch_edit(name, 1);
    g_edit.replay = id;
    toast("New replay: new edit " + name + " (" + was + " is in the Edit menu)");
}

// ----- offline render (Render): Final Cut, stepped frame by frame and captured into ffmpeg ----------------------
// Phases: 0 the real-time run-up (the HUD presses happen here), 1 waiting for the stepped state, 2 frames. In the
// photo-mode replay state (5) the replay advances by exactly RS+0x28 seconds once, then holds. Each output frame: hold `settle` rendered frames (the image settles), capture, then step speed / fps.
std::atomic<int> g_want_rs_state{-1};  // camera thread -> director thread: setState (2 Play, 5 stepped)
using SetState_t = void (*)(uintptr_t rs, uint32_t state, uint32_t notify);
constexpr uintptr_t kRsSetStateLockedRva = 0x45c74b0;  // ReplaySystem::setState with its lock (L1)

// Phases: 0 run-up, 1 to the stepped state, 2 video frames, 3 the sound pass (real time, recording the game's
// audio), after which ffmpeg joins the two.
struct RenderJob {
    bool on, waiting, audio;
    int phase, frame, hold, settle, total;
    float t0, t1, fps, time;
    int64_t t0_qpc;           // sound pass: when the replay reached the edit's start (QPC, 100 ns)
    int blur, sub, cap;       // motion blur: captures per frame (1 = off), this frame's capture, captures so far
    int format;               // render_format at the start (the file type)
    std::string trace;        // render_keep_parts, per output frame: replay time, eye, every car's centre (CSV)
    std::string trace_rt;     // sound pass, per game frame: QPC time, replay time, eye, every car's centre (CSV)
    float first_rt;           // the replay clock at the first frame's capture (what the video really starts on)
    int reseeks;              // start corrections: the stepped seek landed off the first frame's time
    float shutter;            // motion blur: the shutter as a fraction of the frame time (180 degrees = 0.5)
    float frame_time;         // the replay time of the frame being made
    bool audio_started;       // sound pass: the load hold is over, recording
    ULONGLONG hold_until;     // sound pass: the end of the load hold (GetTickCount64)
    float last_t, drift;      // sound pass: last replay time; worst lag of the replay behind real time (s)
    double expect;            // sound pass: real seconds the replay should have taken since the edit's start
    HWND hwnd;
    std::wstring base, ffmpeg;  // output path without the extension; ffmpeg.exe
} g_render;
render::Recorder g_rec;
audiocap::Recorder g_audio;

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The game's main window: this process's largest visible top-level window.
HWND game_hwnd()
{
    struct Best {
        HWND h;
        LONG area;
    } best{nullptr, 0};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            RECT rc;
            if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetClientRect(h, &rc)) {
                auto *b = reinterpret_cast<Best *>(lp);
                if (rc.right * rc.bottom > b->area)
                    *b = {h, rc.right * rc.bottom};
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));
    return best.h;
}

void finish_final_cut(const Replay &r)
{
    g_preview = false;
    if (g_final.exchange(false))
        hud_request(g_cfg.editor_hides_hud);  // the editor opens again: its HUD setting
    g_editor = true;
    g_fc_init = false;
    if (g_final_return >= 0)
        seek(r, g_final_return);
}

// Runs ffmpeg with `args` (after the exe) and waits; returns its exit code (-1 if it didn't run).
int run_ffmpeg(const std::wstring &exe, const std::wstring &args, const std::wstring &log)
{
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE lg = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, OPEN_ALWAYS, 0, nullptr);
    if (lg != INVALID_HANDLE_VALUE)
        SetFilePointer(lg, 0, nullptr, FILE_END);
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = lg != INVALID_HANDLE_VALUE ? lg : nullptr;
    si.hStdError = si.hStdOutput;
    PROCESS_INFORMATION pi{};
    int code = -1;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD c = 0;
        GetExitCodeProcess(pi.hProcess, &c);
        code = int(c);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (lg != INVALID_HANDLE_VALUE)
        CloseHandle(lg);
    return code;
}

// The file extension of a render format (render_format): H.264 and H.265 in .mp4, ProRes in .mov.
bool render_is_mp4(int format) { return format == 0 || format == 3; }
const wchar_t *render_ext(int format) { return render_is_mp4(format) ? L".mp4" : L".mov"; }

// Finishes the files on a worker: waits for the video encode, then joins the sound to it (the WAV cut `offset`
// seconds in: where the first frame's replay moment is, plus the user's render_audio_offset) and removes the
// parts. Without sound, the video file becomes the final one. `trace` (render_keep_parts): the video pass's
// per-frame CSV, kept with the parts for checking the sync.
void render_finish_files(std::wstring base, std::wstring ffmpeg, int frames, bool with_audio, double offset,
                         float fps, std::string trace, int format)
{
    bool keep_parts = g_cfg.render_keep_parts != 0;
    std::thread([=] {
        const wchar_t *ext = render_ext(format);
        if (with_audio && keep_parts)
            if (FILE *f = _wfopen((base + L".sync.csv").c_str(), L"wb")) {
                fprintf(f, "# nominal %.6f fps %.3f\n", offset, fps);
                fwrite(trace.data(), 1, trace.size(), f);
                fclose(f);
            }
        std::wstring video = base + L".video" + ext, wav = base + L".wav", out = base + ext,
                     log = out + L".ffmpeg.log", vlog = video + L".ffmpeg.log";
        int vcode = g_rec.stop();
        if (vcode != 0) {
            toast("ffmpeg reported a problem with the video: see " + narrow(vlog));
            swofty::log("[cinecam] render video: ffmpeg exit %d", vcode);
            return;
        }
        if (with_audio) {
            // H.264 / H.265: AAC in .mp4 (H.265 keeps the hvc1 tag through the copy, for Apple players). ProRes:
            // uncompressed 24-bit PCM in .mov, as editors expect with it.
            const wchar_t *acodec = format == 3   ? L"-c:a aac -b:a 320k -tag:v hvc1 -movflags +faststart"
                                    : format == 0 ? L"-c:a aac -b:a 320k -movflags +faststart"
                                                  : L"-c:a pcm_s24le";
            wchar_t args[2048];
            swprintf(args, 2048,
                     L"-y -hide_banner -loglevel warning -i \"%ls\" -ss %.6f -i \"%ls\" -map 0:v "
                     L"-map 1:a -c:v copy -af apad %ls -t %.6f \"%ls\"",  // the video's exact length (-shortest
                     video.c_str(), std::max(0.0, offset), wav.c_str(), acodec,  // cut the sound 16-60 ms early)
                     double(frames) / double(fps), out.c_str());
            int code = run_ffmpeg(ffmpeg, args, log);
            swofty::log("[cinecam] render: joined the sound (offset %.4f s): ffmpeg exit %d", offset, code);
            if (code == 0) {
                if (!keep_parts) {
                    DeleteFileW(video.c_str());
                    DeleteFileW(wav.c_str());
                }
                DeleteFileW(vlog.c_str());  // the logs (empty or a harmless note when it went fine)
                DeleteFileW(log.c_str());
                toast("Rendered " + std::to_string(frames) + " frames with sound to " + narrow(out));
                return;
            }
            toast("Joining the sound failed (see " + narrow(log) + "): the video has no sound");
        }
        MoveFileExW(video.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (!with_audio)
            DeleteFileW(vlog.c_str());
        toast("Rendered " + std::to_string(frames) + " frames to " + narrow(out));
    }).detach();
}

// Ends a render. ok in phase 2 (the video done): on to the sound pass when it is on, else finish. Anything else
// (a cancel, a failure, the sound pass done) finishes: the files on a worker, the replay back to the editor.
void render_end(const Replay &r, bool ok, const std::string &why)
{
    RenderJob &R = g_render;
    if (!R.on)
        return;
    if (ok && R.phase == 2 && R.audio) {  // the video is done: the sound pass, the same edit in real time
        // Jump to the run-up and hold there (still, muted) while the game loads that part of the replay; the
        // recording and the playback start together once it has settled (render_tick).
        R.phase = 3;
        R.audio_started = false;
        R.t0_qpc = 0;
        R.hold_until = GetTickCount64() + ULONGLONG(std::max(0.5f, g_cfg.render_audio_wait) * 1000);
        R.drift = 0;
        R.last_t = -1;
        g_want_rs_state = 2;
        g_preview_inside = false;
        seek(r, std::max(0.0f, R.t0 - std::max(1.0f, g_cfg.preroll)));
        swofty::log("[cinecam] render: %d frames done, now the sound pass (holding %.1f s to load)", R.frame,
                    g_cfg.render_audio_wait);
        return;
    }
    bool sound_ok = R.phase == 3 && R.audio_started && ok && R.t0_qpc != 0;
    double offset = 0;  // where the edit's start is in the recording, by the replay clock
    if (R.phase == 3 && R.audio_started) {
        g_audio.stop();
        offset = double(R.t0_qpc - g_audio.first_qpc()) / 1e7;
        if (R.first_rt >= 0) {  // the video starts where the replay really was at its first frame
            offset += double(R.first_rt - R.t0);
            swofty::log("[cinecam] render: the video starts at replay %.4f s (edit %.4f s, %d re-seek(s))", R.first_rt,
                        R.t0, R.reseeks);
        }
        if (g_audio.first_qpc() == 0)
            sound_ok = false;
        swofty::log("[cinecam] render: sound pass drift %.3f s (replay behind real time at the worst moment)",
                    R.drift);
        if (!R.trace_rt.empty())
            if (FILE *f = _wfopen((R.base + L".soundpass.csv").c_str(), L"wb")) {
                fprintf(f, "# first_qpc %lld t0_qpc %lld first_wall %lld\n", (long long)g_audio.first_qpc(),
                        (long long)R.t0_qpc, (long long)g_audio.first_wall_.load());
                fwrite(R.trace_rt.data(), 1, R.trace_rt.size(), f);
                fclose(f);
            }
    }
    R.on = false;
    g_want_rs_state = 2;
    finish_final_cut(r);
    swofty::log("[cinecam] render %s after %d frame(s): %s", ok ? "done" : "stopped", R.frame, why.c_str());
    bool have_video = R.phase == 3 || (ok && R.phase == 2);  // a cancel during the sound pass keeps the video
    if (have_video) {
        toast("Render: " + std::to_string(R.frame) + " frames, finishing the video...");
        render_finish_files(R.base, R.ffmpeg, R.frame, sound_ok, offset + g_cfg.render_audio_offset, R.fps, R.trace,
                            R.format);
    } else {
        toast("Render stopped: " + why);
        std::thread([] { g_rec.stop(); }).detach();
    }
}

// Where renders go: cfg render_dir, else the user's Videos folder, wherever Windows has it (it can be moved to
// another drive) \ Cinematic Replay. Camera thread (reads g_cfg); cached per render_dir.
std::wstring render_output_dir()
{
    static std::string cached_for = "\x01";
    static std::wstring cached;
    if (g_cfg.render_dir == cached_for)
        return cached;
    std::wstring dir = widen(g_cfg.render_dir);
    if (dir.empty()) {
        PWSTR videos = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos)) && videos) {
            dir = std::wstring(videos) + L"\\Cinematic Replay";
        } else {
            wchar_t up[MAX_PATH] = {};
            GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
            dir = std::wstring(up) + L"\\Videos\\Cinematic Replay";
        }
        CoTaskMemFree(videos);
    }
    cached_for = g_cfg.render_dir;
    cached = dir;
    return dir;
}

// Renders replay time [from, to] (to <= from: the whole edit).
void render_start(const Replay &r, float from, float to)
{
    auto toast = [](const std::string &m) {  // also logged: a failed start is easy to miss
        ::toast(m);
        swofty::log("[cinecam] render: %s", m.c_str());
    };
    if (g_render.on || g_rec.active()) {
        toast("A render is still finishing");
        return;
    }
    float s, e;
    {
        std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
        if (g_edit.shots.empty()) {
            toast("Nothing to render: add a shot first");
            return;
        }
        s = g_edit.start();
        e = g_edit.end();
    }
    if (to > from + 0.05f) {
        s = from;
        e = to;
    }
    HWND hwnd = game_hwnd();
    std::wstring ff = render::find_ffmpeg(widen(g_cfg.ffmpeg));
    if (ff.empty()) {
        toast("Render needs ffmpeg: Install in the Render menu, or set ffmpeg = <path> in cinecam.cfg");
        return;
    }
    if (!hwnd) {
        toast("Render: the game window was not found");
        return;
    }
    std::wstring dir = render_output_dir();
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    wchar_t name[96];
    swprintf(name, 96, L"%04d%02d%02d-%02d%02d%02d", tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond);
    int format = g_cfg.render_format;
    std::wstring base = dir + L"\\" + widen(seq_name()) + L"_" + name, path = base + L".video" + render_ext(format);
    std::string err;
    if (!g_rec.start(hwnd, g_cfg.render_fps, g_cfg.render_crf, ff, path, err, g_cfg.render_blur,
                     g_cfg.render_screen_capture, format)) {
        toast("Render: " + err);
        return;
    }
    if (!g_rec.window_capture() && GetForegroundWindow() != hwnd) {  // the screen capture would film what's on top
        std::thread([] { g_rec.stop(0); }).detach();
        toast("Render: keep the game in front (window capture unavailable, it captures the screen)");
        return;
    }
    g_render = {};
    g_render.on = true;
    g_render.settle = g_cfg.render_settle;
    g_render.fps = float(g_cfg.render_fps);
    g_render.t0 = s;
    g_render.t1 = e;
    g_render.time = s;
    g_render.total = int((e - s) * g_render.fps) + 1;
    g_render.hwnd = hwnd;
    g_render.base = base;
    g_render.format = format;
    g_render.ffmpeg = ff;
    g_render.audio = g_cfg.render_audio;
    g_render.blur = std::max(1, g_cfg.render_blur);
    g_render.shutter = std::clamp(g_cfg.render_shutter, 10.0f, 360.0f) / 360.0f;
    // the clean start of Final Cut
    g_final_return = r.t;
    g_editor = false;
    g_fly = false;
    g_preview = true;
    g_preview_inside = false;
    g_final = true;
    g_frozen = false;
    g_play_speed = 1;
    hud_request(true);
    seek(r, std::max(0.0f, s - std::max(1.0f, g_cfg.preroll)));
    swofty::log("[cinecam] render: %dx%d at %d fps, settle %d, blur %d, %s capture, %s", g_rec.width(), g_rec.height(),
                g_cfg.render_fps, g_cfg.render_settle, g_cfg.render_blur, g_rec.window_capture() ? "window" : "screen",
                narrow(path).c_str());
}

// A sync trace row (render_keep_parts): `head` (times), our eye, every car's centre (0s for none), CSV.
void trace_row(std::string &out, const char *head, const cine::Cars &cars)
{
    char b[96];
    out += head;
    snprintf(b, sizeof b, ",%.3f,%.3f,%.3f", g_shown.eye.x, g_shown.eye.y, g_shown.eye.z);
    out += b;
    for (int i = 0; i < std::min(slot_count(), 32); i++) {
        auto f = cars.frame(i);
        V3 c = f ? f->at(cine::kCarCentre) : V3{0, 0, 0};
        snprintf(b, sizeof b, ",%.3f,%.3f,%.3f", c.x, c.y, c.z);
        out += b;
    }
    out += '\n';
}

// One game frame of the render (camera thread, after the camera is written). `speed` = the shot's replay speed.
void render_tick(const Replay &r, float speed, const cine::Cars &cars)
{
    RenderJob &R = g_render;
    if (!R.on)
        return;
    if (R.phase == 3) {  // the sound pass: real time, until the edit's end
        if (r.state == 5) {
            g_want_rs_state = 2;
            return;
        }
        if (!R.audio_started) {  // holding at the run-up (apply keeps the speed at 0) until it has loaded
            if (GetTickCount64() < R.hold_until)
                return;
            std::string err;
            if (!g_audio.start(R.base + L".wav", err)) {
                swofty::log("[cinecam] render: no sound pass: %s", err.c_str());
                toast("Render: no sound (" + err + ")");
                R.audio = false;
                render_end(r, false, "no sound capture");
                return;
            }
            R.audio_started = true;
            return;
        }
        if (R.t0_qpc == 0 && r.t >= R.t0 && r.t < R.t0 + 0.5f) {  // the moment the edit starts, on the audio clock
            R.t0_qpc = audiocap::Recorder::now_qpc100() -
                       int64_t(double(r.t - R.t0) / std::max(0.05, double(speed)) * 1e7);
            R.expect = 0;
            R.last_t = r.t;
        } else if (R.t0_qpc != 0 && R.last_t >= 0 && r.t > R.last_t) {  // how far the replay falls behind the clock
            R.expect += double(r.t - R.last_t) / std::max(0.05, double(speed));
            R.last_t = r.t;
            double wall = double(audiocap::Recorder::now_qpc100() - R.t0_qpc) / 1e7;
            R.drift = std::max(R.drift, float(wall - R.expect));
        }
        if (g_cfg.render_keep_parts != 0) {  // what the game shows in real time against its clock, for diagnosis
            char b[64];
            snprintf(b, sizeof b, "%lld,%.4f", (long long)audiocap::Recorder::now_qpc100(), r.t);
            trace_row(R.trace_rt, b, cars);
        }
        if (r.t >= R.t1 + 0.3f + std::max(0.0f, g_cfg.render_audio_offset))  // the last frames' sound too
            render_end(r, true, "the end of the sound pass");
        return;
    }
    if (g_rec.failed()) {
        render_end(r, false, g_rec.error());
        return;
    }
    if (R.phase == 0) {  // real time until the HUD presses are done and the run-up is nearly over
        if (!hud_busy() && r.t >= R.t0 - 0.3f) {
            g_want_rs_state = 5;
            R.phase = 1;
        }
        return;
    }
    if (R.phase == 1) {  // the stepped state, then the first frame's time
        if (r.state != 5) {
            g_want_rs_state = 5;
            return;
        }
        seek(r, R.t0);
        *reinterpret_cast<volatile float *>(r.rs + 0x28) = 0;  // no step left over from before
        R.time = R.frame_time = R.t0;
        R.first_rt = -1;
        R.reseeks = 0;
        R.sub = R.cap = 0;
        R.phase = 2;
        R.hold = 0;
        R.waiting = false;
        return;
    }
    if (r.state != 5) {  // something put the replay back to normal play: keep it stepped
        g_want_rs_state = 5;
        return;
    }
    if (!R.waiting) {
        R.hold++;
        if (R.hold >= R.settle && (g_rec.window_capture() || GetForegroundWindow() == R.hwnd)) {
            // The first frame: the stepped seek has landed off the edit's start before (0.5 s late, live), so
            // check the clock and seek again until it is there.
            if (R.cap == 0 && std::fabs(r.t - R.frame_time) > 0.004f && R.reseeks < 8) {
                swofty::log("[cinecam] render: the replay is at %.4f s, the first frame is %.4f s: seeking again",
                            r.t, R.frame_time);
                seek(r, R.frame_time);
                *reinterpret_cast<volatile float *>(r.rs + 0x28) = 0;
                R.reseeks++;
                R.hold = 0;
                return;
            }
            if (R.cap == 0)
                R.first_rt = r.t;
            g_rec.request(R.cap);
            if (R.sub == 0 && g_cfg.render_keep_parts != 0) {  // the frame's first capture: its moment
                char b[32];
                snprintf(b, sizeof b, "%.4f", r.t);
                trace_row(R.trace, b, cars);
            }
            R.waiting = true;
        }
    } else if (g_rec.done() >= R.cap) {
        R.waiting = false;
        R.hold = 0;
        R.cap++;
        // Motion blur: `blur` captures per output frame, spread over the shutter (a fraction of the frame time),
        // then the rest of the frame time to the next frame's first capture.
        float dt = std::clamp(speed, 0.01f, 4.0f) / R.fps, sub = dt * R.shutter / float(R.blur), step;
        if (R.sub + 1 < R.blur) {
            R.sub++;
            step = sub;
        } else {
            step = dt - sub * float(R.blur - 1);
            R.sub = 0;
            R.frame++;
            R.frame_time += dt;
            if (R.frame_time > R.t1) {
                render_end(r, true, "the end of the edit");
                return;
            }
            if (R.frame % 60 == 0)
                swofty::log("[cinecam] render frame %d / ~%d (replay %.2f s)", R.frame, R.total, R.frame_time);
        }
        R.time += step;
        *reinterpret_cast<volatile float *>(r.rs + 0x28) = step;
    }
}

void run_commands(const Replay &r, GameCars &cars, uintptr_t entry)
{
    std::vector<Cmd> cmds;
    {
        std::lock_guard<std::mutex> lk(g_cmd_mx);
        cmds.swap(g_cmds);
    }
    for (const Cmd &c : cmds) {
        switch (c.kind) {
        case CmdKind::ToggleFly:
            g_fly = !g_fly.load();
            g_fc_init = false;
            break;
        case CmdKind::Seek:
            seek(r, std::max(0.0f, c.t));
            break;
        case CmdKind::Freeze:
            g_frozen = !g_frozen;
            break;
        case CmdKind::PlaySpeed:
            g_play_speed = c.t;
            g_frozen = false;
            break;
        case CmdKind::PlayFromStart: {
            float s;
            {
                std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
                s = g_edit.start();
            }
            g_editor = false;
            g_fly = false;
            g_preview = true;
            g_preview_inside = false;
            g_frozen = false;
            seek(r, s);
            toast("Playing the edit (F9 opens the editor again)");
            break;
        }
        case CmdKind::Render:
            render_start(r, c.t, c.t2);
            break;
        case CmdKind::FinalCut: {  // Play all, clean: every HUD hidden, a run-up in black, fades, bars
            float s;
            {
                std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
                if (g_edit.shots.empty()) {
                    toast("Nothing to play: add a shot first");
                    break;
                }
                s = g_edit.start();
            }
            g_final_return = r.t;
            g_editor = false;
            g_fly = false;
            g_preview = true;
            g_preview_inside = false;
            g_final = true;
            g_frozen = false;
            g_play_speed = 1;
            hud_request(true);  // the game's own Y: everything hidden (during the run-up)
            seek(r, std::max(0.0f, s - g_cfg.preroll));
            break;
        }
        case CmdKind::SwitchEdit:
            switch_edit(c.name, c.type);
            break;
        case CmdKind::StopPreview:
            g_preview = false;
            break;
        case CmdKind::AddKey:
            add_key(r, cars, entry);
            break;
        case CmdKind::AddShot: {
            V3 eye;
            cine::Quat q;
            float fov;
            editor_view(entry, cars, eye, q, fov);
            std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
            cine::Edit &ed = g_edit;
            if (cine::ShotType(c.type) == cine::ShotType::transition) {
                // fills the gap the playhead is in: from the shot before's end to the shot after's start
                int before = -1, after = -1;
                for (int i = 0; i < int(ed.shots.size()); i++) {
                    const cine::Shot &o = ed.shots[size_t(i)];
                    if (o.t1 <= r.t + 1e-3f && (before < 0 || o.t1 > ed.shots[size_t(before)].t1))
                        before = i;
                    if (o.t0 >= r.t - 1e-3f && (after < 0 || o.t0 < ed.shots[size_t(after)].t0))
                        after = i;
                }
                if (ed.shot_at(r.t) >= 0 || before < 0 || after < 0 ||
                    ed.shots[size_t(after)].t0 - ed.shots[size_t(before)].t1 < 0.25f) {
                    toast("A transition fills a gap: trim a shot to leave room, then put the playhead in the gap");
                    break;
                }
                cine::Shot s;
                s.type = cine::ShotType::transition;
                s.t0 = ed.shots[size_t(before)].t1;
                s.t1 = ed.shots[size_t(after)].t0;
                s.car = -1;
                s.ease_in = s.ease_out = true;  // smooth by default
                s.height = 0;                   // no arc
                ed.shots.push_back(s);
                ed.sort();
                for (int i = 0; i < int(ed.shots.size()); i++)
                    if (ed.shots[size_t(i)].t0 == s.t0 && ed.shots[size_t(i)].type == cine::ShotType::transition)
                        g_select_shot = i;
                g_fly = false;  // watch it
                mark_dirty();
                toast("Transition " + mmss(s.t0) + " - " + mmss(s.t1));
                break;
            }
            cine::Shot s;
            s.type = cine::ShotType(c.type);
            s.t0 = r.t;
            s.t1 = r.t + 6;
            s.car = g_followed_slot >= 0 ? g_followed_slot : 0;
            s.driver = car_name(s.car);
            if (s.type == cine::ShotType::onboard)
                s.fov = cine::find_mount(s.mount)->fov;
            if (s.type == cine::ShotType::chase)
                s.distance = cine::kChases[0].distance, s.height = cine::kChases[0].height, s.fov = cine::kChases[0].fov;
            if (s.type == cine::ShotType::track) {
                s.point = eye;
                s.fov = std::clamp(fov, 10.0f, 70.0f);
            }
            if (s.type == cine::ShotType::orbit)
                s.height = 2, s.fov = 60;
            int under = ed.shot_at(r.t);  // a new shot starts here: the one under the playhead ends here
            if (under >= 0)
                ed.shots[size_t(under)].t1 = r.t;
            std::erase_if(ed.shots, [](const cine::Shot &o) { return o.t1 - o.t0 < 0.25f; });
            ed.shots.push_back(s);
            ed.sort();
            int at = -1;
            for (int i = 0; i < int(ed.shots.size()); i++)
                if (ed.shots[size_t(i)].t0 == s.t0)
                    at = i;
            fit_shot(ed, at);
            if (s.type == cine::ShotType::free_path) {
                auto k = cine::key_from_view(ed.shots[size_t(at)], r.t, eye, q, fov, cars);
                if (k)
                    ed.shots[size_t(at)].keys.push_back(*k);
            }
            g_select_shot = at;
            g_fly = s.type == cine::ShotType::free_path || s.type == cine::ShotType::track ? g_fly.load() : false;
            mark_dirty();
            toast(std::string(cine::shot_type_name(s.type)) + " shot at " + mmss(r.t));
            break;
        }
        case CmdKind::SetKeyFromView:
        case CmdKind::PlaceTrack: {
            V3 eye;
            cine::Quat q;
            float fov;
            editor_view(entry, cars, eye, q, fov);
            std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
            if (c.shot < 0 || c.shot >= int(g_edit.shots.size()))
                break;
            cine::Shot &s = g_edit.shots[size_t(c.shot)];
            if (c.kind == CmdKind::PlaceTrack) {
                s.point = eye;
                toast("Track cam placed");
            } else if (c.key >= 0 && c.key < int(s.keys.size())) {
                float kt = s.keys[size_t(c.key)].t;
                if (auto k = cine::key_from_view(s, kt, eye, q, fov, cars)) {
                    k->selected = true;
                    s.keys[size_t(c.key)] = *k;
                    toast("Key set from the view");
                }
            }
            mark_dirty();
            break;
        }
        }
    }
}

// One frame of the free camera. Returns true when it wrote the camera.
// Orbit-the-car fly mode (O / pad B): the camera looks at a car's centre and moves around it in the car's level
// frame (heading only), so it follows the car without the horizon shaking. The car is the one of the shot under
// the playhead, else the one the replay follows.
struct OrbitFly {
    bool on;
    int car;
    float az, el, dist, roll;  // degrees around (0: behind), degrees up, metres, degrees
} g_orb;

bool freecam_frame(uintptr_t entry, const Replay &r, GameCars &cars, float dt, bool first, const KbState &kb,
                   const PadState &pad, uint16_t b, uint16_t hit)
{
    constexpr uint16_t UP = 0x1, DOWN = 0x2, BACK = 0x20, L3 = 0x40, R3 = 0x80, LB = 0x100, RB = 0x200, X = 0x4000;
    cine::Frame world;
    auto ride_frame = [&]() -> std::optional<cine::Frame> { return cars.frame(g_fc.ride_car); };
    if (first) {
        V3 eye;
        cine::Quat q;
        float fov;
        shown_view(entry, eye, q, fov);  // fly on from exactly what is on screen, lens included
        float fly = g_fc.speed;
        g_fc = cine::FreeCam{};
        g_fc.speed = fly;
        g_fc.set_from_world(world, eye, q, std::clamp(fov, 5.0f, 120.0f));
    }
    std::optional<cine::Frame> rf = g_fc.ride ? ride_frame() : std::nullopt;
    if (g_fc.ride && !rf) {
        g_fc.ride = false;
        toast("Car lost: back to a world camera");
    }
    const cine::Frame &fr = g_fc.ride ? *rf : world;
    (void)r;

    if (first)
        g_orb.on = false;
    if ((hit & 0x2000) || kb.orbit) {  // pad B
        if (g_orb.on) {
            g_orb.on = false;
            toast("Free flying");
        } else {
            int car = g_followed_slot;
            {
                std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
                int at = g_edit.shot_at(r.t);
                if (at >= 0 && g_edit.shots[size_t(at)].uses_car())
                    car = g_edit.shots[size_t(at)].car;
            }
            auto cf = cars.frame(car);
            if (!cf) {
                toast("No car to orbit");
            } else {
                cine::Frame lf = cine::level_frame(*cf);
                cine::Frame cur = world;
                if (g_fc.ride)
                    if (auto rf0 = cars.frame(g_fc.ride_car))
                        cur = *rf0;
                V3 d = g_fc.eye(cur) - cf->at(cine::kCarCentre);
                float x = cine::dot(d, lf.r), y = cine::dot(d, lf.u), z = cine::dot(d, lf.f);
                g_orb = {true, car, std::atan2(x, -z) / cine::kDeg,
                         std::atan2(y, std::sqrt(x * x + z * z)) / cine::kDeg, std::clamp(cine::len(d), 1.5f, 400.0f), 0};
                g_fc.ride = false;
                toast("Orbiting " + car_label(car) + " (O / B: free fly)");
            }
        }
    }
    if ((hit & X) || kb.ride) {
        if (g_fc.ride) {
            g_fc.rebase(fr, world);
            g_fc.ride = false;
            toast("World camera");
        } else {
            int slot = followed_slot();
            g_fc.ride_car = slot >= 0 ? slot : -1;
            if (auto cf = ride_frame()) {
                g_fc.rebase(world, *cf);
                g_fc.ride = true;
                toast("Riding " + car_label(g_fc.ride_car));
            }
        }
    }
    if ((hit & R3) && !(b & L3)) {
        g_fc.next_preset();
        char m[48];
        snprintf(m, sizeof m, "Fly speed %.0f m/s", g_fc.speed);
        toast(m);
    }
    if (kb.wheel != 0)
        g_fc.scale_speed(std::pow(1.25f, kb.wheel));
    if ((hit & BACK) || kb.level)
        g_fc.level(g_fc.ride && rf ? *rf : world);
    if (kb.align) {  // look exactly along the car's heading, level in its frame
        auto to = g_fc.ride ? rf : cars.frame(-1);
        if (to) {
            g_fc.align(g_fc.ride ? *rf : world, *to);
            toast("Aligned to " + car_label(g_fc.ride ? g_fc.ride_car : g_followed_slot));
        }
    }
    bool both_bumpers = (b & (LB | RB)) == (LB | RB) && (hit & (LB | RB));
    if (kb.flat || kb.level_surface || both_bumpers) {
        const cine::Frame &cur = g_fc.ride && rf ? *rf : world;
        cine::Frame nf;
        int near_slot = nearest_car(cars, g_fc.eye(cur), nf);
        if (near_slot >= -1) {
            if (kb.level_surface)
                g_fc.level_to(cur, nf.u);
            else
                g_fc.flat_on(cur, nf.u);
            toast(std::string(kb.level_surface ? "Rolled to" : "Flat on") + " the track at " + car_label(near_slot));
        } else {
            toast("No car within 300 m to read the track surface from");
        }
    }

    std::optional<cine::Frame> rf2 = g_fc.ride ? ride_frame() : std::nullopt;
    if (g_fc.ride && !rf2)
        g_fc.ride = false;
    const cine::Frame &fr2 = g_fc.ride ? *rf2 : world;
    cine::FreeInput in;
    in.move_fwd = axis(pad.ly) + (kb.w ? 1.f : 0.f) - (kb.s ? 1.f : 0.f);
    in.move_right = axis(pad.lx) + (kb.d ? 1.f : 0.f) - (kb.a ? 1.f : 0.f);
    in.move_up = pad.rt / 255.f - pad.lt / 255.f + (kb.e ? 1.f : 0.f) - (kb.q ? 1.f : 0.f);
    float fine = (kb.ctrl || ((b & L3) && !(b & R3))) ? 0.2f : 1.0f;
    float zoom = g_fc.fov / 60;
    float look = 70 * dt * zoom * fine;
    float mouse = 0.06f * zoom * fine;
    float srx = axis(pad.rx), sry = axis(pad.ry);
    in.yaw = srx * std::fabs(srx) * look + kb.mdx * mouse + ((kb.right ? 1.f : 0.f) - (kb.left ? 1.f : 0.f)) * look;
    in.pitch = sry * std::fabs(sry) * look - kb.mdy * mouse + ((kb.up ? 1.f : 0.f) - (kb.down ? 1.f : 0.f)) * look;
    bool lb_roll = (b & LB) && !(b & (0x4 | 0x8));  // LB + d-pad left/right jumps keys instead
    in.roll_rate = ((b & RB) && !(b & LB) ? 1.f : 0.f) - (lb_roll && !(b & RB) ? 1.f : 0.f) + (kb.c ? 1.f : 0.f) -
                   (kb.z ? 1.f : 0.f);
    in.fov_rate = ((b & DOWN) ? 1.f : 0.f) - ((b & UP) ? 1.f : 0.f) + (kb.f ? 1.f : 0.f) - (kb.r ? 1.f : 0.f);
    in.speed_mul = kb.shift ? 4.f : 1.f;
    in.fine = fine;
    if (g_orb.on) {
        auto cf = cars.frame(g_orb.car);
        if (!cf) {
            g_orb.on = false;
            toast("Car lost: free flying");
        } else {
            // look: around / up; W/S: closer / further; A/D: around; Q/E: down / up; roll and zoom as usual
            float sp = in.speed_mul * fine;
            g_orb.az = std::remainder(g_orb.az - in.yaw - in.move_right * 60 * dt * sp, 360.0f);
            g_orb.el = std::clamp(g_orb.el - in.pitch + in.move_up * 40 * dt * sp, -80.0f, 85.0f);
            g_orb.dist = std::clamp(g_orb.dist * std::exp(-in.move_fwd * 1.2f * dt * sp), 1.5f, 400.0f);
            g_orb.roll += in.roll_rate * 45 * dt * fine;
            g_fc.fov = std::clamp(g_fc.fov + in.fov_rate * 25 * fine * dt * (g_fc.fov / 60), 5.0f, 140.0f);
            cine::Frame lf = cine::level_frame(*cf);
            V3 centre = cf->at(cine::kCarCentre);
            float a = g_orb.az * cine::kDeg, e = g_orb.el * cine::kDeg;
            V3 eye = centre + lf.dir({std::sin(a) * std::cos(e) * g_orb.dist, std::sin(e) * g_orb.dist,
                                      -std::cos(a) * std::cos(e) * g_orb.dist});
            cine::Quat q = cine::look_at_quat(eye, centre, g_orb.roll);
            g_fc.ride = false;
            g_fc.set_from_world(world, eye, q, g_fc.fov);  // so K, Set from view and leaving orbit see this view
            write_view(entry, eye, q, g_fc.fov, first);
            return true;
        }
    }
    g_fc.update(in, dt, fr2);
    write_view(entry, g_fc.eye(fr2), g_fc.rotation(fr2), g_fc.fov, first);
    return true;
}

// Recorded trackside pass-by sounds: off while in a replay (unless recorded_passby = 1), the game's own value
// back outside. The system is found from the last per-car audio object the stub saw, checked by its back link.
uintptr_t g_passby;        // the pass-by system we changed (0: none)
uint8_t g_passby_orig = 1; // its flag before we changed it

void passby_tick(bool in_replay)
{
    uintptr_t self = reinterpret_cast<uintptr_t>(g_audio_this), a = 0, mgr = 0, sys = 0, back = 0, check = 0;
    uint8_t flag = 0;
    bool ok = self && rd(self + 0xdf0, a) && rd(a + 0xa0, mgr) && rd(mgr + 0xc10, sys) && rd(sys + 0x48, back) &&
              rd(back + 0xc10, check) && check == sys && rd(sys + 1, flag);
    if (!ok)
        return;
    bool want_off = in_replay && !g_cfg.recorded_passby;
    if (want_off && flag != 0) {
        if (g_passby != sys)
            g_passby_orig = flag;
        g_passby = sys;
        *reinterpret_cast<volatile uint8_t *>(sys + 1) = 0;
        swofty::log("[cinecam] recorded pass-by sounds off for the replay (system %p)", reinterpret_cast<void *>(sys));
    } else if (!want_off && g_passby == sys && flag != g_passby_orig) {
        *reinterpret_cast<volatile uint8_t *>(sys + 1) = g_passby_orig;
        g_passby = 0;
        swofty::log("[cinecam] recorded pass-by sounds restored");
    }
}

// The audio listener follows the view we put on screen. apply() publishes that view each frame we drive the
// camera (game thread); the listener update wrapper (whatever thread runs Camera::Update) swaps it into the camera's
// working state for the game's own listener code, then puts the game's values back. One frame behind the picture.
std::mutex g_audio_view_mx;
Shown g_audio_view;           // guarded by g_audio_view_mx
bool g_audio_view_on;         // guarded by g_audio_view_mx: we drive the camera and the listener should follow
using ListenerUpdate_t = uint64_t (*)(uintptr_t listener, uintptr_t ctx);
ListenerUpdate_t g_listener_orig;
std::atomic<bool> g_listener_logged{false};

// The replay's own camera rides the shot's car (so its sound does). The camera thread asks; the director's thread
// (director_tick, from the stub) switches.
using SwitchCar_t = void (*)(uintptr_t helper, uint8_t car, uint8_t mode, uintptr_t cam);
std::atomic<uintptr_t> g_director;
std::atomic<int> g_want_game_car{-1};

// Mutes / unmutes this process's audio sessions on every active output device (what the Windows volume mixer
// does per app), on a short-lived worker thread so COM never runs on a game thread. Unmutes only what it muted.
std::atomic<int> g_mute_state{0};  // 0 not muted by us, 1 muted by us

void mute_sessions(bool mute)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return;
    IMMDeviceEnumerator *en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void **>(&en)))) {
        IMMDeviceCollection *devs = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs))) {
            UINT nd = 0;
            devs->GetCount(&nd);
            for (UINT d = 0; d < nd; d++) {
                IMMDevice *dev = nullptr;
                IAudioSessionManager2 *mgr = nullptr;
                IAudioSessionEnumerator *se = nullptr;
                if (SUCCEEDED(devs->Item(d, &dev)) &&
                    SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                            reinterpret_cast<void **>(&mgr))) &&
                    SUCCEEDED(mgr->GetSessionEnumerator(&se))) {
                    int ns = 0;
                    se->GetCount(&ns);
                    for (int i = 0; i < ns; i++) {
                        IAudioSessionControl *c = nullptr;
                        IAudioSessionControl2 *c2 = nullptr;
                        ISimpleAudioVolume *vol = nullptr;
                        DWORD pid = 0;
                        if (SUCCEEDED(se->GetSession(i, &c)) &&
                            SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void **>(&c2))) &&
                            SUCCEEDED(c2->GetProcessId(&pid)) && pid == GetCurrentProcessId() &&
                            SUCCEEDED(c->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&vol))))
                            vol->SetMute(mute, nullptr);
                        if (vol)
                            vol->Release();
                        if (c2)
                            c2->Release();
                        if (c)
                            c->Release();
                    }
                }
                if (se)
                    se->Release();
                if (mgr)
                    mgr->Release();
                if (dev)
                    dev->Release();
            }
            devs->Release();
        }
        en->Release();
    }
    CoUninitialize();
}

// Unmutes the game's sessions whatever our state says: Windows remembers a per-app mute between launches (a game
// closed while frozen starts muted), and at plugin load the game has no audio session yet to unmute.
void force_unmute()
{
    g_mute_state = 0;
    std::thread([] { mute_sessions(false); }).detach();
}

void set_game_muted(bool mute)
{
    int want = mute ? 1 : 0;
    if (g_mute_state.exchange(want) == want)
        return;
    std::thread([mute] { mute_sessions(mute); }).detach();
}

void director_tick_impl(void *d)
{
    if (g_director.exchange(reinterpret_cast<uintptr_t>(d)) != reinterpret_cast<uintptr_t>(d))
        swofty::log("[cinecam] replay director at 0x%llx", (unsigned long long)reinterpret_cast<uintptr_t>(d));
    hud_tick(reinterpret_cast<uintptr_t>(d));
    if (int st = g_want_rs_state.exchange(-1); st == 2 || st == 5) {  // the render's stepped state and back
        uintptr_t rs = 0;
        uint32_t cur = 0;
        if (rd(g_base + kReplaySystemRva, rs) && rs && rd(rs + 0x20, cur) && cur != uint32_t(st) && cur >= 2 && cur <= 5)
            reinterpret_cast<SetState_t>(g_base + kRsSetStateLockedRva)(rs, uint32_t(st), 1);
    }
    int want = g_want_game_car.exchange(-1);
    if (want < 0 || want > 0xfe)
        return;
    uintptr_t helper = reinterpret_cast<uintptr_t>(d) + 0x70;
    uint8_t focus = 0xff, mode = 0;
    if (!rd(helper + 0x14, focus) || !rd(helper + 0x15, mode) || (focus == want && mode == 1))
        return;
    reinterpret_cast<SwitchCar_t>(g_base + kSwitchCarRva)(helper, uint8_t(want), 1, 0);
    swofty::log("[cinecam] the replay's camera now rides car %d (was car %d, mode %d), for its sound", want, focus, mode);
}

// Called by the camera thread while we drive the camera: the replay's camera rides `car` (car-camera mode).
void request_game_car(int car)
{
    uintptr_t d = g_director;
    if (!d || car < 0)
        return;
    uint8_t focus = 0xff, mode = 0;
    if (rd(d + 0x70 + 0x14, focus) && rd(d + 0x70 + 0x15, mode) && (focus != car || mode != 1))
        g_want_game_car = car;
}

void publish_audio_view(bool on)
{
    std::lock_guard<std::mutex> lk(g_audio_view_mx);
    g_audio_view_on = on;
    if (on)
        g_audio_view = g_shown;
}

uint64_t listener_update(uintptr_t self, uintptr_t ctx)
{
    Shown v;
    bool on;
    {
        std::lock_guard<std::mutex> lk(g_audio_view_mx);
        on = g_audio_view_on;
        v = g_audio_view;
    }
    uintptr_t ws = 0;
    if (!on || !ctx || !rd(ctx + 0x10, ws) || !ws)  // ctx = {dt, time, &working state, flag}
        return g_listener_orig(self, ctx);
    float saved[4], quat_saved[4];  // the game's eye (vec4 at +0x120) and quat (+0x140), put back after the call
    memcpy(saved, reinterpret_cast<void *>(ws + 0x120), 16);
    memcpy(quat_saved, reinterpret_cast<void *>(ws + 0x140), 16);
    float eye[4] = {v.eye.x, v.eye.y, v.eye.z, saved[3]};
    float q[4] = {v.q.x, v.q.y, v.q.z, v.q.w};
    memcpy(reinterpret_cast<void *>(ws + 0x120), eye, 16);
    memcpy(reinterpret_cast<void *>(ws + 0x140), q, 16);
    uint64_t r = g_listener_orig(self, ctx);
    memcpy(reinterpret_cast<void *>(ws + 0x120), saved, 16);
    memcpy(reinterpret_cast<void *>(ws + 0x140), quat_saved, 16);
    if (!g_listener_logged.exchange(true))
        swofty::log("[cinecam] the audio listener follows our camera (listener %p)", reinterpret_cast<void *>(self));
    return r;
}

void apply(void *vpm)
{
    if ((g_frames % 30) == 0) {
        load_config();
        std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
        if (!g_dirty)
            load_sequence();
    }
    const Config &c = g_cfg;
    Replay r = replay();
    if ((g_frames % 30) == 15 && !g_render.on)
        replay_check(r);  // the edit for this replay
    if (g_ray_tramp && (g_frames % 120) == 0) {  // once: the game's collision world was found (the clipping check)
        static void *logged = nullptr;
        void *w = g_ray_world.load();
        if (w && w != logged) {
            swofty::log("[cinecam] raycast: collision world %p (game layers seen 0x%llx)", w,
                        (unsigned long long)g_ray_layers.load());
            logged = w;
        }
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_last_qpc.QuadPart ? float(now.QuadPart - g_last_qpc.QuadPart) / float(g_qpf.QuadPart) : 0.016f;
    dt = std::clamp(dt, 0.0f, 0.1f);
    g_last_qpc = now;

    // input: keyboard / mouse (from the overlay) and the pad (from our XInput hook)
    KbState kb;
    {
        std::lock_guard<std::mutex> lk(g_kb_mx);
        kb = g_kb;
        g_kb.mdx = g_kb.mdy = 0;
        g_kb.step = g_kb.wheel = 0;
        g_kb.speed_dir = 0;
        g_kb.toggle = g_kb.add = g_kb.freeze = g_kb.level = g_kb.ride = g_kb.align = g_kb.flat = g_kb.level_surface =
            g_kb.fly = g_kb.orbit = 0;
    }
    PadState pad{};
    {
        std::lock_guard<std::mutex> lk(g_pad_mx);
        for (auto &pp : g_pad)
            if (pp.ok) {
                pad = pp;
                break;
            }
    }
    uint16_t b = pad.ok ? pad.buttons : 0, hit = b & ~g_prev_buttons;
    g_prev_buttons = b;
    constexpr uint16_t LEFT = 0x4, RIGHT = 0x8, START = 0x10, A = 0x1000, Y = 0x8000;
    if (pad.ok && (b || std::abs(pad.lx) > 8000 || std::abs(pad.ly) > 8000 || std::abs(pad.rx) > 8000 ||
                   std::abs(pad.ry) > 8000 || pad.lt > 40 || pad.rt > 40))
        g_device = 1;

    int toggles = kb.toggle;
    {
        static bool both_was;
        bool both = (b & 0xc0) == 0xc0;
        if (both && !both_was)
            toggles++;
        both_was = both;
    }
    if (g_cfg.freecam != g_last_freecam) {
        toggles++;
        g_last_freecam = g_cfg.freecam;
    }

    bool in_replay = r.ok && r.mode != 3;
    {  // entering a replay: clear a mute Windows kept from an earlier session (the game has its audio by now)
        static bool was_in_replay;
        if (in_replay && !was_in_replay && !g_frozen)
            force_unmute();
        was_in_replay = in_replay;
    }
    if ((g_frames % 30) == 0)
        passby_tick(in_replay);
    if (!in_replay) {
        publish_audio_view(false);
        if (g_render.on)
            render_end(r, false, "the replay closed");
        g_final = false;
        g_overriding = false;
        g_last_shot = -1;
        g_editor = false;
        g_preview = false;
        g_block_pad = false;
        std::lock_guard<std::mutex> lk(g_status_mx);
        g_status.in_replay = false;
        return;
    }
    if ((toggles & 1) && g_render.on) {  // F9 cancels a render (and opens the editor where it was)
        render_end(r, false, "cancelled");
        toggles = 0;
    }
    if (toggles & 1) {
        bool on = !g_editor.load();
        g_editor = on;
        g_preview = false;
        g_final = false;  // F9 stops Final Cut: the editor at this moment
        g_fc_init = false;
        // the game's HUD: hidden while the editor is open (editor_hides_hud), back as it was when it closes
        hud_request(on && g_cfg.editor_hides_hud);
        if (on) {
            g_prev_buttons = 0xffff;  // ignore the buttons that toggled it
            std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
            load_sequence();
            if (g_seq_loaded != seq_name()) {
                g_edit = {};
                g_edit.name = seq_name();
                g_seq_loaded = seq_name();
            }
            g_hist_reset = true;
            g_fly = g_edit.shots.empty();  // nothing to watch yet: fly
            toast(g_fly.load() ? "Editor open: flying (F to watch the edit)" : "Editor open: watching (F to fly)");
        } else {
            g_frozen = false;
            g_fly = false;
            toast("Editor closed");
        }
    }
    bool editor = g_editor.load();
    if (editor && (kb.fly || (hit & START))) {
        g_fly = !g_fly.load();
        g_fc_init = false;  // fly on from the view on screen
        toast(g_fly.load() ? "Flying" : "Watching the edit");
    }
    bool fly = editor && g_fly.load(), preview = g_preview.load();
    g_block_pad = editor;

    uintptr_t list = 0, entry = 0;
    if (!rd(reinterpret_cast<uintptr_t>(vpm) + 0x40, list) || !rd(list + 8, entry) || !entry)
        return;
    GameCars cars;
    g_tracks.record(r.t, r.end, cars, slot_count());  // true paths: where every car is at this replay time

    // config one-shots
    if (c.seek >= 0 && c.seek != g_last_seek)
        seek(r, c.seek);
    g_last_seek = c.seek;
    static float last_select = -1;
    if (c.select_shot >= 0 && c.select_shot != last_select && !g_cfg_first)
        g_select_shot = int(c.select_shot);
    last_select = c.select_shot;
    static float last_dump = -1e9f;
    if (last_dump == -1e9f)
        last_dump = c.dump_tracks;  // a value already in the file doesn't fire at load
    if (c.dump_tracks != last_dump) {
        last_dump = c.dump_tracks;
        size_t n = g_tracks.dump(g_dir + "\\tracks.csv");
        swofty::log("[cinecam] dump_tracks: %zu samples written to tracks.csv", n);
    }
    if (c.restart != g_last_restart) {
        std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
        if (!g_edit.shots.empty())
            seek(r, g_edit.start());
    }
    g_last_restart = c.restart;
    if (c.capture != g_last_capture)
        capture(entry, r, cars);
    g_last_capture = c.capture;
    if (c.list_cars != g_last_list)
        list_cars(cars);
    if (c.final_cut != g_last_final) {
        g_last_final = c.final_cut;
        post({CmdKind::FinalCut});
    }
    if (c.render != g_last_render) {
        g_last_render = c.render;
        post({CmdKind::Render});
    }
    {  // diagnostic: the followed car's speed over replay time (finding straights and braking zones)
        static float until = -1, last_t = -1;
        static V3 last_p;
        if (c.speed_trace != g_last_trace) {
            g_last_trace = c.speed_trace;
            until = r.t + 120;
            last_t = -1;
        }
        if (until > 0 && r.t < until && (last_t < 0 || r.t - last_t >= 0.2f || r.t < last_t))
            if (auto car = cars.frame(-1)) {
                if (last_t >= 0 && r.t > last_t)
                    swofty::log("[cinecam] trace t %.2f speed %.0f km/h pos %.0f %.0f %.0f", r.t,
                                cine::len(car->p - last_p) / (r.t - last_t) * 3.6f, car->p.x, car->p.y, car->p.z);
                last_t = r.t;
                last_p = car->p;
            }
        if (until > 0 && r.t >= until)
            until = -1;
    }
    g_last_list = c.list_cars;
    int fslot = g_followed_slot;
    if ((g_frames % 120) == 0 || fslot == -2) {
        fslot = followed_slot();
        if (fslot != g_followed_slot) {
            swofty::log("[cinecam] the replay follows car slot %d", fslot);
            g_followed_slot = fslot;
        }
    }

    // editor actions (keyboard / pad), then the UI's commands
    if (editor) {
        if (kb.add || (hit & A))
            add_key(r, cars, entry);
        static bool y_used;
        if (hit & Y)
            y_used = false;
        int dir = kb.speed_dir;
        if (b & Y) {
            if (hit & LEFT)
                dir--, y_used = true;
            if (hit & RIGHT)
                dir++, y_used = true;
        }
        bool y_released = g_prev_y && !(b & Y);
        g_prev_y = (b & Y) != 0;
        if ((y_released && !y_used) || kb.freeze) {
            g_frozen = !g_frozen;
            toast(g_frozen ? "Replay frozen" : "Replay running");
        }
        if (dir) {
            g_play_speed = next_play_speed(g_play_speed, dir);
            g_frozen = false;
            char m[48];
            snprintf(m, sizeof m, "Replay speed %.2fx", g_play_speed);
            toast(m);
        }
        constexpr uint16_t LB = 0x100;
        if ((b & LB) && (hit & (LEFT | RIGHT))) {
            std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
            if (auto kt = jump_key(g_edit, r.t, (hit & RIGHT) ? 1 : -1))
                seek(r, *kt);
        }
        bool no_step = (b & (Y | LB)) != 0;
        float step = kb.step + (((hit & LEFT) && !no_step) ? -0.1f : 0.f) + (((hit & RIGHT) && !no_step) ? 0.1f : 0.f);
        if (step != 0)
            seek(r, r.t + step);
    }
    run_commands(r, cars, entry);
    editor = g_editor.load();
    fly = editor && g_fly.load();
    preview = g_preview.load();

    bool can_override =
        r.state == 2 || r.state == 3 || r.state == 4 || (r.state == 5 && (c.in_photo_mode || g_render.on));
    bool overriding = false;
    float speed = 1.0f;
    int shot = -1;
    cine::Edit ed;
    {
        std::lock_guard<std::recursive_mutex> lk(g_seq_mx);
        ed = g_edit;
    }
    bool watch = (editor && !fly) || preview || (!editor && c.mode == 2);
    if (preview && r.t <= ed.end())
        g_preview_inside = true;

    if (fly && can_override) {
        overriding = freecam_frame(entry, r, cars, dt, !g_fc_init || !g_overriding, kb, pad, b, hit);
        g_fc_init = true;
    } else if (watch && can_override) {
        if (auto v = cine::evaluate_edit(ed, r.t, cars)) {
            // a new shot is a hard cut only if the view jumps (a split shot plays on without one)
            bool jump = cine::len(v->eye - g_shown.eye) > 1.5f || std::fabs(cine::qdot(v->q, g_shown.q)) < 0.9976f;
            write_view(entry, v->eye, v->q, v->fov, !g_overriding || (v->shot != g_last_shot && jump));
            shot = v->shot;
            speed = v->speed;
            overriding = true;
        } else if (preview && r.t > ed.end() && g_preview_inside && !g_render.on) {
            g_preview = false;
            if (g_final.exchange(false)) {  // Final Cut done: the editor where it was (and its HUD setting)
                hud_request(g_cfg.editor_hides_hud);
                g_editor = true;
                g_fc_init = false;
                if (g_final_return >= 0)
                    seek(r, g_final_return);
                toast("Final Cut finished");
            } else {
                toast("End of the edit");
            }
        }
    } else if (can_override && !editor && c.mode == 1) {
        if (auto car = cars.frame(-1)) {
            V3 eye = car->at({c.ox, c.oy, c.oz});
            write_camera(entry, eye, eye, cine::look_quat(*car, c.yaw, c.pitch, c.roll), c.fov, c.near_, !g_overriding);
            overriding = true;
        }
    }
    g_last_shot = shot;
    // The game's audio state: while we drive the camera, the replay's own camera is kept in car-camera mode (live
    // engines; TV mode plays recorded pass-bys and turns the engines down), on the car it already follows. It never
    // changes car: a change is audible. The listener itself follows our camera (move_listener).
    if (c.sound_follows_shot && overriding && (g_frames % 15) == 0) {
        uintptr_t d = g_director;
        uint8_t focus = 0xff, mode = 0;
        if (d && rd(d + 0x70 + 0x14, focus) && rd(d + 0x70 + 0x15, mode) && mode != 1)
            request_game_car(focus != 0xff ? int(focus) : g_followed_slot);
    }
    // Frozen: the game keeps its engine sounds going on one note (in its own Pause too), so the game's audio session
    // is muted while frozen and unmuted after (only if we muted it).
    bool render_hold = g_render.on && g_render.phase == 3 && !g_render.audio_started;  // the sound pass loading
    set_game_muted((editor && g_frozen) || (g_render.on && (g_render.phase < 3 || render_hold)));
    render_tick(r, speed, cars);
    if (render_hold)
        speed = 0;
    if (editor)
        speed = g_frozen ? 0.0f : speed * g_play_speed;
    if (c.base_speed != 0)
        speed = c.base_speed;
    if (speed != 1.0f || g_speed_written) {
        float cur = 1;
        rd(r.rs + 0x24, cur);
        if (cur != speed)
            set_speed(r, speed);
    }

    {
        std::lock_guard<std::mutex> lk(g_status_mx);
        Status &s = g_status;
        s.in_replay = true;
        s.editor = editor;
        s.fly = fly;
        s.orbit = fly && g_orb.on;
        s.preview = preview;
        s.frozen = g_frozen;
        s.ride = g_fc.ride;
        s.ride_car = g_fc.ride_car;
        s.playing = overriding && watch;
        s.followed = fslot;
        s.slots = slot_count();
        s.shot = ed.shot_at(r.t);
        s.t = r.t;
        s.end = r.end;
        s.speed = speed;
        s.play_speed = g_play_speed;
        s.fly_speed = g_fc.speed;
        s.final_cut = g_final.load();
        s.edit_t0 = ed.start();
        s.edit_t1 = ed.end();
        s.letterbox = c.letterbox;
        s.rendering = g_render.on;
        s.render_frame = g_render.frame;
        s.render_total = g_render.total;
        s.render_phase = g_render.phase;
        s.o_fps = c.render_fps;
        s.o_settle = c.render_settle;
        s.o_crf = c.render_crf;
        s.o_format = c.render_format;
        s.o_blur = c.render_blur;
        s.o_shutter = c.render_shutter;
        s.o_audio = c.render_audio;
        s.o_listener = c.move_listener;
        s.o_follow = c.sound_follows_shot;
        s.o_passby = c.recorded_passby;
        s.o_hidehud = c.editor_hides_hud;
        s.o_keycurve = c.key_curve;
        s.o_clip = c.check_clipping;
        s.o_offset = c.render_audio_offset;
        s.o_wait = c.render_audio_wait;
        s.o_preroll = c.preroll;
        snprintf(s.o_dir, sizeof s.o_dir, "%s", narrow(render_output_dir()).c_str());
        // the progress line only on hold frames well before a capture (settle >= 4 leaves 2 clean frames)
        s.render_show = g_render.on && (g_render.phase < 2 || (!g_render.waiting && g_render.hold + 2 < g_render.settle));
        s.fade = c.fade;
        snprintf(s.seq, sizeof s.seq, "%s", seq_name().c_str());
        if (editor) {
            V3 eye;
            cine::Quat q;
            float fov;
            read_view(entry, eye, q, fov);  // what is on screen (written this frame or the game's)
            s.fov = fov;
            cine::Frame world;
            V3 r_, u_, b_;
            cine::cols_from_quat(q, r_, u_, b_);
            V3 f_ = b_ * -1;
            cine::local_angles(world, f_, s.yaw, s.pitch);
            s.wroll = cine::roll_of(world, r_, f_);
            cine::Frame nf;
            s.near_car = nearest_car(cars, eye, nf);
            if (s.near_car >= -1) {
                cine::local_angles(nf, f_, s.c_yaw, s.c_pitch);
                s.c_roll = cine::roll_of(nf, r_, f_);
                s.c_height = cine::dot(eye - nf.p, nf.u);
                s.c_dist = cine::len(eye - nf.p);
            }
        }
    }

    publish_audio_view(overriding && c.move_listener);
    // the camera path for the overlay: the view every frame, the sampled paths a few times a second
    {
        bool draw = editor && g_show_path.load() && can_override;
        std::vector<PathDraw> fresh;
        bool resample = draw && (g_frames % 4) == 0;
        if (resample) {
            int sel = g_path_shot.load(), under = ed.shot_at(r.t), ns = int(ed.shots.size());
            if (under < 0)  // in a gap: the next shot coming up
                for (int i = 0; i < ns && under < 0; i++)
                    if (ed.shots[size_t(i)].t0 > r.t)
                        under = i;
            for (int idx : {sel, under}) {
                if (idx < 0 || idx >= ns || (idx == under && under == sel && !fresh.empty()))
                    continue;
                const cine::Shot &s = ed.shots[size_t(idx)];
                if (s.type == cine::ShotType::transition) {  // its move, on the recorded drive when covered
                    bool use = g_tracks.coverage(g_followed_slot, s.t0, s.t1) > 0.9f;
                    TrackCars tc(cars, use, g_followed_slot);
                    fresh.push_back({cine::sample_transition(ed, idx, tc), s.type, idx == sel, idx, use});
                } else if (s.type == cine::ShotType::orbit || s.type == cine::ShotType::track || s.keyed()) {
                    // a shot on a car: along the car's recorded drive when the tracks cover the shot
                    int slot = s.car < 0 ? g_followed_slot : s.car;
                    bool use = s.uses_car() && g_tracks.coverage(slot, s.t0, s.t1) > 0.9f;
                    TrackCars tc(cars, use, g_followed_slot);
                    fresh.push_back({cine::sample_path(s, idx, tc), s.type, idx == sel, idx, use});
                }
            }
            if (g_cfg.check_clipping) {
                static double ms_logged = -1;
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                size_t rays = 0;
                for (auto &pd : fresh) {
                    clip_check(pd.ps);
                    rays += pd.ps.pts.size() * 4;
                }
                QueryPerformanceCounter(&t1);
                double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
                if (ms_logged < 0 && rays) {  // once: what it costs
                    swofty::log("[cinecam] clipping check: %zu rays in %.2f ms", rays, ms);
                    ms_logged = ms;
                }
            }
        }
        PathView view{g_shown.eye, g_shown.q, g_shown.fov};
        if (draw && !overriding)  // the game's camera is on screen
            read_view(entry, view.eye, view.q, view.fov);
        std::lock_guard<std::mutex> lk(g_path_mx);
        g_path_valid = draw;
        g_path_view = view;
        if (resample)
            g_path_draw.swap(fresh);
    }
    if (overriding) {
        g_overrides++;
        if (c.log_every > 0 && g_overrides % uint64_t(c.log_every) == 0)
            swofty::log("[cinecam] t %.2f state %u%s%s%s: %llu override frame(s)", r.t, r.state, editor ? " editor" : "",
                        fly ? " fly" : "", preview ? " play" : "", (unsigned long long)g_overrides);
    }
    g_overriding = overriding;
}

uint64_t vpm_update(void *vpm, void *a2, void *a3, void *a4)
{
    g_frames++;
    if (g_frames == 1)
        swofty::log("[cinecam] first ViewportManager::Update call, thread %lu, vpm %p", GetCurrentThreadId(), vpm);
    if (g_globals_ok)
        apply(vpm);
    return g_orig(vpm, a2, a3, a4);
}

// ----- install ---------------------------------------------------------------------------------

int find_slots(uintptr_t base, const char *name, uintptr_t value, void ***out, int max)
{
    auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    auto *sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (strncmp(reinterpret_cast<const char *>(sec->Name), name, IMAGE_SIZEOF_SHORT_NAME) != 0)
            continue;
        // Only the pages that are committed and readable: on the EA app's build the section has pages that aren't
        // (a plain scan faulted at the first one, crashing the game at start: player report, 2026-11-05).
        uintptr_t at = base + sec->VirtualAddress, end = at + sec->Misc.VirtualSize;
        while (at < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<void *>(at), &mbi, sizeof mbi))
                break;
            uintptr_t rend = std::min(end, uintptr_t(mbi.BaseAddress) + mbi.RegionSize);
            constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                auto *p = reinterpret_cast<uintptr_t *>((at + 7) & ~uintptr_t(7));
                for (; uintptr_t(p + 1) <= rend; p++)
                    if (*p == value) {
                        if (n < max)
                            out[n] = reinterpret_cast<void **>(p);
                        n++;
                    }
            }
            if (rend <= at)
                break;
            at = rend;
        }
    }
    return n;
}

bool write_ptr(void **at, void *value)
{
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof value, PAGE_READWRITE, &old))
        return false;
    *at = value;
    VirtualProtect(at, sizeof value, old, &old);
    return true;
}

std::string module_addon_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&module_addon_dir), &self);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string p = path;
    for (int i = 0; i < 2; i++) {  // ...\<addon>\native\cinecam.dll -> ...\<addon>
        size_t s = p.find_last_of("\\/");
        if (s == std::string::npos)
            break;
        p.resize(s);
    }
    return p;
}

void setup_dirs()
{
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    g_addon_dir = module_addon_dir();
    g_dir = n ? std::string(buf) + "\\Swofty\\cinematic-replay" : g_addon_dir;
    CreateDirectoryA((std::string(buf) + "\\Swofty").c_str(), nullptr);
    CreateDirectoryA(g_dir.c_str(), nullptr);
    CreateDirectoryA((g_dir + "\\sequences").c_str(), nullptr);
    // first run: seed the live config from the addon's copy (edits are made in the editor)
    CopyFileA((g_addon_dir + "\\cinecam.cfg").c_str(), (g_dir + "\\cinecam.cfg").c_str(), TRUE);
}

void install()
{
    const SwoftyApi *api = swofty::api;
    g_base = api->module_base();
    setup_dirs();
    load_config();

    uintptr_t fn = 0;
    int n = api->scan(".xtls", kVpmUpdatePattern, &fn);
    if (n != 1) {
        swofty::log("[cinecam] off: ViewportManager::Update signature has %d match(es) (need 1)", n);
        return;
    }
    g_globals_ok = fn - g_base == kVpmUpdateRva;
    if (!g_globals_ok)
        swofty::log("[cinecam] ViewportManager::Update at base+0x%llx, not base+0x%llx: not build d873a1c8, the "
                    "replay and car globals stay off (camera untouched)",
                    (unsigned long long)(fn - g_base), (unsigned long long)kVpmUpdateRva);
    void **found[4];
    int ns = find_slots(g_base, ".xtext", fn, found, 4);
    if (ns != 1) {
        swofty::log("[cinecam] off: ViewportManager::Update is in %d table slot(s) (need 1)", ns);
        return;
    }
    g_slot = found[0];
    g_orig = reinterpret_cast<Update_t>(fn);
    if (!write_ptr(g_slot, reinterpret_cast<void *>(&vpm_update))) {
        swofty::log("[cinecam] off: could not write the vtable slot at base+0x%llx",
                    (unsigned long long)(uintptr_t(g_slot) - g_base));
        return;
    }
    QueryPerformanceFrequency(&g_qpf);
    install_pad_hook();
    install_file_hook();
    if (g_globals_ok)  // its entry is a fixed address of build d873a1c8: on any other build it may not be readable
        install_ray_hook();
    g_mute_state = 1;  // a freeze mute left over from a game closed while frozen: undo it once at start
    set_game_muted(false);
    if (g_globals_ok) {  // the ReplayDirector's update: the replay's own camera can follow the shot's car
        void **dslot = reinterpret_cast<void **>(g_base + kDirectorUpdateSlotRva);
        uintptr_t cur = 0;
        if (rd(uintptr_t(dslot), cur) && cur == g_base + kDirectorUpdateRva) {
            g_director_orig = reinterpret_cast<void *>(cur);
            if (write_ptr(dslot, reinterpret_cast<void *>(&director_update_stub)))
                swofty::log("[cinecam] replay director: update slot base+0x%llx wrapped (sound follows the shot's car)",
                            (unsigned long long)kDirectorUpdateSlotRva);
        } else {
            swofty::log("[cinecam] replay director off: slot base+0x%llx holds base+0x%llx, not base+0x%llx",
                        (unsigned long long)kDirectorUpdateSlotRva, (unsigned long long)(cur - g_base),
                        (unsigned long long)kDirectorUpdateRva);
        }
    }
    // the audio hooks, only when a setting asks for them at launch (both off: the game's audio is untouched)
    if (g_globals_ok && !g_cfg.recorded_passby) {  // the per-car audio update's vtable slot, for the pass-by switch
        void **aslots[4];
        int na = find_slots(g_base, ".xtext", g_base + kCarAudioUpdateRva, aslots, 4);
        if (na == 1) {
            g_audio_orig = reinterpret_cast<void *>(g_base + kCarAudioUpdateRva);
            if (write_ptr(aslots[0], reinterpret_cast<void *>(&audio_update_stub)))
                swofty::log("[cinecam] pass-by switch: per-car audio update slot base+0x%llx",
                            (unsigned long long)(uintptr_t(aslots[0]) - g_base));
        } else {
            swofty::log("[cinecam] pass-by switch off: the per-car audio update is in %d slot(s) (need 1)", na);
        }
    }
    if (g_globals_ok && g_cfg.move_listener) {  // the AudioCameraListener's update: the listener hears from our camera
        void **lslot = reinterpret_cast<void **>(g_base + kListenerUpdateSlotRva);
        uintptr_t cur = 0;
        if (rd(uintptr_t(lslot), cur) && cur == g_base + kListenerUpdateRva) {
            g_listener_orig = reinterpret_cast<ListenerUpdate_t>(cur);
            if (write_ptr(lslot, reinterpret_cast<void *>(&listener_update)))
                swofty::log("[cinecam] audio listener: update slot base+0x%llx wrapped",
                            (unsigned long long)kListenerUpdateSlotRva);
        } else {
            swofty::log("[cinecam] audio listener off: slot base+0x%llx holds base+0x%llx, not base+0x%llx",
                        (unsigned long long)kListenerUpdateSlotRva, (unsigned long long)(cur - g_base),
                        (unsigned long long)kListenerUpdateRva);
        }
    }
    g_vm_resolve = reinterpret_cast<Resolve_t *>(api->resolve("Vm_Resolve_RegistryNode"));
    g_vm_registry = reinterpret_cast<void **>(api->resolve("g_Vm_Registry"));
    if (g_vm_resolve && g_vm_registry && swofty::events::available() &&
        swofty::events::on_frame(name_tick) == swofty::events::Result::ok)
        swofty::log("[cinecam] driver names: from the view model on the frame tick");
    else
        swofty::log("[cinecam] driver names: unavailable (bindings %p %p, events %d); cars show as slots",
                    reinterpret_cast<void *>(g_vm_resolve), static_cast<void *>(g_vm_registry),
                    int(swofty::events::available()));
    if (ov::available()) {
        auto pr = ov::on_paint(paint);
        swofty::log("[cinecam] overlay panel: %s", pr == ov::Result::ok ? "on" : "unavailable");
    } else {
        swofty::log("[cinecam] overlay unavailable: the free camera works, without its panel or keyboard focus");
    }
    swofty::log("[cinecam] on: ViewportManager::Update base+0x%llx, vtable slot base+0x%llx (vtable base+0x%llx); "
                "files in %s",
                (unsigned long long)(fn - g_base), (unsigned long long)(uintptr_t(g_slot) - g_base),
                (unsigned long long)(uintptr_t(g_slot) - kVpmUpdateSlot * 8 - g_base), g_dir.c_str());
}

} // namespace

extern "C" void director_tick(void *director) // called by director_update_stub
{
    director_tick_impl(director);
}

SWOFTY_ON_LOAD
{
    install();
}

SWOFTY_ADDON()
