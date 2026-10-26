/* Classic RFCOMM sessions.
 *
 * spp.c is the radio. The rules (who owns a session, which PIN is tried,
 * which security level counts) live in cl_sess.c and do not include BTstack.
 * Nothing here knows a device, a service UUID, or a PIN. Channel 0 means
 * look up the UUID the caller passed. A fallback channel is used only when
 * the caller passed one.
 *
 * Events are queued on the session and emitted from classic_poll, which the
 * main loop calls after the radio poll. The packet handler only records.
 */
#include "bt/classic/spp.h"

#include <stdio.h>
#include <string.h>

#include "api/ops.h"
#include "autoconf.h"
#include "bt/bt.h"
#include "btstack.h"
#include "pico/stdlib.h"

#define INQUIRY_DEFAULT 8
#define HEX_MAX 512

static btstack_packet_callback_registration_t s_hci;
static btstack_context_callback_registration_t s_sdp_reg;

static int s_sdp_h;
static int s_sdp_stand;
static uint8_t s_sdp_addr[6];
static uint16_t s_sdp_uuid;
static uint8_t s_sdp_ch;
static int s_sdp_status;
static int s_sdp_done;

static int s_restart;
static int s_credit;
static uint8_t s_disc_sent[CL_SESS_MAX];

static uint8_t s_name_q[CL_DEV_MAX][6];
static uint8_t s_name_psrm[CL_DEV_MAX];
static uint16_t s_name_clk[CL_DEV_MAX];
static int s_name_n;
static int s_name_wait;
static uint8_t s_name_addr[6];

static uint8_t s_hex[HEX_MAX];

static const int s_io_cap[] = {
    SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT,
    SSP_IO_CAPABILITY_DISPLAY_ONLY,
    SSP_IO_CAPABILITY_DISPLAY_YES_NO,
    SSP_IO_CAPABILITY_KEYBOARD_ONLY,
};

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void addr_str(const uint8_t *a, char *out, size_t cap) {
    snprintf(out, cap, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[0], a[1], a[2], a[3], a[4], a[5]);
}

static int slot_index(int handle) {
    int s = (handle & 0xFF) - 1;
    if (s < 0 || s >= CL_SESS_MAX) { return -1; }
    return s;
}

static void apply_io(uint8_t io) {
    if (io > CL_IO_KEYBOARD) { io = CL_IO_NONE; }
    gap_ssp_set_io_capability(s_io_cap[io]);
}

static void restore_io(void) {
    if (cl_io_held() < 0) { apply_io(CL_IO_NONE); }
}

static int by_cid(uint16_t cid) {
    int hs[CL_SESS_MAX];
    int n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (cl_info(hs[i], &in) == CL_OK && in.cid == cid) { return hs[i]; }
    }
    return 0;
}

static void note_key(const uint8_t *addr, int auth, int sc) {
    int hs[CL_SESS_MAX];
    int n = cl_find_addr(addr, hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) { cl_set_key(hs[i], 1, auth, sc); }
}

static void want_restart(int handle) {
    cl_info_t in;
    bd_addr_t addr;
    int hs[CL_SESS_MAX];
    if (cl_info(handle, &in) != CL_OK) { return; }
    memcpy(addr, in.addr, 6);
    gap_drop_link_key_for_bd_addr(addr);
    s_restart = handle;
    if (in.cid) {
        rfcomm_disconnect(in.cid);
        return;
    }
    if (in.acl && cl_find_addr(addr, hs, CL_SESS_MAX) <= 1) {
        gap_disconnect(in.acl);
        return;
    }
    cl_set_acl(handle, 0);
    s_restart = 0;
}

static void ensure_security(int handle) {
    cl_info_t in;
    if (cl_info(handle, &in) != CL_OK || !in.acl || in.secure) { return; }
    if (in.level >= 2) {
        gap_request_security_level(in.acl, (gap_security_level_t)in.level);
    } else {
        cl_security(handle, in.level, 0, 0, 1);
    }
}

static void reassess(int handle, int got, int status, int final) {
    cl_info_t in;
    int auth, sc, rc;
    if (cl_info(handle, &in) != CL_OK) { return; }
    if (in.state != CL_OPENING && in.state != CL_OPEN) { return; }
    auth = in.key_known ? in.key_auth : got >= 3;
    sc = in.key_known ? in.key_sc : got >= 4;
    if (status) {
        rc = cl_auth_fail(handle);
        if (rc == CL_RETRY) { want_restart(handle); }
        return;
    }
    rc = cl_security(handle, got, auth, sc, final);
    if (rc == CL_OK) { cl_io_done(handle); }
    if (rc == CL_RETRY) { want_restart(handle); }
}

static void reassess_acl(uint16_t acl, int got, int status, int final) {
    int hs[CL_SESS_MAX];
    int n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (cl_info(hs[i], &in) != CL_OK || in.acl != acl) { continue; }
        reassess(hs[i], got, status, final);
    }
}

static int open_rfcomm(int handle) {
    cl_info_t in;
    bd_addr_t addr;
    uint16_t cid = 0;
    uint8_t err;
    if (cl_info(handle, &in) != CL_OK) { return CL_ENOENT; }
    if (!in.channel) {
        cl_fail(handle, CL_WHY_SDP, "no channel");
        return CL_EINVAL;
    }
    if (in.bond == CL_BOND_DEDICATED) { return CL_OK; }
    memcpy(addr, in.addr, 6);
    cl_set_phase(handle, in.acl ? CL_PHASE_RFCOMM : CL_PHASE_PAGE);
    err = rfcomm_create_channel(packet_handler, addr, in.channel, &cid);
    if (err) {
        cl_fail(handle, CL_WHY_RFCOMM, "RFCOMM create failed");
        return CL_EBUSY;
    }
    cl_set_cid(handle, cid);
    bt_log("info", "RFCOMM connect %s channel %u", bd_addr_to_str(addr), in.channel);
    return CL_OK;
}

static void start_sdp_query(void *context) {
    int handle = (int)(uintptr_t)context;
    cl_info_t in;
    bd_addr_t addr;
    uint8_t err;
    (void)context;
    if (handle) {
        if (cl_info(handle, &in) != CL_OK) { s_sdp_h = 0; return; }
        memcpy(addr, in.addr, 6);
        s_sdp_uuid = in.uuid;
    } else {
        memcpy(addr, s_sdp_addr, 6);
    }
    s_sdp_ch = 0;
    err = sdp_client_query_rfcomm_channel_and_name_for_uuid(packet_handler, addr, s_sdp_uuid);
    if (err) {
        bt_log("warn", "SDP query start failed 0x%02x", err);
        if (handle) { cl_fail(handle, CL_WHY_SDP, "SDP query did not start"); }
        else { s_sdp_status = err; s_sdp_done = 1; }
        s_sdp_h = 0;
        s_sdp_stand = 0;
    }
}

