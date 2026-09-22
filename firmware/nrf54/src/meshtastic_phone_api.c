#include "meshtastic_phone_api.h"

#include <errno.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>
#if defined(CONFIG_WIFI_MORSE_SM)
#include "morse_mesh.h"
#include <mbedtls/aes.h>
#include <mbedtls/cipher.h>
#include <psa/crypto.h>
#endif
#include "mesh/generated/meshtastic/admin.pb.h"
#include "mesh/generated/meshtastic/channel.pb.h"
#include "mesh/generated/meshtastic/config.pb.h"
#include "mesh/generated/meshtastic/device_ui.pb.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include "mesh/generated/meshtastic/module_config.pb.h"

LOG_MODULE_REGISTER(meshtastic_phone_api, LOG_LEVEL_DBG);

#define SPECIAL_NONCE_ONLY_CONFIG 69420
#define SPECIAL_NONCE_ONLY_NODES 69421
#define MOCK_LORA_CONFIG_COUNT 2
#define MOCK_PRIMARY_CHANNEL_INDEX 0
#define MOCK_CHANNEL_COUNT 8
#define MOCK_NODEDB_MAX 8
#define MOCK_ADMIN_PASSKEY_SIZE 8
#define PHONE_API_FROM_RADIO_QUEUE_DEPTH 8
#define PHONE_API_CONFIG_STORE_COUNT (meshtastic_Config_device_ui_tag + 1)
#define PHONE_API_MODULE_CONFIG_STORE_COUNT (meshtastic_ModuleConfig_tak_tag + 1)
#define PHONE_API_SETTINGS_PREFIX "mesh/phoneapi"
#define PHONE_API_SETTINGS_NODE_NUM PHONE_API_SETTINGS_PREFIX "/node_num"
#define PHONE_API_SETTINGS_NODEDB_COUNT PHONE_API_SETTINGS_PREFIX "/nodedb_count"
#define PHONE_API_SETTINGS_NODEDB PHONE_API_SETTINGS_PREFIX "/nodedb"
#define PHONE_API_SETTINGS_LORA PHONE_API_SETTINGS_PREFIX "/lora"
#define PHONE_API_SETTINGS_CHANNEL PHONE_API_SETTINGS_PREFIX "/channel"
#define PHONE_API_SETTINGS_CONFIGS PHONE_API_SETTINGS_PREFIX "/configs"
#define PHONE_API_SETTINGS_CHANNELS PHONE_API_SETTINGS_PREFIX "/channels"
#define PHONE_API_SETTINGS_MODULE_CONFIGS PHONE_API_SETTINGS_PREFIX "/module_configs"
#define HALOW_MESHTASTIC_HEADER_LEN 16
#define HALOW_MAX_RADIO_BUFFER_LEN 256
#define HALOW_PACKET_FLAGS_HOP_LIMIT_MASK 0x07
#define HALOW_PACKET_FLAGS_WANT_ACK_MASK 0x08
#define HALOW_PACKET_FLAGS_VIA_MQTT_MASK 0x10
#define HALOW_PACKET_FLAGS_HOP_START_MASK 0xe0
#define HALOW_PACKET_FLAGS_HOP_START_SHIFT 5
#define HALOW_MESH_VENDOR_IE_ID 221
#define HALOW_MESH_VENDOR_HEADER_LEN 7
#define HALOW_MESH_VENDOR_MAX_PAYLOAD_LEN 255
#define HALOW_MESH_VENDOR_DATA_LEN (255 - HALOW_MESH_VENDOR_HEADER_LEN)
#define HALOW_MESH_VENDOR_MAX_FRAGS 2
#define HALOW_MESH_VENDOR_IE_TOTAL_LEN (2 + HALOW_MESH_VENDOR_MAX_PAYLOAD_LEN)
#define HALOW_MESH_VENDOR_IE_BUF_LEN (HALOW_MESH_VENDOR_MAX_FRAGS * HALOW_MESH_VENDOR_IE_TOTAL_LEN)
#define HALOW_MESH_VENDOR_NODEINFO_TYPE 1
#define HALOW_MESH_VENDOR_NODEINFO_VERSION 1
#define HALOW_MIN_PRIMARY_PSK_LEN 16
#define HALOW_MAX_PRIMARY_PSK_LEN 32
#define HALOW_DATA_BITFIELD_OK_TO_MQTT_SHIFT 0
#define HALOW_DATA_BITFIELD_WANT_RESPONSE_SHIFT 1
#define MESHTASTIC_DEFAULT_MESH_ID "LongFast"

static const uint8_t meshtastic_default_psk[HALOW_MIN_PRIMARY_PSK_LEN] = {
	0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
	0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

static const char *const meshtastic_default_preset_names[] = {
	MESHTASTIC_DEFAULT_MESH_ID,
	"LongSlow",
	"MediumFast",
	"MediumSlow",
	"ShortFast",
	"ShortSlow",
};

struct halow_packet_header {
	uint32_t to;
	uint32_t from;
	uint32_t id;
	uint8_t flags;
	uint8_t channel;
	uint8_t next_hop;
	uint8_t relay_node;
} __packed;

enum phone_api_state {
	STATE_SEND_NOTHING,
	STATE_SEND_UIDATA,
	STATE_SEND_MY_INFO,
	STATE_SEND_OWN_NODEINFO,
	STATE_SEND_METADATA,
	STATE_SEND_CONFIG,
	STATE_SEND_MODULECONFIG,
	STATE_SEND_OTHER_NODEINFOS,
	STATE_SEND_FILEMANIFEST,
	STATE_SEND_COMPLETE_ID,
	STATE_SEND_PACKETS,
};

struct phone_api_context {
	enum phone_api_state state;
	uint8_t config_state;
	uint32_t config_nonce;
	uint32_t from_radio_num;
	uint32_t packet_num;
	uint32_t node_num;
	uint32_t last_contact_ms;
	bool heartbeat_received;
	uint8_t from_radio_queue_head;
	uint8_t from_radio_queue_tail;
	uint8_t from_radio_queue_count;
	meshtastic_FromRadio from_radio_queue[PHONE_API_FROM_RADIO_QUEUE_DEPTH];
};

static struct phone_api_context api;
static meshtastic_NodeInfo mock_nodedb[MOCK_NODEDB_MAX];
static uint8_t mock_nodedb_count;
static meshtastic_Config stored_configs[PHONE_API_CONFIG_STORE_COUNT];
static meshtastic_ModuleConfig stored_module_configs[PHONE_API_MODULE_CONFIG_STORE_COUNT];
static meshtastic_Channel stored_channels[MOCK_CHANNEL_COUNT];
#if defined(CONFIG_WIFI_MORSE_SM)
static meshtastic_phone_api_from_radio_notify_cb_t from_radio_notify_cb;
static uint8_t halow_profile_channel_hash(const struct meshtastic_halow_profile *profile);
static bool halow_channel_hash_and_key(uint8_t channel_index, uint8_t *hash,
				       uint8_t *expanded_key, size_t *expanded_len);
static bool halow_channel_index_for_hash(uint8_t channel_hash, uint8_t *channel_index);
#endif
#if defined(CONFIG_WIFI_MORSE_SM)
static bool halow_rx_registered;
static bool halow_last_rx_channel_valid;
static uint8_t halow_last_rx_channel;
#endif

BUILD_ASSERT(sizeof(struct halow_packet_header) == HALOW_MESHTASTIC_HEADER_LEN,
	     "HaLow packet header must match Meshtastic RadioBuffer layout");

static const uint8_t halow_mesh_vendor_oui[3] = {'m', 's', 'h'};

static const char *state_name(enum phone_api_state state)
{
	switch (state) {
	case STATE_SEND_NOTHING:
		return "SEND_NOTHING";
	case STATE_SEND_UIDATA:
		return "SEND_UIDATA";
	case STATE_SEND_MY_INFO:
		return "SEND_MY_INFO";
	case STATE_SEND_OWN_NODEINFO:
		return "SEND_OWN_NODEINFO";
	case STATE_SEND_METADATA:
		return "SEND_METADATA";
	case STATE_SEND_CONFIG:
		return "SEND_CONFIG";
	case STATE_SEND_MODULECONFIG:
		return "SEND_MODULECONFIG";
	case STATE_SEND_OTHER_NODEINFOS:
		return "SEND_OTHER_NODEINFOS";
	case STATE_SEND_FILEMANIFEST:
		return "SEND_FILEMANIFEST";
	case STATE_SEND_COMPLETE_ID:
		return "SEND_COMPLETE_ID";
	case STATE_SEND_PACKETS:
		return "SEND_PACKETS";
	default:
		return "UNKNOWN";
	}
}

static void bytes_to_base64(const uint8_t *bytes, size_t len, char *out, size_t out_len)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t pos = 0;
	size_t i = 0;

	if (out_len == 0) {
		return;
	}

	while (i < len && pos + 4 < out_len) {
		size_t remaining = len - i;
		uint32_t octet_a = bytes[i++];
		uint32_t octet_b = remaining > 1 ? bytes[i++] : 0;
		uint32_t octet_c = remaining > 2 ? bytes[i++] : 0;
		uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

		out[pos++] = b64[(triple >> 18) & 0x3f];
		out[pos++] = b64[(triple >> 12) & 0x3f];
		out[pos++] = remaining > 1 ? b64[(triple >> 6) & 0x3f] : '=';
		out[pos++] = remaining > 2 ? b64[triple & 0x3f] : '=';
	}
	out[pos] = '\0';
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_len)
{
	static const char hex[] = "0123456789abcdef";
	size_t pos = 0;

	if (out_len == 0) {
		return;
	}

	for (size_t i = 0; i < len && pos + 2 < out_len; i++) {
		out[pos++] = hex[bytes[i] >> 4];
		out[pos++] = hex[bytes[i] & 0x0f];
	}
	out[pos] = '\0';
}

static const char *to_radio_variant_name(uint32_t tag)
{
	switch (tag) {
	case meshtastic_ToRadio_packet_tag:
		return "packet";
	case meshtastic_ToRadio_want_config_id_tag:
		return "want_config_id";
	case meshtastic_ToRadio_disconnect_tag:
		return "disconnect";
	case meshtastic_ToRadio_xmodemPacket_tag:
		return "xmodemPacket";
	case meshtastic_ToRadio_mqttClientProxyMessage_tag:
		return "mqttClientProxyMessage";
	case meshtastic_ToRadio_heartbeat_tag:
		return "heartbeat";
	default:
		return "unknown";
	}
}

static const char *from_radio_variant_name(uint32_t tag)
{
	switch (tag) {
	case meshtastic_FromRadio_packet_tag:
		return "packet";
	case meshtastic_FromRadio_my_info_tag:
		return "my_info";
	case meshtastic_FromRadio_node_info_tag:
		return "node_info";
	case meshtastic_FromRadio_config_tag:
		return "config";
	case meshtastic_FromRadio_log_record_tag:
		return "log_record";
	case meshtastic_FromRadio_config_complete_id_tag:
		return "config_complete_id";
	case meshtastic_FromRadio_rebooted_tag:
		return "rebooted";
	case meshtastic_FromRadio_moduleConfig_tag:
		return "moduleConfig";
	case meshtastic_FromRadio_channel_tag:
		return "channel";
	case meshtastic_FromRadio_queueStatus_tag:
		return "queueStatus";
	case meshtastic_FromRadio_xmodemPacket_tag:
		return "xmodemPacket";
	case meshtastic_FromRadio_metadata_tag:
		return "metadata";
	case meshtastic_FromRadio_mqttClientProxyMessage_tag:
		return "mqttClientProxyMessage";
	case meshtastic_FromRadio_fileInfo_tag:
		return "fileInfo";
	case meshtastic_FromRadio_clientNotification_tag:
		return "clientNotification";
	case meshtastic_FromRadio_deviceuiConfig_tag:
		return "deviceuiConfig";
	case meshtastic_FromRadio_lockdown_status_tag:
		return "lockdown_status";
	default:
		return "unknown";
	}
}

static const char *config_variant_name(uint32_t tag)
{
	switch (tag) {
	case meshtastic_Config_device_tag:
		return "device";
	case meshtastic_Config_position_tag:
		return "position";
	case meshtastic_Config_power_tag:
		return "power";
	case meshtastic_Config_network_tag:
		return "network";
	case meshtastic_Config_display_tag:
		return "display";
	case meshtastic_Config_lora_tag:
		return "lora";
	case meshtastic_Config_bluetooth_tag:
		return "bluetooth";
	case meshtastic_Config_security_tag:
		return "security";
	case meshtastic_Config_sessionkey_tag:
		return "sessionkey";
	case meshtastic_Config_device_ui_tag:
		return "device_ui";
	default:
		return "unknown";
	}
}

static const char *module_config_variant_name(uint32_t tag)
{
	switch (tag) {
	case meshtastic_ModuleConfig_mqtt_tag:
		return "mqtt";
	case meshtastic_ModuleConfig_serial_tag:
		return "serial";
	case meshtastic_ModuleConfig_external_notification_tag:
		return "external_notification";
	case meshtastic_ModuleConfig_store_forward_tag:
		return "store_forward";
	case meshtastic_ModuleConfig_range_test_tag:
		return "range_test";
	case meshtastic_ModuleConfig_telemetry_tag:
		return "telemetry";
	case meshtastic_ModuleConfig_canned_message_tag:
		return "canned_message";
	case meshtastic_ModuleConfig_audio_tag:
		return "audio";
	case meshtastic_ModuleConfig_remote_hardware_tag:
		return "remote_hardware";
	case meshtastic_ModuleConfig_neighbor_info_tag:
		return "neighbor_info";
	case meshtastic_ModuleConfig_ambient_lighting_tag:
		return "ambient_lighting";
	case meshtastic_ModuleConfig_detection_sensor_tag:
		return "detection_sensor";
	case meshtastic_ModuleConfig_paxcounter_tag:
		return "paxcounter";
	case meshtastic_ModuleConfig_statusmessage_tag:
		return "statusmessage";
	case meshtastic_ModuleConfig_traffic_management_tag:
		return "traffic_management";
	case meshtastic_ModuleConfig_tak_tag:
		return "tak";
	default:
		return "unknown";
	}
}

static size_t encode_from_radio(uint8_t *buf, size_t len, const meshtastic_FromRadio *from_radio)
{
	pb_ostream_t stream = pb_ostream_from_buffer(buf, len);

	if (!pb_encode(&stream, &meshtastic_FromRadio_msg, from_radio)) {
		LOG_ERR("FromRadio encode failed: %s", PB_GET_ERROR(&stream));
		return 0;
	}

	return stream.bytes_written;
}

static bool decode_to_radio(const uint8_t *buf, size_t len, meshtastic_ToRadio *to_radio)
{
	pb_istream_t stream = pb_istream_from_buffer(buf, len);

	*to_radio = (meshtastic_ToRadio)meshtastic_ToRadio_init_zero;
	if (!pb_decode(&stream, &meshtastic_ToRadio_msg, to_radio)) {
		LOG_WRN("ToRadio decode failed len=%u reason=%s", (unsigned int)len,
			PB_GET_ERROR(&stream));
		return false;
	}

	return true;
}

