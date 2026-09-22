#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <errno.h>

#include "meshtastic_ble.h"
#include "livestocking_config.h"
#include "meshtastic_phone_api.h"
#include "edgez_config.h"
#include "edgez_gps.h"
#include "edgez_imu.h"
#include "edgez_reboot.h"
#include "usb_control.pb.h"

#if defined(CONFIG_WIFI_MORSE_SM)
#include "morse_mesh.h"
#include <mmwlan.h>
#if __has_include(<morse_diag.h>)
#include <morse_diag.h>
#define HAS_MORSE_DIAG 1
#endif

extern const struct mmwlan_regulatory_db *get_regulatory_db(void);
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define HEARTBEAT_PERIOD_SECONDS 15
#define WIFI_CONNECT_WARN_SECONDS 30
#define WIFI_CONNECT_TIMEOUT_SECONDS 120
#define WIFI_LED_FLASH_PERIOD_MS 500
#define BLE_LED_CONNECTED_OFF_MS 5000
#define DEVICE_LED_HEARTBEAT_OFF_MS 30000
#define WIFI_CONNECT_THREAD_STACK_SIZE 8192
#define WIFI_CONNECT_THREAD_PRIORITY 7
#define HALOW_MANUAL_BOOT_THREAD_STACK_SIZE 6144
#define HALOW_MANUAL_BOOT_THREAD_PRIORITY 7
#define HALOW_START_DELAY_MS 3000
#define HALOW_BEACON_TX_FAILURE_REBOOT_LIMIT 20
#define HALOW_BEACON_REBOOT_DELAY_MS 250
#define REBOOT_BLE_RECHECK_MS 500

static atomic_t halow_beacon_tx_failure_count;
static atomic_t pending_reboot_reasons;
static atomic_t reboot_waiting_for_ble;

static void halow_beacon_reboot_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(halow_beacon_reboot_work, halow_beacon_reboot_work_handler);

static void halow_beacon_reboot_work_handler(struct k_work *work)
{
	uint32_t reasons;

	ARG_UNUSED(work);
	reasons = (uint32_t)atomic_get(&pending_reboot_reasons);
	if (reasons == 0U) {
		return;
	}
	if (meshtastic_ble_is_connected()) {
		if (atomic_cas(&reboot_waiting_for_ble, 0, 1)) {
			LOG_WRN("Reboot pending reasons=0x%02x; waiting for BLE disconnect",
				reasons);
		}
		(void)k_work_reschedule(&halow_beacon_reboot_work,
					K_MSEC(REBOOT_BLE_RECHECK_MS));
		return;
	}

	LOG_WRN("Rebooting now that BLE is disconnected reasons=0x%02x", reasons);
	sys_reboot(SYS_REBOOT_COLD);
}

void edgez_request_reboot(uint32_t reason)
{
	if (reason == 0U) {
		return;
	}
	atomic_or(&pending_reboot_reasons, (atomic_val_t)reason);
	(void)k_work_reschedule(&halow_beacon_reboot_work,
				K_MSEC(HALOW_BEACON_REBOOT_DELAY_MS));
}

/* Called from the Morse radio driver after a beacon TX completion, or when a
 * beacon cannot be submitted to the radio. Keep this callback non-blocking. */
void mmwlan_mesh_beacon_tx_status(bool success)
{
	if (success) {
		atomic_set(&halow_beacon_tx_failure_count, 0);
		return;
	}

	atomic_val_t failures = atomic_inc(&halow_beacon_tx_failure_count) + 1;

	LOG_WRN("HaLow beacon RF TX failed (%ld/%u)", (long)failures,
		(unsigned int)HALOW_BEACON_TX_FAILURE_REBOOT_LIMIT);
	if (failures == HALOW_BEACON_TX_FAILURE_REBOOT_LIMIT) {
		edgez_request_reboot(EDGEZ_REBOOT_REASON_HALOW_TX_FAILURE);
	}
}

#ifndef HALOW_WIFI_SSID
#ifdef CONFIG_WIFI_SSID
#define HALOW_WIFI_SSID CONFIG_WIFI_SSID
#else
#define HALOW_WIFI_SSID "LongFast"
#endif

#ifndef HALOW_MESH_SCAN_DWELL_MS
#define HALOW_MESH_SCAN_DWELL_MS 120U
#endif
#ifndef HALOW_MESH_SCAN_HOME_DWELL_MS
#define HALOW_MESH_SCAN_HOME_DWELL_MS 120U
#endif
#ifndef HALOW_MESH_CONNECT_SCAN_BASE_S
#define HALOW_MESH_CONNECT_SCAN_BASE_S 20U
#endif
#ifndef HALOW_MESH_CONNECT_SCAN_LIMIT_S
#define HALOW_MESH_CONNECT_SCAN_LIMIT_S 120U
#endif

static int mmwlan_status_to_errno(enum mmwlan_status status)
{
	switch (status) {
	case MMWLAN_SUCCESS:
		return 0;
	case MMWLAN_INVALID_ARGUMENT:
		return -EINVAL;
	case MMWLAN_UNAVAILABLE:
		return -EAGAIN;
	case MMWLAN_CHANNEL_LIST_NOT_SET:
	case MMWLAN_CHANNEL_INVALID:
		return -ECHRNG;
	case MMWLAN_NO_MEM:
		return -ENOMEM;
	case MMWLAN_TIMED_OUT:
		return -ETIMEDOUT;
	case MMWLAN_NOT_FOUND:
	case MMWLAN_NOT_RUNNING:
		return -ENODEV;
	case MMWLAN_ERROR:
	case MMWLAN_SHUTDOWN_BLOCKED:
	default:
		return -EIO;
	}
}
#endif

#ifndef HALOW_WIFI_PSK
#ifdef CONFIG_WIFI_PSK
#define HALOW_WIFI_PSK CONFIG_WIFI_PSK
#else
#define HALOW_WIFI_PSK ""
#endif
#endif

#ifdef CONFIG_WIFI_MORSE_REGION
#define HALOW_WIFI_REGION CONFIG_WIFI_MORSE_REGION
#else
#define HALOW_WIFI_REGION "US"
#endif

#define HALOW_MESH_VENDOR_IE_ID 221
#define HALOW_MESH_VENDOR_OUI0 'E'
#define HALOW_MESH_VENDOR_OUI1 'd'
#define HALOW_MESH_VENDOR_OUI2 'g'
#define HALOW_MESH_IE_MESH_ID 114
#define HALOW_MESH_IE_MESH_CONFIG 113
#define HALOW_MESH_VENDOR_HEADER_LEN 7
#define HALOW_MESH_SCAN_IES_BUF_LEN (512U)
#ifndef HALOW_MESH_INFO_SCAN_INTERVAL_MS
#define HALOW_MESH_INFO_SCAN_INTERVAL_MS 15000U
#endif
#define EDGEZ_IMU_BEACON_REFRESH_MS 1000U
#define EDGEZ_IMU_POST_BEACON_SETTLE_MS 3000U
#define EDGEZ_IMU_INIT_RETRY_INTERVAL_MS 2000U
#define EDGEZ_GPS_POST_BEACON_SETTLE_MS 1000U
#define GREEN_LED_NODE DT_ALIAS(led0)
#define RED_LED_NODE DT_ALIAS(led1)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#if !DT_NODE_HAS_STATUS(GREEN_LED_NODE, okay)
#error "led0 alias is not defined in devicetree"
#endif

#if !DT_NODE_EXISTS(ZEPHYR_USER_NODE)
#error "zephyr,user node is not defined in devicetree"
#endif

enum halow_state {
	HALOW_NO_SSID,
	HALOW_IDLE,
	HALOW_CONNECTING,
	HALOW_BOOTED,
	HALOW_CONNECTED,
	HALOW_ERROR,
};

static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(GREEN_LED_NODE, gpios);
#if DT_NODE_HAS_STATUS(RED_LED_NODE, okay)
static const struct gpio_dt_spec red_led = GPIO_DT_SPEC_GET(RED_LED_NODE, gpios);
#define HAS_RED_LED 1
#else
static const struct gpio_dt_spec red_led = {0};
#define HAS_RED_LED 0
#endif
static const struct gpio_dt_spec button =
	GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, key_gpios);
#if DT_NODE_HAS_PROP(ZEPHYR_USER_NODE, halow_power_en_gpios)
static const struct gpio_dt_spec halow_power_en =
	GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, halow_power_en_gpios);
#define HAS_HALOW_POWER_EN 1
#else
static const struct gpio_dt_spec halow_power_en = {0};
#define HAS_HALOW_POWER_EN 0
#endif

static bool green_led_ready;
static bool red_led_ready;
static bool button_ready;
static bool halow_power_ready;
static bool ipv4_ready;
static int wifi_last_error;
#if defined(CONFIG_WIFI) && !defined(CONFIG_WIFI_MORSE_TEST)
static int64_t wifi_connect_started_ms;
static bool wifi_connect_warned;
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;
static struct net_if *wifi_iface;
static struct k_thread wifi_connect_thread_data;
K_THREAD_STACK_DEFINE(wifi_connect_stack, WIFI_CONNECT_THREAD_STACK_SIZE);
#endif
#if defined(CONFIG_WIFI_MORSE_SM)
static bool halow_manual_boot_started;
static uint32_t edgez_applied_generation;
static bool edgez_restart_pending;
static atomic_t edgez_start_pending;
static int64_t edgez_next_start_attempt_ms;
static struct k_thread halow_manual_boot_thread_data;
K_THREAD_STACK_DEFINE(halow_manual_boot_stack, HALOW_MANUAL_BOOT_THREAD_STACK_SIZE);
#endif

