/*
 * Unit tests for the Phase 5 hardening features: bulk operations
 * (vlan/lag/nhg members, fdb and route entries) with both error modes,
 * the NH MAC refresh (BCM_L3_REPLACE) and bridge-port admin state.
 */
#include <stdio.h>
#include <string.h>

#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_l3.h"
#include "mock/mock_bcm.h"

static int failures = 0;

#define CHECK(cond, ...) do {                        \
    if (!(cond)) {                                   \
        fprintf(stderr, "FAIL %s:%d: ",              \
                __FILE__, __LINE__);                 \
        fprintf(stderr, __VA_ARGS__);               \
        fprintf(stderr, "\n");                       \
        failures++;                                  \
    }                                                \
} while (0)

static const sai_switch_api_t *sw;
static const sai_vlan_api_t *vlan;
static const sai_lag_api_t *lag;
static const sai_next_hop_group_api_t *nhg;
static const sai_next_hop_api_t *nh;
static const sai_fdb_api_t *fdb;
static const sai_route_api_t *route;
static const sai_neighbor_api_t *nbr;
static const sai_router_interface_api_t *rif;
static const sai_bridge_api_t *bridge;

static sai_object_id_t sid;
static sai_object_id_t ports[MOCK_BCM_FRONT_PANEL_PORTS];
static sai_object_id_t bridge_ports[MOCK_BCM_FRONT_PANEL_PORTS];
static sai_object_id_t default_vr;

static sai_attribute_t mkattr(sai_attr_id_t id)
{
    sai_attribute_t a = {0};
    a.id = id;
    return a;
}

static void setup(void)
{
    mock_bcm_reset();
    memset(&g_sai_bcm_sw, 0, sizeof(g_sai_bcm_sw));
    g_sai_bcm_sw.unit = SAI_BCM_UNIT_DEFAULT;
    sai_bcm_idx_reset_all();

    CHECK(sai_api_initialize(0, NULL) == SAI_STATUS_SUCCESS, "init");
    CHECK(sai_api_query(SAI_API_SWITCH, (void **)&sw) == SAI_STATUS_SUCCESS,
          "q sw");
    CHECK(sai_api_query(SAI_API_VLAN, (void **)&vlan) == SAI_STATUS_SUCCESS,
          "q vlan");
    CHECK(sai_api_query(SAI_API_LAG, (void **)&lag) == SAI_STATUS_SUCCESS,
          "q lag");
    CHECK(sai_api_query(SAI_API_NEXT_HOP_GROUP, (void **)&nhg) ==
          SAI_STATUS_SUCCESS, "q nhg");
    CHECK(sai_api_query(SAI_API_NEXT_HOP, (void **)&nh) == SAI_STATUS_SUCCESS,
          "q nh");
    CHECK(sai_api_query(SAI_API_FDB, (void **)&fdb) == SAI_STATUS_SUCCESS,
          "q fdb");
    CHECK(sai_api_query(SAI_API_ROUTE, (void **)&route) == SAI_STATUS_SUCCESS,
          "q route");
    CHECK(sai_api_query(SAI_API_NEIGHBOR, (void **)&nbr) ==
          SAI_STATUS_SUCCESS, "q nbr");
    CHECK(sai_api_query(SAI_API_ROUTER_INTERFACE, (void **)&rif) ==
          SAI_STATUS_SUCCESS, "q rif");
    CHECK(sai_api_query(SAI_API_BRIDGE, (void **)&bridge) ==
          SAI_STATUS_SUCCESS, "q bridge");

    CHECK(sw->create_switch(&sid, 0, NULL) == SAI_STATUS_SUCCESS, "switch");

    sai_attribute_t pl = mkattr(SAI_SWITCH_ATTR_PORT_LIST);
    pl.value.objlist.count = MOCK_BCM_FRONT_PANEL_PORTS;
    pl.value.objlist.list = ports;
    CHECK(sw->get_switch_attribute(sid, 1, &pl) == SAI_STATUS_SUCCESS,
          "plist");

    sai_attribute_t bl = mkattr(SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID);
    CHECK(sw->get_switch_attribute(sid, 1, &bl) == SAI_STATUS_SUCCESS,
          "dbridge");
    sai_attribute_t bpl = mkattr(SAI_BRIDGE_ATTR_PORT_LIST);
    bpl.value.objlist.count = MOCK_BCM_FRONT_PANEL_PORTS;
    bpl.value.objlist.list = bridge_ports;
    CHECK(bridge->get_bridge_attribute(bl.value.oid, 1, &bpl) ==
          SAI_STATUS_SUCCESS, "bridge ports");

    sai_attribute_t dv = mkattr(SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID);
    CHECK(sw->get_switch_attribute(sid, 1, &dv) == SAI_STATUS_SUCCESS, "dvr");
    default_vr = dv.value.oid;
}

