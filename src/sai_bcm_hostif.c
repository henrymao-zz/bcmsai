/*
 * SAI Host Interface API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 4 scope:
 *  - trap groups: SAI objects carrying the CPU queue selection
 *    (SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE); the per-reason CPU cosq
 *    programming (bcm_rx_cosq_mapping_set) arrives with the trap config
 *  - traps: hardware rx traps via bcm_rx_trap_type_create + bcm_rx_trap_set
 *    with a curated SAI -> BCM trap type map; well-known protocol traps
 *    use their named BCM counterparts, the rest fall back to
 *    bcmRxTrapUserDefine (protocol binding via bcm_rx_trap_protocol_set
 *    is P5 work)
 *  - hostifs: KNET netdevs via bcm_knet_netif_create/destroy
 *    (SAI_HOSTIF_TYPE_NETDEV only)
 *  - hostif table entries, user-defined traps and packet alloc/send/recv
 *    return SAI_STATUS_NOT_SUPPORTED (packet I/O needs the KNET kernel
 *    interface and is out of scope for the mock-validated phase)
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_hostif.h"

#include <stdio.h>
#include <string.h>

static struct sai_bcm_trap_group g_trap_groups[SAI_BCM_MAX_TRAP_GROUPS];
static struct sai_bcm_trap      g_traps[SAI_BCM_MAX_TRAPS];
static struct sai_bcm_hostif    g_hostifs[SAI_BCM_MAX_HOSTIFS];

/* ---------------------------------------------------------------------------
 * State accessors
 * --------------------------------------------------------------------------- */
