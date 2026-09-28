#include "mqtt_l2_relay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
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
constexpr size_t kFrameLimit = 480;
constexpr size_t kMaxTopic = 383;
constexpr size_t kMaxPayload = 16 * 1024;
constexpr int64_t kGatewayMaxAgeUs = 15 * 1000000LL;
constexpr size_t kPeerCount = 16;
constexpr size_t kAssemblyCount = 4;

enum class FrameType : uint8_t { kAdvertise = 1, kPublish = 2, kCommand = 3 };

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
  uint8_t payload[];
};

bool is_gateway;
bool gateway_online;
char local_serial[37];
uint8_t gateway_mac[6];
int64_t gateway_seen_us;
Peer peers[kPeerCount]{};
Assembly assemblies[kAssemblyCount]{};
mqtt_l2_publish_fn_t publish_callback;
mqtt_l2_command_fn_t command_callback;
QueueHandle_t delivery_queue;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

bool gateway_topic_for_relay(const char *source_topic, char *gateway_topic,
                             size_t gateway_topic_size) {
  constexpr char marker[] = "/devices/";
  if (!source_topic || !gateway_topic || !gateway_topic_size || !local_serial[0])
    return false;
  const char *serial = std::strstr(source_topic, marker);
  if (!serial) return false;
  serial += sizeof(marker) - 1;
  const char *suffix = std::strchr(serial, '/');
  if (!suffix) return false;
  const size_t prefix_length = static_cast<size_t>(serial - source_topic);
  const int written = std::snprintf(gateway_topic, gateway_topic_size, "%.*s%s%s",
                                    static_cast<int>(prefix_length), source_topic,
                                    local_serial, suffix);
  return written > 0 && static_cast<size_t>(written) < gateway_topic_size;
}

void remember_peer(const char *topic, const uint8_t mac[6]) {
  constexpr char marker[] = "/devices/";
  const char *start = std::strstr(topic, marker);
  if (!start) return;
  start += sizeof(marker) - 1;
  const char *end = std::strchr(start, '/');
  const size_t length = end ? static_cast<size_t>(end - start) : std::strlen(start);
  if (!length || length > 36) return;
  Peer *slot = nullptr;
  Peer *oldest = &peers[0];
  for (auto &peer : peers) {
    if (peer.used && std::strlen(peer.serial) == length &&
        std::memcmp(peer.serial, start, length) == 0) { slot = &peer; break; }
    if (!peer.used && !slot) slot = &peer;
    if (peer.seen_us < oldest->seen_us) oldest = &peer;
  }
  if (!slot) slot = oldest;
  slot->used = true;
  std::memcpy(slot->serial, start, length);
  slot->serial[length] = '\0';
  std::memcpy(slot->mac, mac, 6);
  slot->seen_us = esp_timer_get_time();
}

bool peer_for_topic(const char *topic, uint8_t mac[6]) {
  constexpr char marker[] = "/devices/";
  const char *start = std::strstr(topic, marker);
  if (!start) return false;
  start += sizeof(marker) - 1;
  const char *end = std::strchr(start, '/');
  const size_t length = end ? static_cast<size_t>(end - start) : std::strlen(start);
  for (const auto &peer : peers) {
    if (peer.used && std::strlen(peer.serial) == length &&
        std::memcmp(peer.serial, start, length) == 0) {
      std::memcpy(mac, peer.mac, 6);
      return true;
    }
  }
  return false;
}

esp_err_t send_message(FrameType type, const uint8_t destination[6], const char *topic,
                       const void *payload, size_t length, int qos, bool retain) {
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
    const esp_err_t result = halow_send_batman(
        destination, frame, sizeof(header) + included_topic + chunk);
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
      char gateway_topic[kMaxTopic + 1]{};
      if (!gateway_topic_for_relay(delivery->topic, gateway_topic,
                                   sizeof(gateway_topic))) {
        ESP_LOGW(kTag, "Dropped relayed publish with invalid topic: %s",
                 delivery->topic);
      } else {
        // The payload stays byte-for-byte unchanged and carries the leaf's
        // Appwrite clientId. Publish it beneath the authenticated gateway's
        // serial so EMQX's per-device ACL accepts it; the ingestion function
        // uses clientId to store the row against the leaf device.
        const int message_id = publish_callback(
            gateway_topic, delivery->payload, delivery->length, delivery->qos,
            delivery->retain);
        if (message_id >= 0) {
          ESP_LOGI(kTag, "Relayed %s as %s (%d)", delivery->topic,
                   gateway_topic, message_id);
        } else {
          ESP_LOGW(kTag, "Could not relay %s: MQTT is offline", delivery->topic);
        }
      }
    } else if (delivery->type == FrameType::kCommand && !is_gateway && command_callback) {
      command_callback(delivery->topic, delivery->payload, delivery->length);
    }
    heap_caps_free(delivery);
  }
}

