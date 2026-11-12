/*
 * SAI Bridge / Bridge Port API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 2 scope:
 *  - one default 1Q bridge created at switch bring-up (1Q bridge create is
 *    rejected: SAI allows only a single VLAN-aware bridge)
 *  - 1D bridges via bcm_vswitch_create; ports added/removed on bridge port
 *    create/remove via bcm_vswitch_port_add/delete
 *  - bridge ports reference a front-panel port or a LAG
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"

#include <string.h>

static struct sai_bcm_bridge       g_bridges[SAI_BCM_MAX_BRIDGES];
static struct sai_bcm_bridge_port  g_bridge_ports[SAI_BCM_MAX_BRIDGE_PORTS];

sai_object_id_t g_sai_bcm_default_1q_bridge_oid = SAI_NULL_OBJECT_ID;

/* ---------------------------------------------------------------------------
 * State accessors
 * --------------------------------------------------------------------------- */
struct sai_bcm_bridge *sai_bcm_bridge_get(sai_object_id_t bridge_oid)
{
    if (sai_bcm_oid_type(bridge_oid) != SAI_OBJECT_TYPE_BRIDGE) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(bridge_oid);
    if (idx >= SAI_BCM_MAX_BRIDGES || !g_bridges[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_BRIDGE, idx) != bridge_oid) {
        return NULL;
    }
    return &g_bridges[idx];
}

struct sai_bcm_bridge_port *
sai_bcm_bridge_port_get(sai_object_id_t bridge_port_oid)
{
    if (sai_bcm_oid_type(bridge_port_oid) != SAI_OBJECT_TYPE_BRIDGE_PORT) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(bridge_port_oid);
    if (idx >= SAI_BCM_MAX_BRIDGE_PORTS || !g_bridge_ports[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_BRIDGE_PORT, idx) !=
        bridge_port_oid) {
        return NULL;
    }
    return &g_bridge_ports[idx];
}

int sai_bcm_bridge_port_to_gport(sai_object_id_t bridge_port_oid,
                                bcm_gport_t *gport)
{
    struct sai_bcm_bridge_port *bp = sai_bcm_bridge_port_get(bridge_port_oid);
    if (bp == NULL || gport == NULL) {
        return BCM_E_NOT_FOUND;
    }
    return sai_bcm_port_or_lag_to_gport(bp->port_oid, gport);
}

/* ---------------------------------------------------------------------------
 * Init: default 1Q bridge + per-port bridge ports (called from
 * create_switch; the recursive adapter lock is already held)
 * --------------------------------------------------------------------------- */
static sai_status_t bridge_port_create_internal(sai_object_id_t bridge_oid,
                                               sai_object_id_t port_oid,
                                               sai_object_id_t *bridge_port_oid);

sai_status_t sai_bcm_bridge_init(void)
{
    memset(g_bridges, 0, sizeof(g_bridges));
    memset(g_bridge_ports, 0, sizeof(g_bridge_ports));

    /* Reserve index 0 for the default 1Q bridge so that 1D creates
     * allocate different indices. */
    uint32_t idx0;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_BRIDGE, &idx0);
    if (st != SAI_STATUS_SUCCESS || idx0 != 0) {
        return (st != SAI_STATUS_SUCCESS) ? st : SAI_STATUS_FAILURE;
    }

    g_bridges[0] = (struct sai_bcm_bridge){
        .used = true,
        .type = SAI_BRIDGE_TYPE_1Q,
        .is_default_1q = true,
        .vsi = 0,
    };
    g_sai_bcm_default_1q_bridge_oid =
        sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_BRIDGE, 0);

    /* One bridge port per front-panel port on the default 1Q bridge. */
    for (uint32_t i = 0; i < g_sai_bcm_sw.port_count; i++) {
        sai_object_id_t bpid = SAI_NULL_OBJECT_ID;
        sai_status_t st = bridge_port_create_internal(
            g_sai_bcm_default_1q_bridge_oid, g_sai_bcm_sw.ports[i].port_oid,
            &bpid);
        if (st != SAI_STATUS_SUCCESS) {
            return st;
        }
    }
    return SAI_STATUS_SUCCESS;
}

