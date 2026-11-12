/*
 * SAI Next Hop Group API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 3 scope: ECMP groups only (FINE_GRAINED/CLASS based types return
 * SAI_STATUS_NOT_SUPPORTED). The hardware ECMP object is created lazily
 * on the first member add (bcm_l3_egress_ecmp_create) and extended with
 * bcm_l3_egress_ecmp_add/delete afterwards.
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_l3.h"

#include <string.h>

static struct sai_bcm_nhg         g_nhgs[SAI_BCM_MAX_NH_GROUPS];
static struct sai_bcm_nhg_member g_nhg_members[SAI_BCM_MAX_NHG_MEMBERS];

struct sai_bcm_nhg *sai_bcm_nhg_get(sai_object_id_t nhg_oid)
{
    if (sai_bcm_oid_type(nhg_oid) != SAI_OBJECT_TYPE_NEXT_HOP_GROUP) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(nhg_oid);
    if (idx >= SAI_BCM_MAX_NH_GROUPS || !g_nhgs[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_NEXT_HOP_GROUP, idx) != nhg_oid) {
        return NULL;
    }
    return &g_nhgs[idx];
}

struct sai_bcm_nhg_member *
sai_bcm_nhg_member_get(sai_object_id_t member_oid)
{
    if (sai_bcm_oid_type(member_oid) != SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(member_oid);
    if (idx >= SAI_BCM_MAX_NHG_MEMBERS || !g_nhg_members[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, idx) !=
        member_oid) {
        return NULL;
    }
    return &g_nhg_members[idx];
}

/* Resolve NEXT_HOP or NEXT_HOP_GROUP oid to the l3 egress id used by
 * routes. Exported via sai_bcm_l3.h. */
int sai_bcm_nh_or_group_to_l3_intf(sai_object_id_t nh_oid, bcm_if_t *intf)
{
    if (intf == NULL) {
        return BCM_E_PARAM;
    }
    struct sai_bcm_nh *nh = sai_bcm_nh_get(nh_oid);
    if (nh != NULL) {
        *intf = nh->egress_if;
        return BCM_E_NONE;
    }
    struct sai_bcm_nhg *nhg = sai_bcm_nhg_get(nh_oid);
    if (nhg != NULL && nhg->hw_created) {
        *intf = nhg->ecmp_intf;
        return BCM_E_NONE;
    }
    return BCM_E_NOT_FOUND;
}

/* ---------------------------------------------------------------------------
 * SAI Next Hop Group API
 * --------------------------------------------------------------------------- */
