/*
 * SAI Router Interface API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 3 scope: RIFs over PORT/LAG and VLAN.
 *  - every RIF gets an L3 ingress interface (bcm_l3_ingress_create) bound
 *    to its virtual router
 *  - VLAN RIFs keep the vlan as their egress L3 interface id (classic XGS
 *    model); PORT/LAG RIFs have no egress interface (their destination is
 *    encoded in the next-hop egress objects)
 *  - LOOPBACK/MPLS RIFs return SAI_STATUS_NOT_SUPPORTED for now
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_l3.h"

#include <string.h>

static struct sai_bcm_rif g_rifs[SAI_BCM_MAX_RIFS];

struct sai_bcm_rif *sai_bcm_rif_get(sai_object_id_t rif_oid)
{
    if (sai_bcm_oid_type(rif_oid) != SAI_OBJECT_TYPE_ROUTER_INTERFACE) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(rif_oid);
    if (idx >= SAI_BCM_MAX_RIFS || !g_rifs[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_ROUTER_INTERFACE, idx) !=
        rif_oid) {
        return NULL;
    }
    return &g_rifs[idx];
}

uint32_t sai_bcm_rif_count_on_vr(sai_object_id_t vr_oid)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < SAI_BCM_MAX_RIFS; i++) {
        if (g_rifs[i].used && g_rifs[i].vr_oid == vr_oid) {
            n++;
        }
    }
    return n;
}

int sai_bcm_rif_egress_params(const struct sai_bcm_rif *rif,
                             bcm_vlan_t *vid, bcm_gport_t *gport,
                             bool *is_trunk)
{
    if (rif == NULL || vid == NULL || gport == NULL || is_trunk == NULL) {
        return BCM_E_PARAM;
    }

    *vid = 0;
    *is_trunk = false;

    if (rif->type == SAI_ROUTER_INTERFACE_TYPE_VLAN) {
        if (sai_bcm_oid_type(rif->vlan_oid) != SAI_OBJECT_TYPE_VLAN) {
            return BCM_E_PARAM;
        }
        *vid = (bcm_vlan_t)sai_bcm_vid_from_oid(rif->vlan_oid);
        *gport = 0;
        return BCM_E_NONE;
    }
    if (rif->type == SAI_ROUTER_INTERFACE_TYPE_PORT) {
        return sai_bcm_port_or_lag_to_gport(rif->port_oid, gport);
    }
    return BCM_E_PARAM;
}

/* ---------------------------------------------------------------------------
 * SAI Router Interface API
 * --------------------------------------------------------------------------- */
