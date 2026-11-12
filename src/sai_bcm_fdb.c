/*
 * SAI FDB API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 2 scope: create/remove/set/get entries via bcm_l2_addr_add /
 * bcm_l2_addr_delete / bcm_l2_addr_get (1Q entries keyed by mac+vlan),
 * flush via bcm_l2_addr_delete_by_vlan / _by_port / _by_trunk.
 *
 * Bulk entry ops return SAI_STATUS_NOT_SUPPORTED for now.
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"

#include <string.h>

#define SAI_BCM_FDB_MAX SAI_BCM_MAX_FDB_ENTRIES

static struct sai_bcm_fdb g_fdb[SAI_BCM_FDB_MAX];

/* ---------------------------------------------------------------------------
 * State accessors
 * --------------------------------------------------------------------------- */
struct sai_bcm_fdb *sai_bcm_fdb_get(const sai_fdb_entry_t *fdb_entry)
{
    if (fdb_entry == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < SAI_BCM_FDB_MAX; i++) {
        if (!g_fdb[i].used) {
            continue;
        }
        if (g_fdb[i].bv_id != fdb_entry->bv_id ||
            memcmp(g_fdb[i].mac, fdb_entry->mac_address,
                   sizeof(sai_mac_t)) != 0) {
            continue;
        }
        return &g_fdb[i];
    }
    return NULL;
}

void sai_bcm_fdb_reset(void)
{
    memset(g_fdb, 0, sizeof(g_fdb));
}

/* Find the local slot index for an entry (used as its "oid index"). */
static struct sai_bcm_fdb *fdb_slot_alloc(uint32_t *idx)
{
    for (uint32_t i = 0; i < SAI_BCM_FDB_MAX; i++) {
        if (!g_fdb[i].used) {
            *idx = i;
            return &g_fdb[i];
        }
    }
    return NULL;
}

/* Resolve a bv_id (vlan oid or bridge oid) + bridge port oid to the BCM
 * l2addr destination. Returns 0 on success. */
static int fdb_resolve_dest(sai_object_id_t bridge_port_oid,
                             bcm_l2_addr_t *l2addr)
{
    bcm_gport_t gport = 0;
    struct sai_bcm_bridge_port *bp = sai_bcm_bridge_port_get(bridge_port_oid);
    if (bp == NULL) {
        return BCM_E_NOT_FOUND;
    }
    int rv = sai_bcm_port_or_lag_to_gport(bp->port_oid, &gport);
    if (rv < 0) {
        return rv;
    }

    if (BCM_GPORT_IS_TRUNK(gport)) {
        l2addr->tgid = BCM_GPORT_TRUNK_GET(gport);
        l2addr->flags |= BCM_L2_TRUNK_MEMBER;
    } else {
        l2addr->port = BCM_GPORT_LOCAL_GET(gport);
        l2addr->flags &= ~(uint32)BCM_L2_TRUNK_MEMBER;
    }
    return BCM_E_NONE;
}

/* ---------------------------------------------------------------------------
 * SAI FDB API
 * --------------------------------------------------------------------------- */