static sai_status_t create_next_hop_group(sai_object_id_t *next_hop_group_id,
                                          sai_object_id_t switch_id,
                                          uint32_t attr_count,
                                          const sai_attribute_t *attr_list)
{
    sai_next_hop_group_type_t type = SAI_NEXT_HOP_GROUP_TYPE_ECMP;

    (void)switch_id;

    if (next_hop_group_id == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < attr_count; i++) {
        if (attr_list[i].id == SAI_NEXT_HOP_GROUP_ATTR_TYPE) {
            type = (sai_next_hop_group_type_t)attr_list[i].value.s32;
        }
    }
    if (type != SAI_NEXT_HOP_GROUP_TYPE_ECMP) {
        return SAI_STATUS_NOT_SUPPORTED;
    }

    sai_bcm_lock();

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_NH_GROUPS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, idx);
        }
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_nhgs[idx] = (struct sai_bcm_nhg){
        .used = true,
        .hw_created = false,
        .ecmp_intf = 0,
    };
    *next_hop_group_id = sai_bcm_oid_encode(
        SAI_BCM_SWITCH_INSTANCE_FIRST, SAI_OBJECT_TYPE_NEXT_HOP_GROUP, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_next_hop_group(sai_object_id_t next_hop_group_id)
{
    sai_bcm_lock();

    struct sai_bcm_nhg *nhg = sai_bcm_nhg_get(next_hop_group_id);
    if (nhg == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    /* Refuse while members still exist. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_NHG_MEMBERS; i++) {
        if (g_nhg_members[i].used &&
            g_nhg_members[i].nhg_oid == next_hop_group_id) {
            sai_bcm_unlock();
            return SAI_STATUS_OBJECT_IN_USE;
        }
    }

    int rv = BCM_E_NONE;
    if (nhg->hw_created) {
        bcm_l3_egress_ecmp_t ecmp;
        bcm_l3_egress_ecmp_t_init(&ecmp);
        ecmp.ecmp_intf = nhg->ecmp_intf;
        rv = bcm_l3_egress_ecmp_destroy(g_sai_bcm_sw.unit, &ecmp);
        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_l3_egress_ecmp_destroy failed: %d (%s)", rv,
                        bcm_errmsg(rv));
        }
    }

    uint64_t idx = sai_bcm_oid_index(next_hop_group_id);
    memset(nhg, 0, sizeof(*nhg));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, (uint32_t)idx);

    sai_bcm_unlock();
    return (rv < 0) ? sai_bcm_status(rv) : SAI_STATUS_SUCCESS;
}

static sai_status_t set_next_hop_group_attribute(
        sai_object_id_t next_hop_group_id, const sai_attribute_t *attr)
{
    (void)next_hop_group_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_next_hop_group_attribute(
        sai_object_id_t next_hop_group_id, uint32_t attr_count,
        sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_nhg *nhg = sai_bcm_nhg_get(next_hop_group_id);
    if (nhg == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_NEXT_HOP_GROUP_ATTR_TYPE:
            attr->value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP;
            break;
        case SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_COUNT: {
            uint32_t n = 0;
            for (uint32_t j = 0; j < SAI_BCM_MAX_NHG_MEMBERS; j++) {
                if (g_nhg_members[j].used &&
                    g_nhg_members[j].nhg_oid == next_hop_group_id) {
                    n++;
                }
            }
            attr->value.u32 = n;
            break;
        }
        case SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_MEMBER_LIST: {
            uint32_t n = 0;
            for (uint32_t j = 0; j < SAI_BCM_MAX_NHG_MEMBERS; j++) {
                if (g_nhg_members[j].used &&
                    g_nhg_members[j].nhg_oid == next_hop_group_id) {
                    n++;
                }
            }
            if (attr->value.objlist.list == NULL ||
                attr->value.objlist.count < n) {
                attr->value.objlist.count = n;
                out = SAI_STATUS_BUFFER_OVERFLOW;
            } else {
                uint32_t k = 0;
                for (uint32_t j = 0; j < SAI_BCM_MAX_NHG_MEMBERS; j++) {
                    if (g_nhg_members[j].used &&
                        g_nhg_members[j].nhg_oid == next_hop_group_id) {
                        attr->value.objlist.list[k++] =
                            sai_bcm_oid_encode(
                                SAI_BCM_SWITCH_INSTANCE_FIRST,
                                SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, j);
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
 * Group members
 * --------------------------------------------------------------------------- */
static sai_status_t create_next_hop_group_member(
        sai_object_id_t *next_hop_group_member_id, sai_object_id_t switch_id,
        uint32_t attr_count, const sai_attribute_t *attr_list)
{
    sai_object_id_t nhg_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t nh_oid = SAI_NULL_OBJECT_ID;

    (void)switch_id;

    if (next_hop_group_member_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID:
            nhg_oid = attr_list[i].value.oid;
            break;
        case SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID:
            nh_oid = attr_list[i].value.oid;
            break;
        default:
            break;
        }
    }

    sai_bcm_lock();

    struct sai_bcm_nhg *nhg = sai_bcm_nhg_get(nhg_oid);
    struct sai_bcm_nh *nh = sai_bcm_nh_get(nh_oid);
    if (nhg == NULL || nh == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    /* Duplicate member check. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_NHG_MEMBERS; i++) {
        if (g_nhg_members[i].used &&
            g_nhg_members[i].nhg_oid == nhg_oid &&
            g_nhg_members[i].nh_oid == nh_oid) {
            sai_bcm_unlock();
            return SAI_STATUS_ITEM_ALREADY_EXISTS;
        }
    }

    int rv;
    if (!nhg->hw_created) {
        /* First member: create the ECMP object with this member. */
        bcm_l3_egress_ecmp_t ecmp;
        bcm_l3_egress_ecmp_t_init(&ecmp);
        bcm_if_t members[1] = { nh->egress_if };
        rv = bcm_l3_egress_ecmp_create(g_sai_bcm_sw.unit, &ecmp, 1, members);
        if (rv >= 0) {
            nhg->hw_created = true;
            nhg->ecmp_intf = ecmp.ecmp_intf;
        }
    } else {
        bcm_l3_egress_ecmp_t ecmp;
        bcm_l3_egress_ecmp_t_init(&ecmp);
        ecmp.ecmp_intf = nhg->ecmp_intf;
        rv = bcm_l3_egress_ecmp_add(g_sai_bcm_sw.unit, &ecmp, nh->egress_if);
    }
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "ecmp add failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER,
                                       &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_NHG_MEMBERS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, idx);
        }
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_nhg_members[idx] = (struct sai_bcm_nhg_member){
        .used = true,
        .nhg_oid = nhg_oid,
        .nh_oid = nh_oid,
    };
    *next_hop_group_member_id = sai_bcm_oid_encode(
        SAI_BCM_SWITCH_INSTANCE_FIRST,
        SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_next_hop_group_member(
        sai_object_id_t next_hop_group_member_id)
{
    sai_bcm_lock();

    struct sai_bcm_nhg_member *m = sai_bcm_nhg_member_get(next_hop_group_member_id);
    if (m == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    struct sai_bcm_nhg *nhg = sai_bcm_nhg_get(m->nhg_oid);
    struct sai_bcm_nh *nh = sai_bcm_nh_get(m->nh_oid);

    int rv = BCM_E_NONE;
    if (nhg != NULL && nhg->hw_created && nh != NULL) {
        bcm_l3_egress_ecmp_t ecmp;
        bcm_l3_egress_ecmp_t_init(&ecmp);
        ecmp.ecmp_intf = nhg->ecmp_intf;
        rv = bcm_l3_egress_ecmp_delete(g_sai_bcm_sw.unit, &ecmp,
                                       nh->egress_if);
        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "ecmp delete failed: %d (%s)", rv, bcm_errmsg(rv));
        }
    }

    uint64_t idx = sai_bcm_oid_index(next_hop_group_member_id);
    memset(m, 0, sizeof(*m));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, (uint32_t)idx);

    sai_bcm_unlock();
    return (rv < 0) ? sai_bcm_status(rv) : SAI_STATUS_SUCCESS;
}

