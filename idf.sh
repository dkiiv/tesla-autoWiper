#!/usr/bin/env bash
# idf.sh — wrapper around docker compose for ESP-IDF builds
#
# Supports: macOS, Linux, Windows (Git Bash or WSL)
#
# Usage:
#   ./idf.sh build
#   ./idf.sh flash
#   ./idf.sh monitor
#   ./idf.sh flash monitor
#   ./idf.sh menuconfig
#   ./idf.sh shell
#   ./idf.sh clean
#   FLASH_PORT=/dev/tty.usbserial-0001 ./idf.sh flash   # macOS
#   FLASH_PORT=COM3                    ./idf.sh flash   # Windows

set -euo pipefail

COMPOSE="docker compose"
BUILD_DIR="/build"   # named volume — keeps build OFF the host filesystem

# ── Git Bash path conversion fix ─────────────────────────────────────────────
# Git Bash (MINGW) auto-converts absolute Unix paths like /build into
# Windows paths like C:/Program Files/Git/build, breaking Docker args.
# Disabling it here fixes -B /build and volume mount paths.
case "$(uname -s)" in
  CYGWIN*|MINGW*|MSYS*)
    export MSYS_NO_PATHCONV=1
    export MSYS2_ARG_CONV_EXCL="*"
    ;;
esac

# ── OS detection ──────────────────────────────────────────────────────────────

detect_os() {
  case "$(uname -s)" in
    Darwin)  echo "macos"   ;;
    Linux)
      # WSL reports Linux but needs Windows-style COM ports
      if grep -qi microsoft /proc/version 2>/dev/null; then
        echo "wsl"
      else
        echo "linux"
      fi
      ;;
    CYGWIN*|MINGW*|MSYS*)   echo "windows" ;;
    *)                       echo "unknown" ;;
  esac
}

OS="$(detect_os)"

# ── Default port per OS ───────────────────────────────────────────────────────

default_port() {
  case "$OS" in
    macos)    echo "/dev/tty.usbmodem101" ;;
    linux)    echo "/dev/ttyUSB0"         ;;
    wsl)      echo "COM3"                 ;;
    windows)  echo "COM3"                 ;;
    *)        echo "/dev/ttyUSB0"         ;;
  esac
}

PORT="${FLASH_PORT:-$(default_port)}"

# ── Python command detection ──────────────────────────────────────────────────
# Windows has a fake 'python' stub from the Microsoft Store that responds to
# 'command -v' but isn't real Python. We verify by actually running it.
PYTHON=""
for candidate in python3 python py; do
  if $candidate --version &>/dev/null 2>&1; then
    PYTHON="$candidate"
    break
  fi
done

if [ -z "$PYTHON" ]; then
  echo "❌  Python not found."
  echo "    Install from https://python.org (check 'Add to PATH' during install)"
  echo "    Then disable the Microsoft Store stub:"
  echo "    Settings > Apps > Advanced app settings > App execution aliases"
  echo "    → turn off 'python.exe' and 'python3.exe'"
  exit 1
fi

# ── pip helper — handles externally-managed Python on modern macOS/Linux ──────

pip_install() {
  if $PYTHON -m pip install "$@" 2>/dev/null; then
    return 0
  fi
  $PYTHON -m pip install --break-system-packages "$@"
}

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
  if ! $PYTHON -m esptool version &>/dev/null; then
    echo "📦  esptool not found — installing..."
    pip_install "esptool==4.8.1"
  fi
}

ensure_pyserial() {
  if ! $PYTHON -c "import serial" &>/dev/null; then
    echo "📦  pyserial not found — installing..."
    pip_install pyserial
  fi
}

do_flash() {
  ensure_esptool
  echo "⚡  Flashing to ${PORT}  [${OS}]..."
  cd firmware
  $PYTHON -m esptool --chip esp32p4 --port "$PORT" --baud 460800 \
    write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB \
    0x2000 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0x10000 wiper_controller.bin
  cd ..
}

do_monitor() {
  ensure_pyserial
  echo "🖥️   Opening monitor on ${PORT}  [${OS}] — exit with Ctrl+]"
  $PYTHON -m serial.tools.miniterm "$PORT" 115200 --filter colorize
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
    do_flash
    ;;

  monitor)
    do_monitor
    ;;

  "flash monitor")
    do_flash
    do_monitor
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

  port)
    # Utility: show detected OS and available serial ports
    echo "Detected OS: ${OS}"
    echo "Current FLASH_PORT: ${PORT}"
    echo ""
    case "$OS" in
      macos)
        echo "Available ports:"
        ls /dev/tty.* 2>/dev/null || echo "  (none found)"
        ;;
      linux|wsl)
        echo "Available ports:"
        ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "  (none found)"
        if [ "$OS" = "wsl" ]; then
          echo ""
          echo "Windows COM ports visible from WSL:"
          ls /dev/ttyS* 2>/dev/null | head -10 || echo "  (none found)"
          echo "  Note: COM3 = /dev/ttyS2, COM4 = /dev/ttyS3, etc."
        fi
        ;;
      windows)
        echo "Run in PowerShell to list COM ports:"
        echo "  Get-PnpDevice -Class Ports | Select-Object FriendlyName"
        ;;
    esac
    ;;

  help|*)
    cat <<EOF
Usage: ./idf.sh <command> [options]

Commands:
  build              Set target + build firmware, copy .bin/.elf to ./firmware/
  flash              Flash the device (runs on host, not in container)
  monitor            Open serial monitor (runs on host)
  flash monitor      Flash then open monitor
  menuconfig         Open Kconfig menu (runs inside container)
  clean              Run idf.py fullclean
  firmware           Re-copy built firmware to ./firmware/ without rebuilding
  shell              Drop into an interactive container shell
  rebuild            Rebuild the Docker image
  port               Show detected OS and available serial ports

Environment variables:
  FLASH_PORT         Override serial port
  IDF_TARGET         ESP-IDF target chip (default: esp32p4)

Default ports by OS:
  macOS              /dev/tty.usbmodem101   (find yours: ./idf.sh port)
  Linux              /dev/ttyUSB0           (find yours: ./idf.sh port)
  WSL / Windows      COM3                   (find yours: ./idf.sh port)

Examples:
  ./idf.sh build
  ./idf.sh flash
  ./idf.sh monitor
  ./idf.sh flash monitor

  FLASH_PORT=/dev/tty.usbserial-0001 ./idf.sh flash   # macOS custom port
  FLASH_PORT=COM4 ./idf.sh flash                       # Windows/WSL

Windows note:
  Requires Git Bash (https://gitforwindows.org) or WSL.
  Docker Desktop must be running for build/menuconfig/shell/clean.
EOF
    ;;
esac