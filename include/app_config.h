/*
 * Board and capacity constants that are NOT build options.
 *
 * Anything a person might reasonably want to turn on or off lives in Kconfig
 * and arrives via autoconf.h. This header is for the things that follow from
 * the hardware, plus the handful of derived values the copied lwIP/TinyUSB
 * headers expect by name.
 *
 * SDK-free on purpose: it is included by third-party translation units
 * (lwipopts.h, tusb_config.h) that are compiled before any SDK header.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "autoconf.h"

/* USB-NCM. The device answers on .1 and the DHCP server hands the host .16.
 * Deliberately not 192.168.7.x -- see the Kconfig help. */
#if CONFIG_NET_USB_NCM
#define USB_IP0 CONFIG_NET_NCM_IP0
#define USB_IP1 CONFIG_NET_NCM_IP1
#define USB_IP2 CONFIG_NET_NCM_IP2
#define USB_IP3 1
#endif

/* The cooperative loop runs cyw43 + lwIP + BTstack off one async_context_poll,
 * so there is no RTOS and no lwIP locking. 150 MHz is the RP2350 default and
 * there is no reason here to leave it. */
#ifndef APP_SYS_CLOCK_KHZ
#define APP_SYS_CLOCK_KHZ 150000
#endif

/* Pins the CYW43439 owns on a Pico 2 W. Claiming any of these kills Wi-Fi AND
 * Bluetooth -- including the transport you would use to undo it -- so the pin
 * broker (phase 7) treats them as permanently unavailable. */
#define APP_RADIO_PINS { 23, 24, 25, 29 }

#endif
