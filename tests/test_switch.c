/*
 * Unit tests for the SAI Switch API over the mock BCM backend.
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

static void init_adapter(void)
{
    mock_bcm_reset();
    memset(&g_sai_bcm_sw, 0, sizeof(g_sai_bcm_sw));
    g_sai_bcm_sw.unit = SAI_BCM_UNIT_DEFAULT;

    sai_status_t st = sai_api_initialize(0, NULL);
    CHECK(st == SAI_STATUS_SUCCESS, "sai_api_initialize -> 0x%x", st);

    st = sai_api_query(SAI_API_SWITCH, (void **)&sw);
    CHECK(st == SAI_STATUS_SUCCESS, "sai_api_query(SWITCH) -> 0x%x", st);
    CHECK(sw != NULL, "switch table non-null");
}

int main(void)
{
    init_adapter();

    /* -- create_switch without INIT_SWITCH must still succeed -- */
    sai_object_id_t sid = SAI_NULL_OBJECT_ID;
    sai_status_t st = sw->create_switch(&sid, 0, NULL);
    CHECK(st == SAI_STATUS_SUCCESS, "create_switch -> 0x%x", st);
    CHECK(sid != SAI_NULL_OBJECT_ID, "switch oid non-null");
    CHECK(sai_bcm_oid_type(sid) == SAI_OBJECT_TYPE_SWITCH,
          "oid type is SWITCH");
    CHECK(mock_bcm_init_count() == 1, "bcm_init called once");

    /* -- duplicate create must fail -- */
    sai_object_id_t sid2 = SAI_NULL_OBJECT_ID;
    st = sw->create_switch(&sid2, 0, NULL);
    CHECK(st == SAI_STATUS_ITEM_ALREADY_EXISTS,
          "second create -> 0x%x (expected ALREADY_EXISTS)", st);

    /* -- port number / list -- */
    sai_attribute_t attr = {0};
    attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;
    st = sw->get_switch_attribute(sid, 1, &attr);
    CHECK(st == SAI_STATUS_SUCCESS, "get PORT_NUMBER -> 0x%x", st);
    CHECK(attr.value.u32 == MOCK_BCM_FRONT_PANEL_PORTS,
          "PORT_NUMBER = %u (expected %d)", attr.value.u32,
          MOCK_BCM_FRONT_PANEL_PORTS);

    sai_object_id_t port_ids[MOCK_BCM_FRONT_PANEL_PORTS];
    sai_attribute_t lattr = {0};
    lattr.id = SAI_SWITCH_ATTR_PORT_LIST;
    lattr.value.objlist.count = MOCK_BCM_FRONT_PANEL_PORTS;
    lattr.value.objlist.list = port_ids;
    st = sw->get_switch_attribute(sid, 1, &lattr);
    CHECK(st == SAI_STATUS_SUCCESS, "get PORT_LIST -> 0x%x", st);
    CHECK(lattr.value.objlist.count == MOCK_BCM_FRONT_PANEL_PORTS,
          "PORT_LIST count");
    for (uint32_t i = 0; i < lattr.value.objlist.count; i++) {
        CHECK(port_ids[i] != SAI_NULL_OBJECT_ID, "port[%u] non-null", i);
        CHECK(sai_bcm_oid_type(port_ids[i]) == SAI_OBJECT_TYPE_PORT,
              "port[%u] type", i);
    }

    /* -- PORT_LIST with too small a buffer must overflow -- */
    sai_object_id_t small[2];
    sai_attribute_t sattr = {0};
    sattr.id = SAI_SWITCH_ATTR_PORT_LIST;
    sattr.value.objlist.count = 2;
    sattr.value.objlist.list = small;
    st = sw->get_switch_attribute(sid, 1, &sattr);
    CHECK(st == SAI_STATUS_BUFFER_OVERFLOW,
          "small PORT_LIST -> 0x%x (expected BUFFER_OVERFLOW)", st);

    /* -- notifications: set then get round-trip -- */
    sai_attribute_t nattr = {0};
    nattr.id = SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY;
    nattr.value.ptr = (void *)(uintptr_t)0xdeadbeef;
    st = sw->set_switch_attribute(sid, &nattr);
    CHECK(st == SAI_STATUS_SUCCESS, "set notify -> 0x%x", st);

    sai_attribute_t getattr = {0};
    getattr.id = SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY;
    st = sw->get_switch_attribute(sid, 1, &getattr);
    CHECK(st == SAI_STATUS_SUCCESS, "get notify -> 0x%x", st);
    CHECK(getattr.value.ptr == (void *)(uintptr_t)0xdeadbeef,
          "notify round-trip");

    /* -- wrong switch id must fail -- */
    sai_object_id_t bogus = sai_bcm_oid_encode(9, SAI_OBJECT_TYPE_SWITCH, 0);
    attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;
    st = sw->get_switch_attribute(bogus, 1, &attr);
    CHECK(st == SAI_STATUS_ITEM_NOT_FOUND,
          "wrong switch id -> 0x%x (expected NOT_FOUND)", st);

    /* -- unknown attribute must return NOT_SUPPORTED -- */
    attr.id = SAI_SWITCH_ATTR_MAX_NUMBER_OF_TEMP_SENSORS;
    st = sw->get_switch_attribute(sid, 1, &attr);
    CHECK(st == SAI_STATUS_NOT_SUPPORTED,
          "unknown attr -> 0x%x (expected NOT_SUPPORTED)", st);

    /* -- object queries -- */
    CHECK(sai_object_type_query(port_ids[0]) == SAI_OBJECT_TYPE_PORT,
          "object_type_query(port)");
    CHECK(sai_object_type_query(sid) == SAI_OBJECT_TYPE_SWITCH,
          "object_type_query(switch)");
    CHECK(sai_object_type_query(SAI_NULL_OBJECT_ID) == SAI_OBJECT_TYPE_NULL,
          "object_type_query(null)");
    CHECK(sai_switch_id_query(port_ids[3]) == sid,
          "switch_id_query(port) == switch oid");

    /* -- availability -- */
    uint64_t avail = 0;
    st = sai_object_type_get_availability(sid, SAI_OBJECT_TYPE_PORT, 0, NULL,
                                          &avail);
    CHECK(st == SAI_STATUS_SUCCESS, "get_availability(PORT) -> 0x%x", st);
    CHECK(avail == MOCK_BCM_FRONT_PANEL_PORTS, "availability = %lu",
          (unsigned long)avail);

    /* -- remove, then everything must report not-found -- */
    st = sw->remove_switch(sid);
    CHECK(st == SAI_STATUS_SUCCESS, "remove_switch -> 0x%x", st);
    attr.id = SAI_SWITCH_ATTR_PORT_NUMBER;
    st = sw->get_switch_attribute(sid, 1, &attr);
    CHECK(st == SAI_STATUS_ITEM_NOT_FOUND,
          "get after remove -> 0x%x (expected NOT_FOUND)", st);
    st = sw->remove_switch(sid);
    CHECK(st == SAI_STATUS_ITEM_NOT_FOUND,
          "double remove -> 0x%x (expected NOT_FOUND)", st);
    CHECK(sai_switch_id_query(port_ids[0]) == SAI_NULL_OBJECT_ID,
          "switch_id_query after remove");

    /* -- INIT_SWITCH=false must be rejected -- */
    memset(&g_sai_bcm_sw, 0, sizeof(g_sai_bcm_sw));
    g_sai_bcm_sw.unit = SAI_BCM_UNIT_DEFAULT;
    sai_attribute_t bad_init = {0};
    bad_init.id = SAI_SWITCH_ATTR_INIT_SWITCH;
    bad_init.value.booldata = false;
    st = sw->create_switch(&sid, 1, &bad_init);
    CHECK(st == SAI_STATUS_INVALID_PARAMETER,
          "INIT_SWITCH=false -> 0x%x (expected INVALID_PARAMETER)", st);

    if (failures == 0) {
        printf("test_switch: OK\n");
        return 0;
    }
    printf("test_switch: %d FAILURES\n", failures);
    return 1;
}