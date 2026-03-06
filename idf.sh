#!/usr/bin/env bash
# idf.sh — wrapper around docker compose for ESP-IDF builds
#
# Usage:
#   ./idf.sh build
#   ./idf.sh flash
#   ./idf.sh monitor
#   ./idf.sh menuconfig
#   ./idf.sh shell
#   ./idf.sh clean
#   FLASH_PORT=/dev/tty.usbserial-0001 ./idf.sh flash

set -euo pipefail

COMPOSE="docker compose"
BUILD_DIR="/build"   # named volume mount point — keeps build OFF the macOS bind mount
PORT="${FLASH_PORT:-/dev/tty.usbmodem101}"

# ── helpers ───────────────────────────────────────────────────────────────────

build_image() {
  echo "🔨  Building Docker image…"
  $COMPOSE build
}

ensure_image() {
  if ! docker image inspect tesla-autowiper-idf &>/dev/null; then
    build_image
  fi
}

run_idf() {
  ensure_image
  $COMPOSE run --rm idf idf.py -B "$BUILD_DIR" "$@"
}

copy_firmware() {
  local out_dir="./firmware"
  mkdir -p "$out_dir"
  local vol
  vol="$(basename "$(pwd)" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9-')_idf-build-cache"
  docker run --rm \
    -v "${vol}:/build_vol" \
    -v "$(pwd)/${out_dir}":/out \
    busybox sh -c "
      cp /build_vol/*.bin /out/ 2>/dev/null || true
      cp /build_vol/*.elf /out/ 2>/dev/null || true
      cp /build_vol/flasher_args.json /out/ 2>/dev/null || true
      cp -r /build_vol/bootloader /out/ 2>/dev/null || true
      cp -r /build_vol/partition_table /out/ 2>/dev/null || true
    "
  echo "✅  Firmware copied to ${out_dir}/"
  ls -lh "${out_dir}/"
}

ensure_esptool() {
  if ! command -v esptool.py &>/dev/null; then
    echo "📦  esptool not found — installing..."
    pip3 install "esptool==4.8.1" --break-system-packages
  fi
}

ensure_pyserial() {
  if ! python3 -c "import serial" &>/dev/null; then
    echo "📦  pyserial not found — installing..."
    pip3 install pyserial --break-system-packages
  fi
}

# ── main ──────────────────────────────────────────────────────────────────────

CMD="${1:-help}"
shift || true

case "$CMD" in

  build)
    ensure_image
    $COMPOSE run --rm idf idf.py -B "$BUILD_DIR" \
      set-target "${IDF_TARGET:-esp32p4}" \
      build
    copy_firmware
    ;;

  flash)
    ensure_esptool
    echo "⚡  Flashing to ${PORT}..."
    cd firmware
    esptool.py --chip esp32p4 --port "$PORT" --baud 460800 \
      write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB \
      0x2000 bootloader/bootloader.bin \
      0x8000 partition_table/partition-table.bin \
      0x10000 wiper_controller.bin
    ;;

  monitor)
    ensure_pyserial
    echo "🖥️   Opening monitor on ${PORT} — exit with Ctrl+]"
    python3 -m serial.tools.miniterm "$PORT" 115200
    ;;

  "flash monitor")
    ensure_esptool
    ensure_pyserial
    echo "⚡  Flashing to ${PORT}..."
    cd firmware
    esptool.py --chip esp32p4 --port "$PORT" --baud 460800 \
      write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB \
      0x2000 bootloader/bootloader.bin \
      0x8000 partition_table/partition-table.bin \
      0x10000 wiper_controller.bin
    cd ..
    echo "🖥️   Opening monitor on ${PORT} — exit with Ctrl+]"
    python3 -m serial.tools.miniterm "$PORT" 115200
    ;;

  menuconfig)
    ensure_image
    $COMPOSE run --rm idf idf.py -B "$BUILD_DIR" menuconfig
    ;;

  clean)
    run_idf fullclean
    echo "🧹  Build cache cleared."
    ;;

  firmware)
    copy_firmware
    ;;

  shell)
    ensure_image
    $COMPOSE run --rm idf /bin/bash
    ;;

  rebuild)
    build_image
    ;;

  help|*)
    cat <<EOF
Usage: ./idf.sh <command> [options]

Commands:
  build              Set target + build firmware, copy .bin/.elf to ./firmware/
  flash              Flash the device from the host (macOS/Linux)
  monitor            Open serial monitor from the host
  flash monitor      Flash then open monitor
  menuconfig         Open Kconfig menu (runs inside container)
  clean              Run idf.py fullclean
  firmware           Re-copy built firmware to ./firmware/ without rebuilding
  shell              Drop into an interactive container shell
  rebuild            Rebuild the Docker image

Environment variables:
  FLASH_PORT         Serial port, default /dev/tty.usbmodem101
                     Find yours with: ls /dev/tty.*
  IDF_TARGET         ESP-IDF target chip, default esp32p4

Examples:
  ./idf.sh build
  ./idf.sh flash
  FLASH_PORT=/dev/tty.usbserial-0001 ./idf.sh flash
  ./idf.sh monitor
  ./idf.sh flash monitor
EOF
    ;;
esac