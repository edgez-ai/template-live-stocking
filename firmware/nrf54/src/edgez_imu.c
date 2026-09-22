#include "edgez_imu.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(edgez_imu, LOG_LEVEL_INF);

#define EDGEZ_IMU_SAMPLE_RATE_HZ 12
#define EDGEZ_IMU_SETTLE_MS 100
#define EDGEZ_IMU_SAMPLE_PERIOD_MS (1000 / EDGEZ_IMU_SAMPLE_RATE_HZ)
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
#define EDGEZ_IMU_CTRL7_G_REG 0x16U
#define EDGEZ_IMU_OUTX_L_G_REG 0x22U
#define EDGEZ_IMU_CTRL3_SW_RESET BIT(0)
#define EDGEZ_IMU_CTRL3_IF_INC BIT(2)
#define EDGEZ_IMU_CTRL3_BDU BIT(6)
#define EDGEZ_IMU_ODR_12_5_HZ BIT(4)
#define EDGEZ_IMU_GYRO_FS_125_DPS BIT(1)
#define EDGEZ_IMU_ACCEL_M_S2_PER_LSB 0.00059820565f
#define EDGEZ_IMU_GYRO_RAD_S_PER_LSB 0.00007635815f
#define EDGEZ_IMU_RESET_TIMEOUT_MS 50U
#define EDGEZ_IMU_BUS_RECOVERY_SETTLE_MS 10U

K_MUTEX_DEFINE(imu_lock);

#if DT_HAS_ALIAS(imu0) && DT_NODE_HAS_STATUS(DT_ALIAS(imu0), okay)
static const struct i2c_dt_spec imu_i2c = I2C_DT_SPEC_GET(DT_ALIAS(imu0));
#else
static const struct i2c_dt_spec imu_i2c;
#endif

static bool imu_ready;
static bool imu_sample_valid;
static uint32_t imu_sample_failure_count;
static atomic_t halow_last_tx_ms;
static struct edgez_imu_sample latest_sample;

static void imu_sample_work_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(imu_sample_work, imu_sample_work_handler);

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
	uint8_t gyro_ctrl = hz > 0 ?
		EDGEZ_IMU_ODR_12_5_HZ | EDGEZ_IMU_GYRO_FS_125_DPS : 0U;
	int rc = i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL1_XL_REG,
				       accel_ctrl);

	if (rc < 0) {
		return rc;
	}
	return i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL2_G_REG,
				      gyro_ctrl);
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
		rc = i2c_reg_write_byte_dt(&imu_i2c, EDGEZ_IMU_CTRL7_G_REG, BIT(7));
	}
	if (rc == 0) {
		rc = set_sampling_rate(EDGEZ_IMU_SAMPLE_RATE_HZ);
	}
	return rc;
}

static void imu_sample_work_handler(struct k_work *work)
{
	struct edgez_imu_sample next_sample;
	uint8_t raw[12];
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

	rc = i2c_burst_read_dt(&imu_i2c, EDGEZ_IMU_OUTX_L_G_REG,
			    raw, sizeof(raw));
	if (rc == 0) {
		for (size_t i = 0; i < 3; i++) {
			int16_t gyro_raw = (int16_t)sys_get_le16(&raw[i * 2U]);
			int16_t accel_raw = (int16_t)sys_get_le16(&raw[6U + i * 2U]);

			next_sample.accel_m_s2[i] =
				(float)accel_raw * EDGEZ_IMU_ACCEL_M_S2_PER_LSB;
			next_sample.gyro_rad_s[i] =
				(float)gyro_raw * EDGEZ_IMU_GYRO_RAD_S_PER_LSB;
		}
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

	(void)k_work_schedule(&imu_sample_work, K_MSEC(EDGEZ_IMU_SAMPLE_PERIOD_MS));
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
	init_failure_count = 0;
	(void)k_work_schedule(&imu_sample_work, K_MSEC(EDGEZ_IMU_SETTLE_MS));
	LOG_INF("6-axis IMU ready bus=%s addr=0x%02x async_sample_rate=%d Hz",
		imu_i2c.bus->name, imu_i2c.addr, EDGEZ_IMU_SAMPLE_RATE_HZ);
	return 0;
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
