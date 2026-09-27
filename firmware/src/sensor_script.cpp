#include "sensor_script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "nvs.h"

namespace {
constexpr char kTag[] = "sensor_script";
constexpr char kNamespace[] = "sensor_script";
constexpr char kSourceKey[] = "source";
constexpr char kIntervalKey[] = "interval";
constexpr size_t kMaxScriptSize = 8192;
constexpr size_t kGlobalBufferDefaultSize = 16 * 1024;
constexpr size_t kGlobalBufferMaxSize = 16 * 1024;
constexpr size_t kMaxRequestIdSize = 64;
constexpr uint32_t kDefaultIntervalSeconds = 30;
constexpr uint32_t kMinIntervalSeconds = 5;
constexpr uint32_t kMaxIntervalSeconds = 86400;
constexpr i2c_port_t kI2cPort = I2C_NUM_0;
constexpr uart_port_t kUartPort = UART_NUM_1;
constexpr uart_port_t kRs485Port = UART_NUM_2;
constexpr gpio_num_t kSensorPower = GPIO_NUM_14;
constexpr gpio_num_t kRs485Power = GPIO_NUM_13;

enum class CommandAction : uint8_t { kSet, kRun, kDelete };

struct ScriptCommand {
  CommandAction action;
  uint32_t interval_seconds;
  char *source;
  char request_id[kMaxRequestIdSize];
};

QueueHandle_t command_queue;
SemaphoreHandle_t hardware_mutex;
SensorScriptResultCallback result_callback;
SensorScriptStatusCallback status_callback;
SensorScriptBlobCallback blob_callback;
char *active_source;
size_t active_source_length;
uint32_t active_interval_seconds = kDefaultIntervalSeconds;
bool i2c_open;
uint8_t i2c_address = 0x44;
size_t i2c_rx_size = 6;
bool uart_open;
bool rs485_open;
size_t uart_rx_size = 256;
size_t rs485_rx_size = 256;
uint8_t *global_buffer;
size_t global_buffer_capacity;
size_t global_buffer_length;
uint32_t execution_counter;

bool valid_interval(double value) {
  return value >= kMinIntervalSeconds && value <= kMaxIntervalSeconds &&
         value == static_cast<uint32_t>(value);
}

esp_err_t set_power(gpio_num_t pin, bool enabled) {
  gpio_config_t config{};
  config.pin_bit_mask = 1ULL << pin;
  config.mode = GPIO_MODE_OUTPUT;
  esp_err_t result = gpio_config(&config);
  if (result == ESP_OK) result = gpio_set_level(pin, enabled ? 1 : 0);
  if (result == ESP_OK && enabled) vTaskDelay(pdMS_TO_TICKS(1000));
  return result;
}

void close_i2c() {
  if (i2c_open) i2c_driver_delete(kI2cPort);
  i2c_open = false;
  if (!uart_open) gpio_set_level(kSensorPower, 0);
}

void close_uart_port(uart_port_t port, bool *opened, gpio_num_t power) {
  if (*opened) uart_driver_delete(port);
  *opened = false;
  gpio_set_level(power, 0);
}

void close_script_interfaces() {
  close_i2c();
  close_uart_port(kUartPort, &uart_open, kSensorPower);
  close_uart_port(kRs485Port, &rs485_open, kRs485Power);
}

esp_err_t open_uart_port(uart_port_t port, int baud, int tx, int rx,
                         size_t rx_size, bool *opened, gpio_num_t power) {
  if (baud < 300 || baud > 3000000) return ESP_ERR_INVALID_ARG;
  if (port == kUartPort && i2c_open) close_i2c();
  close_uart_port(port, opened, power);
  esp_err_t result = set_power(power, true);
  uart_config_t config{};
  config.baud_rate = baud;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  if (result == ESP_OK) result = uart_param_config(port, &config);
  if (result == ESP_OK) result = uart_set_pin(port, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (result == ESP_OK) result = uart_driver_install(port, rx_size, 0, 0, nullptr, 0);
  if (result == ESP_OK) result = uart_set_mode(port, UART_MODE_UART);
  if (result == ESP_OK) {
    *opened = true;
    uart_flush_input(port);
  } else {
    gpio_set_level(power, 0);
  }
  return result;
}

int push_result(lua_State *state, esp_err_t result, const char *message) {
  lua_pushboolean(state, result == ESP_OK);
  if (result == ESP_OK) return 1;
  lua_pushstring(state, message ? message : esp_err_to_name(result));
  return 2;
}

int lua_i2c_connect(lua_State *state) {
  const int address = static_cast<int>(luaL_optinteger(state, 1, 0x44));
  if (address < 0x08 || address > 0x77) return push_result(state, ESP_ERR_INVALID_ARG, "invalid i2c address");
  if (uart_open) close_uart_port(kUartPort, &uart_open, kSensorPower);
  if (!i2c_open) {
    esp_err_t result = set_power(kSensorPower, true);
    i2c_config_t config{};
    config.mode = I2C_MODE_MASTER;
    config.sda_io_num = GPIO_NUM_19;
    config.scl_io_num = GPIO_NUM_20;
    config.sda_pullup_en = GPIO_PULLUP_ENABLE;
    config.scl_pullup_en = GPIO_PULLUP_ENABLE;
    config.master.clk_speed = 100000;
    if (result == ESP_OK) result = i2c_param_config(kI2cPort, &config);
    if (result == ESP_OK) result = i2c_driver_install(kI2cPort, I2C_MODE_MASTER, 0, 0, 0);
    if (result != ESP_OK) return push_result(state, result, nullptr);
    i2c_open = true;
  }
  i2c_address = static_cast<uint8_t>(address);
  return push_result(state, ESP_OK, nullptr);
}

int lua_i2c_safe_close(lua_State *) { close_i2c(); return 0; }
int lua_i2c_reset_rx_cursor(lua_State *state) {
  return push_result(state, i2c_open ? ESP_OK : ESP_ERR_INVALID_STATE, "i2c not open");
}
int lua_i2c_set_rx_size(lua_State *state) {
  if (!i2c_open) return push_result(state, ESP_ERR_INVALID_STATE, "i2c not open");
  lua_Integer size = luaL_checkinteger(state, 1);
  if (size < 1 || size > 256) return push_result(state, ESP_ERR_INVALID_ARG, "i2c rx size must be 1..256");
  i2c_rx_size = static_cast<size_t>(size);
  return push_result(state, ESP_OK, nullptr);
}
int lua_i2c_write(lua_State *state) {
  if (!i2c_open) return push_result(state, ESP_ERR_INVALID_STATE, "i2c not open");
  size_t length = 0;
  const char *data = luaL_checklstring(state, 1, &length);
  return push_result(state, i2c_master_write_to_device(kI2cPort, i2c_address,
      reinterpret_cast<const uint8_t *>(data), length, pdMS_TO_TICKS(1000)), nullptr);
}
int lua_i2c_read_chunk(lua_State *state) {
  if (!i2c_open) { lua_pushliteral(state, ""); return 1; }
  uint8_t data[256]{};
  esp_err_t result = i2c_master_read_from_device(kI2cPort, i2c_address, data,
                                                  i2c_rx_size, pdMS_TO_TICKS(1000));
  if (result != ESP_OK) { lua_pushliteral(state, ""); return 1; }
  lua_pushlstring(state, reinterpret_cast<const char *>(data), i2c_rx_size);
  return 1;
}

int lua_uart_connect(lua_State *state) {
  const int baud = static_cast<int>(luaL_optinteger(state, 1, 115200));
  return push_result(state, open_uart_port(kUartPort, baud, 19, 20, uart_rx_size,
                                           &uart_open, kSensorPower), nullptr);
}
int lua_rs485_connect(lua_State *state) {
  const int baud = static_cast<int>(luaL_optinteger(state, 1, 9600));
  return push_result(state, open_uart_port(kRs485Port, baud, 17, 18, rs485_rx_size,
                                           &rs485_open, kRs485Power), nullptr);
}
int lua_uart_safe_close(lua_State *) { close_uart_port(kUartPort, &uart_open, kSensorPower); return 0; }
int lua_rs485_safe_close(lua_State *) { close_uart_port(kRs485Port, &rs485_open, kRs485Power); return 0; }

int reset_uart(lua_State *state, uart_port_t port, bool opened, const char *message) {
  if (!opened) return push_result(state, ESP_ERR_INVALID_STATE, message);
  return push_result(state, uart_flush_input(port), nullptr);
}
int lua_uart_reset_rx_cursor(lua_State *state) { return reset_uart(state, kUartPort, uart_open, "uart not open"); }
int lua_rs485_reset_rx_cursor(lua_State *state) { return reset_uart(state, kRs485Port, rs485_open, "rs485 not open"); }

int set_uart_rx_size(lua_State *state, size_t *target, bool opened, const char *message) {
  if (!opened) return push_result(state, ESP_ERR_INVALID_STATE, message);
  lua_Integer size = luaL_checkinteger(state, 1);
  if (size < 1 || size > 4096) return push_result(state, ESP_ERR_INVALID_ARG, "rx size must be 1..4096");
  *target = static_cast<size_t>(size);
  return push_result(state, ESP_OK, nullptr);
}
int lua_uart_set_rx_size(lua_State *state) {
  return set_uart_rx_size(state, &uart_rx_size, uart_open, "uart not open");
}

int write_uart(lua_State *state, uart_port_t port, bool opened, const char *message) {
  if (!opened) return push_result(state, ESP_ERR_INVALID_STATE, message);
  size_t length = 0;
  const char *data = luaL_checklstring(state, 1, &length);
  const int written = uart_write_bytes(port, data, length);
  if (written == static_cast<int>(length)) uart_wait_tx_done(port, pdMS_TO_TICKS(1000));
  return push_result(state, written == static_cast<int>(length) ? ESP_OK : ESP_FAIL, "uart tx failed");
}
int lua_uart_write(lua_State *state) { return write_uart(state, kUartPort, uart_open, "uart not open"); }
int lua_rs485_write(lua_State *state) { return write_uart(state, kRs485Port, rs485_open, "rs485 not open"); }

int read_uart(lua_State *state, uart_port_t port, bool opened, size_t rx_size) {
  if (!opened) { lua_pushliteral(state, ""); return 1; }
  uint8_t data[256]{};
  const size_t wanted = rx_size < sizeof(data) ? rx_size : sizeof(data);
  const int length = uart_read_bytes(port, data, wanted, pdMS_TO_TICKS(100));
  lua_pushlstring(state, reinterpret_cast<const char *>(data), length > 0 ? length : 0);
  return 1;
}
int lua_uart_read_chunk(lua_State *state) { return read_uart(state, kUartPort, uart_open, uart_rx_size); }
int lua_uart_read_frame(lua_State *state) { return lua_uart_read_chunk(state); }
int lua_rs485_read_chunk(lua_State *state) { return read_uart(state, kRs485Port, rs485_open, rs485_rx_size); }

int lua_sleep(lua_State *state) {
  const double seconds = luaL_optnumber(state, 1, 0);
  if (seconds > 0 && seconds <= 60) vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(seconds * 1000)));
  return 0;
}

