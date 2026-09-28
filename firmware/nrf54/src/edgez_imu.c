#include "edgez_imu.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(edgez_imu, LOG_LEVEL_INF);

#define EDGEZ_IMU_SAMPLE_RATE_HZ 12
#define EDGEZ_IMU_SETTLE_MS 100
#define EDGEZ_IMU_SAMPLE_PERIOD_MS (1000 / EDGEZ_IMU_SAMPLE_RATE_HZ)
#define EDGEZ_IMU_STRONG_MOTION_DELTA_M_S2 2.5f
#define EDGEZ_IMU_MOTION_HOLD_MS 15000U
#define EDGEZ_IMU_REINIT_TIMEOUT_SECONDS 60U
#define EDGEZ_IMU_REINIT_FAILURE_LIMIT \
	(EDGEZ_IMU_SAMPLE_RATE_HZ * EDGEZ_IMU_REINIT_TIMEOUT_SECONDS)
#define EDGEZ_IMU_HALOW_TX_QUIET_MS 100U
#define EDGEZ_IMU_WHO_AM_I_REG 0x0FU
#define EDGEZ_IMU_WHO_AM_I_VALUE 0x6AU
#define EDGEZ_IMU_CTRL1_XL_REG 0x10U
#define EDGEZ_IMU_CTRL2_G_REG 0x11U
#define EDGEZ_IMU_CTRL3_C_REG 0x12U
#define EDGEZ_IMU_CTRL6_C_REG 0x15U
#define EDGEZ_IMU_WAKE_UP_SRC_REG 0x1BU
#define EDGEZ_IMU_OUTX_L_A_REG 0x28U
#define EDGEZ_IMU_TAP_CFG_REG 0x58U
#define EDGEZ_IMU_WAKE_UP_THS_REG 0x5BU
#define EDGEZ_IMU_WAKE_UP_DUR_REG 0x5CU
#define EDGEZ_IMU_MD1_CFG_REG 0x5EU
#define EDGEZ_IMU_CTRL3_SW_RESET BIT(0)
#define EDGEZ_IMU_CTRL3_IF_INC BIT(2)
#define EDGEZ_IMU_CTRL3_BDU BIT(6)
#define EDGEZ_IMU_ODR_12_5_HZ BIT(4)
#define EDGEZ_IMU_INTERRUPTS_ENABLE BIT(7)
#define EDGEZ_IMU_LATCH_INTERRUPTS BIT(0)
#define EDGEZ_IMU_INT1_WAKE_UP BIT(5)
/* At the +/-2 g scale one threshold LSB is 2 g / 64 = 31.25 mg. Eight
 * counts is approximately 2.45 m/s2, matching the software fallback. */
#define EDGEZ_IMU_WAKE_THRESHOLD 8U
#define EDGEZ_IMU_WAKE_DURATION BIT(5)
#define EDGEZ_IMU_ACCEL_M_S2_PER_LSB 0.00059820565f
#define EDGEZ_IMU_RESET_TIMEOUT_MS 50U
#define EDGEZ_IMU_BUS_RECOVERY_SETTLE_MS 10U

K_MUTEX_DEFINE(imu_lock);

#if DT_HAS_ALIAS(imu0) && DT_NODE_HAS_STATUS(DT_ALIAS(imu0), okay)
static const struct i2c_dt_spec imu_i2c = I2C_DT_SPEC_GET(DT_ALIAS(imu0));
#if DT_NODE_HAS_PROP(DT_ALIAS(imu0), irq_gpios)
static const struct gpio_dt_spec imu_irq =
	GPIO_DT_SPEC_GET(DT_ALIAS(imu0), irq_gpios);
#define EDGEZ_IMU_HAS_IRQ 1
#else
static const struct gpio_dt_spec imu_irq = {0};
#define EDGEZ_IMU_HAS_IRQ 0
#endif
#else
static const struct i2c_dt_spec imu_i2c;
static const struct gpio_dt_spec imu_irq = {0};
#define EDGEZ_IMU_HAS_IRQ 0
#endif

static bool imu_ready;
static bool imu_sample_valid;
static uint32_t imu_sample_failure_count;
static atomic_t halow_last_tx_ms;
static atomic_t last_strong_motion_ms;
static struct edgez_imu_sample latest_sample;
static float previous_accel_m_s2[3];
static bool previous_accel_valid;
static bool imu_irq_ready;
static struct gpio_callback imu_irq_callback;

static void imu_sample_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(imu_sample_work, imu_sample_work_handler);

