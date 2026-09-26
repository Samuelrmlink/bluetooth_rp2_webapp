/*
 * BTstack configuration. LE central today; the Classic block below is present
 * but inert so the seam stays visible.
 *
 * Shape follows ~/Projects/pm220-pico2w/include/btstack_config.h, which is a
 * proven-on-hardware RP2350 + CYW43439 configuration -- but that project is
 * Classic (SDP/RFCOMM/SPP) and this one is LE, so the feature set is inverted.
 */
#ifndef _PICO_BTSTACK_BTSTACK_CONFIG_H
#define _PICO_BTSTACK_BTSTACK_CONFIG_H

#include "autoconf.h"

#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

/* ---- LE ---------------------------------------------------------------- */
#define ENABLE_BLE
#define ENABLE_LE_CENTRAL
#define ENABLE_GATT_CLIENT_PAIRING
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_LE_SECURE_CONNECTIONS
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE

/* 517 is the largest ATT MTU that fits an LE Data Length Extension PDU, and
 * some peripherals negotiate right up to it. Anker's answer 253, but the point
 * of this firmware is that it works with peripherals we have never seen. */
/* Room for the LOCAL ATT database, which att_db_util builds at RUNTIME rather
 * than compile_gatt.py building it at compile time. A central still needs one:
 * the peer can act as a GATT client over the same connection and read our GAP
 * service, and some stacks (Apple's notably) do exactly that. Building it at
 * runtime is also what lets a Lua script declare services without a reflash. */
#define MAX_ATT_DB_SIZE 1024
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

/* ---- Classic (phase 9; not built) --------------------------------------- */
#if CONFIG_BT_CLASSIC
#define ENABLE_CLASSIC
#define ENABLE_SDP
#define ENABLE_RFCOMM
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#define MAX_NR_RFCOMM_CHANNELS 1
#define MAX_NR_RFCOMM_MULTIPLEXERS 1
#define MAX_NR_RFCOMM_SERVICES 1
#define MAX_NR_SERVICE_RECORD_ITEMS 4
/* Classic wants the full 1691-byte ACL payload; LE alone does not. */
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#else
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)
#endif

/* ---- Sizing ------------------------------------------------------------- */
/* Shared across BOTH transports, so enabling Classic later means re-checking
 * the RAM budget rather than restructuring anything. */
#define MAX_NR_HCI_CONNECTIONS CONFIG_BT_MAX_LINKS
#define MAX_NR_GATT_CLIENTS    CONFIG_BT_MAX_LINKS
#define MAX_NR_L2CAP_CHANNELS  (CONFIG_BT_MAX_LINKS + 2)
#define MAX_NR_L2CAP_SERVICES  3
#define MAX_NR_WHITELIST_ENTRIES 16
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16

#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000
#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#endif
