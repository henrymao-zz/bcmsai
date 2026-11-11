/*
 * SAI Port API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 1 scope: admin state, speed, MTU, autoneg, FEC, oper status and the
 * RFC-1213/1757 statistics subset backed by bcm_stat_*.
 */
#include "sai_bcm_internal.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * SAI stat id -> BCM stat value mapping
 * --------------------------------------------------------------------------- */
struct sai_bcm_stat_map {
    sai_stat_id_t sai_id;
    bcm_stat_val_t bcm_id;
};

static const struct sai_bcm_stat_map k_stat_map[] = {
    { SAI_PORT_STAT_IF_IN_OCTETS,            snmpIfInOctets },
    { SAI_PORT_STAT_IF_IN_UCAST_PKTS,         snmpIfInUcastPkts },
    { SAI_PORT_STAT_IF_IN_DISCARDS,           snmpIfInDiscards },
    { SAI_PORT_STAT_IF_IN_ERRORS,             snmpIfInErrors },
    { SAI_PORT_STAT_IF_IN_BROADCAST_PKTS,     snmpEtherStatsBroadcastPkts },
    { SAI_PORT_STAT_IF_IN_MULTICAST_PKTS,     snmpEtherStatsMulticastPkts },
    { SAI_PORT_STAT_IF_OUT_OCTETS,            snmpIfOutOctets },
    { SAI_PORT_STAT_IF_OUT_UCAST_PKTS,         snmpIfOutUcastPkts },
    { SAI_PORT_STAT_IF_OUT_DISCARDS,          snmpIfOutDiscards },
    { SAI_PORT_STAT_IF_OUT_ERRORS,            snmpIfOutErrors },
    { SAI_PORT_STAT_ETHER_STATS_DROP_EVENTS,     snmpEtherStatsDropEvents },
    { SAI_PORT_STAT_ETHER_STATS_PKTS,            snmpEtherStatsPkts },
    { SAI_PORT_STAT_ETHER_STATS_OCTETS,          snmpEtherStatsOctets },
    { SAI_PORT_STAT_ETHER_STATS_CRC_ALIGN_ERRORS,
      snmpEtherStatsCRCAlignErrors },
    { SAI_PORT_STAT_ETHER_STATS_UNDERSIZE_PKTS,  snmpEtherStatsUndersizePkts },
    { SAI_PORT_STAT_ETHER_STATS_OVERSIZE_PKTS,   snmpEtherStatsOversizePkts },
    { SAI_PORT_STAT_ETHER_STATS_FRAGMENTS,       snmpEtherStatsFragments },
    { SAI_PORT_STAT_ETHER_STATS_JABBERS,         snmpEtherStatsJabbers },
};