static bool encode_admin_message(uint8_t *buf, size_t len, size_t *out_len,
				 const meshtastic_AdminMessage *admin)
{
	pb_ostream_t stream = pb_ostream_from_buffer(buf, len);

	if (!pb_encode(&stream, &meshtastic_AdminMessage_msg, admin)) {
		LOG_WRN("AdminMessage encode failed: %s", PB_GET_ERROR(&stream));
		return false;
	}

	*out_len = stream.bytes_written;
	return true;
}

static bool encode_user_message(uint8_t *buf, size_t len, size_t *out_len,
			       const meshtastic_User *user)
{
	pb_ostream_t stream = pb_ostream_from_buffer(buf, len);

	if (!pb_encode(&stream, &meshtastic_User_msg, user)) {
		LOG_WRN("User encode failed: %s", PB_GET_ERROR(&stream));
		return false;
	}

	*out_len = stream.bytes_written;
	return true;
}

static bool expand_primary_channel_psk(const uint8_t *psk, size_t psk_len, uint8_t *out_psk,
				      size_t *out_len)
{
	if (psk == NULL || out_psk == NULL || out_len == NULL) {
		return false;
	}

	*out_len = 0;
	if (psk_len == 0) {
		return false;
	}

	if (psk_len == 1) {
		if (psk[0] == 0) {
			return false;
		}

		memcpy(out_psk, meshtastic_default_psk, HALOW_MIN_PRIMARY_PSK_LEN);
		out_psk[HALOW_MIN_PRIMARY_PSK_LEN - 1] = meshtastic_default_psk[HALOW_MIN_PRIMARY_PSK_LEN - 1] +
							 psk[0] - 1;
		*out_len = HALOW_MIN_PRIMARY_PSK_LEN;
		return true;
	}

	memcpy(out_psk, psk, MIN(psk_len, HALOW_MAX_PRIMARY_PSK_LEN));
	if (psk_len < HALOW_MIN_PRIMARY_PSK_LEN) {
		memset(out_psk + psk_len, 0, HALOW_MIN_PRIMARY_PSK_LEN - psk_len);
		*out_len = HALOW_MIN_PRIMARY_PSK_LEN;
	} else if (psk_len < HALOW_MAX_PRIMARY_PSK_LEN && psk_len != HALOW_MIN_PRIMARY_PSK_LEN) {
		memset(out_psk + psk_len, 0, HALOW_MAX_PRIMARY_PSK_LEN - psk_len);
		*out_len = HALOW_MAX_PRIMARY_PSK_LEN;
	} else {
		*out_len = MIN(psk_len, HALOW_MAX_PRIMARY_PSK_LEN);
	}

	return true;
}

static bool encrypt_payload_with_profile_key(uint32_t from_node, uint64_t packet_id, const uint8_t *key,
					   size_t key_len, uint8_t *payload, size_t payload_len)
{
	if (payload == NULL || (payload_len > 0 && key == NULL)) {
		return false;
	}

	if (payload_len == 0) {
		return true;
	}

	if (key_len != 0 && key_len != HALOW_MIN_PRIMARY_PSK_LEN && key_len != HALOW_MAX_PRIMARY_PSK_LEN) {
		LOG_WRN("PhoneAPI Halow PSK length %u invalid for AES-CTR", (unsigned int)key_len);
		return false;
	}

	if (key_len == 0) {
		return true;
	}

	if (payload_len > HALOW_MAX_RADIO_BUFFER_LEN) {
		LOG_WRN("PhoneAPI Halow packet payload too large for encryption id=0x%llx payload=%u max=%u",
			(unsigned long long)packet_id, (unsigned int)payload_len,
			(unsigned int)HALOW_MAX_RADIO_BUFFER_LEN);
		return false;
	}

	psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
	psa_key_id_t key_id = 0;
	psa_status_t status;
	uint8_t counter[16] = {0};
	uint8_t stream_block[16] = {0};
	size_t stream_len = 0;

	psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
	psa_set_key_bits(&attrs, key_len * 8U);
	psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT);
	psa_set_key_algorithm(&attrs, PSA_ALG_ECB_NO_PADDING);
	status = psa_import_key(&attrs, key, key_len, &key_id);
	psa_reset_key_attributes(&attrs);
	if (status != PSA_SUCCESS) {
		LOG_WRN("PhoneAPI Halow AES key import failed status=%d key_len=%u",
			(int)status, (unsigned int)key_len);
		return false;
	}

	memcpy(counter, &packet_id, sizeof(uint64_t));
	memcpy(counter + sizeof(uint64_t), &from_node, sizeof(uint32_t));

	for (size_t off = 0; off < payload_len; off += sizeof(stream_block)) {
		size_t todo = MIN(sizeof(stream_block), payload_len - off);

		status = psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING,
					    counter, sizeof(counter),
					    stream_block, sizeof(stream_block), &stream_len);
		if (status != PSA_SUCCESS || stream_len != sizeof(stream_block)) {
			LOG_WRN("PhoneAPI Halow AES-CTR block failed status=%d stream=%u",
				(int)status, (unsigned int)stream_len);
			(void)psa_destroy_key(key_id);
			return false;
		}

		for (size_t i = 0; i < todo; i++) {
			payload[off + i] ^= stream_block[i];
		}

		for (int i = 15; i >= 12; i--) {
			counter[i]++;
			if (counter[i] != 0) {
				break;
			}
		}
	}

	status = psa_destroy_key(key_id);
	if (status != PSA_SUCCESS) {
		LOG_WRN("PhoneAPI Halow AES key destroy failed status=%d", (int)status);
		return false;
	}
	return true;
}

static void log_decoded_payload_preview(const meshtastic_MeshPacket *packet)
{
	char preview[65];
	char text_preview[129];
	size_t preview_len;

	if (!packet || packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
		return;
	}

	preview_len = MIN(packet->decoded.payload.size, (size_t)64);
	if (preview_len == 0) {
		return;
	}

	memcpy(preview, packet->decoded.payload.bytes, preview_len);
	for (size_t i = 0; i < preview_len; i++) {
		if (!isprint((unsigned char)preview[i])) {
			preview[i] = '.';
		}
	}
	preview[preview_len] = '\0';
	snprintf(text_preview, sizeof(text_preview), "\"%s\"", preview);
	if (packet->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
		size_t text_len = MIN((size_t)128, packet->decoded.payload.size);

		for (size_t i = 0; i < text_len; i++) {
			if (isprint((unsigned char)packet->decoded.payload.bytes[i]) ||
			    packet->decoded.payload.bytes[i] == '\n' ||
			    packet->decoded.payload.bytes[i] == '\r' ||
			    packet->decoded.payload.bytes[i] == '\t') {
				text_preview[i] = (char)packet->decoded.payload.bytes[i];
			} else {
				text_preview[i] = '.';
			}
		}
		text_preview[text_len] = '\0';
		LOG_INF("PhoneAPI HaLow RX text id=0x%x from=0x%x to=0x%x len=%u text=%s",
			packet->id, packet->from, packet->to,
			(unsigned int)packet->decoded.payload.size,
			text_preview);
	}

	LOG_INF("PhoneAPI HaLow RX decoded payload preview id=0x%x from=0x%x to=0x%x len=%u preview=\"%s\"",
		packet->id, packet->from, packet->to, (unsigned int)packet->decoded.payload.size, preview);
}

static bool decode_halow_encrypted_payload(meshtastic_MeshPacket *packet)
{
	uint8_t expanded_key[HALOW_MAX_PRIMARY_PSK_LEN];
	uint8_t plain_buf[sizeof(packet->encrypted.bytes)];
	uint8_t local_hash = 0;
	size_t expanded_len = 0;
	size_t payload_len;

	if (!packet || packet->which_payload_variant != meshtastic_MeshPacket_encrypted_tag) {
		return false;
	}
	payload_len = packet->encrypted.size;
	if (payload_len == 0 || payload_len > sizeof(plain_buf)) {
		return false;
	}

	for (uint8_t ch_index = 0; ch_index < ARRAY_SIZE(stored_channels); ch_index++) {
		pb_istream_t stream;
		meshtastic_Data data = meshtastic_Data_init_zero;

		if (!halow_channel_hash_and_key(ch_index, &local_hash, expanded_key, &expanded_len) ||
		    packet->channel != local_hash) {
			continue;
		}

		memcpy(plain_buf, packet->encrypted.bytes, payload_len);
		if (!encrypt_payload_with_profile_key(packet->from, packet->id, expanded_key,
						      expanded_len, plain_buf, payload_len)) {
			LOG_WRN("PhoneAPI HaLow RX decrypt failed id=0x%x ch_index=%u len=%u",
				packet->id, ch_index, (unsigned int)payload_len);
			continue;
		}

		stream = pb_istream_from_buffer(plain_buf, payload_len);
		if (!pb_decode(&stream, &meshtastic_Data_msg, &data)) {
			LOG_DBG("PhoneAPI HaLow RX Data decode failed id=0x%x ch_index=%u len=%u reason=%s",
				packet->id, ch_index, (unsigned int)payload_len,
				PB_GET_ERROR(&stream));
			continue;
		}
		if (data.portnum == meshtastic_PortNum_UNKNOWN_APP) {
			LOG_DBG("PhoneAPI HaLow RX invalid decoded port id=0x%x ch_index=%u",
				packet->id, ch_index);
			continue;
		}

		if (data.has_bitfield) {
			data.want_response =
				data.want_response ||
				((data.bitfield & (1U << HALOW_DATA_BITFIELD_WANT_RESPONSE_SHIFT)) != 0U);
		}
		packet->channel = ch_index;
		packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
		packet->decoded = data;
		LOG_INF("PhoneAPI HaLow RX decoded id=0x%x from=0x%x to=0x%x ch_index=%u hash=0x%02x port=%u payload=%u want_response=%u",
			packet->id, packet->from, packet->to, ch_index, local_hash, packet->decoded.portnum,
			(unsigned int)packet->decoded.payload.size, packet->decoded.want_response);
		LOG_HEXDUMP_INF(packet->decoded.payload.bytes,
				MIN((size_t)packet->decoded.payload.size, (size_t)64),
				"PhoneAPI HaLow RX decoded payload");
		log_decoded_payload_preview(packet);
		return true;
	}

	for (size_t preset = 0; preset < ARRAY_SIZE(meshtastic_default_preset_names); preset++) {
		pb_istream_t stream;
		meshtastic_Data data = meshtastic_Data_init_zero;
		const char *name = meshtastic_default_preset_names[preset];

		local_hash = 0;
		for (size_t i = 0; name[i] != '\0'; i++) {
			local_hash ^= (uint8_t)name[i];
		}
		for (size_t i = 0; i < sizeof(meshtastic_default_psk); i++) {
			local_hash ^= meshtastic_default_psk[i];
		}
		if (packet->channel != local_hash) {
			continue;
		}

		memcpy(plain_buf, packet->encrypted.bytes, payload_len);
		if (!encrypt_payload_with_profile_key(packet->from, packet->id, meshtastic_default_psk,
						      sizeof(meshtastic_default_psk), plain_buf, payload_len)) {
			LOG_WRN("PhoneAPI HaLow RX preset decrypt failed id=0x%x preset=%s len=%u",
				packet->id, name, (unsigned int)payload_len);
			continue;
		}

		stream = pb_istream_from_buffer(plain_buf, payload_len);
		if (!pb_decode(&stream, &meshtastic_Data_msg, &data)) {
			LOG_DBG("PhoneAPI HaLow RX preset Data decode failed id=0x%x preset=%s len=%u reason=%s",
				packet->id, name, (unsigned int)payload_len,
				PB_GET_ERROR(&stream));
			continue;
		}
		if (data.portnum == meshtastic_PortNum_UNKNOWN_APP) {
			LOG_DBG("PhoneAPI HaLow RX preset invalid decoded port id=0x%x preset=%s",
				packet->id, name);
			continue;
		}

		if (data.has_bitfield) {
			data.want_response =
				data.want_response ||
				((data.bitfield & (1U << HALOW_DATA_BITFIELD_WANT_RESPONSE_SHIFT)) != 0U);
		}
		packet->channel = MOCK_PRIMARY_CHANNEL_INDEX;
		packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
		packet->decoded = data;
		LOG_INF("PhoneAPI HaLow RX decoded default preset id=0x%x preset=%s hash=0x%02x port=%u payload=%u want_response=%u",
			packet->id, name, local_hash, packet->decoded.portnum,
			(unsigned int)packet->decoded.payload.size, packet->decoded.want_response);
		LOG_HEXDUMP_INF(packet->decoded.payload.bytes,
				MIN((size_t)packet->decoded.payload.size, (size_t)64),
				"PhoneAPI HaLow RX decoded preset payload");
		log_decoded_payload_preview(packet);
		return true;
	}

	LOG_INF("PhoneAPI HaLow RX encrypted channel hash mismatch/decode failed id=0x%x ch=0x%02x",
		packet->id, packet->channel);
	return false;
}

static bool decode_admin_message(const meshtastic_Data *data, meshtastic_AdminMessage *admin)
{
	pb_istream_t stream = pb_istream_from_buffer(data->payload.bytes, data->payload.size);

	*admin = (meshtastic_AdminMessage)meshtastic_AdminMessage_init_zero;
	if (!pb_decode(&stream, &meshtastic_AdminMessage_msg, admin)) {
		LOG_WRN("AdminMessage decode failed len=%u reason=%s",
			(unsigned int)data->payload.size, PB_GET_ERROR(&stream));
		return false;
	}

	return true;
}

static bool queue_from_radio(const meshtastic_FromRadio *from_radio)
{
	if (api.from_radio_queue_count >= ARRAY_SIZE(api.from_radio_queue)) {
		const meshtastic_FromRadio *oldest =
			&api.from_radio_queue[api.from_radio_queue_head];

		LOG_WRN("PhoneAPI proto FromRadio queue full, dropping oldest variant=%s(%u) for new=%s(%u)",
			from_radio_variant_name(oldest->which_payload_variant),
			oldest->which_payload_variant,
			from_radio_variant_name(from_radio->which_payload_variant),
			from_radio->which_payload_variant);
		api.from_radio_queue_head =
			(api.from_radio_queue_head + 1) % ARRAY_SIZE(api.from_radio_queue);
		api.from_radio_queue_count--;
	}

	api.from_radio_queue[api.from_radio_queue_tail] = *from_radio;
	api.from_radio_queue_tail =
		(api.from_radio_queue_tail + 1) % ARRAY_SIZE(api.from_radio_queue);
	api.from_radio_queue_count++;
	api.state = STATE_SEND_PACKETS;
#if defined(CONFIG_WIFI_MORSE_SM)
	if (from_radio_notify_cb) {
		from_radio_notify_cb();
	}
#endif
	return true;
}

static bool pop_from_radio(meshtastic_FromRadio *from_radio)
{
	if (api.from_radio_queue_count == 0) {
		return false;
	}

	*from_radio = api.from_radio_queue[api.from_radio_queue_head];
	api.from_radio_queue_head =
		(api.from_radio_queue_head + 1) % ARRAY_SIZE(api.from_radio_queue);
	api.from_radio_queue_count--;
	return true;
}

