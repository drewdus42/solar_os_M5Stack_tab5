idf_component_register(
    SRCS
        "src/crypto.c"
        "src/wireguard.c"
        "src/wireguardif.c"
        "src/crypto/refc/blake2s.c"
        "src/crypto/refc/chacha20.c"
        "src/crypto/refc/chacha20poly1305.c"
        "src/crypto/refc/poly1305-donna.c"
        "src/crypto/refc/x25519.c"
    INCLUDE_DIRS "src"
    REQUIRES lwip
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-but-set-variable
    -Wno-unused-variable
    -Wno-error=unterminated-string-initialization
    -Wno-unterminated-string-initialization
)