static void get_halow_profile(struct meshtastic_halow_profile *profile)
{
	struct edgez_halow_profile edgez_profile = {0};
	edgez_config_get_profile(&edgez_profile);
	*profile = (struct meshtastic_halow_profile){0};
	strncpy(profile->mesh_id, edgez_profile.mesh_id, sizeof(profile->mesh_id) - 1);
	strncpy(profile->passphrase, edgez_profile.passphrase, sizeof(profile->passphrase) - 1);
	profile->passphrase_len = strlen(profile->passphrase);
	profile->mesh_frequency_khz = edgez_profile.mesh_frequency_khz;
	profile->mesh_bandwidth_mhz = edgez_profile.mesh_bandwidth_mhz;
	profile->beacon_interval_seconds = edgez_profile.beacon_interval_seconds;
}

#if defined(CONFIG_WIFI_MORSE_SM)
static void halow_beacon_vendor_ie_cb(const uint8_t *ies, uint32_t ies_len, void *arg)
{
	ARG_UNUSED(arg);

	if (!ies || ies_len == 0) {
		LOG_WRN("MM_MESH app beacon vendor IE empty");
		return;
	}

	LOG_INF("MM_MESH app beacon_vendor_ie len=%u", (unsigned int)ies_len);
	LOG_HEXDUMP_INF(ies, ies_len, "MM_MESH beacon_vendor_ie");
	/* EdgeZ beacon decoding/forwarding can be added here; advertising is independent. */
}
#endif

static atomic_t halow_state = ATOMIC_INIT(HALOW_IDLE);

static void maybe_init_imu_after_halow_start(void)
{
	static int64_t init_not_before_ms;
	static uint32_t init_failure_count;
	int64_t now;
	int rc;

	if (edgez_imu_is_ready()) {
		init_not_before_ms = 0;
		init_failure_count = 0;
		return;
	}
#if defined(CONFIG_WIFI_MORSE_MESH_MODE) && defined(CONFIG_WIFI_MORSE_SM)
	if (!edgez_config_halow_beacon_started()) {
		return;
	}
#else
	if ((enum halow_state)atomic_get(&halow_state) != HALOW_CONNECTED) {
		return;
	}
#endif

	now = k_uptime_get();
	if (init_not_before_ms == 0) {
		init_not_before_ms = now + EDGEZ_IMU_POST_BEACON_SETTLE_MS;
		LOG_INF("First HaLow beacon build observed; deferring IMU init for %u ms",
			(unsigned int)EDGEZ_IMU_POST_BEACON_SETTLE_MS);
		return;
	}
	if (now < init_not_before_ms) {
		return;
	}
	if (!edgez_imu_halow_tx_quiet()) {
		return;
	}

	rc = edgez_imu_init();
	if (rc < 0) {
		init_failure_count++;
		init_not_before_ms = now + EDGEZ_IMU_INIT_RETRY_INTERVAL_MS;
		if (init_failure_count == 1U || init_failure_count % 5U == 0U) {
			LOG_WRN("IMU unavailable during startup (%d); attempt=%u, retry in %u ms",
				rc, init_failure_count,
				(unsigned int)EDGEZ_IMU_INIT_RETRY_INTERVAL_MS);
		}
		return;
	}

	if (init_failure_count > 0) {
		LOG_INF("IMU startup recovered after %u failed attempt(s)",
			(unsigned int)init_failure_count);
	}
	init_failure_count = 0;
	init_not_before_ms = 0;
}

static void maybe_init_gps_after_halow_start(void)
{
	static int64_t init_not_before_ms;
	struct edgez_halow_profile profile = {0};
	int64_t now;

	edgez_config_get_profile(&profile);
	if (!profile.device_gps_enabled ||
	    (profile.device_type != ai_edgez_halow_DeviceType_DEVICE_TYPE_BEACON &&
	     profile.device_type != ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR)) {
		edgez_gps_stop();
		init_not_before_ms = 0;
		return;
	}
	if (edgez_gps_is_ready()) {
		return;
	}
	/* Bring up the IMU first so GPS initialization and UART traffic cannot
	 * overlap the IMU's sensitive I2C cold-start sequence. */
	if (!edgez_imu_is_ready()) {
		return;
	}
#if defined(CONFIG_WIFI_MORSE_MESH_MODE) && defined(CONFIG_WIFI_MORSE_SM)
	if (!edgez_config_halow_beacon_started()) {
		return;
	}
#else
	if ((enum halow_state)atomic_get(&halow_state) != HALOW_CONNECTED) {
		return;
	}
#endif

	now = k_uptime_get();
	if (init_not_before_ms == 0) {
		init_not_before_ms = now + EDGEZ_GPS_POST_BEACON_SETTLE_MS;
		return;
	}
	if (now >= init_not_before_ms && edgez_gps_init() < 0) {
		init_not_before_ms = now + 5000;
	}
}

#if defined(CONFIG_WIFI_MORSE_SM)
struct halow_raw_packet_header {
	uint32_t to;
	uint32_t from;
	uint32_t id;
	uint8_t flags;
	uint8_t channel;
	uint8_t next_hop;
	uint8_t relay_node;
} __packed;

static atomic_t sdk_scan_count;
static atomic_t sdk_target_scan_count;
static atomic_t sdk_last_rssi;
static atomic_t sdk_last_freq_khz;
static atomic_t sdk_last_bw_mhz;
static atomic_t sdk_mesh_raw_rx_count;
static atomic_t sdk_last_raw_payload_len;
static atomic_t sdk_last_raw_header_len;
static atomic_t sdk_last_sta_event;
static atomic_t sdk_mesh_scan_count;
static atomic_t sdk_mesh_target_scan_count;
static atomic_t sdk_mesh_last_ies_scan_state;
static atomic_t sdk_vif_up;
static int64_t mesh_scan_last_request_ms;
static atomic_t sdk_link_state;
static char sdk_last_ssid[MMWLAN_SSID_MAXLEN + 1];
static uint8_t sdk_mesh_scan_ies[HALOW_MESH_SCAN_IES_BUF_LEN];
#endif
#if defined(CONFIG_WIFI) && !defined(CONFIG_WIFI_MORSE_TEST)
static atomic_t app_connect_stage;
static atomic_t app_net_mgmt_rc;
static atomic_t app_wifi_event_status;
#endif

static void publish_status_message(const char *msg)
{
	printk("%s\n", msg);
	LOG_INF("%s", msg);
}