static void fill_user_for_node(meshtastic_User *user, uint32_t node_num,
			       const char *long_name, const char *short_name)
{
	*user = (meshtastic_User)meshtastic_User_init_zero;
	snprintk(user->id, sizeof(user->id), "!%08x", node_num);
	strncpy(user->long_name, long_name, sizeof(user->long_name) - 1);
	strncpy(user->short_name, short_name, sizeof(user->short_name) - 1);
	user->hw_model = meshtastic_HardwareModel_PRIVATE_HW;
	user->role = meshtastic_Config_DeviceConfig_Role_CLIENT;
	user->is_licensed = true;
}

static void init_lora_config_defaults(void)
{
	stored_configs[meshtastic_Config_lora_tag] =
		(meshtastic_Config)meshtastic_Config_init_zero;
	stored_configs[meshtastic_Config_lora_tag].which_payload_variant =
		meshtastic_Config_lora_tag;
	stored_configs[meshtastic_Config_lora_tag].payload_variant.lora.use_preset = true;
	stored_configs[meshtastic_Config_lora_tag].payload_variant.lora.region =
		meshtastic_Config_LoRaConfig_RegionCode_TW;
	stored_configs[meshtastic_Config_lora_tag].payload_variant.lora.hop_limit = 3;
	stored_configs[meshtastic_Config_lora_tag].payload_variant.lora.tx_enabled = true;
}

static void init_channel_defaults(void)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(stored_channels); i++) {
		stored_channels[i] = (meshtastic_Channel)meshtastic_Channel_init_zero;
		stored_channels[i].index = i;
		stored_channels[i].role = meshtastic_Channel_Role_DISABLED;
	}

	stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].has_settings = true;
	stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].role = meshtastic_Channel_Role_PRIMARY;
	strncpy(stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.name,
		MESHTASTIC_DEFAULT_MESH_ID,
		sizeof(stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.name) - 1);
	stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.psk.size = 1;
	stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.psk.bytes[0] = 1;
}

static void init_config_defaults(void)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(stored_configs); i++) {
		stored_configs[i] = (meshtastic_Config)meshtastic_Config_init_zero;
	}

	stored_configs[meshtastic_Config_device_tag].which_payload_variant =
		meshtastic_Config_device_tag;
	stored_configs[meshtastic_Config_device_tag].payload_variant.device.role =
		meshtastic_Config_DeviceConfig_Role_CLIENT;
	stored_configs[meshtastic_Config_device_tag]
		.payload_variant.device.node_info_broadcast_secs = 10800;

	stored_configs[meshtastic_Config_position_tag].which_payload_variant =
		meshtastic_Config_position_tag;
	stored_configs[meshtastic_Config_position_tag].payload_variant.position.gps_mode =
		meshtastic_Config_PositionConfig_GpsMode_DISABLED;

	stored_configs[meshtastic_Config_power_tag].which_payload_variant =
		meshtastic_Config_power_tag;
	stored_configs[meshtastic_Config_power_tag].payload_variant.power.wait_bluetooth_secs = 60;
	stored_configs[meshtastic_Config_power_tag].payload_variant.power.ls_secs = 300;
	stored_configs[meshtastic_Config_power_tag].payload_variant.power.min_wake_secs = 10;
	stored_configs[meshtastic_Config_power_tag].payload_variant.power.sds_secs = UINT32_MAX;

	stored_configs[meshtastic_Config_network_tag].which_payload_variant =
		meshtastic_Config_network_tag;
	stored_configs[meshtastic_Config_network_tag].payload_variant.network.wifi_enabled = true;
	strncpy(stored_configs[meshtastic_Config_network_tag].payload_variant.network.wifi_ssid,
		"heltec",
		sizeof(stored_configs[meshtastic_Config_network_tag]
			       .payload_variant.network.wifi_ssid) - 1);

	stored_configs[meshtastic_Config_display_tag].which_payload_variant =
		meshtastic_Config_display_tag;

	init_lora_config_defaults();

	stored_configs[meshtastic_Config_bluetooth_tag].which_payload_variant =
		meshtastic_Config_bluetooth_tag;
	stored_configs[meshtastic_Config_bluetooth_tag].payload_variant.bluetooth.enabled = true;
	stored_configs[meshtastic_Config_bluetooth_tag].payload_variant.bluetooth.mode =
		meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN;
	stored_configs[meshtastic_Config_bluetooth_tag].payload_variant.bluetooth.fixed_pin =
		123456;

	stored_configs[meshtastic_Config_security_tag].which_payload_variant =
		meshtastic_Config_security_tag;
	stored_configs[meshtastic_Config_sessionkey_tag].which_payload_variant =
		meshtastic_Config_sessionkey_tag;
	stored_configs[meshtastic_Config_device_ui_tag].which_payload_variant =
		meshtastic_Config_device_ui_tag;
}

static void init_module_config_defaults(void)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(stored_module_configs); i++) {
		stored_module_configs[i] =
			(meshtastic_ModuleConfig)meshtastic_ModuleConfig_init_zero;
	}

	for (uint8_t tag = meshtastic_ModuleConfig_mqtt_tag;
	     tag <= meshtastic_ModuleConfig_tak_tag; tag++) {
		stored_module_configs[tag].which_payload_variant = tag;
	}
}

static void init_node_info(meshtastic_NodeInfo *node_info, uint32_t node_num,
			  const char *long_name, const char *short_name, bool own_node)
{
	*node_info = (meshtastic_NodeInfo)meshtastic_NodeInfo_init_zero;
	node_info->num = node_num;
	node_info->has_user = true;
	fill_user_for_node(&node_info->user, node_num, long_name, short_name);
	node_info->last_heard = k_uptime_get_32() / 1000U;
	node_info->is_favorite = own_node;
	if (!own_node) {
		node_info->snr = 8.0f;
		node_info->has_hops_away = true;
		node_info->hops_away = 0;
	}
}

static bool bytes_nonzero(const uint8_t *bytes, size_t len)
{
	if (!bytes) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (bytes[i] != 0) {
			return true;
		}
	}
	return false;
}

static void sync_own_public_key_from_security_config(void)
{
	const meshtastic_Config *config = &stored_configs[meshtastic_Config_security_tag];
	const meshtastic_Config_SecurityConfig *security;

	if (config->which_payload_variant != meshtastic_Config_security_tag) {
		return;
	}

	security = &config->payload_variant.security;
	if (security->public_key.size == sizeof(mock_nodedb[0].user.public_key.bytes) &&
	    bytes_nonzero(security->public_key.bytes, security->public_key.size)) {
		mock_nodedb[0].user.public_key.size = security->public_key.size;
		memcpy(mock_nodedb[0].user.public_key.bytes, security->public_key.bytes,
		       security->public_key.size);
	} else {
		mock_nodedb[0].user.public_key.size = 0;
		memset(mock_nodedb[0].user.public_key.bytes, 0,
		       sizeof(mock_nodedb[0].user.public_key.bytes));
	}
}

static void derive_short_name_from_long(char *short_name, size_t short_name_len,
				       uint32_t node_num, const char *long_name)
{
	size_t long_len = long_name ? strlen(long_name) : 0;
	size_t derived_len = long_len >= short_name_len ? short_name_len - 1 : long_len;
	if (derived_len >= 4) {
		derived_len = 4;
	}

	if (derived_len == 0) {
		snprintk(short_name, short_name_len, "%04x", node_num & 0xffff);
		return;
	}

	memcpy(short_name, &long_name[long_len - derived_len], derived_len);
	short_name[derived_len] = '\0';
}

static void init_mock_store(void)
{
	init_config_defaults();
	init_module_config_defaults();
	init_channel_defaults();
	mock_nodedb_count = 1;
	init_node_info(&mock_nodedb[0], api.node_num, "HaLow", "", true);
	if (mock_nodedb[0].user.short_name[0] == '\0') {
		snprintk(mock_nodedb[0].user.short_name,
			 sizeof(mock_nodedb[0].user.short_name), "%04x", api.node_num & 0xffff);
	}
}

static bool settings_load_exact(const char *key, void *dst, size_t len)
{
	ssize_t rc = settings_load_one(key, dst, len);

	if (rc == (ssize_t)len) {
		return true;
	}
	if (rc >= 0) {
		LOG_WRN("PhoneAPI settings key=%s size mismatch got=%zd expected=%u",
			key, rc, (unsigned int)len);
	} else {
		LOG_DBG("PhoneAPI settings key=%s not loaded rc=%zd", key, rc);
	}
	return false;
}

static void save_phone_api_store(void)
{
	int err;

	err = settings_save_one(PHONE_API_SETTINGS_NODE_NUM, &api.node_num,
				sizeof(api.node_num));
	if (err) {
		LOG_WRN("PhoneAPI settings save node_num failed err=%d", err);
	}
	err = settings_save_one(PHONE_API_SETTINGS_NODEDB_COUNT, &mock_nodedb_count,
				sizeof(mock_nodedb_count));
	if (err) {
		LOG_WRN("PhoneAPI settings save nodedb_count failed err=%d", err);
	}
	err = settings_save_one(PHONE_API_SETTINGS_NODEDB, mock_nodedb,
				sizeof(mock_nodedb));
	if (err) {
		LOG_WRN("PhoneAPI settings save nodedb failed err=%d", err);
	}
	err = settings_save_one(PHONE_API_SETTINGS_CONFIGS, stored_configs,
				sizeof(stored_configs));
	if (err) {
		LOG_WRN("PhoneAPI settings save configs failed err=%d", err);
	}
	err = settings_save_one(PHONE_API_SETTINGS_CHANNELS, stored_channels,
				sizeof(stored_channels));
	if (err) {
		LOG_WRN("PhoneAPI settings save channels failed err=%d", err);
	}
	err = settings_save_one(PHONE_API_SETTINGS_MODULE_CONFIGS, stored_module_configs,
				sizeof(stored_module_configs));
	if (err) {
		LOG_WRN("PhoneAPI settings save module configs failed err=%d", err);
	}
}

static void load_phone_api_store(void)
{
	int err = settings_subsys_init();
	uint32_t stored_node_num = 0;
	uint8_t stored_count = 0;
	bool have_node_num;
	bool have_count;
	bool have_nodedb;
	bool have_configs;
	bool have_channels;
	bool have_module_configs;
	bool migrated_default_channel = false;
	meshtastic_Config_LoRaConfig legacy_lora;
	meshtastic_Channel legacy_channel;

	if (err && err != -EALREADY) {
		LOG_WRN("PhoneAPI settings init failed err=%d; using defaults", err);
		return;
	}

	have_node_num = settings_load_exact(PHONE_API_SETTINGS_NODE_NUM, &stored_node_num,
					    sizeof(stored_node_num));
	if (!have_node_num) {
		LOG_INF("PhoneAPI settings seeding defaults node=0x%08x", api.node_num);
		save_phone_api_store();
		return;
	}
	if (stored_node_num != api.node_num) {
		LOG_INF("PhoneAPI settings adopting stored node=0x%08x requested=0x%08x",
			stored_node_num, api.node_num);
		api.node_num = stored_node_num;
	}

	have_count = settings_load_exact(PHONE_API_SETTINGS_NODEDB_COUNT, &stored_count,
					 sizeof(stored_count));
	have_nodedb = settings_load_exact(PHONE_API_SETTINGS_NODEDB, mock_nodedb,
					  sizeof(mock_nodedb));
	have_configs = settings_load_exact(PHONE_API_SETTINGS_CONFIGS, stored_configs,
					   sizeof(stored_configs));
	have_channels = settings_load_exact(PHONE_API_SETTINGS_CHANNELS, stored_channels,
					    sizeof(stored_channels));
	have_module_configs = settings_load_exact(PHONE_API_SETTINGS_MODULE_CONFIGS,
						  stored_module_configs,
						  sizeof(stored_module_configs));

	if (have_count && stored_count > 0 && stored_count <= MOCK_NODEDB_MAX && have_nodedb) {
		mock_nodedb_count = stored_count;
	} else {
		LOG_WRN("PhoneAPI settings invalid NodeDB, restoring defaults");
		init_node_info(&mock_nodedb[0], api.node_num, "HaLow", "", true);
		if (mock_nodedb[0].user.short_name[0] == '\0') {
			snprintk(mock_nodedb[0].user.short_name,
				 sizeof(mock_nodedb[0].user.short_name), "%04x", api.node_num & 0xffff);
		}
		mock_nodedb_count = 1;
	}
	if (mock_nodedb[0].user.short_name[0] == '\0') {
		snprintk(mock_nodedb[0].user.short_name, sizeof(mock_nodedb[0].user.short_name), "%04x",
			 api.node_num & 0xffff);
	}
	if (mock_nodedb[0].user.role == (uint8_t)0xff || mock_nodedb[0].user.role == 0) {
		mock_nodedb[0].user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
	}
	if (mock_nodedb[0].user.long_name[0] == '\0') {
		strncpy(mock_nodedb[0].user.long_name, "HaLow", sizeof(mock_nodedb[0].user.long_name) - 1);
	}
	/* nRF54 devices are always licensed; mobile handles entitlement checks. */
	mock_nodedb[0].user.is_licensed = true;
	if (!have_configs) {
		if (settings_load_exact(PHONE_API_SETTINGS_LORA, &legacy_lora,
							  sizeof(legacy_lora))) {
			stored_configs[meshtastic_Config_lora_tag].which_payload_variant =
				meshtastic_Config_lora_tag;
			stored_configs[meshtastic_Config_lora_tag].payload_variant.lora =
				legacy_lora;
			LOG_INF("PhoneAPI settings migrated legacy lora config");
		}
	}
	if (!have_channels) {
		if (settings_load_exact(PHONE_API_SETTINGS_CHANNEL, &legacy_channel,
					sizeof(legacy_channel))) {
			stored_channels[MOCK_PRIMARY_CHANNEL_INDEX] = legacy_channel;
			stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].index =
				MOCK_PRIMARY_CHANNEL_INDEX;
			LOG_INF("PhoneAPI settings migrated legacy primary channel");
		}
	}
	if (!have_module_configs) {
		init_module_config_defaults();
	}
	if (stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].has_settings &&
	    stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].role == meshtastic_Channel_Role_PRIMARY &&
	    stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.psk.size == 1 &&
	    stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.psk.bytes[0] == 1) {
		bool default_name_needs_migration =
			stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.name[0] == '\0' ||
			strcmp(stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.name,
			       "Meshtastic") == 0;

		if (default_name_needs_migration) {
			strncpy(stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.name,
				MESHTASTIC_DEFAULT_MESH_ID,
				sizeof(stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.name) - 1);
			stored_channels[MOCK_PRIMARY_CHANNEL_INDEX]
				.settings.name[sizeof(stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.name) - 1] =
				'\0';
			migrated_default_channel = true;
			LOG_INF("PhoneAPI settings migrated default primary channel name to %s",
				MESHTASTIC_DEFAULT_MESH_ID);
		}
	}

	LOG_INF("PhoneAPI settings loaded node=0x%08x nodedb_count=%u lora_region=%u channel0=\"%s\" module_configs=%u",
		api.node_num, mock_nodedb_count,
		stored_configs[meshtastic_Config_lora_tag].payload_variant.lora.region,
		stored_channels[MOCK_PRIMARY_CHANNEL_INDEX].settings.name,
		have_module_configs);
	if (!have_count || !have_nodedb || !have_configs || !have_channels ||
	    !have_module_configs || migrated_default_channel) {
		save_phone_api_store();
	}
}

