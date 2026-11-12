/*
 * Unit tests for the SAI Hostif / ACL / QoS Map APIs over the mock backend.
 */
#include <stdio.h>
#include <string.h>

#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_hostif.h"
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
static const sai_port_api_t *port;
static const sai_hostif_api_t *hostif;
static const sai_acl_api_t *acl;
static const sai_qos_map_api_t *qosmap;

static sai_object_id_t sid;
static sai_object_id_t ports[MOCK_BCM_FRONT_PANEL_PORTS];

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
    CHECK(sai_api_query(SAI_API_PORT, (void **)&port) == SAI_STATUS_SUCCESS,
          "q port");
    CHECK(sai_api_query(SAI_API_HOSTIF, (void **)&hostif) ==
          SAI_STATUS_SUCCESS, "q hostif");
    CHECK(sai_api_query(SAI_API_ACL, (void **)&acl) == SAI_STATUS_SUCCESS,
          "q acl");
    CHECK(sai_api_query(SAI_API_QOS_MAP, (void **)&qosmap) ==
          SAI_STATUS_SUCCESS, "q qosmap");

    CHECK(sw->create_switch(&sid, 0, NULL) == SAI_STATUS_SUCCESS, "switch");
    sai_attribute_t pl = mkattr(SAI_SWITCH_ATTR_PORT_LIST);
    pl.value.objlist.count = MOCK_BCM_FRONT_PANEL_PORTS;
    pl.value.objlist.list = ports;
    CHECK(sw->get_switch_attribute(sid, 1, &pl) == SAI_STATUS_SUCCESS,
          "plist");
}

