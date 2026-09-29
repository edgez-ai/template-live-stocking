#include "halow_network.h"

#include <cstring>

#include "edgez_halow_events.h"
#include "edgez_halow_radio.hpp"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern "C" {
#include "mmregdb.h"
#include "mmwlan.h"
}

namespace {
constexpr char kTag[] = "halow";
HaLowInterface radio;
bool radio_started;
halow_ready_callback_t ready_callback;
halow_beacon_callback_t beacon_callback;
halow_batman_callback_t batman_callback;

const struct mmwlan_s1g_channel *find_channel(const char *country, uint8_t channel) {
  if (!country || std::strlen(country) != 2 || channel == 0) return nullptr;
  const auto *domain = mmwlan_lookup_regulatory_domain(get_regulatory_db(), country);
  if (!domain) return nullptr;
  for (unsigned index = 0; index < domain->num_channels; ++index) {
    if (domain->channels[index].s1g_chan_num == channel) return &domain->channels[index];
  }
  return nullptr;
}

void deliver_beacon(const edgez_halow_event_t &event) {
  if (!beacon_callback) return;
  const uint8_t *ies = event.data.beacon.ies;
  const size_t length = event.data.beacon.ies_len;
  for (size_t offset = 0; offset + 2 <= length;) {
    const size_t ie_length = ies[offset + 1];
    if (offset + 2 + ie_length > length) break;
    if (ies[offset] == 221 && ie_length > 5 &&
        std::memcmp(ies + offset + 2, "EdgeZ", 5) == 0) {
      beacon_callback(ies + offset + 7, ie_length - 5,
                      event.data.beacon.bssid,
                      static_cast<int16_t>(event.data.beacon.rssi_dbm), true);
    }
    offset += 2 + ie_length;
  }
}

void event_task(void *) {
  static edgez_halow_event_t event;
  while (true) {
    if (!edgez_halow_event_receive(&event, portMAX_DELAY)) continue;
    switch (event.type) {
      case EDGEZ_HALOW_EVENT_BEACON:
        deliver_beacon(event);
        break;
      case EDGEZ_HALOW_EVENT_PEER_ADMISSION:
        (void)edgez_halow_event_respond_peer_admission(event.request_id, true, 0);
        break;
      case EDGEZ_HALOW_EVENT_BATMAN_PAYLOAD:
        if (event.data.batman_payload.payload_len >= 14 &&
            event.data.batman_payload.payload[12] == 0x88 &&
            event.data.batman_payload.payload[13] == 0xb5) {
          ESP_LOGI(kTag,
                   "HaLow BATMAN relay frame originator=" MACSTR " bytes=%u",
                   MAC2STR(event.data.batman_payload.originator),
                   static_cast<unsigned>(event.data.batman_payload.payload_len));
        }
        if (batman_callback) {
          batman_callback(event.data.batman_payload.originator,
                          event.data.batman_payload.payload,
                          event.data.batman_payload.payload_len);
        }
        break;
      default:
        break;
    }
  }
}
}  // namespace

bool halow_channel_supported(const char *country, uint8_t channel) {
  return find_channel(country, channel) != nullptr;
}

void halow_set_beacon_callback(halow_beacon_callback_t callback) {
  beacon_callback = callback;
}

void halow_set_batman_callback(halow_batman_callback_t callback) {
  batman_callback = callback;
}

bool halow_get_peer_rssi(const uint8_t peer_mac[6], int16_t *rssi_dbm) {
  return peer_mac && rssi_dbm &&
         mmwlan_get_mesh_peer_rssi(peer_mac, rssi_dbm) == MMWLAN_SUCCESS;
}

bool halow_get_local_mac(uint8_t mac[6]) {
  return mac && mmwlan_get_mac_addr(mac) == MMWLAN_SUCCESS;
}

size_t halow_snapshot_routes(HalowRouteSnapshot *routes, size_t capacity) {
  if (!routes || capacity == 0) return 0;
  constexpr size_t kSnapshotCapacity = 16;
  edgez_batadv_route_snapshot_t snapshot[kSnapshotCapacity]{};
  const size_t requested = capacity < kSnapshotCapacity ? capacity : kSnapshotCapacity;
  const size_t count = radio.snapshotBatmanRoutes(snapshot, requested);
  for (size_t index = 0; index < count; ++index) {
    std::memcpy(routes[index].originator, snapshot[index].originator, 6);
    std::memcpy(routes[index].next_hop, snapshot[index].next_hop, 6);
    routes[index].age_ms = snapshot[index].age_ms;
    routes[index].hops = snapshot[index].hops;
  }
  return count;
}

void halow_set_batman_gateway(bool available) {
  radio.setBatmanGateway(available);
}

bool halow_mark_mqtt_gateway(const uint8_t originator[6],
                             const uint8_t ethernet_mac[6]) {
  return originator && ethernet_mac &&
         radio.markBatmanMqttGateway(originator, ethernet_mac);
}

bool halow_selected_mqtt_gateway(uint8_t gateway[6],
                                 uint8_t ethernet_mac[6]) {
  return gateway && ethernet_mac &&
         radio.selectedBatmanMqttGateway(gateway, ethernet_mac);
}

esp_err_t halow_send_batman(const uint8_t destination[6], const uint8_t *data,
                            size_t length) {
  if (!radio_started || !destination || !data || !length) return ESP_ERR_INVALID_STATE;
  const EdgezRadioError result = radio.sendBatmanPayloadTo(destination, data, length);
  return result == EDGEZ_RADIO_OK ? ESP_OK :
         result == EDGEZ_RADIO_RETRY ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

esp_err_t halow_broadcast_batman(const uint8_t *data, size_t length) {
  if (!radio_started || !data || !length) return ESP_ERR_INVALID_STATE;
  const EdgezRadioError result = radio.sendBatmanBroadcastPayload(data, length);
  return result == EDGEZ_RADIO_OK ? ESP_OK :
         result == EDGEZ_RADIO_RETRY ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

esp_err_t halow_connect(const char *mesh_id, const char *passphrase,
                        const char *country, uint8_t channel, bool wifi_upstream,
                        halow_ready_callback_t on_ready) {
  (void)wifi_upstream;
  const auto *selected = find_channel(country, channel);
  if (!mesh_id || !mesh_id[0] || std::strlen(mesh_id) > 32 || !passphrase ||
      std::strlen(passphrase) < 8 || std::strlen(passphrase) > 63 || !selected)
    return ESP_ERR_INVALID_ARG;
  if (radio_started) return ESP_OK;
  if (edgez_halow_event_queue_init() != ESP_OK) return ESP_ERR_NO_MEM;
  if (xTaskCreate(event_task, "halow-events", 6144, nullptr, 7, nullptr) != pdPASS)
    return ESP_ERR_NO_MEM;
  radio.setCountryCode(country);
  radio.setMeshId(mesh_id);
  radio.setMeshSaePassphrase(passphrase);
  radio.setMeshRadio(selected->centre_freq_hz / 1000U, selected->bw_mhz);
  radio.setProactiveJoinEnabled(true);
  radio.setRelayModeEnabled(true);
  ready_callback = on_ready;
  ESP_LOGI(kTag, "Starting BATMAN mesh %s in %s channel %u", mesh_id, country, channel);
  if (!radio.init()) return ESP_FAIL;
  radio_started = true;
  if (ready_callback) ready_callback();
  return ESP_OK;
}
