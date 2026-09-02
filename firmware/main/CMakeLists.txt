idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_update crash_log nvs_flash config_store input_conditioner render_core
                  led_driver
                  net_services status_led ui_button dev_console
)
