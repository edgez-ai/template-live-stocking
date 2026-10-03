#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <strings.h>

#include "cJSON.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "halow_network.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_bt.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "mqtt_l2_relay.h"
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
#include "mbedtls/error.h"
#include "mbedtls/base64.h"
#include "mbedtls/x509_crt.h"
#include "ota_proxy_cert.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "sensor_script.h"
#include "device_system.pb.h"
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
constexpr size_t kBeaconQueueDepth = 8;
constexpr size_t kTelemetryQueueDepth = 8;
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
  int16_t rssi_dbm;
  bool rssi_valid;
};

struct RemoteBeacon {
  char client_id[37];
  uint8_t halow_mac[6];
  bool halow_mac_valid;
  int16_t observer_rssi_dbm;
  bool observer_rssi_valid;
  int64_t observed_at_ms;
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
bool restart_after_provisioning = false;
bool halow_connect_started = false;
bool mqtt_relay_started = false;
QueueHandle_t beacon_queue;
QueueHandle_t telemetry_queue;
QueueHandle_t ota_queue;
TopologyPeer topology_peers[kMaxTopologyPeers]{};
portMUX_TYPE topology_lock = portMUX_INITIALIZER_UNLOCKED;

void show_device_status(const char *title, const char *status);
void start_mqtt();
esp_err_t configure_mesh(cJSON *root);
void handle_ota_command(const esp_mqtt_event_handle_t event);

int publish_mqtt_direct(const char *topic, const void *payload, size_t length,
                        int qos, bool retain) {
  if (!mqtt_client || !(xEventGroupGetBits(state_events) & kMqttConnected)) return -1;
  char gateway_status_topic[384]{};
  if (!topic || !topic[0] || std::strcmp(topic, "system/status") == 0 ||
      std::strcmp(topic, "telemetry/sensors") == 0) {
    const char *suffix = topic && std::strcmp(topic, "telemetry/sensors") == 0
        ? "telemetry/sensors" : "system/status";
    std::snprintf(gateway_status_topic, sizeof(gateway_status_topic),
                  "projects/%s/devices/%s/%s",
                  mqtt_config.project_id, mqtt_config.username, suffix);
    topic = gateway_status_topic;
  }
  return esp_mqtt_client_publish(mqtt_client, topic,
                                 static_cast<const char *>(payload), length,
                                 qos, retain);
}

void receive_relayed_mqtt_command(const char *topic, const uint8_t *payload,
                                  size_t length) {
  if (!topic || (!payload && length)) return;
  char local_topic[384]{};
  if (std::strcmp(topic, "system/commands") == 0) {
    std::snprintf(local_topic, sizeof(local_topic),
                  "projects/%s/devices/%s/system/commands",
                  mqtt_config.project_id, mqtt_config.username);
    topic = local_topic;
  }
  const int topic_length = std::strlen(topic);
  if (sensor_script_handle_mqtt_command(
          topic, topic_length, reinterpret_cast<const char *>(payload), length,
          mqtt_config.project_id, mqtt_config.username)) return;
  esp_mqtt_event_t event{};
  event.topic = const_cast<char *>(topic);
  event.topic_len = topic_length;
  event.data = reinterpret_cast<char *>(const_cast<uint8_t *>(payload));
  event.data_len = length;
  event.total_data_len = length;
  event.current_data_offset = 0;
  handle_ota_command(&event);
}

esp_err_t ensure_mqtt_relay() {
  if (mqtt_relay_started) return ESP_OK;
  const esp_err_t result = mqtt_l2_relay_init(
      halow_config.wifi_upstream, device_serial, publish_mqtt_direct,
      receive_relayed_mqtt_command);
  if (result == ESP_OK) {
    mqtt_relay_started = true;
    halow_set_batman_callback(mqtt_l2_relay_receive);
  }
  return result;
}

void enqueue_script_telemetry(const SensorScriptValue *values, size_t count) {
  if (!values || !count || !telemetry_queue || !mqtt_config.client_id[0]) return;
  RemoteBeacon reading{};
  strlcpy(reading.client_id, mqtt_config.client_id, sizeof(reading.client_id));
  for (size_t index = 0; index < count && reading.sensor_data_count < 9; ++index) {
    auto &sensor = reading.sensor_data[reading.sensor_data_count++];
    sensor.type = static_cast<ai_edgez_halow_SensorType>(values[index].type);
    switch (values[index].kind) {
      case SensorScriptValueKind::kBool:
        sensor.which_value = ai_edgez_halow_SensorData_bool_value_tag;
        sensor.value.bool_value = values[index].value.bool_value;
        break;
      case SensorScriptValueKind::kInt:
        sensor.which_value = ai_edgez_halow_SensorData_int_value_tag;
        sensor.value.int_value = values[index].value.int_value;
        break;
      case SensorScriptValueKind::kFloat:
        sensor.which_value = ai_edgez_halow_SensorData_float_value_tag;
        sensor.value.float_value = values[index].value.float_value;
        break;
    }
  }
  if (xQueueSend(telemetry_queue, &reading, pdMS_TO_TICKS(100)) != pdTRUE)
    ESP_LOGW(kTag, "Sensor script telemetry queue full; record dropped");
}

void publish_script_status(const char *request_id, const char *status, const char *error) {
  if (!mqtt_client || !(xEventGroupGetBits(state_events) & kMqttConnected)) return;
  cJSON *root = cJSON_CreateObject();
  if (!root) return;
  if (request_id && request_id[0]) cJSON_AddStringToObject(root, "requestId", request_id);
  cJSON_AddStringToObject(root, "status", status ? status : "failed");
  if (error && error[0]) cJSON_AddStringToObject(root, "error", error);
  cJSON_AddStringToObject(root, "firmwareVersion", esp_app_get_description()->version);
  char *payload = cJSON_PrintUnformatted(root);
  if (payload) {
    char topic[384]{};
    std::snprintf(topic, sizeof(topic), "projects/%s/devices/%s/telemetry/script",
                  mqtt_config.project_id, mqtt_config.username);
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
    cJSON_free(payload);
  }
  cJSON_Delete(root);
}

void publish_script_blob(uint32_t execution_id, const uint8_t *data, size_t length) {
  if (!data || !length || !mqtt_client ||
      !(xEventGroupGetBits(state_events) & kMqttConnected)) return;
  constexpr size_t kRawChunkSize = 1536;
  constexpr size_t kEncodedChunkSize = ((kRawChunkSize + 2) / 3) * 4;
  const size_t chunk_count = (length + kRawChunkSize - 1) / kRawChunkSize;
  char encoded[kEncodedChunkSize + 1]{};
  char topic[384]{};
  std::snprintf(topic, sizeof(topic), "projects/%s/devices/%s/telemetry/script/blob",
                mqtt_config.project_id, mqtt_config.username);
  for (size_t chunk = 0; chunk < chunk_count; ++chunk) {
    const size_t offset = chunk * kRawChunkSize;
    const size_t raw_length = length - offset < kRawChunkSize ? length - offset : kRawChunkSize;
    size_t encoded_length = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(encoded), sizeof(encoded) - 1,
                              &encoded_length, data + offset, raw_length) != 0) {
      ESP_LOGE(kTag, "Could not encode script global buffer chunk %u",
               static_cast<unsigned>(chunk));
      return;
    }
    encoded[encoded_length] = '\0';
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "clientId", mqtt_config.client_id);
    cJSON_AddNumberToObject(root, "executionId", execution_id);
    cJSON_AddNumberToObject(root, "chunkIndex", chunk);
    cJSON_AddNumberToObject(root, "chunkCount", chunk_count);
    cJSON_AddNumberToObject(root, "totalBytes", length);
    cJSON_AddStringToObject(root, "encoding", "base64");
    cJSON_AddStringToObject(root, "data", encoded);
    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
      const int message_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
      cJSON_free(payload);
      if (message_id < 0) {
        cJSON_Delete(root);
        ESP_LOGE(kTag, "Could not publish script global buffer chunk %u",
                 static_cast<unsigned>(chunk));
        return;
      }
    }
    cJSON_Delete(root);
  }
}

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

