# Roadmap — tesla-autoWiper

> Status date: 2026-10-26
> **Production target: ESP32-C6.** The bench board is an ESP32-P4, which is
> still supported.
> **Verification level:** the firmware builds for both chips, and the protocol
> and policy logic are unit-tested. **Nothing has been verified on hardware or
> in the car yet.**

This is the ordered work plan. Every phase has an exit criterion, and a phase
isn't done until that criterion is met. Phases 0–2 need no hardware. Phases 3
and later need the bench or the car.

---

## Phase 0: Correctness & housekeeping ✅ DONE (2026-10-26)

- [x] Removed the per-poll `ESP_LOGW` flood from the CAN hot path. The gateway
      now uses counters, which are reported in the `[HEALTH]` log.
- [x] Wiper speed is clamped to 14, so `DAS_WIPER_SPEED_INVALID` (15) can't be
      emitted (`tesla_wiper_speed_set`).
- [x] Logging is opt-in. `APP_LOG_LEVEL` defaults to INFO, and the forced
      VERBOSE level ("REMOVE BEFORE FLIGHT") is gone.
- [x] Fixed stale architecture comments and removed the dead
      `TX_QUEUE_DEPTH`.
- [x] Fixed DOCKER_DEV.md: correct artifact name, `.env.example` added, and
      idf.sh now loads `.env`.
- [x] Added a safety disclaimer to the README.
- [x] **Retargeted to ESP32-C6.** Pins are selected per chip in config.h.
      idf.sh, compose and the devcontainer default to `esp32c6`. `flash` reads
      the chip and offsets from the build, because the bootloader offset
      differs between the C6 (0x0) and the P4 (0x2000).
- [ ] **LICENSE: owner decision needed** (MIT suggested). Not added without
      the owner's sign-off.

## Phase 1: Host-side unit tests ✅ DONE (2026-10-26)

- [x] Pure logic moved into `main/protocol.c` (LIN PID/checksum, Tesla
      checksum, wiper-speed get/set) and `main/wiper_policy.c`. Neither has an
      ESP-IDF dependency.
- [x] `test/`: a dependency-free C runner and Makefile, with ASan and UBSan
      enabled. About 4,100 checks, including randomized sweeps against
      independent reference implementations of the LIN spec and opendbc's
      `tesla_checksum()`.
- [x] A mutation check showed the tests catch a broken clamp and a broken
      checksum carry.
- [x] GitHub Actions CI runs the host tests and builds `esp32c6` and
      `esp32p4`, uploading the firmware as artifacts.

## Phase 2: Wiper policy & gateway robustness ✅ CODE DONE, ❌ bench-unverified

- [x] **Mapping table** `WIPER_RAIN_TO_SPEED_MAP` in config.h. It is an
      initial guess that roughly matches the rain level one-to-one.
- [x] **Hysteresis:** speed rises immediately. It falls only after the lower
      target has held for `WIPER_FALL_HOLD_MS` (3 s), and then drops to the
      highest target seen during that hold.
- [x] **Stale-data timeout:** readings carry a timestamp. If the reading is
      older than `LIN_STALE_MS` (500 ms), the device falls back to passthrough
      and the policy state resets.
- [x] **The car wins:** the output is `max(oem, ours)`. A frame the car marked
      SNA (15) is left untouched, and so is a frame whose length isn't 8.
- [x] **LIN echo handling:** the driver now reads and verifies the echoed
      sync+PID instead of blindly flushing them. It counts timeouts, checksum
      errors and echo errors separately.
- [x] **Gateway redesign:** one blocking-RX task per direction, so there's no
      sleep tuning. Each task is registered with the task watchdog (3 s,
      panic → reboot). An alert task does bus-off recovery. Queues are 64
      deep, and the TWAI ISR runs in IRAM. The tick rate is 1 kHz so LIN
      timeouts are accurate.
- [ ] **Bench run** to close out this phase. Exit criterion: on the P4 bench
      (or a C6 dev board), with the sensor connected and hand-wetted, the
      health log shows `LIN OK` and speed transitions are stable. A second CAN
      node replaying 0x3E9 frames should show `modified` going up, and a
      receiving node should confirm the rewritten frames have valid checksums.

### Bench procedure for the human (Phase 2 exit)
1. Flash the bench board: `IDF_TARGET=esp32p4 ./idf.sh build && ./idf.sh flash monitor`.
2. **Sensor only:** confirm the `[HEALTH] LIN OK` line appears. If it says
   `--`, check the counters:
   - `to` going up: no response. Check wiring, 12 V to the sensor, and the
     frame ID.
   - `echo` going up: the transceiver RXD isn't seeing our TX. Check the wiring.
   - `cs` going up: the checksum type may be classic rather than enhanced.
     That would be a code change.
   Set `DEBUG_LIN_SENSOR 1` and wet the sensor to confirm byte 0 bits [3:0]
   track the rain.