static void note_strong_motion(void)
{
	uint32_t now_ms = k_uptime_get_32();

	atomic_set(&last_strong_motion_ms,
		   (atomic_val_t)(now_ms == 0U ? 1U : now_ms));
}

static void imu_irq_handler(const struct device *port, struct gpio_callback *cb,
			    uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	/* GPIO callbacks run in interrupt context: latch the event and defer the
	 * EasyDMA I2C transfer and WAKE_UP_SRC acknowledgement to system work. */
	note_strong_motion();
	(void)k_work_reschedule(&imu_sample_work, K_NO_WAIT);
}

static uint32_t halow_tx_quiet_remaining_ms(void)
{
	uint32_t last_tx_ms = (uint32_t)atomic_get(&halow_last_tx_ms);
	uint32_t elapsed_ms;

	if (last_tx_ms == 0U) {
		return 0U;
	}
	elapsed_ms = k_uptime_get_32() - last_tx_ms;
	return elapsed_ms < EDGEZ_IMU_HALOW_TX_QUIET_MS ?
		EDGEZ_IMU_HALOW_TX_QUIET_MS - elapsed_ms : 0U;
}

void edgez_imu_note_halow_tx(void)
{
	atomic_set(&halow_last_tx_ms, (atomic_val_t)k_uptime_get_32());
}

bool edgez_imu_halow_tx_quiet(void)
{
	return halow_tx_quiet_remaining_ms() == 0U;
}

static int set_sampling_rate(int hz)
{
	uint8_t accel_ctrl = hz > 0 ? EDGEZ_IMU_ODR_12_5_HZ : 0U;
	int rc = i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL1_XL_REG,
				       accel_ctrl);

	if (rc < 0) {
		return rc;
	}
	/* Motion detection only needs acceleration. Leaving CTRL2_G at zero keeps
	 * the substantially more expensive gyroscope powered down. */
	return i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL2_G_REG, 0U);
}

