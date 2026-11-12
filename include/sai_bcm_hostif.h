/*
 * Shared state for the Host Interface (trap group / trap / hostif) module.
 */
#ifndef SAI_BCM_HOSTIF_H
#define SAI_BCM_HOSTIF_H

#include <stdbool.h>
#include <stdint.h>

#include <sai.h>

#include <bcm/rx.h>
#include <bcm/knet.h>

#include "sai_bcm_internal.h"

#define SAI_BCM_MAX_TRAP_GROUPS  64
#define SAI_BCM_MAX_TRAPS        256
#define SAI_BCM_MAX_HOSTIFS      64

/* ---------------------------------------------------------------------------
 * Trap groups
 * --------------------------------------------------------------------------- */
struct sai_bcm_trap_group {
    bool            used;
    uint32_t        cpu_queue;      /* SAI_HOSTIF_TRAP_GROUP_ATTR_QUEUE */
};

struct sai_bcm_trap_group *sai_bcm_trap_group_get(sai_object_id_t oid);

/* ---------------------------------------------------------------------------
 * Traps
 * --------------------------------------------------------------------------- */
struct sai_bcm_trap {
    bool                      used;
    sai_hostif_trap_type_t    trap_type;
    sai_object_id_t           group_oid;
    sai_packet_action_t       action;
    uint32_t                  priority;
    int                       bcm_trap_id;    /* bcm_rx_trap_type_create res */
};

struct sai_bcm_trap *sai_bcm_trap_get(sai_object_id_t oid);

/* Map a SAI hostif trap type to the BCM rx trap enum.
 * Returns false when the type has no hardware counterpart at all. */
bool sai_bcm_trap_type_map(sai_hostif_trap_type_t sai_type,
                          bcm_rx_trap_t *bcm_type);

/* ---------------------------------------------------------------------------
 * Hostifs (netdev)
 * --------------------------------------------------------------------------- */
struct sai_bcm_hostif {
    bool            used;
    sai_object_id_t obj_id;        /* port/vlan/lag the netdev is bound to */
    char            name[16];
    int             knet_netif_id;
};

struct sai_bcm_hostif *sai_bcm_hostif_get(sai_object_id_t oid);

#endif /* SAI_BCM_HOSTIF_H */