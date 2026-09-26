/*
 * TinyUSB: USB device only, and for now only CDC-NCM.
 *
 * No host role -- this board is not a USB host, unlike the meter project this
 * networking code came from, so all the PIO-USB and CFG_TUH_* machinery is gone.
 *
 * CDC-ACM is deliberately OFF. The debug console rides SWD via RTT (no UART is
 * wired from the probe to the target), so a serial interface would earn nothing
 * today. CONFIG_IO_CDC turns it on in phase 7, when scripts want data ports to
 * talk to other boards and hosts.
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

/* Build options come from Kconfig; this header is compiled inside a
 * third-party translation unit, so it needs them explicitly. */
#include "app_config.h"

#define CFG_TUSB_MCU            OPT_MCU_RP2040   /* RP2350 shares the controller */
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUD_ENABLED         1
#define CFG_TUH_ENABLED         0

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
#endif

#define CFG_TUD_ENDPOINT0_SIZE  64

#if CONFIG_NET_USB_NCM
#define CFG_TUD_NCM             1
/* One full Ethernet frame plus NCM framing. Undersizing this shows up as
 * mysteriously dropped large packets rather than an error. */
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE   3200
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE  3200
#define CFG_TUD_NET_MTU               1514
#else
#define CFG_TUD_NCM             0
#endif

#if CONFIG_IO_CDC
#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_RX_BUFSIZE  512
#define CFG_TUD_CDC_TX_BUFSIZE  512
#else
#define CFG_TUD_CDC             0
#endif

#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#endif
