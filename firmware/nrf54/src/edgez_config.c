#include "edgez_config.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

#include "nanopb/pb_decode.h"
#include "nanopb/pb_encode.h"
#include "edgez_battery.h"
#include "edgez_gps.h"
#include "edgez_imu.h"
#include "edgez_reboot.h"
#include "usb_control.pb.h"

LOG_MODULE_REGISTER(edgez_config, LOG_LEVEL_INF);

#define EDGEZ_IE_ID 221
#define EDGEZ_VENDOR "EdgeZ"
#define EDGEZ_VENDOR_LEN 5
#define EDGEZ_DEFAULT_INTERVAL_SECONDS 30
#define EDGEZ_PROFILE_SETTINGS_KEY "edgez/profile/v1"
#define EDGEZ_PROFILE_MAGIC 0x455A5031U
#define EDGEZ_PROFILE_VERSION 1U
#define EDGEZ_PROVISIONING_COMMIT_GRACE_MS 5000

struct edgez_persisted_profile {
	uint32_t magic;
	uint16_t version;
	uint16_t settings_len;
	uint32_t mesh_frequency_khz;
	uint32_t mesh_bandwidth_mhz;
	uint8_t settings_data[ai_edgez_halow_DeviceSettings_size];
};

K_MUTEX_DEFINE(config_lock);
static ai_edgez_halow_DeviceSettings settings;
static uint32_t mesh_frequency_khz;
static uint32_t mesh_bandwidth_mhz;
static uint32_t config_generation = 1;
static bool halow_ready;
static int64_t runtime_start_not_before_ms;
static atomic_t halow_beacon_build_count;
static atomic_t halow_beacon_last_ies_len;
static uint8_t beacon_vendor_ies[EDGEZ_VENDOR_IES_MAX_LEN];
static size_t beacon_vendor_ies_len;
static struct k_spinlock beacon_vendor_ies_lock;

/*
 * Called by the Morse mesh beacon builder. This must stay non-blocking because
 * it runs on the radio's management-frame path; IMU sampling is performed by
 * the application thread before publishing a new cached payload here.
 */
size_t mmwlan_mesh_beacon_dynamic_ies(uint8_t *out, size_t out_cap)
{
	size_t len;
	k_spinlock_key_t key;

	if (!out || out_cap == 0) {
		return 0;
	}

	/* Reaching this callback means the Morse management-frame path is building
	 * a real mesh beacon. Only record the milestone here; never touch I2C from
	 * the radio driver thread. */
	edgez_imu_note_halow_tx();
	atomic_inc(&halow_beacon_build_count);

	key = k_spin_lock(&beacon_vendor_ies_lock);
	len = beacon_vendor_ies_len;
	if (len > out_cap) {
		len = 0;
	} else if (len > 0) {
		memcpy(out, beacon_vendor_ies, len);
	}
	k_spin_unlock(&beacon_vendor_ies_lock, key);
	atomic_set(&halow_beacon_last_ies_len, (atomic_val_t)len);
	return len;
}

static bool persistent_device_mode(ai_edgez_halow_DeviceType device_type)
{
	/* A provisioned USER needs the same radio profile after reboot. Persisting
	 * only appliance modes made HaLowInitConfig start the radio with a volatile
	 * USER profile, racing the follow-up DeviceSettings SET over BLE. */
	return device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_USER ||
	       device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_BEACON ||
	       device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR ||
	       device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_RELAY;
}

