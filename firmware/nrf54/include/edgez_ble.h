#ifndef EDGEZ_BLE_H
#define EDGEZ_BLE_H

#include <stdbool.h>
#include <stdint.h>

int edgez_ble_start(void);
bool edgez_ble_is_connected(void);
bool edgez_ble_is_enabled(void);
void edgez_ble_update_battery(uint8_t level);

#endif