uint16_t modbus_crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xffff;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
  }
  return crc;
}

int lua_util_crc8(lua_State *state) {
  size_t length = 0;
  const auto *data = reinterpret_cast<const uint8_t *>(luaL_checklstring(state, 1, &length));
  uint8_t polynomial = static_cast<uint8_t>(luaL_optinteger(state, 2, 0x31));
  uint8_t crc = static_cast<uint8_t>(luaL_optinteger(state, 3, 0xff));
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) crc = crc & 0x80 ? (crc << 1) ^ polynomial : crc << 1;
  }
  lua_pushinteger(state, crc);
  return 1;
}
int lua_util_bytes_to_hex(lua_State *state) {
  size_t length = 0;
  const auto *data = reinterpret_cast<const uint8_t *>(luaL_checklstring(state, 1, &length));
  luaL_Buffer output;
  luaL_buffinit(state, &output);
  for (size_t index = 0; index < length; ++index) {
    char hex[3]{};
    std::snprintf(hex, sizeof(hex), "%02x", data[index]);
    luaL_addlstring(&output, hex, 2);
  }
  luaL_pushresult(&output);
  return 1;
}
int lua_util_build_read_holding_request(lua_State *state) {
  uint8_t frame[8]{};
  const int unit = static_cast<int>(luaL_checkinteger(state, 1));
  const int address = static_cast<int>(luaL_checkinteger(state, 2));
  const int count = static_cast<int>(luaL_checkinteger(state, 3));
  frame[0] = unit; frame[1] = 3; frame[2] = address >> 8; frame[3] = address;
  frame[4] = count >> 8; frame[5] = count;
  const uint16_t crc = modbus_crc16(frame, 6);
  frame[6] = crc; frame[7] = crc >> 8;
  lua_pushlstring(state, reinterpret_cast<const char *>(frame), sizeof(frame));
  return 1;
}
int lua_util_extract_modbus_frame(lua_State *state) {
  size_t length = 0;
  const auto *data = reinterpret_cast<const uint8_t *>(luaL_checklstring(state, 1, &length));
  const uint8_t unit = luaL_checkinteger(state, 2);
  const uint8_t function = luaL_checkinteger(state, 3);
  const size_t byte_count = luaL_checkinteger(state, 4);
  const size_t expected = byte_count + 5;
  for (size_t index = 0; expected <= length && index <= length - expected; ++index) {
    if (data[index] != unit || data[index + 1] != function || data[index + 2] != byte_count) continue;
    const uint16_t crc = modbus_crc16(data + index, expected - 2);
    if (data[index + expected - 2] == (crc & 0xff) && data[index + expected - 1] == (crc >> 8)) {
      lua_pushlstring(state, reinterpret_cast<const char *>(data + index), expected);
      return 1;
    }
  }
  lua_pushnil(state);
  return 1;
}
int lua_util_decode_bcd_32(lua_State *state) {
  uint32_t value = static_cast<uint32_t>(luaL_checkinteger(state, 1));
  uint32_t output = 0;
  uint32_t multiplier = 1;
  for (int nibble = 0; nibble < 8; ++nibble) {
    const uint32_t digit = (value >> (nibble * 4)) & 0xf;
    if (digit > 9) return luaL_error(state, "invalid BCD digit");
    output += digit * multiplier;
    multiplier *= 10;
  }
  lua_pushinteger(state, output);
  return 1;
}

