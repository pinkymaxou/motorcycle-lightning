idf_component_register(
    SRCS "config_nvs.cpp" "config_proto.cpp" "config_rules.cpp"
    INCLUDE_DIRS "include"
    REQUIRES fx nvs_flash protocol
)
