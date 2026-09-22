#ifndef EDGEZ_REBOOT_H
#define EDGEZ_REBOOT_H

#include <stdint.h>

#define EDGEZ_REBOOT_REASON_HALOW_TX_FAILURE (1U << 0)
#define EDGEZ_REBOOT_REASON_SETTINGS_SAVED   (1U << 1)

void edgez_request_reboot(uint32_t reason);

#endif
