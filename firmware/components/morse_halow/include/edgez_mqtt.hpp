#pragma once

#include <cstddef>
#include <cstdint>

#include "edgez_halow_radio.hpp"
#include "esp_err.h"

using edgez_mqtt_publish_fn_t = int (*)(const char *topic, const void *payload,
                                        size_t length, int qos, bool retain);
using edgez_mqtt_command_fn_t = void (*)(const char *topic,
                                         const uint8_t *payload,
                                         size_t length);

/**
 * Initialize the MQTT namespace and transparent BATMAN relay transport.
 * The SDK owns the system/status and telemetry/sensors topic selection and
 * the leading relay discriminator; payload bytes are never inspected.
 */
esp_err_t edgez_mqtt_init(HaLowInterface *radio, bool gateway,
                          const char *device_serial,
                          edgez_mqtt_publish_fn_t publish_fn,
                          edgez_mqtt_command_fn_t command_fn);
void edgez_mqtt_set_gateway_online(bool online);
void edgez_mqtt_set_local_mac(const uint8_t mac[6]);
bool edgez_mqtt_gateway_available();

/** Publish opaque protobuf system status bytes to system/status. */
int edgez_mqtt_publish_system_status(const void *payload, size_t length,
                                     int qos = 0, bool retain = false);

/** Publish opaque application bytes to telemetry/sensors. */
int edgez_mqtt_publish_telemetry(const void *payload, size_t length,
                                 int qos = 0, bool retain = false);

bool edgez_mqtt_forward_command(const char *topic, const void *payload,
                                size_t length, int qos, bool retain);
void edgez_mqtt_receive(const uint8_t originator[6], const uint8_t *data,
                        size_t length);