static int imu_configure(void)
{
	uint8_t chip_id = 0U;
	uint8_t ctrl3;
	int recovery_rc;
	int rc;

	if (!imu_i2c.bus || !device_is_ready(imu_i2c.bus)) {
		return -ENODEV;
	}

	rc = i2c_reg_read_byte_dt(&imu_i2c, EDGEZ_IMU_WHO_AM_I_REG, &chip_id);
	if (rc < 0) {
		/* A warm MCU reboot does not power-cycle the IMU. If reset happens
		 * during a transfer, the peripheral can retain a partial I2C
		 * transaction and hold the bus until it sees recovery clocks. A
		 * plain delayed WHO_AM_I retry cannot clear that state. */
		recovery_rc = i2c_recover_bus(imu_i2c.bus);
		if (recovery_rc == 0) {
			k_msleep(EDGEZ_IMU_BUS_RECOVERY_SETTLE_MS);
			rc = i2c_reg_read_byte_dt(&imu_i2c,
					      EDGEZ_IMU_WHO_AM_I_REG, &chip_id);
			if (rc == 0) {
				LOG_INF("IMU I2C bus recovered after WHO_AM_I failure");
			}
		}
		if (rc < 0) {
			LOG_WRN("IMU WHO_AM_I read failed bus=%s addr=0x%02x rc=%d recovery_rc=%d",
				imu_i2c.bus->name, imu_i2c.addr, rc, recovery_rc);
			return rc;
		}
	}
	if (chip_id != EDGEZ_IMU_WHO_AM_I_VALUE) {
		LOG_WRN("Unexpected IMU WHO_AM_I bus=%s addr=0x%02x got=0x%02x expected=0x%02x",
			imu_i2c.bus->name, imu_i2c.addr, chip_id,
			EDGEZ_IMU_WHO_AM_I_VALUE);
		return -ENODEV;
	}

	/* A direct software reset keeps this initialization retryable. Unlike
	 * device_init(), a failed transaction does not poison Zephyr's device
	 * state and the next delayed attempt reaches the sensor again. */
	rc = i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL3_C_REG,
				       EDGEZ_IMU_CTRL3_SW_RESET);
	if (rc < 0) {
		return rc;
	}
	for (uint32_t waited_ms = 0U; waited_ms < EDGEZ_IMU_RESET_TIMEOUT_MS;
	     waited_ms++) {
		k_msleep(1);
		rc = i2c_reg_read_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL3_C_REG, &ctrl3);
		if (rc < 0) {
			return rc;
		}
		if ((ctrl3 & EDGEZ_IMU_CTRL3_SW_RESET) == 0U) {
			break;
		}
		if (waited_ms + 1U == EDGEZ_IMU_RESET_TIMEOUT_MS) {
			return -ETIMEDOUT;
		}
	}

	rc = i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL3_C_REG,
				       EDGEZ_IMU_CTRL3_BDU | EDGEZ_IMU_CTRL3_IF_INC);
	if (rc == 0) {
		/* Use the low-power paths at the 12.5 Hz output data rate. */
		rc = i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL6_C_REG, BIT(4));
	}
	if (rc == 0) {
		rc = set_sampling_rate(EDGEZ_IMU_SAMPLE_RATE_HZ);
	}
	if (rc == 0 && EDGEZ_IMU_HAS_IRQ && gpio_is_ready_dt(&imu_irq)) {
		uint8_t wake_source;
		int irq_rc;

		irq_rc = gpio_pin_configure_dt(&imu_irq, GPIO_INPUT);
		if (irq_rc == 0) {
			gpio_init_callback(&imu_irq_callback, imu_irq_handler,
					   BIT(imu_irq.pin));
			irq_rc = gpio_add_callback(imu_irq.port, &imu_irq_callback);
		}
		if (irq_rc == 0) {
			irq_rc = gpio_pin_interrupt_configure_dt(&imu_irq,
							     GPIO_INT_EDGE_TO_ACTIVE);
		}
		if (irq_rc == 0) {
			irq_rc = i2c_reg_write_byte_dt(&imu_i2c,
							   EDGEZ_IMU_WAKE_UP_THS_REG,
							   EDGEZ_IMU_WAKE_THRESHOLD);
		}
		if (irq_rc == 0) {
			irq_rc = i2c_reg_write_byte_dt(&imu_i2c,
							   EDGEZ_IMU_WAKE_UP_DUR_REG,
							   EDGEZ_IMU_WAKE_DURATION);
		}
		if (irq_rc == 0) {
			irq_rc = i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_TAP_CFG_REG,
							   EDGEZ_IMU_INTERRUPTS_ENABLE |
							   EDGEZ_IMU_LATCH_INTERRUPTS);
		}
		if (irq_rc == 0) {
			irq_rc = i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_MD1_CFG_REG,
							   EDGEZ_IMU_INT1_WAKE_UP);
		}
		if (irq_rc == 0) {
			/* Clear a wake condition that may have accumulated while the GPIO
			 * callback was being installed. */
			irq_rc = i2c_reg_read_byte_dt(&imu_i2c, EDGEZ_IMU_WAKE_UP_SRC_REG,
							    &wake_source);
		}
		if (irq_rc == 0) {
			imu_irq_ready = true;
		} else {
			(void)gpio_pin_interrupt_configure_dt(&imu_irq, GPIO_INT_DISABLE);
			(void)gpio_remove_callback(imu_irq.port, &imu_irq_callback);
			LOG_WRN("IMU wake interrupt unavailable rc=%d; using 12.5 Hz polling fallback",
				irq_rc);
		}
	}
	return rc;
}

