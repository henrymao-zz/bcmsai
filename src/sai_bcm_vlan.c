/*
 * SAI VLAN / VLAN Member API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 2 scope: VLAN create/remove via bcm_vlan_create/destroy, member
 * add/remove via bcm_vlan_gport_add/delete (through bridge ports, per the
 * SAI model where vlan members reference bridge ports of the default 1Q
 * bridge), member list, tagging mode.
 *
 * PRIORITY_TAGGED members are programmed like UNTAGGED (BCM VLAN ports are
 * either tagged or untagged); flagged as a known P2 limitation.
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"

#include <string.h>

bool g_sai_bcm_vlan_used[SAI_BCM_VLAN_COUNT];

static struct sai_bcm_vlan_member g_vlan_members[SAI_BCM_MAX_VLAN_MEMBERS];

/* ---------------------------------------------------------------------------
 * State accessors
 * --------------------------------------------------------------------------- */
struct sai_bcm_vlan_member *
sai_bcm_vlan_member_get(sai_object_id_t member_oid)
{
    if (sai_bcm_oid_type(member_oid) != SAI_OBJECT_TYPE_VLAN_MEMBER) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(member_oid);
    if (idx >= SAI_BCM_MAX_VLAN_MEMBERS || !g_vlan_members[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_VLAN_MEMBER, idx) != member_oid) {
        return NULL;
    }
    return &g_vlan_members[idx];
}

/* ---------------------------------------------------------------------------
 * VLAN API
 * --------------------------------------------------------------------------- */
