#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "halow_network.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

namespace {
constexpr char kTag[] = "iot_prov";
constexpr char kMqttNamespace[] = "mqtt";
constexpr char kHalowNamespace[] = "halow";
constexpr char kMqttEndpoint[] = "mqtt-config";
constexpr char kHalowConfigEndpoint[] = "halow-config";
constexpr char kMqttBrokerUri[] = "mqtts://mqtt.edgez.ai:8883";
constexpr TickType_t kTemperaturePublishInterval = pdMS_TO_TICKS(30000);
constexpr EventBits_t kNetworkConnected = BIT0;
constexpr EventBits_t kMqttConfigured = BIT1;
constexpr EventBits_t kMqttConnected = BIT2;
constexpr gpio_num_t kUserButton = GPIO_NUM_0;
constexpr int kResetHoldSeconds = 5;

struct MqttConfig {
  char client_id[64];
  char username[37];
  char password[160];
  char project_id[64];
  char channel[81];
};

struct HalowConfig {
  char mesh_id[33];
  char passphrase[64];
  char country[3];
  uint8_t channel;
  bool wifi_upstream;
};

EventGroupHandle_t state_events;
esp_mqtt_client_handle_t mqtt_client;
temperature_sensor_handle_t temperature_sensor;
MqttConfig mqtt_config{};
HalowConfig halow_config{};
char provisioning_name[32]{};
char provisioning_pop[16]{};
char device_serial[24]{};
char device_status[96] = "STARTING";
bool provisioning_active = false;
bool halow_connect_started = false;

void show_device_status(const char *title, const char *status);
void start_mqtt();

void halow_ready() {
  xEventGroupSetBits(state_events, kNetworkConnected);
  show_device_status("HALOW CONNECTED",
                     xEventGroupGetBits(state_events) & kMqttConfigured
                         ? "CONNECTING MQTT"
                         : "MQTT SETUP REQUIRED");
  start_mqtt();
}

void show_device_status(const char *title, const char *status) {
  strlcpy(device_status, status, sizeof(device_status));
  ESP_LOGI(kTag, "%s: %s", title ? title : "DEVICE STATUS", device_status);
}

void show_current_state() {
  if (provisioning_active) {
    char instructions[96]{};
    std::snprintf(instructions, sizeof(instructions), "PAIR %s POP %s",
                  provisioning_name, provisioning_pop);
    show_device_status("PROVISION DEVICE", instructions);
    return;
  }
  const EventBits_t bits = state_events ? xEventGroupGetBits(state_events) : 0;
  if (bits & kMqttConnected) {
    show_device_status("MQTT CONNECTED", device_serial);
  } else if (bits & kNetworkConnected) {
    show_device_status("HALOW CONNECTED",
                       bits & kMqttConfigured ? "CONNECTING MQTT" : "MQTT SETUP REQUIRED");
  } else {
    ESP_LOGI(kTag, "DEVICE STATUS: %s", device_status);
  }
}

bool valid_serial(const char *value) {
  if (!value) return false;
  const size_t length = std::strlen(value);
  if (length < 1 || length > 36) return false;
  for (size_t index = 0; index < length; ++index) {
    const char c = value[index];
    const bool alpha_numeric = (c >= 'A' && c <= 'Z') ||
                               (c >= 'a' && c <= 'z') ||
                               (c >= '0' && c <= '9');
    if (index == 0 && !alpha_numeric) return false;
    if (!alpha_numeric && c != '.' && c != '_' && c != ':' && c != '-') return false;
  }
  return true;
}

bool copy_json_string(cJSON *root, const char *key, char *output, size_t capacity) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsString(item) || !item->valuestring) return false;
  const size_t length = std::strlen(item->valuestring);
  if (length == 0 || length >= capacity) return false;
  std::memcpy(output, item->valuestring, length + 1);
  return true;
}

esp_err_t save_mqtt_config(const MqttConfig &config) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open(kMqttNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) return result;
  const struct { const char *key; const char *value; } values[] = {
      {"client", config.client_id},
      {"username", config.username}, {"password", config.password},
      {"project", config.project_id}, {"channel", config.channel},
  };
  for (const auto &value : values) {
    result = nvs_set_str(handle, value.key, value.value);
    if (result != ESP_OK) break;
  }
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result;
}

bool load_nvs_string(nvs_handle_t handle, const char *key, char *output, size_t capacity) {
  size_t length = capacity;
  return nvs_get_str(handle, key, output, &length) == ESP_OK && output[0] != '\0';
}

