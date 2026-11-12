/*
 * Shared L2 state for the VLAN / Bridge / LAG / FDB modules.
 *
 * All symbols are internal to libsai.so. State is authoritative for
 * object metadata (oid -> attributes) while OpenBCM holds the actual
 * hardware tables; the two are kept in sync by each module.
 */
#ifndef SAI_BCM_L2_H
#define SAI_BCM_L2_H

#include <stdbool.h>
#include <stdint.h>

#include <sai.h>

#include <bcm/types.h>
#include <bcm/l2.h>
#include <bcm/trunk.h>
#include <bcm/vlan.h>
#include <bcm/vswitch.h>

#include "sai_bcm_internal.h"

#define SAI_BCM_VLAN_MIN        1
#define SAI_BCM_VLAN_MAX        4094
#define SAI_BCM_VLAN_COUNT      (SAI_BCM_VLAN_MAX + 1)

#define SAI_BCM_MAX_VLAN_MEMBERS   4096
#define SAI_BCM_MAX_BRIDGES        64
#define SAI_BCM_MAX_BRIDGE_PORTS   (SAI_BCM_MAX_PORTS + 128)
#define SAI_BCM_MAX_LAGS           128
#define SAI_BCM_MAX_LAG_MEMBERS    512
#define SAI_BCM_MAX_FDB_ENTRIES    4096

/* ---------------------------------------------------------------------------
 * Bridge
 * --------------------------------------------------------------------------- */
struct sai_bcm_bridge {
    bool            used;
    sai_bridge_type_t type;
    bool            is_default_1q;  /* the implicit default VLAN-aware bridge */
    bcm_vlan_t      vsi;            /* BCM vswitch id (1D bridges only)   */
};

struct sai_bcm_bridge_port {
    bool              used;
    sai_object_id_t   bridge_oid;
    sai_object_id_t   port_oid;     /* SAI PORT or LAG oid              */
    bool              admin_state;
};

/* The single default 1Q bridge (index 0 of g_sai_bcm_bridges). */
extern sai_object_id_t g_sai_bcm_default_1q_bridge_oid;

struct sai_bcm_bridge *sai_bcm_bridge_get(sai_object_id_t bridge_oid);
struct sai_bcm_bridge_port *
sai_bcm_bridge_port_get(sai_object_id_t bridge_port_oid);

/* Create the default 1Q bridge + one bridge port per front-panel port.
 * Called from create_switch (lock is already held; recursive). */
sai_status_t sai_bcm_bridge_init(void);
void sai_bcm_bridge_deinit(void);

/* Resolve a bridge port oid to a BCM gport (port or trunk).
 * Returns BCM_E_NONE on success (gport filled), negative BCM error code on
 * failure (port not found / not initialized). */
int sai_bcm_bridge_port_to_gport(sai_object_id_t bridge_port_oid,
                                bcm_gport_t *gport);

/* ---------------------------------------------------------------------------
 * VLAN
 * --------------------------------------------------------------------------- */
extern bool g_sai_bcm_vlan_used[SAI_BCM_VLAN_COUNT];

struct sai_bcm_vlan_member {
    bool                   used;
    sai_object_id_t        vlan_oid;
    sai_object_id_t        bridge_port_oid;
    sai_vlan_tagging_mode_t tagging_mode;
};

struct sai_bcm_vlan_member *
sai_bcm_vlan_member_get(sai_object_id_t member_oid);

/* vid <-> vlan oid (index == vid) */
static inline sai_object_id_t sai_bcm_vlan_oid(uint16_t vid)
{
    return sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                              SAI_OBJECT_TYPE_VLAN, vid);
}

static inline uint16_t sai_bcm_vid_from_oid(sai_object_id_t oid)
{
    return (uint16_t)sai_bcm_oid_index(oid);
}

/* ---------------------------------------------------------------------------
 * LAG
 * --------------------------------------------------------------------------- */
struct sai_bcm_lag {
    bool          used;
    bcm_trunk_t   trunk_id;
};

struct sai_bcm_lag_member {
    bool            used;
    sai_object_id_t lag_oid;
    sai_object_id_t port_oid;
};

struct sai_bcm_lag *sai_bcm_lag_get(sai_object_id_t lag_oid);
struct sai_bcm_lag_member *sai_bcm_lag_member_get(sai_object_id_t member_oid);

/* Resolve a LAG oid to a BCM trunk gport (BCM_GPORT_TRUNK_SET). */
int sai_bcm_lag_to_gport(sai_object_id_t lag_oid, bcm_gport_t *gport);

/* Is this oid a valid LAG member port target? (PORT or LAG) */
bool sai_bcm_is_port_or_lag(sai_object_id_t oid);

/* Resolve PORT or LAG oid to BCM gport. */
int sai_bcm_port_or_lag_to_gport(sai_object_id_t oid, bcm_gport_t *gport);

/* ---------------------------------------------------------------------------
 * FDB
 * --------------------------------------------------------------------------- */
struct sai_bcm_fdb {
    bool                 used;
    sai_mac_t            mac;
    sai_object_id_t      bv_id;         /* VLAN oid (1Q) or bridge oid (1D) */
    sai_object_id_t      bridge_port_oid;
    sai_fdb_entry_type_t type;
};

struct sai_bcm_fdb *sai_bcm_fdb_get(const sai_fdb_entry_t *fdb_entry);
void sai_bcm_fdb_reset(void);

#endif /* SAI_BCM_L2_H */