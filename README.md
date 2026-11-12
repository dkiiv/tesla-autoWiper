# Rain-Sensor Wiper Controller (Tesla Model 3/Y)

Tesla picks auto-wiper speed with a camera and a neural net. This project uses a
real VW/Audi (VAG) optical rain sensor instead. A small ESP32 sits inline on the
CAN link between the Autopilot computer and the car. It reads the rain sensor
over LIN and rewrites the wiper-speed request on the way through.

> **Status: work in progress, NOT verified in a car.** Firmware builds for both
> chips and the protocol/policy math is unit-tested. Bench and vehicle testing
> haven't happened yet (see [ROADMAP.md](ROADMAP.md)).
>
> **Picking this up?** Start with [AGENTS.md](AGENTS.md) (orientation for humans
> and LLMs), then work through [ROADMAP.md](ROADMAP.md) from the top.

> ⚠️ **Safety: read all of this before connecting anything to a car.**
>
> - **It sits in series on a vehicle CAN bus.** If it loses power, crashes,
>   reboots (watchdog), is still booting, or runs at the wrong bitrate,
>   **every frame on the intercepted bus segment stops**, not just wiper
>   traffic. Which bus this is, and which other nodes share that segment,
>   is **not documented yet** (see [docs/hardware.md §7](docs/hardware.md#7-vehicle-harness)),
>   so the real impact of a failure is unknown.
> - **A hardware bypass is a hard prerequisite for driving.** Software can't
>   cover a dead chip, a brown-out, the boot window or a watchdog reboot. Do
>   not drive with this installed until the normally-closed bypass from
>   ROADMAP Phase 4 is fitted and tested ([docs/hardware.md §8](docs/hardware.md#8-hardware-bypass-required-before-driving)).
> - **It may override the driver.** By default the firmware is
>   authoritative and forces the wipers OFF whenever the sensor reads dry.
>   It is **unknown** whether the body controller ignores this frame while
>   the stalk is in a manual setting or during a washer cycle (ROADMAP known
>   issue #13). If it doesn't, the device could switch off wipers the driver
>   asked for.
> - **A sensor can talk fine and still read dry in the rain.** A sensor
>   that isn't optically coupled to the glass (missing gel pad, air gap, bad
>   bracket, glass it can't see through) still answers on LIN with valid
>   checksums and reports "no rain". In authoritative mode that **forces the
>   wipers off in real rain**. The firmware can't detect this yet. For first
>   road tests, build in raise-only mode (`WIPER_ALLOW_OVERRIDE_DOWN 0` in
>   `main/config.h`), where the device can add wipes but never remove one.
> - **Vehicle electrical hazards.** Car 12 V has cranking dips, load-dump
>   surges and fast transients, and a wiring mistake can reverse polarity.
>   The supply needs a fuse, reverse-polarity protection, a TVS and an
>   automotive-rated regulator ([docs/hardware.md §3](docs/hardware.md#3-power-supply-requirements)).
>   **The firmware never sleeps.** On an always-on feed it will keep
>   drawing current while the car is parked and can drain the 12 V battery.
>
> No validated hardware design exists yet. Use at your own risk, and work
> through the [pre-install checklist](docs/bench-bringup.md#4-pre-install-checklist-gates-the-car)
> before the car.

---

## Supported chips

| Chip | Role | Notes |
|------|------|-------|
| **ESP32-C6** | **Production** (in-car module), default build target | Easy to source; 2× TWAI + spare UART |
| ESP32-P4 | Bench / dev board (current breadboard) | Several incompatible hardware revisions in circulation, so it's not the production target |

Pins for each chip are in `main/config.h`, and the right block is chosen at
compile time.

---

## How it works

```
VAG rain/light sensor ──LIN──► ESP32 ◄──CAN (TWAI0)── AP computer ("car side")
                                  │
                                  └────CAN (TWAI1)──► body controller / wiper side
```

- **LIN master:** polls the rain sensor (frame ID `0x21`) at 20 Hz.
- **CAN man-in-the-middle:** forwards every frame in both directions. Only
  `0x3E9 DAS_bodyControls` (car → wiper side) can be changed: the
  `DAS_wiperSpeed` nibble and the checksum. The rolling counter and all other
  bits pass through untouched.
- **Wiper policy** (`main/wiper_policy.h`):
  - Rain intensity is mapped to a Tesla speed through a lookup table.
  - Speed goes up immediately and only comes down after 3 s, so the wipers
    don't hunt between speeds.
  - **It's authoritative (default).** While the sensor is healthy, its speed
    is what goes on the bus, whether that's higher or lower than Tesla's
    request. A phantom wipe on dry glass is forced to OFF.
  - **Raise-only mode** (`WIPER_ALLOW_OVERRIDE_DOWN 0`, recommended for first
    road tests): it may raise Tesla's speed but never lower it.
  - It never sends `15` (`DAS_WIPER_SPEED_INVALID`).
  - It falls back to pure passthrough if the sensor is missing, its reading
    is more than 500 ms old, or its status byte matches
    `LIN_SENSOR_FAULT_MASK` (disabled by default because the status bits are
    unverified).

---

## Hardware connections

| Signal | ESP32-C6 (prod) | ESP32-P4 (bench) | Connected to |
|---|---|---|---|
| CAN0 TX (car side) | GPIO 4  | GPIO 35 | CAN transceiver 0 |
| CAN0 RX            | GPIO 5  | GPIO 36 | CAN transceiver 0 |
| CAN1 TX (wiper side) | GPIO 6 | GPIO 31 | CAN transceiver 1 |
| CAN1 RX            | GPIO 7  | GPIO 34 | CAN transceiver 1 |
| LIN TX (UART1)     | GPIO 10 | GPIO 20 | LIN transceiver TXD |
| LIN RX (UART1)     | GPIO 11 | GPIO 21 | LIN transceiver RXD |

The C6 pins are a first pick from free, non-strapping pins on the
ESP32-C6-DevKitC-1 and haven't been tested on hardware yet. Change them in
`main/config.h` to match your board.

**CAN bitrate: there is no default, and the build fails until you choose
one.** Tesla vehicle buses are generally reported as **500 kbit/s**. That's
expected but **unconfirmed** for the bus this device sits on. Measure it
with a sniffer before connecting to a car, then build with
`CAN_BITRATE_KBPS=<rate>` (see [Build, flash, test](#build-flash-test)). A
wrong rate in the car means a dead bus segment. The boot log prints the
compiled-in rate.

### Recommended ICs
- **CAN transceivers:** SN65HVD230 (3.3 V, works directly with the ESP32) or
  TJA1051T/3 with its VIO pin on 3.3 V. TJA1050 and MCP2551 are 5 V parts
  with a 5 V RXD output, so don't wire RXD straight to an ESP32 pin.
- **LIN transceiver:** TJA1021, TJA1027, or MCP2003B. The transceiver drives
  the single LIN wire and echoes our own transmission back on RXD, which the
  firmware checks. As LIN master you need a 1 kΩ pull-up + series diode on
  the LIN line. On the TJA1021/MCP2003B, `SLP_N`/`CS` must be held high and
  RXD is open-drain (needs a pull-up).
- **Termination:** splitting the bus makes two segments. Each needs 120 Ω at
  both ends, so add one at each ESP transceiver.

Full requirements, BOM and the (mostly TODO) sensor and vehicle details:
**[docs/hardware.md](docs/hardware.md)**. Bench procedure:
**[docs/bench-bringup.md](docs/bench-bringup.md)**.

---

## LIN break-field workaround

The ESP32 UART can't send a LIN break field (≥13 dominant bits) directly. The
workaround, adapted from
[mestrode/Lin-Interface-Library](https://github.com/mestrode/Lin-Interface-Library),
works like this:

1. Drop the baud rate to `LIN_BAUD / 2` (9600 bps).
2. Send one `0x00` byte. At the nominal rate this looks like about 18 dominant bits.
3. Restore `LIN_BAUD` (19200 bps) and discard the break echo.
4. Send the sync byte `0x55` and the protected ID.
5. Read the echoed sync+PID (and verify them), then read the sensor's
   response plus its checksum.

## VAG sensor protocol (LIN 2.x)

This byte map comes from community reverse-engineering and is **unverified**
against our sensor. Log raw bytes with `DEBUG_LIN_SENSOR=1` first.

| Byte | Field            | Notes                                  |
|------|------------------|----------------------------------------|
| 0    | Rain intensity   | Lower nibble `[3:0]`, range 0-15       |
| 1    | Light intensity  | 0 = bright, 255 = very dark            |
| 2    | Status flags     | Sensor fault / init bits               |
| 3-6  | Other channels   | IR / solar / additional light data     |
| 7    | Checksum         | LIN 2.x enhanced (PID included)        |

Frame ID: `0x21` → Protected ID: `0x61`

## Tesla signal (from commaai/opendbc)

`BO_ 1001 DAS_bodyControls` (`0x3E9`, 8 bytes):
- `DAS_wiperSpeed`: bits 4-7 of byte 0. 0 = off, 1-14 = speeds, 15 = INVALID.
- `DAS_bodyControlsCounter`: upper nibble of byte 6. Passed through unchanged.
- `DAS_bodyControlsChecksum`: byte 7, computed as
  `(0xE9 + 0x03 + sum(bytes 0-6)) & 0xFF`.

---

## Build, flash, test

**Prerequisites**

- **Build:** Docker (the `./idf.sh` wrapper runs ESP-IDF v5.3.2 in a
  container). Use Git Bash or WSL on Windows and a terminal on macOS/Linux.
  See [DOCKER_DEV.md](DOCKER_DEV.md). Or install ESP-IDF v5.3.2 natively
  (see AGENTS.md).
- **Flash / monitor (run on the host, not in the container):** Python 3 with
  `esptool` and `pyserial` (`python3 -m pip install esptool pyserial`;
  `./idf.sh flash` installs them if missing). You also need the USB serial
  driver for your board's USB-UART bridge (for example CP210x or CH340 on
  Windows/macOS, depending on the board; boards using the chip's native USB
  usually need no driver). On Linux your user needs access to the serial
  device (often the `dialout` group).
- **A CAN bitrate. There is no default.** The build fails with an `#error`
  until you set `CAN_BITRATE_KBPS` (125, 250, 500 or 1000). The car is
  expected to be 500 kbit/s, but that's **unconfirmed**: measure before
  installing. Pass it per build or put `CAN_BITRATE_KBPS=500` in `.env`.

```bash
CAN_BITRATE_KBPS=500 ./idf.sh build                      # ESP32-C6 (production, default)
CAN_BITRATE_KBPS=500 IDF_TARGET=esp32p4 ./idf.sh build   # ESP32-P4 bench board
./idf.sh flash                       # chip + offsets come from the build
./idf.sh monitor
./idf.sh flash monitor               # both
./idf.sh test                        # host unit tests (protocol + wiper policy)
```

Native ESP-IDF: `idf.py -B build -DCAN_BITRATE_KBPS=500 set-target esp32c6 build`.

If you have a C compiler and `make`, you can run the tests without Docker:
`make -C test`.

CI (GitHub Actions) runs the unit tests and builds both chips on every push.
CI firmware artifacts are **bench builds at 500 kbit/s**
(`firmware-<target>-bench-500kbps-UNVERIFIED`), not validated car images.

Before bench work, read [docs/bench-bringup.md](docs/bench-bringup.md).

---

## File structure

```
tesla-autoWiper/
├── README.md, AGENTS.md, ROADMAP.md, LICENSE
├── docs/hardware.md           ← wiring requirements, BOM, TODO-to-measure tables
├── docs/bench-bringup.md      ← bench procedure + pre-install checklist
├── CMakeLists.txt, sdkconfig.defaults
├── Dockerfile, docker-compose.yml, idf.sh, DOCKER_DEV.md, .env.example
├── .github/workflows/ci.yml   ← unit tests + C6/P4 builds
├── test/                      ← host unit tests (plain C, no ESP-IDF)
└── main/
    ├── config.h               ← ALL pins, IDs, timings, policy tuning, debug switches
    ├── main.c                 ← startup, task creation, health log
    ├── can_gateway.h/.c       ← dual-TWAI forwarding tasks + bus-off recovery
    ├── lin_sensor.h/.c        ← LIN master driver (break workaround, echo check)
    ├── wiper_logic.h/.c       ← glue: sensor + policy → rewrite 0x3E9
    ├── wiper_policy.h/.c      ← pure C: rain → speed decision, hysteresis
    └── protocol.h/.c          ← pure C: LIN PID/checksum, Tesla checksum + bit packing
```

## Reading the serial log

Every 5 s you get three `[HEALTH]` lines:

```
[HEALTH] LIN OK  rain=3 light=120 st=0x00 age=12ms ok/to/cs/echo/flt=1234/0/0/0/0
[HEALTH] CAN car→act rx=51234 drop=0 miss=0 off=0 | act→car rx=8812 drop=0 miss=0 off=0
[HEALTH] WIPER 0x3E9 seen=500 modified=480 badDLC=0 fault=0 oem=5 out=0 mode=auth
```

- **LIN `--`:** no fresh sensor data, so the device is in passthrough.
  `to` counts timeouts (no sensor or wiring problem), `cs` counts checksum
  errors, and `echo` counts header echo mismatches (transceiver wiring).
  `st` is the raw status byte of the last good reading. `flt` counts good
  readings whose status matched `LIN_SENSOR_FAULT_MASK` (always 0 while the
  mask is 0x00).
- **`drop` / `miss` / `off`:** frames lost on TX, frames lost on RX, and
  bus-off events. These should all be 0.
- **`oem` / `out`:** the wiper speed the car asked for and the speed we
  actually sent. In the example, `oem=5 out=0` is a suppressed phantom wipe.
- **`fault`:** 0x3E9 frames passed through because the sensor status matched
  the fault mask. **`mode`:** `auth` (authoritative) or `raise` (raise-only).

---

## License

MIT. See [LICENSE](LICENSE).
