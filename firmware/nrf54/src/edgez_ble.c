#include "edgez_ble.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "edgez_config.h"
#include "livestocking_config.h"

LOG_MODULE_REGISTER(edgez_ble, LOG_LEVEL_INF);

#define EDGEZ_FRAME_HEADER_LEN 4
#define EDGEZ_FRAME_MAX_PAYLOAD 512
#define EDGEZ_FRAME_MAX_LEN (EDGEZ_FRAME_HEADER_LEN + EDGEZ_FRAME_MAX_PAYLOAD)
#define EDGEZ_BLE_NOTIFY_RETRY_DELAY K_MSEC(10)
#define EDGEZ_BLE_NOTIFY_MAX_RETRIES 10
#define MQTT_CONFIG_MAX_LEN 1024
#define MQTT_CONFIRMATION_ID_MAX_LEN 64
#define PROVISIONING_NAME_PREFIX "PROV_"

/* Direct nRF provisioning: service, mqtt-config write, result read. */
#define LIVESTOCK_SERVICE_UUID BT_UUID_128_ENCODE(0xa3631000, 0xb82e, 0x44c2, 0x9b1d, 0xa790675b4ac1)
#define LIVESTOCK_CONFIG_UUID BT_UUID_128_ENCODE(0xa3631001, 0xb82e, 0x44c2, 0x9b1d, 0xa790675b4ac1)
#define LIVESTOCK_STATUS_UUID BT_UUID_128_ENCODE(0xa3631002, 0xb82e, 0x44c2, 0x9b1d, 0xa790675b4ac1)

static struct bt_uuid_128 livestock_service_uuid = BT_UUID_INIT_128(LIVESTOCK_SERVICE_UUID);
static struct bt_uuid_128 livestock_config_uuid = BT_UUID_INIT_128(LIVESTOCK_CONFIG_UUID);
static struct bt_uuid_128 livestock_status_uuid = BT_UUID_INIT_128(LIVESTOCK_STATUS_UUID);
extern const struct bt_gatt_service_static livestock_svc;
static char mqtt_config_buffer[MQTT_CONFIG_MAX_LEN + 1];
static size_t mqtt_config_expected;
static size_t mqtt_config_received;
static char mqtt_config_status[192] = "{\"pending\":true}";

static struct bt_uuid_16 edgez_service_uuid = BT_UUID_INIT_16(0xfff0);
static struct bt_uuid_16 edgez_rx_uuid = BT_UUID_INIT_16(0xfff1);
static struct bt_uuid_16 edgez_tx_uuid = BT_UUID_INIT_16(0xfff2);
extern const struct bt_gatt_service_static edgez_svc;
static struct bt_conn *current_conn;
static uint8_t rx_buffer[EDGEZ_FRAME_MAX_LEN * 2];
static size_t rx_length;
static uint8_t tx_frame[EDGEZ_FRAME_MAX_LEN];
static uint8_t pending_response[EDGEZ_FRAME_MAX_PAYLOAD];
static size_t pending_response_len;
static uint8_t pending_response_retries;
static uint8_t battery_level = 100;
static bool bt_ready;
static bool advertising;
static bool provisioning_enabled;
static bool tx_notify_enabled;
static char advertised_name[20];

static const char *conn_addr_str(const struct bt_conn *conn, char *buf, size_t len)
{
	const bt_addr_le_t *addr = bt_conn_get_dst(conn);

	if (!addr || bt_addr_le_to_str(addr, buf, len) < 0) {
		strncpy(buf, "unknown", len);
		buf[len - 1] = '\0';
	}
	return buf;
}

static void start_advertising(void);
static const char *get_advertised_name(void);
static void restart_advertising_work_handler(struct k_work *work);
static void response_notify_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(restart_advertising_work, restart_advertising_work_handler);
K_WORK_DELAYABLE_DEFINE(response_notify_work, response_notify_work_handler);

static int send_response(const uint8_t *payload, size_t payload_len)
{
	if (!current_conn) {
		LOG_WRN("BLE TX blocked: no connected client payload_len=%u",
			(unsigned int)payload_len);
		return -ENOTCONN;
	}
	if (!tx_notify_enabled) {
		LOG_WRN("BLE TX blocked: FFF2 notifications not enabled payload_len=%u",
			(unsigned int)payload_len);
		return -ENOTCONN;
	}
	if (payload_len > EDGEZ_FRAME_MAX_PAYLOAD) {
		LOG_ERR("BLE TX blocked: payload_len=%u exceeds max=%u",
			(unsigned int)payload_len, EDGEZ_FRAME_MAX_PAYLOAD);
		return -ENOTCONN;
	}
	tx_frame[0] = 'E';
	tx_frame[1] = 'Z';
	sys_put_le16((uint16_t)payload_len, &tx_frame[2]);
	memcpy(&tx_frame[EDGEZ_FRAME_HEADER_LEN], payload, payload_len);
	return bt_gatt_notify_uuid(current_conn, &edgez_tx_uuid.uuid, edgez_svc.attrs,
				   tx_frame, payload_len + EDGEZ_FRAME_HEADER_LEN);
}

