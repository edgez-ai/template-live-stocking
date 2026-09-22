#ifndef LIVESTOCKING_CONFIG_H
#define LIVESTOCKING_CONFIG_H

#include <stddef.h>
#include <stdbool.h>

void livestock_config_init(void);
const char *livestock_config_country(void);
bool livestock_config_is_provisioned(void);
int livestock_config_apply_json(char *json, size_t length, const char *device_serial);

#endif
