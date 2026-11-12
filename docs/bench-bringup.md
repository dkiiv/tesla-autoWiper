# Bench bring-up and pre-install checklist

> **Status: this procedure has not been run end to end yet.** It is the plan
> for closing ROADMAP Phase 2 on the bench. Log output shown here is the
> *format* the firmware prints, not a capture from real hardware. If
> something behaves differently, write it down and update this file.

Wiring requirements are in [hardware.md](hardware.md). Work through these
stages in order, and don't move on until the current one passes.

1. [Build and flash](#1-build-and-flash)
2. [Sensor-only bring-up (no CAN)](#2-sensor-only-bring-up-no-can)
3. [CAN replay rig](#3-can-replay-rig)
4. [Pre-install checklist (gates the car)](#4-pre-install-checklist-gates-the-car)

---

## 1. Build and flash

The build has **no default CAN bitrate**. On the bench, pick the rate your
USB-CAN adapters will use. 500 kbit/s matches what the car is *expected* to
use (unconfirmed), so it's a sensible bench choice:

```bash
CAN_BITRATE_KBPS=500 IDF_TARGET=esp32p4 ./idf.sh build   # bench board
CAN_BITRATE_KBPS=500 ./idf.sh build                      # ESP32-C6
./idf.sh flash monitor
```

The boot log prints the compiled-in rate and policy mode. Check both:

```
I (...) CAN_GW: CAN bitrate 500 kbit/s (both controllers)
I (...) WIPER: Wiper policy ready  fall_hold=3000ms  stale=500ms  mode=authoritative  fault_mask=0x00
```

## 2. Sensor-only bring-up (no CAN)

Goal: `[HEALTH] LIN OK` and rain values that track a wet sensor.

Setup:

1. 12 V bench supply **with a current limit** (start low, e.g. a few hundred
   mA, then raise it if the sensor needs more; record the real draw in
   hardware.md §6).
2. Sensor powered from 12 V and ground. The pinout is **TODO**; measure it
   for your sensor and record it. Don't guess.
3. LIN transceiver: VBAT on 12 V, **1 kΩ + series diode master pull-up** on
   the LIN line, enable pin high (`SLP_N` on TJA1021, `CS` on MCP2003B),
   **pull-up on RXD** (open-drain on both of those parts), TXD/RXD to the
   ESP pins in `config.h`.
4. Common ground between the ESP, the transceiver and the sensor.
5. Set `DEBUG_LIN_SENSOR 1` in `main/config.h` for this stage. CAN does not
   need to be wired. The CAN tasks just idle and feed the watchdog.

Expected health output every 5 s once the sensor answers (format only):

```
[HEALTH] LIN OK  rain=0 light=120 st=0x00 age=12ms ok/to/cs/echo/flt=100/0/0/0/0
[HEALTH] CAN car→act rx=0 drop=0 miss=0 off=0 | act→car rx=0 drop=0 miss=0 off=0
[HEALTH] WIPER 0x3E9 seen=0 modified=0 badDLC=0 fault=0 oem=0 out=0 mode=auth
```

`ok` should climb by about 100 per 5 s (20 Hz polling). If the line says
`LIN --`:

| Counter rising | Likely cause |
|---|---|
| `to` (timeout) | No response. Check 12 V to the sensor, the master pull-up, the transceiver enable pin, the RXD pull-up, the frame ID (`LIN_RAIN_SENSOR_FRAME_ID`), and whether this sensor needs a master frame (configuration or status) before it answers (**unknown**, see hardware.md §6). |
| `echo` | RXD isn't seeing our own TX. Check the RXD pull-up and wiring. On a setup without echo, set `LIN_EXPECT_ECHO 0`. |
| `cs` | The response arrives but the checksum doesn't match. The sensor may use the classic checksum or a different payload length. That needs a code change, so capture raw bytes first. |
| nothing at all, `ok` = 0 | Check the UART pins and that `LIN init` didn't fail at boot. |

Then, with `DEBUG_LIN_SENSOR 1`:

1. Mount the sensor on a piece of glass with its gel pad (optical coupling
   matters. A sensor in free air may read nothing useful).
2. Record raw bytes dry, misted, wet and soaked. Confirm byte 0 bits [3:0]
   track the water. **The byte map is unverified until this step passes.**
3. Record byte 2 (`st=`) at power-up, in normal operation, and with the
   sensor lifted off the glass. These observations are what
   `LIN_SENSOR_FAULT_MASK` needs. Leave the mask at `0x00` until you know
   which bits mean "fault/initialising".
4. Unplug the sensor while running: within about 500 ms the line should go
   to `LIN --` and `to` should climb.
5. Record everything in hardware.md §6 and set `DEBUG_LIN_SENSOR 0` again.

## 3. CAN replay rig

Goal: prove passthrough is byte-exact both ways, 0x3E9 is rewritten with a
valid checksum, and nothing is dropped under load.

### Wiring

```
 PC ─USB─ can0 ── segment A ── ESP xcvr 0 (TWAI0, "car side")
 PC ─USB─ can1 ── segment B ── ESP xcvr 1 (TWAI1, "wiper side")
```

- Two USB-CAN adapters with Linux SocketCAN support (or one 2-channel
  adapter). Exact models used: **TODO, record them**.
- **Termination:** each segment is its own bus and needs 120 Ω at **both**
  ends: one at the adapter, one at the ESP transceiver. Many adapters have a
  switchable terminator. With everything unpowered, measure about 60 Ω
  between CANH and CANL on each segment.
- Common ground between the adapters and the ESP.

### Bring the interfaces up

Use the same rate as `CAN_BITRATE_KBPS`:

```bash
sudo ip link set can0 type can bitrate 500000
sudo ip link set can1 type can bitrate 500000
sudo ip link set can0 up
sudo ip link set can1 up
```

Watch both sides in two terminals:

```bash
candump -td can0     # car side
candump -td can1     # wiper side
```

### Tests

Frames below are built with the checksum rule from the README
(`(0xE9 + 0x03 + sum(bytes 0-6)) & 0xFF`), with counter nibble 1 in byte 6.
`tools/make_3e9.py` builds them for you and checks captures:

```bash
./tools/make_3e9.py --speed 5 --counter 1           # → 3E9#500000000000104C
./tools/make_3e9.py --speed 5 --count 200 --stream | \
  while read f; do cansend can0 "$f"; sleep 0.05; done   # 20 Hz, rolling counter
candump -L can1,3E9:7FF | ./tools/make_3e9.py --check     # checksum + counter per frame
```

**Timing before the override tests (2, 3):** once `LIN OK` appears, the
sensor must stay healthy for `WIPER_WARMUP_MS` (2 s) before the device
overrides anything. Until then the health reason is `passthrough(warm-up)`
and frames pass unchanged. A sensor fault or a dropout restarts the
warm-up. A rise also has to hold for `WIPER_RISE_HOLD_MS` (100 ms), so
stream frames as above rather than sending a single one.

1. **Passthrough, sensor unplugged.** Send a 0x3E9 asking for speed 5:

   ```bash
   cansend can0 3E9#500000000000104C
   ```

   Expected on can1: `3E9 [8] 50 00 00 00 00 00 10 4C`, byte-for-byte
   identical. Health shows `seen` rising and `modified=0`.

2. **Override down, sensor connected and dry.** Same frame. In the default
   authoritative mode the expected frame on can1 is speed 0 with a
   recomputed checksum: `3E9 [8] 00 00 00 00 00 00 10 FC`. Health shows
   `modified` rising and `oem=5 out=0`. (In raise-only mode,
   `WIPER_ALLOW_OVERRIDE_DOWN 0`, the frame passes unchanged instead.)

3. **Override up, sensor wet.** Send speed 0
   (`cansend can0 3E9#00000000000010FC`) while wetting the sensor. Can1
   should show the mapped speed (`WIPER_RAIN_TO_SPEED_MAP`) **equal to** our
   target, with a valid byte 7. Check byte 7 yourself with the rule above
   for every rewritten frame.

4. **SNA left alone.** `cansend can0 3E9#F0000000000010EC` (speed 15)
   should arrive unchanged.

5. **Wrong length left alone.** `cansend can0 3E9#5000` should arrive
   unchanged, and `badDLC` should go up.

6. **Other IDs, both directions.** For example:

   ```bash
   cansend can0 123#DEADBEEF
   cansend can1 456#0102030405060708
   ```

   Each must appear on the other side byte-for-byte identical.

7. **Flood.** In each direction, then both at once:

   ```bash
   cangen can0 -g 0 -I r -L r -D r      # random IDs/lengths, no gap
   cangen can1 -g 0 -I r -L r -D r
   ```

   Watch `drop`, `miss` and `off` in the health log. Note that the ESP
   retransmits everything from one node in FIFO order, so the original
   senders' priority arbitration is lost. Under load a high-priority frame
   can queue behind low-priority ones. Use `candump -td` timestamps to
   check 0x3E9 latency while flooding with other IDs, and record the
   result. If drops appear, see ROADMAP Phase 5 (hardware filter).

8. **Bus-off recovery.** Short CANH to CANL on one segment briefly (or
   unplug its adapter so frames go un-ACKed). `off` should increment and the
   log should show `BUS-OFF - initiating recovery` then `recovered`. The other
   direction should keep working.

9. **Soak.** Leave a realistic replay running for hours and confirm the
   counters stay at 0 and the device doesn't reboot (a watchdog reset prints
   a panic + reboot on the console).

10. **Warm-up and debounce.** With the sensor wet and frames streaming:
    unplug the sensor (health → `passthrough(no sensor)` within about
    0.5 s), plug it back in (about 2 s of `passthrough(warm-up)`, then our
    speed). A brief splash that dries within 100 ms shouldn't raise the
    speed.

11. **Log path.** Watch `logDrop` in the WIPER health line. It counts
    transition log lines dropped because the log queue was full. Forwarding
    never waits on the console, so a non-zero value is cosmetic, but
    record it.

### Sniffer build check (before Phase 3.1)

The sniffer build is how you measure the car's bus without risking it:

```bash
SNIFFER=1 CAN_BITRATE_KBPS=500 ./idf.sh build && ./idf.sh flash monitor
```

- The boot log shows `SNIFFER build: listen-only` and `TWAI0 ready
  LISTEN-ONLY`. The WIPER health line is replaced by `SNIFF TWAI0/1` lines:
  0x3E9 `seen`, `badCS`, `ctrSkip`, the min..max frame interval, and the
  last speed.
- Stream 0x3E9 from can0 (above). `seen` rises, and `badCS=0 ctrSkip=0`.
  Send a corrupt frame (`cansend can0 3E9#5000000000001000`): it's logged
  with `checksum=BAD`.
- **The sniffer never ACKs, never transmits and forwards nothing.** Keep
  the second adapter on the segment so frames get acknowledged.
- In the car it connects **in parallel** (a T-tap, TWAI0 only), never
  inline. If the bitrate is wrong it stays silent (no error frames), which
  is the point: try 500, then the others, until `seen` rises and
  `badCS=0`.

## 4. Pre-install checklist (gates the car)

Don't connect this to a car until **every** box is ticked and the evidence
is written down (ROADMAP / `docs/captures/`).

- [ ] **Bitrate measured on the car** (ROADMAP Phase 3.1, sniff only, inject
      nothing; use the `SNIFFER=1` build connected in parallel) and the
      firmware built with that exact `CAN_BITRATE_KBPS`.
- [ ] Harness tap point, connector, CANH/CANL pins and the other nodes on
      that segment documented in hardware.md §7.
- [ ] Real 0x3E9 frames captured. `tesla_checksum` reproduces byte 7 for
      every captured frame, and golden vectors were added to the tests.
- [ ] **Manual stalk and washer behaviour captured** (ROADMAP Phase 3.1,
      known issue #13): it is known whether the body controller obeys
      DAS_wiperSpeed outside auto mode.
- [ ] Bench stages 2 and 3 above passed, including flood and soak.
- [ ] Termination measured on both segments (about 60 Ω each).
- [ ] Power supply has a fuse, reverse-polarity protection and a TVS.
      Battery drain considered (the firmware never sleeps).
- [ ] **Passthrough soak in the car passed** with the sensor unplugged
      (ROADMAP Phase 3.2): no bus errors, no warnings, several ignition
      cycles.
- [ ] **Hardware bypass fitted and tested** (hardware.md §8, ROADMAP
      Phase 4): pull the ESP's power while the bus is active and confirm the
      car side and wiper side stay connected. **Required before driving.**
- [ ] **First drive in raise-only mode:** build with
      `WIPER_ALLOW_OVERRIDE_DOWN 0` until the manual/washer question above
      is answered and the sensor has been shown to read rain through Tesla
      glass (Phase 3.3). In that mode the device can add wipes but can never
      remove one Tesla asked for.
