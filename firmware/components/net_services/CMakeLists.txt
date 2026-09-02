# HTTP + WebSocket services. The protocol bindings come from the `protocol`
# component, which owns the .proto and regenerates them on every build.
idf_component_register(
    SRCS "wifi_ap.cpp" "http_api.cpp" "static_files.cpp" "ws_stream.cpp"
         "ota_update.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES config_store
    PRIV_REQUIRES tasks crash_log protocol nanopb lwip
                  esp_wifi esp_netif esp_event esp_http_server
                  render_core fx input_conditioner status_led esp_timer esp_app_format
                  app_update
    EMBED_FILES "webui_dist/index.html.gz"
)
