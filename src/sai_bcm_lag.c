/*
 * SAI LAG / LAG Member API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 2 scope: LAG groups via bcm_trunk_create/destroy, members via
 * bcm_trunk_member_add/delete (member gport = front-panel port), member
 * enumeration via SAI_LAG_ATTR_PORT_LIST.
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"

#include <string.h>

static struct sai_bcm_lag        g_lags[SAI_BCM_MAX_LAGS];
static struct sai_bcm_lag_member g_lag_members[SAI_BCM_MAX_LAG_MEMBERS];

/* ---------------------------------------------------------------------------
 * State accessors
 * --------------------------------------------------------------------------- */
struct sai_bcm_lag *sai_bcm_lag_get(sai_object_id_t lag_oid)
{
    if (sai_bcm_oid_type(lag_oid) != SAI_OBJECT_TYPE_LAG) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(lag_oid);
    if (idx >= SAI_BCM_MAX_LAGS || !g_lags[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_LAG, idx) != lag_oid) {
        return NULL;
    }
    return &g_lags[idx];
}

struct sai_bcm_lag_member *
sai_bcm_lag_member_get(sai_object_id_t member_oid)
{
    if (sai_bcm_oid_type(member_oid) != SAI_OBJECT_TYPE_LAG_MEMBER) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(member_oid);
    if (idx >= SAI_BCM_MAX_LAG_MEMBERS || !g_lag_members[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_LAG_MEMBER, idx) != member_oid) {
        return NULL;
    }
    return &g_lag_members[idx];
}

int sai_bcm_lag_to_gport(sai_object_id_t lag_oid, bcm_gport_t *gport)
{
    struct sai_bcm_lag *lag = sai_bcm_lag_get(lag_oid);
    if (lag == NULL || gport == NULL) {
        return BCM_E_NOT_FOUND;
    }
    BCM_GPORT_TRUNK_SET(*gport, lag->trunk_id);
    return BCM_E_NONE;
}

bool sai_bcm_is_port_or_lag(sai_object_id_t oid)
{
    return sai_bcm_switch_port_lookup(oid) != NULL ||
           sai_bcm_lag_get(oid) != NULL;
}

int sai_bcm_port_or_lag_to_gport(sai_object_id_t oid, bcm_gport_t *gport)
{
    if (gport == NULL) {
        return BCM_E_PARAM;
    }
    const struct sai_bcm_port_entry *pe = sai_bcm_switch_port_lookup(oid);
    if (pe != NULL) {
        BCM_GPORT_LOCAL_SET(*gport, pe->bcm_port);
        return BCM_E_NONE;
    }
    return sai_bcm_lag_to_gport(oid, gport);
}

/* ---------------------------------------------------------------------------
 * LAG API
 * --------------------------------------------------------------------------- */