void advertise_task(void *) {
  const uint8_t frame[] = {'E', 'Z', 'M', 'Q', kVersion,
                           static_cast<uint8_t>(FrameType::kAdvertise)};
  while (true) {
    if (is_gateway && gateway_online) (void)halow_broadcast_batman(frame, sizeof(frame));
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
}  // namespace

esp_err_t mqtt_l2_relay_init(bool gateway, const char *device_serial,
                             mqtt_l2_publish_fn_t publish_fn,
                             mqtt_l2_command_fn_t command_fn) {
  is_gateway = gateway;
  strlcpy(local_serial, device_serial ? device_serial : "", sizeof(local_serial));
  publish_callback = publish_fn;
  command_callback = command_fn;
  delivery_queue = xQueueCreate(4, sizeof(Delivery *));
  if (!delivery_queue) return ESP_ERR_NO_MEM;
  if (xTaskCreate(delivery_task, "mqtt-l2-rx", 4096, nullptr, 6, nullptr) != pdPASS ||
      xTaskCreate(advertise_task, "mqtt-l2-adv", 3072, nullptr, 5, nullptr) != pdPASS)
    return ESP_ERR_NO_MEM;
  return ESP_OK;
}

void mqtt_l2_relay_set_gateway_online(bool online) { gateway_online = online; }

bool mqtt_l2_relay_gateway_available() {
  return !is_gateway && gateway_seen_us &&
         esp_timer_get_time() - gateway_seen_us < kGatewayMaxAgeUs;
}

int mqtt_l2_relay_publish(const char *topic, const void *payload, size_t length,
                          int qos, bool retain) {
  if (is_gateway) return publish_callback ? publish_callback(topic, payload, length, qos, retain) : -1;
  if (!mqtt_l2_relay_gateway_available()) return -1;
  return send_message(FrameType::kPublish, gateway_mac, topic, payload, length,
                      qos, retain) == ESP_OK ? static_cast<int>(esp_random() & 0x7fffffff) : -1;
}

bool mqtt_l2_relay_forward_command(const char *topic, const void *payload,
                                   size_t length, int qos, bool retain) {
  if (!is_gateway || !topic) return false;
  uint8_t destination[6];
  if (!peer_for_topic(topic, destination)) return false;
  return send_message(FrameType::kCommand, destination, topic, payload, length,
                      qos, retain) == ESP_OK;
}

void mqtt_l2_relay_receive(const uint8_t originator[6], const uint8_t *data,
                           size_t length) {
  if (!originator || !data) return;
  if (length == 6 && std::memcmp(data, "EZMQ", 4) == 0 && data[4] == kVersion &&
      data[5] == static_cast<uint8_t>(FrameType::kAdvertise)) {
    if (!is_gateway) {
      std::memcpy(gateway_mac, originator, 6);
      gateway_seen_us = esp_timer_get_time();
    }
    return;
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
  if (!item->topic[0]) return;
  if (header.fragment_length) {
    std::memcpy(item->payload + header.fragment_offset,
                data + sizeof(Header) + header.topic_length, header.fragment_length);
  }
  item->received += header.fragment_length;
  item->updated_us = esp_timer_get_time();
  if (item->received != item->total_length) return;
  if (is_gateway) remember_peer(item->topic, originator);
  Delivery *delivery = static_cast<Delivery *>(heap_caps_malloc(
      sizeof(Delivery) + item->total_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (delivery) {
    delivery->type = item->type;
    delivery->qos = item->qos;
    delivery->retain = item->retain;
    delivery->length = item->total_length;
    strlcpy(delivery->topic, item->topic, sizeof(delivery->topic));
    std::memcpy(delivery->payload, item->payload, item->total_length);
    if (xQueueSend(delivery_queue, &delivery, 0) != pdTRUE) heap_caps_free(delivery);
  }
  heap_caps_free(item->payload);
  *item = {};
}
