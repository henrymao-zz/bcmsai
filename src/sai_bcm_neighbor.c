/*
 * SAI Neighbor API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 3 scope: IPv4/IPv6 neighbor entries via bcm_l3_host_add /
 * bcm_l3_host_delete (L3 host table), keyed by (RIF, IP). The RIF's
 * destination (vlan/port/trunk) and the neighbor MAC are programmed into
 * the host entry; remove_all walks the locally tracked entries.
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_l3.h"

#include <string.h>

static struct sai_bcm_neighbor g_neighbors[SAI_BCM_MAX_NEIGHBORS];

struct sai_bcm_neighbor *
sai_bcm_neighbor_find(sai_object_id_t rif_oid, const sai_ip_address_t *ip)
{
    if (ip == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < SAI_BCM_MAX_NEIGHBORS; i++) {
        struct sai_bcm_neighbor *nb = &g_neighbors[i];
        if (!nb->used || nb->rif_oid != rif_oid) {
            continue;
        }
        if (nb->ip.addr_family != ip->addr_family) {
            continue;
        }
        if (ip->addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
            if (nb->ip.addr.ip4 == ip->addr.ip4) {
                return nb;
            }
        } else {
            if (memcmp(nb->ip.addr.ip6, ip->addr.ip6,
                       sizeof(sai_ip6_t)) == 0) {
                return nb;
            }
        }
    }
    return NULL;
}

static void host_fill(const sai_ip_address_t *ip, bcm_l3_host_t *host)
{
    if (ip->addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
        host->l3a_ip_addr = ip->addr.ip4;
    } else {
        host->l3a_flags |= BCM_L3_IP6;
        memcpy(host->l3a_ip6_addr, ip->addr.ip6, sizeof(host->l3a_ip6_addr));
    }
}

/* ---------------------------------------------------------------------------
 * SAI Neighbor API
 * --------------------------------------------------------------------------- */
