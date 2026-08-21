# nanopb 0.4.9.1 — runtime Protocol Buffers minimal (encode/décode par flux, callbacks,
# zéro allocation). Vendorisé pour un build reproductible (comme bluepad32/btstack).
idf_component_register(
    SRCS "pb_common.c" "pb_encode.c" "pb_decode.c"
    INCLUDE_DIRS "."
)