bool load_mqtt_config() {
  nvs_handle_t handle;
  if (nvs_open(kMqttNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  const bool loaded =
      load_nvs_string(handle, "client", mqtt_config.client_id, sizeof(mqtt_config.client_id)) &&
      load_nvs_string(handle, "username", mqtt_config.username, sizeof(mqtt_config.username)) &&
      load_nvs_string(handle, "password", mqtt_config.password, sizeof(mqtt_config.password)) &&
      load_nvs_string(handle, "project", mqtt_config.project_id, sizeof(mqtt_config.project_id)) &&
      load_nvs_string(handle, "channel", mqtt_config.channel, sizeof(mqtt_config.channel));
  nvs_close(handle);
  return loaded && valid_serial(mqtt_config.username);
}

esp_err_t save_halow_config(const HalowConfig &config) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open(kHalowNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) return result;
  result = nvs_set_str(handle, "meshId", config.mesh_id);
  if (result == ESP_OK) result = nvs_set_str(handle, "passphrase", config.passphrase);
  if (result == ESP_OK) result = nvs_set_str(handle, "country", config.country);
  if (result == ESP_OK) result = nvs_set_u8(handle, "channel", config.channel);
  if (result == ESP_OK) result = nvs_set_u8(handle, "upstream", config.wifi_upstream ? 1 : 0);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result;
}

bool load_halow_config() {
  nvs_handle_t handle;
  if (nvs_open(kHalowNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  uint8_t wifi_upstream = 0;
  const bool loaded =
      load_nvs_string(handle, "meshId", halow_config.mesh_id, sizeof(halow_config.mesh_id)) &&
      load_nvs_string(handle, "passphrase", halow_config.passphrase, sizeof(halow_config.passphrase)) &&
      load_nvs_string(handle, "country", halow_config.country, sizeof(halow_config.country)) &&
      nvs_get_u8(handle, "channel", &halow_config.channel) == ESP_OK &&
      nvs_get_u8(handle, "upstream", &wifi_upstream) == ESP_OK;
  nvs_close(handle);
  halow_config.wifi_upstream = wifi_upstream != 0;
  return loaded;
}

esp_err_t mqtt_config_handler(uint32_t, const uint8_t *input, ssize_t input_length,
                              uint8_t **output, ssize_t *output_length, void *) {
  if (!input || input_length <= 0 || input_length > 1024 || !output || !output_length) {
    return ESP_ERR_INVALID_ARG;
  }
  cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(input), input_length);
  MqttConfig candidate{};
  bool valid = root &&
      copy_json_string(root, "clientId", candidate.client_id, sizeof(candidate.client_id)) &&
      copy_json_string(root, "username", candidate.username, sizeof(candidate.username)) &&
      copy_json_string(root, "password", candidate.password, sizeof(candidate.password)) &&
      copy_json_string(root, "projectId", candidate.project_id, sizeof(candidate.project_id)) &&
      copy_json_string(root, "channel", candidate.channel, sizeof(candidate.channel));
  valid = valid && valid_serial(candidate.username) &&
          std::strcmp(candidate.username, device_serial) == 0 &&
          std::strchr(candidate.channel, '/') == nullptr;

  esp_err_t result = valid ? save_mqtt_config(candidate) : ESP_ERR_INVALID_ARG;
  if (result == ESP_OK) {
    mqtt_config = candidate;
    xEventGroupSetBits(state_events, kMqttConfigured);
    ESP_LOGI(kTag, "MQTT credential stored for serial %s", mqtt_config.username);
    show_device_status("MQTT CONFIG", "CREDENTIAL STORED");
  } else {
    ESP_LOGW(kTag, "Rejected MQTT provisioning data: %s", esp_err_to_name(result));
  }

  const char *response = result == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"invalid mqtt config\"}";
  *output = static_cast<uint8_t *>(std::malloc(std::strlen(response) + 1));
  if (*output) std::memcpy(*output, response, std::strlen(response) + 1);
  if (!*output) result = ESP_ERR_NO_MEM;
  *output_length = *output ? std::strlen(response) : 0;
  cJSON_Delete(root);
  return result;
}

esp_err_t write_json_response(cJSON *root, uint8_t **output, ssize_t *output_length) {
  if (!root || !output || !output_length) return ESP_ERR_INVALID_ARG;
  char *json = cJSON_PrintUnformatted(root);
  if (!json) return ESP_ERR_NO_MEM;
  *output = reinterpret_cast<uint8_t *>(json);
  *output_length = std::strlen(json);
  return ESP_OK;
}

void connect_halow_task(void *) {
  if (provisioning_active) {
    vTaskDelay(pdMS_TO_TICKS(750));
    network_prov_mgr_stop_provisioning();
  }
  show_device_status("HALOW STATUS", "CONNECTING");
  const esp_err_t result = halow_connect(
      halow_config.mesh_id, halow_config.passphrase,
      halow_config.country, halow_config.channel, halow_ready);
  if (result != ESP_OK) {
    show_device_status("HALOW FAILED", esp_err_to_name(result));
    ESP_LOGE(kTag, "HaLow connect failed: %s", esp_err_to_name(result));
  }
  vTaskDelete(nullptr);
}

esp_err_t start_halow_connection() {
  if (halow_connect_started) return ESP_OK;
  if (xTaskCreate(connect_halow_task, "halow-connect", 6144, nullptr, 5, nullptr) != pdPASS)
    return ESP_ERR_NO_MEM;
  halow_connect_started = true;
  return ESP_OK;
}

esp_err_t halow_config_handler(uint32_t, const uint8_t *input, ssize_t input_length,
                               uint8_t **output, ssize_t *output_length, void *) {
  if (!input || input_length <= 0 || input_length > 512 || !output || !output_length) {
    return ESP_ERR_INVALID_ARG;
  }
  cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(input), input_length);
  HalowConfig candidate{};
  cJSON *passphrase = root ? cJSON_GetObjectItemCaseSensitive(root, "passphrase") : nullptr;
  cJSON *channel = root ? cJSON_GetObjectItemCaseSensitive(root, "channel") : nullptr;
  cJSON *wifi_upstream = root ? cJSON_GetObjectItemCaseSensitive(root, "wifiUpstream") : nullptr;
  const bool valid = root &&
      copy_json_string(root, "meshId", candidate.mesh_id, sizeof(candidate.mesh_id)) &&
      copy_json_string(root, "country", candidate.country, sizeof(candidate.country)) &&
      std::strlen(candidate.country) == 2 &&
      cJSON_IsString(passphrase) && passphrase->valuestring &&
      std::strlen(passphrase->valuestring) >= 8 &&
      std::strlen(passphrase->valuestring) < sizeof(candidate.passphrase) &&
      cJSON_IsNumber(channel) && channel->valuedouble >= 1 && channel->valuedouble <= 255 &&
      channel->valuedouble == channel->valueint &&
      cJSON_IsBool(wifi_upstream) &&
      halow_channel_supported(candidate.country, static_cast<uint8_t>(channel->valueint));
  if (valid) {
    strlcpy(candidate.passphrase, passphrase->valuestring, sizeof(candidate.passphrase));
    candidate.channel = static_cast<uint8_t>(channel->valueint);
    candidate.wifi_upstream = cJSON_IsTrue(wifi_upstream);
  }
  esp_err_t result = valid ? save_halow_config(candidate) : ESP_ERR_INVALID_ARG;
  if (result == ESP_OK) {
    halow_config = candidate;
    if (!candidate.wifi_upstream) result = start_halow_connection();
  }
  cJSON *response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "ok", result == ESP_OK);
  if (result != ESP_OK) cJSON_AddStringToObject(response, "error", esp_err_to_name(result));
  const esp_err_t response_result = write_json_response(response, output, output_length);
  cJSON_Delete(response);
  cJSON_Delete(root);
  return response_result;
}

