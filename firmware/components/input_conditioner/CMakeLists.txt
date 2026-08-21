idf_component_register(
    SRCS "blinker.cpp" "input_conditioner.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_timer
)
