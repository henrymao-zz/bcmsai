/*
 * SAI Virtual Router API on top of OpenBCM (sdk-6.5.27).
 *
 * OpenBCM 6.5.27 has no bcm_vrf_create/destroy: VRFs are implicit integer
 * identifiers referenced by bcm_l3_* structures. This module allocates VRF
 * ids from an internal pool, exposes the switch default VR (vrf 0) via
 * SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID, and retains the SAI-side
 * attributes (admin states, src mac).
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l3.h"

#include <string.h>

static struct sai_bcm_vr g_vrs[SAI_BCM_MAX_VRS];

sai_object_id_t g_sai_bcm_default_vr_oid = SAI_NULL_OBJECT_ID;

struct sai_bcm_vr *sai_bcm_vr_get(sai_object_id_t vr_oid)
{
    if (sai_bcm_oid_type(vr_oid) != SAI_OBJECT_TYPE_VIRTUAL_ROUTER) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(vr_oid);
    if (idx >= SAI_BCM_MAX_VRS || !g_vrs[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_VIRTUAL_ROUTER, idx) != vr_oid) {
        return NULL;
    }
    return &g_vrs[idx];
}

sai_status_t sai_bcm_vr_init(void)
{
    memset(g_vrs, 0, sizeof(g_vrs));

    /* Reserve index 0 for the default VR. */
    uint32_t idx0;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, &idx0);
    if (st != SAI_STATUS_SUCCESS || idx0 != 0) {
        return (st != SAI_STATUS_SUCCESS) ? st : SAI_STATUS_FAILURE;
    }

    g_vrs[0] = (struct sai_bcm_vr){
        .used = true,
        .is_default = true,
        .vrf = 0,
        .admin_v4 = true,
        .admin_v6 = true,
    };
    g_sai_bcm_default_vr_oid = sai_bcm_oid_encode(
        SAI_BCM_SWITCH_INSTANCE_FIRST, SAI_OBJECT_TYPE_VIRTUAL_ROUTER, 0);
    return SAI_STATUS_SUCCESS;
}

void sai_bcm_vr_deinit(void)
{
    memset(g_vrs, 0, sizeof(g_vrs));
    g_sai_bcm_default_vr_oid = SAI_NULL_OBJECT_ID;
    sai_bcm_idx_free(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, 0);
}

/* ---------------------------------------------------------------------------
 * SAI Virtual Router API
 * --------------------------------------------------------------------------- */
static sai_status_t create_virtual_router(sai_object_id_t *virtual_router_id,
                                         sai_object_id_t switch_id,
                                         uint32_t attr_count,
                                         const sai_attribute_t *attr_list)
{
    (void)switch_id;

    if (virtual_router_id == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    /* Find a free slot + vrf id (vrf 0 is the default VR). */
    for (uint32_t i = 1; i < SAI_BCM_MAX_VRS; i++) {
        if (!g_vrs[i].used) {
            g_vrs[i] = (struct sai_bcm_vr){
                .used = true,
                .is_default = false,
                .vrf = (bcm_vrf_t)i,
                .admin_v4 = true,
                .admin_v6 = true,
            };
            *virtual_router_id = sai_bcm_oid_encode(
                SAI_BCM_SWITCH_INSTANCE_FIRST, SAI_OBJECT_TYPE_VIRTUAL_ROUTER,
                i);
            sai_bcm_unlock();
            return SAI_STATUS_SUCCESS;
        }
    }

    sai_bcm_unlock();
    return SAI_STATUS_TABLE_FULL;
}

static sai_status_t remove_virtual_router(sai_object_id_t virtual_router_id)
{
    sai_bcm_lock();

    struct sai_bcm_vr *vr = sai_bcm_vr_get(virtual_router_id);
    if (vr == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }
    if (vr->is_default) {
        sai_bcm_unlock();
        return SAI_STATUS_NOT_SUPPORTED;
    }

    /* Refuse while RIFs still reference this VR. */
    if (sai_bcm_rif_count_on_vr(virtual_router_id) > 0) {
        sai_bcm_unlock();
        return SAI_STATUS_OBJECT_IN_USE;
    }

    uint64_t idx = sai_bcm_oid_index(virtual_router_id);
    memset(vr, 0, sizeof(*vr));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_virtual_router_attribute(sai_object_id_t vr_oid,
                                                 const sai_attribute_t *attr)
{
    if (attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_vr *vr = sai_bcm_vr_get(vr_oid);
    if (vr == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_SUCCESS;
    switch (attr->id) {
    case SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE:
        vr->admin_v4 = attr->value.booldata;
        break;
    case SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE:
        vr->admin_v6 = attr->value.booldata;
        break;
    case SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS:
        memcpy(vr->src_mac, attr->value.mac, sizeof(vr->src_mac));
        break;
    default:
        st = SAI_STATUS_NOT_SUPPORTED;
        break;
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_virtual_router_attribute(sai_object_id_t vr_oid,
                                                uint32_t attr_count,
                                                sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_vr *vr = sai_bcm_vr_get(vr_oid);
    if (vr == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V4_STATE:
            attr->value.booldata = vr->admin_v4;
            break;
        case SAI_VIRTUAL_ROUTER_ATTR_ADMIN_V6_STATE:
            attr->value.booldata = vr->admin_v6;
            break;
        case SAI_VIRTUAL_ROUTER_ATTR_SRC_MAC_ADDRESS:
            memcpy(attr->value.mac, vr->src_mac, sizeof(vr->src_mac));
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
 * vtable
 * --------------------------------------------------------------------------- */
const sai_virtual_router_api_t sai_bcm_virtual_router_api = {
    .create_virtual_router        = create_virtual_router,
    .remove_virtual_router        = remove_virtual_router,
    .set_virtual_router_attribute = set_virtual_router_attribute,
    .get_virtual_router_attribute = get_virtual_router_attribute,
};