static const char *halow_state_name(enum halow_state state)
{
	switch (state) {
	case HALOW_NO_SSID:
		return "no_ssid";
	case HALOW_IDLE:
		return "idle";
	case HALOW_CONNECTING:
		return "connecting";
	case HALOW_BOOTED:
		return "booted";
	case HALOW_CONNECTED:
		return "connected";
	case HALOW_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

#if defined(CONFIG_WIFI_MORSE_SM)
static int connect_morse_sdk_sta(void);
static void halow_sdk_scan_rx_cb(const struct mmwlan_scan_result *result, void *arg);

static const char *mmwlan_sta_state_name(enum mmwlan_sta_state state)
{
	switch (state) {
	case MMWLAN_STA_DISABLED:
		return "disabled";
	case MMWLAN_STA_CONNECTING:
		return "connecting";
	case MMWLAN_STA_CONNECTED:
		return "connected";
	default:
		return "unknown";
	}
}

static const char *mmwlan_sta_event_name(enum mmwlan_sta_event event)
{
	switch (event) {
	case MMWLAN_STA_EVT_SCAN_REQUEST:
		return "scan_request";
	case MMWLAN_STA_EVT_SCAN_COMPLETE:
		return "scan_complete";
	case MMWLAN_STA_EVT_SCAN_ABORT:
		return "scan_abort";
	case MMWLAN_STA_EVT_AUTH_REQUEST:
		return "auth_request";
	case MMWLAN_STA_EVT_ASSOC_REQUEST:
		return "assoc_request";
	case MMWLAN_STA_EVT_DEAUTH_TX:
		return "deauth_tx";
	case MMWLAN_STA_EVT_CTRL_PORT_OPEN:
		return "ctrl_port_open";
	case MMWLAN_STA_EVT_CTRL_PORT_CLOSED:
		return "ctrl_port_closed";
	default:
		return "unknown";
	}
}

static const char *mmwlan_link_state_name(enum mmwlan_link_state state)
{
	switch (state) {
	case MMWLAN_LINK_DOWN:
		return "down";
	case MMWLAN_LINK_UP:
		return "up";
	default:
		return "other";
	}
}

static const char *mmwlan_vif_name(enum mmwlan_vif vif)
{
	switch (vif) {
	case MMWLAN_VIF_UNSPECIFIED:
		return "unspecified";
	case MMWLAN_VIF_STA:
		return "sta";
	case MMWLAN_VIF_AP:
		return "ap";
	default:
		return "unknown";
	}
}

static const char *mmwlan_vif_state_name(enum mmwlan_link_state state)
{
	switch (state) {
	case MMWLAN_LINK_DOWN:
		return "down";
	case MMWLAN_LINK_UP:
		return "up";
	default:
		return "other";
	}
}

static const char *mmwlan_scan_state_name(enum mmwlan_scan_state state)
{
	switch (state) {
	case MMWLAN_SCAN_SUCCESSFUL:
		return "successful";
	case MMWLAN_SCAN_TERMINATED:
		return "terminated";
	case MMWLAN_SCAN_RUNNING:
		return "running";
	default:
		return "unknown";
	}
}

static void halow_parse_scan_ie_mesh_info(const struct mmwlan_scan_result *result)
{
	struct meshtastic_halow_profile profile;
	const uint8_t *mesh_id = NULL;
	uint8_t mesh_id_len = 0;
	bool has_mesh_config = false;
	bool has_mesh_vendor = false;
	size_t off = 0;

	if (!result || !result->ies || result->ies_len == 0) {
		return;
	}

	get_halow_profile(&profile);
	while (off + 2U <= result->ies_len) {
		uint8_t eid = result->ies[off];
		uint8_t len = result->ies[off + 1];
		size_t next = off + 2U + len;

		if (next > result->ies_len) {
			break;
		}

		if (eid == HALOW_MESH_IE_MESH_ID && len > 0) {
			mesh_id = &result->ies[off + 2];
			mesh_id_len = len;
		} else if (eid == HALOW_MESH_IE_MESH_CONFIG) {
			has_mesh_config = true;
		} else if (eid == HALOW_MESH_VENDOR_IE_ID &&
			   len >= 5 &&
			   result->ies[off + 2] == HALOW_MESH_VENDOR_OUI0 &&
			   result->ies[off + 3] == HALOW_MESH_VENDOR_OUI1 &&
			   result->ies[off + 4] == HALOW_MESH_VENDOR_OUI2) {
			has_mesh_vendor = true;
		}

		off = next;
	}

	if (!mesh_id && !has_mesh_config && !has_mesh_vendor) {
		return;
	}

	size_t target_len = strnlen(profile.mesh_id, sizeof(profile.mesh_id));
	bool mesh_id_match = mesh_id && mesh_id_len == target_len &&
			     memcmp(mesh_id, profile.mesh_id, target_len) == 0;
	if (mesh_id_match) {
		atomic_inc(&sdk_mesh_target_scan_count);
	}

	LOG_INF("MM_MESH app mmwlan_scan_mesh_ies bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d "
		"mesh_cfg=%u vendor=%u mesh_id_match=%u mesh_id=\"%.*s\" len=%u",
		result->bssid[0], result->bssid[1], result->bssid[2],
		result->bssid[3], result->bssid[4], result->bssid[5],
		result->rssi,
		has_mesh_config ? 1 : 0, has_mesh_vendor ? 1 : 0,
		mesh_id_match ? 1 : 0,
		(int)mesh_id_len, mesh_id ? (const char *)mesh_id : "",
		(unsigned int)result->ies_len);
	if (has_mesh_vendor) {
		LOG_HEXDUMP_INF(result->ies, result->ies_len, "MM_MESH scan ies vendor path");
	}

	/* EdgeZ receive parsing is intentionally separate from the legacy packet API. */
}

static void halow_mesh_scan_complete_cb(enum mmwlan_scan_state scan_state, void *arg)
{
	ARG_UNUSED(arg);

	atomic_set(&sdk_mesh_last_ies_scan_state, scan_state);
	LOG_INF("MM_MESH app mmwlan_mesh_scan_complete state=%s(%d) total=%d target=%d",
		mmwlan_scan_state_name(scan_state), scan_state,
		(int)atomic_get(&sdk_mesh_scan_count),
		(int)atomic_get(&sdk_mesh_target_scan_count));
}

static bool request_mesh_info_scan(void)
{
	struct mmwlan_scan_req req = MMWLAN_SCAN_REQ_INIT;
	struct meshtastic_halow_profile profile;
	enum mmwlan_status status;
	uint8_t *mesh_scan_ies = sdk_mesh_scan_ies;
	uint8_t discovery_vendor_ie[EDGEZ_VENDOR_IES_MAX_LEN] = {0};
	uint8_t current_mac[6] = {0};
	size_t discovery_vendor_ie_len = 0;
	size_t mesh_scan_ies_cap = sizeof(sdk_mesh_scan_ies);
	size_t mesh_id_len;
	size_t mesh_scan_len = 0;

	get_halow_profile(&profile);
	mesh_id_len = strnlen(profile.mesh_id, sizeof(profile.mesh_id));
	if (mesh_id_len == 0) {
		LOG_WRN("MM_MESH app mesh scan skipped, empty mesh id");
		return false;
	}

	if (atomic_get(&sdk_mesh_last_ies_scan_state) == MMWLAN_SCAN_RUNNING) {
		LOG_INF("MM_MESH app mesh scan request skipped, previous scan still running");
		return false;
	}

	mesh_scan_ies[0] = HALOW_MESH_IE_MESH_ID;
	mesh_scan_ies[1] = (uint8_t)mesh_id_len;
	memcpy(&mesh_scan_ies[2], profile.mesh_id, mesh_id_len);
	mesh_scan_len = 2U + mesh_id_len;

	(void)mmwlan_get_vif_mac_addr(MMWLAN_VIF_STA, current_mac);
	if (edgez_config_build_vendor_ies(discovery_vendor_ie, sizeof(discovery_vendor_ie),
					 current_mac, &discovery_vendor_ie_len) != 0) {
		discovery_vendor_ie_len = 0;
	}
	if (discovery_vendor_ie_len > 0 &&
	    mesh_scan_len + discovery_vendor_ie_len <= mesh_scan_ies_cap) {
		memcpy(&mesh_scan_ies[mesh_scan_len], discovery_vendor_ie, discovery_vendor_ie_len);
		mesh_scan_len += discovery_vendor_ie_len;
	}

	req.scan_rx_cb = halow_sdk_scan_rx_cb;
	req.scan_complete_cb = halow_mesh_scan_complete_cb;
	req.scan_cb_arg = NULL;
	req.args.dwell_time_ms = HALOW_MESH_SCAN_DWELL_MS;
	memcpy(req.args.ssid, profile.mesh_id, mesh_id_len);
	req.args.ssid_len = mesh_id_len;
	req.args.extra_ies = mesh_scan_ies;
	req.args.extra_ies_len = mesh_scan_len;

	mesh_scan_last_request_ms = k_uptime_get();
	status = mmwlan_scan_request(&req);
	if (status != MMWLAN_SUCCESS) {
		LOG_WRN("MM_MESH app mmwlan_scan_request failed=%d errno=%d", status,
			mmwlan_status_to_errno(status));
		return false;
	}

	atomic_set(&sdk_mesh_last_ies_scan_state, MMWLAN_SCAN_RUNNING);
	atomic_inc(&sdk_mesh_scan_count);
	LOG_INF("MM_MESH app mesh scan requested mesh_id=\"%s\" extra_ies=%u dwell=%ums", profile.mesh_id,
		(unsigned int)mesh_scan_len, (unsigned int)req.args.dwell_time_ms);
	LOG_HEXDUMP_INF(mesh_scan_ies, mesh_scan_len < 48U ? mesh_scan_len : 48U, "MM_MESH mesh_scan_ies");
	return true;
}

static void halow_sdk_sta_status_cb(enum mmwlan_sta_state sta_state)
{
	LOG_INF("MM_MESH app mmwlan_sta_status state=%d", sta_state);
	switch (sta_state) {
	case MMWLAN_STA_DISABLED:
		atomic_set(&halow_state, HALOW_BOOTED);
		break;
	case MMWLAN_STA_CONNECTING:
		/* Discovery-only mesh startup has no peer-connected terminal state. */
		atomic_set(&halow_state,
			   IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ?
			   HALOW_BOOTED : HALOW_CONNECTING);
		break;
	case MMWLAN_STA_CONNECTED:
		atomic_set(&halow_state, HALOW_CONNECTED);
		break;
	default:
		break;
	}
}

static void halow_sdk_sta_event_cb(const struct mmwlan_sta_event_cb_args *sta_event, void *arg)
{
	ARG_UNUSED(arg);

	if (sta_event) {
		LOG_INF("MM_MESH app mmwlan_sta_event event=%d(%s) arg=0x%p",
			sta_event->event, mmwlan_sta_event_name(sta_event->event), arg);
		atomic_set(&sdk_last_sta_event, sta_event->event);
	}
}

static void halow_mesh_link_state_cb(enum mmwlan_link_state link_state, void *arg)
{
	ARG_UNUSED(arg);

	atomic_set(&sdk_link_state, (int)link_state);
	LOG_INF("MM_MESH app mmwlan_link_state=%s(%d)", mmwlan_link_state_name(link_state), (int)link_state);
}

static void halow_mesh_vif_state_cb(const struct mmwlan_vif_state *state, void *arg)
{
	ARG_UNUSED(arg);

	if (!state) {
		return;
	}

	atomic_set(&sdk_link_state, (int)state->link_state);
	if (state->vif == MMWLAN_VIF_STA) {
		atomic_set(&sdk_vif_up, state->link_state == MMWLAN_LINK_UP ? 1 : 0);
	}
	LOG_INF("MM_MESH app mmwlan_vif_state vif=%s(%d) link=%s(%d)",
		mmwlan_vif_name(state->vif), (int)state->vif,
		mmwlan_vif_state_name(state->link_state), (int)state->link_state);
}

static void halow_mesh_rx_cb(uint8_t *header, unsigned header_len,
				   uint8_t *payload, unsigned payload_len, void *arg)
{
	ARG_UNUSED(arg);

	atomic_inc(&sdk_mesh_raw_rx_count);
	atomic_set(&sdk_last_raw_header_len, (int)header_len);
	atomic_set(&sdk_last_raw_payload_len, (int)payload_len);
	LOG_INF("MM_MESH app raw_rx count=%u header_len=%u payload_len=%u", (unsigned)atomic_get(&sdk_mesh_raw_rx_count),
		header_len, payload_len);
	if (header && header_len >= (unsigned)sizeof(struct halow_raw_packet_header)) {
		const struct halow_raw_packet_header *halow_header =
			(const struct halow_raw_packet_header *)header;

		LOG_INF("MM_MESH app raw_rx frame id=0x%x to=0x%x from=0x%x ch=%u flags=0x%02x hop_limit=%u hop_start=%u want_ack=%u via_mqtt=%u next_hop=0x%02x relay_node=0x%02x payload_len=%u",
			halow_header->id, halow_header->to, halow_header->from,
			halow_header->channel, halow_header->flags,
			(unsigned int)(halow_header->flags & 0x07U),
			(unsigned int)(halow_header->flags >> 5U),
			(unsigned int)((halow_header->flags & 0x08U) != 0U),
			(unsigned int)((halow_header->flags & 0x10U) != 0U),
			halow_header->next_hop, halow_header->relay_node,
			payload_len);
	}
	if (header && header_len > 0U) {
		LOG_HEXDUMP_INF(header, header_len < 32U ? header_len : 32U, "MM_MESH raw_rx_header");
	}
	if (payload && payload_len > 0U) {
		LOG_HEXDUMP_INF(payload, payload_len < 64U ? payload_len : 64U, "MM_MESH raw_rx_payload");
	}
}

static void halow_sdk_scan_rx_cb(const struct mmwlan_scan_result *result, void *arg)
{
	ARG_UNUSED(arg);

	if (!result) {
		return;
	}

	atomic_inc(&sdk_scan_count);
	atomic_set(&sdk_last_rssi, result->rssi);
	atomic_set(&sdk_last_freq_khz, result->channel_freq_hz / 1000);
	atomic_set(&sdk_last_bw_mhz, result->bw_mhz);
	LOG_INF("MM_MESH app mmwlan_scan_result len=%u freq=%u rssi=%d scan_count=%u",
		(unsigned int)result->ssid_len, (unsigned int)result->channel_freq_hz,
		result->rssi, (unsigned int)atomic_get(&sdk_scan_count));

	size_t ssid_len = result->ssid_len;
	struct meshtastic_halow_profile profile;
	if (ssid_len > MMWLAN_SSID_MAXLEN) {
		ssid_len = MMWLAN_SSID_MAXLEN;
	}
	memcpy(sdk_last_ssid, result->ssid, ssid_len);
	sdk_last_ssid[ssid_len] = '\0';

	get_halow_profile(&profile);
	if (ssid_len == strlen(profile.mesh_id) &&
	    memcmp(result->ssid, profile.mesh_id, ssid_len) == 0) {
		atomic_inc(&sdk_target_scan_count);
	}

	halow_parse_scan_ie_mesh_info(result);
}
#endif

static void set_led(const struct gpio_dt_spec *led, bool ready, bool on)
{
	if (ready) {
		(void)gpio_pin_set_dt(led, on ? 1 : 0);
	}
}

static void set_status_leds(bool red_on, bool green_on)
{
	set_led(&green_led, green_led_ready, green_on);

	if (HAS_RED_LED && red_led_ready) {
		set_led(&red_led, red_led_ready, red_on);
	} else if (red_on) {
		set_led(&green_led, green_led_ready, true);
	}
}

static void update_status_leds(void)
{
	static int64_t last_flash_ms;
	static bool flash_green;
	static int64_t ble_connected_cycle_start_ms;
	static bool ble_was_connected;
	struct edgez_halow_profile profile = {0};
	int64_t now_ms = k_uptime_get();
	bool ble_connected = meshtastic_ble_is_connected();

	enum halow_state state = (enum halow_state)atomic_get(&halow_state);
	edgez_config_get_profile(&profile);

	if (state == HALOW_ERROR) {
		set_status_leds(true, false);
		return;
	}

	if ((profile.device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_BEACON ||
	     profile.device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR) &&
	    !meshtastic_ble_is_enabled()) {
		int64_t cycle_ms = WIFI_LED_FLASH_PERIOD_MS + DEVICE_LED_HEARTBEAT_OFF_MS;
		set_status_leds(false, now_ms % cycle_ms < WIFI_LED_FLASH_PERIOD_MS);
		return;
	}

	if (profile.device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_USER) {
		bool ble_enabled = meshtastic_ble_is_enabled();

		if (ble_enabled && !ble_connected) {
			ble_was_connected = false;
			if (now_ms - last_flash_ms >= WIFI_LED_FLASH_PERIOD_MS) {
				flash_green = !flash_green;
				last_flash_ms = now_ms;
			}
			set_status_leds(false, flash_green);
			return;
		}

		if (ble_enabled && !ble_was_connected) {
			ble_was_connected = true;
			ble_connected_cycle_start_ms = now_ms;
		}
		int64_t cycle_ms = WIFI_LED_FLASH_PERIOD_MS + BLE_LED_CONNECTED_OFF_MS;
		/* Once BLE sleeps there is no connection event to anchor the cycle, so
		 * use uptime for the same slow heartbeat pattern. */
		int64_t elapsed_ms = ble_enabled ?
			(now_ms - ble_connected_cycle_start_ms) % cycle_ms :
			now_ms % cycle_ms;
		set_status_leds(false, elapsed_ms < WIFI_LED_FLASH_PERIOD_MS);
		return;
	}

	switch (state) {
	case HALOW_NO_SSID:
	case HALOW_IDLE:
		set_status_leds(false, false);
		break;
	case HALOW_CONNECTING:
		if (now_ms - last_flash_ms >= WIFI_LED_FLASH_PERIOD_MS) {
			flash_green = !flash_green;
			last_flash_ms = now_ms;
		}
		if (HAS_RED_LED && red_led_ready) {
			set_status_leds(!flash_green, flash_green);
		} else {
			set_status_leds(false, flash_green);
		}
		break;
	case HALOW_CONNECTED:
	case HALOW_BOOTED:
		set_status_leds(false, true);
		break;
	case HALOW_ERROR:
		set_status_leds(true, false);
		break;
	default:
		set_status_leds(false, false);
		break;
	}
}

static void publish_heartbeat(void)
{
#if defined(CONFIG_EDGEZ_VERBOSE_HEARTBEAT)
	static uint32_t seq;
	enum halow_state state = (enum halow_state)atomic_get(&halow_state);
	const char *iface_state = "unknown";
	int iface_status_rc = -ENODEV;
	const char *morse_stage = "n/a";
	const char *morse_sta = "n/a";
	const char *morse_evt = "n/a";
	const char *scan_ssid = "n/a";
	const char *morse_fw = "n/a";
	const char *morselib = "n/a";
	const char *morse_chip = "n/a";
	const char *morse_bcf = "n/a";
	const char *morse_bcf_build = "n/a";
	uint32_t morse_chip_id = 0;
	int scan_count = 0;
	int target_scan_count = 0;
	int scan_rssi = 0;
	int scan_freq_khz = 0;
	int scan_bw_mhz = 0;
	int morse_boot_errno = 0;
	int morse_boot_status = 0;
	int mesh_scan_count = 0;
	int mesh_target_scan_count = 0;
	int mesh_scan_state = 0;
	int mesh_raw_rx_count = 0;
	int mesh_raw_header_len = 0;
	int mesh_raw_payload_len = 0;
	int link_state = -1;
	int vif_up = 0;
	int connect_stage = -1;
	int net_mgmt_rc = -9999;
	int wifi_event_status = -9999;
	uint8_t morse_mac[6] = {0};
	struct meshtastic_halow_profile profile;

	get_halow_profile(&profile);

#if defined(CONFIG_WIFI) && !defined(CONFIG_WIFI_MORSE_TEST)
	struct wifi_iface_status iface_status = {0};

	if (wifi_iface) {
		iface_status_rc = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, wifi_iface,
					   &iface_status, sizeof(iface_status));
		if (iface_status_rc == 0) {
			iface_state = wifi_state_txt(iface_status.state);
		}
	}
	connect_stage = atomic_get(&app_connect_stage);
	net_mgmt_rc = atomic_get(&app_net_mgmt_rc);
	wifi_event_status = atomic_get(&app_wifi_event_status);
#endif

#if defined(HAS_MORSE_DIAG)
	struct morse_diag morse = {0};

	morse_get_diag(&morse);
	morse_stage = morse.boot_stage;
	morse_fw = morse.fw_version;
	morselib = morse.morselib_version;
	morse_chip = morse.chip_id_string;
	morse_bcf = morse.bcf_board_desc;
	morse_bcf_build = morse.bcf_build_version;
	morse_chip_id = morse.chip_id;
	morse_boot_errno = morse.boot_errno;
	morse_boot_status = morse.boot_mmwlan_status;
	memcpy(morse_mac, morse.mac_addr, sizeof(morse_mac));
#elif defined(CONFIG_WIFI_MORSE_SM)
	if (state == HALOW_CONNECTING || state == HALOW_BOOTED ||
	    state == HALOW_CONNECTED || state == HALOW_ERROR) {
		struct mmwlan_version version = {0};
		struct mmwlan_bcf_metadata bcf = {0};
		enum mmwlan_status status;

		status = mmwlan_get_version(&version);
		morse_boot_status = status;
		if (status == MMWLAN_SUCCESS) {
			morselib = version.morselib_version[0] ? version.morselib_version : "n/a";
			morse_fw = version.morse_fw_version[0] ? version.morse_fw_version : "n/a";
			morse_chip = version.morse_chip_id_string[0] ?
				     version.morse_chip_id_string : "n/a";
			morse_chip_id = version.morse_chip_id;
		}

		status = mmwlan_get_mac_addr(morse_mac);
		if (status != MMWLAN_SUCCESS) {
			memset(morse_mac, 0, sizeof(morse_mac));
		}

		status = mmwlan_get_bcf_metadata(&bcf);
		if (status == MMWLAN_SUCCESS) {
			morse_bcf = bcf.board_desc[0] ? bcf.board_desc : "n/a";
			morse_bcf_build = bcf.build_version[0] ? bcf.build_version : "n/a";
		}
	}
#endif

#if defined(CONFIG_WIFI_MORSE_SM)
	morse_sta = mmwlan_sta_state_name(mmwlan_get_sta_state());
	morse_evt = mmwlan_sta_event_name((enum mmwlan_sta_event)atomic_get(&sdk_last_sta_event));
	scan_count = atomic_get(&sdk_scan_count);
	target_scan_count = atomic_get(&sdk_target_scan_count);
	scan_rssi = atomic_get(&sdk_last_rssi);
	scan_freq_khz = atomic_get(&sdk_last_freq_khz);
	scan_bw_mhz = atomic_get(&sdk_last_bw_mhz);
	mesh_scan_count = (int)atomic_get(&sdk_mesh_scan_count);
	mesh_target_scan_count = (int)atomic_get(&sdk_mesh_target_scan_count);
	mesh_scan_state = (int)atomic_get(&sdk_mesh_last_ies_scan_state);
	mesh_raw_rx_count = (int)atomic_get(&sdk_mesh_raw_rx_count);
	mesh_raw_header_len = (int)atomic_get(&sdk_last_raw_header_len);
	mesh_raw_payload_len = (int)atomic_get(&sdk_last_raw_payload_len);
	link_state = (int)atomic_get(&sdk_link_state);
	vif_up = (int)atomic_get(&sdk_vif_up);
	if (sdk_last_ssid[0]) {
		scan_ssid = sdk_last_ssid;
	}
#endif

#if defined(CONFIG_WIFI_MORSE_SM)
	LOG_INF("MM_MESH app heartbeat sdk_mesh_scan=%d mesh_target=%d mesh_state=%d(%s)",
		mesh_scan_count, mesh_target_scan_count, mesh_scan_state,
		mmwlan_scan_state_name((enum mmwlan_scan_state)mesh_scan_state));
	LOG_INF("MM_MESH app heartbeat mesh_link=%s(%d) mesh_vif_up=%d raw_rx_count=%d raw_header=%d raw_payload=%d",
		mmwlan_link_state_name((enum mmwlan_link_state)link_state), link_state, vif_up,
		mesh_raw_rx_count, mesh_raw_header_len, mesh_raw_payload_len);
#endif

	printk("[hb] seq=%u uptime_ms=%u led_ready=%d red_led_ready=%d button_ready=%d "
	       "halow_power_ready=%d halow=%s halow_mode=%s iface=%s iface_rc=%d "
	       "connect_stage=%d net_mgmt_rc=%d wifi_evt=%d "
	       "mesh_id=\"%s\" country=%s cfg_freq_khz=%u cfg_bw_mhz=%u "
	       "wifi_security=%s wifi_passphrase_len=%u "
	       "ipv4=%d wifi_err=%d ble_enabled=%d ble_connected=%d "
	       "morse_stage=%s morse_sta=%s morse_evt=%s sdk_scan=%d sdk_target_scan=%d "
	       "sdk_last_ssid=\"%s\" sdk_rssi=%d sdk_freq_khz=%d sdk_bw=%d "
	       "morse_boot_err=%d morse_boot_status=%d morse_fw=\"%s\" "
	       "morselib=\"%s\" morse_chip=\"%s\" morse_chip_id=0x%08x "
	       "morse_serial=\"%02x%02x%02x%02x%02x%02x\" morse_bcf=\"%s\" morse_bcf_build=\"%s\"\n",
	       seq++, k_uptime_get_32(), green_led_ready, red_led_ready, button_ready,
	       halow_power_ready, halow_state_name(state),
	       IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? "mesh" : "sta",
	       iface_state, iface_status_rc, connect_stage, net_mgmt_rc,
	       wifi_event_status, profile.mesh_id, livestock_config_country(),
	       profile.mesh_frequency_khz, profile.mesh_bandwidth_mhz,
	       IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? "open" : "WPA3-SAE",
	       IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? 0U :
		       (unsigned int)profile.passphrase_len,
	       ipv4_ready, wifi_last_error, meshtastic_ble_is_enabled(),
	       meshtastic_ble_is_connected(),
	       morse_stage, morse_sta, morse_evt, scan_count, target_scan_count,
	       scan_ssid, scan_rssi, scan_freq_khz, scan_bw_mhz,
	       morse_boot_errno, morse_boot_status, morse_fw, morselib,
	       morse_chip, morse_chip_id, morse_mac[0], morse_mac[1], morse_mac[2],
	       morse_mac[3], morse_mac[4], morse_mac[5], morse_bcf, morse_bcf_build);
#else
	static uint32_t seq;
	enum halow_state state = (enum halow_state)atomic_get(&halow_state);

	LOG_INF("heartbeat seq=%u halow=%s ble=%s ipv4=%d",
		seq++, halow_state_name(state),
		meshtastic_ble_is_connected() ? "connected" : "idle", ipv4_ready);
#endif
}

#if defined(CONFIG_WIFI_MORSE_TEST)
static void start_halow_manual_boot(void);
#endif

static void poll_button(void)
{
	static bool was_pressed;

	if (!button_ready) {
		return;
	}

	int pressed = gpio_pin_get_dt(&button);
	if (pressed < 0) {
		printk("Failed to read KEY state: %d\n", pressed);
		LOG_ERR("Failed to read KEY state: %d", pressed);
		button_ready = false;
		return;
	}

	if (pressed && !was_pressed) {
		LOG_INF("KEY pressed");
#if defined(CONFIG_WIFI_MORSE_TEST)
		start_halow_manual_boot();
#endif
		if (!meshtastic_ble_is_enabled() && !meshtastic_ble_is_connected()) {
			int rc = meshtastic_ble_start();
			LOG_INF("KEY re-enabled BLE provisioning rc=%d", rc);
		}
	}

	was_pressed = pressed != 0;
}

#if defined(CONFIG_WIFI) && !defined(CONFIG_WIFI_MORSE_TEST)
static void wifi_mgmt_event(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			    struct net_if *iface)
{
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		wifi_last_error = status ? status->status : -ENODATA;
		atomic_set(&app_connect_stage, 7);
		atomic_set(&app_wifi_event_status, wifi_last_error);
		printk("[MM_MESH] app wifi_event connect_result status=%d\n", wifi_last_error);
		LOG_INF("MM_MESH app wifi_event connect_result status=%d", wifi_last_error);
		if (status && status->status == WIFI_STATUS_CONN_SUCCESS) {
			atomic_set(&halow_state, HALOW_CONNECTED);
			ipv4_ready = false;
			if (IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE)) {
				publish_status_message("HaLow mesh interface ready");
			} else {
				publish_status_message("HaLow connected; starting DHCPv4");
				net_dhcpv4_start(wifi_iface);
			}
		} else {
			if (IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE)) {
				atomic_set(&halow_state, HALOW_BOOTED);
				LOG_WRN("MM_MESH app mesh link not up yet status=%d", wifi_last_error);
			} else {
				atomic_set(&halow_state, HALOW_ERROR);
				LOG_ERR("HaLow connect failed: %d", wifi_last_error);
			}
		}
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		wifi_last_error = status ? status->status : -ENODATA;
		atomic_set(&app_connect_stage, 8);
		atomic_set(&app_wifi_event_status, wifi_last_error);
		ipv4_ready = false;
		atomic_set(&halow_state,
			   IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? HALOW_BOOTED : HALOW_ERROR);
		printk("[MM_MESH] app wifi_event disconnect_result status=%d\n", wifi_last_error);
		LOG_WRN("HaLow %sdisconnected: %d",
			IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? "mesh " : "",
			wifi_last_error);
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(mgmt_event);
	ARG_UNUSED(iface);

	ipv4_ready = true;
	publish_status_message("HaLow IPv4 address ready");
}

