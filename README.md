# wooting-aim v0.7

Real-time adaptive keyboard tuner for **Wooting 60HE** + **Counter-Strike 2**.

Dynamically adjusts per-key actuation points (AP) and rapid trigger (RT) sensitivity during gameplay based on your movement — tighter AP/RT when counter-strafing, relaxed when idle.

## Features

- **Dual-axis counter-strafe detection** — independent A/D (horizontal) and W/S (vertical) tracking
- **Velocity-aware AP scaling** — AP tightens as your character moves faster (closer to accuracy threshold)
- **Jiggle peek detection** — pre-arms both directions after rapid A-D-A sequences
- **Phase decay** — ultra-aggressive AP (0.15mm) during first 80ms of counter-strafe, then linear relax
- **CS2 Game State Integration** — weapon-specific profiles (rifle/awp/pistol/smg/knife) and round phase detection
- **Crouch-peek optimization** — detects L-Ctrl, tightens RT (crouching speed is already at 34% accuracy threshold)
- **Predictive pre-arming** — detects finger lift before the counter-press happens
- **Counter-strafe quality rating** — PERF/GOOD/FAST/LATE classification per strafe
- **Statistics logging** — CSV log of every counter-strafe with timing data
- **Auto-start** — `--watch` mode detects cs2.exe and starts automatically

## Requirements

- **Wooting 60HE** (v1 or v2) with firmware 2.12+
- **MSYS2 MinGW64** toolchain (GCC)
- **Windows 10/11**
- **CS2** (optional, for GSI features)

## Building

### Quick build (batch script)

```batch
build.bat
```

### MSYS2 MinGW shell

```bash
make
```

### Manual

```bash
gcc -O2 -Wall -g -I./include -I/mingw64/include \
    -o wooting-aim.exe src/main.c src/hid_writer.c \
    -L./lib -L/mingw64/lib \
    -lwooting_analog_sdk -lhidapi -lsetupapi -lws2_32 -ladvapi32
```

## Usage

```
wooting-aim.exe [mode]

Modes:
  --adaptive   Active tuning (default) — reads analog, writes AP/RT in real-time
  --readonly   Monitor only — reads analog values, no writes to keyboard
  --watch      Auto-start — waits for cs2.exe, then runs adaptive mode
  --demo       Test mode — alternates AP on D key between 0.1mm and 3.8mm
```

### Typical usage

```batch
wooting-aim.exe --adaptive
```

Leave the console window open while playing. Close it to stop and restore default AP/RT.

## Configuration

Settings are stored in `wooting-aim.cfg` (auto-generated on first run):

```ini
# Base AP/RT (used when GSI not connected or no counter-strafe)
ap_normal=1.2       # Normal actuation point (mm)
ap_aggro=0.4        # Aggressive AP during counter-strafe (mm)
rt_normal=1.0       # Normal rapid trigger (mm)
rt_aggro=0.1        # Aggressive RT during counter-strafe (mm)
write_interval_ms=50

# Predictive finger-lift detection
predict_threshold=0.70
predict_min_peak=0.30

# Crouch optimization
crouch_rt_factor=0.50

# Weapon profiles (active when GSI connected)
rifle_ap=0.4    rifle_rt=0.1
awp_ap=0.8      awp_rt=0.4
pistol_ap=0.3   pistol_rt=0.1
smg_ap=0.5      smg_rt=0.2
knife_ap=1.5    knife_rt=1.0

# GSI
gsi_enabled=1
gsi_port=58732

# Velocity estimation
vel_enabled=1
vel_scale_enabled=1

# v0.7
jiggle_enabled=1
phase_decay=1
poll_rate_hz=8000
```

## CS2 Game State Integration

The program auto-creates the GSI config at:
```
<CS2 install>/game/csgo/cfg/gamestate_integration_wooting_aim.cfg
```