static int begin_sdp(int handle) {
    if (s_sdp_h || s_sdp_stand || s_sdp_done) { return CL_EBUSY; }
    if (handle) {
        cl_set_phase(handle, CL_PHASE_SDP);
        s_sdp_h = handle;
    } else {
        s_sdp_stand = 1;
    }
    s_sdp_done = 0;
    s_sdp_ch = 0;
    s_sdp_reg.callback = &start_sdp_query;
    s_sdp_reg.context = (void *)(uintptr_t)handle;
    sdp_client_register_query_callback(&s_sdp_reg);
    return CL_OK;
}

static int kick(int handle) {
    cl_info_t in;
    bd_addr_t addr;
    if (cl_info(handle, &in) != CL_OK) { return CL_ENOENT; }
    apply_io(in.io);
    if (in.level <= 1) { cl_security(handle, in.level, 0, 0, 1); }
    memcpy(addr, in.addr, 6);
    if (in.bond == CL_BOND_DEDICATED) {
        int err = gap_dedicated_bonding(addr, in.level >= 3);
        if (err) {
            cl_fail(handle, CL_WHY_PAGE, "dedicated bonding did not start");
            return CL_EBUSY;
        }
        cl_set_phase(handle, CL_PHASE_DEDICATED);
        return CL_OK;
    }
    if (!in.channel) {
        if (!in.uuid) {
            cl_fail(handle, CL_WHY_SDP, "no uuid");
            return CL_EINVAL;
        }
        return begin_sdp(handle);
    }
    return open_rfcomm(handle);
}

static int radio_block(const cl_open_t *opt) {
    int dedicated = opt->bond == CL_BOND_DEDICATED;
    int needs_sdp = opt->channel == 0 && !dedicated;
    int needs_page = !cl_addr_held(opt->addr);
    int hs[CL_SESS_MAX];
    int n;
    if (needs_sdp && !opt->uuid) { return CL_EINVAL; }
    if (needs_page && cl_inq_active()) { return CL_EBUSY; }
    if (needs_sdp && (s_sdp_h || s_sdp_stand || s_sdp_done)) { return CL_EBUSY; }
    n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (cl_info(hs[i], &in) != CL_OK) { continue; }
        if (needs_page && (in.phase == CL_PHASE_PAGE || in.phase == CL_PHASE_DEDICATED)) {
            return CL_EBUSY;
        }
        if (needs_sdp && in.phase == CL_PHASE_SDP) { return CL_EBUSY; }
    }
    return CL_OK;
}

static void finish_sdp(uint8_t status) {
    int handle = s_sdp_h;
    uint8_t ch = s_sdp_ch;
    s_sdp_h = 0;
    if (s_sdp_stand) {
        s_sdp_stand = 0;
        s_sdp_status = status ? status : (ch ? 0 : 1);
        s_sdp_ch = ch;
        s_sdp_done = 1;
    }
    if (!handle) { return; }
    if (!ch && !status) {
        cl_info_t in;
        if (cl_info(handle, &in) == CL_OK && in.fallback) { ch = in.fallback; }
    }
    if (status && !ch) {
        cl_info_t in;
        if (cl_info(handle, &in) == CL_OK && in.fallback) { ch = in.fallback; }
    }
    if (!ch) {
        cl_fail(handle, CL_WHY_SDP, status ? "SDP query failed" : "no channel");
        return;
    }
    cl_set_channel(handle, ch);
    open_rfcomm(handle);
}

static void name_pump(void) {
    bd_addr_t addr;
    if (s_name_wait || s_name_n <= 0) { return; }
    memcpy(addr, s_name_q[0], 6);
    memcpy(s_name_addr, addr, 6);
    if (gap_remote_name_request(addr, s_name_psrm[0], s_name_clk[0])) { return; }
    memmove(s_name_q[0], s_name_q[1], (size_t)(s_name_n - 1) * 6);
    memmove(s_name_psrm, s_name_psrm + 1, (size_t)(s_name_n - 1));
    memmove(s_name_clk, s_name_clk + 1, (size_t)(s_name_n - 1) * sizeof s_name_clk[0]);
    s_name_n--;
    s_name_wait = 1;
}

static void name_enqueue(const uint8_t *addr, uint8_t psrm, uint16_t clk) {
    for (int i = 0; i < s_name_n; i++) {
        if (memcmp(s_name_q[i], addr, 6) == 0) { return; }
    }
    if (s_name_wait && memcmp(s_name_addr, addr, 6) == 0) { return; }
    if (s_name_n >= CL_DEV_MAX) { return; }
    memcpy(s_name_q[s_name_n], addr, 6);
    s_name_psrm[s_name_n] = psrm;
    s_name_clk[s_name_n] = clk;
    s_name_n++;
    name_pump();
}

static void on_acl(const uint8_t *addr, uint16_t handle, int ok) {
    int hs[CL_SESS_MAX];
    int n = cl_find_addr(addr, hs, CL_SESS_MAX);
    int max_level = 0;
    uint16_t acl = 0;
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (cl_info(hs[i], &in) != CL_OK) { continue; }
        if (!ok) {
            if (in.state == CL_OPENING) { cl_fail(hs[i], CL_WHY_PAGE, "page failed"); }
            if (s_restart == hs[i]) { s_restart = 0; }
            continue;
        }
        cl_set_acl(hs[i], handle);
        if (in.phase == CL_PHASE_DEDICATED || in.bond == CL_BOND_DEDICATED) {
            cl_set_phase(hs[i], CL_PHASE_RFCOMM);
        }
        if (in.level > max_level) { max_level = in.level; }
        acl = handle;
    }
    if (ok && acl && max_level >= 2) {
        gap_request_security_level(acl, (gap_security_level_t)max_level);
    } else if (ok) {
        /* Level 0 and 1 do not need encryption. Pass the level the session
         * asked for, so a later ACL-up does not fail it. */
        for (int i = 0; i < n; i++) {
            cl_info_t in;
            if (cl_info(hs[i], &in) != CL_OK || in.level > 1) { continue; }
            cl_security(hs[i], in.level, 0, 0, 1);
        }
    }
}

static void on_disc(uint16_t acl) {
    int hs[CL_SESS_MAX];
    int n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (cl_info(hs[i], &in) != CL_OK || in.acl != acl) { continue; }
        cl_set_acl(hs[i], 0);
        cl_set_cid(hs[i], 0);
        if (s_restart == hs[i] && in.state == CL_OPENING) {
            s_restart = 0;
            kick(hs[i]);
            continue;
        }
        if (in.state == CL_OPENING || in.state == CL_OPEN) {
            cl_fail(hs[i], CL_WHY_REMOTE, "acl down");
        }
    }
}

