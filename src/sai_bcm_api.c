/*
 * libsai-bcm entry points.
 *
 * P0: implements the SAI adapter entry points and the api_query dispatch
 * over the 44 zero-initialized per-module vtables. No BCM calls yet — the
 * real bcm_init()/bcm_attach() happens in sai_bcm_switch.c (P1) once the
 * OpenBCM SDK libraries are linked in.
 */
#include "sai_bcm_internal.h"

#include <pthread.h>

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
    .lock         = PTHREAD_MUTEX_INITIALIZER,
};

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
#define SAI_BCM_API(ENUM, NAME) \
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
 * Object queries — P0 stubs (no OID registry yet)
 * --------------------------------------------------------------------------- */
sai_object_type_t sai_object_type_query(sai_object_id_t object_id)
{
    (void)object_id;
    return SAI_OBJECT_TYPE_NULL;
}

sai_object_id_t sai_switch_id_query(sai_object_id_t object_id)
{
    (void)object_id;
    return SAI_NULL_OBJECT_ID;
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
    (void)switch_id;
    (void)object_type;
    (void)attr_count;
    (void)attr_list;

    if (count == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    *count = 0;
    return SAI_STATUS_NOT_SUPPORTED;
}