esp_err_t ensure_global_buffer(size_t capacity) {
  if (!capacity || capacity > kGlobalBufferMaxSize) return ESP_ERR_INVALID_ARG;
  if (global_buffer && global_buffer_capacity == capacity) return ESP_OK;
  auto *replacement = static_cast<uint8_t *>(std::malloc(capacity));
  if (!replacement) return ESP_ERR_NO_MEM;
  std::free(global_buffer);
  global_buffer = replacement;
  global_buffer_capacity = capacity;
  global_buffer_length = 0;
  return ESP_OK;
}

int lua_util_init_global_buffer(lua_State *state) {
  const lua_Integer requested = luaL_optinteger(state, 1, kGlobalBufferDefaultSize);
  const esp_err_t result = requested > 0 && requested <= kGlobalBufferMaxSize
      ? ensure_global_buffer(static_cast<size_t>(requested)) : ESP_ERR_INVALID_ARG;
  if (result == ESP_OK) global_buffer_length = 0;
  return push_result(state, result, result == ESP_ERR_INVALID_ARG ? "invalid global buffer size" : nullptr);
}

int lua_util_append_global_buffer(lua_State *state) {
  size_t length = 0;
  const auto *data = reinterpret_cast<const uint8_t *>(luaL_checklstring(state, 1, &length));
  esp_err_t result = global_buffer ? ESP_OK : ensure_global_buffer(kGlobalBufferDefaultSize);
  if (result == ESP_OK && (!length || length > global_buffer_capacity - global_buffer_length))
    result = ESP_ERR_NO_MEM;
  if (result == ESP_OK) {
    std::memcpy(global_buffer + global_buffer_length, data, length);
    global_buffer_length += length;
  }
  return push_result(state, result, nullptr);
}

