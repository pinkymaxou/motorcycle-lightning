idf_component_register(
    SRCS "stats_record.cpp" "stats_store.cpp" "stats_flash.cpp"
    INCLUDE_DIRS "include"
    REQUIRES input_conditioner
    PRIV_REQUIRES spi_flash esp_partition esp_timer
)
