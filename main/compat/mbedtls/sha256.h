// Compatibility wrapper: ESP-IDF v6.x (mbedtls 4.x) moved sha256.h
// to mbedtls/private/sha256.h.  This shim makes the old include path
// work for components (e.g. esp-dl fbs_loader) that haven't been
// updated yet.
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include <mbedtls/private/sha256.h>
#undef MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