int lua_util_write_global_buffer_at(lua_State *state) {
  const lua_Integer position = luaL_checkinteger(state, 1);
  size_t length = 0;
  const auto *data = reinterpret_cast<const uint8_t *>(luaL_checklstring(state, 2, &length));
  esp_err_t result = global_buffer ? ESP_OK : ensure_global_buffer(kGlobalBufferDefaultSize);
  if (result == ESP_OK && (position < 0 || !length ||
      static_cast<size_t>(position) >= global_buffer_capacity ||
      length > global_buffer_capacity - static_cast<size_t>(position))) result = ESP_ERR_NO_MEM;
  if (result == ESP_OK) {
    const size_t offset = static_cast<size_t>(position);
    if (offset > global_buffer_length)
      std::memset(global_buffer + global_buffer_length, 0, offset - global_buffer_length);
    std::memcpy(global_buffer + offset, data, length);
    if (offset + length > global_buffer_length) global_buffer_length = offset + length;
  }
  return push_result(state, result, position < 0 ? "invalid position" : nullptr);
}

void register_functions(lua_State *state) {
  lua_register(state, "i2c_connect", lua_i2c_connect);
  lua_register(state, "i2c_safe_close", lua_i2c_safe_close);
  lua_register(state, "i2c_reset_rx_cursor", lua_i2c_reset_rx_cursor);
  lua_register(state, "i2c_set_rx_size", lua_i2c_set_rx_size);
  lua_register(state, "i2c_write", lua_i2c_write);
  lua_register(state, "i2c_read_chunk", lua_i2c_read_chunk);
  lua_register(state, "i2c_sleep", lua_sleep);
  lua_register(state, "uart_connect", lua_uart_connect);
  lua_register(state, "uart_safe_close", lua_uart_safe_close);
  lua_register(state, "uart_reset_rx_cursor", lua_uart_reset_rx_cursor);
  lua_register(state, "uart_set_rx_size", lua_uart_set_rx_size);
  lua_register(state, "uart_write", lua_uart_write);
  lua_register(state, "uart_read_chunk", lua_uart_read_chunk);
  lua_register(state, "uart_read_frame", lua_uart_read_frame);
  lua_register(state, "uart_sleep", lua_sleep);
  lua_register(state, "rs485_connect", lua_rs485_connect);
  lua_register(state, "rs485_safe_close", lua_rs485_safe_close);
  lua_register(state, "rs485_reset_rx_cursor", lua_rs485_reset_rx_cursor);
  lua_register(state, "rs485_write", lua_rs485_write);
  lua_register(state, "rs485_read_chunk", lua_rs485_read_chunk);
  lua_register(state, "rs485_sleep", lua_sleep);
  lua_register(state, "util_crc8", lua_util_crc8);
  lua_register(state, "util_bytes_to_hex", lua_util_bytes_to_hex);
  lua_register(state, "util_build_read_holding_request", lua_util_build_read_holding_request);
  lua_register(state, "util_extract_modbus_frame", lua_util_extract_modbus_frame);
  lua_register(state, "util_decode_bcd_32", lua_util_decode_bcd_32);
  lua_register(state, "util_init_global_buffer", lua_util_init_global_buffer);
  lua_register(state, "util_append_global_buffer", lua_util_append_global_buffer);
  lua_register(state, "util_write_global_buffer_at", lua_util_write_global_buffer_at);
}

