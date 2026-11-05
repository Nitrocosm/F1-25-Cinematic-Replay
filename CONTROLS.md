# Cinematic Replay controls

In the editor, **H** shows these on screen (the controller list when you're using a controller).

## Keyboard and mouse

### Editor

| Key | Does |
|---|---|
| F9 | open / close the editor (also stops Final Cut or a render) |
| F | fly the camera / watch the edit |
| K | add a key here (makes a new Free path if there's no shot here) |
| H / G | all controls / guides (thirds, horizon) |
| U | hide / show the editor (preview) |
| J | show / hide the camera path in 3D |

### Fly

| Key | Does |
|---|---|
| W A S D, Q / E | move along the camera's own axes, down / up |
| Right mouse | hold and move to look |
| Shift / Ctrl | faster / fine |
| Mouse wheel | fly speed |
| Z / C | roll |
| R / V | zoom in / out |
| L / Home | align to the car / level the horizon |
| Shift+L / Shift+Home | flat on / roll to the track |
| T | ride / leave the followed car |
| O | orbit the car: look at it, move around it |
| , / . | previous / next key (or double-click a key) |

### Replay

| Key | Does |
|---|---|
| Space / P | freeze / run |
| [ / ] | back / forward 0.1 s (Shift: 1 s, Ctrl: one frame) |
| - / = | replay speed down / up |

### Timeline

| Input | Does |
|---|---|
| Click / drag a shot | select / move it; drag its edges to trim |
| Drag an edge two shots share | trims the shot on the side you grab; shots never overlap |
| Keys lane | click, drag or box-select the shot's keys |
| Blue dot under the keys lane (a slider key) | double-click: go to that key |
| Shift+click / drag on SHOTS | select several shots |
| Ctrl+C / Ctrl+V, Del | copy / paste keys or shots, delete |
| Ctrl+Z / Ctrl+Y | undo / redo |
| B | split the shot at the playhead |
| N | snapping on / off (hold Alt to drag without snapping) |
| Wheel / Ctrl+wheel | scroll / zoom the timeline |

### Camera path (selected shot, J on)

| Input | Does |
|---|---|
| Drag a key's white dot | move the key (with Ctrl, its neighbours follow) |
| Drag a blue handle | bend that segment |
| Double-click the line | add an anchor (a key) there |
| Double-click a handle / dot | straighten the segment / go to the key |
| Shift while dragging | nearer / farther instead of across the screen |
| Arrows on the picked key or handle | drag one: move along that axis only (red right, green up, blue forward, in the shot's frame) |

Where the path goes into the track or a barrier it turns **red** (a red dot where it starts, red ticks on the
shot in the timeline); within 30 cm of the ground it turns **orange**. The check uses the game's own collision
and can be turned off in Options.

The inspector's **Reset position** puts the selected keys back where they were before they were first dragged;
**Straighten bend** does the same for the picked handle's segment. New slider keys get the curve set in
**Options** (Smooth or Linear); existing keys keep theirs.

### Properties panel (left, with a shot selected)

Each keyable value has a row: its value at the playhead, a diamond to key it there (or remove the key there),
and its keys along the shot.

| Input | Does |
|---|---|
| Click between two keys | shape that segment's speed curve in the graph below (Smooth, Linear, Ease in, Ease out) |
| Key in a row | click: pick the segment after it; double-click: go to it; right-click: remove it |
| Graph: drag an anchor or handle | reshape the curve (it can go below and above the two values) |
| Graph: double-click the curve / an anchor | add an anchor there / remove it |

The same graph edits a Transition's curves (Forward, Side, Height, Look) in the inspector.

### Sliders

| Input | Does |
|---|---|
| Diamond left of a slider | key the value here / remove that key |
| Ctrl+click the diamond | clear all the value's keys |
| Ctrl+drag | fine (20x slower) |
| Wheel / Shift+wheel | one step (1 cm, 0.5 degree) / ten steps |

## Controller

| Button | Does |
|---|---|
| L3 + R3 (both sticks in) | open / close the editor |
| Start | fly the camera / watch the edit |
| A | add a key here |
| Left stick, LT / RT | move, down / up |
| Right stick | look |
| Hold the left stick in | fine |
| Right stick click | fly speed preset |
| LB / RB | roll |
| LB + RB together | flat on the track surface |
| D-pad up / down | zoom |
| Back | level the horizon |
| X | ride / leave the followed car |
| B | orbit the car |
| LB + D-pad left / right | previous / next key |
| Y | freeze / run |
| D-pad left / right | back / forward 0.1 s |
| Y + D-pad left / right | replay speed |

The panels and the timeline are used with the mouse.
