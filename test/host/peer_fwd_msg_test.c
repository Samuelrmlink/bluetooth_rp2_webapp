/* One-hop envelope: node is only the first key, the forward has no node,
 * and the tag puts it back. Prints "N checks, M failed". */
#include <stdio.h>
#include <string.h>

#include "net/peer_fwd_msg.h"

static int checks, failures;

static void expect(int cond, const char *what) {
    checks++;
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    char name[64];
    char buf[512];
    long id = -1;
    const char *in =
        "{\"node\":\"ESP32C5-3844be0a31f0\",\"id\":12,\"op\":\"fs.read\","
        "\"params\":{\"path\":\"sd/log/c5.log\"}}";
    const char *reply = "{\"id\":7,\"ok\":true,\"result\":{\"n\":0}}";
    const char *evt = "{\"evt\":\"fs.changed\",\"data\":{\"path\":\"lua/main.lua\"}}";

    expect(peer_msg_node("{\"id\":1,\"op\":\"sys.info\",\"params\":{}}", name, sizeof name) == 0,
           "no node is local");
    expect(peer_msg_node("{\"node\":\"\",\"id\":1,\"op\":\"sys.info\"}", name, sizeof name) == 0,
           "empty node is local");
    expect(peer_msg_node("{\"id\":1,\"node\":\"ESP32C5-3844be0a31f0\",\"op\":\"sys.info\"}",
                         name, sizeof name) == 0,
           "node that is not first is not a route");
    expect(peer_msg_node(in, name, sizeof name) == 1, "leading node");
    expect(strcmp(name, "ESP32C5-3844be0a31f0") == 0, "name copied");
    expect(peer_msg_node("{\"node\":\"a/b\",\"id\":1}", name, sizeof name) < 0, "bad hostname");
    expect(peer_msg_request_id(in, &id) == 1 && id == 12, "browser id");
    expect(peer_msg_forward(in, 7, buf, sizeof buf) > 0, "forward fits");
    expect(strcmp(buf, "{\"id\":7,\"op\":\"fs.read\",\"params\":{\"path\":\"sd/log/c5.log\"}}") == 0,
           "forward drops node and remaps id");
    expect(strstr(buf, "node") == NULL, "forward has no node");
    expect(peer_msg_kind(reply, &id) == 1 && id == 7, "reply id");
    expect(peer_msg_tag(reply, "ESP32C5-3844be0a31f0", 12, 1, buf, sizeof buf) > 0, "tag reply");
    expect(strcmp(buf, "{\"node\":\"ESP32C5-3844be0a31f0\",\"id\":12,\"ok\":true,\"result\":{\"n\":0}}") == 0,
           "tag restores the browser id");
    expect(peer_msg_kind(evt, &id) == 2, "event");
    expect(peer_msg_tag(evt, "bluetooth-rp2-webapp", 0, 0, buf, sizeof buf) > 0, "tag event");
    expect(strcmp(buf, "{\"node\":\"bluetooth-rp2-webapp\",\"evt\":\"fs.changed\",\"data\":{\"path\":\"lua/main.lua\"}}") == 0,
           "event keeps its body");
    expect(peer_msg_forward("{\"node\":\"Ab\",\"id\":1}", 3, buf, 8) < 0, "short buffer fails");

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
