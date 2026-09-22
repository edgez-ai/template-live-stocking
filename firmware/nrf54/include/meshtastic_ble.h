#ifndef MESHTASTIC_BLE_H
#define MESHTASTIC_BLE_H

#include <stdbool.h>
#include <stdint.h>

int meshtastic_ble_start(void);
bool meshtastic_ble_is_connected(void);
bool meshtastic_ble_is_enabled(void);
void meshtastic_ble_update_battery(uint8_t level);

#endif
