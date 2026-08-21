idf_component_register(
    SRCS "led_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_rmt config_store
)