static void wifi_connect_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	struct wifi_connect_req_params params = {0};
	struct meshtastic_halow_profile profile;

	atomic_set(&app_connect_stage, 4);
	printk("[MM_MESH] app wifi_connect_thread start\n");
	LOG_INF("MM_MESH app wifi_connect_thread start");
	get_halow_profile(&profile);
	params.ssid = profile.mesh_id;
	params.ssid_length = strlen(profile.mesh_id);
	params.channel = WIFI_CHANNEL_ANY;
#if defined(CONFIG_WIFI_MORSE_MESH_MODE) && defined(CONFIG_WIFI_MORSE_SM)
	params.psk = NULL;
	params.psk_length = 0;
	params.security = WIFI_SECURITY_TYPE_NONE;
	params.mfp = WIFI_MFP_DISABLE;
#else
	params.psk = profile.passphrase;
	params.psk_length = profile.passphrase_len;
	params.security = WIFI_SECURITY_TYPE_SAE;
	params.mfp = WIFI_MFP_OPTIONAL;
#endif

	wifi_last_error = 0;
	wifi_connect_started_ms = k_uptime_get();
	wifi_connect_warned = false;
	atomic_set(&halow_state, HALOW_CONNECTING);

	LOG_INF("Connecting HaLow mesh_id=\"%s\" mode=%s security=%s country=%s wifi_passphrase_len=%u",
		profile.mesh_id,
		IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? "mesh" : "sta",
		params.security == WIFI_SECURITY_TYPE_NONE ? "open" : "WPA3-SAE",
		livestock_config_country(),
		(unsigned int)params.psk_length);
