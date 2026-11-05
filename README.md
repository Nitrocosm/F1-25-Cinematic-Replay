# Cinematic Replay

<img src="addon/icons/cinematic-replay.png" alt="Cinematic Replay icon" width="128" align="right">

A camera editor for F1 25 replays, built into the game. Place shots on a timeline, ride or chase any car, fly
free camera paths with keyframes, play the result back without any HUD, and render it to a video file with
the game's sound.

A Swofty Launcher addon by xSoul.

## What it does

- **Shots on a timeline.** Five kinds:
  - **Onboard:** ride a car (cockpit, T-cam, nose, front wing, rear wing, sidepods, the four wheels).
  - **Chase:** follow behind a driver with a level horizon.
  - **Track cam:** a fixed camera that turns to follow a driver.
  - **Orbit:** circle a driver, either way round.
  - **Free path:** fly the camera and drop keys; it flies a smooth path through them.
  - **Transition:** fills the gap between two shots, moving the camera from the first into the second while both
    keep following their cars. Each axis of the move (forward, side, height) and the turn of the view has its
    own speed curve: e.g. come in level first, then drop onto the car.
- **Every slider can be keyed:** distance, height, angles, roll, FOV and more can change over a shot. The
  **Properties** panel (left) shows each value's keys along the shot; jump to a key, delete it, or shape how the
  value moves between two keys.
- **Speed curves you can draw:** transitions and keyed values use graph editors like a video editor's: drag the
  handles, add anchors, overshoot or dip below the values, or pick Smooth, Linear, Ease in or Ease out. New keys
  are smooth by default.
- **Path shaping in 3D:**
  - See the camera path over the track.
  - Drag keys, bend segments and add anchors where you need them; arrows on the picked key move it along one
    axis, and **Reset position** puts it back.
  - Choose per segment: smooth, hold, ease in / out, or a corkscrew.
  - **Clipping check:** the path turns red where it goes into the track or a barrier (orange where it skims the
    ground), using the game's own collision, with red marks on the shot in the timeline.
- **Paths that follow the real drive:** paths on a car follow where the car really went, once that part of the
  replay has been watched.
- **Editing:** undo / redo, split, snapping, multi-select, copy / paste, and several edits per replay.
- **Sound from the camera:** you hear the replay from where the camera is.
- **Final Cut:** plays the edit with every HUD hidden, with optional letterbox bars.
- **Render:** saves the edit as a video file (H.264, or ProRes for grading) with the game's sound.
  - Choose the whole edit, the selected shots, or an in / out range.
  - Frame by frame, so the video is smooth whatever your frame rate.
  - Optional motion blur.

## What you need

- **Windows** 10 (version 2004 or newer) or 11. Linux / Steam Deck isn't supported.
- **F1 25** on Steam, game build 25251167. The addon is made for one game build: after a game update it switches
  itself off (the game runs normally) until the addon is updated.
- **Swofty Launcher** with its in-game overlay (the addon's controls are drawn by it).
- For rendering: [ffmpeg](https://ffmpeg.org), free. If it isn't on your PC, the Render menu shows an
  **Install** button that installs it with WinGet (or run `winget install Gyan.FFmpeg` yourself). It isn't
  included in the addon: it's large and has its own license (GPL).

## Installing

1. Download `xsoul.cinematic-replay.swoftyaddon` from the latest release.
2. Drop it on Swofty Launcher (or the **Addons** page), and turn it on in your profile.
3. Start the game from the launcher.

## Graphics settings

- **Use TAA or DLAA for anti-aliasing, not FSR, and turn frame generation off.** FSR and frame generation smear
  replays when the camera cuts or moves fast.
- For renders: turn **HDR off** (renders are captured in SDR). Also turn off FPS counters and other overlays
  (Steam, NVIDIA, AMD, MSI Afterburner), because they end up in the video.

## Getting started

1. Open a replay (Theatre, or a session's replay).
2. Press **F9** to open the editor. On a controller, press both sticks in.
3. In the panel on the right, pick a shot kind. It is placed on the timeline at the playhead, on the car the
   replay follows.
4. Click a shot on the timeline to change it: its car, mount or angles, and its sliders.
5. For a free path, press **F** to fly the camera, move to a spot and press **K** to drop a key. Scrub the
   replay forward, move, and press **K** again.
6. Press **F** again to watch the edit, or use **Play > Play all**. **Space** plays and pauses.
7. To move from one shot into the next, leave a gap between them, put the playhead in it and add a **Transition**.
8. **Play > Final Cut** shows it clean. **Play > Render...** saves it as a video.

The game's HUD is hidden while the editor is open (Options). Press **H** in the editor for every control (see
also [CONTROLS.md](CONTROLS.md)). **U** hides the editor for a moment so you can see the shot alone.

## Rendering

- **Play > Render...** renders the whole edit, the selected shots, or a range between an In and an Out point.
- The video is made frame by frame; the replay plays very slowly while it renders. Then the edit plays once more
  at normal speed while the game's sound is recorded, and the two are joined.
- Keep the game open while it renders: other windows may cover it, but **don't minimize it**. **F9** cancels.
- Files go to your Videos folder, in `Cinematic Replay` (**Options > Open folder**; `render_dir` in
  `cinecam.cfg` changes it).
- **Options** (in the timeline bar) sets the frame rate, the format (H.264, ProRes 422 HQ, ProRes 4444), quality,
  motion blur, the shutter, and the sound. ProRes files are large: about 60 MB per second at 1080p for 422 HQ.

## Your files

Everything lives in `%LOCALAPPDATA%\Swofty\cinematic-replay\`:

- `sequences\<name>.json`: your edits. The **Edit** button in the timeline bar makes new ones, duplicates,
  renames and deletes them. Deleted edits go to `sequences\deleted\`.
- `cinecam.cfg`: the settings. Most are in the Options pane too; the file explains each one.

Each edit remembers the replay it was made on (its shots are tied to that replay's times and cars): the replay
file it came from, or for a session's own replay, its length and drivers. Opening the editor in a replay opens
that replay's latest edit, or starts a new empty one named after the replay file for a replay you haven't edited
yet. The Edit menu lists the edits for the replay you're in.

## Known limits

- Windows only, and one game build at a time (see above).
- Paths on a car follow its real drive only for parts of the replay that have been watched; elsewhere they use
  the car's position at the playhead.
- Final Cut hides the HUD by pressing the game's own hide-HUD button for you. If a press is missed, the HUD may
  come back in a different state from the one you had.

## Building from source

Needs [zig](https://ziglang.org) 0.17 and the Swofty SDK headers (`packages/sdk/include` in the Swofty Launcher
repository).

```sh
SWOFTY_SDK=/path/to/SwoftyLauncher/packages/sdk/include bash addon/build.sh   # addon/native/cinecam.dll
SWOFTY_SDK=... bash addon/tests/run.sh                                          # the shot-engine tests
SWOFTY_SDK=... bash package.sh                                                  # dist/xsoul.cinematic-replay.swoftyaddon
```

`addon/` is the addon folder (`swofty addons install addon` installs a build for testing). The code is in
`addon/native/src/`: `cinecam.cpp` (game hooks, editor, Final Cut, render), `shots.hpp` (shots, keys and camera
paths), `sequence.hpp`, `freecam.hpp`, `render.hpp` / `wgc.hpp` (frame capture), `audio_capture.hpp` (sound
capture).

## License

MIT, see [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