static void start_config(uint32_t nonce)
{
	api.config_nonce = nonce;
	api.config_state = 0;
	api.state = nonce == SPECIAL_NONCE_ONLY_NODES ?
		STATE_SEND_OWN_NODEINFO : STATE_SEND_MY_INFO;
	LOG_INF("PhoneAPI proto config start nonce=%u state=%s", nonce,
		state_name(api.state));
}

static void send_config_complete(meshtastic_FromRadio *from_radio)
{
	from_radio->which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
	from_radio->config_complete_id = api.config_nonce;
	api.config_nonce = 0;
	api.config_state = 0;
	api.state = STATE_SEND_PACKETS;
	LOG_INF("PhoneAPI proto config complete");
}

static void fill_my_info(meshtastic_MyNodeInfo *my_info)
{
	*my_info = (meshtastic_MyNodeInfo)meshtastic_MyNodeInfo_init_zero;
	my_info->my_node_num = api.node_num;
	my_info->reboot_count = 1;
	my_info->min_app_version = 30200;
	strncpy(my_info->pio_env, "nrf54l15-halow", sizeof(my_info->pio_env) - 1);
	my_info->firmware_edition = meshtastic_FirmwareEdition_VANILLA;
	my_info->nodedb_count = mock_nodedb_count;
}

static void fill_own_node_info(meshtastic_NodeInfo *node_info)
{
	sync_own_public_key_from_security_config();
	*node_info = mock_nodedb[0];
	node_info->last_heard = k_uptime_get_32() / 1000U;
}

static void fill_metadata(meshtastic_DeviceMetadata *metadata)
{
	*metadata = (meshtastic_DeviceMetadata)meshtastic_DeviceMetadata_init_zero;
	strncpy(metadata->firmware_version, "2.8.10", sizeof(metadata->firmware_version) - 1);
	metadata->device_state_version = 23;
	metadata->hasWifi = true;
	metadata->hasBluetooth = true;
	metadata->role = meshtastic_Config_DeviceConfig_Role_CLIENT;
	metadata->hw_model = meshtastic_HardwareModel_PRIVATE_HW;
}

static void fill_channel(meshtastic_Channel *channel, uint8_t index)
{
	*channel = (meshtastic_Channel)meshtastic_Channel_init_zero;
	if (index < ARRAY_SIZE(stored_channels)) {
		*channel = stored_channels[index];
		channel->index = (int8_t)index;
		return;
	}

	channel->index = (int8_t)index;
	channel->role = meshtastic_Channel_Role_DISABLED;
}

static void fill_config(meshtastic_Config *config, uint8_t tag)
{
	*config = (meshtastic_Config)meshtastic_Config_init_zero;
	if (tag < ARRAY_SIZE(stored_configs) &&
	    stored_configs[tag].which_payload_variant == tag) {
		*config = stored_configs[tag];
		return;
	}

	config->which_payload_variant = tag;
}

static void fill_module_config(meshtastic_ModuleConfig *module_config, uint8_t tag)
{
	*module_config = (meshtastic_ModuleConfig)meshtastic_ModuleConfig_init_zero;
	if (tag < ARRAY_SIZE(stored_module_configs) &&
	    stored_module_configs[tag].which_payload_variant == tag) {
		*module_config = stored_module_configs[tag];
		return;
	}

	module_config->which_payload_variant = tag;
}

static void fill_queue_status(meshtastic_QueueStatus *queue_status)
{
	*queue_status = (meshtastic_QueueStatus)meshtastic_QueueStatus_init_zero;
	queue_status->free = 1;
	queue_status->maxlen = 1;
}

static bool queue_tx_status_for_phone(uint32_t mesh_packet_id, int8_t res)
{
	meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;

	from_radio.which_payload_variant = meshtastic_FromRadio_queueStatus_tag;
	fill_queue_status(&from_radio.queueStatus);
	from_radio.queueStatus.res = res;
	from_radio.queueStatus.mesh_packet_id = mesh_packet_id;
	LOG_INF("PhoneAPI proto queueStatus TX id=0x%x res=%d free=%u max=%u",
		mesh_packet_id, res, from_radio.queueStatus.free, from_radio.queueStatus.maxlen);
	return queue_from_radio(&from_radio);
}

static bool queue_sent_packet_for_phone(const meshtastic_MeshPacket *packet)
{
	meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;

	if (!packet || packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
		return false;
	}

	from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
	from_radio.packet = *packet;
	from_radio.packet.rx_time = k_uptime_get_32() / 1000U;
	from_radio.packet.rx_snr = 1.5f;
	LOG_INF("PhoneAPI proto local echo packet id=0x%x from=0x%x to=0x%x ch=%u port=%u payload_len=%u",
		from_radio.packet.id, from_radio.packet.from, from_radio.packet.to,
		from_radio.packet.channel, from_radio.packet.decoded.portnum,
		(unsigned int)from_radio.packet.decoded.payload.size);
	return queue_from_radio(&from_radio);
}

static bool queue_routing_response_for_phone(const meshtastic_MeshPacket *request,
					     meshtastic_Routing_Error error)
{
	meshtastic_Routing routing = meshtastic_Routing_init_zero;
	meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;
	meshtastic_MeshPacket *packet = &from_radio.packet;
	meshtastic_Data *data = &packet->decoded;
	pb_ostream_t stream;

	if (!request || request->id == 0) {
		return false;
	}

	routing.which_variant = meshtastic_Routing_error_reason_tag;
	routing.error_reason = error;

	from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
	packet->from = request->to != UINT32_MAX ? request->to : api.node_num;
	packet->to = api.node_num;
	packet->id = ++api.packet_num;
	packet->channel = MOCK_PRIMARY_CHANNEL_INDEX;
	packet->rx_time = k_uptime_get_32() / 1000U;
	packet->rx_snr = 1.5f;
	packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	packet->priority = meshtastic_MeshPacket_Priority_ACK;
	data->portnum = meshtastic_PortNum_ROUTING_APP;
	data->request_id = request->id;

	stream = pb_ostream_from_buffer(data->payload.bytes, sizeof(data->payload.bytes));
	if (!pb_encode(&stream, &meshtastic_Routing_msg, &routing)) {
		LOG_WRN("PhoneAPI proto local routing response encode failed request_id=0x%x reason=%s",
			request->id, PB_GET_ERROR(&stream));
		return false;
	}
	data->payload.size = stream.bytes_written;
	LOG_INF("PhoneAPI proto local routing response request_id=0x%x response_id=0x%x err=%u from=0x%x to=0x%x",
		request->id, packet->id, error, packet->from, packet->to);
	return queue_from_radio(&from_radio);
}

static bool queue_routing_ack_for_phone(const meshtastic_MeshPacket *request)
{
	return queue_routing_response_for_phone(request, meshtastic_Routing_Error_NONE);
}

static meshtastic_NodeInfo *find_node(uint32_t node_num);

static uint8_t config_type_to_payload_tag(meshtastic_AdminMessage_ConfigType type)
{
	switch (type) {
	case meshtastic_AdminMessage_ConfigType_DEVICE_CONFIG:
		return meshtastic_Config_device_tag;
	case meshtastic_AdminMessage_ConfigType_POSITION_CONFIG:
		return meshtastic_Config_position_tag;
	case meshtastic_AdminMessage_ConfigType_POWER_CONFIG:
		return meshtastic_Config_power_tag;
	case meshtastic_AdminMessage_ConfigType_NETWORK_CONFIG:
		return meshtastic_Config_network_tag;
	case meshtastic_AdminMessage_ConfigType_DISPLAY_CONFIG:
		return meshtastic_Config_display_tag;
	case meshtastic_AdminMessage_ConfigType_LORA_CONFIG:
		return meshtastic_Config_lora_tag;
	case meshtastic_AdminMessage_ConfigType_BLUETOOTH_CONFIG:
		return meshtastic_Config_bluetooth_tag;
	case meshtastic_AdminMessage_ConfigType_SECURITY_CONFIG:
		return meshtastic_Config_security_tag;
	case meshtastic_AdminMessage_ConfigType_SESSIONKEY_CONFIG:
		return meshtastic_Config_sessionkey_tag;
	case meshtastic_AdminMessage_ConfigType_DEVICEUI_CONFIG:
		return meshtastic_Config_device_ui_tag;
	default:
		return 0;
	}
}

static uint8_t module_config_type_to_payload_tag(meshtastic_AdminMessage_ModuleConfigType type)
{
	switch (type) {
	case meshtastic_AdminMessage_ModuleConfigType_MQTT_CONFIG:
		return meshtastic_ModuleConfig_mqtt_tag;
	case meshtastic_AdminMessage_ModuleConfigType_SERIAL_CONFIG:
		return meshtastic_ModuleConfig_serial_tag;
	case meshtastic_AdminMessage_ModuleConfigType_EXTNOTIF_CONFIG:
		return meshtastic_ModuleConfig_external_notification_tag;
	case meshtastic_AdminMessage_ModuleConfigType_STOREFORWARD_CONFIG:
		return meshtastic_ModuleConfig_store_forward_tag;
	case meshtastic_AdminMessage_ModuleConfigType_RANGETEST_CONFIG:
		return meshtastic_ModuleConfig_range_test_tag;
	case meshtastic_AdminMessage_ModuleConfigType_TELEMETRY_CONFIG:
		return meshtastic_ModuleConfig_telemetry_tag;
	case meshtastic_AdminMessage_ModuleConfigType_CANNEDMSG_CONFIG:
		return meshtastic_ModuleConfig_canned_message_tag;
	case meshtastic_AdminMessage_ModuleConfigType_AUDIO_CONFIG:
		return meshtastic_ModuleConfig_audio_tag;
	case meshtastic_AdminMessage_ModuleConfigType_REMOTEHARDWARE_CONFIG:
		return meshtastic_ModuleConfig_remote_hardware_tag;
	case meshtastic_AdminMessage_ModuleConfigType_NEIGHBORINFO_CONFIG:
		return meshtastic_ModuleConfig_neighbor_info_tag;
	case meshtastic_AdminMessage_ModuleConfigType_AMBIENTLIGHTING_CONFIG:
		return meshtastic_ModuleConfig_ambient_lighting_tag;
	case meshtastic_AdminMessage_ModuleConfigType_DETECTIONSENSOR_CONFIG:
		return meshtastic_ModuleConfig_detection_sensor_tag;
	case meshtastic_AdminMessage_ModuleConfigType_PAXCOUNTER_CONFIG:
		return meshtastic_ModuleConfig_paxcounter_tag;
	case meshtastic_AdminMessage_ModuleConfigType_STATUSMESSAGE_CONFIG:
		return meshtastic_ModuleConfig_statusmessage_tag;
	case meshtastic_AdminMessage_ModuleConfigType_TRAFFICMANAGEMENT_CONFIG:
		return meshtastic_ModuleConfig_traffic_management_tag;
	case meshtastic_AdminMessage_ModuleConfigType_TAK_CONFIG:
		return meshtastic_ModuleConfig_tak_tag;
	default:
		return 0;
	}
}

static bool persist_config(const meshtastic_Config *config)
{
	if (config->which_payload_variant == 0 ||
	    config->which_payload_variant >= ARRAY_SIZE(stored_configs)) {
		LOG_WRN("PhoneAPI proto ignoring unsupported set_config tag=%s(%u)",
			config_variant_name(config->which_payload_variant),
			config->which_payload_variant);
		return false;
	}

	stored_configs[config->which_payload_variant] = *config;
	if (config->which_payload_variant == meshtastic_Config_security_tag) {
		sync_own_public_key_from_security_config();
	}
	save_phone_api_store();
	LOG_INF("PhoneAPI proto persisted config tag=%s(%u)",
		config_variant_name(config->which_payload_variant),
		config->which_payload_variant);
	return true;
}

static bool persist_module_config(const meshtastic_ModuleConfig *module_config)
{
	if (module_config->which_payload_variant == 0 ||
	    module_config->which_payload_variant >= ARRAY_SIZE(stored_module_configs)) {
		LOG_WRN("PhoneAPI proto ignoring unsupported set_module_config tag=%s(%u)",
			module_config_variant_name(module_config->which_payload_variant),
			module_config->which_payload_variant);
		return false;
	}

	stored_module_configs[module_config->which_payload_variant] = *module_config;
	save_phone_api_store();
	LOG_INF("PhoneAPI proto persisted moduleConfig tag=%s(%u)",
		module_config_variant_name(module_config->which_payload_variant),
		module_config->which_payload_variant);
	return true;
}

static void persist_channel(const meshtastic_Channel *channel)
{
	uint8_t index = channel->role == meshtastic_Channel_Role_PRIMARY ?
		MOCK_PRIMARY_CHANNEL_INDEX : (uint8_t)channel->index;

	if (index < ARRAY_SIZE(stored_channels)) {
		stored_channels[index] = *channel;
		stored_channels[index].index = index;
		if (channel->role == meshtastic_Channel_Role_PRIMARY) {
			stored_channels[index].role = meshtastic_Channel_Role_PRIMARY;
		}
		save_phone_api_store();
		LOG_INF("PhoneAPI proto persisted channel index=%u role=%u name=\"%s\" psk_len=%u",
			index, stored_channels[index].role,
			stored_channels[index].settings.name,
			(unsigned int)stored_channels[index].settings.psk.size);
		return;
	}

	LOG_WRN("PhoneAPI proto ignoring unsupported channel set index=%d role=%u",
		channel->index, channel->role);
}

static meshtastic_NodeInfo *find_node(uint32_t node_num)
{
	for (uint8_t i = 0; i < mock_nodedb_count; i++) {
		if (mock_nodedb[i].num == node_num) {
			return &mock_nodedb[i];
		}
	}

	return NULL;
}

#if defined(CONFIG_WIFI_MORSE_SM)
static void remember_node_channel(uint32_t node_num, uint8_t channel_index)
{
	meshtastic_NodeInfo *node = find_node(node_num);

	if (!node || channel_index >= ARRAY_SIZE(stored_channels)) {
		return;
	}
	if (node->channel == channel_index) {
		return;
	}

	node->channel = channel_index;
	save_phone_api_store();
	LOG_INF("PhoneAPI HaLow NodeDB node=0x%08x channel_index=%u", node_num,
		channel_index);
}
#endif

static void queue_node_info_for_phone(const meshtastic_NodeInfo *node_info)
{
	meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;

	from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
	from_radio.node_info = *node_info;
	from_radio.node_info.last_heard = k_uptime_get_32() / 1000U;
	(void)queue_from_radio(&from_radio);
}

