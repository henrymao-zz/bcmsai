/*
 * libsai-bcm entry points.
 *
 * sai_api_initialize/uninitialize/log_set lifecycle, sai_api_query dispatch
 * over the per-module vtables, and the OID-based object queries.
 */
/* PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP (GNU) — the adapter lock must be
 * recursive: module init helpers re-enter the lock from create_switch. */
#define _GNU_SOURCE

#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"
#include "sai_bcm_l3.h"

#include <string.h>

/* ---------------------------------------------------------------------------
 * Global adapter state
 * --------------------------------------------------------------------------- */
static struct {
    bool                      initialized;
    const sai_service_method_table_t *services;
    sai_log_level_t           log_level;
    pthread_mutex_t           lock;
} g_state = {
    .initialized = false,
    .services     = NULL,
    .log_level    = SAI_LOG_LEVEL_WARN,
    /* Recursive: module implementations (e.g. sai_bcm_bridge_init called
     * from create_switch) take the same lock while it is held. */
    .lock         = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP,
};

/* ---------------------------------------------------------------------------
 * Adapter lock / lifecycle helpers (used by all modules)
 * --------------------------------------------------------------------------- */
void sai_bcm_lock(void)
{
    pthread_mutex_lock(&g_state.lock);
}

void sai_bcm_unlock(void)
{
    pthread_mutex_unlock(&g_state.lock);
}

bool sai_bcm_initialized(void)
{
    return g_state.initialized;
}

/* ---------------------------------------------------------------------------
 * sai_api_initialize / sai_api_uninitialize
 * --------------------------------------------------------------------------- */
sai_status_t sai_api_initialize(uint64_t flags,
                                const sai_service_method_table_t *services)
{
    (void)flags;

    pthread_mutex_lock(&g_state.lock);

    if (g_state.initialized) {
        pthread_mutex_unlock(&g_state.lock);
        SAI_BCM_LOG(SAI_LOG_LEVEL_WARN, "already initialized");
        return SAI_STATUS_FAILURE;
    }

    g_state.services  = services;
    g_state.log_level = SAI_LOG_LEVEL_WARN;
    sai_bcm_log_init(g_state.log_level);
    g_state.initialized = true;

    SAI_BCM_LOG(SAI_LOG_LEVEL_INFO,
                "initialized (target=%s stack=%s macro=%s)",
                SAI_BCM_TARGET_NAME, SAI_BCM_OBCM_STACK,
                SAI_BCM_OBCM_CHIP_MACRO);

    pthread_mutex_unlock(&g_state.lock);
    return SAI_STATUS_SUCCESS;
}

sai_status_t sai_api_uninitialize(void)
{
    pthread_mutex_lock(&g_state.lock);

    if (!g_state.initialized) {
        pthread_mutex_unlock(&g_state.lock);
        return SAI_STATUS_UNINITIALIZED;
    }

    if (g_sai_bcm_sw.created) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_WARN,
                    "uninitialize with live switch; detaching");
        (void)bcm_detach(g_sai_bcm_sw.unit);
        sai_bcm_bridge_deinit();
        sai_bcm_vr_deinit();
        memset(&g_sai_bcm_sw, 0, sizeof(g_sai_bcm_sw));
        g_sai_bcm_sw.unit = SAI_BCM_UNIT_DEFAULT;
    }

    sai_bcm_idx_reset_all();

    g_state.initialized = false;
    g_state.services     = NULL;

    pthread_mutex_unlock(&g_state.lock);
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * sai_api_query — dispatch to the per-module vtable
 * --------------------------------------------------------------------------- */
