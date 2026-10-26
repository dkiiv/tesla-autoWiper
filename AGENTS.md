# AGENTS.md: orientation for LLM agents and new contributors

Read this before you touch any code. It covers what the project is, what has
and hasn't been verified, how to build and test, and the rules that keep
changes safe. The ordered work plan is in **ROADMAP.md**. Work through it from
the top.

## What this is

This is firmware for an ESP32 that gives a Tesla Model 3/Y proper rain-sensing
wipers. Tesla picks the wiper speed with a camera and a neural net, and it
often gets it wrong. This device:

1. Reads a real **VAG (VW/Audi) rain/light sensor** over **LIN bus**. The ESP
   is the LIN master and the sensor answers on frame ID 0x21.
2. Sits **inline (man-in-the-middle)** on the CAN link that carries the
   Autopilot computer's `DAS_bodyControls` frame (ID **0x3E9**). The car-side
   bus is on TWAI0 and the actuator side is on TWAI1. Every frame is forwarded
   in both directions.
3. For car→actuator 0x3E9 frames only, the wiper policy **replaces**
   `DAS_wiperSpeed` with its own decision from the rain sensor, raising or
   lowering it (including forcing 0 to suppress Tesla's phantom wipes on dry
   glass). The Tesla checksum is recomputed before the frame is forwarded.

If the sensor is missing or its data is stale, the device falls back to pure
passthrough and the stock behavior is unchanged.

## Target hardware

- **ESP32-C6: production target and default build** (`IDF_TARGET=esp32c6`).
  It was chosen because it is easy to source. It has 2 TWAI controllers and
  UART1 free for LIN.
- **ESP32-P4: the current bench breadboard.** It still builds
  (`IDF_TARGET=esp32p4`). It is not the production target because several
  incompatible P4 hardware revisions are in circulation, which makes sourcing
  unreliable.
- Pins are chosen per chip with `#if CONFIG_IDF_TARGET_*` in `main/config.h`.
  The C6 pins are a first pick and haven't been tested on hardware.
- **Every change must build for BOTH targets.** CI enforces this.

## Verification state (be honest about this)

| Area | State |
|---|---|
| Firmware compiles, C6 + P4 | ✅ verified (local native IDF 5.3.2 build, 2026-10-26; CI on every push) |
| LIN PID / checksum, Tesla checksum, bit packing | ✅ unit-tested against independent reference implementations (opendbc / LIN spec) |
| Wiper policy logic (map, hysteresis, stale, authoritative override) | ✅ unit-tested |
| C6 pin choice | ❌ untested on hardware |
| LIN reads from the real sensor (byte map, echo behavior) | ❌ unverified |
| CAN bitrate (1 Mbit/s set; Tesla likely 500 kbit/s) | ❌ unverified, **must confirm before car install** |
| In-car passthrough / override acceptance / speed meaning | ❌ unverified |

Rules for agents:

- Never describe a feature as "working" without bench or car evidence.
  "Compiles" and "unit-tested" are not the same as "works on hardware". Keep
  the table above accurate.
- You can verify compilation (`./idf.sh build` for both targets) and logic
  (`./idf.sh test` or `make -C test`). You cannot verify hardware behavior. Say
  so, and leave the human a concrete bench or car test procedure instead.

## Repo map

```
main/
  config.h          ALL tunables: per-chip pins, CAN bitrate/queues, LIN settings,
                    wiper map + hysteresis, task priorities, debug switches.
  main.c            app_main: init order, task spawn, [HEALTH] log (3 lines / 5 s)
  can_gateway.c/.h  Two forwarding tasks (one per direction, blocking RX, watchdog-fed)
                    + one alert task for bus-off recovery. Counters for health log.
  lin_sensor.c/.h   LIN master: break via baud-halving, reads + verifies header echo,
                    enhanced checksum, timestamped latest reading, poll counters.
  wiper_logic.c/.h  Glue: for 0x3E9 → get sensor + age → wiper_policy_step → rewrite.
  wiper_policy.c/.h PURE C. Decision rules (see header). Host-tested.
  protocol.c/.h     PURE C. LIN PID/checksum, Tesla checksum, DAS_wiperSpeed get/set
                    (set clamps to 14). Host-tested.
test/               Plain-C test runner + Makefile. No ESP-IDF. `make -C test`.
.github/workflows/ci.yml   host tests + build matrix [esp32c6, esp32p4]
idf.sh, Dockerfile, docker-compose.yml   containerized build; flash/monitor on host
```

