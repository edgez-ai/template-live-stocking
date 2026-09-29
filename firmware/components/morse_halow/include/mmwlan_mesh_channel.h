#pragma once
#include "mmwlan.h"

/* Scan frequency is the RX/primary center, not necessarily the operating
 * center. Resolve S1G Operation (EID 232) through the installed regulatory
 * table; never infer an operating channel merely from overlapping spectrum. */
static inline bool mmwlan_mesh_operating_channel(
    const struct mmwlan_s1g_channel_list *channels,
    const uint8_t *ies, size_t ies_len, uint32_t rx_hz, uint8_t rx_bw,
    uint8_t advertised_bw, uint32_t *op_hz, uint8_t *op_bw)
{
    *op_hz = rx_hz;
    *op_bw = advertised_bw ? advertised_bw : rx_bw;
    if (!ies && ies_len) return false;
    const uint8_t *operation = NULL;
    for (size_t off = 0; off < ies_len;) {
        if (ies_len - off < 2 || ies[off + 1] > ies_len - off - 2) return false;
        if (ies[off] == 232) {
            if (ies[off + 1] != 6 || operation) return false;
            operation = ies + off + 2;
        }
        off += 2 + ies[off + 1];
    }
    if (!operation) return true; /* Legacy exact-center fallback. */
    uint8_t bw = ((operation[0] >> 1) & 15) + 1;
    uint8_t primary_bw = (operation[0] & 1) ? 1 : 2;
    if ((bw != 1 && bw != 2 && bw != 4 && bw != 8 && bw != 16) ||
        primary_bw > bw || (advertised_bw && advertised_bw != bw) ||
        !channels || !channels->channels) return false;
    for (uint32_t i = 0; i < channels->num_channels; ++i) {
        const struct mmwlan_s1g_channel *ch = &channels->channels[i];
        if (ch->s1g_chan_num != operation[3] || ch->bw_mhz != bw) continue;
        if (rx_hz) {
            uint32_t delta = rx_hz > ch->centre_freq_hz ?
                rx_hz - ch->centre_freq_hz : ch->centre_freq_hz - rx_hz;
            if (rx_bw > bw || (uint64_t)delta * 2 + (uint64_t)rx_bw * 1000000 >
                             (uint64_t)bw * 1000000) return false;
        }
        *op_hz = ch->centre_freq_hz;
        *op_bw = bw;
        return true;
    }
    return false;
}

static inline bool mmwlan_mesh_channel_matches(uint32_t op_hz, uint8_t op_bw,
                                                uint32_t home_hz, uint8_t home_bw)
{
    return (!home_hz || (op_hz && op_hz == home_hz)) &&
           (!home_bw || (op_bw && op_bw == home_bw));
}

/* Accept only the bonded subchannel containing the configured primary 1 MHz.
 * An arbitrary overlapping channel (including the other primary half) is not
 * sufficient. Metadata reports the received PPDU width/center, not BSS width. */
static inline bool mmwlan_mesh_rx_channel_matches(
    uint32_t rx_hz, uint8_t rx_bw, uint32_t op_hz, uint8_t op_bw,
    uint32_t primary_hz, uint8_t primary_bw, uint8_t primary_loc)
{
    if (!rx_hz || !op_hz || !primary_hz ||
        (op_bw != 1 && op_bw != 2 && op_bw != 4 && op_bw != 8 && op_bw != 16) ||
        (rx_bw != 1 && rx_bw != 2 && rx_bw != 4 && rx_bw != 8 && rx_bw != 16) ||
        (primary_bw != 1 && primary_bw != 2) || primary_bw > op_bw ||
        rx_bw > op_bw || primary_loc >= primary_bw) return false;
    const int64_t low_hz = (int64_t)op_hz - (int64_t)op_bw * 500000;
    const int64_t primary_low_hz = (int64_t)primary_hz - (int64_t)primary_bw * 500000;
    const int64_t offset_hz = primary_low_hz - low_hz;
    if (offset_hz < 0 || offset_hz % ((int64_t)primary_bw * 1000000) != 0 ||
        offset_hz + (int64_t)primary_bw * 1000000 > (int64_t)op_bw * 1000000)
        return false;
    const uint8_t primary_index = offset_hz / 1000000 + primary_loc;
    const int64_t expected_hz = low_hz +
        (primary_index / rx_bw) * rx_bw * 1000000 + (int64_t)rx_bw * 500000;
    return (int64_t)rx_hz == expected_hz;
}