void sai_bcm_bridge_deinit(void)
{
    memset(g_bridges, 0, sizeof(g_bridges));
    memset(g_bridge_ports, 0, sizeof(g_bridge_ports));
    g_sai_bcm_default_1q_bridge_oid = SAI_NULL_OBJECT_ID;
    sai_bcm_idx_free(SAI_OBJECT_TYPE_BRIDGE, 0);
}

/* ---------------------------------------------------------------------------
 * Bridge API
 * --------------------------------------------------------------------------- */
static sai_status_t create_bridge(sai_object_id_t *bridge_id,
                                 sai_object_id_t switch_id,
                                 uint32_t attr_count,
                                 const sai_attribute_t *attr_list)
{
    sai_bridge_type_t type = SAI_BRIDGE_TYPE_1D;

    (void)switch_id;

    if (bridge_id == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < attr_count; i++) {
        if (attr_list[i].id == SAI_BRIDGE_ATTR_TYPE) {
            type = (sai_bridge_type_t)attr_list[i].value.s32;
        }
    }

    if (type != SAI_BRIDGE_TYPE_1D) {
        /* The single 1Q bridge exists implicitly; it cannot be created. */
        return SAI_STATUS_NOT_SUPPORTED;
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    bcm_vlan_t vsi = 0;
    int rv = bcm_vswitch_create(g_sai_bcm_sw.unit, &vsi);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_vswitch_create failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_BRIDGE, &idx);
    if (st != SAI_STATUS_SUCCESS) {
        (void)bcm_vswitch_destroy(g_sai_bcm_sw.unit, vsi);
        sai_bcm_unlock();
        return st;
    }
    if (idx >= SAI_BCM_MAX_BRIDGES) {
        sai_bcm_idx_free(SAI_OBJECT_TYPE_BRIDGE, idx);
        (void)bcm_vswitch_destroy(g_sai_bcm_sw.unit, vsi);
        sai_bcm_unlock();
        return SAI_STATUS_TABLE_FULL;
    }

    g_bridges[idx] = (struct sai_bcm_bridge){
        .used = true,
        .type = SAI_BRIDGE_TYPE_1D,
        .is_default_1q = false,
        .vsi = vsi,
    };
    *bridge_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                    SAI_OBJECT_TYPE_BRIDGE, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_bridge(sai_object_id_t bridge_id)
{
    sai_bcm_lock();

    struct sai_bcm_bridge *b = sai_bcm_bridge_get(bridge_id);
    if (b == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }
    if (b->is_default_1q) {
        sai_bcm_unlock();
        return SAI_STATUS_NOT_SUPPORTED;
    }

    /* Refuse while bridge ports still reference it. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_BRIDGE_PORTS; i++) {
        if (g_bridge_ports[i].used &&
            g_bridge_ports[i].bridge_oid == bridge_id) {
            sai_bcm_unlock();
            return SAI_STATUS_OBJECT_IN_USE;
        }
    }

    int rv = bcm_vswitch_destroy(g_sai_bcm_sw.unit, b->vsi);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_vswitch_destroy failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint64_t idx = sai_bcm_oid_index(bridge_id);
    memset(b, 0, sizeof(*b));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_BRIDGE, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_bridge_attribute(sai_object_id_t bridge_id,
                                        const sai_attribute_t *attr)
{
    (void)bridge_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_bridge_attribute(sai_object_id_t bridge_id,
                                        uint32_t attr_count,
                                        sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_bridge *b = sai_bcm_bridge_get(bridge_id);
    if (b == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_BRIDGE_ATTR_TYPE:
            attr->value.s32 = b->type;
            break;
        case SAI_BRIDGE_ATTR_PORT_LIST: {
            uint32_t n = 0;
            for (uint32_t j = 0; j < SAI_BCM_MAX_BRIDGE_PORTS; j++) {
                if (g_bridge_ports[j].used &&
                    g_bridge_ports[j].bridge_oid == bridge_id) {
                    n++;
                }
            }
            if (attr->value.objlist.list == NULL ||
                attr->value.objlist.count < n) {
                attr->value.objlist.count = n;
                out = SAI_STATUS_BUFFER_OVERFLOW;
            } else {
                uint32_t k = 0;
                for (uint32_t j = 0; j < SAI_BCM_MAX_BRIDGE_PORTS; j++) {
                    if (g_bridge_ports[j].used &&
                        g_bridge_ports[j].bridge_oid == bridge_id) {
                        attr->value.objlist.list[k++] =
                            sai_bcm_oid_encode(
                                SAI_BCM_SWITCH_INSTANCE_FIRST,
                                SAI_OBJECT_TYPE_BRIDGE_PORT, j);
                    }
                }
                attr->value.objlist.count = n;
            }
            break;
        }
        default:
            out = SAI_STATUS_NOT_SUPPORTED;
            break;
        }
    }

    sai_bcm_unlock();
    return out;
}

/* ---------------------------------------------------------------------------
 * Bridge Port API
 * --------------------------------------------------------------------------- */
static sai_status_t bridge_port_create_internal(sai_object_id_t bridge_oid,
                                               sai_object_id_t port_oid,
                                               sai_object_id_t *bridge_port_oid)
{
    struct sai_bcm_bridge *b = sai_bcm_bridge_get(bridge_oid);
    if (b == NULL) {
        return SAI_STATUS_ITEM_NOT_FOUND;
    }
    if (!sai_bcm_is_port_or_lag(port_oid)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    /* One bridge port per (bridge, port) pair. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_BRIDGE_PORTS; i++) {
        if (g_bridge_ports[i].used &&
            g_bridge_ports[i].bridge_oid == bridge_oid &&
            g_bridge_ports[i].port_oid == port_oid) {
            return SAI_STATUS_ITEM_ALREADY_EXISTS;
        }
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_BRIDGE_PORT, &idx);
    if (st != SAI_STATUS_SUCCESS) {
        return st;
    }
    if (idx >= SAI_BCM_MAX_BRIDGE_PORTS) {
        sai_bcm_idx_free(SAI_OBJECT_TYPE_BRIDGE_PORT, idx);
        return SAI_STATUS_TABLE_FULL;
    }

    /* 1D bridges keep their port set in the vswitch. */
    if (b->type == SAI_BRIDGE_TYPE_1D) {
        bcm_gport_t gport = 0;
        int rv = sai_bcm_port_or_lag_to_gport(port_oid, &gport);
        if (rv >= 0) {
            rv = bcm_vswitch_port_add(g_sai_bcm_sw.unit, b->vsi, gport);
        }
        if (rv < 0) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_BRIDGE_PORT, idx);
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_vswitch_port_add failed: %d (%s)",
                        rv, bcm_errmsg(rv));
            return sai_bcm_status(rv);
        }
    }

    g_bridge_ports[idx] = (struct sai_bcm_bridge_port){
        .used = true,
        .bridge_oid = bridge_oid,
        .port_oid = port_oid,
        .admin_state = true,
    };
    *bridge_port_oid = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                         SAI_OBJECT_TYPE_BRIDGE_PORT, idx);
    return SAI_STATUS_SUCCESS;
}