#if defined(CONFIG_WIFI_MORSE_SM)
	meshtastic_phone_api_register_halow_rx();
#endif
#if defined(CONFIG_WIFI_MORSE_MESH_MODE) && defined(CONFIG_WIFI_MORSE_SM)
	LOG_INF("MM_MESH app using direct Morse SDK mesh start");
	int rc = connect_morse_sdk_sta();
	atomic_set(&app_connect_stage, 6);
	atomic_set(&app_net_mgmt_rc, rc);
	if (rc == 0) {
		atomic_set(&edgez_start_pending, 0);
		atomic_set(&halow_state, HALOW_BOOTED);
		LOG_INF("MM_MESH app discovery-only advertiser start accepted");
		return;
	}
	atomic_set(&edgez_start_pending, 0);
	wifi_last_error = rc;
	atomic_set(&halow_state, HALOW_ERROR);
	LOG_ERR("HaLow connect request failed: %d", rc);
	return;
#else
	atomic_set(&app_connect_stage, 5);
	printk("[MM_MESH] app net_mgmt_connect begin mesh_id=\"%s\" mode=%s psk_len=%u iface=%p\n",
	       profile.mesh_id, IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? "mesh" : "sta",
	       (unsigned int)params.psk_length, wifi_iface);
	int rc = net_mgmt(NET_REQUEST_WIFI_CONNECT, wifi_iface, &params, sizeof(params));
	atomic_set(&app_connect_stage, 6);
	atomic_set(&app_net_mgmt_rc, rc);
	printk("[MM_MESH] app net_mgmt_connect rc=%d\n", rc);
	LOG_INF("MM_MESH app net_mgmt_connect rc=%d", rc);
	if (rc) {
#if defined(CONFIG_WIFI_MORSE_MESH_MODE) && defined(CONFIG_WIFI_MORSE_SM)
		LOG_WRN("MM_MESH app net_mgmt mesh connect rejected rc=%d, falling back to direct mmwlan mesh start",
			rc);
		rc = connect_morse_sdk_sta();
		atomic_set(&app_net_mgmt_rc, rc);
		if (rc == 0) {
			atomic_set(&halow_state, HALOW_BOOTED);
			LOG_INF("MM_MESH app direct mmwlan mesh start accepted");
			return;
		}
#endif
		wifi_last_error = rc;
		atomic_set(&halow_state, HALOW_ERROR);
		LOG_ERR("HaLow connect request failed: %d", rc);
	} else if (IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE)) {
		atomic_set(&halow_state, HALOW_BOOTED);
		LOG_INF("MM_MESH app mesh start request accepted");
	}
