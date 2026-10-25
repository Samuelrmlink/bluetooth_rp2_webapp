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

/* The ATT MTU ceiling is HCI_ACL_PAYLOAD_SIZE (below) less the 4-byte L2CAP
 * header: 255 on the LE-only build. (An earlier note here said 517, the most
 * an LE Data Length Extension PDU allows -- nothing configured that.) Going
 * past 255 means a larger HCI_ACL_PAYLOAD_SIZE, which costs RAM in every ACL
 * buffer. The value a connection actually asks for is set at runtime by the
 * bt.mtu op (src/bt/bt.c), saved in cfg/bt.json. */
/* Room for the LOCAL ATT database, which att_db_util builds at RUNTIME rather
 * than compile_gatt.py building it at compile time. A central still needs one:
 * the peer can act as a GATT client over the same connection and read our GAP
 * service, and some stacks (Apple's notably) do exactly that. Building it at
 * runtime is also what lets a Lua script declare services without a reflash. */
#define MAX_ATT_DB_SIZE 1024
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

/* ---- Classic. Off unless BT_CLASSIC is set. ----------------------------- */
#if CONFIG_BT_CLASSIC
#define ENABLE_CLASSIC
#define ENABLE_SDP
#define ENABLE_RFCOMM
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
/* Linux sends the L2CAP Information Request when the ACL comes up, and the
 * Connection Request only after that exchange. This starts the exchange at
 * ACL-up. tools/patch_l2cap_info_wait.py holds the Connection Request until
 * the reply, including for basic mode (BNEP). */
#define ENABLE_L2CAP_INFORMATION_REQUESTS_ON_CONNECT
#define MAX_NR_RFCOMM_CHANNELS 1
#define MAX_NR_RFCOMM_MULTIPLEXERS 1
#define MAX_NR_RFCOMM_SERVICES 1
#define MAX_NR_SERVICE_RECORD_ITEMS 4
/* BNEP's minimum L2CAP MTU is 1691. l2cap_max_mtu() is this value minus
 * the 4-byte L2CAP header, and that is what the Configure Request offers.
 * 1536 (offered MTU 1532) let the phone accept the connection and then
 * stall in configuration. The radio malloc still needs 8192 bytes between
 * __end__ and __HeapLimit. Do not flash a build whose gap is smaller. */
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_HOST_ACL_PACKET_LEN HCI_ACL_PAYLOAD_SIZE
#define MAX_NR_L2CAP_SERVICES  5
#define MAX_NR_L2CAP_CHANNELS  (CONFIG_BT_MAX_LINKS + 4)
#else
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)
#define MAX_NR_L2CAP_SERVICES  3
#define MAX_NR_L2CAP_CHANNELS  (CONFIG_BT_MAX_LINKS + 2)
#endif

#if CONFIG_BT_CLASSIC_NAP
#define ENABLE_BNEP
#define MAX_NR_BNEP_SERVICES 2
#define MAX_NR_BNEP_CHANNELS 1
#endif

#if CONFIG_BT_PERIPHERAL
#define ENABLE_LE_PERIPHERAL
#endif

/* ---- Sizing ------------------------------------------------------------- */
/* Shared across BOTH transports, so enabling Classic later means re-checking
 * the RAM budget rather than restructuring anything. */
#define MAX_NR_HCI_CONNECTIONS CONFIG_BT_MAX_LINKS
#define MAX_NR_GATT_CLIENTS    CONFIG_BT_MAX_LINKS
#define MAX_NR_WHITELIST_ENTRIES 16
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16

#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3
#define MAX_NR_CONTROLLER_SCO_PACKETS 3
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#ifndef HCI_HOST_ACL_PACKET_LEN
#define HCI_HOST_ACL_PACKET_LEN 1024
#endif
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000
#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#endif
