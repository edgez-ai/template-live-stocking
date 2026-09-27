#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

enum class SensorScriptValueKind : uint8_t {
  kBool,
  kInt,
  kFloat,
};

struct SensorScriptValue {
  int type;
  SensorScriptValueKind kind;
  union {
    bool bool_value;
    int32_t int_value;
    float float_value;
  } value;
};

using SensorScriptResultCallback = void (*)(const SensorScriptValue *values, size_t count);
using SensorScriptStatusCallback = void (*)(const char *request_id, const char *status,
                                            const char *error);
using SensorScriptBlobCallback = void (*)(uint32_t execution_id, const uint8_t *data,
                                          size_t length);

// Loads any saved script and starts the script command/execution task.
esp_err_t sensor_script_init(SensorScriptResultCallback result_callback,
                             SensorScriptStatusCallback status_callback,
                             SensorScriptBlobCallback blob_callback);

// Handles an unfragmented MQTT command. Returns true when topic is the script topic.
bool sensor_script_handle_mqtt_command(const char *topic, size_t topic_length,
                                       const char *payload, size_t payload_length,
                                       const char *project_id, const char *serial);

// GPIO20 is shared with the board's battery divider control. Battery sampling
// uses this guard so it cannot reconfigure the pin during a Lua I2C/UART run.
bool sensor_script_try_acquire_hardware();
void sensor_script_release_hardware();