static void note_scan(const uint8_t *packet) {
    bd_addr_t addr;
    uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
    int8_t rssi = gap_event_inquiry_result_get_rssi_available(packet)
                      ? gap_event_inquiry_result_get_rssi(packet) : 0;
    char name[CL_NAME_LEN];
    int have = 0;
    name[0] = 0;
    gap_event_inquiry_result_get_bd_addr(packet, addr);
    if (gap_event_inquiry_result_get_name_available(packet)) {
        int n = gap_event_inquiry_result_get_name_len(packet);
        const uint8_t *p = gap_event_inquiry_result_get_name(packet);
        if (n > CL_NAME_LEN - 1) { n = CL_NAME_LEN - 1; }
        if (n < 0) { n = 0; }
        memcpy(name, p, (size_t)n);
        name[n] = 0;
        have = name[0] != 0;
    }
    cl_inq_note(addr, rssi, cod, have ? name : NULL);
    if (!have) {
        name_enqueue(addr,
                     gap_event_inquiry_result_get_page_scan_repetition_mode(packet),
                     gap_event_inquiry_result_get_clock_offset(packet));
    }
    bt_log("info", "inquiry %s COD %06lx rssi %d %s", bd_addr_to_str(addr),
           (unsigned long)cod, rssi, name);
}

static void send_answer(int handle) {
    cl_info_t in;
    bd_addr_t addr;
    int kind = 0, neg = 0;
    char pin[CL_PIN_LEN];
    uint32_t num = 0;
    if (!cl_take_answer(handle, &kind, &neg, pin, sizeof pin, &num)) { return; }
    if (cl_info(handle, &in) != CL_OK) { return; }
    memcpy(addr, in.addr, 6);
    if (kind == CL_ASK_PIN) {
        if (neg) { gap_pin_code_negative(addr); }
        else { gap_pin_code_response(addr, pin); }
    } else if (kind == CL_ASK_PASSKEY) {
        if (neg) { gap_ssp_passkey_negative(addr); }
        else { gap_ssp_passkey_response(addr, num); }
    } else if (kind == CL_ASK_CONFIRM) {
        if (neg) { gap_ssp_confirmation_negative(addr); }
        else { gap_ssp_confirmation_response(addr); }
    }
}

static void drive_down(int handle) {
    cl_info_t in;
    int ix;
    int hs[CL_SESS_MAX];
    int n, others = 0;
    if (cl_info(handle, &in) != CL_OK) { return; }
    if (in.state != CL_CLOSING && in.state != CL_FAILED) { return; }
    ix = slot_index(handle);
    if (in.cid && ix >= 0 && !s_disc_sent[ix]) {
        s_disc_sent[ix] = 1;
        rfcomm_disconnect(in.cid);
        return;
    }
    if (in.cid) { return; }
    n = cl_find_addr(in.addr, hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t o;
        if (hs[i] == handle) { continue; }
        if (cl_info(hs[i], &o) != CL_OK) { continue; }
        if (o.state == CL_OPEN || o.state == CL_OPENING) { others++; }
    }
    if (in.acl && !others) {
        gap_disconnect(in.acl);
        cl_set_acl(handle, 0);
        return;
    }
    /* A module still has the close or failure queued. Free the slot after it reads it. */
    if (in.owner && cl_mod_pending(handle)) { return; }
    cl_reap(handle);
    if (ix >= 0) { s_disc_sent[ix] = 0; }
}

static void pump_tx(void) {
    int hs[CL_SESS_MAX];
    int n;
    if (s_credit) { return; }
    n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        const uint8_t *p = NULL;
        if (!cl_tx_eligible(hs[i])) { continue; }
        if (cl_info(hs[i], &in) != CL_OK || !in.cid) { continue; }
        if (cl_tx_peek(hs[i], &p, 1) <= 0) { continue; }
        s_credit = in.cid;
        rfcomm_request_can_send_now_event(in.cid);
        return;
    }
}

static void on_can_send(uint16_t cid) {
    int handle = by_cid(cid);
    int other = 0;
    int hs[CL_SESS_MAX];
    int n;
    const uint8_t *p = NULL;
    cl_info_t in;
    s_credit = 0;
    if (handle && cl_info(handle, &in) == CL_OK && in.state == CL_OPEN) {
        uint16_t room = in.mtu ? in.mtu : 127;
        int got = cl_tx_peek(handle, &p, room);
        if (got > 0 && p) {
            rfcomm_send(cid, (uint8_t *)p, (uint16_t)got);
            cl_tx_advance(handle, (size_t)got);
        }
    }
    n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        if (hs[i] == handle || !cl_tx_eligible(hs[i])) { continue; }
        other = hs[i];
        break;
    }
    if (!other && handle && cl_tx_eligible(handle)) { other = handle; }
    if (other && cl_info(other, &in) == CL_OK && in.cid) {
        s_credit = in.cid;
        rfcomm_request_can_send_now_event(in.cid);
    }
}

/* ACL and pairing, one line each. These events are marked handled. */
static const char *io_cap_name(uint8_t cap) {
    switch (cap) {
    case 0: return "display only";
    case 1: return "display yes/no";
    case 2: return "keyboard";
    case 3: return "no input/output";
    default: return "unknown";
    }
}

static const char *key_type_name(uint8_t t) {
    switch (t) {
    case 0: return "combination";
    case 3: return "debug";
    case 4: return "unauthenticated";
    case 5: return "authenticated";
    case 6: return "changed";
    default: return "other";
    }
}