Data flow: TWAI0 RX → `can_fwd_task(car→act)` → `wiper_logic_process_can_frame`
→ (maybe `tesla_wiper_speed_set`) → TWAI1 TX. The reverse direction is blind
passthrough.

**The pure-C rule:** `protocol.c` and `wiper_policy.c` must not include any
ESP-IDF or FreeRTOS headers, so they stay testable on a host. New logic that
can be pure goes there, with tests. Driver code stays thin.

## Build / flash / test

```bash
./idf.sh build                       # esp32c6 → ./firmware/
IDF_TARGET=esp32p4 ./idf.sh build    # bench board
./idf.sh flash                       # chip + offsets read from build output
./idf.sh monitor
./idf.sh test                        # host unit tests in the container
make -C test                         # same, if you have cc + make locally
```

Native (no Docker): ESP-IDF **v5.3.2** with **Python ≤ 3.12**, since IDF 5.3
rejects 3.13+. Run `idf.py -B build-c6 set-target esp32c6 build`.

The minimum bar for every commit: tests pass, and both targets build with zero
warnings from `main/`.

## Ground truth & external references

- **Tesla DBC**: commaai/opendbc `opendbc/dbc/tesla_model3_vehicle.dbc`,
  `BO_ 1001 DAS_bodyControls` (1001 == 0x3E9):
  - `DAS_wiperSpeed : 4|4@1+`: upper nibble of byte 0. 0 = OFF, 1-14 =
    speeds, **15 = DAS_WIPER_SPEED_INVALID**. `tesla_wiper_speed_set()`
    clamps to 14.
  - `DAS_bodyControlsCounter : 52|4@1+`: byte 6, upper nibble. Passed
    through unchanged on purpose.
  - `DAS_bodyControlsChecksum : 56|8@1+`: byte 7.
- **Checksum**: matches opendbc `tesla_checksum()` (opendbc/car/tesla/
  teslacan.py). The unit tests compare against a transliteration of it. Not
  yet checked against live car frames (ROADMAP Phase 3, step 1).
- **LIN**: the sensor byte map (rain = lower nibble of byte 0, light = byte 1,
  status = byte 2) comes from community VAG reverse-engineering. The owner
  sourced it from an existing VW rain/light sensor LIN project. Treat it as a
  hypothesis until a bench read confirms it.

## Conventions

- **All tuning constants go in `main/config.h`**, with a comment that says
  what the value means and when to change it.
- Comment style: a file-level `@file/@brief` header, boxed section dividers
  (`/* ─── name ─── */`), and comments that explain *why*. When behavior
  changes, update the comments and diagrams in the same commit.
- Logging: `ESP_LOGx` with a per-file TAG. **Nothing unconditional in hot
  paths** (CAN forwarding, LIN poll). Use counters and the health log instead.
  Per-frame logging must be behind a `DEBUG_*` switch.
- Commits: small, with an imperative subject line in the style of the existing
  history. Author: dkiiv <curtis.x.jenkins@gmail.com>.
- Docs: README is the human intro, ROADMAP.md is the plan, and this file is
  agent orientation. Update all three when reality changes.
- License: MIT (see LICENSE).

## Safety rules (non-negotiable)

1. **Passthrough comes first.** Any failure (dead LIN, stale data, parse
   error, unexpected DLC) must fall back to forwarding the original frames.
   Never block or drop the car↔actuator path because of sensor state.
2. **Never emit invalid signal values** onto the vehicle bus. Clamp
   everything. Speed 15 is impossible by construction.
3. **Only touch 0x3E9**, and within it only byte 0 bits [7:4] and byte 7. The
   counter passes through on purpose.
4. **The module is authoritative over Tesla's auto-wiper.** While the sensor
   is healthy, our speed replaces the AP computer's request, both up and
   down. This is an owner decision and the reason the project exists: Tesla's
   camera wipes when it shouldn't. Do NOT reintroduce "car wins" or `max()`
   logic. The **driver** (manual stalk, washer) must still win. It is
   unverified whether the body controller ignores 0x3E9 in those modes; if it
   doesn't, detecting them is required (ROADMAP Phase 3.1, issue #13).
5. This is a safety device on a moving vehicle, and its failure mode is no
   wipers in the rain. When in doubt, choose the fail-safe option.

## Current known bugs / open issues

See the "Known issues" table at the bottom of ROADMAP.md. If you find a new one
and can't fix it in the same change, add it there.
