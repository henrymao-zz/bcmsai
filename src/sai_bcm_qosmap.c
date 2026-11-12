/*
 * SAI QoS Map API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 4 minimal scope: DSCP -> traffic class maps. Each (dscp, tc) pair
 * is programmed via bcm_qos_map_add with the L3 ingress mapping
 * (map.dscp / map.int_pri) and removed via bcm_qos_map_delete on
 * remove_qos_map. Other map types return SAI_STATUS_NOT_SUPPORTED.
 */
#include "sai_bcm_internal.h"

#include <bcm/qos.h>

#include <string.h>

#define SAI_BCM_MAX_QOS_MAPS  64
#define SAI_BCM_MAX_MAP_ENTRIES 64

struct sai_bcm_qos_map {
    bool               used;
    sai_qos_map_type_t type;
    uint32_t           entry_count;
    sai_qos_map_t      entries[SAI_BCM_MAX_MAP_ENTRIES];
};

static struct sai_bcm_qos_map g_qos_maps[SAI_BCM_MAX_QOS_MAPS];

static struct sai_bcm_qos_map *qos_map_get(sai_object_id_t oid)
{
    if (sai_bcm_oid_type(oid) != SAI_OBJECT_TYPE_QOS_MAP) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(oid);
    if (idx >= SAI_BCM_MAX_QOS_MAPS || !g_qos_maps[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_QOS_MAP, idx) != oid) {
        return NULL;
    }
    return &g_qos_maps[idx];
}

/* ---------------------------------------------------------------------------
 * SAI QoS Map API
 * --------------------------------------------------------------------------- */
static sai_status_t create_qos_map(sai_object_id_t *qos_map_id,
                                  sai_object_id_t switch_id,
                                  uint32_t attr_count,
                                  const sai_attribute_t *attr_list)
{
    (void)switch_id;

    if (qos_map_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_qos_map_type_t type = SAI_QOS_MAP_TYPE_DSCP_TO_TC;
    const sai_qos_map_t *list = NULL;
    uint32_t count = 0;

    for (uint32_t i = 0; i < attr_count; i++) {
        const sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_QOS_MAP_ATTR_TYPE:
            type = (sai_qos_map_type_t)attr->value.s32;
            break;
        case SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST:
            list = attr->value.qosmap.list;
            count = attr->value.qosmap.count;
            break;
        default:
            return SAI_STATUS_NOT_SUPPORTED;
        }
    }

    if (type != SAI_QOS_MAP_TYPE_DSCP_TO_TC) {
        return SAI_STATUS_NOT_SUPPORTED;
    }
    if (count > SAI_BCM_MAX_MAP_ENTRIES ||
        (count > 0 && list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_QOS_MAP, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_QOS_MAPS) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_QOS_MAP, idx);
        }
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    /* Program every dscp -> tc pair as an L3 ingress QoS map entry. */
    for (uint32_t i = 0; i < count; i++) {
        bcm_qos_map_t map;
        bcm_qos_map_t_init(&map);
        map.dscp = list[i].key.dscp;
        map.int_pri = list[i].value.tc;

        int rv = bcm_qos_map_add(g_sai_bcm_sw.unit,
                                 BCM_QOS_MAP_L3 | BCM_QOS_MAP_INGRESS,
                                 &map, 0);
        if (rv < 0) {
            SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                        "bcm_qos_map_add failed: %d (%s)", rv,
                        bcm_errmsg(rv));
            /* Roll back the entries we already added. */
            for (uint32_t j = 0; j < i; j++) {
                bcm_qos_map_t rb;
                bcm_qos_map_t_init(&rb);
                rb.dscp = list[j].key.dscp;
                rb.int_pri = list[j].value.tc;
                (void)bcm_qos_map_delete(g_sai_bcm_sw.unit,
                                         BCM_QOS_MAP_L3 |
                                             BCM_QOS_MAP_INGRESS,
                                         &rb, 0);
            }
            sai_bcm_idx_free(SAI_OBJECT_TYPE_QOS_MAP, idx);
            sai_bcm_unlock();
            return sai_bcm_status(rv);
        }
    }

    g_qos_maps[idx] = (struct sai_bcm_qos_map){
        .used = true,
        .type = type,
        .entry_count = count,
    };
    if (count > 0) {
        memcpy(g_qos_maps[idx].entries, list,
               count * sizeof(sai_qos_map_t));
    }
    *qos_map_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                     SAI_OBJECT_TYPE_QOS_MAP, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_qos_map(sai_object_id_t qos_map_id)
{
    sai_bcm_lock();

    struct sai_bcm_qos_map *map = qos_map_get(qos_map_id);
    if (map == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    for (uint32_t i = 0; i < map->entry_count; i++) {
        bcm_qos_map_t entry;
        bcm_qos_map_t_init(&entry);
        entry.dscp = map->entries[i].key.dscp;
        entry.int_pri = map->entries[i].value.tc;
        (void)bcm_qos_map_delete(g_sai_bcm_sw.unit,
                                 BCM_QOS_MAP_L3 | BCM_QOS_MAP_INGRESS,
                                 &entry, 0);
    }

    uint64_t idx = sai_bcm_oid_index(qos_map_id);
    memset(map, 0, sizeof(*map));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_QOS_MAP, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_qos_map_attribute(sai_object_id_t qos_map_id,
                                         const sai_attribute_t *attr)
{
    (void)qos_map_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_qos_map_attribute(sai_object_id_t qos_map_id,
                                         uint32_t attr_count,
                                         sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_qos_map *map = qos_map_get(qos_map_id);
    if (map == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_QOS_MAP_ATTR_TYPE:
            attr->value.s32 = map->type;
            break;
        case SAI_QOS_MAP_ATTR_MAP_TO_VALUE_LIST:
            if (attr->value.qosmap.list == NULL ||
                attr->value.qosmap.count < map->entry_count) {
                attr->value.qosmap.count = map->entry_count;
                out = SAI_STATUS_BUFFER_OVERFLOW;
            } else {
                memcpy(attr->value.qosmap.list, map->entries,
                       map->entry_count * sizeof(sai_qos_map_t));
                attr->value.qosmap.count = map->entry_count;
            }
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
const sai_qos_map_api_t sai_bcm_qos_map_api = {
    .create_qos_map        = create_qos_map,
    .remove_qos_map        = remove_qos_map,
    .set_qos_map_attribute = set_qos_map_attribute,
    .get_qos_map_attribute = get_qos_map_attribute,
};