void make_device_identity() {
  uint8_t mac[6]{};
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
  std::snprintf(device_serial, sizeof(device_serial),
                "%02X%02X%02X%02X%02X%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  std::snprintf(provisioning_name, sizeof(provisioning_name), "PROV_%s", device_serial);
  strlcpy(provisioning_pop, "abcd1234", sizeof(provisioning_pop));
}

void mqtt_event_handler(void *, esp_event_base_t, int32_t, void *);

void temperature_telemetry_task(void *) {
  while (true) {
    xEventGroupWaitBits(state_events, kMqttConnected, pdFALSE, pdTRUE, portMAX_DELAY);

    float temperature_celsius = 0;
    const esp_err_t result = temperature_sensor_get_celsius(temperature_sensor, &temperature_celsius);
    if (result == ESP_OK && (xEventGroupGetBits(state_events) & kMqttConnected)) {
      char topic[384]{};
      char payload[128]{};
      std::snprintf(topic, sizeof(topic),
                    "projects/%s/devices/%s/telemetry/temp",
                    mqtt_config.project_id, mqtt_config.username);
      std::snprintf(payload, sizeof(payload),
                    "{\"temperatureC\":%.2f,\"unit\":\"celsius\",\"sensor\":\"internal\"}",
                    static_cast<double>(temperature_celsius));
      const int message_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
      ESP_LOGI(kTag, "Temperature %.2f C published to %s (%d)",
               static_cast<double>(temperature_celsius), topic, message_id);
    } else if (result != ESP_OK) {
      ESP_LOGW(kTag, "Temperature read failed: %s", esp_err_to_name(result));
    }

    vTaskDelay(kTemperaturePublishInterval);
  }
}

void start_mqtt() {
  if (mqtt_client || !(xEventGroupGetBits(state_events) & kMqttConfigured)) return;
  esp_mqtt_client_config_t config{};
  config.broker.address.uri = kMqttBrokerUri;
  config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  config.credentials.client_id = mqtt_config.client_id;
  config.credentials.username = mqtt_config.username;
  config.credentials.authentication.password = mqtt_config.password;
  mqtt_client = esp_mqtt_client_init(&config);
  if (!mqtt_client) {
    ESP_LOGE(kTag, "Could not create MQTT client");
    return;
  }
  ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt_client));
}

