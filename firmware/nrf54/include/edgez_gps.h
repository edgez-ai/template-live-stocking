#ifndef EDGEZ_GPS_H
#define EDGEZ_GPS_H

#include <stdbool.h>
#include <stdint.h>

struct edgez_gps_fix {
	float latitude;
	float longitude;
	uint8_t satellites;
	int64_t timestamp_ms;
};

int edgez_gps_init(void);
void edgez_gps_stop(void);
void edgez_gps_poll(void);
bool edgez_gps_is_ready(void);
bool edgez_gps_get_fix(struct edgez_gps_fix *fix);

#endif

