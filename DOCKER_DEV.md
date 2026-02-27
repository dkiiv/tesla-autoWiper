# Docker Dev Environment — tesla-autoWiper

Drop these files into the root of the `tesla-autoWiper` repo. They give you a
fully self-contained ESP-IDF build environment with one-liner commands.

## Prerequisites

- Docker + Docker Compose v2 installed
- On **Linux**: your user in the `dialout` group (`sudo usermod -aG dialout $USER`, then log out/in)
- On **macOS**: Silicon Labs (CP210x) or CH340 driver installed so the device shows up in `/dev/tty.*`

## Quickstart

```bash
# 1. Copy the example env file
cp .env.example .env
# Edit .env — set FLASH_PORT to match your device

# 2. Build the firmware
./idf.sh build
# → compiled .bin and .elf appear in ./firmware/

# 3. Flash
./idf.sh flash

# 4. Open serial monitor
./idf.sh monitor

# Or flash + monitor in one shot
./idf.sh flash monitor
```

## All commands

| Command | What it does |
|---|---|
| `./idf.sh build` | Set target → build → copy firmware to `./firmware/` |
| `./idf.sh flash` | Flash the device |
| `./idf.sh monitor` | Serial monitor (Ctrl+] to exit) |
| `./idf.sh flash monitor` | Flash then monitor |
| `./idf.sh menuconfig` | Interactive Kconfig menu |
| `./idf.sh clean` | `idf.py fullclean` |
| `./idf.sh shell` | Interactive shell inside the container |
| `./idf.sh rebuild` | Rebuild the Docker image (after Dockerfile changes) |

## Finding your firmware

After `./idf.sh build`, the compiled output is in `./firmware/`:

```
firmware/
├── tesla-autowiper.bin       ← main firmware
├── tesla-autowiper.elf       ← for debugging
├── bootloader/bootloader.bin
├── partition_table/...
└── flasher_args.json         ← exact esptool args used to flash
```

You can also flash manually from the host using `esptool.py` and `flasher_args.json`.

## macOS notes

Docker Desktop on macOS runs containers inside a Linux VM, so USB passthrough
is limited. The device still usually works — just make sure:

1. The driver is installed and the device appears as `/dev/tty.usbserial-XXXX`
2. Set that path in `.env` as `FLASH_PORT`

If the container can't open the port, the fallback is to flash from the host:
```bash
pip install esptool
cd firmware
esptool.py --chip esp32p4 --port /dev/tty.usbserial-0001 \
  write_flash @flasher_args.json
```

## Changing IDF version

Edit the `FROM` line in `Dockerfile`:
```dockerfile
FROM espressif/idf:v5.3.2   # ← change this tag
```
Then run `./idf.sh rebuild`.

Available tags: https://hub.docker.com/r/espressif/idf/tags
