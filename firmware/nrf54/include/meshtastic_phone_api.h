#ifndef MESHTASTIC_PHONE_API_H
#define MESHTASTIC_PHONE_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MESHTASTIC_PHONE_API_MAX_PACKET_SIZE 512
#define MESHTASTIC_HALOW_MESH_ID_MAX_LEN 32
#define MESHTASTIC_HALOW_PSK_MAX_LEN 32
#define MESHTASTIC_HALOW_PSK_B64_MAX_LEN (((MESHTASTIC_HALOW_PSK_MAX_LEN + 2) / 3) * 4)
#define MESHTASTIC_HALOW_PSK_HEX_MAX_LEN (MESHTASTIC_HALOW_PSK_MAX_LEN * 2)
#define MESHTASTIC_HALOW_PASSPHRASE_MAX_LEN 100
#define MESHTASTIC_HALOW_DISCOVERY_VENDOR_IE_BUF_LEN (2 * (2 + 255))

struct meshtastic_halow_profile {
	char mesh_id[MESHTASTIC_HALOW_MESH_ID_MAX_LEN + 1];
	uint8_t psk[MESHTASTIC_HALOW_PSK_MAX_LEN];
	size_t psk_len;
	char psk_b64[MESHTASTIC_HALOW_PSK_B64_MAX_LEN + 1];
	char psk_hex[MESHTASTIC_HALOW_PSK_HEX_MAX_LEN + 1];
	char passphrase[MESHTASTIC_HALOW_PASSPHRASE_MAX_LEN + 1];
	size_t passphrase_len;
	bool has_primary_psk;
	uint32_t mesh_frequency_khz;
	uint32_t mesh_bandwidth_mhz;
	uint32_t beacon_interval_seconds;
};

struct meshtastic_halow_node_summary {
	uint32_t node_num;
	uint8_t hw_model;
	uint8_t role;
	bool is_licensed;
	char short_name[5];
	char long_name[40];
	uint8_t public_key[32];
	size_t public_key_len;
};

typedef void (*meshtastic_phone_api_from_radio_notify_cb_t)(void);

void meshtastic_phone_api_init(uint32_t node_num);
void meshtastic_phone_api_register_halow_rx(void);
void meshtastic_phone_api_close(void);
bool meshtastic_phone_api_handle_to_radio(const uint8_t *buf, size_t len);
size_t meshtastic_phone_api_get_from_radio(uint8_t *buf, size_t len);
bool meshtastic_phone_api_available(void);
bool meshtastic_phone_api_is_connected(void);
bool meshtastic_phone_api_send_our_node_info_beacon(void);
void meshtastic_phone_api_set_from_radio_notify_cb(
	meshtastic_phone_api_from_radio_notify_cb_t callback);
uint32_t meshtastic_phone_api_get_notify_num(uint32_t requested_from_radio_num);
uint32_t meshtastic_phone_api_get_node_num(void);
bool meshtastic_phone_api_get_halow_profile(struct meshtastic_halow_profile *profile,
					    const char *fallback_mesh_id);
bool meshtastic_phone_api_get_local_node_summary(struct meshtastic_halow_node_summary *summary);
size_t meshtastic_phone_api_build_halow_discovery_vendor_ies(uint8_t *out, size_t out_len);
bool meshtastic_phone_api_handle_halow_discovery_ies(const uint8_t *ies, size_t ies_len,
						     int8_t rssi);

#endif