static sai_status_t set_next_hop_group_member_attribute(
        sai_object_id_t next_hop_group_member_id, const sai_attribute_t *attr)
{
    (void)next_hop_group_member_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_next_hop_group_member_attribute(
        sai_object_id_t next_hop_group_member_id, uint32_t attr_count,
        sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_nhg_member *m =
        sai_bcm_nhg_member_get(next_hop_group_member_id);
    if (m == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID:
            attr->value.oid = m->nhg_oid;
            break;
        case SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID:
            attr->value.oid = m->nh_oid;
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
static sai_status_t bulk_create(sai_object_id_t switch_id, uint32_t obj_count,
                                const uint32_t *attr_count,
                                const sai_attribute_t **attr_list,
                                sai_bulk_op_error_mode_t mode,
                                sai_object_id_t *object_id,
                                sai_status_t *object_statuses)
{
    return sai_bcm_bulk_object_create(switch_id, obj_count, attr_count,
                                      attr_list, mode, object_id,
                                      object_statuses,
                                      create_next_hop_group_member);
}

static sai_status_t bulk_remove(uint32_t obj_count,
                               const sai_object_id_t *object_id,
                               sai_bulk_op_error_mode_t mode,
                               sai_status_t *object_statuses)
{
    return sai_bcm_bulk_object_remove(obj_count, object_id, mode,
                                     object_statuses,
                                     remove_next_hop_group_member);
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_next_hop_group_api_t sai_bcm_next_hop_group_api = {
    .create_next_hop_group            = create_next_hop_group,
    .remove_next_hop_group            = remove_next_hop_group,
    .set_next_hop_group_attribute     = set_next_hop_group_attribute,
    .get_next_hop_group_attribute     = get_next_hop_group_attribute,
    .create_next_hop_group_member     = create_next_hop_group_member,
    .remove_next_hop_group_member     = remove_next_hop_group_member,
    .set_next_hop_group_member_attribute = set_next_hop_group_member_attribute,
    .get_next_hop_group_member_attribute = get_next_hop_group_member_attribute,
    .create_next_hop_group_members    = bulk_create,
    .remove_next_hop_group_members   = bulk_remove,
};