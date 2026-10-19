/* the vita has no bsd sockets, posix timing or /dev/urandom for mbedtls to
 * lean on: the lobby code does its own socket i/o and feeds it entropy from
 * the hardware rng through mbedtls_hardware_poll() */
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
