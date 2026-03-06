# Rain-Sensor Wiper Controller – ESP32-P4

An ESP project that sits between Tesla's AP-computer and car, using a VAG rain/light sensor over LIN bus to automatically control wiper speed.

---

## Architecture

```
Rain/light sensor  ──LIN──►  ESP32-P4  ◄──CAN0──  AP-Computer
                                 │
                                 └──────CAN1──►  Car
```

The ESP32-P4 acts as:
- **LIN master** – queries the Bosch/Continental VAG rain sensor (frame ID 0x21)
- **CAN MITM gateway** – intercepts the wiper control CAN frame, modifies it based on rain intensity, and forwards it to the car

---

## Hardware connections

| Signal          | ESP32-P4 GPIO | Connected to           |
|-----------------|--------------|------------------------|
| CAN0 TX         | GPIO 4       | CAN transceiver 0 (car side)  |
| CAN0 RX         | GPIO 5       | CAN transceiver 0             |
| CAN1 TX         | GPIO 6       | CAN transceiver 1 (wiper side)|
| CAN1 RX         | GPIO 7       | CAN transceiver 1             |
| LIN TX (UART1)  | GPIO 8       | LIN transceiver TX            |
| LIN RX (UART1)  | GPIO 9       | LIN transceiver RX            |

> Configure in **`main/config.h`**

### Recommended ICs
- **CAN transceivers**: TJA1050, SN65HVD230, or MCP2551
- **LIN transceiver**: TJA1021, TJA1027, or MCP2003B  
  (LIN transceiver handles the single-wire bus voltage; connect TX and RX to the same LIN pin on the transceiver)

---

## LIN break-field workaround

The ESP32 UART cannot natively emit a LIN break field (13+ dominant bits) because the hardware treats it as a UART error.  The workaround (adapted from [mestrode/Lin-Interface-Library](https://github.com/mestrode/Lin-Interface-Library)) is to:

1. Drop baud rate to `LIN_BAUD / 2` (9600 bps)
2. Transmit one `0x00` byte → produces ~20 dominant bits at nominal rate
3. Restore baud rate to `LIN_BAUD` (19200 bps)
4. Send sync byte `0x55` + protected ID
5. Flush the RX FIFO (single-wire echo) and read slave response

---

## VAG sensor protocol (LIN 2.x)

| Byte | Field            | Notes                                  |
|------|------------------|----------------------------------------|
| 0    | Rain intensity   | Lower nibble `[3:0]`, range 0–15       |
| 1    | Light intensity  | 0 = bright, 255 = very dark            |
| 2    | Status flags     | Sensor fault / init bits               |
| 3–6  | Other channels   | IR / solar / additional light data     |
| 7    | Checksum         | LIN 2.x enhanced (PID included)        |

Frame ID: `0x21`  →  Protected ID: `0x61`

---

## Build

> Git bash for Windows, terminal for macOS/Linux
> All env's use Dev Container for IDE

```bash
idf.sh build
idf.sh flash
idf.sh monitor
```

---

## File structure

```
wiper_controller/
├── CMakeLists.txt
└── main/
    ├── CMakeLists.txt
    ├── config.h          ← all pins, IDs, thresholds  (edit this first)
    ├── main.c            ← startup, task creation
    ├── lin_sensor.h/.c   ← LIN master driver + break workaround
    ├── can_gateway.h/.c  ← dual TWAI MITM gateway
    └── wiper_logic.h/.c  ← rain→speed mapping + CAN frame encoding (YOUR CODE)
```