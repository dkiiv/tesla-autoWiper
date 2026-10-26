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

> ⚠️ **Safety:** this device sits **in series** on a vehicle CAN bus. If it loses
> power or crashes, the frames behind it stop. In the worst case that means no
> wipers in the rain. Don't daily-drive it until ROADMAP Phase 4 (fail-safe) is
> done. Use at your own risk.

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
  - It never sends a slower speed than the car asked for.
  - It never sends `15` (`DAS_WIPER_SPEED_INVALID`).
  - It falls back to pure passthrough if the sensor is missing or its reading
    is more than 500 ms old.

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

**CAN bitrate:** currently set to 1 Mbit/s for bench work. Tesla vehicle buses
are generally reported as **500 kbit/s**. Confirm with a sniffer and set
`CAN_BAUD_RATE` in `config.h` before connecting to a car.

### Recommended ICs
- **CAN transceivers:** SN65HVD230 (3.3 V, works directly with the ESP32), TJA1050, or MCP2551 (5 V parts need level care)
- **LIN transceiver:** TJA1021, TJA1027, or MCP2003B. Connect TXD and RXD to the ESP32. The transceiver drives the single LIN wire and echoes our own transmission back on RXD, which the firmware checks.

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

You only need Docker (plus Python on the host for flashing). Use Git Bash on
Windows and a terminal on macOS/Linux. See [DOCKER_DEV.md](DOCKER_DEV.md) for
details.

```bash
./idf.sh build                       # ESP32-C6 (production, default)
IDF_TARGET=esp32p4 ./idf.sh build    # ESP32-P4 bench board
./idf.sh flash                       # chip + offsets come from the build
./idf.sh monitor
./idf.sh test                        # host unit tests (protocol + wiper policy)
```

If you have a C compiler and `make`, you can run the tests without Docker:
`make -C test`.

CI (GitHub Actions) runs the unit tests and builds both chips on every push.

---

## File structure

```
tesla-autoWiper/
├── README.md, AGENTS.md, ROADMAP.md, LICENSE
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
[HEALTH] LIN OK  rain=3 light=120 age=12ms ok/to/cs/echo=1234/0/0/0
[HEALTH] CAN car→act rx=51234 drop=0 miss=0 off=0 | act→car rx=8812 drop=0 miss=0 off=0
[HEALTH] WIPER 0x3E9 seen=500 modified=480 badDLC=0 oem=0 out=3
```

- **LIN `--`:** no fresh sensor data, so the device is in passthrough.
  `to` counts timeouts (no sensor or wiring problem), `cs` counts checksum
  errors, and `echo` counts header echo mismatches (transceiver wiring).
- **`drop` / `miss` / `off`:** frames lost on TX, frames lost on RX, and
  bus-off events. These should all be 0.
- **`oem` / `out`:** the wiper speed the car asked for and the speed we
  actually sent.

---

## License

MIT. See [LICENSE](LICENSE).
