idf_component_register(
    SRCS
        "main.c"
        "can_gateway.c"
        "lin_sensor.c"
        "wiper_logic.c"
        "wiper_policy.c"   # pure C, host-tested (test/)
        "protocol.c"       # pure C, host-tested (test/)
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer
)

# CAN bitrate has no default (see config.h).  `idf.py -DCAN_BITRATE_KBPS=500`
# (or `CAN_BITRATE_KBPS=500 ./idf.sh build`) passes it in as a define.
if(DEFINED CAN_BITRATE_KBPS AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CAN_BITRATE_KBPS MATCHES "^(125|250|500|1000)$")
        message(FATAL_ERROR "CAN_BITRATE_KBPS=${CAN_BITRATE_KBPS}: use 125, 250, 500 or 1000")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CAN_BITRATE_KBPS=${CAN_BITRATE_KBPS})
    message(STATUS "CAN_BITRATE_KBPS=${CAN_BITRATE_KBPS}")
endif()

# Sniffer build (listen-only, nothing forwarded): `idf.py -DWIPER_SNIFFER=1`
# or `SNIFFER=1 ./idf.sh build`.  Cached like the bitrate; idf.sh always
# passes it explicitly so a previous sniffer build can't leak into a normal one.
if(DEFINED WIPER_SNIFFER AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT WIPER_SNIFFER MATCHES "^(0|1)$")
        message(FATAL_ERROR "WIPER_SNIFFER=${WIPER_SNIFFER}: use 0 or 1")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CAN_LISTEN_ONLY=${WIPER_SNIFFER})
    message(STATUS "WIPER_SNIFFER=${WIPER_SNIFFER}")
endif()
