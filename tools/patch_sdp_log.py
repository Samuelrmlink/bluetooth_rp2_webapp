#!/usr/bin/env python3
"""Insert SDP search logs into a copy of BTstack's sdp_server.c.

The handler and the calls live in one file, so a linker --wrap does not
see them. The NAP image compiles this copy and leaves the SDK file alone.
"""
import pathlib
import sys

DECL = """
/* NAP image: log the search before the reply is sent. */
extern void bt_sdp_note_search(uint8_t *packet, int reply);
extern void bt_sdp_note_attr(uint8_t *packet, int reply);
extern void bt_sdp_note_search_attr(uint8_t *packet, int reply);
"""

CALLS = (
    (
        "sdp_server_response_size = sdp_handle_service_search_request(packet, remote_mtu);",
        "sdp_server_response_size = sdp_handle_service_search_request(packet, remote_mtu);\n"
        "                    bt_sdp_note_search(packet, (int)sdp_server_response_size);",
    ),
    (
        "sdp_server_response_size = sdp_handle_service_attribute_request(packet, remote_mtu);",
        "sdp_server_response_size = sdp_handle_service_attribute_request(packet, remote_mtu);\n"
        "                    bt_sdp_note_attr(packet, (int)sdp_server_response_size);",
    ),
    (
        "sdp_server_response_size = sdp_handle_service_search_attribute_request(packet, remote_mtu);",
        "sdp_server_response_size = sdp_handle_service_search_attribute_request(packet, remote_mtu);\n"
        "                    bt_sdp_note_search_attr(packet, (int)sdp_server_response_size);",
    ),
)


def main() -> None:
    path = pathlib.Path(sys.argv[1])
    text = path.read_text()
    anchor = "static void sdp_packet_handler("
    if anchor not in text:
        raise SystemExit(f"{path}: sdp_packet_handler not found")
    if "bt_sdp_note_search" not in text:
        text = text.replace(anchor, DECL + anchor, 1)
    for old, new in CALLS:
        if old not in text:
            raise SystemExit(f"{path}: missing call: {old}")
        if new.split("\n", 1)[1].strip() in text:
            continue
        text = text.replace(old, new, 1)
    path.write_text(text)


if __name__ == "__main__":
    main()