This tells CS2 to POST JSON updates (weapon, health, round phase) to `http://127.0.0.1:58732`. No restart needed if CS2 is already running — it picks up new GSI configs on map load.

### Recommended CS2 launch options

```
-noreflex -high
```

### Recommended NVIDIA settings

- Low Latency Mode: **Ultra**
- V-Sync: **On** (in NVCP, off in-game)

## How it works

1. **Read** — polls WASD + L-Ctrl analog values via Wooting Analog SDK (~2.5M reads/sec)
2. **Detect** — state machine tracks per-axis movement: IDLE → STRAFE → COUNTER
3. **Estimate velocity** — discrete Source 2 friction model (sv_friction=5.2, 64 tick)
4. **Compute targets** — combines weapon profile + velocity + phase + jiggle state into per-key AP/RT
5. **Write** — sends AP/RT to keyboard RAM via HID protocol (report 21/25), no flash wear
6. **Restore** — on exit, writes back normal AP/RT values

### Key algorithm details

- **CS2 input is binary** — analog key depth does NOT affect movement speed. The velocity model uses boolean key state (pressed/released), not analog depth.
- **34% threshold** — below 34% of weapon MaxPlayerSpeed, movement inaccuracy is negligible. The algorithm aims to get you below this threshold as fast as possible.
- **Phase decay** — first 80ms after counter-strafe start: AP drops to 0.15mm (minimum safe value to avoid ghost inputs). Then linearly relaxes back to weapon AP over the next 120ms.
- **Source 2 friction** — per-tick decay factor 0.91875 above stopspeed (80 u/s), fixed 6.5 u/s deceleration below.

## Anti-cheat safety

Adjusting AP and RT values is **permitted** by Valve — this is the same as changing settings in Wootility, just automated. The program does NOT:
- Send fake keypresses
- Modify game memory
- Use SOCD/Snap Tap (banned in CS2)
- Inject into game process

All writes go to the keyboard's own firmware profile via standard HID protocol.

## Project structure

```
wooting-aim/
├── src/
│   ├── main.c          # Main application (1539 lines)
│   ├── hid_writer.c    # Wooting HID protocol implementation
│   ├── hid_writer.h    # HID protocol header
│   └── hid_enum.c      # HID interface diagnostic tool
├── include/
│   └── wooting-analog-sdk.h   # Wooting SDK header
├── lib/
│   ├── libwooting_analog_sdk.a
│   └── wooting_analog_sdk.dll.lib
├── sdk/                # Full Wooting Analog SDK (docs + binaries)
├── wooting-aim.cfg     # Runtime configuration
├── wooting-aim.exe     # Compiled binary
├── wooting_analog_sdk.dll
├── libhidapi-0.dll
├── Makefile
├── build.bat
└── README.md
```

## Statistics

When `stats_enabled=1`, counter-strafe timings are logged to `wooting-aim-stats.csv`:

```csv
timestamp,axis,direction,duration_ms
2026-02-10 20:01:36,H,D,66.09
2026-02-10 20:01:37,H,A,45.23
```

## Display

While running, the status line shows:
```
[2.6M] A:████████............ D:████████............ [H:C V:I] RIFLE/live A:0.4/0.1 D:0.4/0.1 v:156 GOOD 82ms #5
 │      │                     │                      │    │      │           │           │    │    │     │
 │      │                     │                      │    │      │           │           │    │    │     └ write count
 │      │                     │                      │    │      │           │           │    │    └ time to accurate
 │      │                     │                      │    │      │           │           │    └ strafe quality
 │      │                     │                      │    │      │           │           └ velocity (u/s)
 │      │                     │                      │    │      │           └ D key AP/RT
 │      │                     │                      │    │      └ A key AP/RT
 │      │                     │                      │    └ round phase
 │      │                     │                      └ weapon category
 │      │                     └ D key analog bar
 │      └ A key analog bar
 └ reads/sec
```

## License

Personal use. Wooting Analog SDK is property of Wooting.
