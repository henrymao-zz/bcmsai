/*
 * libsai-bcm internal header.
 *
 * Shared adapter state, locking and cross-module accessors. All symbols
 * here are internal: the version script (cmake/sai_bcm.ver) keeps them
 * local to libsai.so.
 */
#ifndef SAI_BCM_INTERNAL_H
#define SAI_BCM_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <sai.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/port.h>
#include <bcm/stat.h>
#include <bcm/init.h>

#include "sai_bcm_oid.h"

/* Extern declarations for the 44 per-module vtable instances.
 * sai_bcm_api_tables.x is included multiple times; define the consumer
 * macro before each inclusion and #undef it after. */
#define SAI_BCM_API(ENUM, NAME, IMPL) \
    extern const sai_##NAME##_api_t sai_bcm_##NAME##_api;
#include "sai_bcm_api_tables.x"
#undef SAI_BCM_API

/* ---------------------------------------------------------------------------
 * Generic per-type index allocator (src/sai_bcm_oid.c)
 * --------------------------------------------------------------------------- */
sai_status_t sai_bcm_idx_alloc(sai_object_type_t type, uint32_t *idx);
void sai_bcm_idx_free(sai_object_type_t type, uint32_t idx);
bool sai_bcm_idx_valid(sai_object_type_t type, uint32_t idx);
uint16_t sai_bcm_idx_count(sai_object_type_t type);
void sai_bcm_idx_reset_all(void);

/* ---------------------------------------------------------------------------
 * Shared bulk operation engine (src/sai_bcm_bulk.c)
 * --------------------------------------------------------------------------- */
typedef sai_status_t (*sai_bcm_bulk_create_one_fn)(
        sai_object_id_t *oid, sai_object_id_t switch_id, uint32_t attr_count,
        const sai_attribute_t *attr_list);

typedef sai_status_t (*sai_bcm_bulk_remove_one_fn)(sai_object_id_t oid);

void sai_bcm_bulk_mark_not_executed(sai_status_t *statuses, uint32_t from,
                                    uint32_t count);

sai_status_t sai_bcm_bulk_object_create(
        sai_object_id_t switch_id, uint32_t obj_count,
        const uint32_t *attr_count, const sai_attribute_t **attr_list,
        sai_bulk_op_error_mode_t mode, sai_object_id_t *object_id,
        sai_status_t *object_statuses,
        sai_bcm_bulk_create_one_fn create_one);

sai_status_t sai_bcm_bulk_object_remove(
        uint32_t obj_count, const sai_object_id_t *object_id,
        sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses,
        sai_bcm_bulk_remove_one_fn remove_one);

/* Build-time target metadata (set by CMake). */
#ifndef SAI_BCM_TARGET_NAME
#define SAI_BCM_TARGET_NAME "BCM56371"
#endif
#ifndef SAI_BCM_OBCM_CHIP_MACRO
#define SAI_BCM_OBCM_CHIP_MACRO "BCM_56370_A0"
#endif
#ifndef SAI_BCM_OBCM_STACK
#define SAI_BCM_OBCM_STACK "ESW"
#endif

#define SAI_BCM_UNIT_DEFAULT 0
#define SAI_BCM_MAX_PORTS   256

/* ---------------------------------------------------------------------------
 * Logging + status mapping
 * --------------------------------------------------------------------------- */
void sai_bcm_log_init(sai_log_level_t level);
sai_log_level_t sai_bcm_log_level(void);
void sai_bcm_log(sai_log_level_t level, const char *file, int line,
                 const char *fmt, ...);

#define SAI_BCM_LOG(lvl, ...) \
    sai_bcm_log((lvl), __FILE__, __LINE__, __VA_ARGS__)

/* Map a BCM error code (_shr_err_t / BCM_E_*) to a sai_status_t. */
sai_status_t sai_bcm_status(int bcm_rv);

/* ---------------------------------------------------------------------------
 * Adapter lock / lifecycle (implemented in sai_bcm_api.c)
 * --------------------------------------------------------------------------- */
void sai_bcm_lock(void);
void sai_bcm_unlock(void);
bool sai_bcm_initialized(void);

/* ---------------------------------------------------------------------------
 * Switch state (implemented in sai_bcm_switch.c)
 * --------------------------------------------------------------------------- */
struct sai_bcm_port_entry {
    bcm_port_t   bcm_port;   /* BCM logical port number               */
    sai_object_id_t port_oid; /* encoded SAI PORT object id            */
};

struct sai_bcm_switch_state {
    bool              created;      /* a switch instance exists          */
    sai_object_id_t   switch_oid;   /* encoded SAI SWITCH object id      */
    int               unit;         /* BCM unit number                   */
    uint32_t          port_count;   /* number of front panel ports       */
    uint32_t          max_ports;    /* max supported front panel ports   */
    struct sai_bcm_port_entry ports[SAI_BCM_MAX_PORTS];

    /* CREATE_AND_SET attributes we retain */
    sai_switch_profile_id_t profile_id;
    sai_mac_t              src_mac;

    /* notifications retained from the adapter host */
    sai_port_state_change_notification_fn  port_state_change_notify;
    sai_fdb_event_notification_fn          fdb_event_notify;
    sai_switch_shutdown_request_notification_fn shutdown_request_notify;
};

extern struct sai_bcm_switch_state g_sai_bcm_sw;

/* Look up a port by encoded SAI port object id.
 * Returns the port entry, or NULL when the oid is not a live port. */
const struct sai_bcm_port_entry *
sai_bcm_switch_port_lookup(sai_object_id_t port_id);

/* Reverse map: BCM logical port number -> SAI port oid
 * (SAI port index and BCM port numbers need not be equal). */
sai_object_id_t sai_bcm_switch_oid_from_bcm_port(bcm_port_t bcm_port);

#endif /* SAI_BCM_INTERNAL_H */