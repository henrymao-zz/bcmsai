/*
 * Unit tests for the SAI VLAN / Bridge / LAG / FDB APIs over the mock
 * BCM backend.
 */
#include <stdio.h>
#include <string.h>

#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
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

static const sai_switch_api_t *sw = NULL;
static const sai_port_api_t *port = NULL;
static const sai_vlan_api_t *vlan = NULL;
static const sai_bridge_api_t *bridge = NULL;
static const sai_lag_api_t *lag = NULL;
static const sai_fdb_api_t *fdb = NULL;

static sai_object_id_t sid;
static sai_object_id_t ports[MOCK_BCM_FRONT_PANEL_PORTS];
static sai_object_id_t bridge_ports[MOCK_BCM_FRONT_PANEL_PORTS];

static void setup(void)
{
    mock_bcm_reset();
    memset(&g_sai_bcm_sw, 0, sizeof(g_sai_bcm_sw));
    g_sai_bcm_sw.unit = SAI_BCM_UNIT_DEFAULT;
    sai_bcm_idx_reset_all();

    CHECK(sai_api_initialize(0, NULL) == SAI_STATUS_SUCCESS, "init");
    CHECK(sai_api_query(SAI_API_SWITCH, (void **)&sw) == SAI_STATUS_SUCCESS,
          "query switch");
    CHECK(sai_api_query(SAI_API_PORT, (void **)&port) == SAI_STATUS_SUCCESS,
          "query port");
    CHECK(sai_api_query(SAI_API_VLAN, (void **)&vlan) == SAI_STATUS_SUCCESS,
          "query vlan");
    CHECK(sai_api_query(SAI_API_BRIDGE, (void **)&bridge) ==
          SAI_STATUS_SUCCESS, "query bridge");
    CHECK(sai_api_query(SAI_API_LAG, (void **)&lag) == SAI_STATUS_SUCCESS,
          "query lag");
    CHECK(sai_api_query(SAI_API_FDB, (void **)&fdb) == SAI_STATUS_SUCCESS,
          "query fdb");

    CHECK(sw->create_switch(&sid, 0, NULL) == SAI_STATUS_SUCCESS,
          "create switch");

    sai_attribute_t lattr = {0};
    lattr.id = SAI_SWITCH_ATTR_PORT_LIST;
    lattr.value.objlist.count = MOCK_BCM_FRONT_PANEL_PORTS;
    lattr.value.objlist.list = ports;
    CHECK(sw->get_switch_attribute(sid, 1, &lattr) == SAI_STATUS_SUCCESS,
          "port list");

    /* default 1Q bridge */
    sai_attribute_t battr = {0};
    battr.id = SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID;
    CHECK(sw->get_switch_attribute(sid, 1, &battr) == SAI_STATUS_SUCCESS,
          "get DEFAULT_1Q_BRIDGE_ID");
    CHECK(battr.value.oid != SAI_NULL_OBJECT_ID, "default bridge non-null");

    /* bridge ports on the default 1Q bridge */
    sai_attribute_t bpattr = {0};
    bpattr.id = SAI_BRIDGE_ATTR_PORT_LIST;
    bpattr.value.objlist.count = MOCK_BCM_FRONT_PANEL_PORTS;
    bpattr.value.objlist.list = bridge_ports;
    CHECK(bridge->get_bridge_attribute(battr.value.oid, 1, &bpattr) ==
          SAI_STATUS_SUCCESS, "default bridge port list");
    CHECK(bpattr.value.objlist.count == MOCK_BCM_FRONT_PANEL_PORTS,
          "default bridge has one port each (%u)",
          bpattr.value.objlist.count);
}

static sai_attribute_t mkattr(sai_attr_id_t id)
{
    sai_attribute_t a = {0};
    a.id = id;
    return a;
}