static bool stat_id_map(sai_stat_id_t sai_id, bcm_stat_val_t *bcm_id)
{
    for (size_t i = 0; i < sizeof(k_stat_map) / sizeof(k_stat_map[0]); i++) {
        if (k_stat_map[i].sai_id == sai_id) {
            *bcm_id = k_stat_map[i].bcm_id;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * FEC mapping (SAI <-> BCM)
 * --------------------------------------------------------------------------- */
static bool fec_map_sai_to_bcm(sai_port_fec_mode_t sai_fec,
                               bcm_port_phy_fec_t *bcm_fec)
{
    switch (sai_fec) {
    case SAI_PORT_FEC_MODE_NONE:
        *bcm_fec = bcmPortPhyFecNone;
        return true;
    case SAI_PORT_FEC_MODE_RS:
        *bcm_fec = bcmPortPhyFecRsFec;
        return true;
    case SAI_PORT_FEC_MODE_FC:
        *bcm_fec = bcmPortPhyFecBaseR;
        return true;
    default:
        return false;
    }
}

static bool fec_map_bcm_to_sai(bcm_port_phy_fec_t bcm_fec,
                               sai_port_fec_mode_t *sai_fec)
{
    switch (bcm_fec) {
    case bcmPortPhyFecNone:
        *sai_fec = SAI_PORT_FEC_MODE_NONE;
        return true;
    case bcmPortPhyFecRsFec:
    case bcmPortPhyFecRs544:
    case bcmPortPhyFecRs272:
        *sai_fec = SAI_PORT_FEC_MODE_RS;
        return true;
    case bcmPortPhyFecBaseR:
        *sai_fec = SAI_PORT_FEC_MODE_FC;
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * Port lookup helper
 * --------------------------------------------------------------------------- */
static sai_status_t port_lookup(sai_object_id_t port_id,
                               const struct sai_bcm_port_entry **out)
{
    const struct sai_bcm_port_entry *pe = sai_bcm_switch_port_lookup(port_id);
    if (pe == NULL) {
        return SAI_STATUS_ITEM_NOT_FOUND;
    }
    *out = pe;
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Stats read path (shared by get_port_stats and get_port_stats_ext)
 * --------------------------------------------------------------------------- */
static sai_status_t sai_bcm_port_stats_common(sai_object_id_t port_id,
                                             uint32_t number_of_counters,
                                             const sai_stat_id_t *counter_ids,
                                             uint64_t *counters,
                                             sai_stats_mode_t mode)
{
    const struct sai_bcm_port_entry *pe;
    sai_status_t st;

    if (number_of_counters > 0 &&
        (counter_ids == NULL || counters == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    st = port_lookup(port_id, &pe);
    if (st != SAI_STATUS_SUCCESS) {
        sai_bcm_unlock();
        return st;
    }

    int unit = g_sai_bcm_sw.unit;
    bcm_port_t port = pe->bcm_port;

    int rv = bcm_stat_sync(unit);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_stat_sync failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    for (uint32_t i = 0; i < number_of_counters; i++) {
        bcm_stat_val_t bcm_id;
        if (!stat_id_map(counter_ids[i], &bcm_id)) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_WARN,
                        "unsupported stat id 0x%x", counter_ids[i]);
            sai_bcm_unlock();
            return SAI_STATUS_NOT_SUPPORTED;
        }
        uint64 value = 0; /* OpenBCM uint64 */
        rv = bcm_stat_get(unit, port, bcm_id, &value);
        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_stat_get(0x%x) failed: %d (%s)",
                        counter_ids[i], rv, bcm_errmsg(rv));
            sai_bcm_unlock();
            return sai_bcm_status(rv);
        }
        counters[i] = (uint64_t)value;
    }

    if (mode == SAI_STATS_MODE_READ_AND_CLEAR) {
        rv = bcm_stat_clear(unit, port);
        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_stat_clear failed: %d (%s)", rv, bcm_errmsg(rv));
            sai_bcm_unlock();
            return sai_bcm_status(rv);
        }
    }

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * SAI Port API
 * --------------------------------------------------------------------------- */
static sai_status_t set_port_attribute(sai_object_id_t port_id,
                                      const sai_attribute_t *attr)
{
    const struct sai_bcm_port_entry *pe;
    sai_status_t st;

    if (attr == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    st = port_lookup(port_id, &pe);
    if (st != SAI_STATUS_SUCCESS) {
        sai_bcm_unlock();
        return st;
    }

    int unit = g_sai_bcm_sw.unit;
    bcm_port_t port = pe->bcm_port;
    int rv = BCM_E_NONE;

    switch (attr->id) {
    case SAI_PORT_ATTR_ADMIN_STATE:
        rv = bcm_port_enable_set(unit, port,
                                 attr->value.booldata ? 1 : 0);
        break;
    case SAI_PORT_ATTR_SPEED:
        rv = bcm_port_speed_set(unit, port, (int)attr->value.u32);
        break;
    case SAI_PORT_ATTR_MTU:
        rv = bcm_port_frame_max_set(unit, port, (int)attr->value.u32);
        break;
    case SAI_PORT_ATTR_AUTO_NEG_MODE:
        rv = bcm_port_autoneg_set(unit, port,
                                  attr->value.booldata ? 1 : 0);
        break;
    case SAI_PORT_ATTR_FEC_MODE: {
        bcm_port_phy_fec_t fec;
        if (!fec_map_sai_to_bcm(attr->value.s32, &fec)) {
            sai_bcm_unlock();
            return SAI_STATUS_INVALID_PARAMETER;
        }
        bcm_port_resource_t res;
        memset(&res, 0, sizeof(res));
        rv = bcm_port_resource_get(unit, port, &res);
        if (rv >= 0) {
            res.fec_type = fec;
            rv = bcm_port_resource_set(unit, port, &res);
        }
        break;
    }
    default:
        sai_bcm_unlock();
        return SAI_STATUS_NOT_SUPPORTED;
    }

    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "port set attr 0x%x failed: %d (%s)",
                    attr->id, rv, bcm_errmsg(rv));
    }
    sai_status_t mapped = sai_bcm_status(rv);
    sai_bcm_unlock();
    return mapped;
}

static sai_status_t get_port_attribute(sai_object_id_t port_id,
                                      uint32_t attr_count,
                                      sai_attribute_t *attr_list)
{
    const struct sai_bcm_port_entry *pe;
    sai_status_t st;

    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    st = port_lookup(port_id, &pe);
    if (st != SAI_STATUS_SUCCESS) {
        sai_bcm_unlock();
        return st;
    }

    int unit = g_sai_bcm_sw.unit;
    bcm_port_t port = pe->bcm_port;
    sai_status_t out = SAI_STATUS_SUCCESS;

    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        int rv = BCM_E_NONE;
        int ival = 0;

        switch (attr->id) {
        case SAI_PORT_ATTR_ADMIN_STATE:
            rv = bcm_port_enable_get(unit, port, &ival);
            attr->value.booldata = (ival != 0);
            break;
        case SAI_PORT_ATTR_SPEED:
            rv = bcm_port_speed_get(unit, port, &ival);
            attr->value.u32 = (uint32_t)ival;
            break;
        case SAI_PORT_ATTR_MTU:
            rv = bcm_port_frame_max_get(unit, port, &ival);
            attr->value.u32 = (uint32_t)ival;
            break;
        case SAI_PORT_ATTR_AUTO_NEG_MODE:
            rv = bcm_port_autoneg_get(unit, port, &ival);
            attr->value.booldata = (ival != 0);
            break;
        case SAI_PORT_ATTR_FEC_MODE: {
            bcm_port_resource_t res;
            memset(&res, 0, sizeof(res));
            rv = bcm_port_resource_get(unit, port, &res);
            if (rv >= 0) {
                sai_port_fec_mode_t sai_fec;
                if (fec_map_bcm_to_sai(res.fec_type, &sai_fec)) {
                    attr->value.s32 = sai_fec;
                } else {
                    attr->value.s32 = SAI_PORT_FEC_MODE_NONE;
                }
            }
            break;
        }
        case SAI_PORT_ATTR_OPER_STATUS: {
            int link = 0;
            rv = bcm_port_link_status_get(unit, port, &link);
            attr->value.s32 = (link != 0) ? SAI_PORT_OPER_STATUS_UP
                                          : SAI_PORT_OPER_STATUS_DOWN;
            break;
        }
        default:
            out = SAI_STATUS_NOT_SUPPORTED;
            continue;
        }

        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "port get attr 0x%x failed: %d (%s)",
                        attr->id, rv, bcm_errmsg(rv));
            out = sai_bcm_status(rv);
        }
    }

    sai_bcm_unlock();
    return out;
}

