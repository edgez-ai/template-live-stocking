#include "halow_network.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
extern "C" {
#include "mmhal.h"
#include "mmipal.h"
#include "mmregdb.h"
#include "mmwlan.h"
}

namespace {
constexpr char kTag[] = "halow";
constexpr char kCountryCode[] = "US";
constexpr uint32_t kScanTimeoutMs = 45000;

SemaphoreHandle_t scan_done;
HalowNetwork *scan_networks;
size_t scan_capacity;
size_t scan_count;
bool radio_initialized;
bool ip_initialized;
halow_ready_callback_t ready_callback;

esp_err_t initialize_radio() {
  if (radio_initialized) return ESP_OK;
  mmhal_init();
  mmwlan_init();
  const auto *channels = mmwlan_lookup_regulatory_domain(get_regulatory_db(), kCountryCode);
  if (!channels) return ESP_ERR_NOT_FOUND;
  if (mmwlan_set_channel_list(channels) != MMWLAN_SUCCESS) return ESP_FAIL;
  radio_initialized = true;
  return ESP_OK;
}

esp_err_t initialize_ip() {
  esp_err_t result = initialize_radio();
  if (result != ESP_OK || ip_initialized) return result;
  struct mmipal_init_args args = MMIPAL_INIT_ARGS_DEFAULT;
  if (mmipal_init_on_existing_lwip(&args) != MMIPAL_SUCCESS) return ESP_FAIL;
  ip_initialized = true;
  return ESP_OK;
}

void scan_result(const struct mmwlan_scan_result *result, void *) {
  if (!result || !result->ssid || !result->ssid_len || result->ssid_len > 32 ||
      !result->bssid || scan_count >= scan_capacity) return;

  for (size_t index = 0; index < scan_count; ++index) {
    if (std::memcmp(scan_networks[index].bssid, result->bssid, 6) == 0) {
      if (result->rssi > scan_networks[index].rssi) scan_networks[index].rssi = result->rssi;
      return;
    }
  }

  HalowNetwork &network = scan_networks[scan_count++];
  std::memset(&network, 0, sizeof(network));
  std::memcpy(network.ssid, result->ssid, result->ssid_len);
  std::memcpy(network.bssid, result->bssid, sizeof(network.bssid));
  network.rssi = result->rssi;
  network.bandwidth_mhz = result->op_bw_mhz ? result->op_bw_mhz : result->bw_mhz;
  network.frequency_khz = result->channel_freq_hz / 1000U;
  network.secured = (result->capability_info & 0x10U) != 0;
  for (size_t offset = 0; offset + 2 <= result->ies_len;) {
    const size_t length = result->ies[offset + 1];
    if (offset + 2 + length > result->ies_len) break;
    if (result->ies[offset] == 48) network.secured = true;
    offset += 2 + length;
  }
}

void scan_complete(enum mmwlan_scan_state state, void *) {
  ESP_LOGI(kTag, "HaLow scan completed with state %d and %u results",
           static_cast<int>(state), static_cast<unsigned>(scan_count));
  if (scan_done) xSemaphoreGive(scan_done);
}

void link_status(const struct mmipal_link_status *status) {
  if (!status || status->link_state != MMIPAL_LINK_UP ||
      status->ip_addr[0] == '\0' || std::strcmp(status->ip_addr, "0.0.0.0") == 0) return;
  ESP_LOGI(kTag, "HaLow network ready at %s", status->ip_addr);
  if (ready_callback) ready_callback();
}
}  // namespace

esp_err_t halow_scan(HalowNetwork *networks, size_t capacity, size_t *count) {
  if (!networks || !capacity || !count) return ESP_ERR_INVALID_ARG;
  esp_err_t result = initialize_ip();
  if (result != ESP_OK) return result;

  if (!scan_done) scan_done = xSemaphoreCreateBinary();
  if (!scan_done) return ESP_ERR_NO_MEM;
  while (xSemaphoreTake(scan_done, 0) == pdTRUE) {}
  scan_networks = networks;
  scan_capacity = capacity;
  scan_count = 0;

  struct mmwlan_scan_req request = MMWLAN_SCAN_REQ_INIT;
  request.scan_rx_cb = scan_result;
  request.scan_complete_cb = scan_complete;
  request.args.dwell_time_ms = 120;
  request.args.dwell_on_home_ms = 0;
  if (mmwlan_scan_request(&request) != MMWLAN_SUCCESS) return ESP_FAIL;
  if (xSemaphoreTake(scan_done, pdMS_TO_TICKS(kScanTimeoutMs)) != pdTRUE) {
    mmwlan_scan_abort();
    return ESP_ERR_TIMEOUT;
  }
  *count = scan_count;
  return ESP_OK;
}

esp_err_t halow_connect(const char *ssid, const char *password,
                        const uint8_t bssid[6], halow_ready_callback_t on_ready) {
  if (!ssid || !ssid[0] || std::strlen(ssid) > 32 || !password || std::strlen(password) > 63) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t result = initialize_ip();
  if (result != ESP_OK) return result;
  ready_callback = on_ready;
  mmipal_set_link_status_callback(link_status);

  struct mmwlan_sta_args args = MMWLAN_STA_ARGS_INIT;
  args.ssid_len = std::strlen(ssid);
  std::memcpy(args.ssid, ssid, args.ssid_len);
  if (bssid) std::memcpy(args.bssid, bssid, sizeof(args.bssid));
  args.passphrase_len = std::strlen(password);
  if (args.passphrase_len) {
    std::memcpy(args.passphrase, password, args.passphrase_len + 1);
    args.security_type = MMWLAN_SAE;
  } else {
    args.security_type = MMWLAN_OPEN;
    args.pmf_mode = MMWLAN_PMF_DISABLED;
  }
  args.scan_interval_base_s = 1;
  args.scan_interval_limit_s = 8;
  ESP_LOGI(kTag, "Connecting to HaLow SSID %s", ssid);
  return mmwlan_sta_enable(&args, nullptr) == MMWLAN_SUCCESS ? ESP_OK : ESP_FAIL;
}
