/*
 * SAI Route API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 3 scope: unicast IPv4/IPv6 prefix routes via bcm_l3_route_add /
 * bcm_l3_route_delete. The NEXT_HOP_ID attribute must be a NEXT_HOP or
 * NEXT_HOP_GROUP object (port / CPU targets return NOT_SUPPORTED).
 * Bulk entry ops return SAI_STATUS_NOT_SUPPORTED.
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_l3.h"

#include <string.h>

static struct sai_bcm_route g_routes[SAI_BCM_MAX_ROUTES];

/* Convert a sai_ip_prefix_t to bcm_l3_route_t subnet/mask fields. */
static void route_fill_prefix(const sai_ip_prefix_t *prefix,
                              bcm_l3_route_t *info)
{
    if (prefix->addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
        info->l3a_subnet = prefix->addr.ip4;
        info->l3a_ip_mask = prefix->mask.ip4;
    } else {
        info->l3a_flags |= BCM_L3_IP6;
        memcpy(info->l3a_ip6_net, prefix->addr.ip6,
               sizeof(info->l3a_ip6_net));
        memcpy(info->l3a_ip6_mask, prefix->mask.ip6,
               sizeof(info->l3a_ip6_mask));
    }
}

static struct sai_bcm_route *route_find(const sai_route_entry_t *route_entry)
{
    for (uint32_t i = 0; i < SAI_BCM_MAX_ROUTES; i++) {
        struct sai_bcm_route *r = &g_routes[i];
        if (!r->used || r->vr_oid != route_entry->vr_id) {
            continue;
        }
        if (r->prefix.addr_family != route_entry->destination.addr_family) {
            continue;
        }
        if (r->prefix.addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
            if (r->prefix.addr.ip4 == route_entry->destination.addr.ip4 &&
                r->prefix.mask.ip4 == route_entry->destination.mask.ip4) {
                return r;
            }
        } else {
            if (memcmp(r->prefix.addr.ip6, route_entry->destination.addr.ip6,
                       sizeof(sai_ip6_t)) == 0 &&
                memcmp(r->prefix.mask.ip6, route_entry->destination.mask.ip6,
                       sizeof(sai_ip6_t)) == 0) {
                return r;
            }
        }
    }
    return NULL;
}

