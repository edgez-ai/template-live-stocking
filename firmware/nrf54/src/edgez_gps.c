#include "edgez_gps.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(edgez_gps, LOG_LEVEL_INF);

#define GPS_NODE DT_ALIAS(gps0)
#define GPS_RX_GPIO_NODE DT_NODELABEL(gpio1)
#define GPS_RX_PIN 12U
#define GPS_LINE_MAX 128U
#define GPS_FIX_PUBLISH_INTERVAL_MS 60000
#define GPS_FIX_MAX_AGE_MS 75000
#define GPS_DIAG_INTERVAL_MS 10000
#define GPS_VALUE_LOG_INTERVAL_MS 60000
#define GPS_DEBUG_CAPTURE_MAX 64U
#define GPS_ASYNC_BUFFER_SIZE 128U
#define GPS_RING_BUFFER_SIZE 512U
#define GPS_RX_TIMEOUT_US 10000

#if DT_NODE_HAS_STATUS(GPS_NODE, okay)
#define EDGEZ_GPS_AVAILABLE 1
#else
#define EDGEZ_GPS_AVAILABLE 0
#endif

#if EDGEZ_GPS_AVAILABLE
static const struct device *const gps_uart = DEVICE_DT_GET(GPS_NODE);
static const struct device *const gps_rx_gpio = DEVICE_DT_GET(GPS_RX_GPIO_NODE);
static char line_buf[GPS_LINE_MAX];
static uint8_t debug_capture[GPS_DEBUG_CAPTURE_MAX];
static uint8_t async_rx_buffer[2][GPS_ASYNC_BUFFER_SIZE];
RING_BUF_DECLARE(gps_rx_ring, GPS_RING_BUFFER_SIZE);
static size_t line_len;
static size_t debug_capture_len;
static bool gps_ready;
static bool fix_reported;
static bool nmea_reported;
static bool gps_warning_reported;
static bool debug_capture_reported;
static uint32_t received_bytes;
static uint32_t uart_error_flags;
static uint32_t ring_dropped_bytes;
static uint32_t rx_restart_count;
static uint8_t async_rx_buffer_idx;
static int64_t gps_init_ms;
static int64_t gps_last_diag_ms;
static int64_t gps_last_value_log_ms;
static int64_t gps_last_fix_publish_ms;
static struct edgez_gps_fix latest_fix;
static struct k_spinlock fix_lock;

static void gps_uart_callback(const struct device *dev, struct uart_event *event,
			      void *user_data)
{
	size_t written;
	int rc;

	ARG_UNUSED(user_data);
	switch (event->type) {
	case UART_RX_RDY:
		written = ring_buf_put(&gps_rx_ring,
				       event->data.rx.buf + event->data.rx.offset,
				       event->data.rx.len);
		if (written < event->data.rx.len) {
			ring_dropped_bytes += (uint32_t)(event->data.rx.len - written);
		}
		break;
	case UART_RX_BUF_REQUEST:
		rc = uart_rx_buf_rsp(dev, async_rx_buffer[async_rx_buffer_idx],
				     sizeof(async_rx_buffer[0]));
		if (rc == 0) {
			async_rx_buffer_idx = async_rx_buffer_idx ? 0U : 1U;
		} else {
			uart_error_flags |= BIT(31);
		}
		break;
	case UART_RX_STOPPED:
		uart_error_flags |= (uint32_t)event->data.rx_stop.reason;
		break;
	case UART_RX_DISABLED:
		if (gps_ready) {
			async_rx_buffer_idx = 1U;
			rc = uart_rx_enable(dev, async_rx_buffer[0],
					    sizeof(async_rx_buffer[0]), GPS_RX_TIMEOUT_US);
			if (rc == 0) {
				rx_restart_count++;
			} else {
				uart_error_flags |= BIT(30);
			}
		}
		break;
	case UART_RX_BUF_RELEASED:
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		break;
	}
}

static int hex_value(char value)
{
	if (value >= '0' && value <= '9') {
		return value - '0';
	}
	if (value >= 'A' && value <= 'F') {
		return value - 'A' + 10;
	}
	if (value >= 'a' && value <= 'f') {
		return value - 'a' + 10;
	}
	return -EINVAL;
}