static int queue_response(const uint8_t *payload, size_t payload_len)
{
	if (!payload || payload_len == 0 || payload_len > sizeof(pending_response)) {
		return -EINVAL;
	}
	if (pending_response_len != 0) {
		return -EBUSY;
	}

	memcpy(pending_response, payload, payload_len);
	pending_response_len = payload_len;
	pending_response_retries = 0;
	LOG_INF("BLE response queued payload_len=%u frame_len=%u",
		(unsigned int)payload_len,
		(unsigned int)(payload_len + EDGEZ_FRAME_HEADER_LEN));
	(void)k_work_reschedule(&response_notify_work, EDGEZ_BLE_NOTIFY_RETRY_DELAY);
	return 0;
}

static void response_notify_work_handler(struct k_work *work)
{
	int rc;

	ARG_UNUSED(work);
	if (pending_response_len == 0) {
		return;
	}

	rc = send_response(pending_response, pending_response_len);
	if ((rc == -ENOMEM || rc == -EAGAIN) &&
	    pending_response_retries++ < EDGEZ_BLE_NOTIFY_MAX_RETRIES) {
		LOG_WRN("BLE response notify busy rc=%d retry=%u/%u", rc,
			pending_response_retries, EDGEZ_BLE_NOTIFY_MAX_RETRIES);
		(void)k_work_reschedule(&response_notify_work, EDGEZ_BLE_NOTIFY_RETRY_DELAY);
		return;
	}
	if (rc) {
		LOG_WRN("EdgeZ BLE response notify failed: %d", rc);
	} else {
		LOG_INF("EdgeZ BLE response notified len=%u",
			(unsigned int)pending_response_len);
	}
	pending_response_len = 0;
}

static void process_rx_frames(void)
{
	while (rx_length >= EDGEZ_FRAME_HEADER_LEN) {
		uint16_t payload_len;
		size_t frame_len;
		uint8_t response[EDGEZ_FRAME_MAX_PAYLOAD];
		size_t response_len = 0;
		int rc;

		if (rx_buffer[0] != 'E' || rx_buffer[1] != 'Z') {
			LOG_WRN("Invalid EdgeZ BLE frame magic=%02x%02x; dropping %u byte(s)",
				rx_buffer[0], rx_buffer[1], (unsigned int)rx_length);
			rx_length = 0;
			return;
		}
		payload_len = sys_get_le16(&rx_buffer[2]);
		if (payload_len > EDGEZ_FRAME_MAX_PAYLOAD) {
			LOG_WRN("Invalid EdgeZ BLE payload length %u", payload_len);
			rx_length = 0;
			return;
		}
		frame_len = EDGEZ_FRAME_HEADER_LEN + payload_len;
		if (rx_length < frame_len) {
			LOG_INF("BLE frame awaiting fragments buffered=%u expected=%u payload=%u",
				(unsigned int)rx_length, (unsigned int)frame_len, payload_len);
			return;
		}
		LOG_INF("BLE frame complete payload=%u buffered=%u trailing=%u",
			payload_len, (unsigned int)rx_length,
			(unsigned int)(rx_length - frame_len));
		rc = edgez_config_handle_packet(&rx_buffer[EDGEZ_FRAME_HEADER_LEN], payload_len,
					response, sizeof(response), &response_len);
		if (rc == 0 && response_len > 0) {
			rc = queue_response(response, response_len);
			if (rc) {
				LOG_WRN("EdgeZ BLE response queue failed: %d", rc);
			}
		} else if (rc) {
			LOG_WRN("EdgeZ BLE request rejected: %d", rc);
		}
		rx_length -= frame_len;
		if (rx_length) {
			memmove(rx_buffer, &rx_buffer[frame_len], rx_length);
		}
	}
}

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	LOG_INF("EdgeZ BLE control write len=%u offset=%u flags=0x%x security=%u",
		len, offset, flags, bt_conn_get_security(conn));
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len > sizeof(rx_buffer) - rx_length) {
		LOG_ERR("BLE RX overflow write=%u buffered=%u capacity=%u; parser reset",
			len, (unsigned int)rx_length, (unsigned int)sizeof(rx_buffer));
		rx_length = 0;
		return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
	}
	memcpy(&rx_buffer[rx_length], buf, len);
	rx_length += len;
	LOG_INF("BLE RX buffered=%u after write", (unsigned int)rx_length);
	process_rx_frames();
	return len;
}