void meshtastic_phone_api_set_from_radio_notify_cb(
	meshtastic_phone_api_from_radio_notify_cb_t callback)
{
#if defined(CONFIG_WIFI_MORSE_SM)
	from_radio_notify_cb = callback;
#endif
}

static meshtastic_NodeInfo *upsert_node_from_user(uint32_t node_num,
						  const meshtastic_User *user,
						  int8_t rssi, bool *created)
{
	meshtastic_NodeInfo *node;

	if (created) {
		*created = false;
	}
	if (node_num == 0 || node_num == api.node_num || !user) {
		return NULL;
	}

	node = find_node(node_num);
	if (!node) {
		if (mock_nodedb_count >= ARRAY_SIZE(mock_nodedb)) {
			LOG_WRN("PhoneAPI proto NodeDB full, cannot add node=0x%08x",
				node_num);
			return NULL;
		}
		node = &mock_nodedb[mock_nodedb_count++];
		*node = (meshtastic_NodeInfo)meshtastic_NodeInfo_init_zero;
		node->num = node_num;
		if (created) {
			*created = true;
		}
	}

	node->has_user = true;
	node->user = *user;
	if (node->user.id[0] == '\0') {
		snprintk(node->user.id, sizeof(node->user.id), "!%08x", node_num);
	}
	if (node->user.short_name[0] == '\0') {
		derive_short_name_from_long(node->user.short_name, sizeof(node->user.short_name),
					   node_num, node->user.long_name);
	}
	if (node->user.long_name[0] == '\0') {
		snprintk(node->user.long_name, sizeof(node->user.long_name),
			 "HaLow %04x", node_num & 0xffff);
	}
	node->last_heard = k_uptime_get_32() / 1000U;
	node->snr = 0.0f;
	node->has_hops_away = true;
	node->hops_away = 0;
	save_phone_api_store();
	queue_node_info_for_phone(node);
	LOG_INF("PhoneAPI proto NodeDB upsert node=0x%08x short=\"%s\" long=\"%s\" rssi=%d count=%u",
		node_num, node->user.short_name, node->user.long_name, rssi,
		mock_nodedb_count);
	return node;
}

static void upsert_node_placeholder(uint32_t node_num, int8_t rssi)
{
	meshtastic_User user = meshtastic_User_init_zero;
	char short_name[sizeof(user.short_name)];

	if (find_node(node_num) || node_num == 0 || node_num == api.node_num) {
		return;
	}

	snprintk(user.id, sizeof(user.id), "!%08x", node_num);
	snprintk(user.long_name, sizeof(user.long_name), "HaLow %04x", node_num & 0xffff);
	derive_short_name_from_long(short_name, sizeof(user.short_name), node_num, user.long_name);
	strncpy(user.short_name, short_name, sizeof(user.short_name) - 1);
	user.hw_model = meshtastic_HardwareModel_PRIVATE_HW;
	user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
	(void)upsert_node_from_user(node_num, &user, rssi, NULL);
}

static void remove_node(uint32_t node_num)
{
	for (uint8_t i = 0; i < mock_nodedb_count; i++) {
		if (mock_nodedb[i].num != node_num || i == 0) {
			continue;
		}
		for (uint8_t j = i; j + 1 < mock_nodedb_count; j++) {
			mock_nodedb[j] = mock_nodedb[j + 1];
		}
		mock_nodedb_count--;
		save_phone_api_store();
		LOG_INF("PhoneAPI proto removed node 0x%08x nodedb_count=%u",
			node_num, mock_nodedb_count);
		return;
	}

	LOG_WRN("PhoneAPI proto remove node ignored num=0x%08x", node_num);
}

static void reset_mock_nodedb(bool preserve_favorites)
{
	bool own_favorite = mock_nodedb[0].is_favorite;
	init_node_info(&mock_nodedb[0], api.node_num, "HaLow", "", true);
	if (mock_nodedb[0].user.short_name[0] == '\0') {
		snprintk(mock_nodedb[0].user.short_name,
			 sizeof(mock_nodedb[0].user.short_name), "%04x", api.node_num & 0xffff);
	}
	mock_nodedb_count = 1;
	if (preserve_favorites) {
		mock_nodedb[0].is_favorite = own_favorite;
	}
	save_phone_api_store();
	LOG_INF("PhoneAPI proto reset persistent NodeDB preserve_favorites=%u", preserve_favorites);
}

static void set_node_flag(uint32_t node_num, uint8_t flag, bool value)
{
	meshtastic_NodeInfo *node = find_node(node_num);

	if (node == NULL) {
		LOG_WRN("PhoneAPI proto NodeDB flag ignored missing node=0x%08x", node_num);
		return;
	}

	switch (flag) {
	case meshtastic_AdminMessage_set_favorite_node_tag:
		node->is_favorite = value;
		break;
	case meshtastic_AdminMessage_set_ignored_node_tag:
		node->is_ignored = value;
		break;
	case meshtastic_AdminMessage_toggle_muted_node_tag:
		node->is_muted = !node->is_muted;
		break;
	default:
		break;
	}
	save_phone_api_store();
	LOG_INF("PhoneAPI proto NodeDB node=0x%08x favorite=%u ignored=%u muted=%u",
		node_num, node->is_favorite, node->is_ignored, node->is_muted);
}

static bool queue_admin_response(const meshtastic_MeshPacket *request,
				 const meshtastic_AdminMessage *admin)
{
	size_t admin_len = 0;
	meshtastic_FromRadio response = meshtastic_FromRadio_init_zero;
	meshtastic_MeshPacket *packet = &response.packet;
	meshtastic_Data *data = &packet->decoded;

	response.which_payload_variant = meshtastic_FromRadio_packet_tag;
	packet->from = api.node_num;
	packet->to = request->from;
	packet->id = ++api.packet_num;
	packet->rx_time = k_uptime_get_32() / 1000U;
	packet->rx_snr = 1.5f;
	packet->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
	data->portnum = meshtastic_PortNum_ADMIN_APP;
	data->request_id = request->id;

	if (!encode_admin_message(data->payload.bytes, sizeof(data->payload.bytes),
				  &admin_len, admin)) {
		return false;
	}
	data->payload.size = admin_len;

	if (!queue_from_radio(&response)) {
		return false;
	}
	LOG_INF("PhoneAPI proto queued admin response req=0x%x rsp=0x%x len=%u variant=%u",
		request->id, packet->id, (unsigned int)admin_len,
		admin->which_payload_variant);
	return true;
}

static void fill_admin_session_passkey(meshtastic_AdminMessage *admin)
{
	static const uint8_t passkey[MOCK_ADMIN_PASSKEY_SIZE] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	};

	admin->session_passkey.size = MOCK_ADMIN_PASSKEY_SIZE;
	memcpy(admin->session_passkey.bytes, passkey, sizeof(passkey));
}

static bool handle_admin_packet(const meshtastic_MeshPacket *packet)
{
	meshtastic_AdminMessage admin;
	meshtastic_AdminMessage response = meshtastic_AdminMessage_init_zero;
	uint8_t config_tag;
	uint8_t module_config_tag;

	if (!decode_admin_message(&packet->decoded, &admin)) {
		return false;
	}

	LOG_INF("PhoneAPI proto AdminMessage variant=%u req=0x%x",
		admin.which_payload_variant, packet->id);
	switch (admin.which_payload_variant) {
	case meshtastic_AdminMessage_set_config_tag:
		(void)persist_config(&admin.set_config);
		return false;
	case meshtastic_AdminMessage_set_module_config_tag:
		(void)persist_module_config(&admin.set_module_config);
		return false;
	case meshtastic_AdminMessage_set_channel_tag:
		persist_channel(&admin.set_channel);
		return false;
	case meshtastic_AdminMessage_set_owner_tag:
		mock_nodedb[0].has_user = true;
		mock_nodedb[0].user = admin.set_owner;
		mock_nodedb[0].user.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
		mock_nodedb[0].user.is_licensed = true;
		save_phone_api_store();
		LOG_INF("PhoneAPI proto persisted owner long=\"%s\" short=\"%s\"",
			mock_nodedb[0].user.long_name, mock_nodedb[0].user.short_name);
		return false;
	case meshtastic_AdminMessage_get_owner_request_tag:
		response.which_payload_variant = meshtastic_AdminMessage_get_owner_response_tag;
		response.get_owner_response = mock_nodedb[0].user;
		fill_admin_session_passkey(&response);
		return queue_admin_response(packet, &response);
	case meshtastic_AdminMessage_get_config_request_tag:
		config_tag = config_type_to_payload_tag(admin.get_config_request);
		response.which_payload_variant = meshtastic_AdminMessage_get_config_response_tag;
		fill_config(&response.get_config_response, config_tag);
		fill_admin_session_passkey(&response);
		return queue_admin_response(packet, &response);
	case meshtastic_AdminMessage_get_module_config_request_tag:
		module_config_tag =
			module_config_type_to_payload_tag(admin.get_module_config_request);
		response.which_payload_variant =
			meshtastic_AdminMessage_get_module_config_response_tag;
		fill_module_config(&response.get_module_config_response, module_config_tag);
		fill_admin_session_passkey(&response);
		return queue_admin_response(packet, &response);
	case meshtastic_AdminMessage_get_device_metadata_request_tag:
		response.which_payload_variant =
			meshtastic_AdminMessage_get_device_metadata_response_tag;
		fill_metadata(&response.get_device_metadata_response);
		fill_admin_session_passkey(&response);
		return queue_admin_response(packet, &response);
	case meshtastic_AdminMessage_get_channel_request_tag:
		response.which_payload_variant = meshtastic_AdminMessage_get_channel_response_tag;
		fill_channel(&response.get_channel_response,
			     admin.get_channel_request == 0 ?
				     0 : admin.get_channel_request - 1);
		fill_admin_session_passkey(&response);
		return queue_admin_response(packet, &response);
	case meshtastic_AdminMessage_remove_by_nodenum_tag:
		remove_node(admin.remove_by_nodenum);
		return false;
	case meshtastic_AdminMessage_set_fixed_position_tag:
		mock_nodedb[0].has_position = true;
		mock_nodedb[0].position = admin.set_fixed_position;
		mock_nodedb[0].position.time = admin.set_fixed_position.time ?
			admin.set_fixed_position.time : k_uptime_get_32() / 1000U;
		save_phone_api_store();
		LOG_INF("PhoneAPI proto persisted fixed position lat=%d lon=%d alt=%d",
			mock_nodedb[0].position.latitude_i,
			mock_nodedb[0].position.longitude_i,
			mock_nodedb[0].position.altitude);
		return false;
	case meshtastic_AdminMessage_remove_fixed_position_tag:
		mock_nodedb[0].has_position = false;
		mock_nodedb[0].position = (meshtastic_Position)meshtastic_Position_init_zero;
		save_phone_api_store();
		LOG_INF("PhoneAPI proto removed fixed position");
		return false;
	case meshtastic_AdminMessage_set_time_only_tag:
		mock_nodedb[0].last_heard = admin.set_time_only;
		save_phone_api_store();
		LOG_INF("PhoneAPI proto accepted set_time_only epoch=%u", admin.set_time_only);
		return false;
	case meshtastic_AdminMessage_set_favorite_node_tag:
		set_node_flag(admin.set_favorite_node,
			      meshtastic_AdminMessage_set_favorite_node_tag, true);
		return false;
	case meshtastic_AdminMessage_remove_favorite_node_tag:
		set_node_flag(admin.remove_favorite_node,
			      meshtastic_AdminMessage_set_favorite_node_tag, false);
		return false;
	case meshtastic_AdminMessage_set_ignored_node_tag:
		set_node_flag(admin.set_ignored_node,
			      meshtastic_AdminMessage_set_ignored_node_tag, true);
		return false;
	case meshtastic_AdminMessage_remove_ignored_node_tag:
		set_node_flag(admin.remove_ignored_node,
			      meshtastic_AdminMessage_set_ignored_node_tag, false);
		return false;
	case meshtastic_AdminMessage_toggle_muted_node_tag:
		set_node_flag(admin.toggle_muted_node,
			      meshtastic_AdminMessage_toggle_muted_node_tag, true);
		return false;
	case meshtastic_AdminMessage_nodedb_reset_tag:
		reset_mock_nodedb(admin.nodedb_reset);
		return false;
	default:
		LOG_WRN("PhoneAPI proto unsupported admin variant=%u",
			admin.which_payload_variant);
		return false;
	}
}

#if defined(CONFIG_WIFI_MORSE_SM)
static void log_halow_mesh_radio_frame(const char *direction, uint32_t id, uint32_t from, uint32_t to,
				      uint8_t flags, uint8_t channel, uint8_t next_hop,
				      uint8_t relay_node, uint16_t payload_len)
{
	LOG_INF("PhoneAPI HaLow %s id=0x%x from=0x%x to=0x%x ch=%u flags=0x%02x hop_limit=%u hop_start=%u want_ack=%u via_mqtt=%u next_hop=0x%02x relay_node=0x%02x payload_len=%u",
		direction, id, from, to, channel, flags,
		(unsigned int)(flags & HALOW_PACKET_FLAGS_HOP_LIMIT_MASK),
		(unsigned int)((flags & HALOW_PACKET_FLAGS_HOP_START_MASK) >>
			       HALOW_PACKET_FLAGS_HOP_START_SHIFT),
		(unsigned int)((flags & HALOW_PACKET_FLAGS_WANT_ACK_MASK) != 0U),
		(unsigned int)((flags & HALOW_PACKET_FLAGS_VIA_MQTT_MASK) != 0U),
		next_hop, relay_node, (unsigned int)payload_len);
}

static uint8_t halow_packet_flags_from_mesh(const meshtastic_MeshPacket *packet)
{
	uint8_t flags = packet->hop_limit & HALOW_PACKET_FLAGS_HOP_LIMIT_MASK;

	if (packet->want_ack && packet->to != UINT32_MAX) {
		flags |= HALOW_PACKET_FLAGS_WANT_ACK_MASK;
	}
	if (packet->via_mqtt) {
		flags |= HALOW_PACKET_FLAGS_VIA_MQTT_MASK;
	}
	flags |= (packet->hop_start << HALOW_PACKET_FLAGS_HOP_START_SHIFT) &
		 HALOW_PACKET_FLAGS_HOP_START_MASK;
	return flags;
}

static bool mesh_packet_payload_for_halow(const meshtastic_MeshPacket *packet,
					  const uint8_t **payload, size_t *payload_len,
					  const char **variant)
{
	switch (packet->which_payload_variant) {
	case meshtastic_MeshPacket_encrypted_tag:
		*payload = packet->encrypted.bytes;
		*payload_len = packet->encrypted.size;
		*variant = "encrypted";
		return true;
	case meshtastic_MeshPacket_decoded_tag:
		*payload = NULL;
		*payload_len = 0;
		*variant = "decoded-unencoded";
		return false;
	default:
		*payload = NULL;
		*payload_len = 0;
		*variant = "unsupported";
		return false;
	}
}

