#include "edgez_battery.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(edgez_battery, LOG_LEVEL_INF);

#define BATTERY_NODE DT_PATH(zephyr_user)
#define BATTERY_DIVIDER_MULTIPLIER 2
#define BATTERY_SAMPLE_INTERVAL_MS 15000

static const struct adc_dt_spec battery_adc = ADC_DT_SPEC_GET_BY_IDX(BATTERY_NODE, 0);
static const struct gpio_dt_spec battery_enable =
	GPIO_DT_SPEC_GET(BATTERY_NODE, battery_enable_gpios);
static bool ready;
static int32_t cached_mv;
static int64_t last_sample_ms;

int edgez_battery_init(void)
{
	int rc;

	if (!adc_is_ready_dt(&battery_adc) || !gpio_is_ready_dt(&battery_enable)) {
		return -ENODEV;
	}
	rc = gpio_pin_configure_dt(&battery_enable, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) return rc;
	rc = adc_channel_setup_dt(&battery_adc);
	if (rc != 0) {
		LOG_ERR("Battery ADC channel setup failed: %d", rc);
		return rc;
	}
	ready = true;
	return 0;
}

int edgez_battery_read_mv(int32_t *millivolts)
{
	struct adc_sequence sequence = {0};
	int16_t raw = 0;
	int32_t input_mv;
	int rc;
	int64_t now = k_uptime_get();

	if (!millivolts || !ready) return -ENODEV;
	if (last_sample_ms > 0 && now - last_sample_ms < BATTERY_SAMPLE_INTERVAL_MS) {
		*millivolts = cached_mv;
		return 0;
	}
	rc = gpio_pin_set_dt(&battery_enable, 1);
	if (rc != 0) return rc;
	k_usleep(1000);
	rc = adc_sequence_init_dt(&battery_adc, &sequence);
	if (rc == 0) {
		sequence.buffer = &raw;
		sequence.buffer_size = sizeof(raw);
		rc = adc_read_dt(&battery_adc, &sequence);
	}
	(void)gpio_pin_set_dt(&battery_enable, 0);
	if (rc != 0) {
		LOG_WRN("Battery ADC read failed: %d", rc);
		return rc;
	}
	input_mv = raw;
	rc = adc_raw_to_millivolts_dt(&battery_adc, &input_mv);
	if (rc != 0) return rc;
	if (input_mv < 0 || input_mv > 2400) return -ERANGE;
	cached_mv = input_mv * BATTERY_DIVIDER_MULTIPLIER;
	last_sample_ms = now;
	*millivolts = cached_mv;
	LOG_DBG("Battery voltage %d mV", cached_mv);
	return 0;
}
