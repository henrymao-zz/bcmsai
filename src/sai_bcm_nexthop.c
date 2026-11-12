/*
 * SAI Next Hop API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 3 scope: IP next hops only (MPLS/tunnel/SID return
 * SAI_STATUS_NOT_SUPPORTED). A next hop is programmed as an L3 egress
 * object (bcm_l3_egress_create) with the destination encoded from its RIF
 * (vlan, port or trunk) and the next-hop IP.
 *
 * The egress object's forwarding MAC is resolved from the neighbor table:
 * if a neighbor for (RIF, IP) already exists at next-hop creation time its
 * MAC is used; otherwise the placeholder MAC is zero until the neighbor
 * shows up (P3 limitation: existing egress objects are not refreshed on
 * later neighbor creation).
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_l3.h"

#include <string.h>

static struct sai_bcm_nh g_nhs[SAI_BCM_MAX_NEXT_HOPS];

void sai_bcm_nh_refresh_mac(sai_object_id_t rif_oid,
                            const sai_ip_address_t *ip,
                            const sai_mac_t mac)
{
    sai_bcm_lock();

    for (uint32_t i = 0; i < SAI_BCM_MAX_NEXT_HOPS; i++) {
        struct sai_bcm_nh *nh = &g_nhs[i];
        if (!nh->used || nh->rif_oid != rif_oid) {
            continue;
        }
        if (nh->ip.addr_family != ip->addr_family) {
            continue;
        }
        if (ip->addr_family == SAI_IP_ADDR_FAMILY_IPV4) {
            if (nh->ip.addr.ip4 != ip->addr.ip4) {
                continue;
            }
        } else if (memcmp(nh->ip.addr.ip6, ip->addr.ip6,
                          sizeof(sai_ip6_t)) != 0) {
            continue;
        }

        /* Re-program the egress object in place with the resolved MAC. */
        struct sai_bcm_rif *rif = sai_bcm_rif_get(nh->rif_oid);
        if (rif == NULL) {
            continue;
        }

        bcm_vlan_t vid = 0;
        bcm_gport_t gport = 0;
        bool is_trunk = false;
        if (sai_bcm_rif_egress_params(rif, &vid, &gport, &is_trunk) < 0) {
            continue;
        }

        bcm_l3_egress_t egr;
        bcm_l3_egress_t_init(&egr);
        egr.intf = (rif->type == SAI_ROUTER_INTERFACE_TYPE_VLAN)
                       ? (bcm_if_t)vid : 0;
        egr.vlan = vid;
        if (is_trunk) {
            egr.flags |= BCM_L3_TGID;
            egr.trunk = BCM_GPORT_TRUNK_GET(gport);
        } else if (rif->type == SAI_ROUTER_INTERFACE_TYPE_PORT) {
            egr.port = BCM_GPORT_LOCAL_GET(gport);
        }
        memcpy(egr.mac_addr, mac, sizeof(egr.mac_addr));

        bcm_if_t if_id = nh->egress_if;
        int rv = bcm_l3_egress_create(g_sai_bcm_sw.unit, BCM_L3_REPLACE,
                                      &egr, &if_id);
        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_l3_egress_create(REPLACE) failed: %d (%s)", rv,
                        bcm_errmsg(rv));
        }
    }

    sai_bcm_unlock();
}

struct sai_bcm_nh *sai_bcm_nh_get(sai_object_id_t nh_oid)
{
    if (sai_bcm_oid_type(nh_oid) != SAI_OBJECT_TYPE_NEXT_HOP) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(nh_oid);
    if (idx >= SAI_BCM_MAX_NEXT_HOPS || !g_nhs[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_NEXT_HOP, idx) != nh_oid) {
        return NULL;
    }
    return &g_nhs[idx];
}

/* ---------------------------------------------------------------------------
 * SAI Next Hop API
 * --------------------------------------------------------------------------- */
