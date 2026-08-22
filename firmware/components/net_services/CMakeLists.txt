# The protocol contract lives in proto/ws_protocol.proto; nanopb regenerates
# the C bindings on every build, so the .proto is the single source of truth.
set(PROTO_DIR "${COMPONENT_DIR}/proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(PROTO_SRC "${PROTO_GEN_DIR}/ws_protocol.pb.c")
set(PROTO_HDR "${PROTO_GEN_DIR}/ws_protocol.pb.h")

idf_component_register(
    SRCS "wifi_ap.cpp" "http_api.cpp" "static_files.cpp" "ws_stream.cpp"
         "ota_update.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES tasks config_store crash_log
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server nanopb
                  render_core fx input_conditioner status_led esp_timer esp_app_format
                  app_update
    EMBED_FILES "webui_dist/index.html.gz"
)

# Everything below runs only in the real build pass (component registration
# returns early during IDF's script-mode discovery pass).
find_program(PROTOC_EXE protoc)
find_program(PROTOC_GEN_NANOPB protoc-gen-nanopb)
if(NOT PROTOC_EXE OR NOT PROTOC_GEN_NANOPB)
    message(FATAL_ERROR
        "protoc and protoc-gen-nanopb are required to build the protocol "
        "bindings (Debian/Ubuntu: apt install protobuf-compiler nanopb).")
endif()

add_custom_command(
    OUTPUT "${PROTO_SRC}" "${PROTO_HDR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROTO_GEN_DIR}"
    COMMAND "${PROTOC_EXE}" "--plugin=protoc-gen-nanopb=${PROTOC_GEN_NANOPB}"
            "--nanopb_out=${PROTO_GEN_DIR}" -I "${PROTO_DIR}" ws_protocol.proto
    DEPENDS "${PROTO_DIR}/ws_protocol.proto" "${PROTO_DIR}/ws_protocol.options"
    # the nanopb plugin picks up <proto>.options from the working directory
    WORKING_DIRECTORY "${PROTO_DIR}"
    COMMENT "nanopb: generating ws_protocol.pb.[ch] from the .proto"
    VERBATIM)

add_custom_target(ws_protocol_gen DEPENDS "${PROTO_SRC}" "${PROTO_HDR}")
add_dependencies(${COMPONENT_LIB} ws_protocol_gen)

set_source_files_properties("${PROTO_SRC}" PROPERTIES GENERATED TRUE)
target_sources(${COMPONENT_LIB} PRIVATE "${PROTO_SRC}")
target_include_directories(${COMPONENT_LIB} PRIVATE "${PROTO_GEN_DIR}")
