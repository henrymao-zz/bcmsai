/*
 * Unit tests for the SAI Port API over the mock BCM backend.
 * Links the adapter statically + mock backend.
 */
#include <stdio.h>
#include <string.h>

#include "sai_bcm_internal.h"
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
static sai_object_id_t sid = SAI_NULL_OBJECT_ID;
static sai_object_id_t ports[MOCK_BCM_FRONT_PANEL_PORTS];

static void setup(void)
{
    mock_bcm_reset();
    memset(&g_sai_bcm_sw, 0, sizeof(g_sai_bcm_sw));
    g_sai_bcm_sw.unit = SAI_BCM_UNIT_DEFAULT;

    sai_status_t st = sai_api_initialize(0, NULL);
    CHECK(st == SAI_STATUS_SUCCESS, "initialize");

    st = sai_api_query(SAI_API_SWITCH, (void **)&sw);
    CHECK(st == SAI_STATUS_SUCCESS, "query switch api");
    st = sai_api_query(SAI_API_PORT, (void **)&port);
    CHECK(st == SAI_STATUS_SUCCESS, "query port api");
    CHECK(port != NULL, "port table non-null");

    st = sw->create_switch(&sid, 0, NULL);
    CHECK(st == SAI_STATUS_SUCCESS, "create switch");

    sai_attribute_t lattr = {0};
    lattr.id = SAI_SWITCH_ATTR_PORT_LIST;
    lattr.value.objlist.count = MOCK_BCM_FRONT_PANEL_PORTS;
    lattr.value.objlist.list = ports;
    st = sw->get_switch_attribute(sid, 1, &lattr);
    CHECK(st == SAI_STATUS_SUCCESS, "get port list");
    CHECK(lattr.value.objlist.count == MOCK_BCM_FRONT_PANEL_PORTS,
          "port list count");
}

static sai_status_t get_attr(sai_object_id_t oid, sai_attr_id_t id,
                             sai_attribute_t *attr)
{
    memset(attr, 0, sizeof(*attr));
    attr->id = id;
    return port->get_port_attribute(oid, 1, attr);
}