static bool encode_decoded_mesh_packet_for_halow(meshtastic_MeshPacket *packet)
{
	uint8_t data_buf[sizeof(packet->encrypted.bytes)];
	uint8_t expanded_key[HALOW_MAX_PRIMARY_PSK_LEN];
	uint8_t channel_hash;
	uint8_t channel_index;
	size_t data_len;
	size_t expanded_len;
	pb_ostream_t data_stream;
	meshtastic_PortNum portnum;

	if (!packet) {
		return false;
	}
	if (packet->which_payload_variant == meshtastic_MeshPacket_encrypted_tag) {
		return true;
	}
	if (packet->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
		LOG_WRN("PhoneAPI HaLow TX cannot encode payload variant=%u id=0x%x",
			packet->which_payload_variant, packet->id);
		return false;
	}
	portnum = packet->decoded.portnum;

	if (packet->from == 0) {
		packet->from = api.node_num;
	}
	if (packet->id == 0) {
		packet->id = ++api.packet_num;
	}
	if (packet->hop_limit == 0) {
		packet->hop_limit = 3;
	}
	if (packet->hop_start == 0) {
		packet->hop_start = packet->hop_limit;
	}
	if (packet->to != UINT32_MAX && packet->to != api.node_num && packet->channel == 0) {
		meshtastic_NodeInfo *node = find_node(packet->to);

		if (node && node->channel != 0) {
			packet->channel = node->channel;
			LOG_INF("PhoneAPI HaLow TX resolved peer channel node=0x%x ch_index=%u",
				packet->to, packet->channel);
		}
	}
	if (packet->to == UINT32_MAX) {
		packet->want_ack = false;
	}
	if (packet->from == api.node_num) {
		packet->decoded.has_bitfield = true;
		packet->decoded.bitfield |= (0U << HALOW_DATA_BITFIELD_OK_TO_MQTT_SHIFT);
		packet->decoded.bitfield |=
			(packet->decoded.want_response ? 1U : 0U) <<
			HALOW_DATA_BITFIELD_WANT_RESPONSE_SHIFT;
	}

	channel_index = packet->channel;
	if (channel_index >= ARRAY_SIZE(stored_channels) ||
	    !halow_channel_hash_and_key(channel_index, &channel_hash, expanded_key, &expanded_len)) {
		LOG_WRN("PhoneAPI HaLow TX invalid channel index=%u id=0x%x, using primary",
			channel_index, packet->id);
		channel_index = MOCK_PRIMARY_CHANNEL_INDEX;
	}
	if (!halow_channel_hash_and_key(channel_index, &channel_hash, expanded_key, &expanded_len)) {
		LOG_WRN("PhoneAPI HaLow TX no usable channel index=%u id=0x%x",
			channel_index, packet->id);
		return false;
	}

	data_stream = pb_ostream_from_buffer(data_buf, sizeof(data_buf));
	if (!pb_encode(&data_stream, &meshtastic_Data_msg, &packet->decoded)) {
		LOG_WRN("PhoneAPI HaLow TX Data encode failed id=0x%x port=%u reason=%s",
			packet->id, packet->decoded.portnum, PB_GET_ERROR(&data_stream));
		return false;
	}
	data_len = data_stream.bytes_written;
	if (data_len > HALOW_MAX_RADIO_BUFFER_LEN - sizeof(struct halow_packet_header)) {
		LOG_WRN("PhoneAPI HaLow TX encoded payload too large id=0x%x len=%u max=%u",
			packet->id, (unsigned int)data_len,
			(unsigned int)(HALOW_MAX_RADIO_BUFFER_LEN - sizeof(struct halow_packet_header)));
		return false;
	}

	if (!encrypt_payload_with_profile_key(packet->from, packet->id, expanded_key, expanded_len,
					      data_buf, data_len)) {
		LOG_WRN("PhoneAPI HaLow TX encryption failed id=0x%x len=%u",
			packet->id, (unsigned int)data_len);
		return false;
	}

	packet->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
	packet->channel = channel_hash;
	packet->encrypted.size = data_len;
	memcpy(packet->encrypted.bytes, data_buf, data_len);
	LOG_INF("PhoneAPI HaLow TX encoded decoded packet id=0x%x port=%u data_len=%u ch_index=%u hash=0x%02x",
		packet->id, portnum, (unsigned int)data_len, channel_index, packet->channel);
	LOG_HEXDUMP_INF(packet->encrypted.bytes, MIN(packet->encrypted.size, (pb_size_t)64),
			"PhoneAPI HaLow TX encoded payload");
	return true;
}

static bool send_halow_mesh_packet(const meshtastic_MeshPacket *packet)
{
	uint8_t radio_buf[HALOW_MAX_RADIO_BUFFER_LEN];
	struct halow_packet_header *header = (struct halow_packet_header *)radio_buf;
	const uint8_t *payload;
	size_t payload_len;
	const char *variant;

	if (!mesh_packet_payload_for_halow(packet, &payload, &payload_len, &variant)) {
		LOG_WRN("PhoneAPI HaLow TX unsupported MeshPacket payload variant=%u id=0x%x",
			packet->which_payload_variant, packet->id);
		return false;
	}

	if (payload_len > sizeof(radio_buf) - sizeof(*header)) {
		LOG_ERR("PhoneAPI HaLow TX payload too large id=0x%x payload=%u max=%u",
			packet->id, (unsigned int)payload_len,
			(unsigned int)(sizeof(radio_buf) - sizeof(*header)));
		return false;
	}

	header->to = packet->to;
	header->from = packet->from ? packet->from : api.node_num;
	header->id = packet->id ? packet->id : ++api.packet_num;
	header->flags = halow_packet_flags_from_mesh(packet);
	header->channel = packet->channel;
	header->next_hop = packet->next_hop;
	header->relay_node = packet->relay_node ? packet->relay_node : (uint8_t)(header->from & 0xffU);
	memcpy(&radio_buf[sizeof(*header)], payload, payload_len);

	size_t radio_len = sizeof(*header) + payload_len;
	const uint32_t tx_ms = k_uptime_get_32();
	log_halow_mesh_radio_frame("TX", header->id, header->from, header->to, header->flags,
				  header->channel, header->next_hop, header->relay_node,
				  (uint16_t)payload_len);
	LOG_INF("PhoneAPI HaLow TX packet id=0x%x variant=%s radio_len=%u tx_ms=%u state=%s",
		header->id, variant, (unsigned int)radio_len, tx_ms, state_name(api.state));
	LOG_HEXDUMP_INF(radio_buf, MIN(radio_len, (size_t)64), "PhoneAPI HaLow TX radio");
	if (payload_len > 0) {
		LOG_HEXDUMP_INF(&radio_buf[sizeof(*header)],
			       MIN(payload_len, (size_t)64),
			       "PhoneAPI HaLow TX payload");
	}

	int rc = morse_mesh_send_radio_buffer(radio_buf, radio_len);
	if (rc < 0) {
		LOG_ERR("PhoneAPI HaLow TX failed id=0x%x rc=%d", header->id, rc);
		return false;
	}
	return true;
}

size_t meshtastic_phone_api_build_halow_discovery_vendor_ies(uint8_t *out, size_t out_len)
{
	struct meshtastic_halow_profile profile;
	struct meshtastic_halow_node_summary summary = {0};
	uint8_t payload[HALOW_MESH_VENDOR_MAX_PAYLOAD_LEN] = {0};
	size_t payload_len = 0;
	size_t in_pos = 0;
	size_t out_pos = 0;
	char short_name_default[5] = {0};
	const char *short_name;
	const char *long_name = "HaLow";
	size_t short_len;
	size_t long_len;
	size_t frag_count;
	uint8_t channel_hash;

	if (!out || out_len == 0 || out_len > MESHTASTIC_HALOW_DISCOVERY_VENDOR_IE_BUF_LEN) {
		return 0;
	}

	(void)meshtastic_phone_api_get_local_node_summary(&summary);
	if (summary.short_name[0]) {
		short_name = summary.short_name;
	} else {
		snprintf(short_name_default, sizeof(short_name_default), "%04x", api.node_num & 0xffff);
		short_name = short_name_default;
	}
	if (summary.long_name[0]) {
		long_name = summary.long_name;
	}

	if (!meshtastic_phone_api_get_halow_profile(&profile, "")) {
		return 0;
	}

	channel_hash = halow_profile_channel_hash(&profile);
	payload[payload_len++] = channel_hash;
	memcpy(&payload[payload_len], &api.node_num, sizeof(api.node_num));
	payload_len += sizeof(api.node_num);
	payload[payload_len++] = summary.hw_model;
	payload[payload_len++] = summary.role;
	payload[payload_len++] = summary.is_licensed ? 1U : 0U;

	short_len = strlen(short_name);
	if (short_len > 39) {
		short_len = 39;
	}
	payload[payload_len++] = (uint8_t)short_len;
	if (payload_len + short_len > sizeof(payload)) {
		return 0;
	}
	memcpy(&payload[payload_len], short_name, short_len);
	payload_len += short_len;

	long_len = strlen(long_name);
	if (long_len > 39) {
		long_len = 39;
	}
	payload[payload_len++] = (uint8_t)long_len;
	if (payload_len + long_len > sizeof(payload)) {
		return 0;
	}
	memcpy(&payload[payload_len], long_name, long_len);
	payload_len += long_len;

	if (summary.public_key_len == 32U) {
		if (payload_len + 1U + summary.public_key_len > sizeof(payload)) {
			return 0;
		}
		payload[payload_len++] = (uint8_t)summary.public_key_len;
		memcpy(&payload[payload_len], summary.public_key, summary.public_key_len);
		payload_len += summary.public_key_len;
	} else {
		if (payload_len + 1U > sizeof(payload)) {
			return 0;
		}
		payload[payload_len++] = 0;
	}

	frag_count = (payload_len + HALOW_MESH_VENDOR_DATA_LEN - 1U) /
		    HALOW_MESH_VENDOR_DATA_LEN;
	if (frag_count == 0 || frag_count > HALOW_MESH_VENDOR_MAX_FRAGS) {
		LOG_WRN("PhoneAPI HaLow discovery vendor payload too large len=%u", (unsigned int)payload_len);
		return 0;
	}

	for (size_t frag = 0; frag < frag_count; frag++) {
		size_t frag_payload_len = payload_len - in_pos;
		if (frag_payload_len > HALOW_MESH_VENDOR_DATA_LEN) {
			frag_payload_len = HALOW_MESH_VENDOR_DATA_LEN;
		}

		if (out_pos + 2 + HALOW_MESH_VENDOR_HEADER_LEN + frag_payload_len > out_len) {
			LOG_WRN("PhoneAPI HaLow discovery vendor IE overflow len=%u cap=%u",
				(unsigned int)payload_len, (unsigned int)out_len);
			return 0;
		}

		out[out_pos++] = HALOW_MESH_VENDOR_IE_ID;
		out[out_pos++] = (uint8_t)(HALOW_MESH_VENDOR_HEADER_LEN + frag_payload_len);
		out[out_pos++] = halow_mesh_vendor_oui[0];
		out[out_pos++] = halow_mesh_vendor_oui[1];
		out[out_pos++] = halow_mesh_vendor_oui[2];
		out[out_pos++] = HALOW_MESH_VENDOR_NODEINFO_TYPE;
		out[out_pos++] = HALOW_MESH_VENDOR_NODEINFO_VERSION;
		out[out_pos++] = (uint8_t)frag;
		out[out_pos++] = (uint8_t)frag_count;
		memcpy(&out[out_pos], &payload[in_pos], frag_payload_len);
		out_pos += frag_payload_len;
		in_pos += frag_payload_len;
	}

	LOG_INF("PhoneAPI HaLow discovery vendor IE node=0x%08x hash=0x%02x len=%u frag_count=%u key_len=%u",
		api.node_num, channel_hash, (unsigned int)out_pos, (unsigned int)frag_count,
		(unsigned int)summary.public_key_len);
	LOG_HEXDUMP_INF(out, out_pos, "PhoneAPI HaLow discovery_vendor_ie");
	return out_pos;
}

bool meshtastic_phone_api_send_our_node_info_beacon(void)
{
	meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
	meshtastic_Data data = meshtastic_Data_init_zero;
	meshtastic_User user = meshtastic_User_init_zero;
	size_t user_len = 0;

	if (mock_nodedb_count == 0) {
		LOG_WRN("PhoneAPI HaLow NodeInfo beacon skipped, nodedb empty");
		return false;
	}

	user = mock_nodedb[0].user;
	snprintk(user.id, sizeof(user.id), "!%08x", api.node_num);
	user.is_licensed = true;
	if (user.hw_model == meshtastic_HardwareModel_UNSET) {
		user.hw_model = meshtastic_HardwareModel_PRIVATE_HW;
	}
	if (user.short_name[0] == '\0') {
		snprintk(user.short_name, sizeof(user.short_name), "%04x", api.node_num & 0xffff);
	}
	if (user.long_name[0] == '\0') {
		strncpy(user.long_name, "HaLow", sizeof(user.long_name) - 1);
	}

	if (!encode_user_message(data.payload.bytes, sizeof(data.payload.bytes), &user_len, &user)) {
		LOG_WRN("PhoneAPI HaLow NodeInfo beacon user encode failed");
		return false;
	}

	data.portnum = meshtastic_PortNum_NODEINFO_APP;
	data.payload.size = user_len;
	data.want_response = true;

	packet.id = ++api.packet_num;
	packet.from = api.node_num;
	packet.to = UINT32_MAX;
	packet.hop_limit = 3;
	packet.hop_start = 3;
	packet.decoded = data;
	packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;

	if (!encode_decoded_mesh_packet_for_halow(&packet)) {
		LOG_WRN("PhoneAPI HaLow NodeInfo beacon encode failed id=0x%x user_len=%u",
			packet.id, (unsigned int)user_len);
		return false;
	}

	return send_halow_mesh_packet(&packet);
}