static sai_status_t route_program(const sai_route_entry_t *route_entry,
                                 sai_object_id_t nh_oid)
{
    struct sai_bcm_vr *vr = sai_bcm_vr_get(route_entry->vr_id);
    if (vr == NULL) {
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    bcm_if_t intf = 0;
    int rv = sai_bcm_nh_or_group_to_l3_intf(nh_oid, &intf);
    if (rv < 0) {
        return sai_bcm_status(rv);
    }

    bcm_l3_route_t info;
    bcm_l3_route_t_init(&info);
    info.l3a_vrf = vr->vrf;
    info.l3a_intf = intf;
    route_fill_prefix(&route_entry->destination, &info);

    rv = bcm_l3_route_add(g_sai_bcm_sw.unit, &info);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_l3_route_add failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        return sai_bcm_status(rv);
    }
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * SAI Route API
 * --------------------------------------------------------------------------- */
static sai_status_t create_route_entry(const sai_route_entry_t *route_entry,
                                      uint32_t attr_count,
                                      const sai_attribute_t *attr_list)
{
    sai_object_id_t nh_oid = SAI_NULL_OBJECT_ID;

    if (route_entry == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        if (attr_list[i].id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID) {
            nh_oid = attr_list[i].value.oid;
        }
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }
    if (route_find(route_entry) != NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_ALREADY_EXISTS;
    }

    sai_status_t st = route_program(route_entry, nh_oid);
    if (st != SAI_STATUS_SUCCESS) {
        sai_bcm_unlock();
        return st;
    }

    struct sai_bcm_route *slot = NULL;
    for (uint32_t i = 0; i < SAI_BCM_MAX_ROUTES; i++) {
        if (!g_routes[i].used) {
            slot = &g_routes[i];
            break;
        }
    }
    if (slot == NULL) {
        /* Roll back the hardware entry. */
        struct sai_bcm_vr *vr = sai_bcm_vr_get(route_entry->vr_id);
        if (vr != NULL) {
            bcm_l3_route_t info;
            bcm_l3_route_t_init(&info);
            info.l3a_vrf = vr->vrf;
            route_fill_prefix(&route_entry->destination, &info);
            (void)bcm_l3_route_delete(g_sai_bcm_sw.unit, &info);
        }
        sai_bcm_unlock();
        return SAI_STATUS_TABLE_FULL;
    }

    slot->used = true;
    slot->vr_oid = route_entry->vr_id;
    slot->prefix = route_entry->destination;
    slot->nh_oid = nh_oid;

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_route_entry(const sai_route_entry_t *route_entry)
{
    if (route_entry == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_route *r = route_find(route_entry);
    if (r == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    struct sai_bcm_vr *vr = sai_bcm_vr_get(route_entry->vr_id);
    if (vr == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    bcm_l3_route_t info;
    bcm_l3_route_t_init(&info);
    info.l3a_vrf = vr->vrf;
    route_fill_prefix(&route_entry->destination, &info);

    int rv = bcm_l3_route_delete(g_sai_bcm_sw.unit, &info);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_l3_route_delete failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    memset(r, 0, sizeof(*r));

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_route_entry_attribute(const sai_route_entry_t *route_entry,
                                             const sai_attribute_t *attr)
{
    if (route_entry == NULL || attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_route *r = route_find(route_entry);
    if (r == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_NOT_SUPPORTED;
    if (attr->id == SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID) {
        st = route_program(route_entry, attr->value.oid);
        if (st == SAI_STATUS_SUCCESS) {
            r->nh_oid = attr->value.oid;
        }
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_route_entry_attribute(const sai_route_entry_t *route_entry,
                                             uint32_t attr_count,
                                             sai_attribute_t *attr_list)
{
    if (route_entry == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_route *r = route_find(route_entry);
    if (r == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID:
            attr->value.oid = r->nh_oid;
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
static sai_status_t bulk_create(uint32_t n, const sai_route_entry_t *entries,
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
        sai_status_t st = create_route_entry(&entries[i], attr_count[i],
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

static sai_status_t bulk_remove(uint32_t n, const sai_route_entry_t *entries,
                               sai_bulk_op_error_mode_t mode,
                               sai_status_t *statuses)
{
    if (n > 0 && (entries == NULL || statuses == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < n; i++) {
        sai_status_t st = remove_route_entry(&entries[i]);
        statuses[i] = st;
        if (st != SAI_STATUS_SUCCESS &&
            mode == SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR) {
            sai_bcm_bulk_mark_not_executed(statuses, i + 1, n - i - 1);
            break;
        }
    }
    return SAI_STATUS_SUCCESS;
}

static sai_status_t bulk_set(uint32_t n, const sai_route_entry_t *entries,
                            const sai_attribute_t *attr_list,
                            sai_bulk_op_error_mode_t mode,
                            sai_status_t *statuses)
{
    if (n > 0 && (entries == NULL || attr_list == NULL || statuses == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    for (uint32_t i = 0; i < n; i++) {
        sai_status_t st = set_route_entry_attribute(&entries[i],
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

static sai_status_t bulk_get(uint32_t n, const sai_route_entry_t *entries,
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
        sai_status_t st = get_route_entry_attribute(&entries[i],
                                                    attr_count[i],
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
const sai_route_api_t sai_bcm_route_api = {
    .create_route_entry        = create_route_entry,
    .remove_route_entry        = remove_route_entry,
    .set_route_entry_attribute = set_route_entry_attribute,
    .get_route_entry_attribute = get_route_entry_attribute,
    .create_route_entries     = bulk_create,
    .remove_route_entries     = bulk_remove,
    .set_route_entries_attribute = bulk_set,
    .get_route_entries_attribute = bulk_get,
};