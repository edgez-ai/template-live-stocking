#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "cJSON.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "halow_network.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "pb_decode.h"
#include "usb_control.pb.h"

namespace {
constexpr char kTag[] = "iot_prov";
constexpr char kMqttNamespace[] = "mqtt";
constexpr char kHalowNamespace[] = "halow";
constexpr char kLocationNamespace[] = "location";
constexpr char kOtaNamespace[] = "ota";
constexpr char kOtaResultKey[] = "result";
constexpr uint32_t kOtaResultMagic = 0x4f544131;
constexpr char kMqttEndpoint[] = "mqtt-config";
constexpr char kMqttBrokerUri[] = "mqtts://mqtt.edgez.ai:8883";
constexpr TickType_t kGatewayTelemetryInterval = pdMS_TO_TICKS(30000);
constexpr int64_t kTopologyPeerMaxAgeMs = 120000;
constexpr size_t kMaxTopologyPeers = 16;
constexpr size_t kMaxOtaUrlLength = 512;
constexpr size_t kMaxOtaRequestIdLength = 64;
constexpr gpio_num_t kBatteryAdcControl = GPIO_NUM_20;
constexpr adc_channel_t kBatteryAdcChannel = ADC_CHANNEL_0;  // GPIO1 / ADC_IN
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

struct DeviceLocation {
  bool has_location;
  int32_t latitude_e6;
  int32_t longitude_e6;
};

struct BeaconFrame {
  uint16_t length;
  uint8_t data[250];
  uint8_t source_mac[6];
};

struct RemoteBeacon {
  char client_id[37];
  pb_size_t sensor_data_count;
  ai_edgez_halow_SensorData sensor_data[9];
};

struct TopologyPeer {
  bool occupied;
  char client_id[37];
  uint8_t radio_mac[6];
  int16_t rssi_dbm;
  bool rssi_valid;
  int64_t last_seen_ms;
};

struct OtaCommand {
  char url[kMaxOtaUrlLength];
  char request_id[kMaxOtaRequestIdLength];
};

enum class OtaResultState : uint8_t { kNone, kPending, kSucceeded, kFailed };

struct OtaResult {
  uint32_t magic;
  OtaResultState state;
  char request_id[kMaxOtaRequestIdLength];
  char detail[96];
};

EventGroupHandle_t state_events;
esp_mqtt_client_handle_t mqtt_client;
adc_oneshot_unit_handle_t battery_adc;
adc_cali_handle_t battery_calibration;
MqttConfig mqtt_config{};
HalowConfig halow_config{};
DeviceLocation device_location{};
char provisioning_name[32]{};
char provisioning_pop[16]{};
char device_serial[24]{};
char device_status[96] = "STARTING";
bool provisioning_active = false;
bool halow_connect_started = false;
QueueHandle_t beacon_queue;
QueueHandle_t telemetry_queue;
QueueHandle_t ota_queue;
TopologyPeer topology_peers[kMaxTopologyPeers]{};
portMUX_TYPE topology_lock = portMUX_INITIALIZER_UNLOCKED;

void show_device_status(const char *title, const char *status);
void start_mqtt();
esp_err_t configure_mesh(cJSON *root);

bool valid_https_url(const char *url) {
  if (!url || std::strncmp(url, "https://", 8) != 0) return false;
  const size_t length = std::strlen(url);
  if (length <= 8 || length >= kMaxOtaUrlLength) return false;
  for (size_t index = 8; index < length; ++index) {
    if (url[index] <= ' ' || url[index] == '\\') return false;
  }
  return true;
}

esp_err_t save_ota_result(const char *request_id, OtaResultState state, const char *detail = nullptr) {
  if (!request_id || !request_id[0]) return ESP_ERR_INVALID_ARG;
  OtaResult result{};
  result.magic = kOtaResultMagic;
  result.state = state;
  strlcpy(result.request_id, request_id, sizeof(result.request_id));
  if (detail) strlcpy(result.detail, detail, sizeof(result.detail));
  nvs_handle_t handle;
  esp_err_t error = nvs_open(kOtaNamespace, NVS_READWRITE, &handle);
  if (error != ESP_OK) return error;
  error = nvs_set_blob(handle, kOtaResultKey, &result, sizeof(result));
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  return error;
}

bool load_ota_result(OtaResult *result) {
  if (!result) return false;
  nvs_handle_t handle;
  if (nvs_open(kOtaNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t size = sizeof(*result);
  const esp_err_t error = nvs_get_blob(handle, kOtaResultKey, result, &size);
  nvs_close(handle);
  return error == ESP_OK && size == sizeof(*result) && result->magic == kOtaResultMagic &&
      result->request_id[0] && (result->state == OtaResultState::kPending ||
                                result->state == OtaResultState::kSucceeded ||
                                result->state == OtaResultState::kFailed);
}

void clear_ota_result() {
  nvs_handle_t handle;
  if (nvs_open(kOtaNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
  nvs_erase_key(handle, kOtaResultKey);
  nvs_commit(handle);
  nvs_close(handle);
}

bool ota_is_pending() {
  OtaResult result{};
  return load_ota_result(&result) && result.state == OtaResultState::kPending;
}

bool publish_ota_status(const OtaCommand &command, const char *status, const char *detail = nullptr) {
  if (!mqtt_client || !(xEventGroupGetBits(state_events) & kMqttConnected)) return false;
  cJSON *root = cJSON_CreateObject();
  cJSON *ota = cJSON_CreateObject();
  if (!root || !ota) {
    cJSON_Delete(root);
    cJSON_Delete(ota);
    return false;
  }
  cJSON_AddStringToObject(root, "clientId", mqtt_config.client_id);
  cJSON_AddStringToObject(root, "firmwareVersion", esp_app_get_description()->version);
  cJSON_AddStringToObject(ota, "requestId", command.request_id);
  cJSON_AddStringToObject(ota, "status", status);
  if (detail && detail[0]) cJSON_AddStringToObject(ota, "detail", detail);
  cJSON_AddItemToObject(root, "ota", ota);
  char *payload = cJSON_PrintUnformatted(root);
  if (payload) {
    char topic[384]{};
    std::snprintf(topic, sizeof(topic), "projects/%s/devices/%s/telemetry/ota",
                  mqtt_config.project_id, mqtt_config.username);
    const bool queued = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0) >= 0;
    cJSON_free(payload);
    cJSON_Delete(root);
    return queued;
  }
  cJSON_Delete(root);
  return false;
}

void publish_saved_ota_result() {
  OtaResult result{};
  if (!load_ota_result(&result) || result.state == OtaResultState::kPending) return;
  OtaCommand command{};
  strlcpy(command.request_id, result.request_id, sizeof(command.request_id));
  const char *status = result.state == OtaResultState::kSucceeded ? "succeeded" : "failed";
  if (publish_ota_status(command, status, result.detail)) clear_ota_result();
}

void ota_task(void *) {
  OtaCommand command{};
  while (true) {
    if (xQueueReceive(ota_queue, &command, portMAX_DELAY) != pdTRUE) continue;
    ESP_LOGI(kTag, "Starting OTA request %s", command.request_id);
    show_device_status("FIRMWARE UPDATE", "DOWNLOADING");
    publish_ota_status(command, "pending", "downloading");

    esp_http_client_config_t http_config{};
    http_config.url = command.url;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 30000;
    http_config.keep_alive_enable = true;
    esp_https_ota_config_t ota_config{};
    ota_config.http_config = &http_config;
    const esp_err_t result = esp_https_ota(&ota_config);
    if (result == ESP_OK) {
      ESP_LOGI(kTag, "OTA request %s installed; rebooting", command.request_id);
      show_device_status("FIRMWARE UPDATE", "INSTALLED - REBOOTING");
      ESP_ERROR_CHECK(save_ota_result(command.request_id, OtaResultState::kSucceeded, "installed"));
      publish_ota_status(command, "pending", "installed - rebooting");
      vTaskDelay(pdMS_TO_TICKS(1500));
      esp_restart();
    }

    ESP_LOGE(kTag, "OTA request %s failed: %s", command.request_id, esp_err_to_name(result));
    show_device_status("FIRMWARE UPDATE", "FAILED");
    ESP_ERROR_CHECK(save_ota_result(command.request_id, OtaResultState::kFailed, esp_err_to_name(result)));
    publish_saved_ota_result();
  }
}

void handle_ota_command(const esp_mqtt_event_handle_t event) {
  char expected_topic[384]{};
  std::snprintf(expected_topic, sizeof(expected_topic), "projects/%s/devices/%s/commands/ota",
                mqtt_config.project_id, mqtt_config.username);
  const size_t expected_length = std::strlen(expected_topic);
  if (event->topic_len != static_cast<int>(expected_length) ||
      std::memcmp(event->topic, expected_topic, expected_length) != 0) return;
  if (event->current_data_offset != 0 || event->data_len != event->total_data_len ||
      event->data_len <= 0 || event->data_len > 1024) {
    ESP_LOGW(kTag, "Rejected fragmented or oversized OTA command");
    return;
  }

  cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
  cJSON *url = root ? cJSON_GetObjectItemCaseSensitive(root, "url") : nullptr;
  cJSON *request_id = root ? cJSON_GetObjectItemCaseSensitive(root, "requestId") : nullptr;
  OtaCommand command{};
  const bool valid = cJSON_IsString(url) && url->valuestring && valid_https_url(url->valuestring) &&
      cJSON_IsString(request_id) && request_id->valuestring && request_id->valuestring[0] &&
      std::strlen(request_id->valuestring) < sizeof(command.request_id);
  if (valid) {
    strlcpy(command.url, url->valuestring, sizeof(command.url));
    strlcpy(command.request_id, request_id->valuestring, sizeof(command.request_id));
  }
  cJSON_Delete(root);
  if (!valid) {
    ESP_LOGW(kTag, "Rejected invalid OTA command");
    return;
  }
  if (ota_is_pending()) {
    ESP_LOGW(kTag, "Ignored OTA request %s because an update is already queued", command.request_id);
    publish_ota_status(command, "busy");
    return;
  }
  const esp_err_t stored = save_ota_result(command.request_id, OtaResultState::kPending, "accepted");
  if (stored != ESP_OK) {
    ESP_LOGE(kTag, "Could not persist OTA request %s: %s", command.request_id, esp_err_to_name(stored));
    publish_ota_status(command, "failed", "could not persist OTA request");
    return;
  }
  if (xQueueSend(ota_queue, &command, 0) != pdTRUE) {
    clear_ota_result();
    ESP_LOGW(kTag, "Ignored OTA request %s because an update is already queued", command.request_id);
    publish_ota_status(command, "busy");
    return;
  }
  publish_ota_status(command, "pending", "accepted");
}

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

esp_err_t save_device_location(const DeviceLocation &location) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open(kLocationNamespace, NVS_READWRITE, &handle);
  if (result != ESP_OK) return result;
  result = nvs_set_u8(handle, "enabled", location.has_location ? 1 : 0);
  if (result == ESP_OK && location.has_location) result = nvs_set_i32(handle, "latitude", location.latitude_e6);
  if (result == ESP_OK && location.has_location) result = nvs_set_i32(handle, "longitude", location.longitude_e6);
  if (result == ESP_OK) result = nvs_commit(handle);
  nvs_close(handle);
  return result;
}

void load_device_location() {
  nvs_handle_t handle;
  if (nvs_open(kLocationNamespace, NVS_READONLY, &handle) != ESP_OK) return;
  uint8_t enabled = 0;
  device_location.has_location =
      nvs_get_u8(handle, "enabled", &enabled) == ESP_OK && enabled != 0 &&
      nvs_get_i32(handle, "latitude", &device_location.latitude_e6) == ESP_OK &&
      nvs_get_i32(handle, "longitude", &device_location.longitude_e6) == ESP_OK;
  nvs_close(handle);
}

void enqueue_beacon(const uint8_t *data, size_t length, const uint8_t source_mac[6]) {
  if (!beacon_queue || !data || !source_mac || length == 0 || length > sizeof(BeaconFrame::data)) return;
  BeaconFrame frame{};
  frame.length = length;
  std::memcpy(frame.data, data, length);
  std::memcpy(frame.source_mac, source_mac, sizeof(frame.source_mac));
  if (xQueueSend(beacon_queue, &frame, 0) != pdTRUE) {
    ESP_LOGW(kTag, "Raw HaLow beacon queue full; record dropped");
  }
}

void remember_topology_peer(const char *client_id, const uint8_t radio_mac[6]) {
  if (!client_id || !client_id[0] || !radio_mac) return;
  int16_t rssi_dbm = 0;
  const bool rssi_valid = halow_get_peer_rssi(radio_mac, &rssi_dbm);
  const int64_t now_ms = esp_timer_get_time() / 1000;
  portENTER_CRITICAL(&topology_lock);
  TopologyPeer *slot = nullptr;
  TopologyPeer *oldest = &topology_peers[0];
  for (auto &peer : topology_peers) {
    if (peer.occupied && std::strcmp(peer.client_id, client_id) == 0) {
      slot = &peer;
      break;
    }
    if (!peer.occupied && !slot) slot = &peer;
    if (peer.last_seen_ms < oldest->last_seen_ms) oldest = &peer;
  }
  if (!slot) slot = oldest;
  slot->occupied = true;
  strlcpy(slot->client_id, client_id, sizeof(slot->client_id));
  std::memcpy(slot->radio_mac, radio_mac, sizeof(slot->radio_mac));
  slot->rssi_dbm = rssi_dbm;
  slot->rssi_valid = rssi_valid;
  slot->last_seen_ms = now_ms;
  portEXIT_CRITICAL(&topology_lock);
}

void append_topology(cJSON *entry) {
  TopologyPeer snapshot[kMaxTopologyPeers]{};
  portENTER_CRITICAL(&topology_lock);
  std::memcpy(snapshot, topology_peers, sizeof(snapshot));
  portEXIT_CRITICAL(&topology_lock);

  cJSON *topology = cJSON_CreateObject();
  cJSON *links = cJSON_CreateArray();
  if (!topology || !links) {
    cJSON_Delete(topology);
    cJSON_Delete(links);
    return;
  }
  cJSON_AddItemToObject(topology, "links", links);
  const int64_t now_ms = esp_timer_get_time() / 1000;
  for (const auto &peer : snapshot) {
    const int64_t age_ms = now_ms - peer.last_seen_ms;
    if (!peer.occupied || age_ms < 0 || age_ms > kTopologyPeerMaxAgeMs) continue;
    cJSON *link = cJSON_CreateObject();
    if (!link) continue;
    char radio_mac[18]{};
    std::snprintf(radio_mac, sizeof(radio_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                  peer.radio_mac[0], peer.radio_mac[1], peer.radio_mac[2],
                  peer.radio_mac[3], peer.radio_mac[4], peer.radio_mac[5]);
    cJSON_AddStringToObject(link, "peerId", peer.client_id);
    cJSON_AddStringToObject(link, "peerRadioMac", radio_mac);
    cJSON_AddNumberToObject(link, "ageMs", static_cast<double>(age_ms));
    if (peer.rssi_valid) cJSON_AddNumberToObject(link, "rssi", peer.rssi_dbm);
    cJSON_AddItemToArray(links, link);
  }
  cJSON_AddItemToObject(entry, "topology", topology);
}

void decode_remote_beacon(const BeaconFrame &frame) {
  ai_edgez_halow_Beacon beacon = ai_edgez_halow_Beacon_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(frame.data, frame.length);
  if (!pb_decode(&stream, ai_edgez_halow_Beacon_fields, &beacon) ||
      (beacon.user_id_high == 0 && beacon.user_id_low == 0)) return;

  RemoteBeacon reading{};
  std::snprintf(reading.client_id, sizeof(reading.client_id),
                "%08llx-%04llx-%04llx-%04llx-%012llx",
                static_cast<unsigned long long>(beacon.user_id_high >> 32),
                static_cast<unsigned long long>((beacon.user_id_high >> 16) & 0xffff),
                static_cast<unsigned long long>(beacon.user_id_high & 0xffff),
                static_cast<unsigned long long>(beacon.user_id_low >> 48),
                static_cast<unsigned long long>(beacon.user_id_low & 0xffffffffffffULL));
  if (std::strcmp(reading.client_id, mqtt_config.client_id) == 0) return;
  remember_topology_peer(reading.client_id, frame.source_mac);
  float latitude = beacon.latitude;
  float longitude = beacon.longitude;
  bool has_latitude = false;
  bool has_longitude = false;
  for (pb_size_t i = 0; i < beacon.sensor_data_count; ++i) {
    const auto &sensor = beacon.sensor_data[i];
    if (sensor.which_value == ai_edgez_halow_SensorData_float_value_tag &&
        std::isfinite(sensor.value.float_value)) {
      if (sensor.type == ai_edgez_halow_SensorType_SENSOR_LATITUDE) {
        latitude = sensor.value.float_value;
        has_latitude = true;
      } else if (sensor.type == ai_edgez_halow_SensorType_SENSOR_LONGITUDE) {
        longitude = sensor.value.float_value;
        has_longitude = true;
      }
    }
  }
  if (has_latitude == has_longitude && std::isfinite(latitude) &&
      std::isfinite(longitude) && latitude >= -90.0f && latitude <= 90.0f &&
      longitude >= -180.0f && longitude <= 180.0f &&
      (latitude != 0.0f || longitude != 0.0f)) {
    auto &lat = reading.sensor_data[reading.sensor_data_count++];
    lat.type = ai_edgez_halow_SensorType_SENSOR_LATITUDE;
    lat.which_value = ai_edgez_halow_SensorData_float_value_tag;
    lat.value.float_value = latitude;
    auto &lon = reading.sensor_data[reading.sensor_data_count++];
    lon.type = ai_edgez_halow_SensorType_SENSOR_LONGITUDE;
    lon.which_value = ai_edgez_halow_SensorData_float_value_tag;
    lon.value.float_value = longitude;
  }
  for (pb_size_t i = 0; i < beacon.sensor_data_count &&
                         reading.sensor_data_count < 9; ++i) {
    const auto &sensor = beacon.sensor_data[i];
    if (sensor.type == ai_edgez_halow_SensorType_SENSOR_LATITUDE ||
        sensor.type == ai_edgez_halow_SensorType_SENSOR_LONGITUDE) continue;
    reading.sensor_data[reading.sensor_data_count++] = sensor;
  }

  if (xQueueSend(telemetry_queue, &reading, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(kTag, "Remote telemetry queue full; beacon record dropped");
  }
}

void remote_beacon_task(void *) {
  BeaconFrame frame{};
  while (true) {
    if (xQueueReceive(beacon_queue, &frame, portMAX_DELAY) == pdTRUE)
      decode_remote_beacon(frame);
  }
}

void append_remote_telemetry(cJSON *batch, const RemoteBeacon *records, size_t count) {
  for (size_t record_index = 0; record_index < count; ++record_index) {
    const auto &reading = records[record_index];
    cJSON *entry = cJSON_CreateObject();
    if (!entry) continue;
    cJSON_AddStringToObject(entry, "clientId", reading.client_id);
    cJSON_AddStringToObject(entry, "status", "online");
    if (std::strcmp(reading.client_id, mqtt_config.client_id) == 0) {
      cJSON_AddStringToObject(entry, "firmwareVersion", esp_app_get_description()->version);
      append_topology(entry);
    }
    cJSON *sensors = cJSON_CreateArray();
    if (sensors) cJSON_AddItemToObject(entry, "sensors", sensors);
    for (pb_size_t i = 0; i < reading.sensor_data_count; ++i) {
      const auto &sensor = reading.sensor_data[i];
      cJSON *value = nullptr;
      switch (sensor.which_value) {
        case ai_edgez_halow_SensorData_bool_value_tag:
          value = cJSON_CreateBool(sensor.value.bool_value);
          break;
        case ai_edgez_halow_SensorData_int_value_tag:
          value = cJSON_CreateNumber(sensor.value.int_value);
          break;
        case ai_edgez_halow_SensorData_float_value_tag:
          if (std::isfinite(sensor.value.float_value))
            value = cJSON_CreateNumber(sensor.value.float_value);
          break;
        default: break;
      }
      if (!value) continue;
      if (sensors) {
        cJSON *record = cJSON_CreateObject();
        if (record) {
          cJSON_AddNumberToObject(record, "type", sensor.type);
          cJSON_AddItemToObject(record, "value", value);
          cJSON_AddItemToArray(sensors, record);
        } else cJSON_Delete(value);
      } else cJSON_Delete(value);
    }
    cJSON_AddItemToArray(batch, entry);
  }
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
  if (result == ESP_OK) result = configure_mesh(root);
  if (result == ESP_OK) {
    mqtt_config = candidate;
    xEventGroupSetBits(state_events, kMqttConfigured);
    ESP_LOGI(kTag, "MQTT, mesh, and location configuration stored for serial %s", mqtt_config.username);
    show_device_status("DEVICE CONFIG", "CREDENTIALS STORED");
  } else {
    ESP_LOGW(kTag, "Rejected device provisioning data: %s", esp_err_to_name(result));
  }

  const char *response = result == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"invalid device config\"}";
  *output = static_cast<uint8_t *>(std::malloc(std::strlen(response) + 1));
  if (*output) std::memcpy(*output, response, std::strlen(response) + 1);
  if (!*output) result = ESP_ERR_NO_MEM;
  *output_length = *output ? std::strlen(response) : 0;
  cJSON_Delete(root);
  return result;
}

void connect_halow_task(void *) {
  if (provisioning_active) {
    vTaskDelay(pdMS_TO_TICKS(750));
    network_prov_mgr_stop_provisioning();
  }
  show_device_status("HALOW STATUS", "CONNECTING");
  const esp_err_t result = halow_connect(
      halow_config.mesh_id, halow_config.passphrase,
      halow_config.country, halow_config.channel,
      halow_config.wifi_upstream, halow_ready);
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

esp_err_t configure_mesh(cJSON *root) {
  HalowConfig candidate{};
  cJSON *passphrase = root ? cJSON_GetObjectItemCaseSensitive(root, "passphrase") : nullptr;
  cJSON *channel = root ? cJSON_GetObjectItemCaseSensitive(root, "halowChannel") : nullptr;
  cJSON *wifi_upstream = root ? cJSON_GetObjectItemCaseSensitive(root, "wifiUpstream") : nullptr;
  cJSON *latitude = root ? cJSON_GetObjectItemCaseSensitive(root, "latitude") : nullptr;
  cJSON *longitude = root ? cJSON_GetObjectItemCaseSensitive(root, "longitude") : nullptr;
  const bool location_provided = latitude || longitude;
  const bool clear_location = cJSON_IsNull(latitude) && cJSON_IsNull(longitude);
  const bool coordinates_valid = cJSON_IsNumber(latitude) && cJSON_IsNumber(longitude) &&
      std::isfinite(latitude->valuedouble) && std::isfinite(longitude->valuedouble) &&
      latitude->valuedouble >= -90 && latitude->valuedouble <= 90 &&
      longitude->valuedouble >= -180 && longitude->valuedouble <= 180;
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
      halow_channel_supported(candidate.country, static_cast<uint8_t>(channel->valueint)) &&
      (!location_provided || clear_location || coordinates_valid);
  if (valid) {
    strlcpy(candidate.passphrase, passphrase->valuestring, sizeof(candidate.passphrase));
    candidate.channel = static_cast<uint8_t>(channel->valueint);
    candidate.wifi_upstream = cJSON_IsTrue(wifi_upstream);
  }
  DeviceLocation new_location{};
  if (coordinates_valid) {
    new_location.has_location = true;
    new_location.latitude_e6 = static_cast<int32_t>(std::lround(latitude->valuedouble * 1000000));
    new_location.longitude_e6 = static_cast<int32_t>(std::lround(longitude->valuedouble * 1000000));
  }
  esp_err_t result = valid ? save_halow_config(candidate) : ESP_ERR_INVALID_ARG;
  if (result == ESP_OK && location_provided) result = save_device_location(new_location);
  if (result == ESP_OK) {
    halow_config = candidate;
    if (location_provided) device_location = new_location;
    if (!candidate.wifi_upstream) result = start_halow_connection();
  }
  return result;
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

esp_err_t read_battery_millivolts(int *battery_mv) {
  // HT-HC33: GPIO20 enables the battery divider, and GPIO1 reads its midpoint.
  // R17 and R28 are both 100 kOhm, so VBAT is twice the calibrated ADC voltage.
  gpio_set_level(kBatteryAdcControl, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  int sum_mv = 0;
  esp_err_t result = ESP_OK;
  for (int sample = 0; sample < 16; ++sample) {
    int raw = 0;
    int adc_mv = 0;
    result = adc_oneshot_read(battery_adc, kBatteryAdcChannel, &raw);
    if (result == ESP_OK) result = adc_cali_raw_to_voltage(battery_calibration, raw, &adc_mv);
    if (result != ESP_OK) break;
    sum_mv += adc_mv;
  }
  gpio_set_level(kBatteryAdcControl, 0);
  if (result == ESP_OK) *battery_mv = (sum_mv / 16) * 2;
  return result;
}

void enqueue_gateway_telemetry() {
  if (!(xEventGroupGetBits(state_events) & kMqttConfigured)) return;

  RemoteBeacon reading{};
  strlcpy(reading.client_id, mqtt_config.client_id, sizeof(reading.client_id));
  int battery_mv = 0;
  const esp_err_t result = read_battery_millivolts(&battery_mv);
  if (result == ESP_OK && battery_mv >= 2500 && battery_mv <= 5000) {
    auto &battery = reading.sensor_data[reading.sensor_data_count++];
    battery.type = ai_edgez_halow_SensorType_SENSOR_BATTERY_VOLTAGE;
    battery.which_value = ai_edgez_halow_SensorData_float_value_tag;
    battery.value.float_value = battery_mv / 1000.0f;
  } else if (result != ESP_OK) {
    ESP_LOGW(kTag, "Battery ADC read failed: %s", esp_err_to_name(result));
  } else {
    ESP_LOGW(kTag, "Battery voltage %d mV outside expected range; skipping telemetry", battery_mv);
  }
  if (device_location.has_location && reading.sensor_data_count + 2 <= 9) {
    auto &latitude = reading.sensor_data[reading.sensor_data_count++];
    latitude.type = ai_edgez_halow_SensorType_SENSOR_LATITUDE;
    latitude.which_value = ai_edgez_halow_SensorData_float_value_tag;
    latitude.value.float_value = device_location.latitude_e6 / 1000000.0f;
    auto &longitude = reading.sensor_data[reading.sensor_data_count++];
    longitude.type = ai_edgez_halow_SensorType_SENSOR_LONGITUDE;
    longitude.which_value = ai_edgez_halow_SensorData_float_value_tag;
    longitude.value.float_value = device_location.longitude_e6 / 1000000.0f;
  }
  if (xQueueSend(telemetry_queue, &reading, pdMS_TO_TICKS(100)) != pdTRUE)
    ESP_LOGW(kTag, "Gateway telemetry queue full; record dropped");
}

void gateway_telemetry_task(void *) {
  while (true) {
    enqueue_gateway_telemetry();
    vTaskDelay(kGatewayTelemetryInterval);
  }
}

void telemetry_publish_task(void *) {
  while (true) {
    xEventGroupWaitBits(state_events, kMqttConnected, pdFALSE, pdTRUE, portMAX_DELAY);
    RemoteBeacon remote{};
    if (xQueueReceive(telemetry_queue, &remote, portMAX_DELAY) != pdTRUE) continue;
    if (!(xEventGroupGetBits(state_events) & kMqttConnected)) continue;

    cJSON *batch = cJSON_CreateArray();
    if (!batch) {
      cJSON_Delete(batch);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    append_remote_telemetry(batch, &remote, 1);
    char *payload = cJSON_PrintUnformatted(batch);
    int message_id = -1;
    if (payload && (xEventGroupGetBits(state_events) & kMqttConnected)) {
      char topic[384]{};
      std::snprintf(topic, sizeof(topic),
                    "projects/%s/devices/%s/telemetry/%s",
                    mqtt_config.project_id, mqtt_config.username, mqtt_config.channel);
      message_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
      if (message_id >= 0)
        ESP_LOGI(kTag, "Telemetry published to %s (%d)", topic, message_id);
    }
    cJSON_free(payload);
    cJSON_Delete(batch);
    if (message_id < 0) vTaskDelay(pdMS_TO_TICKS(1000));
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
    char command_topic[384]{};
    std::snprintf(command_topic, sizeof(command_topic),
                  "projects/%s/devices/%s/commands/#",
                  mqtt_config.project_id, mqtt_config.username);
    const int subscription_id = esp_mqtt_client_subscribe(mqtt_client, command_topic, 1);
    ESP_LOGI(kTag, "MQTT connected; subscribed %s (%d)", command_topic, subscription_id);
    publish_saved_ota_result();
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    xEventGroupClearBits(state_events, kMqttConnected);
    show_device_status("MQTT STATUS", "DISCONNECTED - RETRYING");
    ESP_LOGW(kTag, "MQTT disconnected");
  } else if (event_id == MQTT_EVENT_DATA && event) {
    const int topic_length = event->topic_len < 300 ? event->topic_len : 300;
    const int data_length = event->data_len < 512 ? event->data_len : 512;
    ESP_LOGI(kTag, "Command received topic=%.*s payload=%.*s",
             topic_length, event->topic, data_length, event->data);
    handle_ota_command(event);
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
        ESP_LOGW(kTag, "Erasing HaLow, MQTT, and location provisioning data from NVS");
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

void recover_interrupted_ota() {
  OtaResult result{};
  if (load_ota_result(&result) && result.state == OtaResultState::kPending) {
    ESP_LOGW(kTag, "OTA request %s was interrupted before completion", result.request_id);
    ESP_ERROR_CHECK(save_ota_result(result.request_id, OtaResultState::kFailed, "interrupted before completion"));
  }
}
}  // namespace

extern "C" void app_main() {
  ESP_LOGI(kTag, "HT-HC33 provisioning firmware starting");
  initialize_nvs();
  recover_interrupted_ota();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  make_device_identity();
  ESP_LOGI(kTag, "Device serial: %s", device_serial);
  state_events = xEventGroupCreate();
  ESP_ERROR_CHECK(state_events ? ESP_OK : ESP_ERR_NO_MEM);
  beacon_queue = xQueueCreate(16, sizeof(BeaconFrame));
  telemetry_queue = xQueueCreate(32, sizeof(RemoteBeacon));
  ota_queue = xQueueCreate(1, sizeof(OtaCommand));
  ESP_ERROR_CHECK(beacon_queue && telemetry_queue && ota_queue ? ESP_OK : ESP_ERR_NO_MEM);
  halow_set_beacon_callback(enqueue_beacon);
  ESP_ERROR_CHECK(xTaskCreate(remote_beacon_task, "remote_beacons", 4096, nullptr, 5, nullptr) == pdPASS
                      ? ESP_OK : ESP_ERR_NO_MEM);
  if (load_mqtt_config()) xEventGroupSetBits(state_events, kMqttConfigured);
  load_device_location();

  gpio_config_t battery_control{};
  battery_control.pin_bit_mask = 1ULL << kBatteryAdcControl;
  battery_control.mode = GPIO_MODE_OUTPUT;
  ESP_ERROR_CHECK(gpio_config(&battery_control));
  ESP_ERROR_CHECK(gpio_set_level(kBatteryAdcControl, 0));
  adc_oneshot_unit_init_cfg_t battery_adc_config{};
  battery_adc_config.unit_id = ADC_UNIT_1;
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&battery_adc_config, &battery_adc));
  adc_oneshot_chan_cfg_t battery_channel_config{};
  battery_channel_config.bitwidth = ADC_BITWIDTH_DEFAULT;
  battery_channel_config.atten = ADC_ATTEN_DB_12;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(battery_adc, kBatteryAdcChannel, &battery_channel_config));
  adc_cali_curve_fitting_config_t calibration_config{};
  calibration_config.unit_id = ADC_UNIT_1;
  calibration_config.chan = kBatteryAdcChannel;
  calibration_config.atten = ADC_ATTEN_DB_12;
  calibration_config.bitwidth = ADC_BITWIDTH_DEFAULT;
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&calibration_config, &battery_calibration));
  const BaseType_t gateway_task_created = xTaskCreate(
      gateway_telemetry_task, "gateway_telemetry", 4096, nullptr, 5, nullptr);
  const BaseType_t publish_task_created = xTaskCreate(
      telemetry_publish_task, "telemetry_publish", 4096, nullptr, 5, nullptr);
  ESP_ERROR_CHECK(gateway_task_created == pdPASS && publish_task_created == pdPASS
                      ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(xTaskCreate(ota_task, "ota", 8192, nullptr, 6, nullptr) == pdPASS
                      ? ESP_OK : ESP_ERR_NO_MEM);

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
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, provisioning_pop, provisioning_name, nullptr));
    ESP_ERROR_CHECK(network_prov_mgr_endpoint_register(kMqttEndpoint, mqtt_config_handler, nullptr));
    ESP_LOGI(kTag, "Provision MQTT, mesh, and location with endpoint '%s'", kMqttEndpoint);
  }

  const BaseType_t reset_task_created = xTaskCreate(
      reset_button_task, "reset-button", 3072, nullptr, 6, nullptr);
  ESP_ERROR_CHECK(reset_task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
