/*
 * SAI Switch API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 1 scope: create/remove switch (bcm_init/bcm_detach), front-panel
 * port enumeration via bcm_port_config_get(), switch attributes
 * (port number/list, profile, src mac, notifications).
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_l3.h"

#include <string.h>

struct sai_bcm_switch_state g_sai_bcm_sw = {
    .unit = SAI_BCM_UNIT_DEFAULT,
};

/* ---------------------------------------------------------------------------
 * Port lookup (used by the port module)
 * --------------------------------------------------------------------------- */
sai_object_id_t sai_bcm_switch_oid_from_bcm_port(bcm_port_t bcm_port)
{
    for (uint32_t i = 0; i < g_sai_bcm_sw.port_count; i++) {
        if (g_sai_bcm_sw.ports[i].bcm_port == bcm_port) {
            return g_sai_bcm_sw.ports[i].port_oid;
        }
    }
    return SAI_NULL_OBJECT_ID;
}

const struct sai_bcm_port_entry *
sai_bcm_switch_port_lookup(sai_object_id_t port_id)
{
    /* No created check here: create_switch populates the port map before
     * bridge init walks it, while g_sai_bcm_sw.created is still false. */
    if (sai_bcm_oid_type(port_id) != SAI_OBJECT_TYPE_PORT) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(port_id);
    if (idx >= g_sai_bcm_sw.port_count) {
        return NULL;
    }
    if (g_sai_bcm_sw.ports[idx].port_oid != port_id) {
        return NULL;
    }
    return &g_sai_bcm_sw.ports[idx];
}

/* ---------------------------------------------------------------------------
 * Attribute helpers
 * --------------------------------------------------------------------------- */
static sai_status_t parse_create_attrs(uint32_t attr_count,
                                       const sai_attribute_t *attr_list)
{
    for (uint32_t i = 0; i < attr_count; i++) {
        const sai_attribute_t *attr = &attr_list[i];

        switch (attr->id) {
        case SAI_SWITCH_ATTR_INIT_SWITCH:
            if (!attr->value.booldata) {
                return SAI_STATUS_INVALID_PARAMETER;
            }
            break;
        case SAI_SWITCH_ATTR_SWITCH_PROFILE_ID:
            g_sai_bcm_sw.profile_id = attr->value.u32;
            break;
        case SAI_SWITCH_ATTR_SRC_MAC_ADDRESS:
            memcpy(g_sai_bcm_sw.src_mac, attr->value.mac,
                   sizeof(g_sai_bcm_sw.src_mac));
            break;
        case SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY:
            g_sai_bcm_sw.port_state_change_notify =
                (sai_port_state_change_notification_fn)attr->value.ptr;
            break;
        case SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY:
            g_sai_bcm_sw.fdb_event_notify =
                (sai_fdb_event_notification_fn)attr->value.ptr;
            break;
        case SAI_SWITCH_ATTR_SWITCH_SHUTDOWN_REQUEST_NOTIFY:
            g_sai_bcm_sw.shutdown_request_notify =
                (sai_switch_shutdown_request_notification_fn)attr->value.ptr;
            break;
        default:
            /* Tolerate unknown create attributes; ignore for now. */
            SAI_BCM_LOG(SAI_LOG_LEVEL_DEBUG,
                        "create_switch: ignoring attr id 0x%x", attr->id);
            break;
        }
    }
    return SAI_STATUS_SUCCESS;
}

/* Enumerate front-panel ports from the BCM port configuration and build the
 * SAI port oid map. Ports are indexed 0..port_count-1 in pbmp order. */
static sai_status_t enumerate_ports(void)
{
    bcm_port_config_t cfg;
    int rv = bcm_port_config_get(g_sai_bcm_sw.unit, &cfg);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_port_config_get failed: %d", rv);
        return sai_bcm_status(rv);
    }

    uint32_t n = 0;
    bcm_port_t p;
    BCM_PBMP_ITER(cfg.port, p) {
        if (n >= SAI_BCM_MAX_PORTS) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "too many ports (max %u)",
                        SAI_BCM_MAX_PORTS);
            return SAI_STATUS_TABLE_FULL;
        }
        g_sai_bcm_sw.ports[n].bcm_port = p;
        g_sai_bcm_sw.ports[n].port_oid = sai_bcm_oid_encode(
            SAI_BCM_SWITCH_INSTANCE_FIRST, SAI_OBJECT_TYPE_PORT, n);
        n++;
    }

    g_sai_bcm_sw.port_count = n;
    g_sai_bcm_sw.max_ports = n;
    SAI_BCM_LOG(SAI_LOG_LEVEL_INFO, "enumerated %u front panel ports", n);
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * SAI Switch API
 * --------------------------------------------------------------------------- */