static void halow_mesh_rx_cb(const uint8_t *radio_buf, size_t radio_len, int8_t rssi,
			     void *user_data)
{
	ARG_UNUSED(user_data);

	if (!radio_buf || radio_len < sizeof(struct halow_packet_header)) {
		LOG_WRN("PhoneAPI HaLow RX too short len=%u", (unsigned int)radio_len);
		return;
	}

	const struct halow_packet_header *header = (const struct halow_packet_header *)radio_buf;
	size_t payload_len = radio_len - sizeof(*header);
	meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;
	meshtastic_MeshPacket *packet = &from_radio.packet;

	if (payload_len > sizeof(packet->encrypted.bytes)) {
		LOG_WRN("PhoneAPI HaLow RX payload too large len=%u", (unsigned int)payload_len);
		return;
	}

	from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
	packet->from = header->from;
	packet->to = header->to;
	packet->id = header->id;
	packet->channel = header->channel;
	packet->hop_limit = header->flags & HALOW_PACKET_FLAGS_HOP_LIMIT_MASK;
	packet->want_ack = (header->flags & HALOW_PACKET_FLAGS_WANT_ACK_MASK) != 0;
	packet->via_mqtt = (header->flags & HALOW_PACKET_FLAGS_VIA_MQTT_MASK) != 0;
	packet->hop_start = (header->flags & HALOW_PACKET_FLAGS_HOP_START_MASK) >>
			    HALOW_PACKET_FLAGS_HOP_START_SHIFT;
	packet->next_hop = header->next_hop;
	packet->relay_node = header->relay_node;
	halow_last_rx_channel = header->channel;
	halow_last_rx_channel_valid = true;
	packet->rx_rssi = rssi;
	packet->rx_snr = 0.0f;
	packet->rx_time = k_uptime_get_32() / 1000U;
	packet->which_payload_variant = meshtastic_MeshPacket_encrypted_tag;
	packet->encrypted.size = payload_len;
	memcpy(packet->encrypted.bytes, &radio_buf[sizeof(*header)], payload_len);
	bool decoded = decode_halow_encrypted_payload(packet);

	upsert_node_placeholder(packet->from, rssi);
	if (decoded) {
		remember_node_channel(packet->from, packet->channel);
	}
	bool queued = queue_from_radio(&from_radio);

	log_halow_mesh_radio_frame("RX", header->id, header->from, header->to, header->flags,
				  header->channel, header->next_hop, header->relay_node,
				  (uint16_t)payload_len);
	LOG_INF("PhoneAPI HaLow RX packet id=0x%x variant=%s payload=%u rssi=%d rx_time=%u hop_start=%u want_ack=%u via_mqtt=%u queued=%d queue_count=%u",
		packet->id, decoded ? "decoded" : "encrypted", (unsigned int)payload_len, rssi, packet->rx_time,
		packet->hop_start, packet->want_ack, packet->via_mqtt, queued,
		(unsigned int)api.from_radio_queue_count);
	LOG_HEXDUMP_INF(radio_buf, MIN(radio_len, (size_t)64), "PhoneAPI HaLow RX radio");
	if (payload_len > 0) {
		LOG_HEXDUMP_INF(&radio_buf[sizeof(*header)],
			       MIN(payload_len, (size_t)64),
			       "PhoneAPI HaLow RX payload");
	}
}
#endif /* defined(CONFIG_WIFI_MORSE_SM) */

void meshtastic_phone_api_init(uint32_t node_num)
{
	memset(&api, 0, sizeof(api));
	api.node_num = node_num ? node_num : 0x054a10;
	api.state = STATE_SEND_NOTHING;
	api.packet_num = 50;
	init_mock_store();
	load_phone_api_store();
	LOG_INF("PhoneAPI proto init node=0x%08x", api.node_num);
}

void meshtastic_phone_api_register_halow_rx(void)
{
#if defined(CONFIG_WIFI_MORSE_SM)
	if (halow_rx_registered) {
		return;
	}
	morse_mesh_register_rx_cb(halow_mesh_rx_cb, NULL);
	halow_rx_registered = true;
#endif
}

void meshtastic_phone_api_close(void)
{
	uint32_t old_from_radio_num = api.from_radio_num;
	enum phone_api_state old_state = api.state;
	uint32_t old_nonce = api.config_nonce;
	uint8_t old_config_state = api.config_state;

	api.state = STATE_SEND_NOTHING;
	api.config_nonce = 0;
	api.config_state = 0;
	api.from_radio_num = 0;
	api.heartbeat_received = false;
	api.from_radio_queue_head = 0;
	api.from_radio_queue_tail = 0;
	api.from_radio_queue_count = 0;
	LOG_INF("PhoneAPI proto closed old_state=%s old_nonce=%u old_cfg=%u old_from_id=%u",
		state_name(old_state), old_nonce, old_config_state, old_from_radio_num);
}

bool meshtastic_phone_api_handle_to_radio(const uint8_t *buf, size_t len)
{
	meshtastic_ToRadio to_radio;

	api.last_contact_ms = k_uptime_get_32();
	if (!decode_to_radio(buf, len, &to_radio)) {
		return false;
	}

	LOG_INF("PhoneAPI proto ToRadio variant=%s(%u) state=%s len=%u",
		to_radio_variant_name(to_radio.which_payload_variant),
		to_radio.which_payload_variant, state_name(api.state), (unsigned int)len);
	switch (to_radio.which_payload_variant) {
	case meshtastic_ToRadio_packet_tag:
	{
		const uint8_t *to_payload = NULL;
		size_t to_payload_len = 0;
		const char *to_payload_variant = "unsupported";
		if (to_radio.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
			to_payload = to_radio.packet.decoded.payload.bytes;
			to_payload_len = to_radio.packet.decoded.payload.size;
			to_payload_variant = "decoded";
		} else {
			(void)mesh_packet_payload_for_halow(&to_radio.packet, &to_payload, &to_payload_len,
							   &to_payload_variant);
		}

		LOG_INF("PhoneAPI proto MeshPacket id=0x%x from=0x%x to=0x%x ch=%u hop_limit=%u hop_start=%u want_ack=%u via_mqtt=%u next_hop=0x%02x relay_node=0x%02x port=%u payload_len=%u payload_variant=%s(%u)",
			to_radio.packet.id, to_radio.packet.from, to_radio.packet.to,
			to_radio.packet.channel, to_radio.packet.hop_limit,
			to_radio.packet.hop_start, to_radio.packet.want_ack,
			to_radio.packet.via_mqtt, to_radio.packet.next_hop,
			to_radio.packet.relay_node,
			to_radio.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag ?
				to_radio.packet.decoded.portnum : meshtastic_PortNum_UNKNOWN_APP,
			(unsigned int)to_payload_len,
			to_payload_variant, to_radio.packet.which_payload_variant);
		if (to_payload && to_payload_len > 0) {
			LOG_HEXDUMP_INF(to_payload, MIN(to_payload_len, (size_t)64),
					"PhoneAPI proto ToRadio payload");
		}
		if (to_radio.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
		    to_radio.packet.decoded.portnum == meshtastic_PortNum_ADMIN_APP) {
			return handle_admin_packet(&to_radio.packet);
		}
	}
#if defined(CONFIG_WIFI_MORSE_SM)
	{
		meshtastic_MeshPacket tx_packet = to_radio.packet;
		meshtastic_MeshPacket echo_packet = to_radio.packet;
		if (!encode_decoded_mesh_packet_for_halow(&tx_packet)) {
			if (tx_packet.id != 0) {
				(void)queue_tx_status_for_phone(tx_packet.id, 32);
			}
			return false;
		}
		bool sent = send_halow_mesh_packet(&tx_packet);
		(void)queue_tx_status_for_phone(tx_packet.id, sent ? 0 : 32);
		if (sent && echo_packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
			bool phone_wanted_ack = echo_packet.want_ack;

			echo_packet.id = tx_packet.id;
			echo_packet.from = tx_packet.from ? tx_packet.from : api.node_num;
			echo_packet.to = tx_packet.to;
			echo_packet.channel = MOCK_PRIMARY_CHANNEL_INDEX;
			echo_packet.hop_limit = tx_packet.hop_limit;
			echo_packet.hop_start = tx_packet.hop_start;
			echo_packet.want_ack = phone_wanted_ack;
			echo_packet.via_mqtt = tx_packet.via_mqtt;
			echo_packet.next_hop = tx_packet.next_hop;
			echo_packet.relay_node = tx_packet.relay_node;
			(void)queue_sent_packet_for_phone(&echo_packet);
			if (phone_wanted_ack) {
				(void)queue_routing_ack_for_phone(&echo_packet);
			}
		}
		return sent;
	}
#else
		return false;
#endif
	case meshtastic_ToRadio_want_config_id_tag:
		start_config(to_radio.want_config_id);
		return true;
	case meshtastic_ToRadio_disconnect_tag:
		LOG_INF("PhoneAPI proto client requested disconnect state=%s nonce=%u cfg=%u",
			state_name(api.state), api.config_nonce, api.config_state);
		meshtastic_phone_api_close();
		return false;
	case meshtastic_ToRadio_heartbeat_tag:
		api.heartbeat_received = true;
		LOG_INF("PhoneAPI proto heartbeat nonce=%u", to_radio.heartbeat.nonce);
		return true;
	default:
		LOG_WRN("PhoneAPI proto unhandled ToRadio variant=%u",
			to_radio.which_payload_variant);
		return false;
	}
}

bool meshtastic_phone_api_available(void)
{
	if (api.from_radio_queue_count > 0) {
		return true;
	}

	switch (api.state) {
	case STATE_SEND_NOTHING:
		return api.heartbeat_received;
	case STATE_SEND_PACKETS:
		return api.heartbeat_received;
	default:
		return true;
	}
}

size_t meshtastic_phone_api_get_from_radio(uint8_t *buf, size_t len)
{
	meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_zero;

	if (pop_from_radio(&from_radio)) {
		goto encode;
	}

	if (api.heartbeat_received) {
		from_radio.which_payload_variant = meshtastic_FromRadio_queueStatus_tag;
		fill_queue_status(&from_radio.queueStatus);
		api.heartbeat_received = false;
		goto encode;
	}

	if (!meshtastic_phone_api_available()) {
		LOG_DBG("PhoneAPI proto FromRadio empty state=%s heartbeat=%u",
			state_name(api.state), api.heartbeat_received);
		return 0;
	}

	switch (api.state) {
	case STATE_SEND_NOTHING:
		return 0;
	case STATE_SEND_MY_INFO:
		from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
		fill_my_info(&from_radio.my_info);
		api.state = STATE_SEND_UIDATA;
		break;
	case STATE_SEND_UIDATA:
		from_radio.which_payload_variant = meshtastic_FromRadio_deviceuiConfig_tag;
		from_radio.deviceuiConfig.version = 1;
		api.state = STATE_SEND_OWN_NODEINFO;
		break;
	case STATE_SEND_OWN_NODEINFO:
		from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
		fill_own_node_info(&from_radio.node_info);
		if (api.config_nonce == SPECIAL_NONCE_ONLY_NODES) {
			api.state = STATE_SEND_OTHER_NODEINFOS;
			api.config_state = 1;
		} else {
			api.state = STATE_SEND_METADATA;
		}
		break;
	case STATE_SEND_METADATA:
		from_radio.which_payload_variant = meshtastic_FromRadio_metadata_tag;
		fill_metadata(&from_radio.metadata);
		api.state = STATE_SEND_OTHER_NODEINFOS;
		api.config_state = 1;
		break;
	case STATE_SEND_CONFIG:
		if (api.config_state < MOCK_LORA_CONFIG_COUNT) {
			from_radio.which_payload_variant = meshtastic_FromRadio_config_tag;
			LOG_INF("PhoneAPI proto prepare Android-mock config tag=%s(%u) idx=%u",
				config_variant_name(meshtastic_Config_lora_tag),
				meshtastic_Config_lora_tag, api.config_state);
			fill_config(&from_radio.config, meshtastic_Config_lora_tag);
		} else {
			from_radio.which_payload_variant = meshtastic_FromRadio_channel_tag;
			LOG_INF("PhoneAPI proto prepare Android-mock primary channel idx=%u",
				MOCK_PRIMARY_CHANNEL_INDEX);
			fill_channel(&from_radio.channel, MOCK_PRIMARY_CHANNEL_INDEX);
		}
		api.config_state++;
		if (api.config_state > MOCK_LORA_CONFIG_COUNT) {
			api.state = STATE_SEND_COMPLETE_ID;
			api.config_state = 0;
		}
		break;
	case STATE_SEND_MODULECONFIG:
		from_radio.which_payload_variant = meshtastic_FromRadio_moduleConfig_tag;
		LOG_INF("PhoneAPI proto prepare moduleConfig tag=%s(%u)",
			module_config_variant_name(api.config_state), api.config_state);
		fill_module_config(&from_radio.moduleConfig, api.config_state);
		api.config_state++;
		if (api.config_state > meshtastic_ModuleConfig_tak_tag) {
			api.state = api.config_nonce == SPECIAL_NONCE_ONLY_CONFIG ?
				STATE_SEND_FILEMANIFEST : STATE_SEND_OTHER_NODEINFOS;
			api.config_state = 0;
		}
		break;
	case STATE_SEND_OTHER_NODEINFOS:
		if (api.config_state < mock_nodedb_count) {
			from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
			from_radio.node_info = mock_nodedb[api.config_state];
			from_radio.node_info.last_heard = k_uptime_get_32() / 1000U;
			api.config_state++;
			break;
		}
		if (api.config_nonce == SPECIAL_NONCE_ONLY_NODES) {
			api.state = STATE_SEND_FILEMANIFEST;
		} else {
			api.state = STATE_SEND_CONFIG;
			api.config_state = 0;
		}
		return meshtastic_phone_api_get_from_radio(buf, len);
	case STATE_SEND_FILEMANIFEST:
		LOG_INF("PhoneAPI proto no file manifest");
		api.state = STATE_SEND_COMPLETE_ID;
		return meshtastic_phone_api_get_from_radio(buf, len);
	case STATE_SEND_COMPLETE_ID:
		send_config_complete(&from_radio);
		break;
	case STATE_SEND_PACKETS:
		return 0;
	default:
		return 0;
	}

encode:
	from_radio.id = ++api.from_radio_num;
	size_t encoded = encode_from_radio(buf, len, &from_radio);
	LOG_INF("PhoneAPI proto FromRadio variant=%s(%u) id=%u len=%u next_state=%s nonce=%u cfg=%u config=%s module=%s",
		from_radio_variant_name(from_radio.which_payload_variant),
		from_radio.which_payload_variant, from_radio.id, (unsigned int)encoded,
		state_name(api.state), api.config_nonce, api.config_state,
		from_radio.which_payload_variant == meshtastic_FromRadio_config_tag ?
			config_variant_name(from_radio.config.which_payload_variant) : "n/a",
		from_radio.which_payload_variant == meshtastic_FromRadio_moduleConfig_tag ?
			module_config_variant_name(from_radio.moduleConfig.which_payload_variant) : "n/a");
	if (encoded > 0) {
		LOG_HEXDUMP_DBG(buf, MIN(encoded, 32), "PhoneAPI proto FromRadio first bytes");
	}
	return encoded;
}

bool meshtastic_phone_api_is_connected(void)
{
	return api.state != STATE_SEND_NOTHING;
}

uint32_t meshtastic_phone_api_get_notify_num(uint32_t requested_from_radio_num)
{
	if (requested_from_radio_num != 0) {
		return requested_from_radio_num;
	}

	return api.from_radio_num + 1;
}

uint32_t meshtastic_phone_api_get_node_num(void)
{
	return api.node_num;
}