static void log_link_event(uint8_t event, const uint8_t *packet, uint16_t size) {
    bd_addr_t addr;
    uint8_t st;
    switch (event) {
    case HCI_EVENT_CONNECTION_REQUEST:
        hci_event_connection_request_get_bd_addr(packet, addr);
        bt_log("info", "page from %s cod 0x%06lx",
               bd_addr_to_str(addr),
               (unsigned long)hci_event_connection_request_get_class_of_device(packet));
        break;
    case HCI_EVENT_CONNECTION_COMPLETE:
        st = hci_event_connection_complete_get_status(packet);
        hci_event_connection_complete_get_bd_addr(packet, addr);
        bt_log(st ? "warn" : "info", "acl %s %s (0x%02x) handle %u",
               bd_addr_to_str(addr), bt_hci_reason(st), st,
               hci_event_connection_complete_get_connection_handle(packet));
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        st = hci_event_disconnection_complete_get_reason(packet);
        bt_log("info", "acl down handle %u %s (0x%02x)",
               hci_event_disconnection_complete_get_connection_handle(packet),
               bt_hci_reason(st), st);
        break;
    case HCI_EVENT_AUTHENTICATION_COMPLETE:
        st = hci_event_authentication_complete_get_status(packet);
        bt_log(st ? "warn" : "info", "authentication handle %u %s (0x%02x)",
               hci_event_authentication_complete_get_connection_handle(packet),
               bt_hci_reason(st), st);
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        st = hci_event_encryption_change_get_status(packet);
        bt_log(st ? "warn" : "info", "encryption handle %u %s (0x%02x) enabled %u",
               hci_event_encryption_change_get_connection_handle(packet),
               bt_hci_reason(st), st,
               hci_event_encryption_change_get_encryption_enabled(packet));
        break;
    case HCI_EVENT_LINK_KEY_NOTIFICATION:
        if (size < 25) { break; }
        hci_event_link_key_request_get_bd_addr(packet, addr);
        bt_log("info", "link key from %s type %s (%u)",
               bd_addr_to_str(addr), key_type_name(packet[24]), packet[24]);
        break;
    case HCI_EVENT_IO_CAPABILITY_REQUEST:
        hci_event_io_capability_request_get_bd_addr(packet, addr);
        bt_log("info", "SSP io capability request %s", bd_addr_to_str(addr));
        break;
    case HCI_EVENT_IO_CAPABILITY_RESPONSE:
        hci_event_io_capability_response_get_bd_addr(packet, addr);
        bt_log("info", "peer io %s auth 0x%02x %s",
               io_cap_name(hci_event_io_capability_response_get_io_capability(packet)),
               hci_event_io_capability_response_get_authentication_requirements(packet),
               bd_addr_to_str(addr));
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        bt_log("info", "SSP confirm %s number %lu", bd_addr_to_str(addr),
               (unsigned long)hci_event_user_confirmation_request_get_numeric_value(packet));
        break;
    case HCI_EVENT_USER_PASSKEY_REQUEST:
        hci_event_user_passkey_request_get_bd_addr(packet, addr);
        bt_log("info", "passkey requested %s", bd_addr_to_str(addr));
        break;
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
        st = hci_event_simple_pairing_complete_get_status(packet);
        hci_event_simple_pairing_complete_get_bd_addr(packet, addr);
        bt_log(st ? "warn" : "info", "pairing complete %s %s (0x%02x)",
               bd_addr_to_str(addr), bt_hci_reason(st), st);
        break;
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        st = hci_event_remote_name_request_complete_get_status(packet);
        hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
        bt_log(st ? "warn" : "info", "remote name %s %s", bd_addr_to_str(addr),
               st ? bt_hci_reason(st) : hci_event_remote_name_request_complete_get_remote_name(packet));
        break;
    default:
        break;
    }
}

static int opening_on(const uint8_t *addr) {
    int hs[CL_SESS_MAX];
    int n = cl_find_addr(addr, hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        cl_info_t in;
        if (cl_info(hs[i], &in) == CL_OK && in.state == CL_OPENING) { return hs[i]; }
    }
    return n ? hs[0] : 0;
}

static void handle_sdp(uint8_t *packet) {
    switch (hci_event_packet_get_type(packet)) {
    case SDP_EVENT_QUERY_RFCOMM_SERVICE: {
        uint8_t ch = sdp_event_query_rfcomm_service_get_rfcomm_channel(packet);
        if (ch && !s_sdp_ch) { s_sdp_ch = ch; }
        bt_log("info", "SDP channel %u", ch);
        break;
    }
    case SDP_EVENT_QUERY_COMPLETE:
        finish_sdp(sdp_event_query_complete_get_status(packet));
        break;
    default:
        break;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type == RFCOMM_DATA_PACKET) {
        int h = by_cid(channel);
        if (h && size) { cl_rx_put(h, packet, size); }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) { return; }
    uint8_t event = hci_event_packet_get_type(packet);
    log_link_event(event, packet, size);
    if (event == SDP_EVENT_QUERY_RFCOMM_SERVICE || event == SDP_EVENT_QUERY_COMPLETE) {
        handle_sdp(packet);
        return;
    }
    switch (event) {
    case GAP_EVENT_INQUIRY_RESULT:
        if (cl_inq_active()) { note_scan(packet); }
        break;
    case GAP_EVENT_INQUIRY_COMPLETE:
        cl_inq_stop();
        bt_log("info", "inquiry complete, %d device(s)", cl_inq_count());
        break;
    case HCI_EVENT_CONNECTION_COMPLETE: {
        bd_addr_t addr;
        uint8_t st = hci_event_connection_complete_get_status(packet);
        if (hci_event_connection_complete_get_link_type(packet) != 1) { break; }
        hci_event_connection_complete_get_bd_addr(packet, addr);
        on_acl(addr, hci_event_connection_complete_get_connection_handle(packet), st == 0);
        break;
    }
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        on_disc(hci_event_disconnection_complete_get_connection_handle(packet));
        break;
    case HCI_EVENT_PIN_CODE_REQUEST: {
        bd_addr_t addr;
        char pin[CL_PIN_LEN];
        int h;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        h = opening_on(addr);
        if (!h) {
            gap_pin_code_negative(addr);
            break;
        }
        if (cl_pin_pull(h, pin, sizeof pin) == 1) { gap_pin_code_response(addr, pin); }
        break;
    }
    case HCI_EVENT_USER_PASSKEY_REQUEST: {
        bd_addr_t addr;
        uint32_t pk = 0;
        int h;
        hci_event_user_passkey_request_get_bd_addr(packet, addr);
        h = opening_on(addr);
        if (!h) {
            gap_ssp_passkey_negative(addr);
            break;
        }
        if (cl_passkey_pull(h, &pk) == 1) { gap_ssp_passkey_response(addr, pk); }
        break;
    }
    case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
        bd_addr_t addr;
        uint32_t num;
        int h;
        cl_info_t in;
        hci_event_user_confirmation_request_get_bd_addr(packet, addr);
        num = hci_event_user_confirmation_request_get_numeric_value(packet);
        h = opening_on(addr);
        if (!h || (cl_info(h, &in) == CL_OK && in.io == CL_IO_NONE)) {
            /* Just-works, and a NAP pairing that has no session. */
            gap_ssp_confirmation_response(addr);
            break;
        }
        cl_confirm_ask(h, num);
        break;
    }
    case HCI_EVENT_SIMPLE_PAIRING_COMPLETE: {
        bd_addr_t addr;
        uint8_t st = hci_event_simple_pairing_complete_get_status(packet);
        int hs[CL_SESS_MAX];
        int n;
        hci_event_simple_pairing_complete_get_bd_addr(packet, addr);
        n = cl_find_addr(addr, hs, CL_SESS_MAX);
        for (int i = 0; i < n; i++) {
            cl_io_done(hs[i]);
            if (st) {
                int rc = cl_auth_fail(hs[i]);
                if (rc == CL_RETRY) { want_restart(hs[i]); }
            } else {
                cl_info_t in;
                int got = 0;
                if (cl_info(hs[i], &in) == CL_OK && in.acl) { got = (int)gap_security_level(in.acl); }
                reassess(hs[i], got, 0, 1);
            }
        }
        break;
    }
    case HCI_EVENT_AUTHENTICATION_COMPLETE: {
        uint8_t st = hci_event_authentication_complete_get_status(packet);
        uint16_t acl = hci_event_authentication_complete_get_connection_handle(packet);
        if (st) { reassess_acl(acl, 0, st, 1); }
        else {
            int hs[CL_SESS_MAX];
            int n = cl_handles(hs, CL_SESS_MAX);
            for (int i = 0; i < n; i++) {
                cl_info_t in;
                if (cl_info(hs[i], &in) == CL_OK && in.acl == acl) { cl_io_done(hs[i]); }
            }
        }
        break;
    }
    case HCI_EVENT_ENCRYPTION_CHANGE: {
        uint8_t st = hci_event_encryption_change_get_status(packet);
        uint16_t acl = hci_event_encryption_change_get_connection_handle(packet);
        int got = st ? 0 : (int)gap_security_level(acl);
        int final = st || cl_io_held() < 0;
        reassess_acl(acl, got, st, final);
        break;
    }
    case GAP_EVENT_SECURITY_LEVEL: {
        uint16_t acl = gap_event_security_level_get_handle(packet);
        int got = gap_event_security_level_get_security_level(packet);
        int st = gap_event_security_level_get_status(packet);
        reassess_acl(acl, got, st, st != 0);
        break;
    }
    case HCI_EVENT_LINK_KEY_NOTIFICATION: {
        bd_addr_t addr;
        link_key_type_t kt;
        if (size < 25) { break; }
        hci_event_link_key_request_get_bd_addr(packet, addr);
        kt = (link_key_type_t)packet[24];
        note_key(addr, gap_authenticated_for_link_key_type(kt),
                 gap_secure_connection_for_link_key_type(kt));
        break;
    }
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
        bd_addr_t addr;
        uint8_t st = hci_event_remote_name_request_complete_get_status(packet);
        hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
        if (!st) {
            const char *n = hci_event_remote_name_request_complete_get_remote_name(packet);
            cl_inq_set_name(addr, n);
        }
        s_name_wait = 0;
        name_pump();
        break;
    }
    case GAP_EVENT_DEDICATED_BONDING_COMPLETED: {
        bd_addr_t addr;
        uint8_t st = gap_event_dedicated_bonding_completed_get_status(packet);
        int h;
        gap_event_dedicated_bonding_completed_get_address(packet, addr);
        h = opening_on(addr);
        if (!h) { break; }
        cl_io_done(h);
        if (st) {
            int rc = cl_auth_fail(h);
            if (rc == CL_RETRY) { want_restart(h); }
        } else {
            cl_mark_closed(h, CL_WHY_DEDICATED, "bonded");
        }
        break;
    }
    case RFCOMM_EVENT_CHANNEL_OPENED: {
        uint16_t cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
        uint8_t st = rfcomm_event_channel_opened_get_status(packet);
        int h = by_cid(cid);
        if (!h) { break; }
        if (st) {
            cl_set_cid(h, 0);
            cl_fail(h, CL_WHY_RFCOMM, "RFCOMM open failed");
            break;
        }
        cl_set_cid(h, cid);
        cl_rfcomm_up(h, rfcomm_event_channel_opened_get_max_frame_size(packet));
        ensure_security(h);
        bt_log("info", "RFCOMM open cid %u", cid);
        break;
    }
    case RFCOMM_EVENT_CHANNEL_CLOSED: {
        int h = by_cid(channel ? channel : rfcomm_event_channel_closed_get_rfcomm_cid(packet));
        int ix;
        if (!h) { break; }
        ix = slot_index(h);
        cl_set_cid(h, 0);
        if (ix >= 0) { s_disc_sent[ix] = 0; }
        if (s_restart == h) {
            cl_info_t in;
            s_restart = 0;
            if (cl_info(h, &in) == CL_OK && in.acl) { gap_disconnect(in.acl); s_restart = h; }
            else { kick(h); }
            break;
        }
        {
            cl_info_t in;
            if (cl_info(h, &in) == CL_OK && (in.state == CL_OPEN || in.state == CL_OPENING)) {
                cl_fail(h, CL_WHY_REMOTE, "rfcomm closed");
            }
        }
        break;
    }
    case RFCOMM_EVENT_CAN_SEND_NOW:
        on_can_send(rfcomm_event_can_send_now_get_rfcomm_cid(packet));
        break;
    default:
        break;
    }
}