static sai_status_t get_port_stats(sai_object_id_t port_id,
                                 uint32_t number_of_counters,
                                 const sai_stat_id_t *counter_ids,
                                 uint64_t *counters)
{
    return sai_bcm_port_stats_common(port_id, number_of_counters,
                                      counter_ids, counters,
                                      SAI_STATS_MODE_READ);
}

static sai_status_t get_port_stats_ext(sai_object_id_t port_id,
                                      uint32_t number_of_counters,
                                      const sai_stat_id_t *counter_ids,
                                      sai_stats_mode_t mode,
                                      uint64_t *counters)
{
    return sai_bcm_port_stats_common(port_id, number_of_counters,
                                      counter_ids, counters, mode);
}

static sai_status_t clear_port_stats(sai_object_id_t port_id,
                                    uint32_t number_of_counters,
                                    const sai_stat_id_t *counter_ids)
{
    (void)number_of_counters;
    (void)counter_ids;

    const struct sai_bcm_port_entry *pe;
    sai_status_t st;

    sai_bcm_lock();
    st = port_lookup(port_id, &pe);
    if (st != SAI_STATUS_SUCCESS) {
        sai_bcm_unlock();
        return st;
    }

    /* P1 limitation: OpenBCM clears the whole port counter set. */
    int rv = bcm_stat_clear(g_sai_bcm_sw.unit, pe->bcm_port);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "bcm_stat_clear failed: %d (%s)",
                    rv, bcm_errmsg(rv));
    }
    st = sai_bcm_status(rv);
    sai_bcm_unlock();
    return st;
}

static sai_status_t clear_port_all_stats(sai_object_id_t port_id)
{
    return clear_port_stats(port_id, 0, NULL);
}

/* ---------------------------------------------------------------------------
 * Not-yet-implemented members (explicit NOT_SUPPORTED)
 * --------------------------------------------------------------------------- */
