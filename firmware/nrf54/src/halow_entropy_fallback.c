#include <mbedtls/entropy.h>
#include <zephyr/random/random.h>

#if defined(CONFIG_WIFI_MORSE_SM) && !defined(CONFIG_MBEDTLS_ENTROPY_POLL_ZEPHYR)
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	(void)data;

	if (output == NULL || olen == NULL || len == 0U) {
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	}

	if (sys_csrand_get(output, len) != 0) {
		return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
	}

	*olen = len;
	return 0;
}
#endif