struct sai_bcm_trap_group *sai_bcm_trap_group_get(sai_object_id_t oid)
{
    if (sai_bcm_oid_type(oid) != SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(oid);
    if (idx >= SAI_BCM_MAX_TRAP_GROUPS || !g_trap_groups[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, idx) != oid) {
        return NULL;
    }
    return &g_trap_groups[idx];
}

struct sai_bcm_trap *sai_bcm_trap_get(sai_object_id_t oid)
{
    if (sai_bcm_oid_type(oid) != SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP) {
        /* traps are SAI_OBJECT_TYPE_HOSTIF_TRAP */
    }
    if (sai_bcm_oid_type(oid) != SAI_OBJECT_TYPE_HOSTIF_TRAP) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(oid);
    if (idx >= SAI_BCM_MAX_TRAPS || !g_traps[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_HOSTIF_TRAP, idx) != oid) {
        return NULL;
    }
    return &g_traps[idx];
}

struct sai_bcm_hostif *sai_bcm_hostif_get(sai_object_id_t oid)
{
    if (sai_bcm_oid_type(oid) != SAI_OBJECT_TYPE_HOSTIF) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(oid);
    if (idx >= SAI_BCM_MAX_HOSTIFS || !g_hostifs[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_HOSTIF, idx) != oid) {
        return NULL;
    }
    return &g_hostifs[idx];
}

/* ---------------------------------------------------------------------------
 * SAI trap type -> BCM rx trap mapping (curated)
 * --------------------------------------------------------------------------- */
struct trap_map_entry {
    sai_hostif_trap_type_t sai;
    bcm_rx_trap_t bcm;
};

static const struct trap_map_entry k_trap_map[] = {
    { SAI_HOSTIF_TRAP_TYPE_ARP_REQUEST,         bcmRxTrapArp },
    { SAI_HOSTIF_TRAP_TYPE_ARP_RESPONSE,        bcmRxTrapArpReply },
    { SAI_HOSTIF_TRAP_TYPE_DHCP,                bcmRxTrapDhcpv4Server },
    { SAI_HOSTIF_TRAP_TYPE_DHCPV6,              bcmRxTrapDhcpv6Server },
    { SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_QUERY,     bcmRxTrapIgmpMembershipQuery },
    { SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_LEAVE,     bcmRxTrapIgmpReportLeaveMsg },
    { SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V1_REPORT, bcmRxTrapIgmpReportLeaveMsg },
    { SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V2_REPORT, bcmRxTrapIgmpReportLeaveMsg },
    { SAI_HOSTIF_TRAP_TYPE_IGMP_TYPE_V3_REPORT, bcmRxTrapIgmpUndefined },
};

bool sai_bcm_trap_type_map(sai_hostif_trap_type_t sai_type,
                          bcm_rx_trap_t *bcm_type)
{
    for (size_t i = 0; i < sizeof(k_trap_map) / sizeof(k_trap_map[0]); i++) {
        if (k_trap_map[i].sai == sai_type) {
            *bcm_type = k_trap_map[i].bcm;
            return true;
        }
    }
    /* No named counterpart: a user-defined trap carries the config; the
     * protocol binding (bcm_rx_trap_protocol_set) is P5 work. */
    *bcm_type = bcmRxTrapUserDefine;
    return true;
}

/* ---------------------------------------------------------------------------
 * Trap groups
 * --------------------------------------------------------------------------- */
static sai_status_t create_hostif_trap_group(
        sai_object_id_t *hostif_trap_group_id, sai_object_id_t switch_id,
        uint32_t attr_count, const sai_attribute_t *attr_list)
{
    (void)switch_id;

    if (hostif_trap_group_id == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP,
                                       &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_TRAP_GROUPS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, idx);
        }
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_trap_groups[idx].used = true;
    g_trap_groups[idx].cpu_queue = 0;
    for (uint32_t i = 0; i < attr_count; i++) {
        if (attr_list[i].id == SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE) {
            g_trap_groups[idx].cpu_queue = attr_list[i].value.u32;
        }
    }
    *hostif_trap_group_id = sai_bcm_oid_encode(
        SAI_BCM_SWITCH_INSTANCE_FIRST, SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_hostif_trap_group(sai_object_id_t oid)
{
    sai_bcm_lock();

    struct sai_bcm_trap_group *tg = sai_bcm_trap_group_get(oid);
    if (tg == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }
    for (uint32_t i = 0; i < SAI_BCM_MAX_TRAPS; i++) {
        if (g_traps[i].used && g_traps[i].group_oid == oid) {
            sai_bcm_unlock();
            return SAI_STATUS_OBJECT_IN_USE;
        }
    }

    uint64_t idx = sai_bcm_oid_index(oid);
    memset(tg, 0, sizeof(*tg));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_hostif_trap_group_attribute(
        sai_object_id_t oid, const sai_attribute_t *attr)
{
    if (attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_trap_group *tg = sai_bcm_trap_group_get(oid);
    if (tg == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_NOT_SUPPORTED;
    if (attr->id == SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE) {
        tg->cpu_queue = attr->value.u32;
        st = SAI_STATUS_SUCCESS;
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_hostif_trap_group_attribute(
        sai_object_id_t oid, uint32_t attr_count, sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_trap_group *tg = sai_bcm_trap_group_get(oid);
    if (tg == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE:
            attr->value.u32 = tg->cpu_queue;
            break;
        case SAI_HOSTIF_TRAP_GROUP_ATTR_ADMIN_STATE:
            attr->value.booldata = true;
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
 * Traps
 * --------------------------------------------------------------------------- */
static sai_status_t create_hostif_trap(sai_object_id_t *hostif_trap_id,
                                      sai_object_id_t switch_id,
                                      uint32_t attr_count,
                                      const sai_attribute_t *attr_list)
{
    sai_hostif_trap_type_t type = SAI_HOSTIF_TRAP_TYPE_UNKNOWN_L3_MULTICAST;
    bool have_type = false;
    sai_object_id_t group_oid = SAI_NULL_OBJECT_ID;
    sai_packet_action_t action = SAI_PACKET_ACTION_TRAP;

    (void)switch_id;

    if (hostif_trap_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE:
            type = (sai_hostif_trap_type_t)attr_list[i].value.s32;
            have_type = true;
            break;
        case SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP:
            group_oid = attr_list[i].value.oid;
            break;
        case SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION:
            action = (sai_packet_action_t)attr_list[i].value.s32;
            break;
        default:
            break;
        }
    }
    if (!have_type) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    /* Same (type, group) may appear once. */
    for (uint32_t i = 0; i < SAI_BCM_MAX_TRAPS; i++) {
        if (g_traps[i].used && g_traps[i].trap_type == type &&
            g_traps[i].group_oid == group_oid) {
            sai_bcm_unlock();
            return SAI_STATUS_ITEM_ALREADY_EXISTS;
        }
    }

    bcm_rx_trap_t bcm_type;
    (void)sai_bcm_trap_type_map(type, &bcm_type);

    int trap_id = 0;
    int rv = bcm_rx_trap_type_create(g_sai_bcm_sw.unit, 0, bcm_type, &trap_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_rx_trap_type_create failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    bcm_rx_trap_config_t config;
    bcm_rx_trap_config_t_init(&config);
    config.trap_strength = 7;
    if (action == SAI_PACKET_ACTION_TRAP ||
        action == SAI_PACKET_ACTION_LOG) {
        config.flags |= BCM_RX_TRAP_TRAP;
    } else if (action == SAI_PACKET_ACTION_COPY) {
        config.snoop_strength = 7;
        config.flags |= BCM_RX_TRAP_TRAP;
    }
    /* DROP: no trap flag -> the packet is not claimed by the CPU. */

    rv = bcm_rx_trap_set(g_sai_bcm_sw.unit, trap_id, &config);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_rx_trap_set failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        (void)bcm_rx_trap_type_destroy(g_sai_bcm_sw.unit, trap_id);
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_HOSTIF_TRAP, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_TRAPS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_HOSTIF_TRAP, idx);
        }
        (void)bcm_rx_trap_type_destroy(g_sai_bcm_sw.unit, trap_id);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_traps[idx] = (struct sai_bcm_trap){
        .used = true,
        .trap_type = type,
        .group_oid = group_oid,
        .action = action,
        .priority = 0,
        .bcm_trap_id = trap_id,
    };
    *hostif_trap_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                         SAI_OBJECT_TYPE_HOSTIF_TRAP, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_hostif_trap(sai_object_id_t hostif_trap_id)
{
    sai_bcm_lock();

    struct sai_bcm_trap *trap = sai_bcm_trap_get(hostif_trap_id);
    if (trap == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    int rv = bcm_rx_trap_type_destroy(g_sai_bcm_sw.unit, trap->bcm_trap_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_rx_trap_type_destroy failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint64_t idx = sai_bcm_oid_index(hostif_trap_id);
    memset(trap, 0, sizeof(*trap));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_HOSTIF_TRAP, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_hostif_trap_attribute(sai_object_id_t hostif_trap_id,
                                             const sai_attribute_t *attr)
{
    if (attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_trap *trap = sai_bcm_trap_get(hostif_trap_id);
    if (trap == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t st = SAI_STATUS_SUCCESS;
    switch (attr->id) {
    case SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION:
        trap->action = (sai_packet_action_t)attr->value.s32;
        break;
    case SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP:
        trap->group_oid = attr->value.oid;
        break;
    case SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY:
        trap->priority = attr->value.u32;
        break;
    default:
        st = SAI_STATUS_NOT_SUPPORTED;
        break;
    }

    sai_bcm_unlock();
    return st;
}

static sai_status_t get_hostif_trap_attribute(sai_object_id_t hostif_trap_id,
                                            uint32_t attr_count,
                                            sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_trap *trap = sai_bcm_trap_get(hostif_trap_id);
    if (trap == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_HOSTIF_TRAP_ATTR_TRAP_TYPE:
            attr->value.s32 = trap->trap_type;
            break;
        case SAI_HOSTIF_TRAP_ATTR_PACKET_ACTION:
            attr->value.s32 = trap->action;
            break;
        case SAI_HOSTIF_TRAP_ATTR_TRAP_GROUP:
            attr->value.oid = trap->group_oid;
            break;
        case SAI_HOSTIF_TRAP_ATTR_TRAP_PRIORITY:
            attr->value.u32 = trap->priority;
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
 * Hostifs (KNET netdevs)
 * --------------------------------------------------------------------------- */
static sai_status_t create_hostif(sai_object_id_t *hostif_id,
                                 sai_object_id_t switch_id,
                                 uint32_t attr_count,
                                 const sai_attribute_t *attr_list)
{
    sai_hostif_type_t type = SAI_HOSTIF_TYPE_NETDEV;
    sai_object_id_t obj_id = SAI_NULL_OBJECT_ID;
    const char *name = NULL;

    (void)switch_id;

    if (hostif_id == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        switch (attr_list[i].id) {
        case SAI_HOSTIF_ATTR_TYPE:
            type = (sai_hostif_type_t)attr_list[i].value.s32;
            break;
        case SAI_HOSTIF_ATTR_OBJ_ID:
            obj_id = attr_list[i].value.oid;
            break;
        case SAI_HOSTIF_ATTR_NAME:
            name = attr_list[i].value.chardata;
            break;
        default:
            break;
        }
    }

    if (type != SAI_HOSTIF_TYPE_NETDEV) {
        return SAI_STATUS_NOT_SUPPORTED;
    }
    if (!sai_bcm_is_port_or_lag(obj_id) &&
        sai_bcm_oid_type(obj_id) != SAI_OBJECT_TYPE_VLAN) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    bcm_knet_netif_t netif;
    bcm_knet_netif_t_init(&netif);
    netif.type = BCM_KNET_NETIF_T_TX_CPU_INGRESS;
    if (name != NULL) {
        snprintf(netif.name, sizeof(netif.name), "%s", name);
    }
    if (sai_bcm_oid_type(obj_id) == SAI_OBJECT_TYPE_VLAN) {
        netif.vlan = (bcm_vlan_t)sai_bcm_vid_from_oid(obj_id);
    } else {
        bcm_gport_t gport = 0;
        (void)sai_bcm_port_or_lag_to_gport(obj_id, &gport);
        if (BCM_GPORT_IS_TRUNK(gport)) {
            netif.flags |= 0;
        } else {
            netif.port = BCM_GPORT_LOCAL_GET(gport);
        }
    }

    int rv = bcm_knet_netif_create(g_sai_bcm_sw.unit, &netif);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_knet_netif_create failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_HOSTIF, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_HOSTIFS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_HOSTIF, idx);
        }
        (void)bcm_knet_netif_destroy(g_sai_bcm_sw.unit, netif.id);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_hostifs[idx].used = true;
    g_hostifs[idx].obj_id = obj_id;
    if (name != NULL) {
        snprintf(g_hostifs[idx].name, sizeof(g_hostifs[idx].name), "%s", name);
    }
    g_hostifs[idx].knet_netif_id = netif.id;
    *hostif_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                    SAI_OBJECT_TYPE_HOSTIF, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_hostif(sai_object_id_t hostif_id)
{
    sai_bcm_lock();

    struct sai_bcm_hostif *hif = sai_bcm_hostif_get(hostif_id);
    if (hif == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    int rv = bcm_knet_netif_destroy(g_sai_bcm_sw.unit, hif->knet_netif_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_knet_netif_destroy failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint64_t idx = sai_bcm_oid_index(hostif_id);
    memset(hif, 0, sizeof(*hif));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_HOSTIF, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_hostif_attribute(sai_object_id_t hostif_id,
                                        const sai_attribute_t *attr)
{
    (void)hostif_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_hostif_attribute(sai_object_id_t hostif_id,
                                        uint32_t attr_count,
                                        sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_hostif *hif = sai_bcm_hostif_get(hostif_id);
    if (hif == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_HOSTIF_ATTR_TYPE:
            attr->value.s32 = SAI_HOSTIF_TYPE_NETDEV;
            break;
        case SAI_HOSTIF_ATTR_OBJ_ID:
            attr->value.oid = hif->obj_id;
            break;
        case SAI_HOSTIF_ATTR_NAME:
            snprintf(attr->value.chardata, sizeof(attr->value.chardata), "%s",
                     hif->name);
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
static sai_status_t hostif_not_supported_create(
        sai_object_id_t *id, sai_object_id_t sw, uint32_t n,
        const sai_attribute_t *al)
{
    (void)id; (void)sw; (void)n; (void)al;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t hostif_not_supported_remove(sai_object_id_t id)
{
    (void)id;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t hostif_not_supported_set(sai_object_id_t id,
                                            const sai_attribute_t *a)
{
    (void)id; (void)a;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t hostif_not_supported_get(sai_object_id_t id, uint32_t n,
                                            sai_attribute_t *al)
{
    (void)id; (void)n; (void)al;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t hostif_not_supported_table_set(
        sai_object_id_t id, const sai_attribute_t *a)
{
    (void)id; (void)a;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t hostif_not_supported_table_get(
        sai_object_id_t id, uint32_t n, sai_attribute_t *al)
{
    (void)id; (void)n; (void)al;
    return SAI_STATUS_NOT_SUPPORTED;
}

/* sai_recv_hostif_packet_fn / sai_send_hostif_packet_fn */
static sai_status_t recv_hostif_packet(
        sai_object_id_t hostif_id, sai_size_t *buffer_size, void *buffer,
        uint32_t *attr_count, sai_attribute_t *attr_list)
{
    (void)hostif_id; (void)buffer_size; (void)buffer; (void)attr_count;
    (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t send_hostif_packet(
        sai_object_id_t hostif_id, sai_size_t buffer_size, const void *buffer,
        uint32_t attr_count, const sai_attribute_t *attr_list)
{
    (void)hostif_id; (void)buffer; (void)buffer_size; (void)attr_count;
    (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_hostif_api_t sai_bcm_hostif_api = {
    .create_hostif = create_hostif,
    .remove_hostif = remove_hostif,
    .set_hostif_attribute = set_hostif_attribute,
    .get_hostif_attribute = get_hostif_attribute,
    .create_hostif_table_entry = hostif_not_supported_create,
    .remove_hostif_table_entry = hostif_not_supported_remove,
    .set_hostif_table_entry_attribute = hostif_not_supported_table_set,
    .get_hostif_table_entry_attribute = hostif_not_supported_table_get,
    .create_hostif_trap_group = create_hostif_trap_group,
    .remove_hostif_trap_group = remove_hostif_trap_group,
    .set_hostif_trap_group_attribute = set_hostif_trap_group_attribute,
    .get_hostif_trap_group_attribute = get_hostif_trap_group_attribute,
    .create_hostif_trap = create_hostif_trap,
    .remove_hostif_trap = remove_hostif_trap,
    .set_hostif_trap_attribute = set_hostif_trap_attribute,
    .get_hostif_trap_attribute = get_hostif_trap_attribute,
    .create_hostif_user_defined_trap = hostif_not_supported_create,
    .remove_hostif_user_defined_trap = hostif_not_supported_remove,
    .set_hostif_user_defined_trap_attribute = hostif_not_supported_set,
    .get_hostif_user_defined_trap_attribute = hostif_not_supported_get,
    .recv_hostif_packet = recv_hostif_packet,
    .send_hostif_packet = send_hostif_packet,
};