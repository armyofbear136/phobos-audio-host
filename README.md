# PhobosHost

Headless JUCE-based VST3 host for the PHOBOS DAW.

Replaces Carla as the plugin host for the audio subsystem. Communicates
with the PHOBOS backend over loopback TCP (control) and UDP (MIDI/OSC).

**Authoritative spec:** `PHOBOS-PhobosHost-Spec.md` (in the documentation
repo, under `MASTER_SYSTEM_REFERENCES/PHOBOS_CORE_SYSTEMS/Audio/`).

This repo is the C++ source. Pre-built binaries are distributed via GitHub
release artifacts and fetched into `~/.phobos/services/host/PhobosHost(.exe)`
by `phobos-core/scripts/fetch-phobos-host.js`.

## Layout

```
phobos-host/
├── CMakeLists.txt
├── JUCE/                ← clone of github.com/juce-framework/JUCE (vendored, not committed)
├── Source/
│   ├── Main.cpp         ← entry, audio device, graph init, drain timer
│   ├── ControlServer.*  ← TCP/16332, length-prefixed JSON RPC
│   ├── OscServer.*      ← UDP/16331, OSC decoder, MIDI dispatch
│   └── Logger.*         ← lock-free SPSC ring → stderr + TCP `log` events
├── build.bat            ← Windows: VS2022 + NMake + CMake (Release)
└── build.sh             ← macOS / Linux
```

JUCE 8 is **vendored as a sibling folder** — clone
`https://github.com/juce-framework/JUCE` into `./JUCE/` before building.
Same approach as `phobos-crystal`. The `JUCE/` folder is gitignored.

## Build

### Windows

Requires Visual Studio 2022 Community (or Build Tools), CMake ≥ 3.22.

```cmd
build.bat
```

Output: `build_nmake\PhobosHost_artefacts\Release\PhobosHost.exe`.

### macOS / Linux

Requires CMake ≥ 3.22, a C++20 compiler, and the platform's audio
development headers (CoreAudio on macOS — included; ALSA on Linux —
`libasound2-dev`).

```bash
./build.sh
```

Output: `build/PhobosHost_artefacts/Release/PhobosHost`.

## Run

```
./PhobosHost
```

No CLI arguments. The binary:

1. Opens the system default audio device (WASAPI on Windows, CoreAudio on
   macOS, ALSA on Linux). Plays silence — Session 1 has no plugins yet.
2. Listens on `127.0.0.1:16332/TCP` for control RPC.
3. Listens on `127.0.0.1:16331/UDP` for MIDI/OSC events.

Logs go to stderr (`[INFO]`, `[WARN]`, `[ERR]` prefixes) and, when a
control client is connected, to that client as `log` events.

## Wire protocol

### Control (TCP/16332)

Length-prefixed JSON. Each frame:

```
[4 bytes — uint32 BE — body length]
[body — UTF-8 JSON]
```

Request shape: `{ "id": <int>, "op": "<name>", "args": {...} }`
Response:      `{ "id": <int>, "ok": true,  "result": {...} }`
Error:         `{ "id": <int>, "ok": false, "error": "..."  }`
Event:         `{ "evt": "<name>", ... }` (server-initiated, no `id`)

### Notes (UDP/16331)

OSC. Addresses:

```
/phobos/note_on   i i i i   slotId, midiChannel, note, velocity
/phobos/note_off  i i i     slotId, midiChannel, note
/phobos/cc        i i i i   slotId, midiChannel, controller, value
```

In Session 1, `slotId` is logged but not yet routed (no plugins exist).

## Session 1 op table

| Op       | Args | Result                          |
| -------- | ---- | ------------------------------- |
| ping     | `{}` | `{ uptimeMs, version }`         |
| shutdown | `{}` | `{}` — process exits cleanly    |

The full op set (loadPlugin, getPluginState, showPluginUi, etc.) lands
across Sessions 2–3. See spec §3.3.

## Smoke test

A standalone Node script at `phobos-core/test-phobos-host.ts` exercises
the wire surface — opens TCP, sends `ping`, sends an OSC packet, asserts
the corresponding `log` event arrives. Run from `phobos-core`:

```
tsx test-phobos-host.ts
```

Expected output: `OK` and exit 0.