static void tx_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	tx_notify_enabled = value == BT_GATT_CCC_NOTIFY;
	LOG_INF("EdgeZ BLE notifications %s", tx_notify_enabled ? "enabled" : "disabled");
}

static ssize_t read_battery(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &battery_level,
				 sizeof(battery_level));
}

static ssize_t write_mqtt_config(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *data = buf;
	char confirmation_id[MQTT_CONFIRMATION_ID_MAX_LEN] = {0};
	size_t header;
	int rc;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset || len < 2) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (data[0] == 1 && len >= 4) {
		mqtt_config_expected = sys_get_le16(&data[1]);
		mqtt_config_received = 0;
		strcpy(mqtt_config_status, "{\"pending\":true}");
		header = 3;
		if (!mqtt_config_expected || mqtt_config_expected > MQTT_CONFIG_MAX_LEN) {
			strcpy(mqtt_config_status, "{\"ok\":false,\"error\":\"Configuration too large\"}");
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
	} else if (data[0] == 2 && mqtt_config_expected) {
		header = 1;
	} else {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (mqtt_config_received + len - header > mqtt_config_expected) {
		strcpy(mqtt_config_status, "{\"ok\":false,\"error\":\"Invalid configuration length\"}");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	memcpy(&mqtt_config_buffer[mqtt_config_received], data + header, len - header);
	mqtt_config_received += len - header;
	if (mqtt_config_received == mqtt_config_expected) {
		mqtt_config_buffer[mqtt_config_received] = '\0';
		rc = livestock_config_apply_json(mqtt_config_buffer, mqtt_config_received,
					      get_advertised_name() + sizeof(PROVISIONING_NAME_PREFIX) - 1,
					      confirmation_id, sizeof(confirmation_id));
		if (rc == 0) {
			snprintk(mqtt_config_status, sizeof(mqtt_config_status),
				 "{\"ok\":true,\"persisted\":true,\"confirmationId\":\"%s\"}",
				 confirmation_id);
		} else {
			snprintk(mqtt_config_status, sizeof(mqtt_config_status),
				 "{\"ok\":false,\"persisted\":false,\"confirmationId\":\"%s\",\"error\":\"Invalid configuration or NVS verification failed\"}",
				 confirmation_id);
		}
		if (rc == 0) provisioning_enabled = false;
		LOG_INF("Live Stocking mqtt-config received bytes=%u result=%d",
			(unsigned int)mqtt_config_received, rc);
		mqtt_config_expected = 0;
		mqtt_config_received = 0;
		if (current_conn) {
			int notify_rc = bt_gatt_notify_uuid(current_conn, &livestock_status_uuid.uuid,
						     livestock_svc.attrs, mqtt_config_status,
						     strlen(mqtt_config_status));
			if (notify_rc) LOG_WRN("Provisioning confirmation notify failed: %d", notify_rc);
		}
	}
	return len;
}

static ssize_t read_mqtt_config_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				       void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, mqtt_config_status,
				 strlen(mqtt_config_status));
}