int main(void)
{
    setup();

    /* ---- admin state / oper status ---- */
    sai_attribute_t attr;
    sai_status_t st = get_attr(ports[0], SAI_PORT_ATTR_ADMIN_STATE, &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "get ADMIN_STATE -> 0x%x", st);
    CHECK(attr.value.booldata, "port enabled by default");

    st = get_attr(ports[0], SAI_PORT_ATTR_OPER_STATUS, &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "get OPER_STATUS -> 0x%x", st);
    CHECK(attr.value.s32 == SAI_PORT_OPER_STATUS_UP, "oper up when enabled");

    memset(&attr, 0, sizeof(attr));
    attr.id = SAI_PORT_ATTR_ADMIN_STATE;
    attr.value.booldata = false;
    st = port->set_port_attribute(ports[0], &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "set ADMIN_STATE -> 0x%x", st);

    st = get_attr(ports[0], SAI_PORT_ATTR_OPER_STATUS, &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "get OPER_STATUS after disable");
    CHECK(attr.value.s32 == SAI_PORT_OPER_STATUS_DOWN,
          "oper down when disabled");

    /* ---- speed ---- */
    st = get_attr(ports[1], SAI_PORT_ATTR_SPEED, &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "get SPEED");
    CHECK(attr.value.u32 == 10000, "default speed 10G, got %u", attr.value.u32);

    memset(&attr, 0, sizeof(attr));
    attr.id = SAI_PORT_ATTR_SPEED;
    attr.value.u32 = 25000;
    st = port->set_port_attribute(ports[1], &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "set SPEED 25G");

    st = get_attr(ports[1], SAI_PORT_ATTR_SPEED, &attr);
    CHECK(st == SAI_STATUS_SUCCESS && attr.value.u32 == 25000,
          "speed round-trip");

    /* ---- MTU ---- */
    memset(&attr, 0, sizeof(attr));
    attr.id = SAI_PORT_ATTR_MTU;
    attr.value.u32 = 9216;
    st = port->set_port_attribute(ports[2], &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "set MTU");
    st = get_attr(ports[2], SAI_PORT_ATTR_MTU, &attr);
    CHECK(st == SAI_STATUS_SUCCESS && attr.value.u32 == 9216, "MTU round-trip");

    /* ---- FEC mode round-trip ---- */
    memset(&attr, 0, sizeof(attr));
    attr.id = SAI_PORT_ATTR_FEC_MODE;
    attr.value.s32 = SAI_PORT_FEC_MODE_RS;
    st = port->set_port_attribute(ports[3], &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "set FEC RS");

    st = get_attr(ports[3], SAI_PORT_ATTR_FEC_MODE, &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "get FEC");
    CHECK(attr.value.s32 == SAI_PORT_FEC_MODE_RS, "FEC RS round-trip");

    memset(&attr, 0, sizeof(attr));
    attr.id = SAI_PORT_ATTR_FEC_MODE;
    attr.value.s32 = 99; /* invalid */
    st = port->set_port_attribute(ports[3], &attr);
    CHECK(st == SAI_STATUS_INVALID_PARAMETER,
          "invalid FEC -> 0x%x (expected INVALID_PARAMETER)", st);

    /* ---- autoneg ---- */
    memset(&attr, 0, sizeof(attr));
    attr.id = SAI_PORT_ATTR_AUTO_NEG_MODE;
    attr.value.booldata = false;
    st = port->set_port_attribute(ports[4], &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "set autoneg off");
    st = get_attr(ports[4], SAI_PORT_ATTR_AUTO_NEG_MODE, &attr);
    CHECK(st == SAI_STATUS_SUCCESS && !attr.value.booldata,
          "autoneg round-trip");

    /* ---- stats: read, read+clear, clear-all ---- */
    const int first_port = 1; /* SAI port index 0 maps to BCM port 1 */
    mock_bcm_set_stat(first_port, snmpIfInOctets, 1111);
    mock_bcm_set_stat(first_port, snmpIfOutOctets, 2222);

    sai_stat_id_t ids[] = {
        SAI_PORT_STAT_IF_IN_OCTETS, SAI_PORT_STAT_IF_OUT_OCTETS,
    };
    uint64_t counters[2] = {0, 0};
    st = port->get_port_stats(ports[0], 2, ids, counters);
    CHECK(st == SAI_STATUS_SUCCESS, "get_port_stats -> 0x%x", st);
    CHECK(counters[0] == 1111, "in octets = %lu", (unsigned long)counters[0]);
    CHECK(counters[1] == 2222, "out octets = %lu", (unsigned long)counters[1]);
    CHECK(mock_bcm_stat_sync_count() >= 1, "bcm_stat_sync called");

    st = port->get_port_stats_ext(ports[0], 2, ids,
                                  SAI_STATS_MODE_READ_AND_CLEAR, counters);
    CHECK(st == SAI_STATUS_SUCCESS, "get_port_stats_ext R&C -> 0x%x", st);
    CHECK(counters[0] == 1111 && counters[1] == 2222, "R&C values");
    CHECK(mock_bcm_stat_clear_count(first_port) == 1,
          "R&C cleared counters");
    CHECK(mock_bcm_get_stat(first_port, snmpIfInOctets) == 0,
          "cleared value is zero");

    /* unsupported stat id */
    sai_stat_id_t bogus_id[] = { 0xDEADBEEF };
    st = port->get_port_stats(ports[0], 1, bogus_id, counters);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "bogus stat id -> 0x%x (expected NOT_SUPPORTED)", st);

    /* clear all */
    mock_bcm_set_stat(first_port, snmpIfInOctets, 777);
    st = port->clear_port_all_stats(ports[0]);
    CHECK(st == SAI_STATUS_SUCCESS, "clear_port_all_stats -> 0x%x", st);
    CHECK(mock_bcm_get_stat(first_port, snmpIfInOctets) == 0,
          "cleared after clear-all");

    /* ---- bogus port oid ---- */
    sai_object_id_t bogus = sai_bcm_oid_encode(1, SAI_OBJECT_TYPE_PORT, 9999);
    st = get_attr(bogus, SAI_PORT_ATTR_ADMIN_STATE, &attr);
    CHECK(st == SAI_STATUS_ITEM_NOT_FOUND,
          "bogus port -> 0x%x (expected NOT_FOUND)", st);

    /* non-port oid must also fail */
    st = get_attr(sid, SAI_PORT_ATTR_ADMIN_STATE, &attr);
    CHECK(st == SAI_STATUS_ITEM_NOT_FOUND,
          "switch oid as port -> 0x%x (expected NOT_FOUND)", st);

    /* ---- port pools etc. are not supported yet ---- */
    sai_object_id_t pp = SAI_NULL_OBJECT_ID;
    st = port->create_port_pool(&pp, sid, 0, NULL);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "create_port_pool -> 0x%x (expected NOT_SUPPORTED)", st);

    if (failures == 0) {
        printf("test_port: OK\n");
        return 0;
    }
    printf("test_port: %d FAILURES\n", failures);
    return 1;
}