#endif
}

static void start_halow_connect(void)
{
	struct meshtastic_halow_profile profile;

	get_halow_profile(&profile);
	if (!edgez_config_is_complete()) {
		atomic_set(&edgez_start_pending, 0);
		atomic_set(&halow_state, HALOW_IDLE);
		LOG_WRN("HaLow runtime startup rejected: incomplete profile mesh_id_len=%u frequency=%u kHz bandwidth=%u MHz",
			(unsigned int)strlen(profile.mesh_id), profile.mesh_frequency_khz,
			profile.mesh_bandwidth_mhz);
		return;
	}
	LOG_INF("HaLow runtime startup accepted mesh_id=\"%s\" frequency=%u kHz bandwidth=%u MHz persistent_autostart=%u",
		profile.mesh_id, profile.mesh_frequency_khz, profile.mesh_bandwidth_mhz,
		edgez_config_should_autostart());
	atomic_set(&app_connect_stage, 1);
	atomic_set(&app_net_mgmt_rc, -9999);
	atomic_set(&app_wifi_event_status, -9999);
	printk("[MM_MESH] app start_halow_connect connect_path=%s mesh_id=\"%s\"\n",
	       IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? "direct_morse_sdk" : "zephyr_net_mgmt",
	       profile.mesh_id);
	LOG_INF("MM_MESH app start_halow_connect path=%s mesh_id=\"%s\"",
		IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? "direct_morse_sdk" : "zephyr_net_mgmt",
		profile.mesh_id);
	if (strlen(profile.mesh_id) == 0) {
		atomic_set(&halow_state, HALOW_NO_SSID);
		publish_status_message("HaLow mesh ID is empty; LED off and Wi-Fi connect skipped");
		return;
	}

	wifi_iface = net_if_get_first_wifi();
	if (!wifi_iface) {
		wifi_last_error = -ENODEV;
		atomic_set(&app_connect_stage, 2);
		atomic_set(&halow_state, HALOW_ERROR);
		publish_status_message("No HaLow Wi-Fi interface found");
		return;
	}
	printk("[MM_MESH] app using Zephyr Wi-Fi iface=%p\n", wifi_iface);
	LOG_INF("MM_MESH app using Zephyr Wi-Fi iface=%p", wifi_iface);

	net_mgmt_init_event_callback(&wifi_cb, wifi_mgmt_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	atomic_set(&edgez_start_pending, 1);
	k_thread_create(&wifi_connect_thread_data, wifi_connect_stack,
			K_THREAD_STACK_SIZEOF(wifi_connect_stack), wifi_connect_thread,
			NULL, NULL, NULL, WIFI_CONNECT_THREAD_PRIORITY, 0, K_NO_WAIT);
	atomic_set(&app_connect_stage, 3);
	printk("[MM_MESH] app wifi_connect_thread submitted\n");
	LOG_INF("MM_MESH app wifi_connect_thread submitted iface=%p", wifi_iface);
}

static void check_connect_timeout(void)
{
	if (IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE)) {
		return;
	}

	if ((enum halow_state)atomic_get(&halow_state) != HALOW_CONNECTING) {
		return;
	}

	int64_t elapsed_ms = k_uptime_get() - wifi_connect_started_ms;

	if (!wifi_connect_warned && elapsed_ms > WIFI_CONNECT_WARN_SECONDS * 1000) {
		wifi_connect_warned = true;
		LOG_WRN("HaLow still connecting after %d seconds", WIFI_CONNECT_WARN_SECONDS);
	}

	if (elapsed_ms > WIFI_CONNECT_TIMEOUT_SECONDS * 1000) {
		wifi_last_error = -ETIMEDOUT;
		atomic_set(&halow_state, HALOW_ERROR);
		LOG_ERR("HaLow connect timeout");
	}
}

static void maybe_run_mesh_scan(void)
{
	int state = atomic_get(&halow_state);
	int64_t now = k_uptime_get();

	if (state != HALOW_CONNECTED) {
		return;
	}
	if ((int64_t)(now - mesh_scan_last_request_ms) < HALOW_MESH_INFO_SCAN_INTERVAL_MS) {
		return;
	}
	if (atomic_get(&sdk_mesh_last_ies_scan_state) == MMWLAN_SCAN_RUNNING) {
		return;
	}

	(void)request_mesh_info_scan();
}

static void maybe_refresh_edgez_sensor_beacon(void)
{
	static int64_t last_refresh_ms;
	struct edgez_halow_profile profile = {0};
	uint8_t vendor_ies[EDGEZ_VENDOR_IES_MAX_LEN];
	uint8_t current_mac[6] = {0};
	size_t vendor_ies_len = 0;
	int64_t now = k_uptime_get();

	/* Morse mesh starts beaconing before its station state necessarily reaches
	 * MMWLAN_STA_CONNECTED. Gate on the applied interface generation instead;
	 * requiring HALOW_CONNECTED leaves the initial IMU payload cached forever
	 * on a beacon-only mesh node. */
	if (edgez_applied_generation == 0 || edgez_restart_pending ||
	    (!edgez_imu_is_ready() && !edgez_gps_is_ready())) {
		return;
	}

	edgez_config_get_profile(&profile);
	if (profile.device_type != ai_edgez_halow_DeviceType_DEVICE_TYPE_BEACON &&
	    profile.device_type != ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR) {
		return;
	}
	if (last_refresh_ms != 0 &&
	    now - last_refresh_ms < EDGEZ_IMU_BEACON_REFRESH_MS) {
		return;
	}

	last_refresh_ms = now;
	(void)mmwlan_get_vif_mac_addr(MMWLAN_VIF_STA, current_mac);
	if (edgez_config_build_vendor_ies(vendor_ies, sizeof(vendor_ies),
					 current_mac, &vendor_ies_len) == 0) {
		LOG_DBG("Refreshed dynamic sensor beacon IE len=%u", (unsigned)vendor_ies_len);
	}
}

#endif

static void configure_led(const struct gpio_dt_spec *led, bool *ready, const char *name)
{
	if (!gpio_is_ready_dt(led)) {
		LOG_ERR("%s device is not ready", name);
		return;
	}

	int rc = gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
	if (rc < 0) {
		LOG_ERR("Failed to configure %s pin: %d", name, rc);
		return;
	}

	*ready = true;
}

static void configure_halow_power(void)
{
	if (halow_power_ready) {
		return;
	}

	if (!HAS_HALOW_POWER_EN) {
		LOG_WRN("HaLow power-enable GPIO is not configured");
		return;
	}

	if (!gpio_is_ready_dt(&halow_power_en)) {
		LOG_ERR("HaLow power-enable GPIO device is not ready");
		return;
	}

	int rc = gpio_pin_configure_dt(&halow_power_en, GPIO_OUTPUT_ACTIVE);
	if (rc < 0) {
		LOG_ERR("Failed to assert HaLow power-enable GPIO: %d", rc);
		return;
	}

	halow_power_ready = true;
	LOG_INF("HaLow power enabled on GPIO %s pin %u",
		halow_power_en.port->name, halow_power_en.pin);
	k_msleep(500);
}