static sai_status_t create_bridge_port(sai_object_id_t *bridge_port_id,
                                      sai_object_id_t switch_id,
                                      uint32_t attr_count,
                                      const sai_attribute_t *attr_list)
{
    sai_object_id_t bridge_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t port_oid = SAI_NULL_OBJECT_ID;
    bool have_port = false;

    (void)switch_id;

    if (bridge_port_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_BRIDGE_PORT_ATTR_BRIDGE_ID:
            bridge_oid = attr_list[i].value.oid;
            break;
        case SAI_BRIDGE_PORT_ATTR_PORT_ID:
            port_oid = attr_list[i].value.oid;
            have_port = true;
            break;
        default:
            break;
        }
    }
    if (!have_port) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }
    sai_status_t st = bridge_port_create_internal(bridge_oid, port_oid,
                                                  bridge_port_id);
    sai_bcm_unlock();
    return st;
}

static sai_status_t remove_bridge_port(sai_object_id_t bridge_port_id)
{
    sai_bcm_lock();

    struct sai_bcm_bridge_port *bp = sai_bcm_bridge_port_get(bridge_port_id);
    if (bp == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    struct sai_bcm_bridge *b = sai_bcm_bridge_get(bp->bridge_oid);

    if (b != NULL && b->type == SAI_BRIDGE_TYPE_1D) {
        bcm_gport_t gport = 0;
        int rv = sai_bcm_port_or_lag_to_gport(bp->port_oid, &gport);
        if (rv >= 0) {
            (void)bcm_vswitch_port_delete(g_sai_bcm_sw.unit, b->vsi, gport);
        }
    }

    uint64_t idx = sai_bcm_oid_index(bridge_port_id);
    memset(bp, 0, sizeof(*bp));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_BRIDGE_PORT, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_bridge_port_attribute(sai_object_id_t bridge_port_id,
                                             const sai_attribute_t *attr)
{
    if (attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_bridge_port *bp = sai_bcm_bridge_port_get(bridge_port_id);
    if (bp == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_SUCCESS;
    switch (attr->id) {
    case SAI_BRIDGE_PORT_ATTR_ADMIN_STATE:
        bp->admin_state = attr->value.booldata;
        /* For physical-port bridge ports the admin state drives the port
         * forwarding state (classic Broadcom SAI mapping). */
        if (sai_bcm_oid_type(bp->port_oid) == SAI_OBJECT_TYPE_PORT) {
            const struct sai_bcm_port_entry *pe =
                sai_bcm_switch_port_lookup(bp->port_oid);
            if (pe != NULL) {
                int rv = bcm_port_enable_set(
                    g_sai_bcm_sw.unit, pe->bcm_port,
                    attr->value.booldata ? 1 : 0);
                if (rv < 0) {
                    SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                                "bcm_port_enable_set failed: %d (%s)", rv,
                                bcm_errmsg(rv));
                    st = sai_bcm_status(rv);
                }
            }
        }
        break;
    default:
        st = SAI_STATUS_NOT_SUPPORTED;
        break;
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_bridge_port_attribute(sai_object_id_t bridge_port_id,
                                             uint32_t attr_count,
                                             sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_bridge_port *bp = sai_bcm_bridge_port_get(bridge_port_id);
    if (bp == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_BRIDGE_PORT_ATTR_BRIDGE_ID:
            attr->value.oid = bp->bridge_oid;
            break;
        case SAI_BRIDGE_PORT_ATTR_PORT_ID:
            attr->value.oid = bp->port_oid;
            break;
        case SAI_BRIDGE_PORT_ATTR_ADMIN_STATE:
            attr->value.booldata = bp->admin_state;
            break;
        default:
            out = SAI_STATUS_NOT_SUPPORTED;
            break;
        }
    }

    sai_bcm_unlock();
    return out;
}

/* ---------------------------------------------------------------------------
 * Not-yet-implemented members
 * --------------------------------------------------------------------------- */
static sai_status_t bridge_not_supported_3(sai_object_id_t a, uint32_t b,
                                          const sai_stat_id_t *c, uint64_t *d)
{
    (void)a; (void)b; (void)c; (void)d;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t bridge_not_supported_4(sai_object_id_t a, uint32_t b,
                                          const sai_stat_id_t *c,
                                          sai_stats_mode_t d, uint64_t *e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t bridge_not_supported_3c(sai_object_id_t a, uint32_t b,
                                           const sai_stat_id_t *c)
{
    (void)a; (void)b; (void)c;
    return SAI_STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_bridge_api_t sai_bcm_bridge_api = {
    .create_bridge              = create_bridge,
    .remove_bridge              = remove_bridge,
    .set_bridge_attribute       = set_bridge_attribute,
    .get_bridge_attribute       = get_bridge_attribute,
    .get_bridge_stats           = bridge_not_supported_3,
    .get_bridge_stats_ext       = bridge_not_supported_4,
    .clear_bridge_stats         = bridge_not_supported_3c,
    .create_bridge_port         = create_bridge_port,
    .remove_bridge_port         = remove_bridge_port,
    .set_bridge_port_attribute  = set_bridge_port_attribute,
    .get_bridge_port_attribute  = get_bridge_port_attribute,
    .get_bridge_port_stats       = bridge_not_supported_3,
    .get_bridge_port_stats_ext   = bridge_not_supported_4,
    .clear_bridge_port_stats     = bridge_not_supported_3c,
};