static sai_status_t create_switch(sai_object_id_t *switch_id,
                                 uint32_t attr_count,
                                 const sai_attribute_t *attr_list)
{
    sai_status_t st;

    if (switch_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_ALREADY_EXISTS;
    }

    st = parse_create_attrs(attr_count, attr_list);
    if (st != SAI_STATUS_SUCCESS) {
        sai_bcm_unlock();
        return st;
    }

    int rv = bcm_init(g_sai_bcm_sw.unit);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_init(unit %d) failed: %d (%s)",
                    g_sai_bcm_sw.unit, rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    st = enumerate_ports();
    if (st != SAI_STATUS_SUCCESS) {
        (void)bcm_detach(g_sai_bcm_sw.unit);
        sai_bcm_unlock();
        return st;
    }

    /* Default 1Q bridge + one bridge port per front-panel port. */
    st = sai_bcm_bridge_init();
    if (st != SAI_STATUS_SUCCESS) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bridge init failed: 0x%x", st);
        (void)bcm_detach(g_sai_bcm_sw.unit);
        sai_bcm_unlock();
        return st;
    }

    /* Default virtual router (vrf 0). */
    st = sai_bcm_vr_init();
    if (st != SAI_STATUS_SUCCESS) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "vr init failed: 0x%x", st);
        sai_bcm_bridge_deinit();
        (void)bcm_detach(g_sai_bcm_sw.unit);
        sai_bcm_unlock();
        return st;
    }

    g_sai_bcm_sw.switch_oid = sai_bcm_oid_encode(
        SAI_BCM_SWITCH_INSTANCE_FIRST, SAI_OBJECT_TYPE_SWITCH, 0);
    g_sai_bcm_sw.created = true;
    *switch_id = g_sai_bcm_sw.switch_oid;

    SAI_BCM_LOG(SAI_LOG_LEVEL_INFO,
                "switch created (target=%s unit=%d ports=%u oid=0x%lx)",
                SAI_BCM_TARGET_NAME, g_sai_bcm_sw.unit,
                g_sai_bcm_sw.port_count, (unsigned long)*switch_id);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_switch(sai_object_id_t switch_id)
{
    sai_bcm_lock();

    if (!g_sai_bcm_sw.created || switch_id != g_sai_bcm_sw.switch_oid) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    int rv = bcm_detach(g_sai_bcm_sw.unit);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_detach failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    sai_bcm_bridge_deinit();
    sai_bcm_vr_deinit();
    memset(&g_sai_bcm_sw, 0, sizeof(g_sai_bcm_sw));
    g_sai_bcm_sw.unit = SAI_BCM_UNIT_DEFAULT;

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_switch_attribute(sai_object_id_t switch_id,
                                         const sai_attribute_t *attr)
{
    if (attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created || switch_id != g_sai_bcm_sw.switch_oid) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st;
    switch (attr->id) {
    case SAI_SWITCH_ATTR_SRC_MAC_ADDRESS:
        memcpy(g_sai_bcm_sw.src_mac, attr->value.mac,
               sizeof(g_sai_bcm_sw.src_mac));
        st = SAI_STATUS_SUCCESS;
        break;
    case SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY:
        g_sai_bcm_sw.port_state_change_notify =
            (sai_port_state_change_notification_fn)attr->value.ptr;
        st = SAI_STATUS_SUCCESS;
        break;
    case SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY:
        g_sai_bcm_sw.fdb_event_notify =
            (sai_fdb_event_notification_fn)attr->value.ptr;
        st = SAI_STATUS_SUCCESS;
        break;
    case SAI_SWITCH_ATTR_SWITCH_SHUTDOWN_REQUEST_NOTIFY:
        g_sai_bcm_sw.shutdown_request_notify =
            (sai_switch_shutdown_request_notification_fn)attr->value.ptr;
        st = SAI_STATUS_SUCCESS;
        break;
    default:
        st = SAI_STATUS_NOT_SUPPORTED;
        break;
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_switch_attribute(sai_object_id_t switch_id,
                                         uint32_t attr_count,
                                         sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created || switch_id != g_sai_bcm_sw.switch_oid) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_SUCCESS;

    for (uint32_t i = 0; i < attr_count && st == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];

        switch (attr->id) {
        case SAI_SWITCH_ATTR_PORT_NUMBER:
            attr->value.u32 = g_sai_bcm_sw.port_count;
            break;
        case SAI_SWITCH_ATTR_MAX_NUMBER_OF_SUPPORTED_PORTS:
            attr->value.u32 = g_sai_bcm_sw.max_ports;
            break;
        case SAI_SWITCH_ATTR_PORT_LIST:
            if (attr->value.objlist.list == NULL ||
                attr->value.objlist.count < g_sai_bcm_sw.port_count) {
                attr->value.objlist.count = g_sai_bcm_sw.port_count;
                st = SAI_STATUS_BUFFER_OVERFLOW;
            } else {
                attr->value.objlist.count = g_sai_bcm_sw.port_count;
                for (uint32_t j = 0; j < g_sai_bcm_sw.port_count; j++) {
                    attr->value.objlist.list[j] =
                        g_sai_bcm_sw.ports[j].port_oid;
                }
            }
            break;
        case SAI_SWITCH_ATTR_SWITCH_PROFILE_ID:
            attr->value.u32 = g_sai_bcm_sw.profile_id;
            break;
        case SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID:
            if (!g_sai_bcm_default_1q_bridge_oid) {
                st = SAI_STATUS_ITEM_NOT_FOUND;
            } else {
                attr->value.oid = g_sai_bcm_default_1q_bridge_oid;
            }
            break;
        case SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID:
            if (g_sai_bcm_default_vr_oid == SAI_NULL_OBJECT_ID) {
                st = SAI_STATUS_ITEM_NOT_FOUND;
            } else {
                attr->value.oid = g_sai_bcm_default_vr_oid;
            }
            break;
        case SAI_SWITCH_ATTR_SRC_MAC_ADDRESS:
            memcpy(attr->value.mac, g_sai_bcm_sw.src_mac,
                   sizeof(g_sai_bcm_sw.src_mac));
            break;
        case SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY:
            attr->value.ptr = (void *)(uintptr_t)g_sai_bcm_sw.port_state_change_notify;
            break;
        case SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY:
            attr->value.ptr = (void *)(uintptr_t)g_sai_bcm_sw.fdb_event_notify;
            break;
        case SAI_SWITCH_ATTR_SWITCH_SHUTDOWN_REQUEST_NOTIFY:
            attr->value.ptr = (void *)(uintptr_t)g_sai_bcm_sw.shutdown_request_notify;
            break;
        default:
            st = SAI_STATUS_NOT_SUPPORTED;
            break;
        }
    }

    sai_bcm_unlock();
    return st;
}

/* ---------------------------------------------------------------------------
 * Not-yet-implemented members (explicit NOT_SUPPORTED)
 * --------------------------------------------------------------------------- */
static sai_status_t get_switch_stats(sai_object_id_t switch_id,
                                     uint32_t number_of_counters,
                                     const sai_stat_id_t *counter_ids,
                                     uint64_t *counters)
{
    (void)switch_id; (void)number_of_counters; (void)counter_ids; (void)counters;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_switch_stats_ext(sai_object_id_t switch_id,
                                        uint32_t number_of_counters,
                                        const sai_stat_id_t *counter_ids,
                                        sai_stats_mode_t mode,
                                        uint64_t *counters)
{
    (void)switch_id; (void)number_of_counters; (void)counter_ids; (void)mode;
    (void)counters;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t clear_switch_stats(sai_object_id_t switch_id,
                                       uint32_t number_of_counters,
                                       const sai_stat_id_t *counter_ids)
{
    (void)switch_id; (void)number_of_counters; (void)counter_ids;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t switch_mdio_read(sai_object_id_t switch_id,
                                    uint32_t device_addr,
                                    uint32_t start_reg_addr,
                                    uint32_t number_of_registers,
                                    uint32_t *reg_val)
{
    (void)switch_id; (void)device_addr; (void)start_reg_addr;
    (void)number_of_registers; (void)reg_val;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t switch_mdio_write(sai_object_id_t switch_id,
                                     uint32_t device_addr,
                                     uint32_t start_reg_addr,
                                     uint32_t number_of_registers,
                                     const uint32_t *reg_val)
{
    (void)switch_id; (void)device_addr; (void)start_reg_addr;
    (void)number_of_registers; (void)reg_val;
    return SAI_STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_switch_api_t sai_bcm_switch_api = {
    .create_switch           = create_switch,
    .remove_switch           = remove_switch,
    .set_switch_attribute    = set_switch_attribute,
    .get_switch_attribute    = get_switch_attribute,
    .get_switch_stats        = get_switch_stats,
    .get_switch_stats_ext    = get_switch_stats_ext,
    .clear_switch_stats      = clear_switch_stats,
    .switch_mdio_read        = switch_mdio_read,
    .switch_mdio_write       = switch_mdio_write,
};