static sai_status_t create_neighbor_entry(const sai_neighbor_entry_t *neighbor_entry,
                                          uint32_t attr_count,
                                          const sai_attribute_t *attr_list)
{
    sai_mac_t mac = {0};

    if (neighbor_entry == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        if (attr_list[i].id == SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS) {
            memcpy(mac, attr_list[i].value.mac, sizeof(mac));
        }
    }

    sai_bcm_lock();

    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    struct sai_bcm_rif *rif = sai_bcm_rif_get(neighbor_entry->rif_id);
    if (rif == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }
    if (sai_bcm_neighbor_find(neighbor_entry->rif_id,
                              &neighbor_entry->ip_address) != NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_ALREADY_EXISTS;
    }

    struct sai_bcm_vr *vr = sai_bcm_vr_get(rif->vr_oid);
    if (vr == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    bcm_l3_host_t host;
    bcm_l3_host_t_init(&host);
    host.l3a_vrf = vr->vrf;
    host.l3a_intf = rif->egress_if;
    memcpy(host.l3a_nexthop_mac, mac, sizeof(host.l3a_nexthop_mac));
    host_fill(&neighbor_entry->ip_address, &host);

    int rv = bcm_l3_host_add(g_sai_bcm_sw.unit, &host);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_l3_host_add failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    struct sai_bcm_neighbor *slot = NULL;
    for (uint32_t i = 0; i < SAI_BCM_MAX_NEIGHBORS; i++) {
        if (!g_neighbors[i].used) {
            slot = &g_neighbors[i];
            break;
        }
    }
    if (slot == NULL) {
        bcm_l3_host_t rollback = host;
        (void)bcm_l3_host_delete(g_sai_bcm_sw.unit, &rollback);
        sai_bcm_unlock();
        return SAI_STATUS_TABLE_FULL;
    }

    slot->used = true;
    slot->rif_oid = neighbor_entry->rif_id;
    slot->ip = neighbor_entry->ip_address;
    memcpy(slot->mac, mac, sizeof(slot->mac));

    /* Refresh next-hop egress objects resolving through this neighbor. */
    sai_bcm_nh_refresh_mac(neighbor_entry->rif_id, &neighbor_entry->ip_address,
                           mac);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_neighbor_entry(const sai_neighbor_entry_t *neighbor_entry)
{
    if (neighbor_entry == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_neighbor *nb = sai_bcm_neighbor_find(
        neighbor_entry->rif_id, &neighbor_entry->ip_address);
    if (nb == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    struct sai_bcm_rif *rif = sai_bcm_rif_get(neighbor_entry->rif_id);
    struct sai_bcm_vr *vr = (rif != NULL) ? sai_bcm_vr_get(rif->vr_oid)
                                           : NULL;
    if (vr == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    bcm_l3_host_t host;
    bcm_l3_host_t_init(&host);
    host.l3a_vrf = vr->vrf;
    host_fill(&neighbor_entry->ip_address, &host);

    int rv = bcm_l3_host_delete(g_sai_bcm_sw.unit, &host);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_l3_host_delete failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    memset(nb, 0, sizeof(*nb));

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_neighbor_entry_attribute(
        const sai_neighbor_entry_t *neighbor_entry, const sai_attribute_t *attr)
{
    if (neighbor_entry == NULL || attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_neighbor *nb = sai_bcm_neighbor_find(
        neighbor_entry->rif_id, &neighbor_entry->ip_address);
    if (nb == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_NOT_SUPPORTED;
    if (attr->id == SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS) {
        /* Re-program the host entry with the new MAC. */
        struct sai_bcm_rif *rif = sai_bcm_rif_get(neighbor_entry->rif_id);
        struct sai_bcm_vr *vr =
            (rif != NULL) ? sai_bcm_vr_get(rif->vr_oid) : NULL;
        if (vr == NULL) {
            sai_bcm_unlock();
            return SAI_STATUS_ITEM_NOT_FOUND;
        }
        bcm_l3_host_t host;
        bcm_l3_host_t_init(&host);
        host.l3a_vrf = vr->vrf;
        host.l3a_intf = rif->egress_if;
        memcpy(host.l3a_nexthop_mac, attr->value.mac,
               sizeof(host.l3a_nexthop_mac));
        host_fill(&neighbor_entry->ip_address, &host);

        int rv = bcm_l3_host_add(g_sai_bcm_sw.unit, &host);
        if (rv >= 0) {
            memcpy(nb->mac, attr->value.mac, sizeof(nb->mac));
            sai_bcm_nh_refresh_mac(neighbor_entry->rif_id,
                                   &neighbor_entry->ip_address,
                                   nb->mac);
            st = SAI_STATUS_SUCCESS;
        } else {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_l3_host_add(re-set) failed: %d (%s)", rv,
                        bcm_errmsg(rv));
            st = sai_bcm_status(rv);
        }
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_neighbor_entry_attribute(
        const sai_neighbor_entry_t *neighbor_entry, uint32_t attr_count,
        sai_attribute_t *attr_list)
{
    if (neighbor_entry == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_neighbor *nb = sai_bcm_neighbor_find(
        neighbor_entry->rif_id, &neighbor_entry->ip_address);
    if (nb == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_NEIGHBOR_ENTRY_ATTR_DST_MAC_ADDRESS:
            memcpy(attr->value.mac, nb->mac, sizeof(nb->mac));
            break;
        default:
            out = SAI_STATUS_NOT_SUPPORTED;
            break;
        }
    }

    sai_bcm_unlock();
    return out;
}

static sai_status_t remove_all_neighbor_entries(sai_object_id_t switch_id)
{
    (void)switch_id;

    sai_bcm_lock();

    int rv = BCM_E_NONE;
    for (uint32_t i = 0; i < SAI_BCM_MAX_NEIGHBORS; i++) {
        if (!g_neighbors[i].used) {
            continue;
        }
        struct sai_bcm_rif *rif = sai_bcm_rif_get(g_neighbors[i].rif_oid);
        struct sai_bcm_vr *vr =
            (rif != NULL) ? sai_bcm_vr_get(rif->vr_oid) : NULL;
        if (vr == NULL) {
            continue;
        }
        bcm_l3_host_t host;
        bcm_l3_host_t_init(&host);
        host.l3a_vrf = vr->vrf;
        host_fill(&g_neighbors[i].ip, &host);
        int rc = bcm_l3_host_delete(g_sai_bcm_sw.unit, &host);
        if (rc < 0 && rv == BCM_E_NONE) {
            rv = rc;
        }
        memset(&g_neighbors[i], 0, sizeof(g_neighbors[i]));
    }

    sai_bcm_unlock();
    return (rv < 0) ? sai_bcm_status(rv) : SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_neighbor_api_t sai_bcm_neighbor_api = {
    .create_neighbor_entry        = create_neighbor_entry,
    .remove_neighbor_entry        = remove_neighbor_entry,
    .set_neighbor_entry_attribute = set_neighbor_entry_attribute,
    .get_neighbor_entry_attribute = get_neighbor_entry_attribute,
    .remove_all_neighbor_entries  = remove_all_neighbor_entries,
};