static void imu_sample_work_handler(struct k_work *work)
{
	struct edgez_imu_sample next_sample;
	uint8_t raw[6];
	uint32_t quiet_remaining_ms;
	int rc;

	ARG_UNUSED(work);
	if (!imu_ready) {
		return;
	}
	quiet_remaining_ms = halow_tx_quiet_remaining_ms();
	if (quiet_remaining_ms > 0U) {
		/* A skipped collision is not an IMU failure. Try again immediately
		 * after the radio's short post-beacon quiet window. */
		(void)k_work_schedule(&imu_sample_work, K_MSEC(quiet_remaining_ms));
		return;
	}

	/* The nRF TWIM driver moves this burst through EasyDMA. Reading only the
	 * accelerometer halves the transfer and lets the gyroscope remain off. */
	if (imu_irq_ready) {
		uint8_t wake_source;

		/* Reading WAKE_UP_SRC acknowledges the latched INT1 signal. */
		(void)i2c_reg_read_byte_dt(&imu_i2c, EDGEZ_IMU_WAKE_UP_SRC_REG,
					    &wake_source);
	}
	rc = i2c_burst_read_dt(&imu_i2c, EDGEZ_IMU_OUTX_L_A_REG,
			    raw, sizeof(raw));
	if (rc == 0) {
		float motion_delta_sq = 0.0f;

		memset(&next_sample, 0, sizeof(next_sample));
		for (size_t i = 0; i < 3; i++) {
			int16_t accel_raw = (int16_t)sys_get_le16(&raw[i * 2U]);

			next_sample.accel_m_s2[i] =
				(float)accel_raw * EDGEZ_IMU_ACCEL_M_S2_PER_LSB;
			if (previous_accel_valid) {
				float delta = next_sample.accel_m_s2[i] - previous_accel_m_s2[i];

				motion_delta_sq += delta * delta;
			}
			previous_accel_m_s2[i] = next_sample.accel_m_s2[i];
		}
		if (previous_accel_valid &&
		    motion_delta_sq >= EDGEZ_IMU_STRONG_MOTION_DELTA_M_S2 *
					       EDGEZ_IMU_STRONG_MOTION_DELTA_M_S2) {
			note_strong_motion();
		}
		previous_accel_valid = true;
		k_mutex_lock(&imu_lock, K_FOREVER);
		latest_sample = next_sample;
		imu_sample_valid = true;
		k_mutex_unlock(&imu_lock);
		imu_sample_failure_count = 0;
	} else {
		imu_sample_failure_count++;
		if (imu_sample_failure_count == 1 ||
		    imu_sample_failure_count % EDGEZ_IMU_SAMPLE_RATE_HZ == 0) {
			LOG_WRN("Async IMU sample failed: %d (count=%u)", rc,
				imu_sample_failure_count);
		}
		if (imu_sample_failure_count >= EDGEZ_IMU_REINIT_FAILURE_LIMIT) {
			imu_ready = false;
			k_mutex_lock(&imu_lock, K_FOREVER);
			imu_sample_valid = false;
			k_mutex_unlock(&imu_lock);
			(void)set_sampling_rate(0);
			LOG_ERR("IMU unavailable for %u seconds; stopping samples for delayed reinitialization",
				(unsigned int)EDGEZ_IMU_REINIT_TIMEOUT_SECONDS);
			return;
		}
	}

	/* With INT1 available, the sensor's wake engine is the sampler clock and
	 * the CPU can stay asleep. Boards without that wire retain polling. */
	if (!imu_irq_ready) {
		(void)k_work_schedule(&imu_sample_work,
				      K_MSEC(EDGEZ_IMU_SAMPLE_PERIOD_MS));
	}
}

int edgez_imu_init(void)
{
	static uint32_t init_failure_count;
	int rc;

	if (!imu_i2c.bus) {
		return -ENODEV;
	}
	if (imu_ready) {
		return 0;
	}

	rc = imu_configure();
	if (rc < 0) {
		init_failure_count++;
		if (init_failure_count == 1 || (init_failure_count % 12U) == 0U) {
			LOG_WRN("IMU initialization failed rc=%d bus=%s addr=0x%02x attempt=%u",
				rc, imu_i2c.bus->name, imu_i2c.addr,
				init_failure_count);
		}
		return rc;
	}
	imu_ready = true;
	imu_sample_failure_count = 0;
	previous_accel_valid = false;
	init_failure_count = 0;
	(void)k_work_schedule(&imu_sample_work, K_MSEC(EDGEZ_IMU_SETTLE_MS));
	LOG_INF("Low-power accelerometer ready bus=%s addr=0x%02x rate=%d Hz wake_irq=%u",
		imu_i2c.bus->name, imu_i2c.addr, EDGEZ_IMU_SAMPLE_RATE_HZ,
		imu_irq_ready);
	return 0;
}

bool edgez_imu_motion_active(void)
{
	uint32_t last_motion_ms = (uint32_t)atomic_get(&last_strong_motion_ms);

	return last_motion_ms != 0U &&
		(k_uptime_get_32() - last_motion_ms) < EDGEZ_IMU_MOTION_HOLD_MS;
}

bool edgez_imu_is_ready(void)
{
	return imu_ready;
}

int edgez_imu_read(struct edgez_imu_sample *sample)
{
	if (!sample) {
		return -EINVAL;
	}
	if (!imu_ready) {
		return -ENODEV;
	}

	k_mutex_lock(&imu_lock, K_FOREVER);
	if (!imu_sample_valid) {
		k_mutex_unlock(&imu_lock);
		return -EAGAIN;
	}
	*sample = latest_sample;
	k_mutex_unlock(&imu_lock);
	return 0;
}

void edgez_imu_request_sample(void)
{
	if (imu_ready) {
		(void)k_work_reschedule(&imu_sample_work, K_NO_WAIT);
	}
}
