#!/usr/bin/env python3
"""Hold a Classic Connection Request until the L2CAP Information exchange ends.

BTstack already waits for that exchange when the channel asks for enhanced
retransmission. BNEP is basic mode, so the request otherwise goes out in the
same turn as the Information Request and the phone answers pending. The
handler and the state live in one file, so a linker --wrap does not see
them. The NAP image compiles this copy and leaves the SDK file alone.
"""
import pathlib
import sys

MARKER = "btgw: hold connection request"

EXTERN = """#ifdef ENABLE_CLASSIC
/* NAP image: src/bt/bt.c. Logged on the websocket with the other Classic lines. */
extern void bt_log(const char *level, const char *fmt, ...);

"""

READY = """static void l2cap_ready_to_connect(l2cap_channel_t * channel){
    /* btgw: hold connection request until the Information exchange finishes.
     * Enhanced retransmission already waited here. BNEP is basic mode, and
     * sending in the same turn makes the phone answer pending.
     * l2cap_handle_information_request_complete releases the request. */
    hci_connection_t * connection = hci_connection_for_handle(channel->con_handle);
    if (connection != NULL){
        switch (connection->l2cap_state.information_state){
            case L2CAP_INFORMATION_STATE_DONE:
                break;
            case L2CAP_INFORMATION_STATE_IDLE:
                connection->l2cap_state.information_state = L2CAP_INFORMATION_STATE_W2_SEND_EXTENDED_FEATURE_REQUEST;
                /* fall through */
            default:
                bt_log("info", "L2CAP hold connection request, information %u",
                       (unsigned)connection->l2cap_state.information_state);
                channel->state = L2CAP_STATE_WAIT_OUTGOING_EXTENDED_FEATURES;
                return;
        }
    }

    bt_log("info", "L2CAP information already done, connection request");
    channel->state = L2CAP_STATE_WILL_SEND_CONNECTION_REQUEST;
}
"""

DONE_OLD = """    connection->l2cap_state.information_state = L2CAP_INFORMATION_STATE_DONE;

    // emit event
"""

DONE_NEW = """    connection->l2cap_state.information_state = L2CAP_INFORMATION_STATE_DONE;
    bt_log("info", "L2CAP information done mask 0x%04x fixed 0x%04x",
           (unsigned)connection->l2cap_state.extended_feature_mask,
           (unsigned)connection->l2cap_state.fixed_channels_supported);

    // emit event
"""

SEND_OLD = """            l2cap_send_classic_signaling_packet(channel->con_handle, CONNECTION_REQUEST, channel->local_sig_id,
                                                channel->psm, channel->local_cid);
            l2cap_start_rtx(channel);
"""

SEND_NEW = """            l2cap_send_classic_signaling_packet(channel->con_handle, CONNECTION_REQUEST, channel->local_sig_id,
                                                channel->psm, channel->local_cid);
            l2cap_start_rtx(channel);
            bt_log("info", "L2CAP connection request sent psm 0x%04x", channel->psm);
"""

RSP_OLD = """                    l2cap_stop_rtx(channel);
                    result = little_endian_read_16 (command, L2CAP_SIGNALING_COMMAND_DATA_OFFSET+4);
                    switch (result) {
"""

RSP_NEW = """                    l2cap_stop_rtx(channel);
                    result = little_endian_read_16 (command, L2CAP_SIGNALING_COMMAND_DATA_OFFSET+4);
                    bt_log("info", "L2CAP connection response result 0x%04x status 0x%04x",
                           result,
                           little_endian_read_16(command, L2CAP_SIGNALING_COMMAND_DATA_OFFSET+6));
                    switch (result) {
"""

CFG_ENTER_OLD = """                            channel->state = L2CAP_STATE_CONFIG;
                            channelStateVarSetFlag(channel, L2CAP_CHANNEL_STATE_VAR_SEND_CONF_REQ);
                            break;
                        case 1:
                            // connection pending. get some coffee, but start the ERTX
"""

CFG_ENTER_NEW = """                            channel->state = L2CAP_STATE_CONFIG;
                            channelStateVarSetFlag(channel, L2CAP_CHANNEL_STATE_VAR_SEND_CONF_REQ);
                            bt_log("info", "L2CAP config local mtu %u", channel->local_mtu);
                            break;
                        case 1:
                            // connection pending. get some coffee, but start the ERTX
"""

CONF_SEND_OLD = """                l2cap_send_classic_signaling_packet(channel->con_handle, CONFIGURE_REQUEST, channel->local_sig_id,
                                                    channel->remote_cid, 0, options_size, &config_options);
                l2cap_start_rtx(channel);
"""