sai_status_t sai_api_query(sai_api_t api, void **api_method_table)
{
    if (api_method_table == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    if (!g_state.initialized) {
        return SAI_STATUS_UNINITIALIZED;
    }

    const void *tbl = NULL;

    switch (api) {
#define SAI_BCM_API(ENUM, NAME, IMPL) \
        case ENUM: tbl = &sai_bcm_##NAME##_api; break;
#include "sai_bcm_api_tables.x"
#undef SAI_BCM_API
        case SAI_API_UNSPECIFIED:
        case SAI_API_MAX:
        default:
            return SAI_STATUS_INVALID_PARAMETER;
    }

    *api_method_table = (void *)tbl;
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * sai_log_set
 * --------------------------------------------------------------------------- */
sai_status_t sai_log_set(sai_api_t api, sai_log_level_t log_level)
{
    (void)api;

    pthread_mutex_lock(&g_state.lock);
    g_state.log_level = log_level;
    sai_bcm_log_init(log_level);
    pthread_mutex_unlock(&g_state.lock);
    return SAI_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Object queries — decode the encoded SAI object id
 * --------------------------------------------------------------------------- */
sai_object_type_t sai_object_type_query(sai_object_id_t object_id)
{
    sai_object_type_t type = sai_bcm_oid_type(object_id);

    if (type == SAI_OBJECT_TYPE_NULL) {
        return SAI_OBJECT_TYPE_NULL;
    }
    return type;
}

sai_object_id_t sai_switch_id_query(sai_object_id_t object_id)
{
    uint8_t sw = sai_bcm_oid_switch(object_id);
    if (sw == 0) {
        return SAI_NULL_OBJECT_ID;
    }
    if (!g_sai_bcm_sw.created) {
        return SAI_NULL_OBJECT_ID;
    }
    if (sw != SAI_BCM_SWITCH_INSTANCE_FIRST) {
        return SAI_NULL_OBJECT_ID;
    }
    return g_sai_bcm_sw.switch_oid;
}

sai_status_t sai_dbg_generate_dump(const char *dump_file_name)
{
    (void)dump_file_name;
    return SAI_STATUS_NOT_SUPPORTED;
}

sai_status_t sai_object_type_get_availability(sai_object_id_t switch_id,
                                              sai_object_type_t object_type,
                                              uint32_t attr_count,
                                              const sai_attribute_t *attr_list,
                                              uint64_t *count)
{
    (void)attr_list;

    if (count == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    if (!g_sai_bcm_sw.created || switch_id != g_sai_bcm_sw.switch_oid) {
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    /* Registry-backed types: free indices = capacity - allocated. */
    switch (object_type) {
    case SAI_OBJECT_TYPE_PORT:
        if (attr_count != 0) {
            return SAI_STATUS_NOT_SUPPORTED;
        }
        *count = g_sai_bcm_sw.port_count;
        return SAI_STATUS_SUCCESS;
    case SAI_OBJECT_TYPE_VLAN: {
        if (attr_count != 0) {
            return SAI_STATUS_NOT_SUPPORTED;
        }
        uint32_t used = 0;
        for (uint32_t vid = SAI_BCM_VLAN_MIN; vid <= SAI_BCM_VLAN_MAX;
             vid++) {
            if (g_sai_bcm_vlan_used[vid]) {
                used++;
            }
        }
        *count = (uint64_t)(SAI_BCM_VLAN_MAX - SAI_BCM_VLAN_MIN + 1) - used;
        return SAI_STATUS_SUCCESS;
    }
#define SAI_BCM_AVAIL(TYPE, CAP)                                            \
    case SAI_OBJECT_TYPE_##TYPE:                                            \
        if (attr_count != 0) {                                              \
            return SAI_STATUS_NOT_SUPPORTED;                               \
        }                                                                   \
        *count = (uint64_t)(CAP) - sai_bcm_idx_count(object_type);          \
        return SAI_STATUS_SUCCESS;
    SAI_BCM_AVAIL(VLAN_MEMBER, 4096)
    SAI_BCM_AVAIL(LAG, 128)
    SAI_BCM_AVAIL(LAG_MEMBER, 512)
    SAI_BCM_AVAIL(VIRTUAL_ROUTER, 64)
    SAI_BCM_AVAIL(ROUTER_INTERFACE, 256)
    SAI_BCM_AVAIL(NEXT_HOP, 1024)
    SAI_BCM_AVAIL(NEXT_HOP_GROUP, 256)
    SAI_BCM_AVAIL(NEXT_HOP_GROUP_MEMBER, 1024)
    SAI_BCM_AVAIL(BRIDGE, 64)
    SAI_BCM_AVAIL(BRIDGE_PORT, 384)
    SAI_BCM_AVAIL(HOSTIF_TRAP_GROUP, 64)
    SAI_BCM_AVAIL(HOSTIF_TRAP, 256)
    SAI_BCM_AVAIL(HOSTIF, 64)
    SAI_BCM_AVAIL(ACL_TABLE, 64)
    SAI_BCM_AVAIL(ACL_ENTRY, 512)
    SAI_BCM_AVAIL(QOS_MAP, 64)
#undef SAI_BCM_AVAIL
    default:
        *count = 0;
        return SAI_STATUS_NOT_SUPPORTED;
    }
}