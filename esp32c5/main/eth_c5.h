#ifndef BTGW_ETH_C5_H
#define BTGW_ETH_C5_H

/*
 * W5500 on the ESP32-C5 Wi-Fi image. The chip state machine is
 * src/net/w5500.c. Addressing is cfg/eth.json, the same three modes as
 * the Pico. This file does not call netif_set_default(): Wi-Fi stays the
 * default route until uplink selection is in this image.
 */
#include <stdbool.h>

/* False when the chip does not answer. The rest of the image still runs. */
bool eth_init(void);
void eth_poll(void);
bool eth_present(void);

#endif
