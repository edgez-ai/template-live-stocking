#ifndef EDGEZ_BATTERY_H
#define EDGEZ_BATTERY_H

#include <stdint.h>

int edgez_battery_init(void);
int edgez_battery_read_mv(int32_t *millivolts);

#endif