int main(void)
{
    setup();

    /* ================= trap groups ================= */
    sai_object_id_t tg = SAI_NULL_OBJECT_ID;
    sai_attribute_t q = mkattr(SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE);
    q.value.u32 = 3;
    sai_status_t st = hostif->create_hostif_trap_group(&tg, sid, 1, &q);
    CHECK(st == SAI_STATUS_SUCCESS, "create trap group -> 0x%x", st);

    sai_attribute_t gq = mkattr(SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE);
    CHECK(hostif->get_hostif_trap_group_attribute(tg, 1, &gq) ==
          SAI_STATUS_SUCCESS, "get trap group queue");
    CHECK(gq.value.u32 == 3, "queue round-trip");

    /* set queue */
    q.value.u32 = 5;
    CHECK(hostif->set_hostif_trap_group_attribute(tg, &q) ==
          SAI_STATUS_SUCCESS, "set trap group queue");
    gq = mkattr(SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE);
    CHECK(hostif->get_hostif_trap_group_attribute(tg, 1, &gq) ==
          SAI_STATUS_SUCCESS, "get queue after set");
    CHECK(gq.value.u32 == 5, "queue set round-trip");

    /* ================= traps ================= */
    sai_attribute_t tattrs[3];
    tattrs[0] = mkattr(SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE);
    tattrs[0].value.s32 = SAI_HOSTIF_TRAP_TYPE_LLDP;
    tattrs[1] = mkattr(SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP);
    tattrs[1].value.oid = tg;
    tattrs[2] = mkattr(SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION);
    tattrs[2].value.s32 = SAI_PACKET_ACTION_TRAP;

    sai_object_id_t trap_lldp = SAI_NULL_OBJECT_ID;
    st = hostif->create_hostif_trap(&trap_lldp, sid, 3, tattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create lldp trap -> 0x%x", st);
    CHECK(mock_bcm_trap_count() == 1, "one hw trap");

    /* duplicate (type, group) */
    sai_object_id_t dup = SAI_NULL_OBJECT_ID;
    st = hostif->create_hostif_trap(&dup, sid, 3, tattrs);
    CHECK(st == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "dup trap -> 0x%x (expected ALREADY_EXISTS)", st);

    /* trap attrs round-trip */
    sai_attribute_t tg2 = mkattr(SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP);
    CHECK(hostif->get_hostif_trap_attribute(trap_lldp, 1, &tg2) ==
          SAI_STATUS_SUCCESS, "get trap group");
    CHECK(tg2.value.oid == tg, "trap group round-trip");

    /* ARP trap with DROP action */
    tattrs[0].value.s32 = SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST;
    tattrs[2].value.s32 = SAI_PACKET_ACTION_DROP;
    sai_object_id_t trap_arp = SAI_NULL_OBJECT_ID;
    st = hostif->create_hostif_trap(&trap_arp, sid, 3, tattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create arp trap -> 0x%x", st);
    CHECK(mock_bcm_trap_count() == 2, "two hw traps");

    /* trap without TRAP_TYPE must fail */
    sai_object_id_t trap_bad = SAI_NULL_OBJECT_ID;
    st = hostif->create_hostif_trap(&trap_bad, sid, 0, NULL);
    CHECK(st == SAI_STATUS_INVALID_PARAMETER,
          "trap no type -> 0x%x (expected INVALID_PARAMETER)", st);

    /* remove group while traps reference it -> OBJECT_IN_USE */
    st = hostif->remove_hostif_trap_group(tg);
    CHECK(st == SAI_STATUS_OBJECT_IN_USE,
          "remove group w/ traps -> 0x%x (expected OBJECT_IN_USE)", st);

    /* ================= hostif (netdev) ================= */
    sai_attribute_t hattrs[3];
    hattrs[0] = mkattr(SAI_HOSTIF_ATTR_TYPE);
    hattrs[0].value.s32 = SAI_HOSTIF_TYPE_NETDEV;
    hattrs[1] = mkattr(SAI_HOSTIF_ATTR_OBJ_ID);
    hattrs[1].value.oid = ports[0];
    hattrs[2] = mkattr(SAI_HOSTIF_ATTR_NAME);
    snprintf(hattrs[2].value.chardata, sizeof(hattrs[2].value.chardata),
             "eth0");

    sai_object_id_t hif = SAI_NULL_OBJECT_ID;
    st = hostif->create_hostif(&hif, sid, 3, hattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create hostif -> 0x%x", st);
    CHECK(mock_bcm_knetif_count() == 1, "one knet netif");

    sai_attribute_t hname = mkattr(SAI_HOSTIF_ATTR_NAME);
    CHECK(hostif->get_hostif_attribute(hif, 1, &hname) == SAI_STATUS_SUCCESS,
          "get hostif name");
    CHECK(strcmp(hname.value.chardata, "eth0") == 0, "hostif name round-trip");

    /* FD hostifs are not supported in P4 */
    hattrs[0].value.s32 = SAI_HOSTIF_TYPE_FD;
    sai_object_id_t hif_fd = SAI_NULL_OBJECT_ID;
    st = hostif->create_hostif(&hif_fd, sid, 3, hattrs);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "fd hostif -> 0x%x (expected NOT_SUPPORTED)", st);

    /* ================= ACL table ================= */
    sai_attribute_t aattrs[5];
    aattrs[0] = mkattr(SAI_ACL_TABLE_ATTR_ACL_STAGE);
    aattrs[0].value.s32 = SAI_ACL_STAGE_INGRESS;
    aattrs[1] = mkattr(SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE);
    aattrs[1].value.booldata = true;
    aattrs[2] = mkattr(SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC);
    aattrs[2].value.booldata = true;
    aattrs[3] = mkattr(SAI_ACL_TABLE_ATTR_FIELD_IN_PORT);
    aattrs[3].value.booldata = true;

    sai_object_id_t table = SAI_NULL_OBJECT_ID;
    st = acl->create_acl_table(&table, sid, 4, aattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create acl table -> 0x%x", st);
    CHECK(mock_bcm_field_group_count() == 1, "one field group");

    /* egress stage not supported */
    aattrs[0].value.s32 = SAI_ACL_STAGE_EGRESS;
    sai_object_id_t tbl_eg = SAI_NULL_OBJECT_ID;
    st = acl->create_acl_table(&tbl_eg, sid, 4, aattrs);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "egress table -> 0x%x (expected NOT_SUPPORTED)", st);
    aattrs[0].value.s32 = SAI_ACL_STAGE_INGRESS;

    /* table get */
    sai_attribute_t stage = mkattr(SAI_ACL_TABLE_ATTR_ACL_STAGE);
    CHECK(acl->get_acl_table_attribute(table, 1, &stage) == SAI_STATUS_SUCCESS,
          "get table stage");
    CHECK(stage.value.s32 == SAI_ACL_STAGE_INGRESS, "stage round-trip");

    /* ================= ACL entries ================= */
    sai_attribute_t eattrs[6];
    eattrs[0] = mkattr(SAI_ACL_ENTRY_ATTR_TABLE_ID);
    eattrs[0].value.oid = table;
    eattrs[1] = mkattr(SAI_ACL_ENTRY_ATTR_PRIORITY);
    eattrs[1].value.u32 = 10;
    eattrs[2] = mkattr(SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE);
    eattrs[2].value.aclfield.enable = true;
    eattrs[2].value.aclfield.data.u16 = 0x0806; /* ARP */
    eattrs[2].value.aclfield.mask.u16 = 0xFFFF;
    eattrs[3] = mkattr(SAI_ACL_ENTRY_ATTR_FIELD_IN_PORT);
    eattrs[3].value.aclfield.enable = true;
    eattrs[3].value.aclfield.data.oid = ports[2];
    eattrs[4] = mkattr(SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION);
    eattrs[4].value.aclaction.enable = true;
    eattrs[4].value.aclaction.parameter.s32 = SAI_PACKET_ACTION_DROP;

    sai_object_id_t entry = SAI_NULL_OBJECT_ID;
    st = acl->create_acl_entry(&entry, sid, 5, eattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create acl entry -> 0x%x", st);
    CHECK(mock_bcm_field_entry_installed(0) == 1, "entry installed");
    CHECK(mock_bcm_field_entry_drops(0) == 1, "entry drops");

    /* match the table does not support -> INVALID_PARAMETER */
    eattrs[2].id = SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC;
    eattrs[2].value.aclfield.enable = true;
    memset(eattrs[2].value.aclfield.data.mac, 0xAA, 6);
    memset(eattrs[2].value.aclfield.mask.mac, 0xFF, 6);
    sai_object_id_t ebad = SAI_NULL_OBJECT_ID;
    st = acl->create_acl_entry(&ebad, sid, 5, eattrs);
    CHECK(st == SAI_STATUS_INVALID_PARAMETER,
          "unsupported match -> 0x%x (expected INVALID_PARAMETER)", st);
    eattrs[2].id = SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE;

    /* FORWARD action entry */
    eattrs[4].value.aclaction.parameter.s32 = SAI_PACKET_ACTION_FORWARD;
    sai_object_id_t entry2 = SAI_NULL_OBJECT_ID;
    st = acl->create_acl_entry(&entry2, sid, 5, eattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create fwd entry -> 0x%x", st);
    CHECK(mock_bcm_field_entry_drops(1) == 0, "fwd entry does not drop");

    /* unknown action */
    eattrs[4].value.aclaction.parameter.s32 = SAI_PACKET_ACTION_DENY;
    sai_object_id_t eact = SAI_NULL_OBJECT_ID;
    st = acl->create_acl_entry(&eact, sid, 5, eattrs);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "redirect action -> 0x%x (expected NOT_SUPPORTED)", st);

    /* entry get */
    sai_attribute_t ep = mkattr(SAI_ACL_ENTRY_ATTR_PRIORITY);
    CHECK(acl->get_acl_entry_attribute(entry, 1, &ep) == SAI_STATUS_SUCCESS,
          "get entry prio");
    CHECK(ep.value.u32 == 10, "entry prio round-trip");

    /* remove table with entries -> OBJECT_IN_USE */
    st = acl->remove_acl_table(table);
    CHECK(st == SAI_STATUS_OBJECT_IN_USE,
          "remove table w/ entries -> 0x%x (expected OBJECT_IN_USE)", st);

    /* ================= QoS map ================= */
    sai_qos_map_t maps[2];
    memset(maps, 0, sizeof(maps));
    maps[0].key.dscp = 46;
    maps[0].value.tc = 5;
    maps[1].key.dscp = 8;
    maps[1].value.tc = 1;

    sai_attribute_t qattrs[2];
    qattrs[0] = mkattr(SAI_QOS_MAP_ATTR_TYPE);
    qattrs[0].value.s32 = SAI_QOS_MAP_TYPE_DSCP_TO_TC;
    qattrs[1] = mkattr(SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST);
    qattrs[1].value.qosmap.count = 2;
    qattrs[1].value.qosmap.list = maps;

    sai_object_id_t qm = SAI_NULL_OBJECT_ID;
    st = qosmap->create_qos_map(&qm, sid, 2, qattrs);
    CHECK(st == SAI_STATUS_SUCCESS, "create qos map -> 0x%x", st);
    CHECK(mock_bcm_qos_map_count() == 2, "two map entries in hw");

    /* get list */
    sai_qos_map_t back[2];
    sai_attribute_t qg = mkattr(SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST);
    qg.value.qosmap.count = 2;
    qg.value.qosmap.list = back;
    CHECK(qosmap->get_qos_map_attribute(qm, 1, &qg) == SAI_STATUS_SUCCESS,
          "get qos map");
    CHECK(qg.value.qosmap.count == 2, "map count round-trip");
    CHECK(back[0].key.dscp == 46 && back[0].value.tc == 5, "map[0] round-trip");

    /* unsupported map type */
    qattrs[0].value.s32 = SAI_QOS_MAP_TYPE_DOT1P_TO_TC;
    sai_object_id_t qm2 = SAI_NULL_OBJECT_ID;
    st = qosmap->create_qos_map(&qm2, sid, 2, qattrs);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "dot1p map -> 0x%x (expected NOT_SUPPORTED)", st);

    st = qosmap->remove_qos_map(qm);
    CHECK(st == SAI_STATUS_SUCCESS, "remove qos map -> 0x%x", st);
    CHECK(mock_bcm_qos_map_count() == 0, "map entries removed from hw");

    /* ================= teardown ================= */
    st = acl->remove_acl_entry(entry);
    CHECK(st == SAI_STATUS_SUCCESS, "remove acl entry -> 0x%x", st);
    st = acl->remove_acl_entry(entry2);
    CHECK(st == SAI_STATUS_SUCCESS, "remove acl entry 2 -> 0x%x", st);
    st = acl->remove_acl_table(table);
    CHECK(st == SAI_STATUS_SUCCESS, "remove acl table -> 0x%x", st);
    CHECK(mock_bcm_field_group_count() == 0, "field groups gone");

    st = hostif->remove_hostif(hif);
    CHECK(st == SAI_STATUS_SUCCESS, "remove hostif -> 0x%x", st);
    CHECK(mock_bcm_knetif_count() == 0, "knet netifs gone");

    st = hostif->remove_hostif_trap(trap_lldp);
    CHECK(st == SAI_STATUS_SUCCESS, "remove lldp trap -> 0x%x", st);
    st = hostif->remove_hostif_trap(trap_arp);
    CHECK(st == SAI_STATUS_SUCCESS, "remove arp trap -> 0x%x", st);
    CHECK(mock_bcm_trap_count() == 0, "hw traps gone");

    st = hostif->remove_hostif_trap_group(tg);
    CHECK(st == SAI_STATUS_SUCCESS, "remove trap group -> 0x%x", st);

    if (failures == 0) {
        printf("test_p4: OK\n");
        return 0;
    }
    printf("test_p4: %d FAILURES\n", failures);
    return 1;
}