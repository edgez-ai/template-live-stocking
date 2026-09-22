#ifndef MBEDTLS_HALOW_CONFIG_H
#define MBEDTLS_HALOW_CONFIG_H

/* Zephyr 4.2's mbedTLS wrapper does not derive OID support from PK_WRITE_C,
 * but mbedTLS 3.6 requires it for public key writing.
 */
#define MBEDTLS_OID_C

#endif /* MBEDTLS_HALOW_CONFIG_H */