static bool valid_checksum(char *line)
{
	char *separator;
	uint8_t checksum = 0;
	int high;
	int low;

	if (line[0] != '$') {
		return false;
	}
	separator = strchr(line, '*');
	if (!separator || separator[1] == '\0' || separator[2] == '\0') {
		return false;
	}
	for (char *cursor = &line[1]; cursor < separator; cursor++) {
		checksum ^= (uint8_t)*cursor;
	}
	high = hex_value(separator[1]);
	low = hex_value(separator[2]);
	if (high < 0 || low < 0 || checksum != (uint8_t)((high << 4) | low)) {
		return false;
	}
	*separator = '\0';
	return true;
}

static bool parse_coordinate(const char *value, const char *hemisphere,
			     float *coordinate)
{
	char *end;
	float raw;
	int degrees;
	float minutes;

	if (!value || !value[0] || !hemisphere || !hemisphere[0]) {
		return false;
	}
	raw = strtof(value, &end);
	if (end == value || *end != '\0' || raw < 0.0f) {
		return false;
	}
	degrees = (int)(raw / 100.0f);
	minutes = raw - (float)(degrees * 100);
	if (minutes < 0.0f || minutes >= 60.0f) {
		return false;
	}
	*coordinate = (float)degrees + minutes / 60.0f;
	if (hemisphere[0] == 'S' || hemisphere[0] == 'W') {
		*coordinate = -*coordinate;
	} else if (hemisphere[0] != 'N' && hemisphere[0] != 'E') {
		return false;
	}
	return true;
}

static bool sentence_type_is(const char *type, const char *suffix)
{
	size_t len = strlen(type);

	return len >= 3U && strcmp(&type[len - 3U], suffix) == 0;
}

static bool gps_value_log_due(void)
{
	int64_t now = k_uptime_get();

	if (gps_last_value_log_ms == 0 ||
	    now - gps_last_value_log_ms >= GPS_VALUE_LOG_INTERVAL_MS) {
		gps_last_value_log_ms = now;
		return true;
	}
	return false;
}

static void publish_fix(float latitude, float longitude, uint8_t satellites)
{
	k_spinlock_key_t key;
	bool first_fix;
	int64_t now;

	if (latitude < -90.0f || latitude > 90.0f ||
	    longitude < -180.0f || longitude > 180.0f) {
		return;
	}
	now = k_uptime_get();
	first_fix = !fix_reported;
	if (!first_fix &&
	    now - gps_last_fix_publish_ms < GPS_FIX_PUBLISH_INTERVAL_MS) {
		return;
	}
	key = k_spin_lock(&fix_lock);
	latest_fix.latitude = latitude;
	latest_fix.longitude = longitude;
	latest_fix.satellites = satellites;
	latest_fix.timestamp_ms = now;
	k_spin_unlock(&fix_lock, key);

	fix_reported = true;
	gps_last_fix_publish_ms = now;
	gps_last_value_log_ms = now;
	LOG_INF("L76K position latitude=%.6f longitude=%.6f satellites=%u%s",
		(double)latitude, (double)longitude, satellites,
		first_fix ? " first_fix" : "");
}