static void emit_devices(void) {
    cl_dev_t d;
    while (cl_inq_next_dirty(&d)) {
        char addr[18], name[64];
        char json[160];
        addr_str(d.addr, addr, sizeof addr);
        api_json_escape(name, sizeof name, d.name);
        snprintf(json, sizeof json,
                 "{\"address\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"cod\":%lu}",
                 addr, name, d.rssi, (unsigned long)d.cod);
        api_emit("classic.device", json);
    }
}

static const char *ask_name(int ask) {
    switch (ask) {
    case CL_ASK_PIN: return "pin";
    case CL_ASK_PASSKEY: return "passkey";
    case CL_ASK_CONFIRM: return "confirm";
    default: return "";
    }
}

static const char *what_of(const cl_info_t *in) {
    if (in->state == CL_OPEN) { return "opened"; }
    if (in->state == CL_FAILED) { return "failed"; }
    if (in->state == CL_CLOSING) { return "closed"; }
    return "opening";
}

static void emit_one(int handle, int flags) {
    cl_info_t in;
    char addr[18], err[96], json[320];
    if (cl_info(handle, &in) != CL_OK) { return; }
    addr_str(in.addr, addr, sizeof addr);
    api_json_escape(err, sizeof err, in.error);
    if (flags & CL_EV_STATE) {
        snprintf(json, sizeof json,
                 "{\"handle\":%d,\"what\":\"%s\",\"state\":\"%s\",\"address\":\"%s\","
                 "\"channel\":%u,\"mtu\":%u,\"reason\":\"%s\",\"error\":\"%s\"}",
                 handle, what_of(&in), cl_state_name(in.state), addr,
                 in.channel, in.mtu, cl_why_name(in.why), err);
        api_emit("classic.state", json);
    }
    if (flags & CL_EV_PIN) {
        snprintf(json, sizeof json, "{\"handle\":%d,\"address\":\"%s\"}", handle, addr);
        api_emit("classic.pin", json);
    }
    if (flags & CL_EV_PASSKEY) {
        snprintf(json, sizeof json, "{\"handle\":%d,\"address\":\"%s\"}", handle, addr);
        api_emit("classic.passkey", json);
    }
    if (flags & CL_EV_CONFIRM) {
        snprintf(json, sizeof json, "{\"handle\":%d,\"address\":\"%s\",\"number\":%lu}",
                 handle, addr, (unsigned long)in.confirm_num);
        api_emit("classic.confirm", json);
    }
    if ((flags & CL_EV_RX) && in.watch) {
        uint8_t buf[CL_LISTEN_MAX];
        int n;
        char hex[CL_LISTEN_MAX * 2 + 1];
        if (in.owner) { n = cl_rx_peek(handle, buf, sizeof buf); }
        else { n = cl_listen(handle, buf, sizeof buf); }
        if (n > 0) {
            size_t u = 0;
            for (int i = 0; i < n && u + 2 < sizeof hex; i++) {
                snprintf(hex + u, sizeof hex - u, "%02x", buf[i]);
                u += 2;
            }
            snprintf(json, sizeof json, "{\"handle\":%d,\"hex\":\"%s\"}", handle, hex);
            api_emit("classic.rx", json);
        }
    }
}

