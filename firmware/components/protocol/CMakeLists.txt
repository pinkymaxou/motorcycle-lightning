# The protocol contract: proto/ws_protocol.proto is the single source of
# truth, and nanopb regenerates the C bindings from it on every build. It
# lives in its own component because two of them need it — net_services for
# the wire, config_store for what it writes to flash — and neither may depend
# on the other.
set(PROTO_DIR "${COMPONENT_DIR}/proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(PROTO_SRC "${PROTO_GEN_DIR}/ws_protocol.pb.c")
set(PROTO_HDR "${PROTO_GEN_DIR}/ws_protocol.pb.h")

# The component's only source is generated. The add_custom_command below
# marks it GENERATED for us — an explicit set_source_files_properties() here
# would break IDF's script-mode discovery pass, which cannot run it.
idf_component_register(
    SRCS "${PROTO_SRC}"
    REQUIRES nanopb
)

# The header is generated too, so the include path is attached to the target
# rather than declared as a component directory.
target_include_directories(${COMPONENT_LIB} PUBLIC "${PROTO_GEN_DIR}")

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
