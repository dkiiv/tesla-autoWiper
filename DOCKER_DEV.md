# Docker Dev Environment — tesla-autoWiper

A self-contained ESP-IDF v5.3.2 build environment with one-line commands. The
only host requirements are Docker, plus Python for flashing and monitoring.

## Prerequisites

- Docker + Docker Compose v2
- **Linux:** add your user to the `dialout` group (`sudo usermod -aG dialout $USER`, then log out and back in)
- **macOS:** install the CP210x or CH340 USB-serial driver, unless your board uses native USB (`/dev/tty.usbmodem*`)
- **Host Python 3** for `flash`/`monitor`. `idf.sh` installs esptool and pyserial automatically.

## Quickstart

```bash
# 1. Optional: per-machine settings
cp .env.example .env        # set FLASH_PORT, and IDF_TARGET if you're on the P4 bench board

# 2. Build (ESP32-C6 by default)
./idf.sh build              # → ./firmware/
IDF_TARGET=esp32p4 ./idf.sh build   # bench board

# 3. Flash + monitor
./idf.sh flash
./idf.sh monitor            # Ctrl+] to exit

# 4. Unit tests (no hardware)
./idf.sh test
```

## All commands

| Command | What it does |
|---|---|
| `./idf.sh build` | Set target (`$IDF_TARGET`, default `esp32c6`), build, and copy the firmware to `./firmware/` |
| `./idf.sh flash` | Flash from the host with esptool. The chip type and offsets are read from `firmware/flash_args` and `firmware/flasher_args.json`, so a C6 image can't be flashed onto a P4 by mistake |
| `./idf.sh monitor` | Serial monitor (Ctrl+] to exit) |
| `./idf.sh test` | Host unit tests (`make -C test`) inside the container |
| `./idf.sh menuconfig` | Interactive Kconfig menu |
| `./idf.sh clean` | `idf.py fullclean` |
| `./idf.sh firmware` | Copy the firmware out of the build volume again |
| `./idf.sh shell` | Interactive shell inside the container |
| `./idf.sh rebuild` | Rebuild the Docker image (after Dockerfile changes) |
| `./idf.sh port` | Show the detected OS and available serial ports |

## Firmware output

```
firmware/
├── wiper_controller.bin       ← application (CMake project name: wiper_controller)
├── wiper_controller.elf       ← for decoding backtraces / debugging
├── bootloader/bootloader.bin
├── partition_table/partition-table.bin
├── flash_args                 ← offsets used by ./idf.sh flash
└── flasher_args.json          ← includes the chip name
```

The offsets differ per chip: the bootloader goes at `0x0` on the C6 and at
`0x2000` on the P4. Always flash with `@flash_args` rather than hand-typed
offsets.

## Building without Docker

With a native ESP-IDF v5.3.2 install (`. $IDF_PATH/export.sh`):

```bash
idf.py -B build set-target esp32c6 build
```

Note that IDF 5.3 does not support Python 3.13+. Use 3.12 or older for the IDF
Python environment.

## macOS notes

Docker Desktop on macOS runs containers in a Linux VM, so USB passthrough is
limited. This is why `flash` and `monitor` run on the host, not in the
container. If the port can't be opened, check the driver and the device path in
`.env`.

## Changing IDF version

Edit the `FROM` line in `Dockerfile` **and** the `container:` line in
`.github/workflows/ci.yml`, then run `./idf.sh rebuild`.
Available tags: https://hub.docker.com/r/espressif/idf/tags