BT_GATT_SERVICE_DEFINE(livestock_svc,
	BT_GATT_PRIMARY_SERVICE(&livestock_service_uuid),
	BT_GATT_CHARACTERISTIC(&livestock_config_uuid.uuid, BT_GATT_CHRC_WRITE,
		BT_GATT_PERM_WRITE, NULL, write_mqtt_config, NULL),
	BT_GATT_CUD("mqtt-config", BT_GATT_PERM_READ),
	BT_GATT_CHARACTERISTIC(&livestock_status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ, read_mqtt_config_status, NULL, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

BT_GATT_SERVICE_DEFINE(edgez_svc,
	BT_GATT_PRIMARY_SERVICE(&edgez_service_uuid),
	BT_GATT_CHARACTERISTIC(&edgez_rx_uuid.uuid,
		BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
		BT_GATT_PERM_WRITE,
		NULL, write_control, NULL),
	BT_GATT_CHARACTERISTIC(&edgez_tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(tx_ccc_changed,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

BT_GATT_SERVICE_DEFINE(battery_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_BAS),
	BT_GATT_CHARACTERISTIC(BT_UUID_BAS_BATTERY_LEVEL,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ, read_battery, NULL, &battery_level),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0xf0, 0xff),
};

static const char *get_advertised_name(void)
{
	bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
	size_t count = ARRAY_SIZE(addrs);
	bt_id_get(addrs, &count);
	if (count) {
		snprintk(advertised_name, sizeof(advertised_name), PROVISIONING_NAME_PREFIX "%02X%02X%02X%02X%02X%02X",
			 addrs[0].a.val[5], addrs[0].a.val[4], addrs[0].a.val[3],
			 addrs[0].a.val[2], addrs[0].a.val[1], addrs[0].a.val[0]);
	} else {
		strcpy(advertised_name, PROVISIONING_NAME_PREFIX "000000000000");
	}
	return advertised_name;
}

static void start_advertising(void)
{
	const char *name;
	struct bt_data sd[1];
	int err;
	if (!bt_ready || !provisioning_enabled || advertising || current_conn) {
		LOG_DBG("BLE advertising skipped ready=%u advertising=%u connected=%u",
			bt_ready, advertising, current_conn != NULL);
		return;
	}
	name = get_advertised_name();
	err = bt_set_name(name);
	if (err) {
		LOG_WRN("BLE dynamic name set failed name=%s err=%d", name, err);
	}
	sd[0] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name));
	LOG_INF("BLE advertising start requested name=%s service=FFF0 connectable=1",
		name);
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("EdgeZ BLE advertising failed: %d", err);
		return;
	}
	advertising = true;
	LOG_INF("BLE provisioning advertising as %s service=FFF0", name);
}

static void restart_advertising_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	start_advertising();
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("EdgeZ BLE connection failed peer=%s hci_err=0x%02x (%s)",
			conn_addr_str(conn, addr, sizeof(addr)), err, bt_hci_err_to_str(err));
		advertising = false;
		(void)k_work_reschedule(&restart_advertising_work, K_MSEC(500));
		return;
	}
	current_conn = bt_conn_ref(conn);
	advertising = false;
	rx_length = 0;
	mqtt_config_expected = 0;
	mqtt_config_received = 0;
	strcpy(mqtt_config_status, "{\"pending\":true}");
	pending_response_len = 0;
	tx_notify_enabled = false;
	LOG_INF("EdgeZ BLE provisioning client connected peer=%s; no pairing required",
		conn_addr_str(conn, addr, sizeof(addr)));
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	LOG_INF("EdgeZ BLE disconnected peer=%s reason=0x%02x (%s) buffered=%u notify=%u",
		conn_addr_str(conn, addr, sizeof(addr)), reason, bt_hci_err_to_str(reason),
		(unsigned int)rx_length, tx_notify_enabled);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	rx_length = 0;
	mqtt_config_expected = 0;
	mqtt_config_received = 0;
	pending_response_len = 0;
	(void)k_work_cancel_delayable(&response_notify_work);
	tx_notify_enabled = false;
	advertising = false;
	(void)k_work_reschedule(&restart_advertising_work, K_MSEC(500));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static void bt_ready_cb(int err)
{
	int settings_rc;

	if (err) {
		LOG_ERR("Bluetooth init failed: %d", err);
		return;
	}
	bt_ready = true;
	settings_rc = settings_load_subtree("bt");
	LOG_INF("Bluetooth initialized settings_load_rc=%d service=FFF0 rx=FFF1 tx=FFF2",
		settings_rc);
	start_advertising();
}

bool edgez_ble_is_connected(void)
{
	return current_conn != NULL;
}

bool edgez_ble_is_enabled(void)
{
	return bt_ready && provisioning_enabled;
}

void edgez_ble_update_battery(uint8_t level)
{
	battery_level = MIN(level, 100);
	if (bt_ready) {
		(void)bt_gatt_notify(NULL, &battery_svc.attrs[2], &battery_level,
				     sizeof(battery_level));
	}
}

int edgez_ble_start(void)
{
	int err;

	LOG_INF("BLE provisioning startup begin ready=%u service=FFF0 rx=FFF1 tx=FFF2 max_payload=%u",
		bt_ready, EDGEZ_FRAME_MAX_PAYLOAD);
	provisioning_enabled = true;
	if (bt_ready) {
		start_advertising();
		return 0;
	}
	err = bt_enable(bt_ready_cb);
	if (err && err != -EALREADY) {
		LOG_ERR("BLE stack enable request failed: %d", err);
		return err;
	}
	LOG_INF("BLE stack enable request accepted rc=%d callback_pending=%u", err,
		err == 0);
	if (err == -EALREADY) {
		bt_ready = true;
		start_advertising();
	}
	return 0;
}
