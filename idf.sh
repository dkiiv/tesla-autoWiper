#!/usr/bin/env bash
# idf.sh — wrapper around docker compose for ESP-IDF builds
#
# Usage:
#   ./idf.sh build
#   ./idf.sh flash
#   ./idf.sh monitor
#   ./idf.sh flash monitor
#   ./idf.sh menuconfig
#   ./idf.sh shell
#   ./idf.sh clean
#   FLASH_PORT=/dev/tty.usbserial-0001 ./idf.sh flash

set -euo pipefail

COMPOSE="docker compose"
BUILD_DIR="/build"   # named volume mount point — keeps build OFF the macOS bind mount

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
  # The build volume is named <project>_idf-build-cache
  local vol
  vol="$(basename "$(pwd)" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9-')_idf-build-cache"
  docker run --rm \
    -v "${vol}:/build_vol" \
    -v "$(pwd)/${out_dir}":/out \
    busybox sh -c "
      cp /build_vol/*.bin /out/ 2>/dev/null || true
      cp /build_vol/*.elf /out/ 2>/dev/null || true
      cp /build_vol/flasher_args.json /out/ 2>/dev/null || true
    "
  echo "✅  Firmware copied to ${out_dir}/"
  ls -lh "${out_dir}/"
}

# ── main ──────────────────────────────────────────────────────────────────────

CMD="${1:-help}"
shift || true

case "$CMD" in

  build)
    # Run set-target AND build in a single container invocation to avoid
    # timestamp drift between separate docker run calls on macOS
    ensure_image
    $COMPOSE run --rm idf idf.py -B "$BUILD_DIR" \
      set-target "${IDF_TARGET:-esp32p4}" \
      build
    copy_firmware
    ;;

  flash)
    run_idf -p "${FLASH_PORT:-/dev/ttyUSB0}" flash "$@"
    ;;

  monitor)
    ensure_image
    $COMPOSE run --rm idf idf.py -B "$BUILD_DIR" \
      -p "${FLASH_PORT:-/dev/ttyUSB0}" monitor "$@"
    ;;

  "flash monitor")
    ensure_image
    $COMPOSE run --rm idf idf.py -B "$BUILD_DIR" \
      -p "${FLASH_PORT:-/dev/ttyUSB0}" flash monitor "$@"
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
  flash              Flash the device  (FLASH_PORT=... ./idf.sh flash)
  monitor            Open serial monitor
  flash monitor      Flash then monitor
  menuconfig         Open Kconfig menu
  clean              Run idf.py fullclean
  firmware           Re-copy built firmware to ./firmware/ without rebuilding
  shell              Drop into an interactive container shell
  rebuild            Rebuild the Docker image

Environment variables:
  FLASH_PORT         Serial port, default /dev/ttyUSB0
  IDF_TARGET         ESP-IDF target chip, default esp32p4

macOS flashing:
  USB serial devices appear as /dev/tty.usbserial-XXXX on Mac.
  Flash from the host using esptool directly:
    pip install esptool
    cd firmware && esptool.py --chip esp32p4 --port /dev/tty.usbserial-0001 \\
      write_flash @flasher_args.json
EOF
    ;;
esac