static void parse_sentence(char *line)
{
	char *fields[20];
	size_t count = 0;
	char *cursor;
	float latitude;
	float longitude;
	uint8_t satellites = 0;

	if (!valid_checksum(line)) {
		return;
	}
	/* Preserve empty NMEA fields so the standard field indices do not shift. */
	cursor = &line[1];
	fields[count++] = cursor;
	while (*cursor != '\0' && count < ARRAY_SIZE(fields)) {
		if (*cursor == ',') {
			*cursor = '\0';
			fields[count++] = cursor + 1;
		}
		cursor++;
	}
	if (count == 0U) {
		return;
	}
	if (!nmea_reported) {
		nmea_reported = true;
		LOG_INF("L76K checksum-valid NMEA stream detected sentence=%s", fields[0]);
	}

	if (sentence_type_is(fields[0], "RMC")) {
		if (count < 7U) {
			return;
		}
		if (fields[2][0] != 'A') {
			if (gps_value_log_due()) {
				LOG_INF("L76K value RMC status=%c latitude_raw=\"%s%s\" longitude_raw=\"%s%s\"",
					fields[2][0] ? fields[2][0] : '-',
					fields[3], fields[4], fields[5], fields[6]);
			}
			return;
		}
		if (!parse_coordinate(fields[3], fields[4], &latitude) ||
		    !parse_coordinate(fields[5], fields[6], &longitude)) {
			return;
		}
	} else if (sentence_type_is(fields[0], "GGA")) {
		long fix_quality;
		long satellite_count;

		if (count < 8U) {
			return;
		}
		fix_quality = strtol(fields[6], NULL, 10);
		satellite_count = strtol(fields[7], NULL, 10);
		if (fix_quality <= 0) {
			if (gps_value_log_due()) {
				LOG_INF("L76K value GGA fix_quality=%ld satellites=%ld latitude_raw=\"%s%s\" longitude_raw=\"%s%s\"",
					fix_quality, satellite_count, fields[2], fields[3],
					fields[4], fields[5]);
			}
			return;
		}
		if (!parse_coordinate(fields[2], fields[3], &latitude) ||
		    !parse_coordinate(fields[4], fields[5], &longitude)) {
			return;
		}
		if (satellite_count > 0) {
			satellites = (uint8_t)MIN(satellite_count, UINT8_MAX);
		}
	} else if (sentence_type_is(fields[0], "GLL")) {
		if (count < 7U) {
			return;
		}
		if (fields[6][0] != 'A') {
			if (gps_value_log_due()) {
				LOG_INF("L76K value GLL status=%c latitude_raw=\"%s%s\" longitude_raw=\"%s%s\"",
					fields[6][0] ? fields[6][0] : '-',
					fields[1], fields[2], fields[3], fields[4]);
			}
			return;
		}
		if (!parse_coordinate(fields[1], fields[2], &latitude) ||
		    !parse_coordinate(fields[3], fields[4], &longitude)) {
			return;
		}
	} else {
		return;
	}
	publish_fix(latitude, longitude, satellites);
}

int edgez_gps_init(void)
{
	struct uart_config config;
	int config_rc;
	int rx_level;

	if (gps_ready) {
		return 0;
	}
	if (!device_is_ready(gps_uart)) {
		LOG_WRN("L76K UART device is not ready");
		return -ENODEV;
	}
	config_rc = uart_callback_set(gps_uart, gps_uart_callback, NULL);
	if (config_rc < 0) {
		LOG_WRN("L76K async UART callback setup failed rc=%d", config_rc);
		return config_rc;
	}
	line_len = 0;
	debug_capture_len = 0;
	ring_buf_reset(&gps_rx_ring);
	received_bytes = 0;
	uart_error_flags = 0;
	ring_dropped_bytes = 0;
	rx_restart_count = 0;
	async_rx_buffer_idx = 1U;
	nmea_reported = false;
	fix_reported = false;
	gps_warning_reported = false;
	debug_capture_reported = false;
	gps_init_ms = k_uptime_get();
	gps_last_diag_ms = gps_init_ms;
	gps_last_value_log_ms = 0;
	gps_last_fix_publish_ms = 0;
	gps_ready = true;
	config_rc = uart_rx_enable(gps_uart, async_rx_buffer[0],
				   sizeof(async_rx_buffer[0]), GPS_RX_TIMEOUT_US);
	if (config_rc < 0) {
		gps_ready = false;
		LOG_WRN("L76K async UART RX enable failed rc=%d", config_rc);
		return config_rc;
	}
	LOG_INF("L76K ready UART=uart21 baud=9600 MCU_TX=P1.11->L76K_RXD MCU_RX=P1.12<-L76K_TXD [swapped-pin test]");
	config_rc = uart_config_get(gps_uart, &config);
	if (config_rc == 0) {
		LOG_INF("L76K UART config baud=%u data_bits=%u stop_bits=%u parity=%u flow_ctrl=%u",
			 config.baudrate, config.data_bits, config.stop_bits,
			 config.parity, config.flow_ctrl);
	} else if (config_rc == -ENOTSUP) {
		LOG_INF("L76K UART uses fixed devicetree config: 9600 8N1 no-flow-control");
	} else {
		LOG_WRN("L76K uart_config_get failed rc=%d", config_rc);
	}
	if (device_is_ready(gps_rx_gpio)) {
		rx_level = gpio_pin_get_raw(gps_rx_gpio, GPS_RX_PIN);
		LOG_INF("L76K RX electrical idle sample MCU P1.12 level=%d (expected 1 when idle)",
			 rx_level);
	} else {
		LOG_WRN("L76K RX diagnostic GPIO1 device is not ready");
	}
	return 0;
}