static sai_status_t create_next_hop(sai_object_id_t *next_hop_id,
                                   sai_object_id_t switch_id,
                                   uint32_t attr_count,
                                   const sai_attribute_t *attr_list)
{
    sai_next_hop_type_t type = SAI_NEXT_HOP_TYPE_IP;
    sai_ip_address_t ip = {0};
    sai_object_id_t rif_oid = SAI_NULL_OBJECT_ID;

    (void)switch_id;

    if (next_hop_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_NEXT_HOP_ATTR_TYPE:
            type = (sai_next_hop_type_t)attr_list[i].value.s32;
            break;
        case SAI_NEXT_HOP_ATTR_IP:
            ip = attr_list[i].value.ipaddr;
            break;
        case SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID:
            rif_oid = attr_list[i].value.oid;
            break;
        default:
            break;
        }
    }

    if (type != SAI_NEXT_HOP_TYPE_IP) {
        return SAI_STATUS_NOT_SUPPORTED;
    }

    sai_bcm_lock();

    struct sai_bcm_rif *rif = sai_bcm_rif_get(rif_oid);
    if (rif == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    bcm_vlan_t vid = 0;
    bcm_gport_t gport = 0;
    bool is_trunk = false;
    int rv = sai_bcm_rif_egress_params(rif, &vid, &gport, &is_trunk);
    if (rv < 0) {
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    bcm_l3_egress_t egr;
    bcm_l3_egress_t_init(&egr);
    egr.intf = (rif->type == SAI_ROUTER_INTERFACE_TYPE_VLAN)
                   ? (bcm_if_t)vid : 0;
    egr.vlan = vid;
    if (is_trunk) {
        egr.flags |= BCM_L3_TGID;
        egr.trunk = BCM_GPORT_TRUNK_GET(gport);
    } else if (rif->type == SAI_ROUTER_INTERFACE_TYPE_PORT) {
        egr.port = BCM_GPORT_LOCAL_GET(gport);
    }
    /* Resolve the forwarding MAC from an existing neighbor, if any. */
    struct sai_bcm_neighbor *nb = sai_bcm_neighbor_find(rif_oid, &ip);
    if (nb != NULL) {
        memcpy(egr.mac_addr, nb->mac, sizeof(egr.mac_addr));
    }

    bcm_if_t egress_if = 0;
    rv = bcm_l3_egress_create(g_sai_bcm_sw.unit, 0, &egr, &egress_if);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_l3_egress_create failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_NEXT_HOP, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_NEXT_HOPS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_NEXT_HOP, idx);
        }
        (void)bcm_l3_egress_destroy(g_sai_bcm_sw.unit, egress_if);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_nhs[idx] = (struct sai_bcm_nh){
        .used = true,
        .rif_oid = rif_oid,
        .ip = ip,
        .egress_if = egress_if,
    };
    *next_hop_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                      SAI_OBJECT_TYPE_NEXT_HOP, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_next_hop(sai_object_id_t next_hop_id)
{
    sai_bcm_lock();

    struct sai_bcm_nh *nh = sai_bcm_nh_get(next_hop_id);
    if (nh == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    int rv = bcm_l3_egress_destroy(g_sai_bcm_sw.unit, nh->egress_if);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_l3_egress_destroy failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint64_t idx = sai_bcm_oid_index(next_hop_id);
    memset(nh, 0, sizeof(*nh));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_NEXT_HOP, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_next_hop_attribute(sai_object_id_t next_hop_id,
                                          const sai_attribute_t *attr)
{
    (void)next_hop_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_next_hop_attribute(sai_object_id_t next_hop_id,
                                         uint32_t attr_count,
                                         sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_nh *nh = sai_bcm_nh_get(next_hop_id);
    if (nh == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_NEXT_HOP_ATTR_TYPE:
            attr->value.s32 = SAI_NEXT_HOP_TYPE_IP;
            break;
        case SAI_NEXT_HOP_ATTR_IP:
            attr->value.ipaddr = nh->ip;
            break;
        case SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID:
            attr->value.oid = nh->rif_oid;
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
const sai_next_hop_api_t sai_bcm_next_hop_api = {
    .create_next_hop        = create_next_hop,
    .remove_next_hop        = remove_next_hop,
    .set_next_hop_attribute = set_next_hop_attribute,
    .get_next_hop_attribute = get_next_hop_attribute,
};