int main(void)
{
    setup();

    /* ================= bulk vlan members ================= */
    sai_attribute_t va = mkattr(SAI_VLAN_ATTR_VLAN_ID);
    va.value.u16 = 300;
    sai_object_id_t vlan300 = SAI_NULL_OBJECT_ID;
    CHECK(vlan->create_vlan(&vlan300, sid, 1, &va) == SAI_STATUS_SUCCESS,
          "create vlan 300");

    /* member attr sets: 0,1 valid on bridge ports 3,4; 2 invalid (bad vlan) */
    sai_attribute_t attrs[3][3];
    for (int i = 0; i < 3; i++) {
        attrs[i][0] = mkattr(SAI_VLAN_MEMBER_ATTR_VLAN_ID);
        attrs[i][0].value.oid = vlan300;
        attrs[i][1] = mkattr(SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID);
        attrs[i][1].value.oid = bridge_ports[3 + i];
        attrs[i][2] = mkattr(SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE);
        attrs[i][2].value.s32 = SAI_VLAN_TAGGING_MODE_TAGGED;
    }
    /* make #2 reference a vlan that does not exist */
    attrs[2][0].value.oid = sai_bcm_vlan_oid(9999);

    const uint32_t counts[3] = {3, 3, 3};
    const sai_attribute_t *lists[3] = {attrs[0], attrs[1], attrs[2]};
    sai_object_id_t ids[3];
    sai_status_t stt[3];

    sai_status_t st = vlan->create_vlan_members(sid, 3, counts, lists,
                                                SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
                                                ids, stt);
    CHECK(st == SAI_STATUS_SUCCESS, "bulk vlan create -> 0x%x", st);
    CHECK(stt[0] == SAI_STATUS_SUCCESS && stt[1] == SAI_STATUS_SUCCESS,
          "first two members created");
    CHECK(stt[2] == SAI_STATUS_ITEM_NOT_FOUND,
          "third member fails (0x%x)", stt[2]);
    CHECK(ids[0] != SAI_NULL_OBJECT_ID && ids[1] != SAI_NULL_OBJECT_ID,
          "valid ids returned");
    CHECK(mock_bcm_vlan_member_count(300) == 2, "two members in hw");

    /* STOP_ON_ERROR: first object fails, rest NOT_EXECUTED */
    const uint32_t counts2[2] = {3, 3};
    const sai_attribute_t *lists2[2] = {attrs[2], attrs[0]};
    sai_object_id_t ids2[2];
    sai_status_t stt2[2];
    st = vlan->create_vlan_members(sid, 2, counts2, lists2,
                                   SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR,
                                   ids2, stt2);
    CHECK(st == SAI_STATUS_SUCCESS, "bulk stop-mode ran");
    CHECK(stt2[0] == SAI_STATUS_ITEM_NOT_FOUND, "first fails");
    CHECK(stt2[1] == SAI_STATUS_NOT_EXECUTED,
          "second NOT_EXECUTED (0x%x)", stt2[1]);

    /* bulk remove (ignore mode, one bad oid) */
    const sai_object_id_t rm_ids[3] = {ids[0], ids[1], ids[1] /* dup */};
    st = vlan->remove_vlan_members(3, rm_ids,
                                   SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, stt);
    CHECK(st == SAI_STATUS_SUCCESS, "bulk vlan remove ran");
    CHECK(stt[0] == SAI_STATUS_SUCCESS && stt[1] == SAI_STATUS_SUCCESS,
          "members removed");
    CHECK(stt[2] == SAI_STATUS_ITEM_NOT_FOUND, "dup remove fails");
    CHECK(mock_bcm_vlan_member_count(300) == 0, "hw empty");

    /* ================= bulk lag members ================= */
    sai_object_id_t lag0 = SAI_NULL_OBJECT_ID;
    CHECK(lag->create_lag(&lag0, sid, 0, NULL) == SAI_STATUS_SUCCESS, "lag");

    sai_attribute_t lm[2][2];
    for (int i = 0; i < 2; i++) {
        lm[i][0] = mkattr(SAI_LAG_MEMBER_ATTR_LAG_ID);
        lm[i][0].value.oid = lag0;
        lm[i][1] = mkattr(SAI_LAG_MEMBER_ATTR_PORT_ID);
        lm[i][1].value.oid = ports[10 + i];
    }
    const uint32_t lcounts[2] = {2, 2};
    const sai_attribute_t *llists[2] = {lm[0], lm[1]};
    sai_object_id_t lids[2];
    sai_status_t lst[2];
    st = lag->create_lag_members(sid, 2, lcounts, llists,
                                 SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR,
                                 lids, lst);
    CHECK(st == SAI_STATUS_SUCCESS, "bulk lag create ran");
    CHECK(lst[0] == SAI_STATUS_SUCCESS && lst[1] == SAI_STATUS_SUCCESS,
          "lag members created");
    CHECK(mock_bcm_trunk_member_count(0) == 2, "trunk has 2");

    const sai_object_id_t lrm[2] = {lids[0], lids[1]};
    st = lag->remove_lag_members(2, lrm,
                                SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, lst);
    CHECK(st == SAI_STATUS_SUCCESS && lst[0] == SAI_STATUS_SUCCESS &&
          lst[1] == SAI_STATUS_SUCCESS, "bulk lag remove");
    CHECK(mock_bcm_trunk_member_count(0) == 0, "trunk empty");
    CHECK(lag->remove_lag(lag0) == SAI_STATUS_SUCCESS, "lag removed");

    /* ================= bulk nhg members ================= */
    sai_object_id_t grp = SAI_NULL_OBJECT_ID;
    CHECK(nhg->create_next_hop_group(&grp, sid, 0, NULL) ==
          SAI_STATUS_SUCCESS, "nhg");

    /* need a RIF + two NHs */
    sai_attribute_t rattrs[3];
    rattrs[0] = mkattr(SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID);
    rattrs[0].value.oid = default_vr;
    rattrs[1] = mkattr(SAI_ROUTER_INTERFACE_ATTR_TYPE);
    rattrs[1].value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
    rattrs[2] = mkattr(SAI_ROUTER_INTERFACE_ATTR_PORT_ID);
    rattrs[2].value.oid = ports[0];
    sai_object_id_t rif0 = SAI_NULL_OBJECT_ID;
    CHECK(rif->create_router_interface(&rif0, sid, 3, rattrs) ==
          SAI_STATUS_SUCCESS, "rif");

    sai_ip_address_t ip = {.addr_family = SAI_IP_ADDR_FAMILY_IPV4,
                           .addr = {.ip4 = 0x0A000001}};
    sai_attribute_t nha[3];
    nha[0] = mkattr(SAI_NEXT_HOP_ATTR_TYPE);
    nha[0].value.s32 = SAI_NEXT_HOP_TYPE_IP;
    nha[1] = mkattr(SAI_NEXT_HOP_ATTR_IP);
    nha[1].value.ipaddr = ip;
    nha[2] = mkattr(SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID);
    nha[2].value.oid = rif0;

    sai_object_id_t nh_a = SAI_NULL_OBJECT_ID, nh_b = SAI_NULL_OBJECT_ID;
    CHECK(nh->create_next_hop(&nh_a, sid, 3, nha) == SAI_STATUS_SUCCESS, "nh a");
    nha[1].value.ipaddr.addr.ip4 = 0x0A000002;
    CHECK(nh->create_next_hop(&nh_b, sid, 3, nha) == SAI_STATUS_SUCCESS,
          "nh b");

    sai_attribute_t gm[2][2];
    gm[0][0] = mkattr(SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID);
    gm[0][0].value.oid = grp;
    gm[0][1] = mkattr(SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID);
    gm[0][1].value.oid = nh_a;
    gm[1][0] = gm[0][0];
    gm[1][1] = gm[0][1];
    gm[1][1].value.oid = nh_b;

    const uint32_t gcounts[2] = {2, 2};
    const sai_attribute_t *glists[2] = {gm[0], gm[1]};
    sai_object_id_t gids[2];
    sai_status_t gst[2];
    st = nhg->create_next_hop_group_members(sid, 2, gcounts, glists,
                                            SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR,
                                            gids, gst);
    CHECK(st == SAI_STATUS_SUCCESS && gst[0] == SAI_STATUS_SUCCESS &&
          gst[1] == SAI_STATUS_SUCCESS, "bulk nhg members");
    CHECK(mock_bcm_l3_ecmp_member_count(0) == 2, "ecmp 2 members");

    const sai_object_id_t grm[2] = {gids[0], gids[1]};
    st = nhg->remove_next_hop_group_members(2, grm,
                                            SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
                                            gst);
    CHECK(st == SAI_STATUS_SUCCESS && gst[0] == SAI_STATUS_SUCCESS &&
          gst[1] == SAI_STATUS_SUCCESS, "bulk nhg remove");
    CHECK(nhg->remove_next_hop_group(grp) == SAI_STATUS_SUCCESS, "nhg removed");

    /* ================= bulk fdb ================= */
    sai_fdb_entry_t ents[3];
    memset(ents, 0, sizeof(ents));
    for (int i = 0; i < 3; i++) {
        ents[i].switch_id = sid;
        ents[i].bv_id = vlan300;
        ents[i].mac_address[0] = 0x00;
        ents[i].mac_address[1] = 0x11;
        ents[i].mac_address[5] = (uint8_t)(0x50 + i);
    }

    sai_attribute_t fattr[3];
    for (int i = 0; i < 3; i++) {
        fattr[i] = mkattr(SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID);
        fattr[i].value.oid = bridge_ports[i];
    }
    const uint32_t fcounts[3] = {1, 1, 1};
    const sai_attribute_t *flists[3] = {&fattr[0], &fattr[1], &fattr[2]};
    sai_status_t fst[3];

    st = fdb->create_fdb_entries(3, ents, fcounts, flists,
                                 SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, fst);
    CHECK(st == SAI_STATUS_SUCCESS, "bulk fdb create ran");
    CHECK(fst[0] == SAI_STATUS_SUCCESS && fst[1] == SAI_STATUS_SUCCESS &&
          fst[2] == SAI_STATUS_SUCCESS, "fdb entries created");
    CHECK(mock_bcm_l2_count() == 3, "three l2 entries");

    /* bulk get */
    sai_attribute_t gattr[3];
    for (int i = 0; i < 3; i++) {
        gattr[i] = mkattr(SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID);
    }
    sai_attribute_t *gptrs[3] = {&gattr[0], &gattr[1], &gattr[2]};
    const uint32_t gcounts2[3] = {1, 1, 1};
    st = fdb->get_fdb_entries_attribute(3, ents, gcounts2, gptrs,
                                        SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
                                        fst);
    CHECK(st == SAI_STATUS_SUCCESS && fst[0] == SAI_STATUS_SUCCESS,
          "bulk fdb get");
    CHECK(gattr[1].value.oid == bridge_ports[1], "fdb get value");

    /* bulk set (move all to bridge port 5) */
    sai_attribute_t sattr[3];
    for (int i = 0; i < 3; i++) {
        sattr[i] = mkattr(SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID);
        sattr[i].value.oid = bridge_ports[5];
    }
    st = fdb->set_fdb_entries_attribute(3, ents, sattr,
                                        SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
                                        fst);
    CHECK(st == SAI_STATUS_SUCCESS && fst[0] == SAI_STATUS_SUCCESS &&
          fst[2] == SAI_STATUS_SUCCESS, "bulk fdb set");

    /* bulk remove (stop mode with a missing entry in the middle) */
    CHECK(fdb->remove_fdb_entry(&ents[1]) == SAI_STATUS_SUCCESS,
          "remove one entry");
    CHECK(mock_bcm_l2_count() == 2, "two entries left");

    st = fdb->remove_fdb_entries(3, ents,
                                 SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, fst);
    CHECK(st == SAI_STATUS_SUCCESS, "bulk fdb remove ran");
    CHECK(fst[0] == SAI_STATUS_SUCCESS, "first removed");
    CHECK(fst[1] == SAI_STATUS_ITEM_NOT_FOUND, "second missing");
    CHECK(fst[2] == SAI_STATUS_NOT_EXECUTED, "third not executed");
    CHECK(mock_bcm_l2_count() == 1, "one entry left");
    CHECK(fdb->remove_fdb_entry(&ents[2]) == SAI_STATUS_SUCCESS, "cleanup");

    /* ================= bulk routes ================= */
    sai_route_entry_t rt[3];
    memset(rt, 0, sizeof(rt));
    for (int i = 0; i < 3; i++) {
        rt[i].switch_id = sid;
        rt[i].vr_id = default_vr;
        rt[i].destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
        rt[i].destination.addr.ip4 = 0xC0A80000 + (uint32_t)i;
        rt[i].destination.mask.ip4 = 0xFFFFFF00;
    }
    sai_attribute_t rattr[3];
    for (int i = 0; i < 3; i++) {
        rattr[i] = mkattr(SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
        rattr[i].value.oid = nh_a;
    }
    const uint32_t rcounts[3] = {1, 1, 1};
    const sai_attribute_t *rlists[3] = {&rattr[0], &rattr[1], &rattr[2]};
    sai_status_t rst[3];

    st = route->create_route_entries(3, rt, rcounts, rlists,
                                     SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
                                     rst);
    CHECK(st == SAI_STATUS_SUCCESS && rst[0] == SAI_STATUS_SUCCESS &&
          rst[1] == SAI_STATUS_SUCCESS && rst[2] == SAI_STATUS_SUCCESS,
          "bulk route create");
    CHECK(mock_bcm_l3_route_count(0) == 3, "three routes");

    /* duplicate route in the middle -> stop mode */
    st = route->create_route_entries(2, rt, rcounts, rlists,
                                     SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR,
                                     rst);
    CHECK(st == SAI_STATUS_SUCCESS && rst[0] == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "dup route detected");
    CHECK(rst[1] == SAI_STATUS_NOT_EXECUTED, "second not executed");

    st = route->remove_route_entries(3, rt,
                                     SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR,
                                     rst);
    CHECK(st == SAI_STATUS_SUCCESS && rst[0] == SAI_STATUS_SUCCESS &&
          rst[1] == SAI_STATUS_SUCCESS && rst[2] == SAI_STATUS_SUCCESS,
          "bulk route remove");
    CHECK(mock_bcm_l3_route_count(0) == 0, "routes gone");

    /* ================= NH MAC refresh (BCM_L3_REPLACE) ================= */
    /* NH created before neighbor: egress MAC starts zeroed. */
    uint8_t mac_out[6] = {0};
    CHECK(mock_bcm_l3_egress_mac(0, mac_out) == 1, "egress 0 exists");
    CHECK(mac_out[0] == 0 && mac_out[1] == 0, "mac placeholder zero");

    /* Create the neighbor: the NH egress must be re-programmed in place. */
    sai_neighbor_entry_t ne;
    memset(&ne, 0, sizeof(ne));
    ne.switch_id = sid;
    ne.rif_id = rif0;
    ne.ip_address = ip;

    uint8_t nbmac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    sai_attribute_t nattr = mkattr(SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS);
    memcpy(nattr.value.mac, nbmac, 6);
    CHECK(nbr->create_neighbor_entry(&ne, 1, &nattr) == SAI_STATUS_SUCCESS,
          "neighbor create");

    CHECK(mock_bcm_l3_egress_mac(0, mac_out) == 1, "egress 0 still exists");
    CHECK(memcmp(mac_out, nbmac, 6) == 0,
          "NH egress MAC refreshed in place (id stable)");

    /* Neighbor MAC change updates the egress object too. */
    uint8_t nbmac2[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x99};
    memcpy(nattr.value.mac, nbmac2, 6);
    CHECK(nbr->set_neighbor_entry_attribute(&ne, &nattr) ==
          SAI_STATUS_SUCCESS, "neighbor set");
    CHECK(mock_bcm_l3_egress_mac(0, mac_out) == 1, "egress exists after set");
    CHECK(memcmp(mac_out, nbmac2, 6) == 0, "NH egress MAC updated");

    /* ================= bridge port admin state ================= */
    sai_attribute_t admin = mkattr(SAI_BRIDGE_PORT_ATTR_ADMIN_STATE);
    admin.value.booldata = false;
    st = bridge->set_bridge_port_attribute(bridge_ports[7], &admin);
    CHECK(st == SAI_STATUS_SUCCESS, "bridge port admin off");

    sai_attribute_t pen = mkattr(SAI_PORT_ATTR_ADMIN_STATE);
    const sai_port_api_t *port_api;
    CHECK(sai_api_query(SAI_API_PORT, (void **)&port_api) ==
          SAI_STATUS_SUCCESS, "q port");
    CHECK(port_api->get_port_attribute(ports[7], 1, &pen) ==
          SAI_STATUS_SUCCESS, "get port admin");
    CHECK(!pen.value.booldata, "port disabled through bridge port");

    admin.value.booldata = true;
    CHECK(bridge->set_bridge_port_attribute(bridge_ports[7], &admin) ==
          SAI_STATUS_SUCCESS, "bridge port admin on");
    pen = mkattr(SAI_PORT_ATTR_ADMIN_STATE);
    CHECK(port_api->get_port_attribute(ports[7], 1, &pen) ==
          SAI_STATUS_SUCCESS, "get port admin 2");
    CHECK(pen.value.booldata, "port re-enabled");

    /* ================= availability ================= */
    uint64_t avail = 0;
    st = sai_object_type_get_availability(sid, SAI_OBJECT_TYPE_VLAN, 0, NULL,
                                         &avail);
    CHECK(st == SAI_STATUS_SUCCESS, "vlan availability");
    CHECK(avail == (uint64_t)(SAI_BCM_VLAN_MAX - SAI_BCM_VLAN_MIN + 1) - 1,
          "vlan slots minus vlan 300 (%lu)", (unsigned long)avail);

    st = sai_object_type_get_availability(sid, SAI_OBJECT_TYPE_NEXT_HOP, 0,
                                          NULL, &avail);
    CHECK(st == SAI_STATUS_SUCCESS && avail == 1022,
          "nh availability 1022 (%lu)", (unsigned long)avail);

    if (failures == 0) {
        printf("test_bulk: OK\n");
        return 0;
    }
    printf("test_bulk: %d FAILURES\n", failures);
    return 1;
}