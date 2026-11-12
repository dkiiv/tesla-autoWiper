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
