cmake_minimum_required(VERSION 3.16)

# Keep sdkconfig in the build dir (named volume), not the source tree (macOS bind mount)
# This prevents the infinite CMake re-run loop on macOS/Docker Desktop
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wiper_controller)