static sai_status_t create_vlan(sai_object_id_t *vlan_id,
                               sai_object_id_t switch_id,
                               uint32_t attr_count,
                               const sai_attribute_t *attr_list)
{
    uint32_t vid = 0;
    bool have_vid = false;

    (void)switch_id;

    if (vlan_id == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < attr_count; i++) {
        if (attr_list[i].id == SAI_VLAN_ATTR_VLAN_ID) {
            vid = attr_list[i].value.u16;
            have_vid = true;
        }
    }
    if (!have_vid || vid < SAI_BCM_VLAN_MIN || vid > SAI_BCM_VLAN_MAX) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }
    if (g_sai_bcm_vlan_used[vid]) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_ALREADY_EXISTS;
    }

    int rv = bcm_vlan_create(g_sai_bcm_sw.unit, (bcm_vlan_t)vid);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_vlan_create(%u) failed: %d (%s)",
                    vid, rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    g_sai_bcm_vlan_used[vid] = true;
    *vlan_id = sai_bcm_vlan_oid((uint16_t)vid);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_vlan(sai_object_id_t vlan_id)
{
    sai_bcm_lock();

    if (sai_bcm_oid_type(vlan_id) != SAI_OBJECT_TYPE_VLAN) {
        sai_bcm_unlock();
        return SAI_STATUS_INVALID_PARAMETER;
    }
    uint16_t vid = sai_bcm_vid_from_oid(vlan_id);
    if (vid < SAI_BCM_VLAN_MIN || vid > SAI_BCM_VLAN_MAX ||
        !g_sai_bcm_vlan_used[vid]) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    /* Refuse while members still exist. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_VLAN_MEMBERS; i++) {
        if (g_vlan_members[i].used &&
            sai_bcm_vid_from_oid(g_vlan_members[i].vlan_oid) == vid) {
            sai_bcm_unlock();
            return SAI_STATUS_OBJECT_IN_USE;
        }
    }

    int rv = bcm_vlan_destroy(g_sai_bcm_sw.unit, (bcm_vlan_t)vid);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_vlan_destroy(%u) failed: %d (%s)",
                    vid, rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    g_sai_bcm_vlan_used[vid] = false;

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_vlan_attribute(sai_object_id_t vlan_id,
                                     const sai_attribute_t *attr)
{
    (void)vlan_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_vlan_attribute(sai_object_id_t vlan_id,
                                      uint32_t attr_count,
                                      sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (sai_bcm_oid_type(vlan_id) != SAI_OBJECT_TYPE_VLAN) {
        sai_bcm_unlock();
        return SAI_STATUS_INVALID_PARAMETER;
    }
    uint16_t vid = sai_bcm_vid_from_oid(vlan_id);
    if (vid < SAI_BCM_VLAN_MIN || vid > SAI_BCM_VLAN_MAX ||
        !g_sai_bcm_vlan_used[vid]) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_VLAN_ATTR_VLAN_ID:
            attr->value.u16 = vid;
            break;
        case SAI_VLAN_ATTR_MEMBER_LIST: {
            uint32_t n = 0;
            for (uint32_t j = 0; j < SAI_BCM_MAX_VLAN_MEMBERS; j++) {
                if (g_vlan_members[j].used &&
                    sai_bcm_vid_from_oid(g_vlan_members[j].vlan_oid) == vid) {
                    n++;
                }
            }
            if (attr->value.objlist.list == NULL ||
                attr->value.objlist.count < n) {
                attr->value.objlist.count = n;
                out = SAI_STATUS_BUFFER_OVERFLOW;
            } else {
                uint32_t k = 0;
                for (uint32_t j = 0; j < SAI_BCM_MAX_VLAN_MEMBERS; j++) {
                    if (g_vlan_members[j].used &&
                        sai_bcm_vid_from_oid(g_vlan_members[j].vlan_oid) ==
                            vid) {
                        attr->value.objlist.list[k++] =
                            sai_bcm_oid_encode(
                                SAI_BCM_SWITCH_INSTANCE_FIRST,
                                SAI_OBJECT_TYPE_VLAN_MEMBER, j);
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
 * VLAN Member API
 * --------------------------------------------------------------------------- */
static sai_status_t create_vlan_member(sai_object_id_t *vlan_member_id,
                                      sai_object_id_t switch_id,
                                      uint32_t attr_count,
                                      const sai_attribute_t *attr_list)
{
    sai_object_id_t vlan_oid = SAI_NULL_OBJECT_ID;
    sai_object_id_t bridge_port_oid = SAI_NULL_OBJECT_ID;
    sai_vlan_tagging_mode_t tag_mode = SAI_VLAN_TAGGING_MODE_UNTAGGED;

    (void)switch_id;

    if (vlan_member_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_VLAN_MEMBER_ATTR_VLAN_ID:
            vlan_oid = attr_list[i].value.oid;
            break;
        case SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID:
            bridge_port_oid = attr_list[i].value.oid;
            break;
        case SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE:
            tag_mode = (sai_vlan_tagging_mode_t)attr_list[i].value.s32;
            break;
        default:
            break;
        }
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    /* VLAN must exist. */
    if (sai_bcm_oid_type(vlan_oid) != SAI_OBJECT_TYPE_VLAN ||
        !g_sai_bcm_vlan_used[sai_bcm_vid_from_oid(vlan_oid)]) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    /* Bridge port must exist and belong to the default 1Q bridge. */
    struct sai_bcm_bridge_port *bp =
        sai_bcm_bridge_port_get(bridge_port_oid);
    if (bp == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }
    if (bp->bridge_oid != g_sai_bcm_default_1q_bridge_oid) {
        sai_bcm_unlock();
        return SAI_STATUS_NOT_SUPPORTED;
    }

    /* Duplicate (vlan, bridge port) check. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_VLAN_MEMBERS; i++) {
        if (g_vlan_members[i].used &&
            g_vlan_members[i].vlan_oid == vlan_oid &&
            g_vlan_members[i].bridge_port_oid == bridge_port_oid) {
            sai_bcm_unlock();
            return SAI_STATUS_ITEM_ALREADY_EXISTS;
        }
    }

    bcm_gport_t gport = 0;
    int rv = sai_bcm_bridge_port_to_gport(bridge_port_oid, &gport);
    if (rv < 0) {
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    int flags = (tag_mode == SAI_VLAN_TAGGING_MODE_UNTAGGED ||
                 tag_mode == SAI_VLAN_TAGGING_MODE_PRIORITY_TAGGED)
                    ? BCM_VLAN_GPORT_ADD_UNTAGGED : 0;
    uint16_t vid = sai_bcm_vid_from_oid(vlan_oid);
    rv = bcm_vlan_gport_add(g_sai_bcm_sw.unit, (bcm_vlan_t)vid, gport, flags);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_vlan_gport_add(%u, gport) failed: %d (%s)",
                    vid, rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_VLAN_MEMBER, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_VLAN_MEMBERS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_VLAN_MEMBER, idx);
        }
        (void)bcm_vlan_gport_delete(g_sai_bcm_sw.unit, (bcm_vlan_t)vid, gport);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_vlan_members[idx] = (struct sai_bcm_vlan_member){
        .used = true,
        .vlan_oid = vlan_oid,
        .bridge_port_oid = bridge_port_oid,
        .tagging_mode = tag_mode,
    };
    *vlan_member_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                         SAI_OBJECT_TYPE_VLAN_MEMBER, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_vlan_member(sai_object_id_t vlan_member_id)
{
    sai_bcm_lock();

    struct sai_bcm_vlan_member *vm = sai_bcm_vlan_member_get(vlan_member_id);
    if (vm == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    uint16_t vid = sai_bcm_vid_from_oid(vm->vlan_oid);
    bcm_gport_t gport = 0;
    int rv = sai_bcm_bridge_port_to_gport(vm->bridge_port_oid, &gport);
    if (rv >= 0) {
        rv = bcm_vlan_gport_delete(g_sai_bcm_sw.unit, (bcm_vlan_t)vid, gport);
    }
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_vlan_gport_delete(%u, gport) failed: %d (%s)",
                    vid, rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint64_t idx = sai_bcm_oid_index(vlan_member_id);
    memset(vm, 0, sizeof(*vm));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_VLAN_MEMBER, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_vlan_member_attribute(sai_object_id_t vlan_member_id,
                                             const sai_attribute_t *attr)
{
    if (attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_vlan_member *vm = sai_bcm_vlan_member_get(vlan_member_id);
    if (vm == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_NOT_SUPPORTED;
    if (attr->id == SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE) {
        /* Re-program the hardware membership with the new tagging. */
        uint16_t vid = sai_bcm_vid_from_oid(vm->vlan_oid);
        bcm_gport_t gport = 0;
        int rv = sai_bcm_bridge_port_to_gport(vm->bridge_port_oid, &gport);
        if (rv < 0) {
            sai_bcm_unlock();
            return sai_bcm_status(rv);
        }
        sai_vlan_tagging_mode_t mode =
            (sai_vlan_tagging_mode_t)attr->value.s32;
        int flags = (mode == SAI_VLAN_TAGGING_MODE_UNTAGGED ||
                     mode == SAI_VLAN_TAGGING_MODE_PRIORITY_TAGGED)
                        ? BCM_VLAN_GPORT_ADD_UNTAGGED : 0;
        rv = bcm_vlan_gport_add(g_sai_bcm_sw.unit, (bcm_vlan_t)vid, gport,
                                flags);
        if (rv >= 0) {
            vm->tagging_mode = mode;
        }
        st = (rv >= 0) ? SAI_STATUS_SUCCESS : sai_bcm_status(rv);
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_vlan_member_attribute(sai_object_id_t vlan_member_id,
                                             uint32_t attr_count,
                                             sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_vlan_member *vm = sai_bcm_vlan_member_get(vlan_member_id);
    if (vm == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_VLAN_MEMBER_ATTR_VLAN_ID:
            attr->value.oid = vm->vlan_oid;
            break;
        case SAI_VLAN_MEMBER_ATTR_BRIDGE_PORT_ID:
            attr->value.oid = vm->bridge_port_oid;
            break;
        case SAI_VLAN_MEMBER_ATTR_VLAN_TAGGING_MODE:
            attr->value.s32 = vm->tagging_mode;
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
                                      object_statuses, create_vlan_member);
}

static sai_status_t bulk_remove(uint32_t obj_count,
                               const sai_object_id_t *object_id,
                               sai_bulk_op_error_mode_t mode,
                               sai_status_t *object_statuses)
{
    return sai_bcm_bulk_object_remove(obj_count, object_id, mode,
                                     object_statuses, remove_vlan_member);
}

static sai_status_t vlan_stats(sai_object_id_t vlan_id, uint32_t n,
                               const sai_stat_id_t *ids, uint64_t *vals)
{
    (void)vlan_id; (void)n; (void)ids; (void)vals;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t vlan_stats_ext(sai_object_id_t vlan_id, uint32_t n,
                                  const sai_stat_id_t *ids,
                                  sai_stats_mode_t mode, uint64_t *vals)
{
    (void)vlan_id; (void)n; (void)ids; (void)mode; (void)vals;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t vlan_stats_clear(sai_object_id_t vlan_id, uint32_t n,
                                    const sai_stat_id_t *ids)
{
    (void)vlan_id; (void)n; (void)ids;
    return SAI_STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_vlan_api_t sai_bcm_vlan_api = {
    .create_vlan             = create_vlan,
    .remove_vlan             = remove_vlan,
    .set_vlan_attribute      = set_vlan_attribute,
    .get_vlan_attribute      = get_vlan_attribute,
    .create_vlan_member      = create_vlan_member,
    .remove_vlan_member      = remove_vlan_member,
    .set_vlan_member_attribute = set_vlan_member_attribute,
    .get_vlan_member_attribute = get_vlan_member_attribute,
    .create_vlan_members     = bulk_create,
    .remove_vlan_members     = bulk_remove,
    .get_vlan_stats          = vlan_stats,
    .get_vlan_stats_ext      = vlan_stats_ext,
    .clear_vlan_stats        = vlan_stats_clear,
};