bool parse_sensor(lua_State *state, int index, SensorScriptValue *output) {
  if (!lua_istable(state, index)) return false;
  if (index < 0) index = lua_gettop(state) + index + 1;
  lua_getfield(state, index, "type");
  if (!lua_isinteger(state, -1)) { lua_pop(state, 1); return false; }
  output->type = static_cast<int>(lua_tointeger(state, -1));
  lua_pop(state, 1);
  if (output->type < 1 || output->type > 12) return false;
  int found = 0;
  lua_getfield(state, index, "bool_value");
  if (lua_isboolean(state, -1)) {
    output->kind = SensorScriptValueKind::kBool;
    output->value.bool_value = lua_toboolean(state, -1);
    ++found;
  }
  lua_pop(state, 1);
  lua_getfield(state, index, "int_value");
  if (lua_isinteger(state, -1)) {
    const lua_Integer value = lua_tointeger(state, -1);
    if (value < INT32_MIN || value > INT32_MAX) { lua_pop(state, 1); return false; }
    output->kind = SensorScriptValueKind::kInt;
    output->value.int_value = static_cast<int32_t>(value);
    ++found;
  }
  lua_pop(state, 1);
  lua_getfield(state, index, "float_value");
  if (lua_isnumber(state, -1)) {
    output->kind = SensorScriptValueKind::kFloat;
    output->value.float_value = static_cast<float>(lua_tonumber(state, -1));
    ++found;
  }
  lua_pop(state, 1);
  return found == 1;
}

