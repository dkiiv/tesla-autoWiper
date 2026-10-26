# AGENTS.md — orientation for LLM agents and new contributors

Read this before touching code. It tells you what this project is, what is
and isn't verified, how to build, and the rules that keep changes safe.
The ordered work plan lives in **ROADMAP.md** — work top-down from there.

## What this is

Firmware for an **ESP32-P4** that gives a Tesla Model 3/Y proper rain-sensing
wipers. Tesla decides wiper speed with a camera + neural net (badly). This
device:

1. Reads a real **VAG (VW/Audi) rain/light sensor** over **LIN bus**
   (ESP is LIN master, sensor frame ID 0x21).
2. Sits **inline (MITM)** on the CAN link carrying the Autopilot computer's
   `DAS_bodyControls` frame (ID **0x3E9**): car-side bus on TWAI0, actuator
   side on TWAI1, forwarding everything in both directions.
3. For 0x3E9 only, rewrites the `DAS_wiperSpeed` nibble from the rain
   sensor's intensity and recomputes the Tesla checksum, then forwards.

Everything else passes through untouched. If the LIN sensor is dead, the
gateway degrades to pure passthrough — the stock stalk/UI keeps working.

## Verification state (be honest about this)

As of 2026-10-26: **nothing is verified end-to-end.** Breadboard is
soldered; firmware builds; bench CAN behavior has been exercised. Not yet
proven: real sensor reads on Tesla glass, in-car passthrough, in-car
override acceptance, the rain→speed mapping.

Rules for agents:

- Never describe a feature as "working" unless there is bench/car evidence.
  "Compiles" ≠ "works". Keep README/ROADMAP claims aligned with reality.
- You can verify **compilation** yourself: `./idf.sh build` (needs Docker).
  You can verify **protocol math** with host unit tests (ROADMAP Phase 1).
  You cannot verify hardware behavior — say so, and leave the human a
  concrete bench/car test procedure instead.

## Repo map

```
CMakeLists.txt            ESP-IDF project (project name: wiper_controller)
sdkconfig.defaults        Build-time Kconfig defaults (currently forces verbose logs)
Dockerfile, docker-compose.yml, idf.sh, DOCKER_DEV.md
                          Containerized build/flash/monitor tooling (no local IDF needed)
main/
  config.h                ALL tuning lives here: pins, CAN ID, baud, poll rate,
                          task priorities, debug switches. New knobs go here.
  main.c                  app_main: init subsystems, spawn 3 tasks, health logger
  lin_sensor.c/.h         LIN master driver. Break-field via baud-halving trick
                          (adapted from mestrode/Lin-Interface-Library). PID +
                          enhanced-checksum helpers. Latest reading is shared
                          under a portMUX critical section.
  can_gateway.c/.h        Dual-TWAI forwarding loop (twai *_v2 multi-controller
                          API). Calls wiper_logic on matching car→actuator frames.
  wiper_logic.c/.h        0x3E9 frame rewrite: speed nibble + Tesla checksum.
```

Flow: `can_gateway_task` RX car frame → if ID==0x3E9 →
`wiper_logic_process_can_frame()` (reads `lin_sensor_get_latest()`,
rewrites bytes 0 and 7) → TX to actuator bus. Reverse direction is blind
passthrough.

## Build / flash / test

```bash
./idf.sh build        # docker build of firmware → ./firmware/wiper_controller.bin
./idf.sh flash        # esptool on the HOST (not container), port from FLASH_PORT
./idf.sh monitor      # pyserial miniterm on host
./idf.sh shell        # bash inside the IDF container
```

- Target: `esp32p4` (set via IDF_TARGET; IDF v5.3.2 pinned in Dockerfile).
- Build artifacts land in a Docker named volume, copied to `./firmware/`.
- There is no `idf.sh test` yet — adding host-side unit tests is ROADMAP
  Phase 1 and the highest-value no-hardware contribution available.
- A successful build is the minimum bar for ANY code change you commit.

## Ground truth & external references

- **Tesla DBC**: commaai/opendbc `opendbc/dbc/tesla_model3_vehicle.dbc`,
  message `BO_ 1001 DAS_bodyControls` (1001 == 0x3E9):
  - `DAS_wiperSpeed : 4|4@1+` → upper nibble of byte 0; values 1–14 are
    speeds, 0 = OFF, **15 = DAS_WIPER_SPEED_INVALID** (do not emit 15).
  - `DAS_bodyControlsCounter : 52|4@1+` (byte 6 high nibble) — we pass it
    through unchanged, deliberately.
  - `DAS_bodyControlsChecksum : 56|8@1+` (byte 7).
- **Checksum**: `compute_checksum()` in wiper_logic.c matches opendbc's
  `tesla_checksum()` (opendbc/car/tesla/teslacan.py): sum of both address
  bytes + all data bytes except the checksum byte, & 0xFF. Verified by
  code comparison 2026-10-26; not yet verified against live car frames
  (ROADMAP Phase 3 step 1).
- **LIN**: break-field workaround documented in README and lin_sensor.c
  header comment; sensor byte map (rain nibble byte 0, light byte 1,
  status byte 2) came from community VAG reverse-engineering and is
  UNVERIFIED against our actual sensor — treat as hypothesis until a bench
  read succeeds.

## Conventions

- **All tuning constants go in `main/config.h`** with a comment saying what
  the value means and when to change it. No magic numbers in .c files.
- Comment style: file-level `@file/@brief` header, boxed section dividers
  (`/* ─── name ─── */`), explain *why* not *what*. Keep diagrams in
  comments up to date when behavior changes — stale comments have already
  bitten this repo (see ROADMAP bug #7).
- Logging: `ESP_LOGx` with per-file TAG. Nothing unconditional in hot
  paths (gateway RX loop, LIN poll) — gate with `DEBUG_*` from config.h.
- Commits: small, imperative subject line, matching existing history
  ("lower timeout for can gateway from 50ms to 25ms"). Author:
  dkiiv <curtis.x.jenkins@gmail.com>.
- Docs: README is the human intro; ROADMAP.md is the plan; this file is
  agent orientation. Update all three when reality changes.

## Safety rules (non-negotiable)

1. **Passthrough is sacred.** Any failure (LIN dead, parse error, logic
   bug) must degrade to forwarding original frames. Never block or drop
   the car↔actuator path based on sensor state.
2. **Never emit invalid signal values** onto the vehicle bus (e.g. wiper
   speed 15). Clamp everything.
3. **Don't touch frames other than 0x3E9**, and within 0x3E9 only bytes
   0 (upper nibble) and 7. The counter passes through on purpose.
4. **The driver wins.** Manual stalk presses and washer cycles must not be
   fought by this device (policy work: ROADMAP Phase 2).
5. This is a moving-vehicle safety device with a wet-weather failure mode
   (no wipers in rain = can't see). Bias every decision toward fail-safe.

## Current known bugs

See the "Known-bugs quick index" table at the bottom of ROADMAP.md —
Phase 0 fixes most of them. If you discover a new one and can't fix it in
the same change, add it to that table.