int main(void)
{
    setup();

    /* ================= VLAN lifecycle ================= */
    sai_attribute_t vattr = mkattr(SAI_VLAN_ATTR_VLAN_ID);
    vattr.value.u16 = 100;
    sai_object_id_t vlan100 = SAI_NULL_OBJECT_ID;
    sai_status_t st = vlan->create_vlan(&vlan100, sid, 1, &vattr);
    CHECK(st == SAI_STATUS_SUCCESS, "create vlan 100 -> 0x%x", st);
    CHECK(vlan100 != SAI_NULL_OBJECT_ID, "vlan oid non-null");
    CHECK(mock_bcm_vlan_used(100), "bcm vlan 100 exists");

    /* duplicate */
    sai_object_id_t dup = SAI_NULL_OBJECT_ID;
    st = vlan->create_vlan(&dup, sid, 1, &vattr);
    CHECK(st == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "dup vlan -> 0x%x (expected ALREADY_EXISTS)", st);

    /* invalid vid */
    sai_attribute_t vbad = mkattr(SAI_VLAN_ATTR_VLAN_ID);
    vbad.value.u16 = 5000;
    sai_object_id_t vbadid = SAI_NULL_OBJECT_ID;
    st = vlan->create_vlan(&vbadid, sid, 1, &vbad);
    CHECK(st == SAI_STATUS_INVALID_PARAMETER, "vid 5000 -> 0x%x", st);

    /* get vlan id back */
    sai_attribute_t gattr = mkattr(SAI_VLAN_ATTR_VLAN_ID);
    CHECK(vlan->get_vlan_attribute(vlan100, 1, &gattr) == SAI_STATUS_SUCCESS,
          "get vlan attr");
    CHECK(gattr.value.u16 == 100, "vlan id round-trip");

    /* ================= VLAN members ================= */
    /* Member on bridge port 0, untagged. */
    sai_attribute_t mattrs[3];
    mattrs[0] = mkattr(SAI_VLAN_MEMBER_ATTR_VLAN_ID);
    mattrs[0].value.oid = vlan100;
    mattrs[1] = mkattr(SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID);
    mattrs[1].value.oid = bridge_ports[0];
    mattrs[2] = mkattr(SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE);
    mattrs[2].value.s32 = SAI_VLAN_TAGGING_MODE_UNTAGGED;

    sai_object_id_t m0 = SAI_NULL_OBJECT_ID;
    st = vlan->create_vlan_member(&m0, sid, 3, mattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create vlan member 0 -> 0x%x", st);
    /* bridge port 0 maps to BCM port 1 (front panel ports start at 1) */
    CHECK(mock_bcm_vlan_member_mode(100, 1) == 1,
          "untagged membership programmed (mode=%d)",
          mock_bcm_vlan_member_mode(100, 1));

    /* Member on bridge port 1, tagged. */
    mattrs[1].value.oid = bridge_ports[1];
    mattrs[2].value.s32 = SAI_VLAN_TAGGING_MODE_TAGGED;
    sai_object_id_t m1 = SAI_NULL_OBJECT_ID;
    st = vlan->create_vlan_member(&m1, sid, 3, mattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create vlan member 1 -> 0x%x", st);
    CHECK(mock_bcm_vlan_member_mode(100, 2) == 2,
          "tagged membership programmed (mode=%d)",
          mock_bcm_vlan_member_mode(100, 2));

    /* duplicate member */
    sai_object_id_t mdup = SAI_NULL_OBJECT_ID;
    mattrs[1].value.oid = bridge_ports[0];
    mattrs[2].value.s32 = SAI_VLAN_TAGGING_MODE_UNTAGGED;
    st = vlan->create_vlan_member(&mdup, sid, 3, mattrs);
    CHECK(st == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "dup member -> 0x%x (expected ALREADY_EXISTS)", st);

    /* member get */
    sai_attribute_t gg = mkattr(SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE);
    CHECK(vlan->get_vlan_member_attribute(m1, 1, &gg) == SAI_STATUS_SUCCESS,
          "get member attr");
    CHECK(gg.value.s32 == SAI_VLAN_TAGGING_MODE_TAGGED,
          "member tag mode round-trip");

    /* member list */
    sai_object_id_t mlist[8];
    sai_attribute_t ml = mkattr(SAI_VLAN_ATTR_MEMBER_LIST);
    ml.value.objlist.count = 8;
    ml.value.objlist.list = mlist;
    CHECK(vlan->get_vlan_attribute(vlan100, 1, &ml) == SAI_STATUS_SUCCESS,
          "get member list");
    CHECK(ml.value.objlist.count == 2, "member list has 2 (%u)",
          ml.value.objlist.count);

    /* remove vlan with members -> OBJECT_IN_USE */
    st = vlan->remove_vlan(vlan100);
    CHECK(st == SAI_STATUS_OBJECT_IN_USE,
          "remove vlan w/ members -> 0x%x (expected OBJECT_IN_USE)", st);

    /* remove member 0 */
    st = vlan->remove_vlan_member(m0);
    CHECK(st == SAI_STATUS_SUCCESS, "remove member 0 -> 0x%x", st);
    CHECK(mock_bcm_vlan_member_mode(100, 1) == 0,
          "membership removed from hardware");

    /* ================= LAG ================= */
    sai_object_id_t lag_oid0 = SAI_NULL_OBJECT_ID;
    st = lag->create_lag(&lag_oid0, sid, 0, NULL);
    CHECK(st == SAI_STATUS_SUCCESS, "create lag -> 0x%x", st);

    sai_attribute_t lm[2];
    lm[0] = mkattr(SAI_LAG_MEMBER_ATTR_LAG_ID);
    lm[0].value.oid = lag_oid0;
    lm[1] = mkattr(SAI_LAG_MEMBER_ATTR_PORT_ID);
    lm[1].value.oid = ports[5];

    sai_object_id_t lm0 = SAI_NULL_OBJECT_ID;
    st = lag->create_lag_member(&lm0, sid, 2, lm);
    CHECK(st == SAI_STATUS_SUCCESS, "create lag member -> 0x%x", st);
    /* ports[5] = SAI index 5 = BCM port 6 */
    CHECK(mock_bcm_trunk_member_count(0) == 1, "trunk has 1 member");

    lm[1].value.oid = ports[6];
    sai_object_id_t lm1 = SAI_NULL_OBJECT_ID;
    st = lag->create_lag_member(&lm1, sid, 2, lm);
    CHECK(st == SAI_STATUS_SUCCESS, "create lag member 2 -> 0x%x", st);
    CHECK(mock_bcm_trunk_member_count(0) == 2, "trunk has 2 members");

    /* LAG PORT_LIST */
    sai_object_id_t plist[4];
    sai_attribute_t pla = mkattr(SAI_LAG_ATTR_PORT_LIST);
    pla.value.objlist.count = 4;
    pla.value.objlist.list = plist;
    CHECK(lag->get_lag_attribute(lag_oid0, 1, &pla) == SAI_STATUS_SUCCESS,
          "get lag port list");
    CHECK(pla.value.objlist.count == 2, "lag has 2 ports (%u)",
          pla.value.objlist.count);

    /* lag member get */
    sai_attribute_t lmg = mkattr(SAI_LAG_MEMBER_ATTR_PORT_ID);
    CHECK(lag->get_lag_member_attribute(lm1, 1, &lmg) == SAI_STATUS_SUCCESS,
          "get lag member");
    CHECK(lmg.value.oid == ports[6], "lag member port round-trip");

    /* remove lag with members -> OBJECT_IN_USE */
    st = lag->remove_lag(lag_oid0);
    CHECK(st == SAI_STATUS_OBJECT_IN_USE,
          "remove lag w/ members -> 0x%x (expected OBJECT_IN_USE)", st);

    st = lag->remove_lag_member(lm0);
    CHECK(st == SAI_STATUS_SUCCESS, "remove lag member -> 0x%x", st);
    st = lag->remove_lag_member(lm1);
    CHECK(st == SAI_STATUS_SUCCESS, "remove lag member 2 -> 0x%x", st);
    st = lag->remove_lag(lag_oid0);
    CHECK(st == SAI_STATUS_SUCCESS, "remove lag -> 0x%x", st);
    CHECK(mock_bcm_trunk_used(0) == 0, "trunk destroyed");

    /* ================= VLAN member on a LAG ================= */
    sai_object_id_t lag2 = SAI_NULL_OBJECT_ID;
    st = lag->create_lag(&lag2, sid, 0, NULL);
    CHECK(st == SAI_STATUS_SUCCESS, "create lag 2 -> 0x%x", st);

    /* bridge port on the default 1Q bridge referencing the LAG */
    sai_attribute_t bpattr[2];
    bpattr[0] = mkattr(SAI_BRIDGE_PORT_ATTR_BRIDGE_ID);
    sai_attribute_t q = mkattr(SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID);
    CHECK(sw->get_switch_attribute(sid, 1, &q) == SAI_STATUS_SUCCESS,
          "get default bridge again");
    bpattr[0].value.oid = q.value.oid;
    bpattr[1] = mkattr(SAI_BRIDGE_PORT_ATTR_PORT_ID);
    bpattr[1].value.oid = lag2;

    sai_object_id_t lag_bp = SAI_NULL_OBJECT_ID;
    st = bridge->create_bridge_port(&lag_bp, sid, 2, bpattr);
    CHECK(st == SAI_STATUS_SUCCESS, "create lag bridge port -> 0x%x", st);
    CHECK(lag_bp != SAI_NULL_OBJECT_ID, "lag bridge port non-null");

    /* vlan member on the LAG bridge port (tagged) */
    mattrs[0].value.oid = vlan100;
    mattrs[1].value.oid = lag_bp;
    mattrs[2].value.s32 = SAI_VLAN_TAGGING_MODE_TAGGED;
    sai_object_id_t mlag = SAI_NULL_OBJECT_ID;
    st = vlan->create_vlan_member(&mlag, sid, 3, mattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "vlan member on lag -> 0x%x", st);

    /* ================= FDB ================= */
    sai_fdb_entry_t ent;
    memset(&ent, 0, sizeof(ent));
    ent.switch_id = sid;
    ent.bv_id = vlan100;
    uint8_t mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(ent.mac_address, mac, 6);

    sai_attribute_t fattrs[2];
    fattrs[0] = mkattr(SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID);
    fattrs[0].value.oid = bridge_ports[2];
    fattrs[1] = mkattr(SAI_FDB_ENTRY_ATTR_TYPE);
    fattrs[1].value.s32 = SAI_FDB_ENTRY_TYPE_STATIC;

    st = fdb->create_fdb_entry(&ent, 2, fattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create fdb entry -> 0x%x", st);
    CHECK(mock_bcm_l2_count() == 1, "l2 entry exists");
    CHECK(mock_bcm_l2_is_static(0, mac, 100) == 1, "entry is static");

    /* duplicate */
    st = fdb->create_fdb_entry(&ent, 2, fattrs);
    CHECK(st == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "dup fdb -> 0x%x (expected ALREADY_EXISTS)", st);

    /* fdb get type */
    sai_attribute_t fget = mkattr(SAI_FDB_ENTRY_ATTR_TYPE);
    CHECK(fdb->get_fdb_entry_attribute(&ent, 1, &fget) == SAI_STATUS_SUCCESS,
          "get fdb attr");
    CHECK(fget.value.s32 == SAI_FDB_ENTRY_TYPE_STATIC, "fdb type round-trip");

    /* fdb get bridge port (hardware readback + reverse map) */
    fget = mkattr(SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID);
    CHECK(fdb->get_fdb_entry_attribute(&ent, 1, &fget) == SAI_STATUS_SUCCESS,
          "get fdb bridge port");
    CHECK(fget.value.oid == bridge_ports[2], "fdb bridge port round-trip");

    /* fdb entry on a LAG dest */
    uint8_t mac2[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66};
    memcpy(ent.mac_address, mac2, 6);
    fattrs[0].value.oid = lag_bp;
    st = fdb->create_fdb_entry(&ent, 2, fattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create fdb entry on lag -> 0x%x", st);
    CHECK(mock_bcm_l2_count() == 2, "two l2 entries");
    int tgid = -1;
    CHECK(mock_bcm_l2_dest_is_trunk(0, mac2, 100, &tgid) == 1,
          "lag fdb dest is trunk");
    CHECK(tgid == 0, "trunk id 0 (tgid=%d)", tgid);

    /* set attr: move entry to another bridge port */
    fattrs[0].value.oid = bridge_ports[3];
    sai_attribute_t setattr = mkattr(SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID);
    setattr.value.oid = bridge_ports[3];
    memcpy(ent.mac_address, mac, 6);
    st = fdb->set_fdb_entry_attribute(&ent, &setattr);
    CHECK(st == SAI_STATUS_SUCCESS, "set fdb bridge port -> 0x%x", st);
    fget = mkattr(SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID);
    CHECK(fdb->get_fdb_entry_attribute(&ent, 1, &fget) == SAI_STATUS_SUCCESS,
          "get after set");
    CHECK(fget.value.oid == bridge_ports[3], "fdb moved");

    /* remove fdb */
    st = fdb->remove_fdb_entry(&ent);
    CHECK(st == SAI_STATUS_SUCCESS, "remove fdb -> 0x%x", st);
    CHECK(mock_bcm_l2_count() == 1, "one entry left");
    st = fdb->remove_fdb_entry(&ent);
    CHECK(st == SAI_STATUS_ITEM_NOT_FOUND,
          "remove gone fdb -> 0x%x (expected NOT_FOUND)", st);

    /* ================= flush ================= */
    /* re-add an entry on mac2 (still present, lag dest) */
    memcpy(ent.mac_address, mac, 6);
    fattrs[0].value.oid = bridge_ports[4];
    st = fdb->create_fdb_entry(&ent, 2, fattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "re-add fdb -> 0x%x", st);
    CHECK(mock_bcm_l2_count() == 2, "two entries before flush");

    /* flush by vlan, ALL types */
    sai_attribute_t flush[2];
    flush[0] = mkattr(SAI_FDB_FLUSH_ATTR_BV_ID);
    flush[0].value.oid = vlan100;
    flush[1] = mkattr(SAI_FDB_FLUSH_ATTR_ENTRY_TYPE);
    flush[1].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_ALL;
    st = fdb->flush_fdb_entries(sid, 2, flush);
    CHECK(st == SAI_STATUS_SUCCESS, "flush by vlan -> 0x%x", st);
    CHECK(mock_bcm_l2_count() == 0, "all flushed");

    /* flush with no attrs (all vlans) */
    memcpy(ent.mac_address, mac, 6);
    st = fdb->create_fdb_entry(&ent, 2, fattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "add for global flush -> 0x%x", st);
    sai_attribute_t flush_all[1];
    flush_all[0] = mkattr(SAI_FDB_FLUSH_ATTR_ENTRY_TYPE);
    flush_all[0].value.s32 = SAI_FDB_FLUSH_ENTRY_TYPE_ALL;
    st = fdb->flush_fdb_entries(sid, 1, flush_all);
    CHECK(st == SAI_STATUS_SUCCESS, "flush all -> 0x%x", st);
    CHECK(mock_bcm_l2_count() == 0, "all flushed again");

    /* ================= 1D bridge ================= */
    sai_attribute_t battrs[1];
    battrs[0] = mkattr(SAI_BRIDGE_ATTR_TYPE);
    battrs[0].value.s32 = SAI_BRIDGE_TYPE_1D;
    sai_object_id_t b1d = SAI_NULL_OBJECT_ID;
    st = bridge->create_bridge(&b1d, sid, 1, battrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create 1D bridge -> 0x%x", st);
    CHECK(mock_bcm_vswitch_used(0) == 1, "vswitch created");

    /* 1Q create must be refused (only the default one exists) */
    sai_attribute_t bq = mkattr(SAI_BRIDGE_ATTR_TYPE);
    bq.value.s32 = SAI_BRIDGE_TYPE_1Q;
    sai_object_id_t b1q = SAI_NULL_OBJECT_ID;
    st = bridge->create_bridge(&b1q, sid, 1, &bq);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "create 1Q bridge -> 0x%x (expected NOT_SUPPORTED)", st);

    /* bridge port on 1D bridge -> vswitch port add */
    sai_attribute_t bpc[2];
    bpc[0] = mkattr(SAI_BRIDGE_PORT_ATTR_BRIDGE_ID);
    bpc[0].value.oid = b1d;
    bpc[1] = mkattr(SAI_BRIDGE_PORT_ATTR_PORT_ID);
    bpc[1].value.oid = ports[10];
    sai_object_id_t bp1d = SAI_NULL_OBJECT_ID;
    st = bridge->create_bridge_port(&bp1d, sid, 2, bpc);
    CHECK(st == SAI_STATUS_SUCCESS, "1D bridge port -> 0x%x", st);
    CHECK(mock_bcm_vswitch_member_count(0) == 1, "vswitch has 1 port");

    /* default bridge is not removable */
    q = mkattr(SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID);
    CHECK(sw->get_switch_attribute(sid, 1, &q) == SAI_STATUS_SUCCESS, "q");
    st = bridge->remove_bridge(q.value.oid);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "remove default bridge -> 0x%x (expected NOT_SUPPORTED)", st);

    /* 1D bridge with ports is not removable */
    st = bridge->remove_bridge(b1d);
    CHECK(st == SAI_STATUS_OBJECT_IN_USE,
          "remove 1D w/ ports -> 0x%x (expected OBJECT_IN_USE)", st);

    st = bridge->remove_bridge_port(bp1d);
    CHECK(st == SAI_STATUS_SUCCESS, "remove 1D bridge port -> 0x%x", st);
    CHECK(mock_bcm_vswitch_member_count(0) == 0, "vswitch port removed");
    st = bridge->remove_bridge(b1d);
    CHECK(st == SAI_STATUS_SUCCESS, "remove 1D bridge -> 0x%x", st);
    CHECK(mock_bcm_vswitch_used(0) == 0, "vswitch destroyed");

    /* ================= teardown ================= */
    st = vlan->remove_vlan_member(mlag);
    CHECK(st == SAI_STATUS_SUCCESS, "remove lag vlan member -> 0x%x", st);
    st = vlan->remove_vlan_member(m1);
    CHECK(st == SAI_STATUS_SUCCESS, "remove vlan member 1 -> 0x%x", st);
    st = bridge->remove_bridge_port(lag_bp);
    CHECK(st == SAI_STATUS_SUCCESS, "remove lag bridge port -> 0x%x", st);
    st = lag->remove_lag(lag2);
    CHECK(st == SAI_STATUS_SUCCESS, "remove lag2 -> 0x%x", st);
    st = vlan->remove_vlan(vlan100);
    CHECK(st == SAI_STATUS_SUCCESS, "remove vlan 100 -> 0x%x", st);
    CHECK(mock_bcm_vlan_used(100) == 0, "vlan destroyed");

    if (failures == 0) {
        printf("test_l2: OK\n");
        return 0;
    }
    printf("test_l2: %d FAILURES\n", failures);
    return 1;
}