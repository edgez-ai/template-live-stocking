#ifndef EDGEZ_IMU_H
#define EDGEZ_IMU_H

#include <stdbool.h>

struct edgez_imu_sample {
	float accel_m_s2[3];
	float gyro_rad_s[3];
};

int edgez_imu_init(void);
bool edgez_imu_is_ready(void);
int edgez_imu_read(struct edgez_imu_sample *sample);
void edgez_imu_note_halow_tx(void);
bool edgez_imu_halow_tx_quiet(void);

#endif
