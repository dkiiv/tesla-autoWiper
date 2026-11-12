# Hardware guide (DRAFT, UNVERIFIED)

> **Status: nothing on this page has been validated on a bench or in a car.**
> It lists the *requirements* a build has to meet and the facts a builder
> still has to *measure and record*. It is not a validated design. Every
> `TODO` / `UNVERIFIED` marker is a gap. Please fill it in (with a photo or
> a capture as evidence) and send a PR.
>
> Read the safety section of the [README](../README.md) first. This device
> sits **in series** on a vehicle CAN bus.

Contents:

1. [Block diagram](#1-block-diagram)
2. [Bill of materials](#2-bill-of-materials)
3. [Power supply requirements](#3-power-supply-requirements)
4. [CAN side: transceivers and termination](#4-can-side-transceivers-and-termination)
5. [LIN side: transceiver wiring](#5-lin-side-transceiver-wiring)
6. [VAG rain/light sensor](#6-vag-rainlight-sensor)
7. [Vehicle harness](#7-vehicle-harness)
8. [Hardware bypass (required before driving)](#8-hardware-bypass-required-before-driving)

---

## 1. Block diagram

```
            vehicle 12 V (switched? always-on? TODO: measure, see §3/§7)
                 │
          ┌──────┴───────┐
          │ fuse → rev.  │
          │ polarity →   │──── 12 V (protected) ──────────────┐
          │ TVS → buck   │                                    │
          └──────┬───────┘                                    │
                 │ 3.3 V                                      │ VBAT
                 ▼                                            ▼
 ┌─────────────────────────────────┐              ┌──────────────────┐
 │ ESP32-C6 (prod) / ESP32-P4 (bench) │  TX/RX   │ LIN transceiver  │  LIN   ┌──────────────┐
 │                                 │◄───UART1───►│ (TJA1021/MCP2003B)│◄──────►│ VAG rain/    │
 │ TWAI0          TWAI1            │             └──────────────────┘ 1 wire │ light sensor │
 └──┬─────────────────┬────────────┘                                          └──────────────┘
    │ TX/RX           │ TX/RX
 ┌──▼──────────┐  ┌───▼─────────┐
 │ CAN xcvr 0  │  │ CAN xcvr 1  │
 │ (car side)  │  │ (wiper side)│
 └──┬──────────┘  └───┬─────────┘
    │ CANH/CANL       │ CANH/CANL
    │ 120 Ω here      │ 120 Ω here        (see §4 — each segment needs
    ▼                 ▼                    termination at both ends)
 AP computer side   body controller / wiper side
 (segment A)        (segment B)

        ╔══════════════════════════════════════════════════════╗
        ║ Bypass relay (NC): bridges segment A ↔ B whenever    ║
        ║ the ESP is unpowered or not asserting "healthy".     ║
        ║ REQUIRED before driving — see §8 / ROADMAP Phase 4.  ║
        ╚══════════════════════════════════════════════════════╝
```

GPIO assignments per chip are in the README "Hardware connections" table and
in `main/config.h` (the C6 pins are a first pick and are untested on
hardware).

## 2. Bill of materials

Generic parts only. Exact part numbers and links are **TODO**: fill them in
from the build you actually tested.

| # | Qty | Part | Requirement / notes | Exact P/N | Source link | Verified? |
|---|---|---|---|---|---|---|
| 1 | 1 | ESP32-C6 dev board (prod) or ESP32-P4 board (bench) | 2× TWAI + a free UART. The C6 pin map assumes ESP32-C6-DevKitC-1 | TODO | TODO | ❌ |
| 2 | 2 | CAN transceiver, 3.3 V logic | SN65HVD230 (3.3 V part), or a TJA1051T/3 with VIO tied to 3.3 V. See §4 | TODO | TODO | ❌ |
| 3 | 1 | LIN transceiver | TJA1021, TJA1027 or MCP2003B. Check the datasheet for enable/sleep pins. See §5 | TODO | TODO | ❌ |
| 4 | 1 | 1 kΩ resistor + series diode | LIN master pull-up (LIN line to VBAT). See §5 | TODO | TODO | ❌ |
| 5 | 1 | Pull-up resistor for LIN RXD | Only if your transceiver's RXD is open-drain (TJA1021 and MCP2003B are) and your board has no pull-up. Value: check the datasheet | TODO | TODO | ❌ |
| 6 | 2 | 120 Ω resistor | CAN termination at the ESP end of each bus segment. See §4 | TODO | TODO | ❌ |
| 7 | 1 | Automotive-rated 12 V → 3.3 V regulator (buck preferred) | Input range must cover cranking dips and transients. See §3 | TODO | TODO | ❌ |
| 8 | 1 | Inline fuse + holder | Rating: TODO (size to the measured current draw plus margin) | TODO | TODO | ❌ |
| 9 | 1 | Reverse-polarity protection | Series diode or ideal-diode / P-FET. See §3 | TODO | TODO | ❌ |
| 10 | 1 | TVS diode on the 12 V input | Automotive type rated for load dump. Choose per regulator max input | TODO | TODO | ❌ |
| 11 | 1 | Bypass relay or analog switch, normally closed, 2 pole (CANH + CANL) | Required before driving. See §8 | TODO | TODO | ❌ |
| 12 | 1 | VAG rain/light sensor | **Exact part number TODO.** See §6 | TODO | TODO | ❌ |
| 13 | 1 | Sensor mounting bracket + optical coupling gel pad | Matched to the sensor. See §6 | TODO | TODO | ❌ |
| 14 | 1 set | Vehicle-side connectors / harness adapter | Unknown, see §7. Do not cut the factory harness without a plan to restore it | TODO | TODO | ❌ |
| 15 | 1 | Enclosure | Heat, vibration, condensation. TODO | TODO | TODO | ❌ |

Bench-only extras (see [bench-bringup.md](bench-bringup.md)):

| Part | Notes |
|---|---|
| 12 V bench supply with current limit | Powers the sensor and LIN pull-up |
| 2× USB-CAN adapters with SocketCAN support (or one 2-channel adapter) | One per bus segment for the replay test |
| Spray bottle / wet cloth | To wet the sensor |

## 3. Power supply requirements

These are **requirements, not a validated design**. Nothing below has been
built or measured.

- **Input:** vehicle 12 V. Where to pick it up (always-on vs switched, which
  fuse/circuit) is **TODO: measure and record** (§7).
- **Fuse** close to the tap point. Rating TODO: measure the device's draw
  first.
- **Reverse-polarity protection** before anything else (series diode, or an
  ideal-diode controller / P-FET for lower drop).
- **Transient protection:** an automotive TVS on the 12 V input, and a
  regulator whose absolute-maximum input rating covers what the TVS lets
  through. Vehicle 12 V sees cranking dips, load-dump surges and fast
  transients. Check the regulator and TVS datasheets against your
  vehicle's expected transients. Do not trust a hobby buck module's
  marketing rating.
- **Regulator:** 12 V → 3.3 V for the ESP and the 3.3 V CAN transceivers.
  A buck converter wastes less heat than a linear regulator from 12 V. The
  sensor and the LIN transceiver's VBAT pin run from the protected 12 V
  rail, not from 3.3 V.
- **Ground:** one common ground between the ESP, both CAN transceivers, the
  LIN transceiver and the sensor. Where to ground in the car is TODO.
- **Sleep / battery drain: NOT HANDLED.** The firmware never sleeps. If you
  power it from an always-on 12 V feed it will draw current while the car is
  parked and can drain the 12 V battery. Until sleep handling exists, either
  use a switched feed (and accept the boot window covered by §8) or measure
  the idle draw and decide. Measured idle current: **TODO**.
- **Boot window:** after power-up the ESP takes some time before it forwards
  frames. During that time the bus segments are disconnected unless the
  bypass (§8) bridges them. Time from 12 V to passthrough active: **TODO:
  measure** (ROADMAP Phase 4).

## 4. CAN side: transceivers and termination

### Transceivers (3.3 V vs 5 V)

The ESP32 GPIOs are **3.3 V only**. The transceiver's RXD output drives an
ESP GPIO directly, so its logic level matters.

| Part | Logic supply | Notes |
|---|---|---|
| SN65HVD230 | 3.3 V | Works directly with the ESP32. Recommended. |
| TJA1051T/3 | 5 V bus supply + **VIO** pin | Tie VIO to 3.3 V so TXD/RXD use 3.3 V levels. Check the datasheet for the exact variant you buy; only the `/3` variant has VIO. |
| TJA1050, MCP2551 | 5 V | 5 V parts with a 5 V RXD output. **Do not connect RXD straight to an ESP32 pin.** You need a level shifter or divider on RXD. Better: use a 3.3 V part. |

Some transceivers have a standby / silent / slope-control pin (for example
the SN65HVD230 `Rs` pin). It must be in the mode that lets the part
transmit. Check the datasheet for your part and your breakout board.

### Termination (the bus is split in two)

Putting the ESP in series cuts the original bus into **two separate
segments**:

- **Segment A:** car side ↔ ESP transceiver 0 (TWAI0)
- **Segment B:** ESP transceiver 1 (TWAI1) ↔ body controller / wiper side

A CAN bus needs **120 Ω termination at each end of each segment**
(about 60 Ω measured between CANH and CANL with the bus powered down). The
original bus had its two terminators at its two far ends. After the split,
each segment has one original terminator (probably, TODO: confirm) and one
new open end at the ESP. So:

1. Fit a 120 Ω resistor between CANH and CANL at **each** ESP transceiver
   (many breakout boards already have one, so check before adding a second).
2. With the car asleep / bus unpowered, measure CANH↔CANL resistance on each
   segment. **Expect about 60 Ω.** About 120 Ω means one terminator is
   missing. About 40 Ω means one too many.
3. Record the measured values in §7.

The bypass relay (§8) reconnects A and B into one bus. When it does, the two
ESP-side terminators end up in the middle of the bus, so the bus would have
four terminators (about 30 Ω). A design that bridges the buses therefore has
to **also disconnect the ESP-side terminators** while bridging (for example
with extra relay poles). This is part of the Phase 4 design work. It is not
solved yet.

## 5. LIN side: transceiver wiring

These are generic LIN-master requirements. Confirm every pin against the
datasheet of the exact part and breakout you use.

- **Master pull-up:** a LIN *master* needs a **1 kΩ pull-up from the LIN line
  to VBAT through a series diode** (the slaves only have their weak internal
  pull-ups). Without it the bus may not idle recessive properly. Some
  breakout boards have a jumper or solder bridge for this, so check yours.
- **VBAT:** the transceiver's battery pin goes to the protected 12 V rail,
  the same supply as the sensor.
- **TXD:** ESP `LIN_TX_PIN` → transceiver TXD.
- **RXD:** transceiver RXD → ESP `LIN_RX_PIN`. On the **TJA1021** and
  **MCP2003B**, RXD is an **open-drain** output. It needs a pull-up to 3.3 V
  (an external resistor, check the datasheet for a value, or the ESP's
  internal pull-up if it's strong enough for 19.2 kbit/s; external is
  safer). With no pull-up, RXD never goes high and the firmware reports
  echo errors or timeouts.
- **Enable pins:**
  - **TJA1021:** `SLP_N` must be driven **high** for normal mode. Left low
    or floating, the transceiver sleeps and nothing happens.
  - **MCP2003B:** `CS` must be driven **high** for operation mode.
  - The firmware does **not** drive these pins. Tie them high in hardware
    (per the datasheet's allowed voltage for that pin) or add a GPIO and code.
  - TJA1027: check the datasheet.
- **Echo:** a single-wire LIN transceiver echoes the master's own
  transmission back on RXD. The firmware relies on this
  (`LIN_EXPECT_ECHO 1` in `config.h`) and checks that the echoed sync + PID
  match. If your setup does not echo, set it to 0.

## 6. VAG rain/light sensor

**Everything here is TODO.** The byte map in the README comes from community
reverse-engineering and has not been confirmed against any specific sensor.
VAG made many rain/light sensor variants. They differ in connector, bracket,
LIN frame IDs and payload. Until someone records a working combination, a
builder cannot know which sensor to buy.

Record for the sensor you test:

| Item | Value |
|---|---|
| Sensor part number (from the label) | **TODO** |
| Vehicle it came from (model / year) | **TODO** |
| Connector type / housing part number | **TODO** |
| Connector pinout (12 V, ground, LIN, any others) | **TODO: measure. Do not guess** |
| Supply current (idle / active) | **TODO** |
| LIN frame ID that answers (firmware assumes `0x21`) | **TODO** |
| Does it answer with no master frame, or does it need a config/status frame from the master first? | **TODO** |
| Payload length (firmware assumes 7 + checksum) | **TODO** |
| Checksum type (firmware assumes enhanced) | **TODO** |
| Raw bytes dry / light mist / wet / soaked (`DEBUG_LIN_SENSOR 1`) | **TODO** |
| Status byte (byte 2) values at power-up, normal, unplugged-from-glass | **TODO** (needed before `LIN_SENSOR_FAULT_MASK` can be set) |
| Bracket part number | **TODO** |
| Gel pad / coupling pad part number | **TODO** |
| Mounting location on the Tesla windshield | **TODO** |
| Does it read rain through Tesla glass (thickness, coatings, frit)? | **TODO** (ROADMAP Phase 3.3) |

An optical rain sensor that is not optically coupled to the glass (missing
gel pad, air gap, bad bracket) can still talk on LIN with valid checksums and
report "dry" forever. In authoritative mode that forces the wipers off in
real rain. See the README safety section. Bench-test the sensor on a piece
of glass before trusting it.

## 7. Vehicle harness

**Unknown. Everything here is TODO-to-measure.** Nothing has been sniffed or
traced on a car yet (ROADMAP Phase 3.1). Do not cut anything based on a
guess.

| Item | Value |
|---|---|
| Which CAN bus carries 0x3E9 between the AP computer and the body controller | **TODO** |
| Physical tap point (which harness, which connector) | **TODO** |
| Connector part numbers (for a plug-in adapter that avoids cutting) | **TODO** |
| CANH / CANL pin numbers at that connector | **TODO: measure** |
| Measured bitrate (firmware has no default, see README) | **TODO** |
| Other nodes on that segment, and what each one does | **TODO** (every frame on the segment stops if the ESP fails, so this list sets the real failure impact) |
| CANH↔CANL resistance, bus asleep, before the split | **TODO** (expect about 60 Ω) |
| Same, each segment after the split, with ESP terminators fitted | **TODO** |
| 12 V source (always-on / switched / which fuse) | **TODO** |
| Ground point | **TODO** |
| Where the module physically mounts | **TODO** |
| Photos | **TODO** |

## 8. Hardware bypass (required before driving)

The ESP physically breaks the CAN path. Software cannot cover a dead chip, a
brown-out, the boot window, or the reboot after a watchdog reset. Before any
driving, the build needs a **normally-closed** bypass that bridges segment A
to segment B whenever the ESP is unpowered or not asserting a "healthy"
signal (ROADMAP Phase 4).

Requirements (no validated design exists yet):

- Normally-closed: with no power, the buses are bridged.
- Switches both CANH and CANL.
- Handles termination while bridged (§4): the ESP-side terminators must not
  stay on the bus while it's bridged.
- Opens only when firmware asserts "healthy". **The firmware does not drive a
  healthy GPIO yet.** That's a TODO in code too.
- Contacts / switch rated for CAN signal levels; keep stubs short.
