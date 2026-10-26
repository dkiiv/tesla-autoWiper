# Roadmap — tesla-autoWiper

> Status date: 2026-10-26
> **Current verification level: NOTHING verified end-to-end yet.**
> Hardware exists on a breadboard (soldered). Firmware builds and runs on the
> bench. Bench CAN behavior has been exercised, but neither the LIN sensor
> readout nor the CAN wiper override has been proven against real hardware
> in a car.

This file is the ordered work plan. Each phase has an explicit exit
criterion — do not claim a phase done without meeting it. Phases 0–2 can be
done by anyone (including an LLM agent) with just the repo and `./idf.sh
build`. Phases 3+ require bench hardware or the car.

---

## Phase 0 — Correctness & housekeeping (no hardware needed)

Low-risk fixes that make the code honest and the repo approachable.

- [ ] **Remove per-poll log spam in the gateway.**
      `main/can_gateway.c` logs `ESP_LOGW(TAG, "TWAI-x status: ...")` on
      *every* `twai_receive_v2()` call, including the empty-queue
      `ESP_ERR_TIMEOUT` path that fires constantly. At verbose level this
      floods the UART and adds latency inside the hot forwarding loop.
      Delete both lines or guard them behind `DEBUG_CAN_GATEWAY`.
- [ ] **Clamp wiper speed to a valid value.**
      Per opendbc `tesla_model3_vehicle.dbc`, `DAS_wiperSpeed` value
      `15 = DAS_WIPER_SPEED_INVALID`. `wiper_logic.c` writes the raw VAG
      rain nibble (0–15) straight into the frame, so max rain emits INVALID.
      Clamp to `WIPER_SPEED_MAX` (new `config.h` define, start at 14 until
      Phase 3 tells us the real usable max).
- [ ] **Make verbose logging opt-in.**
      `main.c` forces `esp_log_level_set("*", ESP_LOG_VERBOSE)` (the
      "REMOVE BEFORE FLIGHT" line) and `sdkconfig.defaults` pins verbose as
      the default level. Tie both to a single `DEBUG_*` switch in `config.h`
      so a production build is quiet by default.
- [ ] **Fix stale comments/docs that lie about the architecture.**
      `can_gateway.c` header says "4-task architecture" and init logs
      "queues allocated" — actual design is one polling task and no app
      queues (`TX_QUEUE_DEPTH` is defined but unused). Fix the comments or
      delete the dead define. README's file-structure block also calls the
      project `wiper_controller/` while the repo is `tesla-autoWiper`.
- [ ] **Fix DOCKER_DEV.md drift.**
      It says the firmware artifact is `tesla-autowiper.bin`; the CMake
      project is `wiper_controller`, so the artifact is
      `wiper_controller.bin` (which `idf.sh flash` correctly uses). It also
      tells users to `cp .env.example .env` — `.env.example` does not exist;
      add one (just `FLASH_PORT=...` and `IDF_TARGET=esp32p4`).
- [ ] **Add a LICENSE** (pick one; MIT suggested) and a short SAFETY
      disclaimer in the README (this device sits inline on a vehicle CAN
      bus; if it loses power, the bus is severed — see Phase 4).

Exit criterion: `./idf.sh build` compiles clean, docs match reality, no
unconditional logging in the gateway hot path.

## Phase 1 — Host-side unit tests (no hardware needed)

The protocol math is pure-function and testable on any machine. This is the
cheapest confidence available and protects every later refactor.

- [ ] Extract pure logic into testable units (no ESP-IDF includes):
      LIN PID calculation, LIN enhanced checksum, Tesla CAN checksum,
      `encode_wiper_speed` bit packing, and (once it exists) the
      rain→speed mapping policy.
- [ ] Add `test/` with a plain-C host test runner (Unity, or even a single
      `test_main.c` + `cc` — keep it dependency-free so it runs anywhere).
- [ ] Golden vectors to encode:
      - LIN PID: frame 0x21 → protected ID 0x61 (already documented in README).
      - Tesla checksum: must match opendbc `tesla_checksum()`
        (opendbc/car/tesla/teslacan.py) — seed for 0x3E9 is 0xE9+0x03=0xEC,
        sum all bytes except byte 7, truncate to 8 bits.
      - Bit packing: speed goes in the upper nibble of byte 0 only; bytes
        1–6 untouched; counter (byte 6 bits 4–7) passes through unchanged.
- [ ] Optional: a GitHub Actions workflow that runs the host tests plus an
      `idf.sh build` in the espressif/idf container on every push.

Exit criterion: `make test` (or equivalent) passes on a bare Linux host and
covers every checksum/encoding function.

## Phase 2 — Wiper policy (design + code, verifiable on bench)

The current "policy" is `tesla_speed = vag_rain_nibble`, applied whenever
LIN data is valid. That is a placeholder, not a policy.

- [ ] **Respect the driver.** Decide and implement when we are ALLOWED to
      override. The intercepted frame is the AP computer's request; the
      driver's manual settings and washer-spray cycles must win. The TODO at
      `wiper_logic.c` (`wiper_logic_process_can_frame`, sensor-invalid
      branch) marks where pass-through criteria belong. Minimum: if the
      original frame already requests a speed higher than ours, forward it
      unmodified.
- [ ] **Mapping table.** VAG rain intensity (0–15) → Tesla wiper speed via a
      lookup table in `config.h`, not an identity map. Initial guess is
      fine; Phase 3 calibrates it.
- [ ] **Hysteresis / smoothing.** The sensor nibble can flicker. Add
      rise-fast/fall-slow behavior (e.g. speed increases apply immediately,
      decreases require N consecutive seconds of lower intensity). Without
      this, wipers will visibly hunt between speeds.