/* config_lock must be held by the caller. Never log passphrases or key data. */
static void log_profile_readiness_locked(const char *context)
{
	bool mode_ok = persistent_device_mode(settings.device_type);
	bool mesh_ok = settings.mesh_id[0] != '\0';
	bool frequency_ok = mesh_frequency_khz > 0;
	bool bandwidth_ok = mesh_bandwidth_mhz > 0;

	LOG_INF("HaLow profile check context=%s ready=%u mode_ok=%u mesh_ok=%u frequency_ok=%u bandwidth_ok=%u device_type=%d mesh_id=\"%s\" passphrase_len=%u frequency=%u kHz bandwidth=%u MHz generation=%u",
		context, mode_ok && mesh_ok && frequency_ok && bandwidth_ok,
		mode_ok, mesh_ok, frequency_ok, bandwidth_ok, settings.device_type,
		settings.mesh_id, (unsigned int)strlen(settings.passphrase),
		mesh_frequency_khz, mesh_bandwidth_mhz, config_generation);
	if (mode_ok && (!frequency_ok || !bandwidth_ok)) {
		LOG_WRN("HaLow profile incomplete after %s: radio frequency/bandwidth are zero or were not provided",
			context);
	}
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
	if (!dst || dst_size == 0) {
		return;
	}
	strncpy(dst, src ? src : "", dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static void normalize_settings(ai_edgez_halow_DeviceSettings *value)
{
	if (value->beacon_interval_seconds < 5 || value->beacon_interval_seconds > 3600) {
		value->beacon_interval_seconds = EDGEZ_DEFAULT_INTERVAL_SECONDS;
	}
	if (value->max_hop == 0 || value->max_hop > UINT8_MAX) {
		value->max_hop = 2;
	}
	if (value->user_name[0] == '\0') {
		copy_string(value->user_name, sizeof(value->user_name), "EdgeZ User");
	}
	if (value->device_type < ai_edgez_halow_DeviceType_DEVICE_TYPE_USER ||
	    value->device_type > ai_edgez_halow_DeviceType_DEVICE_TYPE_RELAY) {
		value->device_type = value->device_mode_enabled ?
			ai_edgez_halow_DeviceType_DEVICE_TYPE_BEACON :
			ai_edgez_halow_DeviceType_DEVICE_TYPE_USER;
	}
}

/* config_lock must be held by the caller. */
static int save_persisted_profile_locked(void)
{
	struct edgez_persisted_profile stored = {
		.magic = EDGEZ_PROFILE_MAGIC,
		.version = EDGEZ_PROFILE_VERSION,
		.mesh_frequency_khz = mesh_frequency_khz,
		.mesh_bandwidth_mhz = mesh_bandwidth_mhz,
	};
	pb_ostream_t output;
	int rc;

	if (!persistent_device_mode(settings.device_type)) {
		LOG_INF("Persistent HaLow save not required device_type=%d (unsupported/non-persistent mode)",
			settings.device_type);
		rc = settings_delete(EDGEZ_PROFILE_SETTINGS_KEY);
		if (rc == 0 || rc == -ENOENT) {
			LOG_INF("Persistent HaLow profile cleared for non-persistent mode");
			return 0;
		}
		LOG_WRN("Persistent HaLow profile delete failed key=%s rc=%d",
			EDGEZ_PROFILE_SETTINGS_KEY, rc);
		return rc;
	}
	if (mesh_frequency_khz == 0 || mesh_bandwidth_mhz == 0) {
		LOG_ERR("Persistent HaLow profile not saved: missing radio config frequency=%u kHz bandwidth=%u MHz device_type=%d mesh_id=\"%s\"",
			mesh_frequency_khz, mesh_bandwidth_mhz, settings.device_type,
			settings.mesh_id);
		return -EAGAIN;
	}
	settings.mesh_frequency_khz = mesh_frequency_khz;
	settings.mesh_bandwidth_mhz = mesh_bandwidth_mhz;

	output = pb_ostream_from_buffer(stored.settings_data,
					 sizeof(stored.settings_data));
	if (!pb_encode(&output, ai_edgez_halow_DeviceSettings_fields, &settings)) {
		LOG_WRN("Persistent DeviceSettings encode failed: %s", PB_GET_ERROR(&output));
		return -EMSGSIZE;
	}
	stored.settings_len = (uint16_t)output.bytes_written;
	rc = settings_save_one(EDGEZ_PROFILE_SETTINGS_KEY, &stored,
			       offsetof(struct edgez_persisted_profile, settings_data) +
			       stored.settings_len);
	if (rc == 0) {
		struct edgez_persisted_profile verified = {0};
		size_t stored_len = offsetof(struct edgez_persisted_profile, settings_data) +
			stored.settings_len;
		ssize_t verified_len = settings_load_one(EDGEZ_PROFILE_SETTINGS_KEY, &verified,
						       sizeof(verified));

		if (verified_len != stored_len || memcmp(&verified, &stored, stored_len) != 0) {
			LOG_ERR("Persistent HaLow profile NVS verification failed bytes=%d expected=%u",
				(int)verified_len, (unsigned int)stored_len);
			return -EIO;
		}
		LOG_INF("Persistent HaLow profile saved device_type=%d mesh_id=%s frequency=%u kHz bandwidth=%u MHz bytes=%u",
			settings.device_type, settings.mesh_id, mesh_frequency_khz,
			mesh_bandwidth_mhz, stored.settings_len);
		edgez_request_reboot(EDGEZ_REBOOT_REASON_SETTINGS_SAVED);
	} else {
		LOG_ERR("Persistent HaLow profile settings write failed key=%s bytes=%u rc=%d",
			EDGEZ_PROFILE_SETTINGS_KEY,
			(unsigned int)(offsetof(struct edgez_persisted_profile, settings_data) +
				       stored.settings_len), rc);
	}
	return rc;
}

/* config_lock must be held by the caller. */
static int load_persisted_profile_locked(void)
{
	struct edgez_persisted_profile stored = {0};
	ai_edgez_halow_DeviceSettings loaded = ai_edgez_halow_DeviceSettings_init_zero;
	pb_istream_t input;
	ssize_t bytes;
	size_t header_len = offsetof(struct edgez_persisted_profile, settings_data);

	bytes = settings_load_one(EDGEZ_PROFILE_SETTINGS_KEY, &stored, sizeof(stored));
	if (bytes < 0) {
		LOG_INF("Persistent HaLow profile read key=%s rc=%d",
			EDGEZ_PROFILE_SETTINGS_KEY, (int)bytes);
		return (int)bytes;
	}
	if (bytes == 0) {
		LOG_INF("Persistent HaLow profile key=%s is empty; treating as absent",
			EDGEZ_PROFILE_SETTINGS_KEY);
		return -ENOENT;
	}
	LOG_INF("Persistent HaLow profile raw bytes=%d header=%u magic=0x%08x expected_magic=0x%08x version=%u expected_version=%u settings_len=%u capacity=%u frequency=%u kHz bandwidth=%u MHz",
		(int)bytes, (unsigned int)header_len, stored.magic, EDGEZ_PROFILE_MAGIC,
		stored.version, EDGEZ_PROFILE_VERSION, stored.settings_len,
		(unsigned int)sizeof(stored.settings_data), stored.mesh_frequency_khz,
		stored.mesh_bandwidth_mhz);
	if ((size_t)bytes < header_len || stored.magic != EDGEZ_PROFILE_MAGIC ||
	    stored.version != EDGEZ_PROFILE_VERSION || stored.settings_len == 0 ||
	    stored.settings_len > sizeof(stored.settings_data) ||
	    (size_t)bytes != header_len + stored.settings_len ||
	    stored.mesh_frequency_khz == 0 || stored.mesh_bandwidth_mhz == 0) {
		LOG_ERR("Persistent HaLow profile validation failed short=%u magic_bad=%u version_bad=%u settings_len_bad=%u total_len_bad=%u frequency_missing=%u bandwidth_missing=%u",
			(size_t)bytes < header_len, stored.magic != EDGEZ_PROFILE_MAGIC,
			stored.version != EDGEZ_PROFILE_VERSION,
			stored.settings_len == 0 ||
				stored.settings_len > sizeof(stored.settings_data),
			(size_t)bytes != header_len + stored.settings_len,
			stored.mesh_frequency_khz == 0, stored.mesh_bandwidth_mhz == 0);
		return -EINVAL;
	}

	input = pb_istream_from_buffer(stored.settings_data, stored.settings_len);
	if (!pb_decode(&input, ai_edgez_halow_DeviceSettings_fields, &loaded)) {
		LOG_WRN("Persistent DeviceSettings decode failed: %s", PB_GET_ERROR(&input));
		return -EBADMSG;
	}
	normalize_settings(&loaded);
	if (!persistent_device_mode(loaded.device_type)) {
		LOG_ERR("Persistent HaLow profile decoded but device_type=%d is not user/beacon/sensor/relay",
			loaded.device_type);
		return -EINVAL;
	}

	settings = loaded;
	settings.action = ai_edgez_halow_DeviceSettingsAction_DEVICE_SETTINGS_REPORT;
	mesh_frequency_khz = stored.mesh_frequency_khz;
	mesh_bandwidth_mhz = stored.mesh_bandwidth_mhz;
	settings.mesh_frequency_khz = mesh_frequency_khz;
	settings.mesh_bandwidth_mhz = mesh_bandwidth_mhz;
	LOG_INF("Persistent HaLow profile loaded device_type=%d mesh_id=%s frequency=%u kHz bandwidth=%u MHz",
		settings.device_type, settings.mesh_id, mesh_frequency_khz,
		mesh_bandwidth_mhz);
	return 0;
}

void edgez_config_init(const char *mesh_id, const char *passphrase)
{
	uint8_t random_identity[40];
	int settings_rc;
	int load_rc;

	sys_rand_get(random_identity, sizeof(random_identity));
	settings_rc = settings_subsys_init();
	LOG_INF("Persistent HaLow settings subsystem init rc=%d", settings_rc);
	if (settings_rc != 0 && settings_rc != -EALREADY) {
		LOG_WRN("Persistent HaLow settings init failed: %d", settings_rc);
	}
	k_mutex_lock(&config_lock, K_FOREVER);
	settings = (ai_edgez_halow_DeviceSettings)ai_edgez_halow_DeviceSettings_init_zero;
	settings.action = ai_edgez_halow_DeviceSettingsAction_DEVICE_SETTINGS_REPORT;
	copy_string(settings.mesh_id, sizeof(settings.mesh_id), mesh_id);
	copy_string(settings.passphrase, sizeof(settings.passphrase), passphrase);
	copy_string(settings.user_name, sizeof(settings.user_name), "EdgeZ User");
	memcpy(&settings.user_id_high, random_identity, sizeof(settings.user_id_high));
	memcpy(&settings.user_id_low, random_identity + 8, sizeof(settings.user_id_low));
	settings.user_public_key.size = 32;
	memcpy(settings.user_public_key.bytes, random_identity + 8, 32);
	settings.device_type = ai_edgez_halow_DeviceType_DEVICE_TYPE_USER;
	settings.beacon_interval_seconds = EDGEZ_DEFAULT_INTERVAL_SECONDS;
	settings.max_hop = 2;
	mesh_frequency_khz = 0;
	mesh_bandwidth_mhz = 0;
	config_generation = 1;
	runtime_start_not_before_ms = 0;
	atomic_set(&halow_beacon_build_count, 0);
	atomic_set(&halow_beacon_last_ies_len, 0);
	if (settings_rc == 0 || settings_rc == -EALREADY) {
		load_rc = load_persisted_profile_locked();
		if (load_rc != 0 && load_rc != -ENOENT) {
			LOG_WRN("Persistent HaLow profile ignored: %d", load_rc);
		} else if (load_rc == -ENOENT) {
			LOG_INF("Persistent HaLow profile absent; BLE provisioning is required");
		}
	}
	log_profile_readiness_locked("boot");
	k_mutex_unlock(&config_lock);
}

void edgez_config_get_profile(struct edgez_halow_profile *profile)
{
	if (!profile) {
		return;
	}
	k_mutex_lock(&config_lock, K_FOREVER);
	copy_string(profile->mesh_id, sizeof(profile->mesh_id), settings.mesh_id);
	copy_string(profile->passphrase, sizeof(profile->passphrase), settings.passphrase);
	profile->mesh_frequency_khz = mesh_frequency_khz;
	profile->mesh_bandwidth_mhz = mesh_bandwidth_mhz;
	profile->beacon_interval_seconds = settings.beacon_interval_seconds;
	profile->device_type = (uint32_t)settings.device_type;
	profile->sleep_mode_enabled = settings.sleep_mode_enabled;
	profile->device_gps_enabled = settings.device_gps_enabled;
	k_mutex_unlock(&config_lock);
}

int edgez_config_apply_provisioning(const char *mesh_id, const char *passphrase,
				   uint32_t frequency_khz, uint64_t user_id_high,
				   uint64_t user_id_low, const char *user_name,
				   bool has_location, float latitude, float longitude,
				   bool use_device_gps)
{
	int rc;

	if (!mesh_id || !passphrase || !user_name || !user_name[0] ||
	    strlen(mesh_id) >= sizeof(settings.mesh_id) ||
	    strlen(passphrase) >= sizeof(settings.passphrase) ||
	    strlen(user_name) >= sizeof(settings.user_name) ||
	    (user_id_high == 0 && user_id_low == 0) || frequency_khz == 0) {
		return -EINVAL;
	}
	k_mutex_lock(&config_lock, K_FOREVER);
	copy_string(settings.mesh_id, sizeof(settings.mesh_id), mesh_id);
	copy_string(settings.passphrase, sizeof(settings.passphrase), passphrase);
	settings.user_id_high = user_id_high;
	settings.user_id_low = user_id_low;
	copy_string(settings.user_name, sizeof(settings.user_name), user_name);
	settings.device_type = ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR;
	settings.device_gps_enabled = use_device_gps;
	settings.share_location = has_location;
	settings.latitude = has_location ? latitude : 0;
	settings.longitude = has_location ? longitude : 0;
	mesh_frequency_khz = frequency_khz;
	mesh_bandwidth_mhz = 1;
	settings.mesh_frequency_khz = frequency_khz;
	settings.mesh_bandwidth_mhz = 1;
	normalize_settings(&settings);
	config_generation++;
	rc = save_persisted_profile_locked();
	if (rc == 0) {
		runtime_start_not_before_ms = 0;
	}
	k_mutex_unlock(&config_lock);
	return rc;
}

uint32_t edgez_config_generation(void)
{
	uint32_t generation;
	k_mutex_lock(&config_lock, K_FOREVER);
	generation = config_generation;
	k_mutex_unlock(&config_lock);
	return generation;
}

void edgez_config_set_halow_ready(bool ready)
{
	halow_ready = ready;
}

bool edgez_config_halow_beacon_started(void)
{
	return atomic_get(&halow_beacon_build_count) > 0;
}

uint32_t edgez_config_halow_beacon_build_count(void)
{
	return (uint32_t)atomic_get(&halow_beacon_build_count);
}

size_t edgez_config_halow_beacon_last_ies_len(void)
{
	return (size_t)atomic_get(&halow_beacon_last_ies_len);
}

bool edgez_config_should_autostart(void)
{
	bool ready;

	k_mutex_lock(&config_lock, K_FOREVER);
	ready = persistent_device_mode(settings.device_type) &&
		settings.mesh_id[0] != '\0' &&
		mesh_frequency_khz > 0 && mesh_bandwidth_mhz > 0;
	k_mutex_unlock(&config_lock);
	return ready;
}

bool edgez_config_is_complete(void)
{
	bool ready;
	int64_t now = k_uptime_get();

	k_mutex_lock(&config_lock, K_FOREVER);
	ready = settings.mesh_id[0] != '\0' &&
		mesh_frequency_khz > 0 && mesh_bandwidth_mhz > 0 &&
		now >= runtime_start_not_before_ms;
	k_mutex_unlock(&config_lock);
	return ready;
}

static void response_base(const ai_edgez_halow_NetworkPacket *request,
			  ai_edgez_halow_NetworkPacket *response)
{
	response->from = request->to;
	response->to = request->from;
	response->operation = ai_edgez_halow_Operation_RESPONSE;
	response->interface = ai_edgez_halow_Interface_HALOW;
}

int edgez_config_handle_packet(const uint8_t *payload, size_t payload_len,
			       uint8_t *response_buf, size_t response_cap,
			       size_t *response_len)
{
	ai_edgez_halow_NetworkPacket request = ai_edgez_halow_NetworkPacket_init_zero;
	ai_edgez_halow_NetworkPacket response = ai_edgez_halow_NetworkPacket_init_zero;
	pb_istream_t input;
	pb_ostream_t output;

	if (!payload || !response_buf || !response_len) {
		return -EINVAL;
	}
	*response_len = 0;
	input = pb_istream_from_buffer(payload, payload_len);
	if (!pb_decode(&input, ai_edgez_halow_NetworkPacket_fields, &request)) {
		LOG_WRN("BLE NetworkPacket decode failed: %s", PB_GET_ERROR(&input));
		return -EBADMSG;
	}
	LOG_INF("BLE NetworkPacket request body=%u operation=%u len=%u",
		request.which_body, request.operation, (unsigned int)payload_len);
	response_base(&request, &response);

	if (request.which_body == ai_edgez_halow_NetworkPacket_device_settings_tag) {
		k_mutex_lock(&config_lock, K_FOREVER);
		LOG_INF("BLE DeviceSettings received action=%u device_type=%d legacy_device_mode=%u mesh_id_len=%u passphrase_len=%u user_name_len=%u public_key_len=%u private_key_len=%u beacon_interval=%u max_hop=%u sleep=%u frequency=%u kHz bandwidth=%u MHz gps=%u",
			request.body.device_settings.action,
			request.body.device_settings.device_type,
			request.body.device_settings.device_mode_enabled,
			(unsigned int)strlen(request.body.device_settings.mesh_id),
			(unsigned int)strlen(request.body.device_settings.passphrase),
			(unsigned int)strlen(request.body.device_settings.user_name),
			(unsigned int)request.body.device_settings.user_public_key.size,
			(unsigned int)request.body.device_settings.user_private_key.size,
			request.body.device_settings.beacon_interval_seconds,
			request.body.device_settings.max_hop,
			request.body.device_settings.sleep_mode_enabled,
			request.body.device_settings.mesh_frequency_khz,
			request.body.device_settings.mesh_bandwidth_mhz,
			request.body.device_settings.device_gps_enabled);
		if (request.body.device_settings.action ==
		    ai_edgez_halow_DeviceSettingsAction_DEVICE_SETTINGS_SET) {
			ai_edgez_halow_DeviceSettings updated = request.body.device_settings;
			int persist_rc;

			/* DeviceSettings is also used for partial mode/interval updates. Proto3
			 * scalar fields have no presence bit here, so omitted strings and identity
			 * fields arrive as empty/zero. Do not erase values previously supplied by
			 * the BLE HaLowInitConfig packet. */
			if (updated.mesh_id[0] == '\0' && settings.mesh_id[0] != '\0') {
				copy_string(updated.mesh_id, sizeof(updated.mesh_id),
					    settings.mesh_id);
			}
			if (updated.passphrase[0] == '\0' && settings.passphrase[0] != '\0') {
				copy_string(updated.passphrase, sizeof(updated.passphrase),
					    settings.passphrase);
			}
			if (updated.user_id_high == 0 && updated.user_id_low == 0 &&
			    (settings.user_id_high != 0 || settings.user_id_low != 0)) {
				updated.user_id_high = settings.user_id_high;
				updated.user_id_low = settings.user_id_low;
			}
			if (updated.user_name[0] == '\0' && settings.user_name[0] != '\0') {
				copy_string(updated.user_name, sizeof(updated.user_name),
					    settings.user_name);
			}
			if (updated.user_public_key.size == 0 && settings.user_public_key.size > 0) {
				updated.user_public_key = settings.user_public_key;
			}
			if (updated.mesh_frequency_khz > 0) {
				mesh_frequency_khz = updated.mesh_frequency_khz;
			} else {
				updated.mesh_frequency_khz = mesh_frequency_khz;
			}
			if (updated.mesh_bandwidth_mhz > 0) {
				mesh_bandwidth_mhz = updated.mesh_bandwidth_mhz;
			} else {
				updated.mesh_bandwidth_mhz = mesh_bandwidth_mhz;
			}
			settings = updated;
			normalize_settings(&settings);
			settings.action = ai_edgez_halow_DeviceSettingsAction_DEVICE_SETTINGS_REPORT;
			config_generation++;
			persist_rc = save_persisted_profile_locked();
			if (persist_rc != 0) {
				LOG_ERR("Persistent HaLow profile update failed rc=%d%s", persist_rc,
					persist_rc == -EAGAIN ?
					" (radio frequency/bandwidth missing)" : "");
			} else {
				runtime_start_not_before_ms = 0;
				LOG_INF("BLE provisioning commit saved; HaLow startup released");
			}
			LOG_INF("BLE configuration applied mesh_id=%s device_type=%d user=%016llx-%016llx generation=%u",
				settings.mesh_id, settings.device_type,
				(unsigned long long)settings.user_id_high,
				(unsigned long long)settings.user_id_low,
				config_generation);
			log_profile_readiness_locked("DeviceSettings SET");
		}
		response.which_body = ai_edgez_halow_NetworkPacket_device_settings_tag;
		response.body.device_settings = settings;
		response.body.device_settings.action =
			ai_edgez_halow_DeviceSettingsAction_DEVICE_SETTINGS_REPORT;
		k_mutex_unlock(&config_lock);
	} else if (request.which_body == ai_edgez_halow_NetworkPacket_init_tag) {
		const ai_edgez_halow_HaLowInitConfig *init = &request.body.init;
		bool has_configuration;

		k_mutex_lock(&config_lock, K_FOREVER);
		LOG_INF("BLE HaLowInitConfig received country=\"%s\" mesh_id_len=%u passphrase_len=%u user_name_len=%u public_key_len=%u max_hop=%u frequency=%u kHz bandwidth=%u MHz location=%u",
			init->country_code, (unsigned int)strlen(init->mesh_id),
			(unsigned int)strlen(init->passphrase),
			(unsigned int)strlen(init->user_name),
			(unsigned int)init->user_public_key.size, init->max_hop,
			init->mesh_frequency_khz, init->mesh_bandwidth_mhz,
			init->has_location);

		/* The mobile SDK also sends a completely empty HaLowInitConfig as its
		 * reconnect/status and license probe. It is not a provisioning command.
		 * Treating it as one erased the persisted mesh and identity immediately
		 * before returning the otherwise-authorized status response. */
		has_configuration = init->mesh_id[0] != '\0' ||
			init->passphrase[0] != '\0' || init->max_hop != 0 ||
			init->user_id_high != 0 || init->user_id_low != 0 ||
			init->user_name[0] != '\0' || init->user_public_key.size != 0 ||
			init->has_location || init->mesh_frequency_khz != 0 ||
			init->mesh_bandwidth_mhz != 0;
		if (!has_configuration) {
			LOG_INF("BLE HaLowInitConfig is an empty status/license probe; preserving persisted profile generation=%u",
				config_generation);
		} else {
			copy_string(settings.mesh_id, sizeof(settings.mesh_id), init->mesh_id);
			copy_string(settings.passphrase, sizeof(settings.passphrase), init->passphrase);
			copy_string(settings.user_name, sizeof(settings.user_name), init->user_name);
			settings.max_hop = init->max_hop;
			settings.user_id_high = init->user_id_high;
			settings.user_id_low = init->user_id_low;
			settings.user_public_key.size = init->user_public_key.size;
			memcpy(settings.user_public_key.bytes, init->user_public_key.bytes,
			       init->user_public_key.size);
			settings.share_location = init->has_location;
			settings.latitude = init->latitude;
			settings.longitude = init->longitude;
			if (init->mesh_frequency_khz > 0 && init->mesh_bandwidth_mhz > 0) {
				mesh_frequency_khz = init->mesh_frequency_khz;
				mesh_bandwidth_mhz = init->mesh_bandwidth_mhz;
				settings.mesh_frequency_khz = init->mesh_frequency_khz;
				settings.mesh_bandwidth_mhz = init->mesh_bandwidth_mhz;
			}
			normalize_settings(&settings);
			config_generation++;
			if (settings.mesh_id[0] != '\0' && mesh_frequency_khz > 0 &&
			    mesh_bandwidth_mhz > 0) {
				runtime_start_not_before_ms =
					k_uptime_get() + EDGEZ_PROVISIONING_COMMIT_GRACE_MS;
				LOG_INF("BLE HaLow startup deferred %d ms awaiting complete DeviceSettings SET",
					EDGEZ_PROVISIONING_COMMIT_GRACE_MS);
			}
			if (persistent_device_mode(settings.device_type)) {
				int persist_rc = save_persisted_profile_locked();

				if (persist_rc != 0) {
					LOG_WRN("Persistent HaLow radio update failed: %d", persist_rc);
				}
			}
			LOG_INF("BLE HaLow radio configuration frequency=%u kHz bandwidth=%u MHz user=%016llx-%016llx generation=%u",
				 mesh_frequency_khz, mesh_bandwidth_mhz,
				 (unsigned long long)settings.user_id_high,
				 (unsigned long long)settings.user_id_low,
				 config_generation);
			log_profile_readiness_locked("HaLowInitConfig");
		}
		k_mutex_unlock(&config_lock);
		response.which_body = ai_edgez_halow_NetworkPacket_status_tag;
	} else if (request.which_body == ai_edgez_halow_NetworkPacket_status_tag ||
		   request.which_body == 0) {
		response.which_body = ai_edgez_halow_NetworkPacket_status_tag;
	} else {
		return -ENOTSUP;
	}

	if (response.which_body == ai_edgez_halow_NetworkPacket_status_tag) {
		response.body.status.supported = true;
		response.body.status.stack_initialized = halow_ready;
		response.body.status.mesh_mode = true;
		response.body.status.link_up = halow_ready;
		response.body.status.route_ready = halow_ready;
		response.body.status.ready_for_report = halow_ready;
		/* nRF54 delegates license enforcement to the mobile client. */
		response.body.status.license_status =
			ai_edgez_halow_LicenseStatus_LICENSE_STATUS_AUTHORIZED;
		k_mutex_lock(&config_lock, K_FOREVER);
		copy_string(response.body.status.mesh_id, sizeof(response.body.status.mesh_id),
			    settings.mesh_id);
		LOG_INF("BLE status response supported=1 stack_initialized=%u link_up=%u license=%u mesh_id=\"%s\" frequency=%u kHz bandwidth=%u MHz",
			halow_ready, halow_ready,
			response.body.status.license_status, settings.mesh_id,
			mesh_frequency_khz, mesh_bandwidth_mhz);
		k_mutex_unlock(&config_lock);
	}

	output = pb_ostream_from_buffer(response_buf, response_cap);
	if (!pb_encode(&output, ai_edgez_halow_NetworkPacket_fields, &response)) {
		LOG_WRN("BLE NetworkPacket response encode failed: %s", PB_GET_ERROR(&output));
		return -ENOSPC;
	}
	*response_len = output.bytes_written;
	LOG_INF("BLE NetworkPacket response body=%u operation=%u len=%u",
		response.which_body, response.operation, (unsigned int)*response_len);
	return 0;
}

int edgez_config_build_vendor_ies(uint8_t *out, size_t out_cap,
				  const uint8_t current_mac[6], size_t *out_len)
{
	ai_edgez_halow_DeviceSettings snapshot;
	ai_edgez_halow_Beacon beacon = ai_edgez_halow_Beacon_init_zero;
	struct edgez_gps_fix gps_fix;
	struct edgez_imu_sample imu_sample;
	int32_t battery_mv;
	pb_ostream_t stream;
	size_t off = 2U + EDGEZ_VENDOR_LEN;

	if (!out || !out_len || !current_mac || out_cap < off) {
		return -EINVAL;
	}
	ARG_UNUSED(current_mac);
	k_mutex_lock(&config_lock, K_FOREVER);
	snapshot = settings;
	k_mutex_unlock(&config_lock);
	if ((snapshot.user_id_high == 0 && snapshot.user_id_low == 0) ||
	    snapshot.user_name[0] == '\0' || snapshot.user_public_key.size == 0 ||
	    snapshot.device_type < ai_edgez_halow_DeviceType_DEVICE_TYPE_USER ||
	    snapshot.device_type > ai_edgez_halow_DeviceType_DEVICE_TYPE_RELAY) {
		return -EINVAL;
	}
	beacon.user_id_high = snapshot.user_id_high;
	beacon.user_id_low = snapshot.user_id_low;
	copy_string(beacon.user_name, sizeof(beacon.user_name), snapshot.user_name);
	beacon.user_public_key.size = snapshot.user_public_key.size;
	memcpy(beacon.user_public_key.bytes, snapshot.user_public_key.bytes,
	       snapshot.user_public_key.size);
	beacon.marker = snapshot.marker;
	beacon.device_type = snapshot.device_type;
	/* Match edge-device-esp32-internal management beacon encoding exactly:
	 * location is SensorData and is reserved before the sampled IMU entries.
	 * The legacy top-level latitude/longitude and sleeping fields remain zero. */
	bool has_location = false;
	float latitude_value = snapshot.latitude;
	float longitude_value = snapshot.longitude;

	if (snapshot.device_gps_enabled && edgez_gps_get_fix(&gps_fix)) {
		latitude_value = gps_fix.latitude;
		longitude_value = gps_fix.longitude;
		has_location = true;
	} else if (!snapshot.device_gps_enabled && snapshot.share_location) {
		has_location = true;
	}
	if (has_location &&
	    isfinite(latitude_value) && isfinite(longitude_value) &&
	    latitude_value >= -90.0f && latitude_value <= 90.0f &&
	    longitude_value >= -180.0f && longitude_value <= 180.0f &&
	    (latitude_value != 0.0f || longitude_value != 0.0f)) {
		ai_edgez_halow_SensorData *latitude =
			&beacon.sensor_data[beacon.sensor_data_count++];
		ai_edgez_halow_SensorData *longitude =
			&beacon.sensor_data[beacon.sensor_data_count++];

		latitude->type = ai_edgez_halow_SensorType_SENSOR_LATITUDE;
		latitude->which_value = ai_edgez_halow_SensorData_float_value_tag;
		latitude->value.float_value = latitude_value;
		longitude->type = ai_edgez_halow_SensorType_SENSOR_LONGITUDE;
		longitude->which_value = ai_edgez_halow_SensorData_float_value_tag;
		longitude->value.float_value = longitude_value;
	}
	if (edgez_battery_read_mv(&battery_mv) == 0 &&
	    beacon.sensor_data_count < ARRAY_SIZE(beacon.sensor_data)) {
		ai_edgez_halow_SensorData *battery =
			&beacon.sensor_data[beacon.sensor_data_count++];

		battery->type = ai_edgez_halow_SensorType_SENSOR_BATTERY_VOLTAGE;
		battery->which_value = ai_edgez_halow_SensorData_float_value_tag;
		battery->value.float_value = battery_mv / 1000.0f;
	}
	if ((snapshot.device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_BEACON ||
	     snapshot.device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR) &&
	    edgez_imu_is_ready()) {
		int imu_rc = edgez_imu_read(&imu_sample);

		if (imu_rc < 0) {
			LOG_WRN("IMU beacon sample failed: %d", imu_rc);
		} else {
			static const ai_edgez_halow_SensorType imu_types[] = {
				ai_edgez_halow_SensorType_SENSOR_ACCEL_X,
				ai_edgez_halow_SensorType_SENSOR_ACCEL_Y,
				ai_edgez_halow_SensorType_SENSOR_ACCEL_Z,
				ai_edgez_halow_SensorType_SENSOR_GYRO_X,
				ai_edgez_halow_SensorType_SENSOR_GYRO_Y,
				ai_edgez_halow_SensorType_SENSOR_GYRO_Z,
			};
			const float imu_values[] = {
				imu_sample.accel_m_s2[0], imu_sample.accel_m_s2[1],
				imu_sample.accel_m_s2[2], imu_sample.gyro_rad_s[0],
				imu_sample.gyro_rad_s[1], imu_sample.gyro_rad_s[2],
			};

			for (size_t i = 0;
			     i < ARRAY_SIZE(imu_types) && beacon.sensor_data_count < ARRAY_SIZE(beacon.sensor_data);
			     i++) {
				ai_edgez_halow_SensorData *entry =
					&beacon.sensor_data[beacon.sensor_data_count++];

				entry->type = imu_types[i];
				entry->which_value = ai_edgez_halow_SensorData_float_value_tag;
				entry->value.float_value = imu_values[i];
			}
			LOG_DBG("IMU beacon sample accel=[%.3f %.3f %.3f] m/s2 gyro=[%.3f %.3f %.3f] rad/s",
				(double)imu_sample.accel_m_s2[0], (double)imu_sample.accel_m_s2[1],
				(double)imu_sample.accel_m_s2[2], (double)imu_sample.gyro_rad_s[0],
				(double)imu_sample.gyro_rad_s[1], (double)imu_sample.gyro_rad_s[2]);
		}
	}
	stream = pb_ostream_from_buffer(&out[off], out_cap - off);
	if (!pb_encode(&stream, ai_edgez_halow_Beacon_fields, &beacon) ||
	    stream.bytes_written > (UINT8_MAX - EDGEZ_VENDOR_LEN)) {
		return -EMSGSIZE;
	}
	out[0] = EDGEZ_IE_ID;
	out[1] = (uint8_t)(EDGEZ_VENDOR_LEN + stream.bytes_written);
	memcpy(&out[2], EDGEZ_VENDOR, EDGEZ_VENDOR_LEN);
	off += stream.bytes_written;
	*out_len = off;
	{
		k_spinlock_key_t key = k_spin_lock(&beacon_vendor_ies_lock);

		memcpy(beacon_vendor_ies, out, off);
		beacon_vendor_ies_len = off;
		k_spin_unlock(&beacon_vendor_ies_lock, key);
	}
	LOG_DBG("EdgeZ HaLow compact beacon IE built len=%u protobuf=%u device_type=%d user=%016llx-%016llx",
		(unsigned)off, (unsigned)stream.bytes_written, snapshot.device_type,
		(unsigned long long)beacon.user_id_high,
		(unsigned long long)beacon.user_id_low);
	return 0;
}