bool validate_script(const char *source, size_t length, char *error, size_t error_size) {
  lua_State *state = luaL_newstate();
  if (!state) {
    strlcpy(error, "could not create Lua state", error_size);
    return false;
  }
  const int result = luaL_loadbuffer(state, source, length, "sensor_script");
  if (result != LUA_OK) {
    const char *message = lua_tostring(state, -1);
    strlcpy(error, message ? message : "Lua syntax error", error_size);
    ESP_LOGE(kTag, "Lua syntax error: %s", error);
  }
  lua_close(state);
  return result == LUA_OK;
}

bool execute_script(char *error, size_t error_size) {
  if (!active_source || !active_source_length) {
    strlcpy(error, "no sensor script is installed", error_size);
    return false;
  }
  if (!hardware_mutex || xSemaphoreTake(hardware_mutex, portMAX_DELAY) != pdTRUE) {
    strlcpy(error, "sensor hardware lock unavailable", error_size);
    return false;
  }
  global_buffer_length = 0;
  const uint32_t execution_id = ++execution_counter;
  lua_State *state = luaL_newstate();
  if (!state) {
    strlcpy(error, "could not create Lua state", error_size);
    ESP_LOGE(kTag, "%s", error);
    xSemaphoreGive(hardware_mutex);
    return false;
  }
  luaL_openlibs(state);
  register_functions(state);
  int result = luaL_loadbuffer(state, active_source, active_source_length, "sensor_script");
  if (result == LUA_OK) result = lua_pcall(state, 0, 1, 0);
  if (result != LUA_OK) {
    const char *message = lua_tostring(state, -1);
    strlcpy(error, message ? message : "Lua execution failed", error_size);
    ESP_LOGE(kTag, "Lua execution failed: %s", error);
    lua_close(state);
    close_script_interfaces();
    xSemaphoreGive(hardware_mutex);
    return false;
  }
  SensorScriptValue values[9]{};
  size_t count = 0;
  if (lua_istable(state, -1)) {
    SensorScriptValue single{};
    if (parse_sensor(state, -1, &single)) {
      values[count++] = single;
    } else {
      const size_t table_length = lua_rawlen(state, -1);
      for (size_t index = 1; index <= table_length && count < 9; ++index) {
        lua_rawgeti(state, -1, index);
        SensorScriptValue value{};
        if (parse_sensor(state, -1, &value)) values[count++] = value;
        else ESP_LOGW(kTag, "Ignored invalid sensor result at index %u", static_cast<unsigned>(index));
        lua_pop(state, 1);
      }
    }
  }
  lua_close(state);
  close_script_interfaces();
  xSemaphoreGive(hardware_mutex);
  if (count && result_callback) {
    result_callback(values, count);
    ESP_LOGI(kTag, "Lua script produced %u MQTT sensor values", static_cast<unsigned>(count));
  }
  if (global_buffer_length && blob_callback) {
    blob_callback(execution_id, global_buffer, global_buffer_length);
    ESP_LOGI(kTag, "Lua script produced %u-byte global buffer",
             static_cast<unsigned>(global_buffer_length));
  }
  if (count || global_buffer_length) return true;
  {
    strlcpy(error, "script returned no sensor values or global buffer data", error_size);
    ESP_LOGW(kTag, "%s", error);
    return false;
  }
}

