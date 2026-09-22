#include "meshtastic_ble.h"

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
#include "meshtastic_phone_api.h"

LOG_MODULE_REGISTER(edgez_ble, LOG_LEVEL_INF);

#define EDGEZ_BLE_PASSKEY 123456
#define EDGEZ_FRAME_HEADER_LEN 4
#define EDGEZ_FRAME_MAX_PAYLOAD 512
#define EDGEZ_FRAME_MAX_LEN (EDGEZ_FRAME_HEADER_LEN + EDGEZ_FRAME_MAX_PAYLOAD)
#define EDGEZ_BLE_NOTIFY_RETRY_DELAY K_MSEC(10)
#define EDGEZ_BLE_NOTIFY_MAX_RETRIES 10

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

BT_GATT_SERVICE_DEFINE(edgez_svc,
	BT_GATT_PRIMARY_SERVICE(&edgez_service_uuid),
	BT_GATT_CHARACTERISTIC(&edgez_rx_uuid.uuid,
		BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
		BT_GATT_PERM_WRITE_ENCRYPT,
		NULL, write_control, NULL),
	BT_GATT_CHARACTERISTIC(&edgez_tx_uuid.uuid, BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(tx_ccc_changed,
		BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

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
		snprintk(advertised_name, sizeof(advertised_name), "EdgeZ-%02X%02X",
			 addrs[0].a.val[1], addrs[0].a.val[0]);
	} else {
		strcpy(advertised_name, "EdgeZ-0000");
	}
	return advertised_name;
}

static void start_advertising(void)
{
	const char *name;
	struct bt_data sd[1];
	int err;
	if (!bt_ready || advertising || current_conn) {
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
	pending_response_len = 0;
	tx_notify_enabled = false;
	LOG_INF("EdgeZ BLE provisioning client connected peer=%s security=%u; waiting for pairing and FFF2 CCC",
		conn_addr_str(conn, addr, sizeof(addr)), bt_conn_get_security(conn));
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
	pending_response_len = 0;
	(void)k_work_cancel_delayable(&response_notify_work);
	tx_notify_enabled = false;
	advertising = false;
	(void)k_work_reschedule(&restart_advertising_work, K_MSEC(500));
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_WRN("EdgeZ BLE security failed peer=%s level=%u err=%u (%s)",
			conn_addr_str(conn, addr, sizeof(addr)), level, err,
			bt_security_err_to_str(err));
	} else {
		LOG_INF("EdgeZ BLE secured peer=%s level=%u",
			conn_addr_str(conn, addr, sizeof(addr)), level);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
};

static void passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	char addr[BT_ADDR_LE_STR_LEN];

	LOG_INF("EdgeZ BLE pairing passkey peer=%s passkey=%06u",
		conn_addr_str(conn, addr, sizeof(addr)), passkey);
}

static void pairing_confirm(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err = bt_conn_auth_pairing_confirm(conn);

	LOG_INF("EdgeZ BLE pairing confirmation peer=%s rc=%d",
		conn_addr_str(conn, addr, sizeof(addr)), err);
}

static void auth_cancel(struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	LOG_WRN("EdgeZ BLE pairing interaction canceled peer=%s",
		conn_addr_str(conn, addr, sizeof(addr)));
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	LOG_INF("EdgeZ BLE pairing complete peer=%s bonded=%u security=%u",
		conn_addr_str(conn, addr, sizeof(addr)), bonded, bt_conn_get_security(conn));
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	LOG_ERR("EdgeZ BLE pairing failed peer=%s reason=%u (%s)",
		conn_addr_str(conn, addr, sizeof(addr)), reason,
		bt_security_err_to_str(reason));
}

static void bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (bt_addr_le_to_str(peer, addr, sizeof(addr)) < 0) {
		strcpy(addr, "unknown");
	}
	LOG_WRN("EdgeZ BLE bond deleted identity=%u peer=%s", id, addr);
}

static struct bt_conn_auth_cb auth_callbacks = {
	.passkey_display = passkey_display,
	.pairing_confirm = pairing_confirm,
	.cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb auth_info_callbacks = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
	.bond_deleted = bond_deleted,
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
	LOG_INF("Bluetooth initialized settings_load_rc=%d service=FFF0 rx=FFF1 tx=FFF2 passkey=%06u",
		settings_rc, EDGEZ_BLE_PASSKEY);
	start_advertising();
}

bool meshtastic_ble_is_connected(void)
{
	return current_conn != NULL;
}

bool meshtastic_ble_is_enabled(void)
{
	return bt_ready;
}

void meshtastic_ble_update_battery(uint8_t level)
{
	battery_level = MIN(level, 100);
	if (bt_ready) {
		(void)bt_gatt_notify(NULL, &battery_svc.attrs[2], &battery_level,
				     sizeof(battery_level));
	}
}

int meshtastic_ble_start(void)
{
	int err;
	int passkey_err;
	int auth_info_err;

	LOG_INF("BLE provisioning startup begin ready=%u service=FFF0 rx=FFF1 tx=FFF2 max_payload=%u",
		bt_ready, EDGEZ_FRAME_MAX_PAYLOAD);
	if (bt_ready) {
		start_advertising();
		return 0;
	}
	/* Keep the existing raw HaLow packet API initialized; BLE no longer exposes it. */
	meshtastic_phone_api_init(0);
	err = bt_conn_auth_cb_register(&auth_callbacks);
	if (err && err != -EALREADY) {
		LOG_ERR("BLE auth callback registration failed: %d; pairing cannot work", err);
	} else {
		LOG_INF("BLE auth callbacks registered rc=%d", err);
	}
	auth_info_err = bt_conn_auth_info_cb_register(&auth_info_callbacks);
	if (auth_info_err && auth_info_err != -EALREADY) {
		LOG_WRN("BLE auth-info callback registration failed: %d", auth_info_err);
	}
	passkey_err = bt_passkey_set(EDGEZ_BLE_PASSKEY);
	if (passkey_err) {
		LOG_ERR("BLE fixed passkey setup failed: %d", passkey_err);
	} else {
		LOG_INF("BLE fixed passkey configured value=%06u", EDGEZ_BLE_PASSKEY);
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