void edgez_gps_stop(void)
{
	k_spinlock_key_t key;

	if (gps_ready) {
		gps_ready = false;
		(void)uart_rx_disable(gps_uart);
	}
	line_len = 0;
	key = k_spin_lock(&fix_lock);
	latest_fix = (struct edgez_gps_fix){0};
	k_spin_unlock(&fix_lock, key);
}

void edgez_gps_poll(void)
{
	unsigned char byte;
	int rx_level = -ENODEV;
	int64_t now;

	if (!gps_ready) {
		return;
	}
	while (ring_buf_get(&gps_rx_ring, &byte, 1U) == 1U) {
		received_bytes++;
		if (debug_capture_len < sizeof(debug_capture)) {
			debug_capture[debug_capture_len++] = byte;
		}
		if (byte == '\n') {
			if (line_len > 0U) {
				line_buf[line_len] = '\0';
				parse_sentence(line_buf);
			}
			line_len = 0;
		} else if (byte != '\r') {
			if (line_len < sizeof(line_buf) - 1U) {
				line_buf[line_len++] = (char)byte;
			} else {
				line_len = 0;
			}
		}
	}
	now = k_uptime_get();
	if (!debug_capture_reported && debug_capture_len > 0U &&
	    (debug_capture_len == sizeof(debug_capture) || now - gps_init_ms >= 2000)) {
		LOG_HEXDUMP_INF(debug_capture, debug_capture_len, "L76K first UART bytes");
		debug_capture_reported = true;
	}
	if (now - gps_last_diag_ms >= GPS_DIAG_INTERVAL_MS) {
		gps_last_diag_ms = now;
		if (device_is_ready(gps_rx_gpio)) {
			rx_level = gpio_pin_get_raw(gps_rx_gpio, GPS_RX_PIN);
		}
		LOG_INF("L76K diag elapsed_ms=%lld bytes=%u rx_P1.12=%d uart_errors=0x%x ring_dropped=%u rx_restarts=%u nmea=%u fix=%u",
			(long long)(now - gps_init_ms), received_bytes, rx_level,
			uart_error_flags, ring_dropped_bytes, rx_restart_count,
			nmea_reported, fix_reported);
	}
	if (!gps_warning_reported && now - gps_init_ms >= 10000) {
		if (received_bytes == 0U) {
			LOG_WRN("L76K has no UART data after 10 s; check module power and L76K_TXD-to-MCU_RX=P1.12 wiring");
			gps_warning_reported = true;
		} else if (!nmea_reported) {
			LOG_WRN("L76K UART data is not valid 9600-baud NMEA bytes=%u; check baud and RX/TX wiring",
				received_bytes);
			gps_warning_reported = true;
		} else if (!fix_reported && k_uptime_get() - gps_init_ms >= 60000) {
			LOG_WRN("L76K NMEA is valid but no position fix after 60 s; check antenna and sky view");
			gps_warning_reported = true;
		}
	}
}

bool edgez_gps_is_ready(void)
{
	return gps_ready;
}

bool edgez_gps_get_fix(struct edgez_gps_fix *fix)
{
	k_spinlock_key_t key;
	int64_t now;

	if (!fix || !gps_ready) {
		return false;
	}
	key = k_spin_lock(&fix_lock);
	*fix = latest_fix;
	k_spin_unlock(&fix_lock, key);
	now = k_uptime_get();
	return fix->timestamp_ms > 0 && now >= fix->timestamp_ms &&
	       now - fix->timestamp_ms <= GPS_FIX_MAX_AGE_MS;
}
#else
int edgez_gps_init(void)
{
	return -ENODEV;
}

void edgez_gps_stop(void)
{
}

void edgez_gps_poll(void)
{
}

bool edgez_gps_is_ready(void)
{
	return false;
}

bool edgez_gps_get_fix(struct edgez_gps_fix *fix)
{
	ARG_UNUSED(fix);
	return false;
}
#endif