- [ ] **Stale-data timeout.** `lin_sensor_get_latest()` returns the last
      good reading forever. Add a timestamp; if the last good LIN poll is
      older than ~1 s, treat the sensor as invalid and pass through.
- [ ] **Review LIN echo handling.** `lin_send_header()` flushes the RX FIFO
      after the header TX completes; a fast slave could already be
      responding, and the flush would eat the first response bytes. Safer:
      read back exactly break+sync+PID echo bytes and verify them instead
      of flushing blind.
- [ ] **Gateway architecture decision.** Current design: one task, zero
      timeout polls, 1 ms / 25 ms sleeps, 32-deep driver RX queues. At
      1 Mbit/s a busy automotive bus can push ~8k frames/s — a 32-frame
      queue fills in ~4 ms. Either justify the polling numbers with a bench
      flood test or switch to two blocking-receive tasks (one per
      controller), which eliminates the sleep tuning entirely.

Exit criterion: documented policy in code comments + README, unit tests for
the mapping/hysteresis, bench run showing stable speed transitions with a
hand-wetted sensor.

## Phase 3 — Vehicle validation (requires car access, go slow)

Order matters: each step is safe only if the previous one passed.

1. [ ] **Sniff first, inject nothing.** Tap the harness, log real
       `0x3E9 DAS_bodyControls` traffic (rate, counter behavior, checksum)
       and verify our checksum function reproduces every observed frame's
       byte 7. Record wiper frames for each manual speed setting and in
       auto mode. Save dumps to `docs/captures/` (gitignore raw logs if
       they're huge; keep summaries).
2. [ ] **Pure passthrough soak.** Install the ESP inline with wiper logic
       disabled (sensor unplugged). Drive. Confirm zero functional
       difference, no bus errors, no UI warnings, over multiple ignition
       cycles. This validates the MITM wiring + gateway throughput.
3. [ ] **Verify sensor fitment.** Confirm the VAG sensor reads believable
       rain/light values when mounted on Tesla glass (glass thickness and
       optical coupling gel matter for these sensors).
4. [ ] **First override.** Enable wiper logic, parked, washer-fluid test.
       Confirm the car acts on our modified speed and that the body
       controller accepts recomputed checksums with the passed-through
       counter. THEN road test in real rain.
5. [ ] **Calibrate the Phase 2 mapping table** against perceived wipe
       appropriateness; document the final table and what each Tesla speed
       value (1–14) actually does — this is currently only partially known
       (values came from opendbc DBC, behavior unobserved).

Exit criterion: video/log evidence of rain-driven wiper control in the car,
calibrated mapping committed.

## Phase 4 — Robustness & fail-safety (before daily-driving it)

- [ ] **Hard fail-safe.** This device physically severs the CAN path: if
      the ESP crashes or loses power, wipers stop responding to the car.
      Options, pick one: normally-closed relay that bridges the two buses
      when the ESP is unpowered/faulted, or document loudly that pulling
      the module restores stock wiring. A software watchdog is NOT enough.
- [ ] **TWAI bus-off recovery.** Neither controller currently handles
      bus-off / error-passive states; add `twai_read_alerts_v2()` handling
      with automatic `twai_initiate_recovery_v2()` and a counter in the
      health log.
- [ ] **Task watchdog** on the gateway task (esp_task_wdt) so a wedged loop
      reboots into passthrough rather than silence.
- [ ] **Brownout/ignition behavior.** Verify boot time from 12 V key-on to
      passthrough-active; if it's slow, frames are lost at every ignition —
      measure and document.
- [ ] **Health counters.** Extend health_task: frames forwarded per side,
      TX drops, LIN timeout rate, uptime. One log line every 5 s.

Exit criterion: documented failure-mode table (power loss, crash, bus-off,
sensor loss) with observed behavior for each.

## Phase 5 — Nice-to-haves (unordered backlog)

- [ ] Use the sensor's light channel (byte 1) — e.g. feed
      DAS_headlightRequest, or just log it. (Car already has auto
      headlights; probably log-only.)
- [ ] Runtime config (NVS or serial console) for mapping/hysteresis so
      tuning doesn't require reflashing.
- [ ] Wi-Fi/BLE debug telemetry (ESP32-P4 needs a companion chip for RF —
      check the actual dev board before promising this).
- [ ] PCB design to replace the breadboard (document the breadboard wiring
      in `docs/hardware.md` first, with photos).
- [ ] CI: build + host tests on every push (see Phase 1).

---

## Known-bugs quick index

| # | Where | Problem |
|---|-------|---------|
| 1 | `can_gateway.c` RX loops | Unconditional ESP_LOGW on every poll incl. timeouts — log flood in hot path |
| 2 | `main.c` app_main | Forced global VERBOSE logging ("REMOVE BEFORE FLIGHT") |
| 3 | `wiper_logic.c` | Rain nibble 15 → DAS_WIPER_SPEED_INVALID sent to car (no clamp) |
| 4 | `wiper_logic.c` | Overrides unconditionally when LIN valid — no manual/washer override respect, no hysteresis |
| 5 | `lin_sensor.c` | No stale-data timeout; last reading lives forever |
| 6 | `lin_sensor.c` `lin_send_header` | Blind RX flush can race a fast slave response |
| 7 | `can_gateway.c/.h` | Stale "4-task / queues" comments; unused `TX_QUEUE_DEPTH`; polling+sleep design unproven at bus load |
| 8 | `DOCKER_DEV.md` | Wrong artifact name (`tesla-autowiper.bin` → `wiper_controller.bin`); references missing `.env.example` |
| 9 | repo root | No LICENSE, no safety disclaimer |
