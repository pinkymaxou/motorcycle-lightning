idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash config_store input_conditioner render_core
                  net_services status_led ui_button dev_console
)