void classic_poll(void) {
    int hs[CL_SESS_MAX];
    int n;
    uint32_t now;
    /* The loop ticks this even when cyw43_arch_init failed and bt_init never ran. */
    if (!bt_stack_up()) { return; }
    n = cl_handles(hs, CL_SESS_MAX);
    now = to_ms_since_boot(get_absolute_time());
    cl_tick(now);
    n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) { send_answer(hs[i]); }
    emit_devices();
    n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) {
        int flags = cl_take_ws(hs[i]);
        if (flags) { emit_one(hs[i], flags); }
    }
    n = cl_handles(hs, CL_SESS_MAX);
    for (int i = 0; i < n; i++) { drive_down(hs[i]); }
    restore_io();
    name_pump();
    pump_tx();
}

bool classic_radio_busy(void) {
    return cl_radio_setup() || s_sdp_h || s_sdp_stand;
}

int classic_scan_start(uint32_t duration_ms) {
    uint8_t units = INQUIRY_DEFAULT;
    int rc;
    if (!bt_stack_up()) { return CL_EBUSY; }
    if (cl_inq_active()) { return CL_EBUSY; }
    if (duration_ms) {
        unsigned u = (duration_ms + 1279u) / 1280u;
        if (u < 1) { u = 1; }
        if (u > 0x30) { u = 0x30; }
        units = (uint8_t)u;
    }
    rc = cl_inq_start();
    if (rc) { return rc; }
    if (gap_inquiry_start(units)) {
        cl_inq_stop();
        return CL_EBUSY;
    }
    return CL_OK;
}

void classic_scan_stop(void) {
    if (cl_inq_active() && bt_stack_up()) { gap_inquiry_stop(); }
    cl_inq_stop();
}

int classic_open(const cl_open_t *opt) {
    int blocked, handle, kicked;
    uint32_t now;
    if (!opt) { return CL_EINVAL; }
    if (!bt_stack_up()) { return CL_EBUSY; }
    blocked = radio_block(opt);
    if (blocked) { return blocked; }
    now = to_ms_since_boot(get_absolute_time());
    handle = cl_open(opt, now);
    if (handle < 0) { return handle; }
    kicked = kick(handle);
    if (kicked) {
        cl_close(handle);
        cl_reap(handle);
        return kicked;
    }
    return handle;
}

int classic_send(int handle, const void *data, size_t n) {
    int rc;
    if (!bt_stack_up()) { return CL_EBUSY; }
    rc = cl_tx_put(handle, data, n);
    if (rc > 0) { pump_tx(); }
    return rc;
}

int classic_submit_pin(int handle, const char *pin) { return cl_submit_pin(handle, pin); }
int classic_submit_passkey(int handle, int negative, uint32_t passkey) {
    return cl_submit_passkey(handle, negative, passkey);
}
int classic_submit_confirm(int handle, int accept) { return cl_submit_confirm(handle, accept); }
int classic_close(int handle) { return cl_close(handle); }

int classic_forget(const uint8_t *addr) {
    bd_addr_t a;
    if (!addr) { return CL_EINVAL; }
    if (!bt_stack_up()) { return CL_EBUSY; }
    memcpy(a, addr, 6);
    gap_drop_link_key_for_bd_addr(a);
    return CL_OK;
}

int classic_sdp_start(const uint8_t *addr, uint16_t uuid) {
    if (!bt_stack_up()) { return CL_EBUSY; }
    if (!addr || !uuid) { return CL_EINVAL; }
    if (s_sdp_h || s_sdp_stand || s_sdp_done) { return CL_EBUSY; }
    memcpy(s_sdp_addr, addr, 6);
    s_sdp_uuid = uuid;
    s_sdp_done = 0;
    return begin_sdp(0);
}

int classic_sdp_take(uint8_t addr[6], uint16_t *uuid, uint8_t *channel, int *status) {
    if (!s_sdp_done) { return 0; }
    s_sdp_done = 0;
    if (addr) { memcpy(addr, s_sdp_addr, 6); }
    if (uuid) { *uuid = s_sdp_uuid; }
    if (channel) { *channel = s_sdp_ch; }
    if (status) { *status = s_sdp_status; }
    return 1;
}