void log_ota_url(const char *label, const char *url) {
  if (!url || !url[0]) {
    ESP_LOGW(kTag, "OTA %s URL is unavailable", label);
    return;
  }
  char redacted[kMaxOtaUrlLength]{};
  strlcpy(redacted, url, sizeof(redacted));
  char *query = std::strchr(redacted, '?');
  if (query) strlcpy(query, "?<redacted>", static_cast<size_t>(redacted + sizeof(redacted) - query));
  ESP_LOGI(kTag, "OTA %s URL: %s", label, redacted);
}

void log_ota_client_url(const char *label, esp_http_client_handle_t client) {
  char url[kMaxOtaUrlLength]{};
  if (client && esp_http_client_get_url(client, url, sizeof(url)) == ESP_OK) {
    log_ota_url(label, url);
  } else {
    ESP_LOGW(kTag, "Could not read OTA %s URL", label);
  }
}

esp_err_t ota_http_event_handler(esp_http_client_event_t *event) {
  if (!event) return ESP_OK;
  if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
    log_ota_client_url("connected", event->client);
    return ESP_OK;
  }
  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
      strcasecmp(event->header_key, "Location") == 0) {
    log_ota_url("redirect", event->header_value);
    return ESP_OK;
  }
  if (event->event_id != HTTP_EVENT_ERROR) return ESP_OK;

  log_ota_client_url("failed", event->client);
  int mbedtls_error = 0;
  int verify_flags = 0;
  const esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(
      event->client, &mbedtls_error, &verify_flags);
  ESP_LOGE(kTag,
           "OTA transport diagnostics: esp-tls=%s (0x%x), mbedTLS=-0x%04x, "
           "verify_flags=0x%08x, errno=%d",
           esp_err_to_name(tls_error), static_cast<unsigned>(tls_error),
           static_cast<unsigned>(mbedtls_error < 0 ? -mbedtls_error : mbedtls_error),
           static_cast<unsigned>(verify_flags), esp_http_client_get_errno(event->client));
  if (mbedtls_error != 0) {
    char error_text[160]{};
    mbedtls_strerror(mbedtls_error, error_text, sizeof(error_text));
    ESP_LOGE(kTag, "OTA mbedTLS error: %s", error_text);
  }
  if (verify_flags != 0) {
    char verify_text[512]{};
    const int length = mbedtls_x509_crt_verify_info(
        verify_text, sizeof(verify_text), "  - ", static_cast<uint32_t>(verify_flags));
    if (length > 0) ESP_LOGE(kTag, "OTA certificate verification reasons:\n%s", verify_text);
  }
  return ESP_OK;
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
    log_ota_url("requested", command.url);
    const time_t now = time(nullptr);
    struct tm utc_time{};
    if (gmtime_r(&now, &utc_time)) {
      ESP_LOGI(kTag, "OTA certificate check time: %04d-%02d-%02dT%02d:%02d:%02dZ (epoch %lld)",
               utc_time.tm_year + 1900, utc_time.tm_mon + 1, utc_time.tm_mday,
               utc_time.tm_hour, utc_time.tm_min, utc_time.tm_sec,
               static_cast<long long>(now));
    } else {
      ESP_LOGW(kTag, "OTA certificate check time unavailable (epoch %lld)",
               static_cast<long long>(now));
    }
    ESP_LOGI(kTag, "OTA trust store: pinned github.edgez.biz self-signed certificate (%u PEM bytes)",
             static_cast<unsigned>(sizeof(kOtaProxyCertPem)));
    show_device_status("FIRMWARE UPDATE", "DOWNLOADING");
    publish_ota_status(command, "pending", "downloading");

    esp_http_client_config_t http_config{};
    http_config.url = command.url;
    // The OTA proxy uses a private, self-signed certificate so the device does
    // not need to validate a resource-intensive public CA chain.
    http_config.cert_pem = kOtaProxyCertPem;
    http_config.event_handler = ota_http_event_handler;
    http_config.timeout_ms = 30000;
    http_config.keep_alive_enable = true;
    // GitHub release assets redirect to a signed Azure URL whose request target
    // exceeds ESP-IDF's default 512-byte transmit buffer.
    http_config.buffer_size_tx = 4096;
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
  std::snprintf(expected_topic, sizeof(expected_topic), "projects/%s/devices/%s/system/commands",
                mqtt_config.project_id, mqtt_config.username);
  const size_t expected_length = std::strlen(expected_topic);
  if (event->topic_len != static_cast<int>(expected_length) ||
      std::memcmp(event->topic, expected_topic, expected_length) != 0) return;
  if (event->current_data_offset != 0 || event->data_len != event->total_data_len ||
      event->data_len <= 0 || event->data_len > 1024) {
    ESP_LOGW(kTag, "Rejected fragmented or oversized OTA command");
    return;
  }

  edgez_devices_v1_SystemCommand message = edgez_devices_v1_SystemCommand_init_zero;
  pb_istream_t stream = pb_istream_from_buffer(
      reinterpret_cast<const pb_byte_t *>(event->data), event->data_len);
  OtaCommand command{};
  const bool decoded = pb_decode(&stream, edgez_devices_v1_SystemCommand_fields, &message);
  const bool valid = decoded && message.has_header && message.header.version == 1 &&
      message.header.request_id[0] &&
      message.which_command == edgez_devices_v1_SystemCommand_ota_tag &&
      valid_https_url(message.command.ota.url);
  if (valid) {
    strlcpy(command.url, message.command.ota.url, sizeof(command.url));
    strlcpy(command.request_id, message.header.request_id, sizeof(command.request_id));
  }
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
  uint8_t halow_mac[6]{};
  if (halow_get_local_mac(halow_mac)) {
    // Proxy system commands are addressed by the HaLow MAC stored by the
    // Devices service. Use the same address in the inner relay Ethernet frame
    // so both uplink peer discovery and downlink destination matching agree.
    mqtt_l2_relay_set_local_mac(halow_mac);
  }
  xEventGroupSetBits(state_events, kNetworkConnected);
  if (halow_config.wifi_upstream) {
    show_device_status("HALOW CONNECTED",
                       xEventGroupGetBits(state_events) & kMqttConfigured
                           ? "CONNECTING MQTT"
                           : "MQTT SETUP REQUIRED");
    start_mqtt();
  } else {
    show_device_status("HALOW CONNECTED", "WAITING FOR MQTT GATEWAY");
  }
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

