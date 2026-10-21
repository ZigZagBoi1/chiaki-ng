---
name: chiaki-ps5
description: Operate a PlayStation 5 (or PS4) through the chiaki-ng MCP server efficiently and reliably. Use whenever controlling a console via chiaki-ng tools (controller_input, take_screenshot, run_routine, ...), especially for repeated or 24/7 automation such as relaunching apps.
---

# Operating a PlayStation through chiaki-ng

You see the console only as video and act only through a virtual DualSense. Every look costs tokens and every
blind press risks drifting off course. Work like an experienced player who has a script for everything
they have done before.

## Golden rules

1. **Reuse before you explore.** Call `list_routines` first. If a routine does the job, `run_routine` it: it
   replays at button speed and only sends a screenshot when something fails.
2. **Locate cheaply.** `match_checkpoints` tells you which known screen is showing without a screenshot. Use
   `take_screenshot {max_width: 960}` for normal looking; use `region` to zoom only when you must read small text.
3. **One input, one check.** Send `controller_input` with `expect: {until: "screen_changed"}` (or a checkpoint)
   so a press that did nothing fails immediately. Never chain more than 3 unverified presses.
4. **When lost, reset; do not wander.** Run `ps5_go_home` (hold PS), confirm the home checkpoint, re-plan.
5. **Turn every success into a routine.** After you complete a task by hand, `save_routine` the exact steps
   with checkpoints between them. The next run is nearly free.
6. **Stop on anything sensitive.** Purchases, account sign-in, passwords, deleting data, system updates,
   factory settings: stop and ask the user.

## PS5 screen map

- **Home screen.** A horizontal row of game/app tiles, most recently used first. The highlighted tile's
  artwork fills the background. The **Game Library** tile is at the far right end of the row (reach it with
  repeated `dpad_right`, or jump there faster with `sequence` presses and a check).
- **Games / Media.** Two home spaces switched at the top-left of the home screen (move up from the tile row,
  then left/right). Streaming apps (YouTube, Netflix, Twitch, ...) live under **Media**.
- **Top-right icons** (move up from the tile row, then right): Search, Settings (gear), your profile.
- **Below a tile** (`dpad_down` from the tile row): that game's hub: activities, news, trophies.
- **Control Center.** Short PS press. A bar at the bottom: cards for the current game/activities and
  controls (notifications, sound, power, ...). Press `circle` or PS again to close it.
- **Hold PS (~1 s).** Goes straight to the home screen from anywhere. This is the most reliable reset.

## Common tasks

- **Launch an app/game:** home screen, highlight its tile, `cross`. If it is not in the row: Game Library,
  then Installed, then find it.
- **Return to a running app:** highlight its tile on the home screen, `cross` (it resumes, it does not restart).
- **Close an app/game:** on the home screen, highlight its tile, `options`, choose **Close Game / Close App**,
  confirm. Alternative: Control Center, highlight the game card, `options`, Close. Read the menu with a
  region zoom the first time; then store the exact `dpad` count in a routine.
- **Relaunch:** close as above, wait until the tile shows it is no longer running (checkpoint or
  `wait_for screen_stable`), then launch.
- **Dismiss an error/notice dialog:** read it first (region zoom). Usually `cross` on OK. "Update available"
  prompts: choose to continue without updating unless the user asked otherwise.
- **"Who's using this controller?"** appears after waking or reconnecting: select the right user, `cross`.

## Controls

- `cross` = confirm, `circle` = back/cancel. (Some consoles swap these in Settings > Accessibility >
  Controllers; if `cross` keeps backing out, that is why.)
- `options` = context menu of the highlighted item; `triangle`/`square` = context actions shown in on-screen
  hints; `l1`/`r1` = switch tabs; `l2`/`r2` = page through long lists in many menus.
- Prefer the d-pad over the left stick in menus: one press moves exactly one item.
- On-screen keyboard: navigate with the d-pad, `cross` types a key. Button shortcuts (delete, space, confirm)
  are shown in hints on the keyboard itself; read them once and reuse.
- In games anything goes: use `steps` with sticks, triggers and `hold_ms`.

## Timing (defaults that work)

| Action | Wait / check |
|---|---|
| Menu move | `expect: {until: "screen_changed"}`, default timeouts |
| Opening a menu or dialog | 300-800 ms, or `wait_for {until: "screen_stable"}` |
| Hold PS to go home | `hold_ms: 1200`, then the `ps5_home` checkpoint with `timeout_ms: 8000` |
| App/game launch | checkpoint of the app's first screen, `timeout_ms` 30000-60000 |
| Waking the console / starting the stream | `ensure_stream` step or `wait_for_stream {state: "video"}` (up to ~60 s) |

`screen_stable` never succeeds on live video (game footage, video playback): use checkpoints or
`screen_changed` there.

## Building a reliable routine (recipe)

1. Get to a known start (`ps5_go_home`) and `save_checkpoint` important screens once while they are settled:
   `ps5_home`, the app's home screen, the close menu.
2. Do the task by hand, one verified input at a time.
3. Save it, with a checkpoint after each transition, `retries` and a `recovery` routine:

```json
{
  "name": "relaunch_youtube",
  "description": "From anywhere: close YouTube if running, start it again, end on the YouTube home screen",
  "retries": 2,
  "recovery": "ps5_go_home",
  "steps": [
    {"ensure_stream": {"index": 0}},
    {"routine": "ps5_go_home"},
    {"checkpoint": "ps5_home", "timeout_ms": 8000},
    {"label": "open tile menu", "controller": {"buttons": ["options"], "expect": {"until": "screen_changed"}}},
    {"label": "select Close App (2 down, verified once)", "controller": {"sequence": ["dpad_down", "dpad_down", "cross"]}, "optional": true},
    {"wait_ms": 1500},
    {"label": "launch", "controller": {"buttons": ["cross"]}},
    {"checkpoint": "youtube_home", "timeout_ms": 45000}
  ]
}
```

The dpad counts above are placeholders: use the ones you verified. Mark steps `optional` when they only
apply sometimes (e.g. closing an app that may not be running).

4. `run_routine` it twice from different starting screens. If it fails, read the failed step in the log and the
   screenshot, fix that step, save again.

## 24/7 automation loops

- Start every cycle with `ensure_stream` (reconnects after network drops or rest mode) and a checkpoint check.
- Drive everything through `run_routine`; on success you get a few lines of text, not images.
- On failure: one look at the attached screenshot, then fix the routine instead of improvising by hand each cycle.
- Watch stream health occasionally with `get_app_state` (bitrate, packet loss). `mcp_self_check` diagnoses setup problems.
- Never leave buttons held: routines release the controller on failure; `release_controller` if in doubt.

## PS4 differences

Home is a horizontal tile row too, but the function area (store, notifications, settings, power) is reached by
pressing up. A short PS press goes to home; holding PS opens the quick menu (close application, power).