3. **CAN:** use a USB-CAN adapter (or a second ESP) on each side. Send 0x3E9
   on the car side and confirm it arrives on the wiper side with speed ≥ our
   target and a valid byte 7. Send other IDs and confirm they arrive
   byte-for-byte identical in both directions. Watch the `drop`/`miss`/`off`
   counters under a flood test (`cangen` at full rate).

## Phase 3: Vehicle validation (requires the car, go slow)

Each step is only safe if the previous one passed.

1. [ ] **Sniff first, inject nothing.** Find the harness point that carries
       0x3E9 between the AP computer and the body controller. **Measure the
       bus bitrate** (500 kbit/s expected) and set `CAN_BAUD_RATE`. Log real
       0x3E9 traffic: its rate, how the counter behaves, and the checksum.
       Confirm `tesla_checksum` reproduces byte 7 for every frame. If it
       does, add a few real frames to `test/test_main.c` as golden vectors.
       Record the wiper frames for each manual stalk speed and in auto mode,
       to learn whether the body controller ignores DAS_wiperSpeed outside
       auto mode. Save summaries to `docs/captures/`.
2. [ ] **Pure passthrough soak.** Install the ESP32-C6 inline with the sensor
       unplugged. Drive, and confirm there is zero functional difference, no
       bus errors, and no UI warnings across several ignition cycles. The
       health counters should show `drop=0 miss=0 off=0`.
3. [ ] **Check the sensor on Tesla glass.** Glass thickness and the coupling
       gel affect optical rain sensors.
4. [ ] **First override.** Park and use washer fluid. Confirm the car acts on
       our speed and accepts the recomputed checksum with the passed-through
       counter. Only then do a road test in real rain.
5. [ ] **Calibrate** `WIPER_RAIN_TO_SPEED_MAP` and `WIPER_FALL_HOLD_MS`.
       Document what each Tesla speed value (1-14) actually does.

## Phase 4: Fail-safety (before daily driving)

- [ ] **Hardware bypass.** The ESP physically breaks the CAN path. Add a
      normally-closed relay (or an analog switch) that bridges the two buses
      whenever the ESP is unpowered or not asserting a "healthy" GPIO.
      Software can't cover a dead chip. This also covers the boot window and
      the reboot after a watchdog reset.
- [x] Task watchdog on the forwarding tasks (done in Phase 2).
- [x] Bus-off recovery (done in Phase 2).
- [ ] Measure the time from key-on 12 V to passthrough being active, and
      document it.
- [ ] Failure-mode table with observed behavior for: power loss, crash/WDT,
      bus-off on either side, sensor unplugged mid-rain, and sensor giving
      garbage.

## Phase 5: Backlog (unordered)

- [ ] Use the light channel (byte 1). At minimum, log it.
- [ ] Runtime tuning through NVS or a serial console, so the map and
      hysteresis can be changed without reflashing.
- [ ] C6-native extras: it has Wi-Fi 6 and BLE built in, so a BLE or Wi-Fi
      debug/telemetry page is possible (the P4 has no radio).
- [ ] PCB for the C6 module. Document the breadboard first in
      `docs/hardware.md`, with photos.
- [ ] Optional hardware filter: only ever take 0x3E9 out of the fast path.
      This only matters if the bench flood test shows drops.

---

## Known issues

| # | Where | Issue | Status |
|---|-------|-------|--------|
| 1 | can_gateway.c | Log flood in RX hot path | ✅ fixed |
| 2 | main.c | Forced VERBOSE logging | ✅ fixed |
| 3 | wiper_logic.c | Could emit speed 15 (INVALID) | ✅ fixed (clamp + test) |
| 4 | wiper_logic.c | Unconditional override, no hysteresis | ✅ fixed (wiper_policy) |
| 5 | lin_sensor.c | Last reading lived forever | ✅ fixed (timestamp + LIN_STALE_MS) |
| 6 | lin_sensor.c | Blind RX flush could eat response | ✅ fixed (read + verify echo) |
| 7 | can_gateway | Stale comments, polling design | ✅ fixed (per-direction blocking tasks) |
| 8 | DOCKER_DEV.md | Wrong artifact, missing .env.example | ✅ fixed |
| 9 | repo root | No LICENSE | ⏳ owner decision |
| 10 | config.h | CAN bitrate 1 Mbit/s, car is likely 500 kbit/s | ⚠️ open: confirm in Phase 3.1 |
| 11 | config.h | C6 pins untested | ⚠️ open: bench check on a C6 board |
| 12 | lin_sensor.c | Assumes the transceiver echoes TX (`LIN_EXPECT_ECHO=1`); a non-echo setup times out | ⚠️ by design, switch in config.h |
| 13 | wiper_policy | Unknown whether the body controller ignores DAS_wiperSpeed when the stalk isn't in auto | ⚠️ open: Phase 3.1 captures |
| 14 | lin_sensor.c | `uart_set_baudrate` mid-frame for the break: timing on the C6 UART is unverified | ⚠️ open: scope it on the bench |