bool meshtastic_phone_api_get_halow_profile(struct meshtastic_halow_profile *profile,
					    const char *fallback_mesh_id)
{
	const meshtastic_Channel *primary;
	const char *mesh_id;

	if (!profile) {
		return false;
	}

	*profile = (struct meshtastic_halow_profile){0};
	primary = &stored_channels[MOCK_PRIMARY_CHANNEL_INDEX];
	mesh_id = (primary->has_settings && primary->settings.name[0]) ?
		primary->settings.name : MESHTASTIC_DEFAULT_MESH_ID;
	if (!mesh_id || mesh_id[0] == '\0') {
		mesh_id = fallback_mesh_id;
	}
	if (!mesh_id || mesh_id[0] == '\0') {
		mesh_id = MESHTASTIC_DEFAULT_MESH_ID;
	}

	strncpy(profile->mesh_id, mesh_id, sizeof(profile->mesh_id) - 1);
	if (primary->has_settings && primary->settings.psk.size > 0) {
		profile->psk_len = MIN((size_t)primary->settings.psk.size, sizeof(profile->psk));
		memcpy(profile->psk, primary->settings.psk.bytes, profile->psk_len);
	}
	profile->has_primary_psk = profile->psk_len > 0;
	if (profile->has_primary_psk) {
		bytes_to_base64(profile->psk, profile->psk_len, profile->psk_b64,
				sizeof(profile->psk_b64));
		bytes_to_hex(profile->psk, profile->psk_len, profile->psk_hex,
			     sizeof(profile->psk_hex));
		memcpy(profile->passphrase, profile->psk, profile->psk_len);
		profile->passphrase_len = profile->psk_len;
	}

	LOG_INF("PhoneAPI HaLow profile mesh_id=\"%s\" wifi_security=open wifi_passphrase_len=0 stored_primary_psk=%s",
		profile->mesh_id, profile->has_primary_psk ? "yes" : "no");

	return true;
}

static uint8_t halow_profile_channel_hash(const struct meshtastic_halow_profile *profile)
{
	uint8_t hash = 0;
	uint8_t expanded_key[HALOW_MAX_PRIMARY_PSK_LEN];
	size_t expanded_len;

	if (!profile) {
		return 0;
	}

	for (size_t i = 0; i < sizeof(profile->mesh_id) && profile->mesh_id[i] != '\0'; i++) {
		hash ^= (uint8_t)profile->mesh_id[i];
	}
	if (expand_primary_channel_psk(profile->psk, profile->psk_len, expanded_key, &expanded_len)) {
		for (size_t i = 0; i < expanded_len; i++) {
			hash ^= expanded_key[i];
		}
	}

	return hash;
}

static bool halow_channel_hash_and_key(uint8_t channel_index, uint8_t *hash,
				       uint8_t *expanded_key, size_t *expanded_len)
{
	const meshtastic_Channel *channel;
	const meshtastic_Channel *primary;
	const uint8_t *psk;
	size_t psk_len;
	const char *name;
	uint8_t local_hash = 0;

	if (!hash || !expanded_key || !expanded_len || channel_index >= ARRAY_SIZE(stored_channels)) {
		return false;
	}

	channel = &stored_channels[channel_index];
	primary = &stored_channels[MOCK_PRIMARY_CHANNEL_INDEX];
	if (!channel->has_settings || channel->role == meshtastic_Channel_Role_DISABLED) {
		return false;
	}

	psk = channel->settings.psk.bytes;
	psk_len = MIN((size_t)channel->settings.psk.size, (size_t)MESHTASTIC_HALOW_PSK_MAX_LEN);
	if (psk_len == 0 && channel->role == meshtastic_Channel_Role_SECONDARY &&
	    primary->has_settings && primary->role != meshtastic_Channel_Role_DISABLED) {
		psk = primary->settings.psk.bytes;
		psk_len = MIN((size_t)primary->settings.psk.size,
			      (size_t)MESHTASTIC_HALOW_PSK_MAX_LEN);
	}

	if (!expand_primary_channel_psk(psk, psk_len, expanded_key, expanded_len)) {
		return false;
	}

	name = channel->settings.name;
	if (!name[0] && channel_index == MOCK_PRIMARY_CHANNEL_INDEX) {
		name = MESHTASTIC_DEFAULT_MESH_ID;
	}
	for (size_t i = 0; name[i] != '\0'; i++) {
		local_hash ^= (uint8_t)name[i];
	}
	for (size_t i = 0; i < *expanded_len; i++) {
		local_hash ^= expanded_key[i];
	}

	*hash = local_hash;
	return true;
}

static bool halow_channel_index_for_hash(uint8_t channel_hash, uint8_t *channel_index)
{
	uint8_t expanded_key[HALOW_MAX_PRIMARY_PSK_LEN];
	size_t expanded_len;
	uint8_t hash;

	if (!channel_index) {
		return false;
	}

	for (uint8_t i = 0; i < ARRAY_SIZE(stored_channels); i++) {
		if (halow_channel_hash_and_key(i, &hash, expanded_key, &expanded_len) &&
		    hash == channel_hash) {
			*channel_index = i;
			return true;
		}
	}

	return false;
}

bool meshtastic_phone_api_get_local_node_summary(struct meshtastic_halow_node_summary *summary)
{
	if (!summary) {
		return false;
	}

	sync_own_public_key_from_security_config();
	*summary = (struct meshtastic_halow_node_summary){0};
	summary->node_num = api.node_num;
	summary->hw_model = (uint8_t)mock_nodedb[0].user.hw_model;
	summary->role = (uint8_t)mock_nodedb[0].user.role;
	summary->is_licensed = true;
	strncpy(summary->short_name, mock_nodedb[0].user.short_name,
		sizeof(summary->short_name) - 1);
	strncpy(summary->long_name, mock_nodedb[0].user.long_name,
		sizeof(summary->long_name) - 1);
	if (mock_nodedb[0].user.public_key.size == sizeof(summary->public_key) &&
	    bytes_nonzero(mock_nodedb[0].user.public_key.bytes,
			  mock_nodedb[0].user.public_key.size)) {
		summary->public_key_len = mock_nodedb[0].user.public_key.size;
		memcpy(summary->public_key, mock_nodedb[0].user.public_key.bytes,
		       summary->public_key_len);
	}
	if (summary->role == (uint8_t)0xff) {
		summary->role = meshtastic_Config_DeviceConfig_Role_CLIENT;
	}
	if (summary->hw_model == (uint8_t)meshtastic_HardwareModel_UNSET) {
		summary->hw_model = (uint8_t)meshtastic_HardwareModel_PRIVATE_HW;
	}
	return true;
}

static bool discovery_get_string(const uint8_t *buf, size_t len, size_t *pos,
				 char *out, size_t out_len)
{
	uint8_t string_len;

	if (*pos >= len || out_len == 0) {
		return false;
	}
	string_len = buf[(*pos)++];
	if (string_len >= out_len || *pos + string_len > len) {
		return false;
	}
	memcpy(out, &buf[*pos], string_len);
	out[string_len] = '\0';
	*pos += string_len;
	return true;
}

static bool discovery_get_byte(const uint8_t *buf, size_t len, size_t *pos, uint8_t *out)
{
	if (*pos >= len || !out) {
		return false;
	}
	*out = buf[(*pos)++];
	return true;
}

static bool discovery_get_u32(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out)
{
	if (*pos + sizeof(*out) > len || !out) {
		return false;
	}
	memcpy(out, &buf[*pos], sizeof(*out));
	*pos += sizeof(*out);
	return true;
}

struct halow_vendor_nodeinfo {
	uint8_t channel_hash;
	uint32_t node_num;
	uint8_t hw_model;
	uint8_t role;
	bool has_licensed;
	bool is_licensed;
	char short_name[32];
	char long_name[64];
	uint8_t public_key[32];
	size_t public_key_len;
};

static bool parse_halow_vendor_nodeinfo(const uint8_t *compact, size_t compact_len, bool has_flags,
				       struct halow_vendor_nodeinfo *out)
{
	size_t pos = 0;

	if (!compact || compact_len == 0 || !out) {
		return false;
	}
	memset(out, 0, sizeof(*out));

	if (!discovery_get_byte(compact, compact_len, &pos, &out->channel_hash)) {
		return false;
	}
	if (!discovery_get_u32(compact, compact_len, &pos, &out->node_num)) {
		return false;
	}
	if (!discovery_get_byte(compact, compact_len, &pos, &out->hw_model)) {
		return false;
	}
	if (!discovery_get_byte(compact, compact_len, &pos, &out->role)) {
		return false;
	}

	if (has_flags) {
		uint8_t flags = 0;
		if (!discovery_get_byte(compact, compact_len, &pos, &flags)) {
			return false;
		}
		out->has_licensed = true;
		out->is_licensed = (flags & 0x01) != 0;
	}

	if (!discovery_get_string(compact, compact_len, &pos, out->short_name, sizeof(out->short_name)) ||
	    !discovery_get_string(compact, compact_len, &pos, out->long_name, sizeof(out->long_name))) {
		return false;
	}
	if (pos < compact_len) {
		uint8_t public_key_len = 0;

		if (!discovery_get_byte(compact, compact_len, &pos, &public_key_len) ||
		    public_key_len > sizeof(out->public_key) || pos + public_key_len > compact_len) {
			return false;
		}
		out->public_key_len = public_key_len;
		if (public_key_len > 0) {
			memcpy(out->public_key, &compact[pos], public_key_len);
			pos += public_key_len;
		}
	}
	return true;
}

bool meshtastic_phone_api_handle_halow_discovery_ies(const uint8_t *ies, size_t ies_len,
						     int8_t rssi)
{
	uint8_t reassembled[HALOW_MESH_VENDOR_MAX_FRAGS * HALOW_MESH_VENDOR_DATA_LEN] = {0};
	size_t frag_lens[HALOW_MESH_VENDOR_MAX_FRAGS] = {0};
	bool frag_seen[HALOW_MESH_VENDOR_MAX_FRAGS] = {false};
	uint8_t expected_frags = 0;
	size_t off = 0;

	if (!ies || ies_len == 0) {
		return false;
	}
	LOG_INF("PhoneAPI HaLow discovery vendor IE RX raw ies_len=%u rssi=%d", (unsigned int)ies_len, rssi);
	LOG_HEXDUMP_INF(ies, ies_len, "PhoneAPI HaLow discovery vendor IEs RX");

	while (off + 2U <= ies_len) {
		uint8_t eid = ies[off];
		uint8_t len = ies[off + 1];
		size_t next = off + 2U + len;
		const uint8_t *body;

		if (next > ies_len) {
			break;
		}
		body = &ies[off + 2];
		if (eid == HALOW_MESH_VENDOR_IE_ID && len >= HALOW_MESH_VENDOR_HEADER_LEN &&
		    memcmp(body, halow_mesh_vendor_oui, sizeof(halow_mesh_vendor_oui)) == 0 &&
		    body[3] == HALOW_MESH_VENDOR_NODEINFO_TYPE &&
		    body[4] == HALOW_MESH_VENDOR_NODEINFO_VERSION) {
			uint8_t frag_index = body[5];
			uint8_t frag_count = body[6];
			size_t frag_payload_len = len - HALOW_MESH_VENDOR_HEADER_LEN;

			if (frag_count > 0 && frag_count <= HALOW_MESH_VENDOR_MAX_FRAGS &&
			    frag_index < frag_count && frag_payload_len <= HALOW_MESH_VENDOR_DATA_LEN) {
				expected_frags = frag_count;
				memcpy(&reassembled[frag_index * HALOW_MESH_VENDOR_DATA_LEN],
				       &body[HALOW_MESH_VENDOR_HEADER_LEN], frag_payload_len);
				frag_lens[frag_index] = frag_payload_len;
				frag_seen[frag_index] = true;
			}
		}
		off = next;
	}

	if (expected_frags == 0) {
		return false;
	}

	uint8_t compact[HALOW_MESH_VENDOR_MAX_FRAGS * HALOW_MESH_VENDOR_DATA_LEN] = {0};
	size_t compact_len = 0;
	for (uint8_t i = 0; i < expected_frags; i++) {
		if (!frag_seen[i]) {
			LOG_WRN("PhoneAPI HaLow discovery missing vendor IE fragment %u/%u",
				i, expected_frags);
			return false;
		}
		if (i + 1 < expected_frags && frag_lens[i] != HALOW_MESH_VENDOR_DATA_LEN) {
			LOG_WRN("PhoneAPI HaLow discovery short middle fragment %u len=%u",
				i, (unsigned int)frag_lens[i]);
			return false;
		}
		memcpy(&compact[compact_len], &reassembled[i * HALOW_MESH_VENDOR_DATA_LEN],
		       frag_lens[i]);
		compact_len += frag_lens[i];
	}
	LOG_HEXDUMP_INF(compact, compact_len, "PhoneAPI HaLow discovery compact payload RX");

	struct halow_vendor_nodeinfo decoded = {0};
	bool has_flags = false;
	meshtastic_User user = meshtastic_User_init_zero;
	struct meshtastic_halow_profile profile;

	if (compact_len < 7) {
		return false;
	}

	if (parse_halow_vendor_nodeinfo(compact, compact_len, true, &decoded)) {
		has_flags = true;
	} else if (parse_halow_vendor_nodeinfo(compact, compact_len, false, &decoded)) {
		has_flags = false;
	} else {
		LOG_WRN("PhoneAPI HaLow discovery malformed node payload len=%u", (unsigned int)compact_len);
		return false;
	}

	strncpy(user.short_name, decoded.short_name, sizeof(user.short_name) - 1);
	strncpy(user.long_name, decoded.long_name, sizeof(user.long_name) - 1);

	(void)meshtastic_phone_api_get_halow_profile(&profile, NULL);
	uint8_t local_hash = halow_profile_channel_hash(&profile);
	if (decoded.channel_hash != local_hash) {
		LOG_WRN("PhoneAPI HaLow discovery hash mismatch remote=0x%02x local=0x%02x; accepting same-mesh scan result",
			decoded.channel_hash, local_hash);
	}

	snprintk(user.id, sizeof(user.id), "!%08x", decoded.node_num);
	user.hw_model = (meshtastic_HardwareModel)decoded.hw_model;
	user.role = (meshtastic_Config_DeviceConfig_Role)decoded.role;
	user.is_licensed = decoded.is_licensed;
	if (decoded.public_key_len == sizeof(user.public_key.bytes)) {
		user.public_key.size = decoded.public_key_len;
		memcpy(user.public_key.bytes, decoded.public_key, decoded.public_key_len);
	}

	if (!has_flags) {
		LOG_WRN("PhoneAPI HaLow discovery parsed legacy vendor payload node=0x%08x", decoded.node_num);
	}

	bool created = false;
	meshtastic_NodeInfo *node = upsert_node_from_user(decoded.node_num, &user, rssi, &created);
	if (!node) {
		return false;
	}
	uint8_t channel_index = 0;
	if (halow_channel_index_for_hash(decoded.channel_hash, &channel_index)) {
		node->channel = channel_index;
		save_phone_api_store();
		LOG_INF("PhoneAPI HaLow discovery node=0x%08x channel_hash=0x%02x ch_index=%u",
			decoded.node_num, decoded.channel_hash, channel_index);
	}

	LOG_INF("PhoneAPI HaLow discovery %s node=0x%08x short=\"%s\" long=\"%s\" hw=%u role=%u key_len=%u rssi=%d",
		created ? "added" : "updated", decoded.node_num, user.short_name, user.long_name,
		decoded.hw_model, decoded.role, (unsigned int)decoded.public_key_len, rssi);
	return true;
}