#if defined(CONFIG_WIFI_MORSE_SM)
static int connect_morse_sdk_sta(void)
{
	struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;
	struct mmwlan_scan_config scan_config = MMWLAN_SCAN_CONFIG_INIT;
	struct meshtastic_halow_profile profile;
	struct mmwlan_beacon_vendor_ie_filter beacon_filter = {0};
	uint8_t discovery_vendor_ie[EDGEZ_VENDOR_IES_MAX_LEN] = {0};
	uint8_t current_mac[6] = {0};
	size_t discovery_vendor_ie_len = 0;
	enum mmwlan_status status;
	size_t ssid_len;
	size_t psk_len;
	int rc;

	get_halow_profile(&profile);
	ssid_len = strlen(profile.mesh_id);
	psk_len = 0;
	if (profile.mesh_frequency_khz == 0 || profile.mesh_bandwidth_mhz == 0) {
		LOG_WRN("HaLow start deferred: BLE mesh frequency/bandwidth not provisioned");
		return -EAGAIN;
	}

	rc = morse_mesh_ensure_booted();
	if (rc) {
		LOG_ERR("MM_MESH app morse lazy boot failed errno=%d", rc);
		return rc;
	}

	if (ssid_len == 0 || ssid_len > sizeof(sta_args.ssid)) {
		LOG_ERR("Invalid HaLow SSID length: %u", (unsigned int)ssid_len);
		return -EINVAL;
	}

	if (psk_len > MMWLAN_PASSPHRASE_MAXLEN) {
		LOG_ERR("Invalid HaLow passphrase length: %u", (unsigned int)psk_len);
		return -EINVAL;
	}

	memcpy(sta_args.ssid, profile.mesh_id, ssid_len);
	sta_args.ssid_len = ssid_len;
	memcpy(sta_args.passphrase, profile.passphrase, psk_len);
	sta_args.passphrase[psk_len] = '\0';
	sta_args.passphrase_len = psk_len;
	sta_args.security_type = MMWLAN_OPEN;
	sta_args.pmf_mode = MMWLAN_PMF_DISABLED;
	sta_args.mesh_mode = true;
	sta_args.mesh_frequency_khz = profile.mesh_frequency_khz;
	sta_args.mesh_bandwidth_mhz = profile.mesh_bandwidth_mhz;
	{
		struct edgez_halow_profile edgez_profile = {0};

		edgez_config_get_profile(&edgez_profile);
		uint32_t beacon_interval_seconds =
			(edgez_profile.device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_BEACON ||
			 edgez_profile.device_type == ai_edgez_halow_DeviceType_DEVICE_TYPE_SENSOR) ?
			1U : profile.beacon_interval_seconds;
		uint64_t beacon_interval_tus =
			((uint64_t)beacon_interval_seconds * 1000000ULL + 512ULL) / 1024ULL;

		if (beacon_interval_tus == 0U) {
			beacon_interval_tus = 100U;
		} else if (beacon_interval_tus > UINT16_MAX) {
			LOG_WRN("BLE beacon interval %u seconds exceeds 802.11 limit; clamping to %u TU",
				beacon_interval_seconds, UINT16_MAX);
			beacon_interval_tus = UINT16_MAX;
		}
		sta_args.mesh_beacon_interval_tus = (uint16_t)beacon_interval_tus;
		LOG_INF("MM_MESH app beacon interval configured=%u effective=%u seconds BSS=%u TU",
			profile.beacon_interval_seconds, beacon_interval_seconds,
			(unsigned int)sta_args.mesh_beacon_interval_tus);
	}
	sta_args.scan_interval_base_s = HALOW_MESH_CONNECT_SCAN_BASE_S;
	sta_args.scan_interval_limit_s = HALOW_MESH_CONNECT_SCAN_LIMIT_S;
	sta_args.bgscan_short_interval_s = 0;
	sta_args.bgscan_long_interval_s = 0;
	sta_args.scan_rx_cb = halow_sdk_scan_rx_cb;
	sta_args.scan_rx_cb_arg = NULL;
	sta_args.sta_evt_cb = halow_sdk_sta_event_cb;
	sta_args.sta_evt_cb_arg = NULL;
	sta_args.extra_assoc_ies = NULL;
	sta_args.extra_assoc_ies_len = 0;

	scan_config.dwell_time_ms = HALOW_MESH_SCAN_DWELL_MS;
	scan_config.home_channel_dwell_time_ms = 0;
	status = mmwlan_set_scan_config(&scan_config);
	if (status != MMWLAN_SUCCESS) {
		LOG_WRN("MM_MESH app mmwlan_set_scan_config failed=%d errno=%d", status,
			mmwlan_status_to_errno(status));
	}

	(void)mmwlan_get_vif_mac_addr(MMWLAN_VIF_STA, current_mac);
	if (edgez_config_build_vendor_ies(discovery_vendor_ie, sizeof(discovery_vendor_ie),
					 current_mac, &discovery_vendor_ie_len) != 0) {
		discovery_vendor_ie_len = 0;
		LOG_WRN("EdgeZ HaLow beacon IE could not be built");
	}
	if (discovery_vendor_ie_len > 0) {
		sta_args.extra_assoc_ies = discovery_vendor_ie;
		sta_args.extra_assoc_ies_len = discovery_vendor_ie_len;
		LOG_INF("MM_MESH app extra_assoc_ies len=%u", discovery_vendor_ie_len);
		LOG_HEXDUMP_INF(discovery_vendor_ie, discovery_vendor_ie_len, "MM_MESH extra_assoc_ies");
	} else {
		sta_args.extra_assoc_ies = NULL;
		sta_args.extra_assoc_ies_len = 0;
	}

	atomic_set(&sdk_scan_count, 0);
	atomic_set(&sdk_target_scan_count, 0);
	atomic_set(&sdk_mesh_scan_count, 0);
	atomic_set(&sdk_mesh_target_scan_count, 0);
	atomic_set(&sdk_mesh_last_ies_scan_state, -1);
	mesh_scan_last_request_ms = 0;
	atomic_set(&sdk_last_rssi, 0);
	atomic_set(&sdk_last_freq_khz, 0);
	atomic_set(&sdk_last_bw_mhz, 0);
	atomic_set(&sdk_mesh_raw_rx_count, 0);
	atomic_set(&sdk_last_raw_header_len, 0);
	atomic_set(&sdk_last_raw_payload_len, 0);
	atomic_set(&sdk_last_sta_event, MMWLAN_STA_EVT_SCAN_REQUEST);
	atomic_set(&sdk_link_state, -1);
	atomic_set(&sdk_vif_up, 0);
	sdk_last_ssid[0] = '\0';
	atomic_set(&halow_state, HALOW_CONNECTING);
#if !defined(CONFIG_WIFI_MORSE_TEST)
	wifi_connect_started_ms = k_uptime_get();
	wifi_connect_warned = false;
#endif

	LOG_INF("Connecting HaLow via Morse SDK mesh_id=\"%s\" mode=%s security=%s pmf=%d country=%s frequency=%u kHz bandwidth=%u MHz wifi_passphrase_len=%u scan_retry=%u..%us",
		profile.mesh_id,
		IS_ENABLED(CONFIG_WIFI_MORSE_MESH_MODE) ? "mesh" : "sta",
		sta_args.security_type == MMWLAN_OPEN ? "open" : "SAE",
		sta_args.pmf_mode,
		livestock_config_country(),
		(unsigned int)sta_args.mesh_frequency_khz,
		(unsigned int)sta_args.mesh_bandwidth_mhz,
		(unsigned int)sta_args.passphrase_len, sta_args.scan_interval_base_s,
		sta_args.scan_interval_limit_s);
	LOG_INF("MM_MESH app mmwlan mesh scan dwell=%ums home_dwell=%ums scan_interval_base=%us scan_limit=%us bgscan=%u/%u",
		(unsigned int)scan_config.dwell_time_ms,
		(unsigned int)scan_config.home_channel_dwell_time_ms,
		(unsigned int)sta_args.scan_interval_base_s,
		(unsigned int)sta_args.scan_interval_limit_s, (unsigned int)sta_args.bgscan_short_interval_s,
		(unsigned int)sta_args.bgscan_long_interval_s);
	meshtastic_phone_api_register_halow_rx();
	beacon_filter.cb = halow_beacon_vendor_ie_cb;
	beacon_filter.cb_arg = NULL;
	beacon_filter.n_ouis = 1;
	memcpy(beacon_filter.ouis[0],
	       (uint8_t[]){HALOW_MESH_VENDOR_OUI0, HALOW_MESH_VENDOR_OUI1, HALOW_MESH_VENDOR_OUI2},
	       sizeof(beacon_filter.ouis[0]));
	status = mmwlan_update_beacon_vendor_ie_filter(&beacon_filter);
	if (status != MMWLAN_SUCCESS) {
		LOG_WRN("MM_MESH app EdgeZ beacon vendor IE filter failed=%d errno=%d",
			status, mmwlan_status_to_errno(status));
	}
	status = mmwlan_sta_enable(&sta_args, halow_sdk_sta_status_cb);
	if (status != MMWLAN_SUCCESS) {
		rc = mmwlan_status_to_errno(status);
		LOG_ERR("MM_MESH app mmwlan_sta_enable failed=%d errno=%d", status, rc);
		return rc;
	}

	edgez_applied_generation = edgez_config_generation();
	edgez_restart_pending = false;
	edgez_config_set_halow_ready(true);
	atomic_set(&halow_state, HALOW_BOOTED);
	LOG_INF("MM_MESH app EdgeZ beacon filter enabled for prefix=%c%c%c generation=%u",
		HALOW_MESH_VENDOR_OUI0, HALOW_MESH_VENDOR_OUI1, HALOW_MESH_VENDOR_OUI2,
		edgez_applied_generation);

	return 0;
}