static sai_status_t create_fdb_entry(const sai_fdb_entry_t *fdb_entry,
                                    uint32_t attr_count,
                                    const sai_attribute_t *attr_list)
{
    if (fdb_entry == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    /* Only 1Q entries (bv_id = VLAN oid) are supported in P2. */
    if (sai_bcm_oid_type(fdb_entry->bv_id) != SAI_OBJECT_TYPE_VLAN) {
        sai_bcm_unlock();
        return SAI_STATUS_NOT_SUPPORTED;
    }
    uint16_t vid = sai_bcm_vid_from_oid(fdb_entry->bv_id);
    if (vid < SAI_BCM_VLAN_MIN || vid > SAI_BCM_VLAN_MAX ||
        !g_sai_bcm_vlan_used[vid]) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    if (sai_bcm_fdb_get(fdb_entry) != NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_ALREADY_EXISTS;
    }

    sai_object_id_t bridge_port_oid = SAI_NULL_OBJECT_ID;
    sai_fdb_entry_type_t type = SAI_FDB_ENTRY_TYPE_DYNAMIC;
    sai_packet_action_t action = SAI_PACKET_ACTION_FORWARD;

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID:
            bridge_port_oid = attr_list[i].value.oid;
            break;
        case SAI_FDB_ENTRY_ATTR_TYPE:
            type = (sai_fdb_entry_type_t)attr_list[i].value.s32;
            break;
        case SAI_FDB_ENTRY_ATTR_PACKET_ACTION:
            action = (sai_packet_action_t)attr_list[i].value.s32;
            break;
        default:
            break;
        }
    }

    uint32_t slot;
    struct sai_bcm_fdb *ent = fdb_slot_alloc(&slot);
    if (ent == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_TABLE_FULL;
    }

    bcm_l2_addr_t l2addr;
    bcm_l2_addr_t_init(&l2addr, fdb_entry->mac_address, (bcm_vlan_t)vid);

    int rv = fdb_resolve_dest(bridge_port_oid, &l2addr);
    if (rv < 0) {
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    if (type == SAI_FDB_ENTRY_TYPE_STATIC) {
        l2addr.flags |= BCM_L2_STATIC;
    }
    if (action == SAI_PACKET_ACTION_DROP) {
        l2addr.flags |= BCM_L2_DISCARD_DST;
    }

    rv = bcm_l2_addr_add(g_sai_bcm_sw.unit, &l2addr);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_l2_addr_add failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    *ent = (struct sai_bcm_fdb){
        .used = true,
        .bv_id = fdb_entry->bv_id,
        .bridge_port_oid = bridge_port_oid,
        .type = type,
    };
    memcpy(ent->mac, fdb_entry->mac_address, sizeof(sai_mac_t));

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_fdb_entry(const sai_fdb_entry_t *fdb_entry)
{
    if (fdb_entry == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_fdb *ent = sai_bcm_fdb_get(fdb_entry);
    if (ent == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    uint16_t vid = sai_bcm_vid_from_oid(fdb_entry->bv_id);
    int rv = bcm_l2_addr_delete(g_sai_bcm_sw.unit, fdb_entry->mac_address,
                                (bcm_vlan_t)vid);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_l2_addr_delete failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    memset(ent, 0, sizeof(*ent));

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_fdb_entry_attribute(const sai_fdb_entry_t *fdb_entry,
                                           const sai_attribute_t *attr)
{
    if (fdb_entry == NULL || attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_fdb *ent = sai_bcm_fdb_get(fdb_entry);
    if (ent == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    uint16_t vid = sai_bcm_vid_from_oid(fdb_entry->bv_id);
    sai_status_t st = SAI_STATUS_NOT_SUPPORTED;

    switch (attr->id) {
    case SAI_FDB_ENTRY_ATTR_TYPE:
    case SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID: {
        bcm_l2_addr_t l2addr;
        bcm_l2_addr_t_init(&l2addr, fdb_entry->mac_address,
                          (bcm_vlan_t)vid);

        if (attr->id == SAI_FDB_ENTRY_ATTR_TYPE) {
            ent->type = (sai_fdb_entry_type_t)attr->value.s32;
            if (ent->type == SAI_FDB_ENTRY_TYPE_STATIC) {
                l2addr.flags |= BCM_L2_STATIC;
            }
        }
        st = SAI_STATUS_SUCCESS;
        if (attr->id == SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID) {
            int rv = fdb_resolve_dest(attr->value.oid, &l2addr);
            if (rv < 0) {
                sai_bcm_unlock();
                return sai_bcm_status(rv);
            }
            ent->bridge_port_oid = attr->value.oid;
        }
        if (ent->type == SAI_FDB_ENTRY_TYPE_STATIC) {
            l2addr.flags |= BCM_L2_STATIC;
        }

        /* Re-program the entry with the new attributes. */
        int rv = bcm_l2_addr_add(g_sai_bcm_sw.unit, &l2addr);
        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_l2_addr_add(re-set) failed: %d (%s)", rv,
                        bcm_errmsg(rv));
            sai_bcm_unlock();
            return sai_bcm_status(rv);
        }
        break;
    }
    default:
        break;
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_fdb_entry_attribute(const sai_fdb_entry_t *fdb_entry,
                                            uint32_t attr_count,
                                            sai_attribute_t *attr_list)
{
    if (fdb_entry == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_fdb *ent = sai_bcm_fdb_get(fdb_entry);
    if (ent == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    uint16_t vid = sai_bcm_vid_from_oid(fdb_entry->bv_id);
    sai_status_t out = SAI_STATUS_SUCCESS;

    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_FDB_ENTRY_ATTR_TYPE: {
            /* Hardware is authoritative: read back the entry. */
            bcm_l2_addr_t l2addr;
            int rv = bcm_l2_addr_get(g_sai_bcm_sw.unit, fdb_entry->mac_address,
                                     (bcm_vlan_t)vid, &l2addr);
            if (rv < 0) {
                out = sai_bcm_status(rv);
                break;
            }
            attr->value.s32 = (l2addr.flags & BCM_L2_STATIC)
                                  ? SAI_FDB_ENTRY_TYPE_STATIC
                                  : SAI_FDB_ENTRY_TYPE_DYNAMIC;
            break;
        }
        case SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID: {
            /* Hardware readback + reverse map gport -> bridge port oid. */
            bcm_l2_addr_t l2addr;
            int rv = bcm_l2_addr_get(g_sai_bcm_sw.unit, fdb_entry->mac_address,
                                     (bcm_vlan_t)vid, &l2addr);
            if (rv < 0) {
                out = sai_bcm_status(rv);
                break;
            }
            sai_object_id_t port_oid = SAI_NULL_OBJECT_ID;
            if (l2addr.flags & BCM_L2_TRUNK_MEMBER) {
                /* Find the LAG oid owning this trunk id. */
                for (uint32_t j = 0; j < SAI_BCM_MAX_LAGS; j++) {
                    sai_object_id_t cand = sai_bcm_oid_encode(
                        SAI_BCM_SWITCH_INSTANCE_FIRST, SAI_OBJECT_TYPE_LAG, j);
                    struct sai_bcm_lag *lag = sai_bcm_lag_get(cand);
                    if (lag != NULL && lag->trunk_id == l2addr.tgid) {
                        port_oid = cand;
                        break;
                    }
                }
            } else {
                port_oid = sai_bcm_switch_oid_from_bcm_port(
                    (bcm_port_t)l2addr.port);
            }
            /* Now find the bridge port on the default 1Q bridge for it. */
            sai_object_id_t found = SAI_NULL_OBJECT_ID;
            for (uint32_t j = 0; j < SAI_BCM_MAX_BRIDGE_PORTS; j++) {
                sai_object_id_t cand = sai_bcm_oid_encode(
                    SAI_BCM_SWITCH_INSTANCE_FIRST,
                    SAI_OBJECT_TYPE_BRIDGE_PORT, j);
                struct sai_bcm_bridge_port *bp = sai_bcm_bridge_port_get(cand);
                if (bp != NULL &&
                    bp->bridge_oid == g_sai_bcm_default_1q_bridge_oid &&
                    bp->port_oid == port_oid) {
                    found = cand;
                    break;
                }
            }
            if (found == SAI_NULL_OBJECT_ID) {
                found = ent->bridge_port_oid;
            }
            attr->value.oid = found;
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

static sai_status_t flush_fdb_entries(sai_object_id_t switch_id,
                                     uint32_t attr_count,
                                     const sai_attribute_t *attr_list)
{
    sai_object_id_t bv_id = SAI_NULL_OBJECT_ID;
    sai_object_id_t bridge_port_id = SAI_NULL_OBJECT_ID;
    sai_fdb_flush_entry_type_t type = SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC;

    (void)switch_id;

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_FDB_FLUSH_ATTR_BV_ID:
            bv_id = attr_list[i].value.oid;
            break;
        case SAI_FDB_FLUSH_ATTR_BRIDGE_PORT_ID:
            bridge_port_id = attr_list[i].value.oid;
            break;
        case SAI_FDB_FLUSH_ATTR_ENTRY_TYPE:
            type = (sai_fdb_flush_entry_type_t)attr_list[i].value.s32;
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

    int rv;
    uint32_t flags = (type == SAI_FDB_FLUSH_ENTRY_TYPE_ALL ||
                     type == SAI_FDB_FLUSH_ENTRY_TYPE_STATIC)
                        ? BCM_L2_DELETE_STATIC
                        : 0;

    if (bv_id != SAI_NULL_OBJECT_ID) {
        if (sai_bcm_oid_type(bv_id) != SAI_OBJECT_TYPE_VLAN) {
            sai_bcm_unlock();
            return SAI_STATUS_NOT_SUPPORTED;
        }
        uint16_t vid = sai_bcm_vid_from_oid(bv_id);
        rv = bcm_l2_addr_delete_by_vlan(g_sai_bcm_sw.unit, (bcm_vlan_t)vid,
                                        flags);
    } else if (bridge_port_id != SAI_NULL_OBJECT_ID) {
        bcm_gport_t gport = 0;
        rv = sai_bcm_bridge_port_to_gport(bridge_port_id, &gport);
        if (rv < 0) {
            sai_bcm_unlock();
            return sai_bcm_status(rv);
        }
        if (BCM_GPORT_IS_TRUNK(gport)) {
            rv = bcm_l2_addr_delete_by_trunk(
                g_sai_bcm_sw.unit, BCM_GPORT_TRUNK_GET(gport), flags);
        } else {
            rv = bcm_l2_addr_delete_by_port(
                g_sai_bcm_sw.unit, 0, BCM_GPORT_LOCAL_GET(gport), flags);
        }
    } else {
        /* Flush everything we track (per-vlan 1Q entries only in P2). */
        for (uint32_t vid = SAI_BCM_VLAN_MIN; vid <= SAI_BCM_VLAN_MAX;
             vid++) {
            if (g_sai_bcm_vlan_used[vid]) {
                (void)bcm_l2_addr_delete_by_vlan(
                    g_sai_bcm_sw.unit, (bcm_vlan_t)vid, flags);
            }
        }
        rv = BCM_E_NONE;
    }

    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "fdb flush failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    /* Drop matching local entries. */
    for (uint32_t i = 0; i < SAI_BCM_FDB_MAX; i++) {
        if (!g_fdb[i].used) {
            continue;
        }
        if (bv_id != SAI_NULL_OBJECT_ID && g_fdb[i].bv_id != bv_id) {
            continue;
        }
        if (type == SAI_FDB_FLUSH_ENTRY_TYPE_STATIC &&
            g_fdb[i].type != SAI_FDB_ENTRY_TYPE_STATIC) {
            continue;
        }
        if (type == SAI_FDB_FLUSH_ENTRY_TYPE_DYNAMIC &&
            g_fdb[i].type == SAI_FDB_ENTRY_TYPE_STATIC) {
            continue;
        }
        memset(&g_fdb[i], 0, sizeof(g_fdb[i]));
    }

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Not-yet-implemented members
 * --------------------------------------------------------------------------- */
static sai_status_t bulk_create(uint32_t n, const sai_fdb_entry_t *entries,
                               const uint32_t *attr_count,
                               const sai_attribute_t **attr_list,
                               sai_bulk_op_error_mode_t mode,
                               sai_status_t *statuses)
{
    if (n > 0 && (entries == NULL || attr_count == NULL || attr_list == NULL ||
                  statuses == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < n; i++) {
        sai_status_t st = create_fdb_entry(&entries[i], attr_count[i],
                                           attr_list[i]);
        statuses[i] = st;
        if (st != SAI_STATUS_SUCCESS &&
            mode == SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR) {
            sai_bcm_bulk_mark_not_executed(statuses, i + 1, n - i - 1);
            break;
        }
    }
    return SAI_STATUS_SUCCESS;
}

static sai_status_t bulk_remove(uint32_t n, const sai_fdb_entry_t *entries,
                               sai_bulk_op_error_mode_t mode,
                               sai_status_t *statuses)
{
    if (n > 0 && (entries == NULL || statuses == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < n; i++) {
        sai_status_t st = remove_fdb_entry(&entries[i]);
        statuses[i] = st;
        if (st != SAI_STATUS_SUCCESS &&
            mode == SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR) {
            sai_bcm_bulk_mark_not_executed(statuses, i + 1, n - i - 1);
            break;
        }
    }
    return SAI_STATUS_SUCCESS;
}

static sai_status_t bulk_set(uint32_t n, const sai_fdb_entry_t *entries,
                            const sai_attribute_t *attr_list,
                            sai_bulk_op_error_mode_t mode,
                            sai_status_t *statuses)
{
    if (n > 0 && (entries == NULL || attr_list == NULL || statuses == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < n; i++) {
        sai_status_t st = set_fdb_entry_attribute(&entries[i],
                                                  &attr_list[i]);
        statuses[i] = st;
        if (st != SAI_STATUS_SUCCESS &&
            mode == SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR) {
            sai_bcm_bulk_mark_not_executed(statuses, i + 1, n - i - 1);
            break;
        }
    }
    return SAI_STATUS_SUCCESS;
}

static sai_status_t bulk_get(uint32_t n, const sai_fdb_entry_t *entries,
                            const uint32_t *attr_count,
                            sai_attribute_t **attr_list,
                            sai_bulk_op_error_mode_t mode,
                            sai_status_t *statuses)
{
    if (n > 0 && (entries == NULL || attr_count == NULL || attr_list == NULL ||
                  statuses == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < n; i++) {
        sai_status_t st = get_fdb_entry_attribute(&entries[i], attr_count[i],
                                                 attr_list[i]);
        statuses[i] = st;
        if (st != SAI_STATUS_SUCCESS &&
            mode == SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR) {
            sai_bcm_bulk_mark_not_executed(statuses, i + 1, n - i - 1);
            break;
        }
    }
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_fdb_api_t sai_bcm_fdb_api = {
    .create_fdb_entry        = create_fdb_entry,
    .remove_fdb_entry        = remove_fdb_entry,
    .set_fdb_entry_attribute = set_fdb_entry_attribute,
    .get_fdb_entry_attribute = get_fdb_entry_attribute,
    .flush_fdb_entries      = flush_fdb_entries,
    .create_fdb_entries     = bulk_create,
    .remove_fdb_entries     = bulk_remove,
    .set_fdb_entries_attribute = bulk_set,
    .get_fdb_entries_attribute = bulk_get,
};