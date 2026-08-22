idf_component_register(
    SRCS "render_core.cpp"
    INCLUDE_DIRS "include"
    REQUIRES tasks fx event_arbiter input_conditioner config_store led_driver
    PRIV_REQUIRES esp_timer
)
