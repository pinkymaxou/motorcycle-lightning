idf_component_register(
    SRCS "status_led.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer esp_driver_rmt
)