esp_err_t save_script(const char *source, size_t length, uint32_t interval) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) return result;
  result = nvs_set_blob(handle, kSourceKey, source, length);
  if (result == ESP_OK) result = nvs_set_u32(handle, kIntervalKey, interval);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result;
}

void replace_active_source(char *source, size_t length, uint32_t interval) {
  std::free(active_source);
  active_source = source;
  active_source_length = length;
  active_interval_seconds = interval;
}

void delete_script() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
  }
  replace_active_source(nullptr, 0, kDefaultIntervalSeconds);
  close_script_interfaces();
  ESP_LOGI(kTag, "Deleted saved sensor script");
}

void process_command(ScriptCommand *command) {
  char error[192]{};
  if (command->action == CommandAction::kDelete) {
    delete_script();
    if (status_callback) status_callback(command->request_id, "deleted", nullptr);
    return;
  }
  if (command->action == CommandAction::kRun) {
    const bool success = execute_script(error, sizeof(error));
    if (status_callback) status_callback(command->request_id, success ? "success" : "failed",
                                         success ? nullptr : error);
    return;
  }
  char *source = command->source;
  size_t length = source ? std::strlen(source) : 0;
  command->source = nullptr;
  if (!validate_script(source, length, error, sizeof(error))) {
    if (status_callback) status_callback(command->request_id, "failed", error);
    std::free(source);
    return;
  }
  const esp_err_t result = save_script(source, length, command->interval_seconds);
  if (result != ESP_OK) {
    ESP_LOGE(kTag, "Could not save script: %s", esp_err_to_name(result));
    if (status_callback) status_callback(command->request_id, "failed", esp_err_to_name(result));
    std::free(source);
    return;
  }
  replace_active_source(source, length, command->interval_seconds);
  ESP_LOGI(kTag, "Installed %u-byte sensor script; interval=%us",
           static_cast<unsigned>(length), static_cast<unsigned>(active_interval_seconds));
  const bool success = execute_script(error, sizeof(error));
  if (status_callback) status_callback(command->request_id, success ? "installed" : "failed",
                                       success ? nullptr : error);
}

void script_task(void *) {
  while (true) {
    ScriptCommand *command = nullptr;
    const TickType_t wait = active_source
        ? pdMS_TO_TICKS(active_interval_seconds * 1000ULL)
        : portMAX_DELAY;
    if (xQueueReceive(command_queue, &command, wait) == pdTRUE) {
      if (command) {
        process_command(command);
        std::free(command->source);
        std::free(command);
      }
    } else if (active_source) {
      char error[192]{};
      if (!execute_script(error, sizeof(error)) && status_callback)
        status_callback("", "failed", error);
    }
  }
}

void load_saved_script() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
  size_t length = 0;
  esp_err_t result = nvs_get_blob(handle, kSourceKey, nullptr, &length);
  uint32_t interval = kDefaultIntervalSeconds;
  if (nvs_get_u32(handle, kIntervalKey, &interval) != ESP_OK || !valid_interval(interval))
    interval = kDefaultIntervalSeconds;
  char *source = result == ESP_OK && length && length <= kMaxScriptSize
      ? static_cast<char *>(std::malloc(length + 1)) : nullptr;
  if (source) result = nvs_get_blob(handle, kSourceKey, source, &length);
  nvs_close(handle);
  if (result == ESP_OK && source) {
    source[length] = '\0';
    char error[192]{};
    if (validate_script(source, length, error, sizeof(error))) replace_active_source(source, length, interval);
    else std::free(source);
  }
}
}  // namespace

esp_err_t sensor_script_init(SensorScriptResultCallback result, SensorScriptStatusCallback status,
                             SensorScriptBlobCallback blob) {
  result_callback = result;
  status_callback = status;
  blob_callback = blob;
  load_saved_script();
  hardware_mutex = xSemaphoreCreateMutex();
  command_queue = xQueueCreate(4, sizeof(ScriptCommand *));
  if (!hardware_mutex || !command_queue) return ESP_ERR_NO_MEM;
  if (xTaskCreate(script_task, "sensor_script", 12288, nullptr, 5, nullptr) != pdPASS)
    return ESP_ERR_NO_MEM;
  ESP_LOGI(kTag, "%s", active_source ? "Saved sensor script loaded" : "No saved sensor script");
  return ESP_OK;
}

