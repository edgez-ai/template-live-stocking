#ifndef EDGEZ_CONFIG_H
#define EDGEZ_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGEZ_VENDOR_IES_MAX_LEN 267

struct edgez_halow_profile {
	char mesh_id[33];
	char passphrase[65];
	uint32_t mesh_frequency_khz;
	uint32_t mesh_bandwidth_mhz;
	uint32_t beacon_interval_seconds;
	uint32_t device_type;
	bool sleep_mode_enabled;
	bool device_gps_enabled;
};

void edgez_config_init(const char *mesh_id, const char *passphrase);
void edgez_config_get_profile(struct edgez_halow_profile *profile);
uint32_t edgez_config_generation(void);
void edgez_config_set_halow_ready(bool ready);
bool edgez_config_halow_beacon_started(void);
uint32_t edgez_config_halow_beacon_build_count(void);
size_t edgez_config_halow_beacon_last_ies_len(void);
bool edgez_config_is_complete(void);
bool edgez_config_should_autostart(void);

int edgez_config_handle_packet(const uint8_t *payload, size_t payload_len,
			       uint8_t *response, size_t response_cap,
			       size_t *response_len);
int edgez_config_build_vendor_ies(uint8_t *out, size_t out_cap,
				  const uint8_t current_mac[6], size_t *out_len);

#endif
