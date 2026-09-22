#ifndef EDGE_DEVICE_MORSE_MESH_H_
#define EDGE_DEVICE_MORSE_MESH_H_

#include <stddef.h>
#include <stdint.h>

typedef void (*morse_mesh_rx_cb_t)(const uint8_t *radio_buf, size_t radio_len, int8_t rssi,
				   void *user_data);

int morse_mesh_send_radio_buffer(const uint8_t *radio_buf, size_t radio_len);
void morse_mesh_register_rx_cb(morse_mesh_rx_cb_t cb, void *user_data);
int morse_mesh_ensure_booted(void);

#endif /* EDGE_DEVICE_MORSE_MESH_H_ */