bool sensor_script_handle_mqtt_command(const char *topic, size_t topic_length,
                                       const char *payload, size_t payload_length,
                                       const char *project_id, const char *serial) {
  char expected[384]{};
  std::snprintf(expected, sizeof(expected), "projects/%s/devices/%s/commands/script",
                project_id, serial);
  if (topic_length != std::strlen(expected) || std::memcmp(topic, expected, topic_length) != 0)
    return false;
  if (!payload || payload_length == 0 || payload_length > kMaxScriptSize + 2048) {
    ESP_LOGW(kTag, "Rejected empty or oversized script command");
    if (status_callback) status_callback("", "failed", "empty or oversized script command");
    return true;
  }
  cJSON *root = cJSON_ParseWithLength(payload, payload_length);
  cJSON *action_json = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : nullptr;
  cJSON *request_id_json = root ? cJSON_GetObjectItemCaseSensitive(root, "requestId") : nullptr;
  cJSON *interval_json = root ? cJSON_GetObjectItemCaseSensitive(root, "intervalSeconds") : nullptr;
  if (!interval_json && root) interval_json = cJSON_GetObjectItemCaseSensitive(root, "interval");
  cJSON *source_json = root ? cJSON_GetObjectItemCaseSensitive(root, "script") : nullptr;
  ScriptCommand *command = static_cast<ScriptCommand *>(std::calloc(1, sizeof(ScriptCommand)));
  bool valid = command && cJSON_IsString(action_json) && action_json->valuestring;
  if (valid) {
    valid = cJSON_IsString(request_id_json) && request_id_json->valuestring &&
        request_id_json->valuestring[0] &&
        std::strlen(request_id_json->valuestring) < sizeof(command->request_id);
    if (valid) strlcpy(command->request_id, request_id_json->valuestring, sizeof(command->request_id));
    command->interval_seconds = active_interval_seconds;
    if (interval_json && (!cJSON_IsNumber(interval_json) || !valid_interval(interval_json->valuedouble)))
      valid = false;
    else if (interval_json)
      command->interval_seconds = static_cast<uint32_t>(interval_json->valuedouble);
  }
  if (valid && (std::strcmp(action_json->valuestring, "set") == 0 ||
                std::strcmp(action_json->valuestring, "download") == 0)) {
    command->action = CommandAction::kSet;
    const size_t length = cJSON_IsString(source_json) && source_json->valuestring
        ? std::strlen(source_json->valuestring) : 0;
    valid = length > 0 && length <= kMaxScriptSize && cJSON_IsNumber(interval_json) &&
        valid_interval(interval_json->valuedouble);
    if (valid) command->source = strdup(source_json->valuestring);
    valid = valid && command->source;
  } else if (valid && std::strcmp(action_json->valuestring, "run") == 0) {
    command->action = CommandAction::kRun;
  } else if (valid && std::strcmp(action_json->valuestring, "delete") == 0) {
    command->action = CommandAction::kDelete;
  } else {
    valid = false;
  }
  cJSON_Delete(root);
  if (!valid || xQueueSend(command_queue, &command, 0) != pdTRUE) {
    ESP_LOGW(kTag, "%s", valid ? "Script command queue full" : "Rejected invalid script command");
    if (status_callback)
      status_callback(command ? command->request_id : "", "failed",
                      valid ? "script command queue full" : "invalid script command");
    if (command) { std::free(command->source); std::free(command); }
  }
  return true;
}

bool sensor_script_try_acquire_hardware() {
  return hardware_mutex && xSemaphoreTake(hardware_mutex, 0) == pdTRUE;
}

void sensor_script_release_hardware() {
  if (hardware_mutex) xSemaphoreGive(hardware_mutex);
}