static sai_status_t create_lag(sai_object_id_t *lag_id,
                              sai_object_id_t switch_id,
                              uint32_t attr_count,
                              const sai_attribute_t *attr_list)
{
    (void)switch_id; (void)attr_count; (void)attr_list;

    if (lag_id == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    bcm_trunk_t trunk_id = 0;
    int rv = bcm_trunk_create(g_sai_bcm_sw.unit, 0, &trunk_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_trunk_create failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_LAG, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_LAGS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_LAG, idx);
        }
        (void)bcm_trunk_destroy(g_sai_bcm_sw.unit, trunk_id);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_lags[idx] = (struct sai_bcm_lag){
        .used = true,
        .trunk_id = trunk_id,
    };
    *lag_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                 SAI_OBJECT_TYPE_LAG, idx);

    SAI_BCM_LOG(SAI_LOG_LEVEL_INFO, "lag created (oid=0x%lx trunk=%d)",
                (unsigned long)*lag_id, trunk_id);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_lag(sai_object_id_t lag_id)
{
    sai_bcm_lock();

    struct sai_bcm_lag *lag = sai_bcm_lag_get(lag_id);
    if (lag == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    /* Refuse while members still exist. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_LAG_MEMBERS; i++) {
        if (g_lag_members[i].used && g_lag_members[i].lag_oid == lag_id) {
            sai_bcm_unlock();
            return SAI_STATUS_OBJECT_IN_USE;
        }
    }

    int rv = bcm_trunk_destroy(g_sai_bcm_sw.unit, lag->trunk_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_trunk_destroy failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint64_t idx = sai_bcm_oid_index(lag_id);
    memset(lag, 0, sizeof(*lag));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_LAG, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_lag_attribute(sai_object_id_t lag_id,
                                     const sai_attribute_t *attr)
{
    (void)lag_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_lag_attribute(sai_object_id_t lag_id,
                                    uint32_t attr_count,
                                    sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    if (sai_bcm_lag_get(lag_id) == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_LAG_ATTR_PORT_LIST: {
            uint32_t n = 0;
            for (uint32_t j = 0; j < SAI_BCM_MAX_LAG_MEMBERS; j++) {
                if (g_lag_members[j].used &&
                    g_lag_members[j].lag_oid == lag_id) {
                    n++;
                }
            }
            if (attr->value.objlist.list == NULL ||
                attr->value.objlist.count < n) {
                attr->value.objlist.count = n;
                out = SAI_STATUS_BUFFER_OVERFLOW;
            } else {
                uint32_t k = 0;
                for (uint32_t j = 0; j < SAI_BCM_MAX_LAG_MEMBERS; j++) {
                    if (g_lag_members[j].used &&
                        g_lag_members[j].lag_oid == lag_id) {
                        attr->value.objlist.list[k++] =
                            g_lag_members[j].port_oid;
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
 * LAG Member API
 * --------------------------------------------------------------------------- */
static sai_status_t create_lag_member(sai_object_id_t *lag_member_id,
                                     sai_object_id_t switch_id,
                                     uint32_t attr_count,
                                     const sai_attribute_t *attr_list)
{
    sai_object_id_t lag_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t port_oid = SAI_NULL_OBJECT_ID;
    bool have_lag = false, have_port = false;

    (void)switch_id;

    if (lag_member_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_LAG_MEMBER_ATTR_LAG_ID:
            lag_oid = attr_list[i].value.oid;
            have_lag = true;
            break;
        case SAI_LAG_MEMBER_ATTR_PORT_ID:
            port_oid = attr_list[i].value.oid;
            have_port = true;
            break;
        default:
            break;
        }
    }
    if (!have_lag || !have_port) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_lag *lag = sai_bcm_lag_get(lag_oid);
    const struct sai_bcm_port_entry *pe = sai_bcm_switch_port_lookup(port_oid);
    if (lag == NULL || pe == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    /* Duplicate member check. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_LAG_MEMBERS; i++) {
        if (g_lag_members[i].used &&
            g_lag_members[i].lag_oid == lag_oid &&
            g_lag_members[i].port_oid == port_oid) {
            sai_bcm_unlock();
            return SAI_STATUS_ITEM_ALREADY_EXISTS;
        }
    }

    bcm_trunk_member_t member;
    bcm_trunk_member_t_init(&member);
    BCM_GPORT_LOCAL_SET(member.gport, pe->bcm_port);
    int rv = bcm_trunk_member_add(g_sai_bcm_sw.unit, lag->trunk_id, &member);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_trunk_member_add failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_LAG_MEMBER, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_LAG_MEMBERS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_LAG_MEMBER, idx);
        }
        (void)bcm_trunk_member_delete(g_sai_bcm_sw.unit, lag->trunk_id,
                                       &member);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_lag_members[idx] = (struct sai_bcm_lag_member){
        .used = true,
        .lag_oid = lag_oid,
        .port_oid = port_oid,
    };
    *lag_member_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                        SAI_OBJECT_TYPE_LAG_MEMBER, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_lag_member(sai_object_id_t lag_member_id)
{
    sai_bcm_lock();

    struct sai_bcm_lag_member *lm = sai_bcm_lag_member_get(lag_member_id);
    if (lm == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    struct sai_bcm_lag *lag = sai_bcm_lag_get(lm->lag_oid);
    const struct sai_bcm_port_entry *pe = sai_bcm_switch_port_lookup(lm->port_oid);
    if (lag == NULL || pe == NULL) {
        /* Inconsistent state; drop it locally anyway. */
        memset(lm, 0, sizeof(*lm));
        sai_bcm_idx_free(SAI_OBJECT_TYPE_LAG_MEMBER,
                         (uint32_t)sai_bcm_oid_index(lag_member_id));
        sai_bcm_unlock();
        return SAI_STATUS_FAILURE;
    }

    bcm_trunk_member_t member;
    bcm_trunk_member_t_init(&member);
    BCM_GPORT_LOCAL_SET(member.gport, pe->bcm_port);
    int rv = bcm_trunk_member_delete(g_sai_bcm_sw.unit, lag->trunk_id,
                                     &member);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_trunk_member_delete failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    memset(lm, 0, sizeof(*lm));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_LAG_MEMBER,
                     (uint32_t)sai_bcm_oid_index(lag_member_id));

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_lag_member_attribute(sai_object_id_t lag_member_id,
                                            const sai_attribute_t *attr)
{
    (void)lag_member_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_lag_member_attribute(sai_object_id_t lag_member_id,
                                            uint32_t attr_count,
                                            sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_lag_member *lm = sai_bcm_lag_member_get(lag_member_id);
    if (lm == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_LAG_MEMBER_ATTR_LAG_ID:
            attr->value.oid = lm->lag_oid;
            break;
        case SAI_LAG_MEMBER_ATTR_PORT_ID:
            attr->value.oid = lm->port_oid;
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
                                      object_statuses, create_lag_member);
}

static sai_status_t bulk_remove(uint32_t obj_count,
                               const sai_object_id_t *object_id,
                               sai_bulk_op_error_mode_t mode,
                               sai_status_t *object_statuses)
{
    return sai_bcm_bulk_object_remove(obj_count, object_id, mode,
                                     object_statuses, remove_lag_member);
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_lag_api_t sai_bcm_lag_api = {
    .create_lag                  = create_lag,
    .remove_lag                  = remove_lag,
    .set_lag_attribute           = set_lag_attribute,
    .get_lag_attribute           = get_lag_attribute,
    .create_lag_member           = create_lag_member,
    .remove_lag_member           = remove_lag_member,
    .set_lag_member_attribute   = set_lag_member_attribute,
    .get_lag_member_attribute   = get_lag_member_attribute,
    .create_lag_members          = bulk_create,
    .remove_lag_members          = bulk_remove,
};