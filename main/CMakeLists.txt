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