void classic_init(void) {
    rfcomm_init();
    sdp_init();
    sdp_client_init();
    s_hci.callback = &packet_handler;
    hci_add_event_handler(&s_hci);
    /* The stack answers the IO capability request from this one value,
     * inside hci.c, before this handler runs. Sessions set it before they
     * page. Idle is no-input, which is what NAP just-works pairing uses.
     * auto-accept stays off so a session can forward a prompt. */
    apply_io(CL_IO_NONE);
    gap_ssp_set_authentication_requirement(
        SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    gap_set_bondable_mode(1);
    gap_set_local_name(CONFIG_NET_HOSTNAME);
    printf("bt: Classic RFCOMM ready (inquiry is classic.scan)\n");
}

static int parse_addr(const char *s, uint8_t *addr) {
    bd_addr_t a;
    if (!s || !sscanf_bd_addr(s, a)) { return 0; }
    memcpy(addr, a, 6);
    return 1;
}

static int parse_pins(const char *json, cl_open_t *o) {
    const char *p, *end;
    if (!json) { return 0; }
    p = strstr(json, "\"pins\"");
    if (!p) { return 0; }
    p = strchr(p, '[');
    if (!p) { return -1; }
    end = strchr(p, ']');
    if (!end) { return -1; }
    while (o->npin < CL_PIN_MAX) {
        const char *q = strchr(p + 1, '"');
        const char *e;
        size_t n;
        if (!q || q > end) { break; }
        e = strchr(q + 1, '"');
        if (!e || e > end) { return -1; }
        n = (size_t)(e - q - 1);
        if (n == 0 || n >= CL_PIN_LEN) { return -1; }
        memcpy(o->pin[o->npin], q + 1, n);
        o->pin[o->npin][n] = 0;
        o->npin++;
        p = e;
    }
    return 0;
}

static int parse_hex(const char *s, uint8_t *out, size_t cap, size_t *n) {
    size_t i = 0, o = 0;
    if (!s) { return -1; }
    while (s[i]) {
        int hi, lo;
        if (s[i] == ' ' || s[i] == ':') { i++; continue; }
        hi = s[i++];
        lo = s[i++];
        if (!lo) { return -1; }
        if (hi >= 'a' && hi <= 'f') { hi -= 'a' - 10; }
        else if (hi >= 'A' && hi <= 'F') { hi -= 'A' - 10; }
        else if (hi >= '0' && hi <= '9') { hi -= '0'; }
        else { return -1; }
        if (lo >= 'a' && lo <= 'f') { lo -= 'a' - 10; }
        else if (lo >= 'A' && lo <= 'F') { lo -= 'A' - 10; }
        else if (lo >= '0' && lo <= '9') { lo -= '0'; }
        else { return -1; }
        if (o >= cap) { return -1; }
        out[o++] = (uint8_t)((hi << 4) | lo);
    }
    *n = o;
    return 0;
}

static int io_of(const char *s, long num, int have_num, uint8_t *io) {
    if (s && s[0]) {
        if (!strcmp(s, "none")) { *io = CL_IO_NONE; return 1; }
        if (!strcmp(s, "display")) { *io = CL_IO_DISPLAY; return 1; }
        if (!strcmp(s, "yesno")) { *io = CL_IO_YESNO; return 1; }
        if (!strcmp(s, "keyboard")) { *io = CL_IO_KEYBOARD; return 1; }
        return 0;
    }
    if (!have_num) { return 1; }
    if (num < 0 || num > CL_IO_KEYBOARD) { return 0; }
    *io = (uint8_t)num;
    return 1;
}

static int bond_of(const char *s, uint8_t *bond) {
    if (!s || !s[0]) { return 1; }
    if (!strcmp(s, "none")) { *bond = CL_BOND_NONE; return 1; }
    if (!strcmp(s, "general")) { *bond = CL_BOND_GENERAL; return 1; }
    if (!strcmp(s, "dedicated")) { *bond = CL_BOND_DEDICATED; return 1; }
    return 0;
}

static void reply_rc(api_reply_fn reply, void *ctx, long id, int rc) {
    if (rc >= 0) { return; }
    if (rc == CL_ENOMEM) { api_reply_err(reply, ctx, id, "nomem", "session table is full"); }
    else if (rc == CL_EBUSY) { api_reply_err(reply, ctx, id, "busy", "the radio cannot start that now"); }
    else if (rc == CL_ENOENT) { api_reply_err(reply, ctx, id, "not_found", "no such session"); }
    else if (rc == CL_ESTATE) { api_reply_err(reply, ctx, id, "bad_state", "session is not waiting for that"); }
    else if (rc == CL_EINVAL) { api_reply_err(reply, ctx, id, "bad_request", "missing address, uuid, or channel"); }
    else { api_reply_err(reply, ctx, id, "failed", "classic"); }
}

static int need_handle(const char *params, api_reply_fn reply, void *ctx, long id, long *handle) {
    if (!params || !api_json_int(params, "handle", handle) || *handle <= 0) {
        api_reply_err(reply, ctx, id, "bad_request", "need handle");
        return 0;
    }
    return 1;
}

void op_classic_scan(const char *params, api_reply_fn reply, void *ctx, long id) {
    bool stop = false;
    long dur = 0;
    int rc;
    if (params) { api_json_bool(params, "stop", &stop); }
    if (stop) {
        classic_scan_stop();
        api_reply_ok(reply, ctx, id, "{\"stopped\":true}");
        return;
    }
    if (params) { api_json_int(params, "duration_ms", &dur); }
    rc = classic_scan_start(dur > 0 ? (uint32_t)dur : 0);
    if (rc) { reply_rc(reply, ctx, id, rc); return; }
    api_reply_ok(reply, ctx, id, "{\"started\":true}");
}

void op_classic_connect(const char *params, api_reply_fn reply, void *ctx, long id) {
    cl_open_t opt;
    char addr[24], io[16], bond[16];
    long level = 0, channel = 0, fallback = 0, uuid = 0, deadline = 0;
    bool drop = false, watch = false;
    int have_level, handle;
    char buf[48];
    memset(&opt, 0, sizeof opt);
    opt.level = CL_LEVEL_OMIT;
    opt.bond = 0xFF;
    addr[0] = io[0] = bond[0] = 0;
    if (!params || !api_json_str(params, "addr", addr, sizeof addr) || !parse_addr(addr, opt.addr)) {
        api_reply_err(reply, ctx, id, "bad_request", "addr is AA:BB:CC:DD:EE:FF");
        return;
    }
    have_level = params && api_json_int(params, "level", &level);
    if (have_level) {
        if (level < 0 || level > 4) {
            api_reply_err(reply, ctx, id, "bad_request", "level is 0..4");
            return;
        }
        opt.level = (uint8_t)level;
    }
    api_json_int(params, "uuid", &uuid);
    api_json_int(params, "channel", &channel);
    api_json_int(params, "fallback", &fallback);
    api_json_int(params, "deadline_ms", &deadline);
    api_json_bool(params, "drop_key", &drop);
    api_json_bool(params, "watch", &watch);
    api_json_str(params, "io", io, sizeof io);
    api_json_str(params, "bond", bond, sizeof bond);
    if (uuid < 0 || uuid > 0xFFFF || channel < 0 || channel > 30 || fallback < 0 || fallback > 30) {
        api_reply_err(reply, ctx, id, "bad_request", "uuid or channel is out of range");
        return;
    }
    opt.uuid = (uint16_t)uuid;
    opt.channel = (uint8_t)channel;
    opt.fallback = (uint8_t)fallback;
    opt.drop_key = drop ? 1 : 0;
    opt.watch = watch ? 1 : 0;
    if (deadline > 0) { opt.deadline_ms = (uint32_t)deadline; }
    {
        long io_num = 0;
        int have_io = api_json_int(params, "io", &io_num);
        if (!io_of(io, io_num, have_io && !io[0], &opt.io)) {
            api_reply_err(reply, ctx, id, "bad_request", "io is none, display, yesno, or keyboard");
            return;
        }
    }
    if (!bond_of(bond, &opt.bond)) {
        api_reply_err(reply, ctx, id, "bad_request", "bond is none, general, or dedicated");
        return;
    }
    if (parse_pins(params, &opt)) {
        api_reply_err(reply, ctx, id, "bad_request", "pins is a list of up to four strings");
        return;
    }
    handle = classic_open(&opt);
    if (handle < 0) { reply_rc(reply, ctx, id, handle); return; }
    snprintf(buf, sizeof buf, "{\"handle\":%d}", handle);
    api_reply_ok(reply, ctx, id, buf);
}

static int append_session(char *b, size_t cap, size_t *used, int handle, int comma) {
    cl_info_t in;
    char addr[18], err[96];
    int m;
    if (cl_info(handle, &in) != CL_OK) { return 0; }
    addr_str(in.addr, addr, sizeof addr);
    api_json_escape(err, sizeof err, in.error);
    m = snprintf(b + *used, cap - *used,
                 "%s{\"handle\":%d,\"address\":\"%s\",\"state\":\"%s\",\"uuid\":%u,"
                 "\"channel\":%u,\"mtu\":%u,\"cod\":%lu,\"rx_drop\":%lu,\"level\":%u,"
                 "\"ask\":\"%s\",\"reason\":\"%s\",\"error\":\"%s\"}",
                 comma ? "," : "", handle, addr, cl_state_name(in.state), in.uuid,
                 in.channel, in.mtu, (unsigned long)in.cod, (unsigned long)in.rx_drop,
                 in.level, ask_name(in.ask), cl_why_name(in.why), err);
    if (m < 0 || *used + (size_t)m >= cap) { return -1; }
    *used += (size_t)m;
    return 0;
}

void op_classic_state(const char *params, api_reply_fn reply, void *ctx, long id) {
    long handle = 0;
    char *b = api_scratch();
    size_t cap = api_scratch_size();
    size_t used;
    int n;
    if (params && api_json_int(params, "handle", &handle) && handle > 0) {
        cl_info_t in;
        if (cl_info((int)handle, &in) != CL_OK) {
            api_reply_err(reply, ctx, id, "not_found", "no such session");
            return;
        }
        used = 0;
        if (append_session(b, cap, &used, (int)handle, 0)) {
            api_reply_err(reply, ctx, id, "too_large", "state");
            return;
        }
        api_reply_ok(reply, ctx, id, b);
        return;
    }
    {
        const char *top = cl_inq_active() ? "scanning" : "idle";
        int hs[CL_SESS_MAX];
        int ns = cl_handles(hs, CL_SESS_MAX);
        int any_open = 0, any_opening = 0;
        for (int i = 0; i < ns; i++) {
            cl_info_t in;
            if (cl_info(hs[i], &in) != CL_OK) { continue; }
            if (in.state == CL_OPEN) { any_open = 1; }
            if (in.state == CL_OPENING) { any_opening = 1; }
        }
        if (!cl_inq_active() && any_open) { top = "open"; }
        else if (!cl_inq_active() && any_opening) { top = "opening"; }
        n = snprintf(b, cap, "{\"state\":\"%s\",\"devices\":[", top);
        if (n < 0 || (size_t)n >= cap) {
            api_reply_err(reply, ctx, id, "too_large", "state");
            return;
        }
        used = (size_t)n;
        for (int i = 0; i < cl_inq_count(); i++) {
            cl_dev_t d;
            char addr[18], name[64];
            int m;
            if (cl_inq_get(i, &d) != CL_OK) { break; }
            addr_str(d.addr, addr, sizeof addr);
            api_json_escape(name, sizeof name, d.name);
            m = snprintf(b + used, cap - used,
                         "%s{\"address\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"cod\":%lu}",
                         i ? "," : "", addr, name, d.rssi, (unsigned long)d.cod);
            if (m < 0 || used + (size_t)m >= cap) { break; }
            used += (size_t)m;
        }
        n = snprintf(b + used, cap - used, "],\"sessions\":[");
        if (n < 0 || used + (size_t)n >= cap) {
            api_reply_err(reply, ctx, id, "too_large", "state");
            return;
        }
        used += (size_t)n;
        for (int i = 0; i < ns; i++) {
            if (append_session(b, cap, &used, hs[i], i > 0)) {
                api_reply_err(reply, ctx, id, "too_large", "state");
                return;
            }
        }
        snprintf(b + used, cap - used, "]}");
        api_reply_ok(reply, ctx, id, b);
    }
}

void op_classic_disconnect(const char *params, api_reply_fn reply, void *ctx, long id) {
    long handle = 0;
    int rc;
    if (!need_handle(params, reply, ctx, id, &handle)) { return; }
    rc = classic_close((int)handle);
    if (rc) { reply_rc(reply, ctx, id, rc); return; }
    api_reply_ok(reply, ctx, id, "{\"closing\":true}");
}

void op_classic_send(const char *params, api_reply_fn reply, void *ctx, long id) {
    long handle = 0;
    char hex[HEX_MAX * 2 + 1];
    size_t n = 0;
    int rc;
    char buf[32];
    if (!need_handle(params, reply, ctx, id, &handle)) { return; }
    hex[0] = 0;
    if (!params || !api_json_str(params, "hex", hex, sizeof hex) || parse_hex(hex, s_hex, sizeof s_hex, &n) || !n) {
        api_reply_err(reply, ctx, id, "bad_request", "hex payload");
        return;
    }
    rc = classic_send((int)handle, s_hex, n);
    if (rc < 0) {
        if (rc == CL_EBUSY) {
            api_reply_err(reply, ctx, id, "busy", "the link is not ready or the buffer is full");
        } else {
            reply_rc(reply, ctx, id, rc);
        }
        return;
    }
    snprintf(buf, sizeof buf, "{\"sent\":%d}", rc);
    api_reply_ok(reply, ctx, id, buf);
}

void op_classic_pin(const char *params, api_reply_fn reply, void *ctx, long id) {
    long handle = 0;
    char pin[CL_PIN_LEN];
    bool neg = false;
    int rc;
    if (!need_handle(params, reply, ctx, id, &handle)) { return; }
    pin[0] = 0;
    if (params) { api_json_bool(params, "negative", &neg); }
    if (params) { api_json_str(params, "pin", pin, sizeof pin); }
    rc = classic_submit_pin((int)handle, (neg || !pin[0]) ? NULL : pin);
    if (rc) { reply_rc(reply, ctx, id, rc); return; }
    api_reply_ok(reply, ctx, id, "{\"ok\":true}");
}

void op_classic_passkey(const char *params, api_reply_fn reply, void *ctx, long id) {
    long handle = 0, passkey = 0;
    bool neg = false;
    int rc;
    if (!need_handle(params, reply, ctx, id, &handle)) { return; }
    if (params) { api_json_bool(params, "negative", &neg); }
    if (!neg && (!params || !api_json_int(params, "passkey", &passkey))) {
        api_reply_err(reply, ctx, id, "bad_request", "need passkey");
        return;
    }
    rc = classic_submit_passkey((int)handle, neg, (uint32_t)passkey);
    if (rc) { reply_rc(reply, ctx, id, rc); return; }
    api_reply_ok(reply, ctx, id, "{\"ok\":true}");
}

void op_classic_confirm(const char *params, api_reply_fn reply, void *ctx, long id) {
    long handle = 0;
    bool accept = false;
    int rc;
    if (!need_handle(params, reply, ctx, id, &handle)) { return; }
    if (!params || !api_json_bool(params, "accept", &accept)) {
        api_reply_err(reply, ctx, id, "bad_request", "need accept");
        return;
    }
    rc = classic_submit_confirm((int)handle, accept);
    if (rc) { reply_rc(reply, ctx, id, rc); return; }
    api_reply_ok(reply, ctx, id, "{\"ok\":true}");
}