static sai_status_t create_port(sai_object_id_t *port_id,
                               sai_object_id_t switch_id,
                               uint32_t attr_count,
                               const sai_attribute_t *attr_list)
{
    (void)port_id; (void)switch_id; (void)attr_count; (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t remove_port(sai_object_id_t port_id)
{
    (void)port_id;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t create_port_pool(sai_object_id_t *port_pool_id,
                                    sai_object_id_t switch_id,
                                    uint32_t attr_count,
                                    const sai_attribute_t *attr_list)
{
    (void)port_pool_id; (void)switch_id; (void)attr_count; (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t remove_port_pool(sai_object_id_t port_pool_id)
{
    (void)port_pool_id;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t set_port_pool_attribute(sai_object_id_t port_pool_id,
                                           const sai_attribute_t *attr)
{
    (void)port_pool_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_port_pool_attribute(sai_object_id_t port_pool_id,
                                           uint32_t attr_count,
                                           sai_attribute_t *attr_list)
{
    (void)port_pool_id; (void)attr_count; (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_port_pool_stats(sai_object_id_t port_pool_id,
                                       uint32_t number_of_counters,
                                       const sai_stat_id_t *counter_ids,
                                       uint64_t *counters)
{
    (void)port_pool_id; (void)number_of_counters; (void)counter_ids;
    (void)counters;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_port_pool_stats_ext(sai_object_id_t port_pool_id,
                                           uint32_t number_of_counters,
                                           const sai_stat_id_t *counter_ids,
                                           sai_stats_mode_t mode,
                                           uint64_t *counters)
{
    (void)port_pool_id; (void)number_of_counters; (void)counter_ids;
    (void)mode; (void)counters;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t clear_port_pool_stats(sai_object_id_t port_pool_id,
                                         uint32_t number_of_counters,
                                         const sai_stat_id_t *counter_ids)
{
    (void)port_pool_id; (void)number_of_counters; (void)counter_ids;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t create_port_connector(sai_object_id_t *port_connector_id,
                                          sai_object_id_t switch_id,
                                          uint32_t attr_count,
                                          const sai_attribute_t *attr_list)
{
    (void)port_connector_id; (void)switch_id; (void)attr_count; (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t remove_port_connector(sai_object_id_t port_connector_id)
{
    (void)port_connector_id;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t set_port_connector_attribute(sai_object_id_t port_connector_id,
                                                const sai_attribute_t *attr)
{
    (void)port_connector_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_port_connector_attribute(sai_object_id_t port_connector_id,
                                                uint32_t attr_count,
                                                sai_attribute_t *attr_list)
{
    (void)port_connector_id; (void)attr_count; (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t create_port_serdes(sai_object_id_t *port_serdes_id,
                                      sai_object_id_t switch_id,
                                      uint32_t attr_count,
                                      const sai_attribute_t *attr_list)
{
    (void)port_serdes_id; (void)switch_id; (void)attr_count; (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t remove_port_serdes(sai_object_id_t port_serdes_id)
{
    (void)port_serdes_id;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t set_port_serdes_attribute(sai_object_id_t port_serdes_id,
                                             const sai_attribute_t *attr)
{
    (void)port_serdes_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_port_serdes_attribute(sai_object_id_t port_serdes_id,
                                             uint32_t attr_count,
                                             sai_attribute_t *attr_list)
{
    (void)port_serdes_id; (void)attr_count; (void)attr_list;
    return SAI_STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_port_api_t sai_bcm_port_api = {
    .create_port                   = create_port,
    .remove_port                   = remove_port,
    .set_port_attribute            = set_port_attribute,
    .get_port_attribute            = get_port_attribute,
    .get_port_stats                = get_port_stats,
    .get_port_stats_ext            = get_port_stats_ext,
    .clear_port_stats              = clear_port_stats,
    .clear_port_all_stats          = clear_port_all_stats,
    .create_port_pool              = create_port_pool,
    .remove_port_pool              = remove_port_pool,
    .set_port_pool_attribute       = set_port_pool_attribute,
    .get_port_pool_attribute       = get_port_pool_attribute,
    .get_port_pool_stats           = get_port_pool_stats,
    .get_port_pool_stats_ext       = get_port_pool_stats_ext,
    .clear_port_pool_stats         = clear_port_pool_stats,
    .create_port_connector         = create_port_connector,
    .remove_port_connector         = remove_port_connector,
    .set_port_connector_attribute  = set_port_connector_attribute,
    .get_port_connector_attribute = get_port_connector_attribute,
    .create_port_serdes            = create_port_serdes,
    .remove_port_serdes            = remove_port_serdes,
    .set_port_serdes_attribute     = set_port_serdes_attribute,
    .get_port_serdes_attribute     = get_port_serdes_attribute,
};