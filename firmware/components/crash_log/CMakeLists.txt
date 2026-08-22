idf_component_register(
    SRCS "crash_log.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_system
)
