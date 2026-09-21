#include "halow_network.h"

#include <cstring>

#include "esp_log.h"
extern "C" {
#include "mmhal.h"
#include "mmipal.h"
#include "mmregdb.h"
#include "mmwlan.h"
}

namespace {
constexpr char kTag[] = "halow";
bool radio_initialized;
bool ip_initialized;
halow_ready_callback_t ready_callback;

const struct mmwlan_s1g_channel *find_channel(const char *country, uint8_t channel) {
  if (!country || std::strlen(country) != 2 || channel == 0) return nullptr;
  const auto *domain = mmwlan_lookup_regulatory_domain(get_regulatory_db(), country);
  if (!domain) return nullptr;
  for (unsigned index = 0; index < domain->num_channels; ++index) {
    if (domain->channels[index].s1g_chan_num == channel) return &domain->channels[index];
  }
  return nullptr;
}

esp_err_t initialize_radio(const char *country) {
  if (radio_initialized) return ESP_OK;
  mmhal_init();
  mmwlan_init();
  const auto *channels = mmwlan_lookup_regulatory_domain(get_regulatory_db(), country);
  if (!channels) return ESP_ERR_NOT_FOUND;
  if (mmwlan_set_channel_list(channels) != MMWLAN_SUCCESS) return ESP_FAIL;
  radio_initialized = true;
  return ESP_OK;
}

esp_err_t initialize_ip(const char *country) {
  esp_err_t result = initialize_radio(country);
  if (result != ESP_OK || ip_initialized) return result;
  struct mmipal_init_args args = MMIPAL_INIT_ARGS_DEFAULT;
  if (mmipal_init_on_existing_lwip(&args) != MMIPAL_SUCCESS) return ESP_FAIL;
  ip_initialized = true;
  return ESP_OK;
}

void link_status(const struct mmipal_link_status *status) {
  if (!status || status->link_state != MMIPAL_LINK_UP ||
      status->ip_addr[0] == '\0' || std::strcmp(status->ip_addr, "0.0.0.0") == 0) return;
  ESP_LOGI(kTag, "HaLow mesh ready at %s", status->ip_addr);
  if (ready_callback) ready_callback();
}
}  // namespace

bool halow_channel_supported(const char *country, uint8_t channel) {
  return find_channel(country, channel) != nullptr;
}

esp_err_t halow_connect(const char *mesh_id, const char *passphrase,
                        const char *country, uint8_t channel,
                        halow_ready_callback_t on_ready) {
  if (!mesh_id || !mesh_id[0] || std::strlen(mesh_id) > 32 ||
      !passphrase || std::strlen(passphrase) < 8 || std::strlen(passphrase) > 63 ||
      !find_channel(country, channel)) return ESP_ERR_INVALID_ARG;

  esp_err_t result = initialize_ip(country);
  if (result != ESP_OK) return result;
  ready_callback = on_ready;
  mmipal_set_link_status_callback(link_status);

  const auto *selected = find_channel(country, channel);
  struct mmwlan_sta_args args = MMWLAN_STA_ARGS_INIT;
  args.ssid_len = std::strlen(mesh_id);
  std::memcpy(args.ssid, mesh_id, args.ssid_len);
  args.passphrase_len = std::strlen(passphrase);
  std::memcpy(args.passphrase, passphrase, args.passphrase_len + 1);
  args.security_type = MMWLAN_SAE;
  args.mesh_mode = true;
  args.mesh_frequency_khz = selected->centre_freq_hz / 1000U;
  args.mesh_bandwidth_mhz = selected->bw_mhz;
  args.scan_interval_base_s = 1;
  args.scan_interval_limit_s = 8;
  ESP_LOGI(kTag, "Joining mesh %s in %s on channel %u", mesh_id, country, channel);
  return mmwlan_sta_enable(&args, nullptr) == MMWLAN_SUCCESS ? ESP_OK : ESP_FAIL;
}
