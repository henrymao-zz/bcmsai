/*
 * Unit tests for the SAI L3 APIs (virtual router, router interface,
 * next hop, next hop group, route, neighbor) over the mock BCM backend.
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
static const sai_virtual_router_api_t *vr;
static const sai_router_interface_api_t *rif;
static const sai_next_hop_api_t *nh;
static const sai_next_hop_group_api_t *nhg;
static const sai_route_api_t *route;
static const sai_neighbor_api_t *nbr;

static sai_object_id_t sid;
static sai_object_id_t ports[MOCK_BCM_FRONT_PANEL_PORTS];
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
    CHECK(sai_api_query(SAI_API_VIRTUAL_ROUTER, (void **)&vr) ==
          SAI_STATUS_SUCCESS, "q vr");
    CHECK(sai_api_query(SAI_API_ROUTER_INTERFACE, (void **)&rif) ==
          SAI_STATUS_SUCCESS, "q rif");
    CHECK(sai_api_query(SAI_API_NEXT_HOP, (void **)&nh) == SAI_STATUS_SUCCESS,
          "q nh");
    CHECK(sai_api_query(SAI_API_NEXT_HOP_GROUP, (void **)&nhg) ==
          SAI_STATUS_SUCCESS, "q nhg");
    CHECK(sai_api_query(SAI_API_ROUTE, (void **)&route) == SAI_STATUS_SUCCESS,
          "q route");
    CHECK(sai_api_query(SAI_API_NEIGHBOR, (void **)&nbr) == SAI_STATUS_SUCCESS,
          "q nbr");

    CHECK(sw->create_switch(&sid, 0, NULL) == SAI_STATUS_SUCCESS, "switch");

    sai_attribute_t pl = mkattr(SAI_SWITCH_ATTR_PORT_LIST);
    pl.value.objlist.count = MOCK_BCM_FRONT_PANEL_PORTS;
    pl.value.objlist.list = ports;
    CHECK(sw->get_switch_attribute(sid, 1, &pl) == SAI_STATUS_SUCCESS, "plist");

    sai_attribute_t dv = mkattr(SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID);
    CHECK(sw->get_switch_attribute(sid, 1, &dv) == SAI_STATUS_SUCCESS,
          "default vr");
    CHECK(dv.value.oid != SAI_NULL_OBJECT_ID, "default vr non-null");
    default_vr = dv.value.oid;
}

int main(void)
{
    setup();

    /* ================= virtual router ================= */
    sai_object_id_t vr2 = SAI_NULL_OBJECT_ID;
    sai_status_t st = vr->create_virtual_router(&vr2, sid, 0, NULL);
    CHECK(st == SAI_STATUS_SUCCESS, "create vr -> 0x%x", st);

    sai_attribute_t a = mkattr(SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE);
    a.value.booldata = false;
    CHECK(vr->set_virtual_router_attribute(vr2, &a) == SAI_STATUS_SUCCESS,
          "set vr admin");
    a.value.booldata = true;
    CHECK(vr->get_virtual_router_attribute(vr2, 1, &a) == SAI_STATUS_SUCCESS,
          "get vr admin");
    CHECK(!a.value.booldata, "vr admin v4 off round-trip");

    /* default VR is not removable */
    st = vr->remove_virtual_router(default_vr);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "remove default vr -> 0x%x (expected NOT_SUPPORTED)", st);

    /* ================= RIF over port ================= */
    sai_attribute_t rattrs[3];
    rattrs[0] = mkattr(SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID);
    rattrs[0].value.oid = default_vr;
    rattrs[1] = mkattr(SAI_ROUTER_INTERFACE_ATTR_TYPE);
    rattrs[1].value.s32 = SAI_ROUTER_INTERFACE_TYPE_PORT;
    rattrs[2] = mkattr(SAI_ROUTER_INTERFACE_ATTR_PORT_ID);
    rattrs[2].value.oid = ports[0];

    sai_object_id_t rif_port = SAI_NULL_OBJECT_ID;
    st = rif->create_router_interface(&rif_port, sid, 3, rattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create port rif -> 0x%x", st);
    CHECK(mock_bcm_l3_ingress_exists(0), "ingress if created");

    /* RIF on a bogus VR must fail */
    rattrs[0].value.oid = sai_bcm_oid_encode(1, SAI_OBJECT_TYPE_VIRTUAL_ROUTER,
                                            77);
    sai_object_id_t rif_bad = SAI_NULL_OBJECT_ID;
    st = rif->create_router_interface(&rif_bad, sid, 3, rattrs);
    CHECK(st == SAI_STATUS_ITEM_NOT_FOUND,
          "rif bogus vr -> 0x%x (expected NOT_FOUND)", st);
    rattrs[0].value.oid = default_vr;

    /* RIF with no destination must fail */
    sai_object_id_t rif_nodest = SAI_NULL_OBJECT_ID;
    st = rif->create_router_interface(&rif_nodest, sid, 2, rattrs);
    CHECK(st == SAI_STATUS_INVALID_PARAMETER,
          "rif no dest -> 0x%x (expected INVALID_PARAMETER)", st);

    /* ================= VLAN + RIF over vlan ================= */
    sai_attribute_t va = mkattr(SAI_VLAN_ATTR_VLAN_ID);
    va.value.u16 = 200;
    sai_object_id_t vlan200 = SAI_NULL_OBJECT_ID;
    CHECK(vlan->create_vlan(&vlan200, sid, 1, &va) == SAI_STATUS_SUCCESS,
          "create vlan 200");

    sai_attribute_t vrattrs[3];
    vrattrs[0] = mkattr(SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID);
    vrattrs[0].value.oid = default_vr;
    vrattrs[1] = mkattr(SAI_ROUTER_INTERFACE_ATTR_TYPE);
    vrattrs[1].value.s32 = SAI_ROUTER_INTERFACE_TYPE_VLAN;
    vrattrs[2] = mkattr(SAI_ROUTER_INTERFACE_ATTR_VLAN_ID);
    vrattrs[2].value.oid = vlan200;

    sai_object_id_t rif_vlan = SAI_NULL_OBJECT_ID;
    st = rif->create_router_interface(&rif_vlan, sid, 3, vrattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create vlan rif -> 0x%x", st);

    /* rif get: type + vlan round-trip */
    sai_attribute_t g1 = mkattr(SAI_ROUTER_INTERFACE_ATTR_TYPE);
    CHECK(rif->get_router_interface_attribute(rif_vlan, 1, &g1) ==
          SAI_STATUS_SUCCESS, "get rif type");
    CHECK(g1.value.s32 == SAI_ROUTER_INTERFACE_TYPE_VLAN, "rif type vlan");
    g1 = mkattr(SAI_ROUTER_INTERFACE_ATTR_VLAN_ID);
    CHECK(rif->get_router_interface_attribute(rif_vlan, 1, &g1) ==
          SAI_STATUS_SUCCESS, "get rif vlan");
    CHECK(g1.value.oid == vlan200, "rif vlan round-trip");

    /* VR with RIFs must not be removable */
    st = vr->remove_virtual_router(default_vr);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED, "default vr still protected");
    sai_object_id_t vr3 = SAI_NULL_OBJECT_ID;
    CHECK(vr->create_virtual_router(&vr3, sid, 0, NULL) == SAI_STATUS_SUCCESS,
          "create vr3");
    vrattrs[0].value.oid = vr3;
    sai_object_id_t rif_v3 = SAI_NULL_OBJECT_ID;
    CHECK(rif->create_router_interface(&rif_v3, sid, 3, vrattrs) ==
          SAI_STATUS_SUCCESS, "rif on vr3");
    st = vr->remove_virtual_router(vr3);
    CHECK(st == SAI_STATUS_OBJECT_IN_USE,
          "remove vr w/ rif -> 0x%x (expected OBJECT_IN_USE)", st);

    /* ================= neighbor ================= */
    sai_neighbor_entry_t ne;
    memset(&ne, 0, sizeof(ne));
    ne.switch_id = sid;
    ne.rif_id = rif_vlan;
    ne.ip_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    /* 10.0.0.1 in network order */
    ne.ip_address.addr.ip4 = 0x0A000001;

    uint8_t nbmac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x01};
    sai_attribute_t nattrs[1];
    nattrs[0] = mkattr(SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS);
    memcpy(nattrs[0].value.mac, nbmac, 6);

    st = nbr->create_neighbor_entry(&ne, 1, nattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create neighbor -> 0x%x", st);
    CHECK(mock_bcm_l3_host_count(0) == 1, "host entry in default vrf");

    /* duplicate */
    st = nbr->create_neighbor_entry(&ne, 1, nattrs);
    CHECK(st == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "dup neighbor -> 0x%x (expected ALREADY_EXISTS)", st);

    /* neighbor get mac */
    sai_attribute_t ng = mkattr(SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS);
    CHECK(nbr->get_neighbor_entry_attribute(&ne, 1, &ng) ==
          SAI_STATUS_SUCCESS, "get neighbor");
    CHECK(memcmp(ng.value.mac, nbmac, 6) == 0, "neighbor mac round-trip");

    /* ================= next hop ================= */
    sai_attribute_t nha[3];
    nha[0] = mkattr(SAI_NEXT_HOP_ATTR_TYPE);
    nha[0].value.s32 = SAI_NEXT_HOP_TYPE_IP;
    nha[1] = mkattr(SAI_NEXT_HOP_ATTR_IP);
    nha[1].value.ipaddr = ne.ip_address;
    nha[2] = mkattr(SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID);
    nha[2].value.oid = rif_vlan;

    sai_object_id_t nh1 = SAI_NULL_OBJECT_ID;
    st = nh->create_next_hop(&nh1, sid, 3, nha);
    CHECK(st == SAI_STATUS_SUCCESS, "create nh -> 0x%x", st);
    CHECK(mock_bcm_l3_egress_count() == 1, "egress object created");
    /* neighbor existed before NH: egress MAC must be the neighbor mac;
     * the mock does not store mac, so just verify existence. */
    CHECK(mock_bcm_l3_egress_exists(0), "egress id 0 exists");
    CHECK(mock_bcm_l3_egress_vlan(0) == 200, "egress vlan 200");

    /* NH on a LAG-based RIF would go via port rif; here test bogus rif */
    nha[2].value.oid = sid; /* switch oid is not a RIF */
    sai_object_id_t nh_bad = SAI_NULL_OBJECT_ID;
    st = nh->create_next_hop(&nh_bad, sid, 3, nha);
    CHECK(st == SAI_STATUS_ITEM_NOT_FOUND,
          "nh bogus rif -> 0x%x (expected NOT_FOUND)", st);
    nha[2].value.oid = rif_vlan;

    /* second NH on the port rif (10.0.0.2) */
    sai_object_id_t nh2 = SAI_NULL_OBJECT_ID;
    sai_ip_address_t ip2 = {.addr_family = SAI_IP_ADDR_FAMILY_IPV4,
                            .addr = {.ip4 = 0x0A000002}};
    nha[1].value.ipaddr = ip2;
    nha[2].value.oid = rif_port;
    st = nh->create_next_hop(&nh2, sid, 3, nha);
    CHECK(st == SAI_STATUS_SUCCESS, "create nh2 -> 0x%x", st);
    CHECK(mock_bcm_l3_egress_count() == 2, "two egress objects");
    CHECK(mock_bcm_l3_egress_vlan(1) == 0, "port-rif egress has vlan 0");

    /* ================= next hop group (ECMP) ================= */
    sai_object_id_t grp = SAI_NULL_OBJECT_ID;
    st = nhg->create_next_hop_group(&grp, sid, 0, NULL);
    CHECK(st == SAI_STATUS_SUCCESS, "create nhg -> 0x%x", st);

    sai_attribute_t gm[2];
    gm[0] = mkattr(SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID);
    gm[0].value.oid = grp;
    gm[1] = mkattr(SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID);
    gm[1].value.oid = nh1;

    sai_object_id_t m1 = SAI_NULL_OBJECT_ID;
    st = nhg->create_next_hop_group_member(&m1, sid, 2, gm);
    CHECK(st == SAI_STATUS_SUCCESS, "nhg member 1 -> 0x%x", st);
    CHECK(mock_bcm_l3_ecmp_exists(0), "ecmp object created");
    CHECK(mock_bcm_l3_ecmp_member_count(0) == 1, "ecmp 1 member");

    gm[1].value.oid = nh2;
    sai_object_id_t m2 = SAI_NULL_OBJECT_ID;
    st = nhg->create_next_hop_group_member(&m2, sid, 2, gm);
    CHECK(st == SAI_STATUS_SUCCESS, "nhg member 2 -> 0x%x", st);
    CHECK(mock_bcm_l3_ecmp_member_count(0) == 2, "ecmp 2 members");

    /* duplicate member */
    st = nhg->create_next_hop_group_member(&m2, sid, 2, gm);
    CHECK(st == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "dup nhg member -> 0x%x (expected ALREADY_EXISTS)", st);

    /* group attrs */
    sai_attribute_t gc = mkattr(SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_COUNT);
    CHECK(nhg->get_next_hop_group_attribute(grp, 1, &gc) == SAI_STATUS_SUCCESS,
          "get nhg count");
    CHECK(gc.value.u32 == 2, "nhg count 2");

    /* remove group with members -> OBJECT_IN_USE */
    st = nhg->remove_next_hop_group(grp);
    CHECK(st == SAI_STATUS_OBJECT_IN_USE,
          "remove nhg w/ members -> 0x%x (expected OBJECT_IN_USE)", st);

    /* ================= routes ================= */
    sai_route_entry_t re;
    memset(&re, 0, sizeof(re));
    re.switch_id = sid;
    re.vr_id = default_vr;
    re.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    /* 192.168.0.0/16 in network order */
    re.destination.addr.ip4 = 0xC0A80000;
    re.destination.mask.ip4 = 0xFFFF0000;

    sai_attribute_t ra[1];
    ra[0] = mkattr(SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
    ra[0].value.oid = nh1;

    st = route->create_route_entry(&re, 1, ra);
    CHECK(st == SAI_STATUS_SUCCESS, "create route -> 0x%x", st);
    CHECK(mock_bcm_l3_route_count(0) == 1, "route in default vrf");

    /* duplicate */
    st = route->create_route_entry(&re, 1, ra);
    CHECK(st == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "dup route -> 0x%x (expected ALREADY_EXISTS)", st);

    /* route get nh */
    sai_attribute_t rg = mkattr(SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
    CHECK(route->get_route_entry_attribute(&re, 1, &rg) == SAI_STATUS_SUCCESS,
          "get route");
    CHECK(rg.value.oid == nh1, "route nh round-trip");

    /* repoint route at the ECMP group */
    ra[0].value.oid = grp;
    st = route->set_route_entry_attribute(&re, &ra[0]);
    CHECK(st == SAI_STATUS_SUCCESS, "set route nh -> 0x%x", st);
    rg = mkattr(SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
    CHECK(route->get_route_entry_attribute(&re, 1, &rg) == SAI_STATUS_SUCCESS,
          "get route after set");
    CHECK(rg.value.oid == grp, "route now points at group");

    /* IPv6 route */
    sai_route_entry_t re6;
    memset(&re6, 0, sizeof(re6));
    re6.switch_id = sid;
    re6.vr_id = default_vr;
    re6.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV6;
    memset(re6.destination.addr.ip6, 0x20, 1);  /* 2000:: */
    memset(re6.destination.mask.ip6, 0xFF, 1);   /* /8 */

    ra[0].value.oid = nh2;
    st = route->create_route_entry(&re6, 1, ra);
    CHECK(st == SAI_STATUS_SUCCESS, "create v6 route -> 0x%x", st);
    CHECK(mock_bcm_l3_route_count(0) == 2, "two routes");

    /* remove v6 route */
    st = route->remove_route_entry(&re6);
    CHECK(st == SAI_STATUS_SUCCESS, "remove v6 route -> 0x%x", st);
    CHECK(mock_bcm_l3_route_count(0) == 1, "one route left");

    /* route in another VR goes to that vrf */
    sai_route_entry_t re3;
    memset(&re3, 0, sizeof(re3));
    re3.switch_id = sid;
    re3.vr_id = vr3;
    re3.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    re3.destination.addr.ip4 = 0x0B000000; /* 11.0.0.0/8 */
    re3.destination.mask.ip4 = 0xFF000000;
    ra[0].value.oid = nh2;
    st = route->create_route_entry(&re3, 1, ra);
    CHECK(st == SAI_STATUS_SUCCESS, "route in vr3 -> 0x%x", st);
    /* vr3 sits at registry index 2 -> vrf 2 (vrf == index in this pool) */
    CHECK(mock_bcm_l3_route_count(2) == 1, "route in vr3's vrf");

    /* ================= neighbor set (mac change) ================= */
    uint8_t nbmac2[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x99};
    nattrs[0].value.mac[5] = 0x99;
    st = nbr->set_neighbor_entry_attribute(&ne, nattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "set neighbor mac -> 0x%x", st);
    ng = mkattr(SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS);
    CHECK(nbr->get_neighbor_entry_attribute(&ne, 1, &ng) == SAI_STATUS_SUCCESS,
          "get neighbor after set");
    CHECK(memcmp(ng.value.mac, nbmac2, 6) == 0, "neighbor mac updated");
    (void)nbmac2;

    /* ================= teardown ================= */
    st = route->remove_route_entry(&re);
    CHECK(st == SAI_STATUS_SUCCESS, "remove route -> 0x%x", st);
    st = route->remove_route_entry(&re3);
    CHECK(st == SAI_STATUS_SUCCESS, "remove route vr3 -> 0x%x", st);
    st = nhg->remove_next_hop_group_member(m1);
    CHECK(st == SAI_STATUS_SUCCESS, "remove nhg member 1 -> 0x%x", st);
    CHECK(mock_bcm_l3_ecmp_member_count(0) == 1, "ecmp 1 member after remove");
    st = nhg->remove_next_hop_group_member(m2);
    CHECK(st == SAI_STATUS_SUCCESS, "remove nhg member 2 -> 0x%x", st);
    st = nhg->remove_next_hop_group(grp);
    CHECK(st == SAI_STATUS_SUCCESS, "remove nhg -> 0x%x", st);

    st = nh->remove_next_hop(nh2);
    CHECK(st == SAI_STATUS_SUCCESS, "remove nh2 -> 0x%x", st);
    st = nh->remove_next_hop(nh1);
    CHECK(st == SAI_STATUS_SUCCESS, "remove nh1 -> 0x%x", st);

    st = nbr->remove_neighbor_entry(&ne);
    CHECK(st == SAI_STATUS_SUCCESS, "remove neighbor -> 0x%x", st);
    CHECK(mock_bcm_l3_host_count(0) == 0, "host table empty");

    st = rif->remove_router_interface(rif_vlan);
    CHECK(st == SAI_STATUS_SUCCESS, "remove vlan rif -> 0x%x", st);
    st = rif->remove_router_interface(rif_port);
    CHECK(st == SAI_STATUS_SUCCESS, "remove port rif -> 0x%x", st);
    st = rif->remove_router_interface(rif_v3);
    CHECK(st == SAI_STATUS_SUCCESS, "remove rif v3 -> 0x%x", st);

    /* vr3 now has no RIFs; removable */
    st = vr->remove_virtual_router(vr3);
    CHECK(st == SAI_STATUS_SUCCESS, "remove vr3 -> 0x%x", st);
    st = vr->remove_virtual_router(vr2);
    CHECK(st == SAI_STATUS_SUCCESS, "remove vr2 -> 0x%x", st);

    if (failures == 0) {
        printf("test_l3: OK\n");
        return 0;
    }
    printf("test_l3: %d FAILURES\n", failures);
    return 1;
}