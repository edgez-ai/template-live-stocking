#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using mqtt_l2_publish_fn_t = int (*)(const char *topic, const void *payload,
                                     size_t length, int qos, bool retain);
using mqtt_l2_command_fn_t = void (*)(const char *topic, const uint8_t *payload,
                                      size_t length);

esp_err_t mqtt_l2_relay_init(bool gateway, const char *device_serial,
                             mqtt_l2_publish_fn_t publish_fn,
                             mqtt_l2_command_fn_t command_fn);
void mqtt_l2_relay_set_local_mac(const uint8_t mac[6]);
void mqtt_l2_relay_set_gateway_online(bool online);
bool mqtt_l2_relay_gateway_available();
int mqtt_l2_relay_publish(const char *topic, const void *payload, size_t length,
                          int qos, bool retain);
bool mqtt_l2_relay_forward_command(const char *topic, const void *payload,
                                   size_t length, int qos, bool retain);
void mqtt_l2_relay_receive(const uint8_t originator[6], const uint8_t *data,
                           size_t length);