static sai_status_t create_router_interface(sai_object_id_t *rif_id,
                                           sai_object_id_t switch_id,
                                           uint32_t attr_count,
                                           const sai_attribute_t *attr_list)
{
    sai_object_id_t vr_oid = SAI_NULL_OBJECT_ID;
    sai_router_interface_type_t type = SAI_ROUTER_INTERFACE_TYPE_PORT;
    sai_object_id_t port_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t vlan_oid = SAI_NULL_OBJECT_ID;
    bool have_port = false;
    bool have_vlan = false;

    (void)switch_id;

    if (rif_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID:
            vr_oid = attr_list[i].value.oid;
            break;
        case SAI_ROUTER_INTERFACE_ATTR_TYPE:
            type = (sai_router_interface_type_t)attr_list[i].value.s32;
            break;
        case SAI_ROUTER_INTERFACE_ATTR_PORT_ID:
            port_oid = attr_list[i].value.oid;
            have_port = true;
            break;
        case SAI_ROUTER_INTERFACE_ATTR_VLAN_ID:
            vlan_oid = attr_list[i].value.oid;
            have_vlan = true;
            break;
        default:
            break;
        }
    }

    if ((type == SAI_ROUTER_INTERFACE_TYPE_PORT && !have_port) ||
        (type == SAI_ROUTER_INTERFACE_TYPE_VLAN && !have_vlan)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }
    if (sai_bcm_vr_get(vr_oid) == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    struct sai_bcm_rif proto = {
        .used = true,
        .vr_oid = vr_oid,
        .type = type,
        .port_oid = port_oid,
        .vlan_oid = vlan_oid,
        .admin_state = true,
        .ingress_if = 0,
        .egress_if = 0,
    };

    /* Validate the type-specific destination. */
    bcm_vlan_t vid = 0;
    bcm_gport_t gport = 0;
    bool is_trunk = false;
    int rv = sai_bcm_rif_egress_params(&proto, &vid, &gport, &is_trunk);
    if (rv < 0) {
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    if (type == SAI_ROUTER_INTERFACE_TYPE_VLAN) {
        if (!g_sai_bcm_vlan_used[sai_bcm_vid_from_oid(vlan_oid)]) {
            sai_bcm_unlock();
            return SAI_STATUS_ITEM_NOT_FOUND;
        }
        proto.egress_if = (bcm_if_t)vid;
    }

    /* Create the L3 ingress interface bound to the RIF's VR. */
    bcm_l3_ingress_t ingress;
    bcm_l3_ingress_t_init(&ingress);
    ingress.vrf = sai_bcm_vr_get(vr_oid)->vrf;
    bcm_if_t ingress_if = 0;
    rv = bcm_l3_ingress_create(g_sai_bcm_sw.unit, &ingress, &ingress_if);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_l3_ingress_create failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }
    proto.ingress_if = ingress_if;

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_ROUTER_INTERFACE,
                                       &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_RIFS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_ROUTER_INTERFACE, idx);
        }
        (void)bcm_l3_ingress_destroy(g_sai_bcm_sw.unit, ingress_if);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_rifs[idx] = proto;
    *rif_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                 SAI_OBJECT_TYPE_ROUTER_INTERFACE, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_router_interface(sai_object_id_t rif_id)
{
    sai_bcm_lock();

    struct sai_bcm_rif *rif = sai_bcm_rif_get(rif_id);
    if (rif == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    int rv = BCM_E_NONE;
    if (rif->ingress_if != 0) {
        rv = bcm_l3_ingress_destroy(g_sai_bcm_sw.unit, rif->ingress_if);
        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_l3_ingress_destroy failed: %d (%s)", rv,
                        bcm_errmsg(rv));
        }
    }

    uint64_t idx = sai_bcm_oid_index(rif_id);
    memset(rif, 0, sizeof(*rif));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_ROUTER_INTERFACE, (uint32_t)idx);

    sai_bcm_unlock();
    return (rv < 0) ? sai_bcm_status(rv) : SAI_STATUS_SUCCESS;
}

static sai_status_t set_router_interface_attribute(sai_object_id_t rif_id,
                                                  const sai_attribute_t *attr)
{
    if (attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_rif *rif = sai_bcm_rif_get(rif_id);
    if (rif == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_NOT_SUPPORTED;
    switch (attr->id) {
    case SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE:
    case SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE:
        rif->admin_state = attr->value.booldata;
        st = SAI_STATUS_SUCCESS;
        break;
    default:
        break;
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_router_interface_attribute(sai_object_id_t rif_id,
                                                  uint32_t attr_count,
                                                  sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_rif *rif = sai_bcm_rif_get(rif_id);
    if (rif == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID:
            attr->value.oid = rif->vr_oid;
            break;
        case SAI_ROUTER_INTERFACE_ATTR_TYPE:
            attr->value.s32 = rif->type;
            break;
        case SAI_ROUTER_INTERFACE_ATTR_PORT_ID:
            attr->value.oid = rif->port_oid;
            break;
        case SAI_ROUTER_INTERFACE_ATTR_VLAN_ID:
            attr->value.oid = rif->vlan_oid;
            break;
        case SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE:
        case SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE:
            attr->value.booldata = rif->admin_state;
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
static sai_status_t rif_not_supported_3(sai_object_id_t a, uint32_t b,
                                        const sai_stat_id_t *c, uint64_t *d)
{
    (void)a; (void)b; (void)c; (void)d;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t rif_not_supported_4(sai_object_id_t a, uint32_t b,
                                        const sai_stat_id_t *c,
                                        sai_stats_mode_t d, uint64_t *e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t rif_not_supported_3c(sai_object_id_t a, uint32_t b,
                                        const sai_stat_id_t *c)
{
    (void)a; (void)b; (void)c;
    return SAI_STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_router_interface_api_t sai_bcm_router_interface_api = {
    .create_router_interface      = create_router_interface,
    .remove_router_interface      = remove_router_interface,
    .set_router_interface_attribute = set_router_interface_attribute,
    .get_router_interface_attribute = get_router_interface_attribute,
    .get_router_interface_stats   = rif_not_supported_3,
    .get_router_interface_stats_ext = rif_not_supported_4,
    .clear_router_interface_stats = rif_not_supported_3c,
};