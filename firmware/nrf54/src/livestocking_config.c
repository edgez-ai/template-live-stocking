#include "livestocking_config.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include "edgez_config.h"

LOG_MODULE_REGISTER(livestock_config, LOG_LEVEL_INF);

#define LIVESTOCK_SETTINGS_KEY "livestock/mqtt/v1"
#define LIVESTOCK_MAGIC 0x4c534331U

struct provisioning_json {
	char *client_id;
	char *username;
	char *password;
	char *project_id;
	char *channel;
	char *mesh_id;
	char *passphrase;
	char *country;
	char *device_name;
	bool use_device_gps;
	int32_t halow_channel;
	int32_t halow_frequency_khz;
	double latitude;
	double longitude;
};

static const struct json_obj_descr provisioning_descr[] = {
	JSON_OBJ_DESCR_PRIM_NAMED(struct provisioning_json, "clientId", client_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct provisioning_json, username, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct provisioning_json, password, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct provisioning_json, "projectId", project_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct provisioning_json, channel, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct provisioning_json, "meshId", mesh_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct provisioning_json, passphrase, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct provisioning_json, country, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct provisioning_json, "halowChannel", halow_channel, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM_NAMED(struct provisioning_json, "halowFrequencyKHz", halow_frequency_khz, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct provisioning_json, latitude, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM(struct provisioning_json, longitude, JSON_TOK_DOUBLE_FP),
	JSON_OBJ_DESCR_PRIM_NAMED(struct provisioning_json, "deviceName", device_name, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct provisioning_json, "useDeviceGps", use_device_gps, JSON_TOK_TRUE),
};

struct livestock_saved_config {
	uint32_t magic;
	char country[3];
	char client_id[96];
	char username[13];
	char password[192];
	char project_id[96];
	char channel[32];
};

static struct livestock_saved_config saved = { .country = CONFIG_WIFI_MORSE_REGION };

static bool valid_string(const char *value, size_t capacity)
{
	return value && value[0] && strlen(value) < capacity;
}

static int parse_device_uuid(const char *value, uint64_t *high, uint64_t *low)
{
	uint64_t halves[2] = {0};
	size_t digits = 0;

	if (!value || strlen(value) != 36 || !high || !low) return -EINVAL;
	for (size_t i = 0; i < 36; ++i) {
		char c = value[i];
		uint8_t nibble;
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-') return -EINVAL;
			continue;
		}
		if (c >= '0' && c <= '9') nibble = (uint8_t)(c - '0');
		else if (c >= 'a' && c <= 'f') nibble = (uint8_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') nibble = (uint8_t)(c - 'A' + 10);
		else return -EINVAL;
		halves[digits / 16] = (halves[digits / 16] << 4) | nibble;
		++digits;
	}
	*high = halves[0];
	*low = halves[1];
	return (*high || *low) ? 0 : -EINVAL;
}

void livestock_config_init(void)
{
	struct livestock_saved_config loaded = {0};
	ssize_t length = settings_load_one(LIVESTOCK_SETTINGS_KEY, &loaded, sizeof(loaded));

	if (length == sizeof(loaded) && loaded.magic == LIVESTOCK_MAGIC &&
	    loaded.country[0] && loaded.country[1] && loaded.country[2] == '\0') {
		saved = loaded;
		LOG_INF("Live Stocking MQTT configuration loaded country=%s project=%s",
			saved.country, saved.project_id);
	} else {
		LOG_INF("Live Stocking MQTT configuration not provisioned rc=%d", (int)length);
	}
}

const char *livestock_config_country(void)
{
	return saved.country;
}

bool livestock_config_is_provisioned(void)
{
	return saved.magic == LIVESTOCK_MAGIC;
}

int livestock_config_apply_json(char *json, size_t length, const char *device_serial)
{
	struct provisioning_json request = {0};
	struct livestock_saved_config updated = { .magic = LIVESTOCK_MAGIC };
	int64_t fields;
	int rc;
	bool has_location;
	uint64_t user_id_high;
	uint64_t user_id_low;

	if (!json || !device_serial || length == 0 || length > 1024) {
		return -EINVAL;
	}
	fields = json_obj_parse(json, length, provisioning_descr,
				ARRAY_SIZE(provisioning_descr), &request);
	if (fields < 0 || (fields & BIT_MASK(10)) != BIT_MASK(10)) {
		LOG_ERR("MQTT provisioning JSON parse failed fields=%lld required=0x%x",
			(long long)fields, BIT_MASK(10));
		return -EBADMSG;
	}
	if (!valid_string(request.client_id, sizeof(updated.client_id)) ||
	    !valid_string(request.username, sizeof(updated.username)) ||
	    strlen(request.username) != 12 || strcmp(request.username, device_serial) != 0 ||
	    !valid_string(request.password, sizeof(updated.password)) ||
	    !valid_string(request.project_id, sizeof(updated.project_id)) ||
	    !valid_string(request.channel, sizeof(updated.channel)) ||
	    !valid_string(request.mesh_id, 33) ||
	    !valid_string(request.passphrase, 65) ||
	    (request.device_name && !valid_string(request.device_name, 65)) ||
	    !request.country || strlen(request.country) != 2 ||
	    request.halow_channel < 1 || request.halow_frequency_khz < 800000 ||
	    request.halow_frequency_khz > 1000000) {
		return -EINVAL;
	}
	if (parse_device_uuid(request.client_id, &user_id_high, &user_id_low) != 0) {
		return -EINVAL;
	}
	for (size_t i = 0; i < 12; ++i) {
		char c = request.username[i];
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
			return -EINVAL;
		}
	}
	has_location = (fields & BIT(10)) && (fields & BIT(11));
	if (request.use_device_gps && has_location) return -EINVAL;
	if (has_location && (!isfinite(request.latitude) || !isfinite(request.longitude) ||
	    request.latitude < -90 || request.latitude > 90 ||
	    request.longitude < -180 || request.longitude > 180)) {
		return -EINVAL;
	}
	strcpy(updated.country, request.country);
	strcpy(updated.client_id, request.client_id);
	strcpy(updated.username, request.username);
	strcpy(updated.password, request.password);
	strcpy(updated.project_id, request.project_id);
	strcpy(updated.channel, request.channel);
	rc = settings_save_one(LIVESTOCK_SETTINGS_KEY, &updated, sizeof(updated));
	if (rc != 0) {
		LOG_ERR("Could not save MQTT configuration rc=%d", rc);
		return rc;
	}
	rc = edgez_config_apply_provisioning(request.mesh_id, request.passphrase,
				     (uint32_t)request.halow_frequency_khz,
				     user_id_high, user_id_low,
				     request.device_name ? request.device_name : request.username,
				     has_location, (float)request.latitude,
				     (float)request.longitude, request.use_device_gps);
	if (rc == 0) {
		saved = updated;
		LOG_INF("Live Stocking configuration saved country=%s channel=%d frequency=%d kHz",
			saved.country, request.halow_channel, request.halow_frequency_khz);
	}
	return rc;
}