static void maybe_refresh_edgez_halow_beacon(void)
{
	uint32_t generation = edgez_config_generation();
	enum mmwlan_sta_state state;
	enum mmwlan_status status;
	int64_t now = k_uptime_get();

	if (!edgez_config_is_complete()) {
		return;
	}

	/* A complete runtime profile should start HaLow immediately. Provisioning
	 * commits complete user/beacon/sensor/relay profiles before this path starts
	 * the radio, so BLE teardown cannot lose the initial configuration. */
	if (edgez_applied_generation == 0) {
		if (atomic_get(&edgez_start_pending) || now < edgez_next_start_attempt_ms) {
			return;
		}
		configure_halow_power();
		if (!halow_power_ready) {
			edgez_next_start_attempt_ms = now + 5000;
			return;
		}
		LOG_INF("Complete HaLow profile generation=%u is ready; starting interface now persistent_autostart=%u",
			generation, edgez_config_should_autostart());
		edgez_next_start_attempt_ms = now + 5000;
		start_halow_connect();
		return;
	}

	/* Wait for the initial Morse start to publish the first applied generation. */
	if (generation == edgez_applied_generation) {
		return;
	}
	state = mmwlan_get_sta_state();
	if (state != MMWLAN_STA_DISABLED) {
		if (!edgez_restart_pending) {
			status = mmwlan_sta_disable();
			if (status != MMWLAN_SUCCESS) {
				LOG_WRN("EdgeZ config refresh: station disable failed=%d", status);
				return;
			}
			edgez_restart_pending = true;
			edgez_config_set_halow_ready(false);
			LOG_INF("EdgeZ config generation changed %u -> %u; restarting HaLow",
				edgez_applied_generation, generation);
		}
		return;
	}
	if (connect_morse_sdk_sta() != 0) {
		LOG_WRN("EdgeZ config refresh: HaLow restart failed");
	}
}

static int boot_morse_transceiver(void)
{
#if defined(CONFIG_WIFI_MORSE_SM)
	const struct mmwlan_regulatory_db *reg_db = get_regulatory_db();
	const struct mmwlan_s1g_channel_list *channel_list;
	enum mmwlan_status status;

	mmwlan_init();

	channel_list = mmwlan_lookup_regulatory_domain(reg_db, livestock_config_country());
	if (!channel_list) {
		LOG_ERR("Could not find regulatory domain matching country code %s", livestock_config_country());
		return -EINVAL;
	}

	status = mmwlan_set_channel_list(channel_list);
	if (status != MMWLAN_SUCCESS) {
		LOG_ERR("mmwlan_set_channel_list failed with code %d", status);
		return -(int)status;
	}

	struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
	status = mmwlan_boot(&boot_args);
	if (status != MMWLAN_SUCCESS) {
		LOG_ERR("mmwlan_boot failed with code %d", status);
		return -(int)status;
	}

	status = mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
	if (status != MMWLAN_SUCCESS) {
		LOG_ERR("Failed to disable power save mode: %d", status);
		return -(int)status;
	}

	/*
	status = mmwlan_set_wnm_sleep_enabled(false);
	if (status != MMWLAN_SUCCESS) {
		LOG_WRN("Failed to disable WNM sleep: %d", status);
	}
	*/

	return 0;
#else
	return -ENOTSUP;
#endif
}

static void halow_manual_boot_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	atomic_set(&halow_state, HALOW_CONNECTING);
	configure_halow_power();
	if (!halow_power_ready) {
		atomic_set(&halow_state, HALOW_ERROR);
		halow_manual_boot_started = false;
		publish_status_message("HaLow manual boot: power GPIO failed; boot skipped");
		return;
	}

	int rc = boot_morse_transceiver();
	if (rc < 0) {
		wifi_last_error = rc;
		atomic_set(&halow_state, HALOW_ERROR);
		halow_manual_boot_started = false;
		LOG_ERR("HaLow manual boot failed: %d", rc);
		return;
	}

	wifi_last_error = 0;
	atomic_set(&halow_state, HALOW_BOOTED);
	publish_status_message("HaLow manual boot complete");

	rc = connect_morse_sdk_sta();
	if (rc < 0) {
		wifi_last_error = rc;
		atomic_set(&halow_state, HALOW_ERROR);
		LOG_ERR("HaLow SDK connect failed: %d", rc);
	}
}

static void start_halow_manual_boot(void)
{
	if (halow_manual_boot_started) {
		LOG_WRN("HaLow manual boot already requested");
		return;
	}

	halow_manual_boot_started = true;
	k_thread_create(&halow_manual_boot_thread_data, halow_manual_boot_stack,
			K_THREAD_STACK_SIZEOF(halow_manual_boot_stack),
			halow_manual_boot_thread, NULL, NULL, NULL,
			HALOW_MANUAL_BOOT_THREAD_PRIORITY, 0, K_NO_WAIT);
}
#endif

int main(void)
{
	printk("edge-device-nrf54 boot\n");
#if defined(CONFIG_WIFI_LOG_LEVEL)
	LOG_INF("Morse Zephyr driver logs enabled at CONFIG_WIFI_LOG_LEVEL=%d", CONFIG_WIFI_LOG_LEVEL);
#else
	LOG_INF("Wi-Fi driver logs disabled");
#endif

	configure_led(&green_led, &green_led_ready, "green LED");
	if (HAS_RED_LED) {
		configure_led(&red_led, &red_led_ready, "red LED");
	}
	/* Give an immediate hardware indication before BLE and HaLow startup. The
	 * normal status policy takes over when the main loop starts. */
	set_status_leds(false, true);
	if (green_led_ready) {
		LOG_INF("Status LED boot indication on GPIO %s pin %u flags=0x%x",
			green_led.port->name, green_led.pin, green_led.dt_flags);
	}

	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("KEY device is not ready");
	} else {
		int rc = gpio_pin_configure_dt(&button, GPIO_INPUT);
		if (rc < 0) {
			LOG_ERR("Failed to configure KEY pin: %d", rc);
		} else {
			button_ready = true;
		}
	}

	edgez_config_init(HALOW_WIFI_SSID, HALOW_WIFI_PSK);
	livestock_config_init();
	if (livestock_config_is_provisioned()) {
		LOG_INF("BLE provisioning disabled after setup; press KEY to enable it");
	} else {
		int ble_rc = meshtastic_ble_start();
		if (ble_rc) {
			LOG_ERR("EdgeZ BLE provisioning failed to start: %d", ble_rc);
		} else {
			publish_status_message("EdgeZ BLE provisioning ready");
		}
	}

#if defined(CONFIG_WIFI_MORSE_TEST)
	publish_status_message("HaLow test mode ready; press KEY to boot Morse");
#elif defined(CONFIG_WIFI)
	atomic_set(&app_connect_stage, 10);
	LOG_INF("Delaying HaLow start for %u ms so BLE can finish startup",
		(unsigned int)HALOW_START_DELAY_MS);
	k_msleep(HALOW_START_DELAY_MS);
	atomic_set(&app_connect_stage, 11);
	if (edgez_config_should_autostart()) {
		struct edgez_halow_profile boot_profile = {0};

		edgez_config_get_profile(&boot_profile);
		LOG_INF("Boot autostart accepted device_type=%u mesh_id=\"%s\" frequency=%u kHz bandwidth=%u MHz",
			boot_profile.device_type, boot_profile.mesh_id,
			boot_profile.mesh_frequency_khz, boot_profile.mesh_bandwidth_mhz);
		configure_halow_power();
		start_halow_connect();
	} else {
		struct edgez_halow_profile boot_profile = {0};

		edgez_config_get_profile(&boot_profile);
		atomic_set(&halow_state, HALOW_IDLE);
		LOG_WRN("HaLow autostart rejected device_type=%u mesh_id_len=%u frequency=%u kHz bandwidth=%u MHz; remains off until a complete beacon/sensor/relay profile is saved",
			boot_profile.device_type, (unsigned int)strlen(boot_profile.mesh_id),
			boot_profile.mesh_frequency_khz, boot_profile.mesh_bandwidth_mhz);
	}
#else
	publish_status_message("Raw nRF54 build ready; Wi-Fi disabled");
#endif
	publish_heartbeat();

	while (1) {
		for (int i = 0; i < HEARTBEAT_PERIOD_SECONDS * 50; i++) {
			poll_button();
#if !defined(CONFIG_WIFI_MORSE_TEST)
#if defined(CONFIG_WIFI)
			check_connect_timeout();
#if defined(CONFIG_WIFI_MORSE_SM)
			maybe_refresh_edgez_halow_beacon();
			maybe_init_imu_after_halow_start();
			maybe_init_gps_after_halow_start();
			edgez_gps_poll();
			maybe_refresh_edgez_sensor_beacon();
#endif
#endif
#endif
			update_status_leds();
			k_msleep(20);
		}

		publish_heartbeat();
	}

	return 0;
}
