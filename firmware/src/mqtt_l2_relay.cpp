#include "mqtt_l2_relay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "halow_network.h"

namespace {
constexpr char kTag[] = "mqtt_l2";
constexpr uint32_t kMagic = 0x455a4d51;  // EZMQ
constexpr uint8_t kVersion = 1;
constexpr uint16_t kEtherType = 0x88b5;
constexpr size_t kEthernetHeaderLength = 14;
constexpr size_t kFrameLimit = 480;
constexpr size_t kMaxTopic = 383;
constexpr size_t kMaxPayload = 16 * 1024;
constexpr size_t kPeerCount = 16;
constexpr size_t kAssemblyCount = 4;
constexpr uint8_t kPayloadSystem = 0;
constexpr uint8_t kPayloadTelemetry = 1;
constexpr uint8_t kCommandSystem = 0;
constexpr uint8_t kCommandApplication = 1;

enum class FrameType : uint8_t {
  kAdvertise = 1,
  kPublish = 2,
  kCommand = 3,
  kPeerAdvertise = 4,
};

#pragma pack(push, 1)
struct Header {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t qos;
  uint8_t flags;
  uint32_t message_id;
  uint32_t total_length;
  uint16_t fragment_offset;
  uint16_t fragment_length;
  uint16_t topic_length;
};
#pragma pack(pop)

struct Peer {
  bool used;
  char serial[37];
  uint8_t mac[6];
  int64_t seen_us;
};

struct Assembly {
  bool used;
  uint8_t source[6];
  FrameType type;
  uint32_t message_id;
  uint32_t total_length;
  uint32_t received;
  uint8_t qos;
  bool retain;
  int64_t updated_us;
  char topic[kMaxTopic + 1];
  uint8_t *payload;
};

struct Delivery {
  FrameType type;
  uint8_t qos;
  bool retain;
  size_t length;
  char topic[kMaxTopic + 1];
  uint8_t *payload;
};

bool is_gateway;
bool gateway_online;
char local_serial[37];
uint8_t local_mac[6];
int64_t last_gateway_missing_log_us;
Peer peers[kPeerCount]{};
Assembly assemblies[kAssemblyCount]{};
mqtt_l2_publish_fn_t publish_callback;
mqtt_l2_command_fn_t command_callback;
QueueHandle_t delivery_queue;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool serial_mac(const char *serial, uint8_t mac[6]) {
  if (!serial || std::strlen(serial) != 12) return false;
  for (size_t index = 0; index < 6; ++index) {
    const int high = hex_value(serial[index * 2]);
    const int low = hex_value(serial[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    mac[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool halow_mac(const char *text, uint8_t mac[6]) {
  unsigned int octets[6];
  if (!text || std::sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
                           &octets[0], &octets[1], &octets[2], &octets[3],
                           &octets[4], &octets[5]) != 6) return false;
  for (size_t index = 0; index < 6; ++index)
    mac[index] = static_cast<uint8_t>(octets[index]);
  return true;
}

esp_err_t send_relay_frame(const uint8_t route_destination[6],
                           const uint8_t ethernet_destination[6], bool broadcast,
                           const uint8_t *payload, size_t length) {
  if (!payload || !length || length + kEthernetHeaderLength > kFrameLimit + kEthernetHeaderLength)
    return ESP_ERR_INVALID_SIZE;
  uint8_t frame[kFrameLimit + kEthernetHeaderLength];
  if (ethernet_destination)
    std::memcpy(frame, ethernet_destination, 6);
  else if (broadcast)
    std::memset(frame, 0xff, 6);
  else
    return ESP_ERR_INVALID_ARG;
  std::memcpy(frame + 6, local_mac, 6);
  frame[12] = static_cast<uint8_t>(kEtherType >> 8);
  frame[13] = static_cast<uint8_t>(kEtherType);
  std::memcpy(frame + kEthernetHeaderLength, payload, length);
  return broadcast
      ? halow_broadcast_batman(frame, length + kEthernetHeaderLength)
      : halow_send_batman(route_destination, frame,
                          length + kEthernetHeaderLength);
}

void remember_peer_serial(const uint8_t *serial, size_t length,
                          const uint8_t mac[6]) {
  if (!length || length > 36) return;
  Peer *slot = nullptr;
  Peer *oldest = &peers[0];
  for (auto &peer : peers) {
    if (peer.used && std::strlen(peer.serial) == length &&
        std::memcmp(peer.serial, serial, length) == 0) { slot = &peer; break; }
    if (!peer.used && !slot) slot = &peer;
    if (peer.seen_us < oldest->seen_us) oldest = &peer;
  }
  if (!slot) slot = oldest;
  slot->used = true;
  std::memcpy(slot->serial, serial, length);
  slot->serial[length] = '\0';
  std::memcpy(slot->mac, mac, 6);
  slot->seen_us = esp_timer_get_time();
}

bool peer_for_serial(const char *serial, size_t length, uint8_t mac[6]) {
  if (!serial || !length || length > 36) return false;
  for (const auto &peer : peers) {
    if (peer.used && std::strlen(peer.serial) == length &&
        std::memcmp(peer.serial, serial, length) == 0) {
      std::memcpy(mac, peer.mac, 6);
      return true;
    }
  }
  return false;
}

esp_err_t send_message(FrameType type, const uint8_t route_destination[6],
                       const uint8_t ethernet_destination[6], bool broadcast_route,
                       const char *topic, const void *payload, size_t length,
                       int qos, bool retain) {
  if (!topic || (!payload && length) || length > kMaxPayload) return ESP_ERR_INVALID_ARG;
  const size_t topic_length = std::strlen(topic);
  if (topic_length > kMaxTopic) return ESP_ERR_INVALID_SIZE;
  const uint32_t message_id = esp_random();
  size_t offset = 0;
  do {
    const size_t included_topic = offset == 0 ? topic_length : 0;
    const size_t capacity = kFrameLimit - sizeof(Header) - included_topic;
    const size_t chunk = std::min(capacity, length - offset);
    uint8_t frame[kFrameLimit];
    Header header{kMagic, kVersion, static_cast<uint8_t>(type),
                  static_cast<uint8_t>(qos), static_cast<uint8_t>(retain ? 1 : 0),
                  message_id, static_cast<uint32_t>(length),
                  static_cast<uint16_t>(offset), static_cast<uint16_t>(chunk),
                  static_cast<uint16_t>(included_topic)};
    std::memcpy(frame, &header, sizeof(header));
    if (included_topic) std::memcpy(frame + sizeof(header), topic, included_topic);
    if (chunk) std::memcpy(frame + sizeof(header) + included_topic,
                           static_cast<const uint8_t *>(payload) + offset, chunk);
    const esp_err_t result = send_relay_frame(
        route_destination, ethernet_destination, broadcast_route, frame,
        sizeof(header) + included_topic + chunk);
    if (result != ESP_OK) return result;
    offset += chunk;
    if (length == 0) break;
  } while (offset < length);
  return ESP_OK;
}

Assembly *find_assembly(const uint8_t source[6], const Header &header) {
  const int64_t now = esp_timer_get_time();
  Assembly *free_slot = nullptr;
  Assembly *oldest = &assemblies[0];
  for (auto &item : assemblies) {
    if (item.used && item.message_id == header.message_id &&
        std::memcmp(item.source, source, 6) == 0) return &item;
    if (!item.used && !free_slot) free_slot = &item;
    if (item.updated_us < oldest->updated_us) oldest = &item;
  }
  Assembly *item = free_slot ? free_slot : oldest;
  heap_caps_free(item->payload);
  *item = {};
  item->payload = static_cast<uint8_t *>(heap_caps_malloc(
      header.total_length ? header.total_length : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!item->payload) return nullptr;
  item->used = true;
  item->type = static_cast<FrameType>(header.type);
  item->message_id = header.message_id;
  item->total_length = header.total_length;
  item->qos = header.qos;
  item->retain = (header.flags & 1) != 0;
  item->updated_us = now;
  std::memcpy(item->source, source, 6);
  return item;
}

void delivery_task(void *) {
  Delivery *delivery = nullptr;
  while (true) {
    if (xQueueReceive(delivery_queue, &delivery, portMAX_DELAY) != pdTRUE) continue;
    if (delivery->type == FrameType::kPublish && is_gateway && publish_callback) {
      if (delivery->length < 2 || delivery->payload[0] > kPayloadTelemetry) {
        ESP_LOGW(kTag, "Rejected relayed uplink with invalid leading kind byte");
        heap_caps_free(delivery->payload);
        heap_caps_free(delivery);
        continue;
      }
      const char *topic = delivery->payload[0] == kPayloadTelemetry
          ? "telemetry/sensors" : "system/status";
      const int message_id = publish_callback(topic, delivery->payload + 1,
                                               delivery->length - 1,
                                               delivery->qos, delivery->retain);
      if (message_id >= 0) {
        ESP_LOGI(kTag, "Relayed BATMAN payload to gateway MQTT status (%d)",
                 message_id);
      } else {
        ESP_LOGW(kTag, "Could not relay BATMAN payload: MQTT is offline");
      }
    } else if (delivery->type == FrameType::kCommand && !is_gateway && command_callback) {
      if (delivery->length < 2 || delivery->payload[0] > kCommandApplication) {
        ESP_LOGW(kTag, "Rejected relayed command with invalid leading kind byte");
      } else {
        command_callback(delivery->topic, delivery->payload + 1,
                         delivery->length - 1);
      }
    }
    heap_caps_free(delivery->payload);
    heap_caps_free(delivery);
  }
}

void advertise_task(void *) {
  const uint8_t gateway_frame[] = {
      'E', 'Z', 'M', 'Q', kVersion,
      static_cast<uint8_t>(FrameType::kAdvertise)};
  while (true) {
    if (is_gateway && gateway_online) {
      (void)send_relay_frame(nullptr, nullptr, true, gateway_frame,
                             sizeof(gateway_frame));
    } else if (!is_gateway && local_serial[0]) {
      uint8_t gateway[6];
      uint8_t gateway_l2[6];
      if (!halow_selected_mqtt_gateway(gateway, gateway_l2)) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }
      uint8_t peer_frame[6 + sizeof(local_serial) - 1] = {
          'E', 'Z', 'M', 'Q', kVersion,
          static_cast<uint8_t>(FrameType::kPeerAdvertise)};
      const size_t serial_length = std::strlen(local_serial);
      std::memcpy(peer_frame + 6, local_serial, serial_length);
      const esp_err_t result = send_relay_frame(
          gateway, gateway_l2, true, peer_frame, 6 + serial_length);
      if (result == ESP_OK) {
        ESP_LOGI(kTag,
                 "Peer advertisement sent route=" MACSTR " ethernet=" MACSTR,
                 MAC2STR(gateway), MAC2STR(gateway_l2));
      } else {
        ESP_LOGW(kTag, "Peer advertisement failed: %s",
                 esp_err_to_name(result));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
}  // namespace

esp_err_t mqtt_l2_relay_init(bool gateway, const char *device_serial,
                             mqtt_l2_publish_fn_t publish_fn,
                             mqtt_l2_command_fn_t command_fn) {
  is_gateway = gateway;
  strlcpy(local_serial, device_serial ? device_serial : "", sizeof(local_serial));
  if (!serial_mac(local_serial, local_mac)) {
    ESP_LOGE(kTag, "Cannot initialize relay: device serial is not a MAC");
    return ESP_ERR_INVALID_ARG;
  }
  ESP_LOGI(kTag, "Initializing as %s serial=%s",
           is_gateway ? "gateway" : "leaf",
           local_serial[0] ? local_serial : "<missing>");
  publish_callback = publish_fn;
  command_callback = command_fn;
  delivery_queue = xQueueCreate(4, sizeof(Delivery *));
  if (!delivery_queue) return ESP_ERR_NO_MEM;
  if (xTaskCreate(delivery_task, "mqtt-l2-rx", 4096, nullptr, 6, nullptr) != pdPASS ||
      xTaskCreate(advertise_task, "mqtt-l2-adv", 5120, nullptr, 5, nullptr) != pdPASS)
    return ESP_ERR_NO_MEM;
  return ESP_OK;
}

void mqtt_l2_relay_set_gateway_online(bool online) {
  gateway_online = online;
  halow_set_batman_gateway(is_gateway && online);
}

void mqtt_l2_relay_set_local_mac(const uint8_t mac[6]) {
  if (!mac) return;
  std::memcpy(local_mac, mac, sizeof(local_mac));
  ESP_LOGI(kTag, "Relay Ethernet identity updated to HaLow MAC " MACSTR,
           MAC2STR(local_mac));
}

bool mqtt_l2_relay_gateway_available() {
  uint8_t gateway[6];
  uint8_t gateway_l2[6];
  return !is_gateway &&
         halow_selected_mqtt_gateway(gateway, gateway_l2);
}

int mqtt_l2_relay_publish(const char *topic, const void *payload, size_t length,
                          int qos, bool retain) {
  if (is_gateway) return publish_callback ? publish_callback(topic, payload, length, qos, retain) : -1;
  if (!topic || (!payload && length) || length + 1 > kMaxPayload) return -1;
  constexpr char telemetry_suffix[] = "/telemetry/sensors";
  constexpr char system_suffix[] = "/system/status";
  const size_t topic_length = std::strlen(topic);
  const auto has_suffix = [topic, topic_length](const char *suffix) {
    const size_t suffix_length = std::strlen(suffix);
    return topic_length >= suffix_length &&
           std::strcmp(topic + topic_length - suffix_length, suffix) == 0;
  };
  uint8_t payload_kind = kPayloadSystem;
  if (has_suffix(telemetry_suffix)) {
    payload_kind = kPayloadTelemetry;
  } else if (!has_suffix(system_suffix)) {
    ESP_LOGW(kTag, "Rejected unsupported relayed MQTT topic: %s", topic);
    return -1;
  }
  uint8_t gateway[6];
  uint8_t gateway_l2[6];
  if (!halow_selected_mqtt_gateway(gateway, gateway_l2)) {
    const int64_t now = esp_timer_get_time();
    if (!last_gateway_missing_log_us || now - last_gateway_missing_log_us >= 10 * 1000000LL) {
      ESP_LOGW(kTag, "Cannot relay MQTT payload: no MQTT-ready BATMAN gateway");
      last_gateway_missing_log_us = now;
    }
    return -1;
  }
  uint8_t *relay_payload = static_cast<uint8_t *>(heap_caps_malloc(
      length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!relay_payload) return -1;
  relay_payload[0] = payload_kind;
  if (length) std::memcpy(relay_payload + 1, payload, length);
  // The MQTT topic belongs to the upstream gateway and is deliberately not
  // carried over BATMAN-adv. A leading kind byte selects system/status (0) or
  // telemetry/sensors (1); the remaining bytes are forwarded unchanged.
  // Mixed ESP32/Linux meshes reliably forward BATMAN broadcast frames across
  // a shared HaLow hard interface. Keep the inner Ethernet destination
  // unicast so only the selected MQTT gateway consumes the opaque payload.
  const esp_err_t result = send_message(FrameType::kPublish, gateway, gateway_l2,
                                        true, "", relay_payload, length + 1,
                                        qos, retain);
  heap_caps_free(relay_payload);
  return result == ESP_OK ? static_cast<int>(esp_random() & 0x7fffffff) : -1;
}

bool mqtt_l2_relay_forward_command(const char *topic, const void *payload,
                                   size_t length, int qos, bool retain) {
  if (!is_gateway || !topic) return false;
  constexpr char devices_marker[] = "/devices/";
  constexpr char proxy_marker[] = "/commands/proxy/";
  constexpr char system_proxy_marker[] = "/system/commands/proxy/";
  const char *devices = std::strstr(topic, devices_marker);
  const char *serial = std::strstr(topic, system_proxy_marker);
  const bool system_command = serial != nullptr;
  if (!serial) serial = std::strstr(topic, proxy_marker);
  if (!devices || !serial || serial <= devices) return false;
  serial += system_command ? sizeof(system_proxy_marker) - 1 : sizeof(proxy_marker) - 1;
  const char *command = system_command ? nullptr : std::strchr(serial, '/');
  if ((!system_command && (!command || command == serial || !command[1])) ||
      (system_command && (!serial[0] || std::strchr(serial, '/')))) return false;
  uint8_t destination[6];
  const size_t serial_length = system_command
      ? std::strlen(serial) : static_cast<size_t>(command - serial);
  if (system_command) {
    if (!halow_mac(serial, destination)) return false;
  } else if (!peer_for_serial(serial, serial_length, destination)) {
    return false;
  }
  char leaf_topic[kMaxTopic + 1]{};
  const int written = system_command
      ? std::snprintf(leaf_topic, sizeof(leaf_topic), "system/commands")
      : std::snprintf(leaf_topic, sizeof(leaf_topic),
                      "%.*s/devices/%.*s/commands/%s",
                      static_cast<int>(devices - topic), topic,
                      static_cast<int>(serial_length), serial, command + 1);
  if (written < 0 || written >= static_cast<int>(sizeof(leaf_topic))) return false;
  if ((!payload && length) || length + 1 > kMaxPayload) return false;
  uint8_t *relay_payload = static_cast<uint8_t *>(heap_caps_malloc(
      length + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!relay_payload) return false;
  relay_payload[0] = system_command ? kCommandSystem : kCommandApplication;
  if (length) std::memcpy(relay_payload + 1, payload, length);
  // The leading byte selects the namespace; the body remains opaque.
  const esp_err_t result = send_message(FrameType::kCommand, destination,
                                        destination, true, leaf_topic,
                                        relay_payload, length + 1, qos, retain);
  heap_caps_free(relay_payload);
  return result == ESP_OK;
}

void mqtt_l2_relay_receive(const uint8_t originator[6], const uint8_t *data,
                           size_t length) {
  const uint8_t *ethernet_source;

  if (!originator || !data) return;
  if (length < kEthernetHeaderLength ||
      data[12] != static_cast<uint8_t>(kEtherType >> 8) ||
      data[13] != static_cast<uint8_t>(kEtherType)) return;
  const bool ethernet_broadcast =
      data[0] == 0xff && data[1] == 0xff && data[2] == 0xff &&
      data[3] == 0xff && data[4] == 0xff && data[5] == 0xff;
  if (!ethernet_broadcast && std::memcmp(data, local_mac, 6) != 0) return;
  ethernet_source = data + 6;
  ESP_LOGI(kTag,
           "RX relay frame originator=" MACSTR " src=" MACSTR
           " dst=" MACSTR " bytes=%u",
           MAC2STR(originator), MAC2STR(data + 6), MAC2STR(data),
           static_cast<unsigned>(length));
  data += kEthernetHeaderLength;
  length -= kEthernetHeaderLength;
  if (length >= 6 && std::memcmp(data, "EZMQ", 4) == 0 && data[4] == kVersion) {
    const auto control_type = static_cast<FrameType>(data[5]);
    if (control_type == FrameType::kAdvertise && length == 6) {
      if (!is_gateway) {
        if (halow_mark_mqtt_gateway(originator, ethernet_source)) {
          ESP_LOGD(kTag,
                   "MQTT gateway route refreshed originator=" MACSTR
                   " ethernet=" MACSTR,
                   MAC2STR(originator), MAC2STR(ethernet_source));
        } else {
          ESP_LOGD(kTag,
                   "MQTT advertisement has no BATMAN gateway route yet: " MACSTR,
                   MAC2STR(originator));
        }
      }
      return;
    }
    if (control_type == FrameType::kPeerAdvertise && is_gateway &&
        length > 6 && length <= 42) {
      remember_peer_serial(data + 6, length - 6, originator);
      return;
    }
  }
  if (length < sizeof(Header)) return;
  Header header;
  std::memcpy(&header, data, sizeof(header));
  if (header.magic != kMagic || header.version != kVersion ||
      header.total_length > kMaxPayload || header.topic_length > kMaxTopic ||
      sizeof(Header) + header.topic_length + header.fragment_length != length ||
      static_cast<size_t>(header.fragment_offset) + header.fragment_length > header.total_length)
    return;
  const auto type = static_cast<FrameType>(header.type);
  if ((type == FrameType::kPublish) != is_gateway) return;
  Assembly *item = find_assembly(originator, header);
  if (!item || item->received != header.fragment_offset) return;
  if (header.topic_length) {
    std::memcpy(item->topic, data + sizeof(Header), header.topic_length);
    item->topic[header.topic_length] = '\0';
  }
  if (type == FrameType::kCommand && !item->topic[0]) return;
  if (header.fragment_length) {
    std::memcpy(item->payload + header.fragment_offset,
                data + sizeof(Header) + header.topic_length, header.fragment_length);
  }
  item->received += header.fragment_length;
  item->updated_us = esp_timer_get_time();
  if (item->received != item->total_length) return;
  Delivery *delivery = static_cast<Delivery *>(heap_caps_malloc(
      sizeof(Delivery), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (delivery) {
    delivery->type = item->type;
    delivery->qos = item->qos;
    delivery->retain = item->retain;
    delivery->length = item->total_length;
    strlcpy(delivery->topic, item->topic, sizeof(delivery->topic));
    delivery->payload = item->payload;
    if (xQueueSend(delivery_queue, &delivery, 0) == pdTRUE) {
      item->payload = nullptr;
    } else {
      heap_caps_free(delivery);
    }
  }
  heap_caps_free(item->payload);
  *item = {};
}