CONF_SEND_NEW = """                l2cap_send_classic_signaling_packet(channel->con_handle, CONFIGURE_REQUEST, channel->local_sig_id,
                                                    channel->remote_cid, 0, options_size, &config_options);
                l2cap_start_rtx(channel);
                bt_log("info", "L2CAP configure request sent mtu %u", channel->local_mtu);
"""

CONF_RSP_SENT_OLD = """                channelStateVarClearFlag(channel, L2CAP_CHANNEL_STATE_VAR_SEND_CONF_RSP_CONT);
            }
            else if (channel->state_var & L2CAP_CHANNEL_STATE_VAR_SEND_CONF_REQ){
"""

CONF_RSP_SENT_NEW = """                bt_log("info", "L2CAP configure response sent");
                channelStateVarClearFlag(channel, L2CAP_CHANNEL_STATE_VAR_SEND_CONF_RSP_CONT);
            }
            else if (channel->state_var & L2CAP_CHANNEL_STATE_VAR_SEND_CONF_REQ){
"""

CFG_REQ_OLD = """static void l2cap_signaling_handle_configure_request(l2cap_channel_t *channel, uint8_t *command){
"""

CFG_REQ_NEW = """static void btgw_log_config_options(const char *which, const uint8_t *command, uint16_t pos){
    uint16_t end_pos = 4u + little_endian_read_16(command, L2CAP_SIGNALING_COMMAND_LENGTH_OFFSET);
    unsigned n = 0;
    while (pos + 1u < end_pos && n < 4u){
        uint8_t type = command[pos] & 0x7f;
        uint8_t length = command[pos + 1];
        uint16_t val = 0;
        if (length >= 2u && (uint16_t)(pos + 3u) < end_pos){
            val = little_endian_read_16(command, pos + 2);
        }
        bt_log("info", "L2CAP %s option %u type %u len %u val %u", which, n, type, length, val);
        if (length > 64u){
            break;
        }
        pos = (uint16_t)(pos + 2u + length);
        n++;
    }
    if (n == 0u){
        bt_log("info", "L2CAP %s options none", which);
    }
}

static void l2cap_signaling_handle_configure_request(l2cap_channel_t *channel, uint8_t *command){
    btgw_log_config_options("configure request", command, 8);
"""

CFG_RSP_OLD = """static void l2cap_signaling_handle_configure_response(l2cap_channel_t *channel, uint16_t result, uint8_t *command){
    log_info("l2cap_signaling_handle_configure_response");
"""

CFG_RSP_NEW = """static void l2cap_signaling_handle_configure_response(l2cap_channel_t *channel, uint16_t result, uint8_t *command){
    log_info("l2cap_signaling_handle_configure_response");
    bt_log("info", "L2CAP configure response result 0x%04x", result);
    btgw_log_config_options("configure response", command, 10);
"""


def replace_function(text, signature, new_func):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"missing {signature}")
    if text.find(signature, start + len(signature)) >= 0:
        raise SystemExit(f"duplicate {signature}")
    brace = text.find("{", start)
    if brace < 0:
        raise SystemExit(f"no body for {signature}")
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[:start] + new_func + text[i + 1:]
    raise SystemExit(f"unbalanced body for {signature}")


def replace_once(text, old, new, label):
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected 1 match, found {n}")
    return text.replace(old, new, 1)


def main() -> None:
    path = pathlib.Path(sys.argv[1])
    text = path.read_text()
    if MARKER in text:
        return
    anchor = "#ifdef ENABLE_CLASSIC\n\n// returns true if channel was finalized\n"
    text = replace_once(text, anchor, EXTERN + "// returns true if channel was finalized\n", "extern")
    text = replace_function(text, "static void l2cap_ready_to_connect(l2cap_channel_t * channel)", READY)
    text = replace_once(text, DONE_OLD, DONE_NEW, "information done")
    text = replace_once(text, SEND_OLD, SEND_NEW, "connection request")
    text = replace_once(text, RSP_OLD, RSP_NEW, "connection response")
    text = replace_once(text, CFG_ENTER_OLD, CFG_ENTER_NEW, "enter config")
    text = replace_once(text, CONF_SEND_OLD, CONF_SEND_NEW, "configure request sent")
    text = replace_once(text, CONF_RSP_SENT_OLD, CONF_RSP_SENT_NEW, "configure response sent")
    text = replace_once(text, CFG_REQ_OLD, CFG_REQ_NEW, "configure request options")
    text = replace_once(text, CFG_RSP_OLD, CFG_RSP_NEW, "configure response options")
    if MARKER not in text:
        raise SystemExit("patch did not land")
    path.write_text(text)


if __name__ == "__main__":
    main()