void mqtt_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data) {
  auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);
  if (event_id == MQTT_EVENT_CONNECTED) {
    xEventGroupSetBits(state_events, kMqttConnected);
    show_device_status("MQTT CONNECTED", device_serial);
    char telemetry_topic[384]{};
    char command_topic[384]{};
    std::snprintf(telemetry_topic, sizeof(telemetry_topic),
                  "projects/%s/devices/%s/telemetry/%s",
                  mqtt_config.project_id, mqtt_config.username, mqtt_config.channel);
    std::snprintf(command_topic, sizeof(command_topic),
                  "projects/%s/devices/%s/commands/#",
                  mqtt_config.project_id, mqtt_config.username);
    const int subscription_id = esp_mqtt_client_subscribe(mqtt_client, command_topic, 1);
    const int publish_id = esp_mqtt_client_publish(
        mqtt_client, telemetry_topic, "{\"status\":\"online\"}", 0, 1, 0);
    ESP_LOGI(kTag, "MQTT connected; subscribed %s (%d), published telemetry (%d)",
             command_topic, subscription_id, publish_id);
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    xEventGroupClearBits(state_events, kMqttConnected);
    show_device_status("MQTT STATUS", "DISCONNECTED - RETRYING");
    ESP_LOGW(kTag, "MQTT disconnected");
  } else if (event_id == MQTT_EVENT_DATA && event) {
    const int topic_length = event->topic_len < 300 ? event->topic_len : 300;
    const int data_length = event->data_len < 512 ? event->data_len : 512;
    ESP_LOGI(kTag, "Command received topic=%.*s payload=%.*s",
             topic_length, event->topic, data_length, event->data);
  }
}

void event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && halow_config.wifi_upstream) {
    xEventGroupSetBits(state_events, kNetworkConnected);
    show_device_status("UPSTREAM WI-FI", "CONNECTED - STARTING MQTT");
    start_mqtt();
    return;
  }
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED &&
      halow_config.wifi_upstream && !provisioning_active) {
    ESP_LOGW(kTag, "Upstream Wi-Fi disconnected; retrying");
    esp_wifi_connect();
    return;
  }
  if (event_base == NETWORK_PROV_EVENT) {
    if (event_id == NETWORK_PROV_START) {
      ESP_LOGI(kTag, "BLE provisioning started as %s", provisioning_name);
    } else if (event_id == NETWORK_PROV_WIFI_CRED_RECV) {
      show_device_status("PROVISION DEVICE", "CREDENTIALS RECEIVED - CONNECTING");
    } else if (event_id == NETWORK_PROV_WIFI_CRED_FAIL) {
      show_device_status("PROVISION FAILED", "CHECK WI-FI CREDENTIALS - RETRY");
      network_prov_mgr_reset_wifi_sm_state_on_failure();
    } else if (event_id == NETWORK_PROV_WIFI_CRED_SUCCESS) {
      show_device_status("PROVISION DEVICE", "WI-FI CONNECTED");
    } else if (event_id == NETWORK_PROV_END) {
      provisioning_active = false;
      network_prov_mgr_deinit();
      if (halow_config.wifi_upstream && halow_config.mesh_id[0]) {
        const esp_err_t result = start_halow_connection();
        if (result != ESP_OK) ESP_LOGE(kTag, "Could not start HaLow mesh: %s", esp_err_to_name(result));
      }
    }
    return;
  }
}