void enqueue_beacon(const uint8_t *data, size_t length, const uint8_t source_mac[6],
                    int16_t rssi_dbm, bool rssi_valid) {
  if (!beacon_queue || !data || !source_mac || length == 0 || length > sizeof(BeaconFrame::data)) return;
  BeaconFrame frame{};
  frame.length = length;
  std::memcpy(frame.data, data, length);
  std::memcpy(frame.source_mac, source_mac, sizeof(frame.source_mac));
  frame.rssi_dbm = rssi_dbm;
  frame.rssi_valid = rssi_valid;
  if (xQueueSend(beacon_queue, &frame, 0) != pdTRUE) {
    ESP_LOGW(kTag, "Raw HaLow beacon queue full; record dropped");
  }
}

void remember_topology_peer(const char *client_id, const uint8_t radio_mac[6],
                            int16_t rssi_dbm, bool rssi_valid) {
  if (!client_id || !client_id[0] || !radio_mac) return;
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
  if (rssi_valid || !slot->rssi_valid) {
    slot->rssi_dbm = rssi_dbm;
    slot->rssi_valid = rssi_valid;
  }
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
  uint8_t seen_macs[kMaxTopologyPeers][6]{};
  cJSON *seen_links[kMaxTopologyPeers]{};
  size_t link_count = 0;
  auto append_link = [&](const uint8_t mac[6], int64_t age_ms,
                         int16_t rssi_dbm, bool rssi_valid) {
    if (!mac || age_ms < 0 || age_ms > kTopologyPeerMaxAgeMs ||
        (mac[0] & 1U) != 0 ||
        (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0 ||
        link_count >= kMaxTopologyPeers) return;
    for (size_t index = 0; index < link_count; ++index) {
      if (std::memcmp(seen_macs[index], mac, 6) != 0) continue;
      // BATMAN supplies reachability only. A later direct Vendor-IE RF
      // observation enriches the same link with the measured radio RSSI.
      if (rssi_valid) {
        cJSON *rssi = cJSON_GetObjectItemCaseSensitive(seen_links[index], "rssi");
        if (rssi) cJSON_SetNumberValue(rssi, rssi_dbm);
        else cJSON_AddNumberToObject(seen_links[index], "rssi", rssi_dbm);
      }
      return;
    }
    cJSON *link = cJSON_CreateObject();
    if (!link) return;
    char radio_mac[18]{};
    std::snprintf(radio_mac, sizeof(radio_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(link, "peerHalowMac", radio_mac);
    cJSON_AddNumberToObject(link, "ageMs", static_cast<double>(age_ms));
    if (rssi_valid) cJSON_AddNumberToObject(link, "rssi", rssi_dbm);
    cJSON_AddItemToArray(links, link);
    std::memcpy(seen_macs[link_count], mac, 6);
    seen_links[link_count] = link;
    ++link_count;
  };

  // BATMAN knows every currently reachable mesh originator, including nodes
  // which do not emit an EdgeZ sensor Vendor IE.
  HalowRouteSnapshot routes[kMaxTopologyPeers]{};
  const size_t route_count = halow_snapshot_routes(routes, kMaxTopologyPeers);
  for (size_t index = 0; index < route_count; ++index) {
    int16_t rssi_dbm = 0;
    const bool rssi_valid =
        halow_get_peer_rssi(routes[index].originator, &rssi_dbm);
    append_link(routes[index].originator, routes[index].age_ms,
                rssi_dbm, rssi_valid);
  }

  // Vendor IE observations may arrive before BATMAN has installed a route.
  for (const auto &peer : snapshot) {
    const int64_t age_ms = now_ms - peer.last_seen_ms;
    if (!peer.occupied || age_ms < 0 || age_ms > kTopologyPeerMaxAgeMs) continue;
    append_link(peer.radio_mac, age_ms, peer.rssi_dbm, peer.rssi_valid);
  }

  cJSON_AddItemToObject(entry, "topology", topology);
}

void append_halow_rf(cJSON *entry) {
  HalowRfSnapshot snapshot{};
  if (!entry || !halow_get_rf_snapshot(&snapshot)) return;
  cJSON *rf = cJSON_CreateObject();
  cJSON *links = cJSON_CreateObject();
  if (!rf || !links) {
    cJSON_Delete(rf);
    cJSON_Delete(links);
    return;
  }
  cJSON_AddNumberToObject(rf, "version", 1);
  // HaLow-only nodes do not have an NTP source. Uptime is sent as a nonzero
  // observation marker; Devices replaces pre-2000 values with broker receive
  // time when it writes history.
  const uint64_t observed_at = std::max<uint64_t>(
      1, static_cast<uint64_t>(esp_timer_get_time() / 1000000));
  cJSON_AddNumberToObject(rf, "observedAt", static_cast<double>(observed_at));
  cJSON_AddBoolToObject(rf, "radioEnabled", snapshot.radio_enabled);
  cJSON_AddBoolToObject(rf, "available", snapshot.available);
  if (snapshot.channel)
    cJSON_AddNumberToObject(rf, "channel", snapshot.channel);
  if (snapshot.frequency_khz)
    cJSON_AddNumberToObject(rf, "frequency", snapshot.frequency_khz);
  if (snapshot.bandwidth_mhz)
    cJSON_AddNumberToObject(rf, "bandwidthMHz", snapshot.bandwidth_mhz);
  if (snapshot.signal_valid) {
    cJSON_AddNumberToObject(rf, "signalDbm", snapshot.signal_dbm);
    cJSON_AddNumberToObject(links, "averageSignalDbm",
                            snapshot.average_signal_dbm);
    cJSON_AddNumberToObject(links, "minimumSignalDbm",
                            snapshot.minimum_signal_dbm);
  }
  cJSON_AddNumberToObject(links, "peerCount", snapshot.peer_count);
  cJSON_AddItemToObject(rf, "links", links);
  cJSON_AddItemToObject(entry, "halowRf", rf);
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
  std::memcpy(reading.halow_mac, frame.source_mac, sizeof(reading.halow_mac));
  reading.halow_mac_valid = true;
  reading.observer_rssi_dbm = frame.rssi_dbm;
  reading.observer_rssi_valid = frame.rssi_valid;
  reading.observed_at_ms = esp_timer_get_time() / 1000;
  remember_topology_peer(reading.client_id, frame.source_mac, frame.rssi_dbm, frame.rssi_valid);
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

void append_remote_telemetry(cJSON *system_batch, cJSON *sensor_batch,
                             const RemoteBeacon *records, size_t count) {
  uint8_t observer_halow_mac[6]{};
  const bool observer_mac_valid = halow_get_local_mac(observer_halow_mac);
  for (size_t record_index = 0; record_index < count; ++record_index) {
    const auto &reading = records[record_index];
    cJSON *entry = cJSON_CreateObject();
    if (!entry) continue;
    cJSON_AddStringToObject(entry, "clientId", reading.client_id);
    cJSON_AddStringToObject(entry, "status", "online");
    uint8_t local_halow_mac[6]{};
    const uint8_t *halow_mac = reading.halow_mac_valid
                                   ? reading.halow_mac
                                   : (halow_get_local_mac(local_halow_mac)
                                          ? local_halow_mac : nullptr);
    if (halow_mac) {
      char formatted[18]{};
      std::snprintf(formatted, sizeof(formatted), "%02X:%02X:%02X:%02X:%02X:%02X",
                    halow_mac[0], halow_mac[1], halow_mac[2],
                    halow_mac[3], halow_mac[4], halow_mac[5]);
      cJSON_AddStringToObject(entry, "halowMac", formatted);
    }
    if (reading.halow_mac_valid && observer_mac_valid) {
      char observer[18]{};
      std::snprintf(observer, sizeof(observer), "%02X:%02X:%02X:%02X:%02X:%02X",
                    observer_halow_mac[0], observer_halow_mac[1],
                    observer_halow_mac[2], observer_halow_mac[3],
                    observer_halow_mac[4], observer_halow_mac[5]);
      const int64_t now_ms = esp_timer_get_time() / 1000;
      const int64_t age_ms = reading.observed_at_ms > 0
                                 ? now_ms - reading.observed_at_ms : 0;
      if (age_ms >= 0 && age_ms <= kTopologyPeerMaxAgeMs) {
        cJSON *topology = cJSON_CreateObject();
        cJSON *links = cJSON_CreateArray();
        cJSON *link = cJSON_CreateObject();
        if (topology && links && link) {
          cJSON_AddStringToObject(link, "peerHalowMac", observer);
          cJSON_AddNumberToObject(link, "ageMs", static_cast<double>(age_ms));
          if (reading.observer_rssi_valid)
            cJSON_AddNumberToObject(link, "rssi", reading.observer_rssi_dbm);
          cJSON_AddItemToArray(links, link);
          cJSON_AddItemToObject(topology, "links", links);
          cJSON_AddItemToObject(entry, "topology", topology);
        } else {
          cJSON_Delete(link);
          cJSON_Delete(links);
          cJSON_Delete(topology);
        }
      }
    }
    if (std::strcmp(reading.client_id, mqtt_config.client_id) == 0) {
      cJSON_AddStringToObject(entry, "firmwareVersion", esp_app_get_description()->version);
      const bool mqtt_online =
          (xEventGroupGetBits(state_events) & kMqttConnected) != 0;
      const bool relay_online = mqtt_l2_relay_gateway_available();
      cJSON_AddStringToObject(
          entry, "gateway_status",
          halow_config.wifi_upstream
              ? (mqtt_online ? "online" : "offline")
              : (relay_online ? "online" : "offline"));
      cJSON_AddBoolToObject(entry, "wifi_enabled",
                            halow_config.wifi_upstream);
      append_topology(entry);
      append_halow_rf(entry);
    }
    cJSON *sensors = cJSON_CreateArray();
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
    cJSON_AddItemToArray(system_batch, entry);
    if (sensors && cJSON_GetArraySize(sensors) > 0) {
      cJSON *sensor_entry = cJSON_CreateObject();
      if (sensor_entry) {
        cJSON_AddStringToObject(sensor_entry, "clientId", reading.client_id);
        cJSON_AddItemToObject(sensor_entry, "sensors", sensors);
        cJSON_AddItemToArray(sensor_batch, sensor_entry);
      } else {
        cJSON_Delete(sensors);
      }
    } else {
      cJSON_Delete(sensors);
    }
  }
}

bool parse_mac_bytes(const char *text, uint8_t output[6]) {
  unsigned int octets[6];
  if (!text || std::sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
                           &octets[0], &octets[1], &octets[2], &octets[3],
                           &octets[4], &octets[5]) != 6) return false;
  for (size_t index = 0; index < 6; ++index)
    output[index] = static_cast<uint8_t>(octets[index]);
  return true;
}

cJSON *json_member(cJSON *object, const char *name) {
  return object ? cJSON_GetObjectItemCaseSensitive(object, name) : nullptr;
}

void fill_topology(cJSON *object, edgez_devices_v1_Topology *topology) {
  cJSON *links = json_member(object, "links");
  if (!cJSON_IsArray(links)) return;
  cJSON *source = nullptr;
  cJSON_ArrayForEach(source, links) {
    if (topology->links_count >= 16) break;
    auto *target = &topology->links[topology->links_count];
    cJSON *value = json_member(source, "peerHalowMac");
    if (!cJSON_IsString(value) ||
        !parse_mac_bytes(value->valuestring, target->peer_halow_mac.bytes)) continue;
    target->peer_halow_mac.size = 6;
    value = json_member(source, "ageMs");
    if (cJSON_IsNumber(value)) target->age_ms = static_cast<uint32_t>(value->valuedouble);
    value = json_member(source, "rssi");
    if (cJSON_IsNumber(value)) {
      target->has_rssi = true;
      target->rssi = value->valueint;
    }
    ++topology->links_count;
  }
}

void fill_rf_links(cJSON *object, edgez_devices_v1_HalowRfLinks *links) {
  cJSON *value = nullptr;
#define RF_LINK_U32(json_name, member) \
  value = json_member(object, json_name); \
  if (cJSON_IsNumber(value)) { links->has_##member = true; links->member = static_cast<uint32_t>(value->valuedouble); }
#define RF_LINK_I32(json_name, member) \
  value = json_member(object, json_name); \
  if (cJSON_IsNumber(value)) { links->has_##member = true; links->member = value->valueint; }
#define RF_LINK_U64(json_name, member) \
  value = json_member(object, json_name); \
  if (cJSON_IsNumber(value)) { links->has_##member = true; links->member = static_cast<uint64_t>(value->valuedouble); }
  RF_LINK_U32("peerCount", peer_count);
  RF_LINK_I32("averageSignalDbm", average_signal_dbm);
  RF_LINK_I32("minimumSignalDbm", minimum_signal_dbm);
  RF_LINK_U64("rxPackets", rx_packets);
  RF_LINK_U64("txPackets", tx_packets);
  RF_LINK_U64("txRetries", tx_retries);
  RF_LINK_U64("txFailed", tx_failed);
#undef RF_LINK_U32
#undef RF_LINK_I32
#undef RF_LINK_U64
}

void fill_halow_rf(cJSON *object, edgez_devices_v1_HalowRf *rf) {
  cJSON *value = json_member(object, "version");
  if (cJSON_IsNumber(value)) rf->version = static_cast<uint32_t>(value->valuedouble);
  value = json_member(object, "observedAt");
  if (cJSON_IsNumber(value)) rf->observed_at = static_cast<uint64_t>(value->valuedouble);
  value = json_member(object, "radioEnabled");
  if (cJSON_IsBool(value)) rf->radio_enabled = cJSON_IsTrue(value);
  value = json_member(object, "available");
  if (cJSON_IsBool(value)) rf->available = cJSON_IsTrue(value);
#define RF_U32(json_name, member) \
  value = json_member(object, json_name); \
  if (cJSON_IsNumber(value)) { rf->has_##member = true; rf->member = static_cast<uint32_t>(value->valuedouble); }
#define RF_I32(json_name, member) \
  value = json_member(object, json_name); \
  if (cJSON_IsNumber(value)) { rf->has_##member = true; rf->member = value->valueint; }
  RF_U32("channel", channel);
  RF_U32("frequency", frequency);
  RF_U32("bandwidthMHz", bandwidth_mhz);
  RF_I32("signalDbm", signal_dbm);
  RF_I32("noiseDbm", noise_dbm);
  RF_U32("quality", quality);
  RF_U32("qualityMax", quality_max);
#undef RF_U32
#undef RF_I32
  value = json_member(object, "links");
  if (cJSON_IsObject(value)) {
    rf->has_links = true;
    fill_rf_links(value, &rf->links);
  }
}

bool encode_system_status(cJSON *array, uint8_t **payload, size_t *length) {
  if (!cJSON_IsArray(array) || !payload || !length) return false;
  *payload = nullptr;
  *length = 0;
  auto *batch = static_cast<edgez_devices_v1_SystemStatusBatch *>(
      heap_caps_calloc(1, sizeof(edgez_devices_v1_SystemStatusBatch),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!batch) return false;
  cJSON *source = nullptr;
  cJSON_ArrayForEach(source, array) {
    if (batch->reports_count >= 16) break;
    auto *target = &batch->reports[batch->reports_count];
    cJSON *value = json_member(source, "clientId");
    if (!cJSON_IsString(value) ||
        strlcpy(target->client_id, value->valuestring, sizeof(target->client_id)) >=
            sizeof(target->client_id)) continue;
    value = json_member(source, "status");
    if (cJSON_IsString(value) && std::strcmp(value->valuestring, "online") == 0)
      target->status = edgez_devices_v1_DeviceStatus_DEVICE_STATUS_ONLINE;
    else if (cJSON_IsString(value) && std::strcmp(value->valuestring, "offline") == 0)
      target->status = edgez_devices_v1_DeviceStatus_DEVICE_STATUS_OFFLINE;
    value = json_member(source, "halowMac");
    if (cJSON_IsString(value) &&
        parse_mac_bytes(value->valuestring, target->halow_mac.bytes))
      target->halow_mac.size = 6;
    value = json_member(source, "firmwareVersion");
    if (cJSON_IsString(value))
      strlcpy(target->firmware_version, value->valuestring,
              sizeof(target->firmware_version));
    value = json_member(source, "topology");
    if (cJSON_IsObject(value)) {
      target->has_topology = true;
      fill_topology(value, &target->topology);
    }
    value = json_member(source, "halowRf");
    if (cJSON_IsObject(value)) {
      target->has_halow_rf = true;
      fill_halow_rf(value, &target->halow_rf);
    }
    value = json_member(source, "gateway_status");
    if (cJSON_IsString(value) && std::strcmp(value->valuestring, "online") == 0)
      target->gateway_status = edgez_devices_v1_GatewayStatus_GATEWAY_STATUS_ONLINE;
    else if (cJSON_IsString(value) && std::strcmp(value->valuestring, "offline") == 0)
      target->gateway_status = edgez_devices_v1_GatewayStatus_GATEWAY_STATUS_OFFLINE;
    else if (cJSON_IsString(value) && std::strcmp(value->valuestring, "disabled") == 0)
      target->gateway_status = edgez_devices_v1_GatewayStatus_GATEWAY_STATUS_DISABLED;
    value = json_member(source, "wifi_enabled");
    if (cJSON_IsBool(value)) {
      target->has_wifi_enabled = true;
      target->wifi_enabled = cJSON_IsTrue(value);
    }
    ++batch->reports_count;
  }
  bool success = batch->reports_count > 0 &&
      pb_get_encoded_size(length, edgez_devices_v1_SystemStatusBatch_fields, batch);
  if (success) {
    *payload = static_cast<uint8_t *>(heap_caps_malloc(
        *length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (*payload) {
      pb_ostream_t stream = pb_ostream_from_buffer(*payload, *length);
      success = pb_encode(&stream, edgez_devices_v1_SystemStatusBatch_fields, batch);
    } else {
      success = false;
    }
  }
  if (!success && *payload) {
    heap_caps_free(*payload);
    *payload = nullptr;
    *length = 0;
  }
  heap_caps_free(batch);
  return success;
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
    restart_after_provisioning = provisioning_active;
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

void finish_provisioning_task(void *) {
  // Let the custom-endpoint response reach the mobile client before closing
  // BLE. NETWORK_PROV_END performs deinitialization and schedules the reboot.
  vTaskDelay(pdMS_TO_TICKS(750));
  if (provisioning_active) network_prov_mgr_stop_provisioning();
  vTaskDelete(nullptr);
}

void restart_task(void *) {
  // Give the final provisioning event and BLE teardown time to complete before
  // rebooting into the lower-memory normal operating path.
  vTaskDelay(pdMS_TO_TICKS(750));
  esp_restart();
}

esp_err_t start_halow_connection() {
  if (halow_connect_started) return ESP_OK;
  esp_err_t result = ensure_mqtt_relay();
  if (result != ESP_OK) return result;
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
    if (!candidate.wifi_upstream) {
      if (provisioning_active) {
        if (xTaskCreate(finish_provisioning_task, "finish-provisioning", 3072,
                        nullptr, 5, nullptr) != pdPASS)
          result = ESP_ERR_NO_MEM;
      } else {
        result = start_halow_connection();
      }
    }
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
  if (!sensor_script_try_acquire_hardware()) return ESP_ERR_INVALID_STATE;
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
  sensor_script_release_hardware();
  return result;
}

void enqueue_gateway_telemetry() {
  if (!(xEventGroupGetBits(state_events) & kMqttConfigured)) return;

  RemoteBeacon reading{};
  strlcpy(reading.client_id, mqtt_config.client_id, sizeof(reading.client_id));
  int battery_mv = 0;
  const esp_err_t result = read_battery_millivolts(&battery_mv);
  if (result == ESP_OK && battery_mv >= 0 && battery_mv <= 10000) {
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
    const EventBits_t required = halow_config.wifi_upstream
                                     ? kMqttConnected : kNetworkConnected;
    xEventGroupWaitBits(state_events, required, pdFALSE, pdTRUE, portMAX_DELAY);
    RemoteBeacon remote{};
    if (xQueueReceive(telemetry_queue, &remote, portMAX_DELAY) != pdTRUE) continue;
    if (halow_config.wifi_upstream &&
        !(xEventGroupGetBits(state_events) & kMqttConnected)) continue;

    cJSON *system_batch = cJSON_CreateArray();
    cJSON *sensor_batch = cJSON_CreateArray();
    if (!system_batch || !sensor_batch) {
      cJSON_Delete(system_batch);
      cJSON_Delete(sensor_batch);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    append_remote_telemetry(system_batch, sensor_batch, &remote, 1);
    uint8_t *system_payload = nullptr;
    size_t system_payload_length = 0;
    int message_id = -1;
    if (encode_system_status(system_batch, &system_payload, &system_payload_length) &&
        (!halow_config.wifi_upstream ||
         (xEventGroupGetBits(state_events) & kMqttConnected))) {
      char topic[384]{};
      std::snprintf(topic, sizeof(topic),
                    "projects/%s/devices/%s/system/status",
                    mqtt_config.project_id, mqtt_config.username);
      // Telemetry is periodic and may be dropped during an outage. QoS 0 keeps
      // stalled publishes out of the MQTT retransmission outbox so they cannot
      // exhaust the heap needed for TLS reconnection.
      message_id = mqtt_l2_relay_publish(topic, system_payload,
                                         system_payload_length, 0, false);
      if (message_id >= 0) {
        if (halow_config.wifi_upstream)
          ESP_LOGI(kTag, "System protobuf published to %s (%d)", topic, message_id);
        else
          ESP_LOGI(kTag, "System protobuf sent over BATMAN-adv (%d)",
                   message_id);
      }
    }
    heap_caps_free(system_payload);
    if (cJSON_GetArraySize(sensor_batch) > 0) {
      char *payload = cJSON_PrintUnformatted(sensor_batch);
      if (payload && (!halow_config.wifi_upstream ||
                      (xEventGroupGetBits(state_events) & kMqttConnected))) {
        char topic[384]{};
        std::snprintf(topic, sizeof(topic),
                      "projects/%s/devices/%s/telemetry/sensors",
                      mqtt_config.project_id, mqtt_config.username);
        const int sensor_message_id = mqtt_l2_relay_publish(
            topic, payload, std::strlen(payload), 0, false);
        if (sensor_message_id >= 0)
          ESP_LOGI(kTag, "Sensor telemetry published to %s (%d)", topic,
                   sensor_message_id);
        else
          message_id = -1;
      }
      cJSON_free(payload);
    }
    cJSON_Delete(system_batch);
    cJSON_Delete(sensor_batch);
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
  config.buffer.size = 12288;
  // Script commands need a large receive buffer, but outbound telemetry and
  // command results are small. Avoid reserving a second 12 KiB MQTT buffer.
  config.buffer.out_size = 4096;
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
    mqtt_l2_relay_set_gateway_online(halow_config.wifi_upstream);
    show_device_status("MQTT CONNECTED", device_serial);
    char command_topic[384]{};
    std::snprintf(command_topic, sizeof(command_topic),
                  "projects/%s/devices/%s/commands/#",
                  mqtt_config.project_id, mqtt_config.username);
    const int subscription_id = esp_mqtt_client_subscribe(mqtt_client, command_topic, 1);
    ESP_LOGI(kTag, "MQTT connected; subscribed %s (%d)", command_topic, subscription_id);
    char system_command_topic[384]{};
    std::snprintf(system_command_topic, sizeof(system_command_topic),
                  "projects/%s/devices/%s/system/commands/#",
                  mqtt_config.project_id, mqtt_config.username);
    const int system_subscription_id = esp_mqtt_client_subscribe(
        mqtt_client, system_command_topic, 1);
    ESP_LOGI(kTag, "MQTT connected; subscribed %s (%d)",
             system_command_topic, system_subscription_id);
    publish_saved_ota_result();
  } else if (event_id == MQTT_EVENT_DISCONNECTED) {
    xEventGroupClearBits(state_events, kMqttConnected);
    mqtt_l2_relay_set_gateway_online(false);
    show_device_status("MQTT STATUS", "DISCONNECTED - RETRYING");
    ESP_LOGW(kTag, "MQTT disconnected");
  } else if (event_id == MQTT_EVENT_DATA && event) {
    const int topic_length = event->topic_len < 300 ? event->topic_len : 300;
    const int data_length = event->data_len < 512 ? event->data_len : 512;
    ESP_LOGI(kTag, "Command received topic=%.*s payload=%.*s",
             topic_length, event->topic, data_length, event->data);
    const bool complete = event->current_data_offset == 0 && event->data_len == event->total_data_len;
    if (complete) {
      char topic[384]{};
      if (event->topic_len > 0 && event->topic_len < static_cast<int>(sizeof(topic))) {
        std::memcpy(topic, event->topic, event->topic_len);
        if (mqtt_l2_relay_forward_command(topic, event->data, event->data_len, 1, false)) return;
      }
    }
    if (complete && sensor_script_handle_mqtt_command(
                        event->topic, event->topic_len, event->data, event->data_len,
                        mqtt_config.project_id, mqtt_config.username)) {
      return;
    }
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
      esp_event_handler_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, event_handler);
      if (restart_after_provisioning) {
        show_device_status("PROVISION COMPLETE", "RESTARTING");
        if (xTaskCreate(restart_task, "provision-restart", 2048, nullptr, 5,
                        nullptr) != pdPASS) {
          ESP_LOGE(kTag, "Could not schedule post-provisioning restart");
          esp_restart();
        }
      } else if (halow_config.wifi_upstream && halow_config.mesh_id[0]) {
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
  const bool provisioned = load_halow_config();
  if (provisioned) {
    // BLE is needed only while provisioning. On normal boots its controller
    // and host data can be returned to the heap before application tasks and
    // MQTT/TLS start allocating memory. A provisioning reset reboots without
    // saved settings, so BLE remains available on that boot.
    const size_t heap_before = esp_get_free_heap_size();
    const esp_err_t release_result = esp_bt_mem_release(ESP_BT_MODE_BTDM);
    if (release_result == ESP_OK) {
      ESP_LOGI(kTag, "Released unused BLE memory: %u bytes",
               static_cast<unsigned>(esp_get_free_heap_size() - heap_before));
    } else {
      ESP_LOGW(kTag, "Could not release unused BLE memory: %s",
               esp_err_to_name(release_result));
    }
  }
  state_events = xEventGroupCreate();
  ESP_ERROR_CHECK(state_events ? ESP_OK : ESP_ERR_NO_MEM);
  // These queues absorb short radio/MQTT bursts. Larger depths permanently
  // consume heap and offer little value because telemetry is intentionally
  // dropped while the uplink is unavailable.
  beacon_queue = xQueueCreate(kBeaconQueueDepth, sizeof(BeaconFrame));
  telemetry_queue = xQueueCreate(kTelemetryQueueDepth, sizeof(RemoteBeacon));
  ota_queue = xQueueCreate(1, sizeof(OtaCommand));
  ESP_ERROR_CHECK(beacon_queue && telemetry_queue && ota_queue ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(sensor_script_init(
      enqueue_script_telemetry, publish_script_status, publish_script_blob));
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
      telemetry_publish_task, "telemetry_publish", 8192, nullptr, 5, nullptr);
  ESP_ERROR_CHECK(gateway_task_created == pdPASS && publish_task_created == pdPASS
                      ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(xTaskCreate(ota_task, "ota", 8192, nullptr, 6, nullptr) == pdPASS
                      ? ESP_OK : ESP_ERR_NO_MEM);

  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event_handler, nullptr));
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));
  if (provisioned) {
    show_device_status("HALOW STATUS", "CONNECTING WITH SAVED SETTINGS");
    if (halow_config.wifi_upstream) {
      ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
      ESP_ERROR_CHECK(esp_wifi_start());
      ESP_ERROR_CHECK(esp_wifi_connect());
    }
    ESP_ERROR_CHECK(start_halow_connection());
  } else {
    // Provisioning owns a sizeable transport stack. Do not register or
    // initialize any of it on normal boots with saved device configuration.
    ESP_ERROR_CHECK(esp_event_handler_register(
        NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, event_handler, nullptr));
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