void reset_button_task(void *) {
  gpio_config_t config{};
  config.pin_bit_mask = 1ULL << kUserButton;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&config));

  bool pressed = false;
  TickType_t pressed_at = 0;
  int last_remaining = -1;
  while (true) {
    const bool is_pressed = gpio_get_level(kUserButton) == 0;
    if (is_pressed && !pressed) {
      pressed = true;
      pressed_at = xTaskGetTickCount();
      last_remaining = kResetHoldSeconds;
      show_device_status("RESET DEVICE", "KEEP HOLDING 5 SECONDS");
      ESP_LOGI(kTag, "User button pressed; hold for 5 seconds to reset provisioning");
    } else if (is_pressed && pressed) {
      const int held_seconds = static_cast<int>(
          pdTICKS_TO_MS(xTaskGetTickCount() - pressed_at) / 1000);
      const int remaining = kResetHoldSeconds - held_seconds;
      if (remaining > 0 && remaining != last_remaining) {
        last_remaining = remaining;
        char message[40]{};
        std::snprintf(message, sizeof(message), "KEEP HOLDING %d SECONDS", remaining);
        show_device_status("RESET DEVICE", message);
      }
      if (held_seconds >= kResetHoldSeconds) {
        ESP_LOGW(kTag, "Erasing HaLow and MQTT provisioning data from NVS");
        show_device_status("RESET DEVICE", "DATA CLEARED - RESTARTING");
        const esp_err_t result = nvs_flash_erase();
        if (result != ESP_OK) {
          ESP_LOGE(kTag, "Could not erase NVS: %s", esp_err_to_name(result));
          show_device_status("RESET FAILED", esp_err_to_name(result));
          pressed = false;
        } else {
          vTaskDelay(pdMS_TO_TICKS(750));
          esp_restart();
        }
      }
    } else if (!is_pressed && pressed) {
      pressed = false;
      last_remaining = -1;
      ESP_LOGI(kTag, "Provisioning reset cancelled");
      show_current_state();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void initialize_nvs() {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    result = nvs_flash_init();
  }
  ESP_ERROR_CHECK(result);
}
}  // namespace

extern "C" void app_main() {
  ESP_LOGI(kTag, "HT-HC33 provisioning firmware starting");
  initialize_nvs();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  make_device_identity();
  ESP_LOGI(kTag, "Device serial: %s", device_serial);
  state_events = xEventGroupCreate();
  ESP_ERROR_CHECK(state_events ? ESP_OK : ESP_ERR_NO_MEM);
  if (load_mqtt_config()) xEventGroupSetBits(state_events, kMqttConfigured);

  temperature_sensor_config_t temperature_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  ESP_ERROR_CHECK(temperature_sensor_install(&temperature_config, &temperature_sensor));
  ESP_ERROR_CHECK(temperature_sensor_enable(temperature_sensor));
  const BaseType_t task_created = xTaskCreate(
      temperature_telemetry_task, "temperature_telemetry", 4096, nullptr, 5, nullptr);
  ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

  ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event_handler, nullptr));
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
  if (load_halow_config()) {
    show_device_status("HALOW STATUS", "CONNECTING WITH SAVED SETTINGS");
    if (halow_config.wifi_upstream) {
      ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
      ESP_ERROR_CHECK(esp_wifi_start());
      ESP_ERROR_CHECK(esp_wifi_connect());
    }
    ESP_ERROR_CHECK(start_halow_connection());
  } else {
    network_prov_mgr_config_t provisioning_config{};
    provisioning_config.scheme = network_prov_scheme_ble;
    provisioning_config.scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
    ESP_ERROR_CHECK(network_prov_mgr_init(provisioning_config));
    provisioning_active = true;
    char instructions[96]{};
    std::snprintf(instructions, sizeof(instructions), "PAIR %s POP %s",
                  provisioning_name, provisioning_pop);
    show_device_status("PROVISION DEVICE", instructions);
    ESP_LOGI(kTag, "BLE provisioning service %s, PoP %s", provisioning_name, provisioning_pop);
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_create(kMqttEndpoint));
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_create(kHalowConfigEndpoint));
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, provisioning_pop, provisioning_name, nullptr));
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_register(kMqttEndpoint, mqtt_config_handler, nullptr));
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_register(kHalowConfigEndpoint, halow_config_handler, nullptr));
    ESP_LOGI(kTag, "Provision HaLow with endpoints '%s' and '%s'",
             kMqttEndpoint, kHalowConfigEndpoint);
  }

  const BaseType_t reset_task_created = xTaskCreate(
      reset_button_task, "reset-button", 3072, nullptr, 6, nullptr);
  ESP_ERROR_CHECK(reset_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
