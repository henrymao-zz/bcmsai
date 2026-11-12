/*
 * Shared L3 state for the Virtual Router / Router Interface / Next Hop /
 * Next Hop Group / Route / Neighbor modules.
 *
 * OpenBCM 6.5.27 has no explicit VRF create/destroy API: bcm_vrf_t values
 * are implicit identifiers referenced by L3 structures. Virtual routers
 * are therefore allocated from an internal pool and only *used* by the
 * bcm_l3_* programming below.
 */
#ifndef SAI_BCM_L3_H
#define SAI_BCM_L3_H

#include <stdbool.h>
#include <stdint.h>

#include <sai.h>

#include <bcm/l3.h>

#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"

#define SAI_BCM_MAX_VRS            64
#define SAI_BCM_MAX_RIFS           256
#define SAI_BCM_MAX_NEXT_HOPS      1024
#define SAI_BCM_MAX_NH_GROUPS      256
#define SAI_BCM_MAX_NHG_MEMBERS    1024
#define SAI_BCM_MAX_ROUTES         4096
#define SAI_BCM_MAX_NEIGHBORS      2048

/* ---------------------------------------------------------------------------
 * Virtual router
 * --------------------------------------------------------------------------- */
struct sai_bcm_vr {
    bool             used;
    bool             is_default;    /* the switch default VR (vrf 0)      */
    bcm_vrf_t        vrf;           /* BCM vrf id                        */
    bool             admin_v4;
    bool             admin_v6;
    sai_mac_t        src_mac;
};

extern sai_object_id_t g_sai_bcm_default_vr_oid;

struct sai_bcm_vr *sai_bcm_vr_get(sai_object_id_t vr_oid);

/* Create the default virtual router (vrf 0); called from create_switch
 * (recursive lock already held). */
sai_status_t sai_bcm_vr_init(void);
void sai_bcm_vr_deinit(void);

/* ---------------------------------------------------------------------------
 * Router interface
 * --------------------------------------------------------------------------- */
struct sai_bcm_rif {
    bool              used;
    sai_object_id_t   vr_oid;
    sai_router_interface_type_t type;
    sai_object_id_t   port_oid;     /* PORT or LAG (PORT/LOOPBACK types) */
    sai_object_id_t   vlan_oid;     /* VLAN (VLAN type RIFs)            */
    bool              admin_state;
    bcm_if_t          ingress_if;   /* bcm_l3_ingress_create result     */
    bcm_if_t          egress_if;    /* l3 egress interface id (vid/0)   */
};

struct sai_bcm_rif *sai_bcm_rif_get(sai_object_id_t rif_oid);

/* Number of RIFs bound to a given virtual router (VR remove check). */
uint32_t sai_bcm_rif_count_on_vr(sai_object_id_t vr_oid);

/* Resolve a RIF's egress programming parameters. Returns BCM_E_* codes. */
int sai_bcm_rif_egress_params(const struct sai_bcm_rif *rif,
                             bcm_vlan_t *vid, bcm_gport_t *gport,
                             bool *is_trunk);

/* ---------------------------------------------------------------------------
 * Next hop
 * --------------------------------------------------------------------------- */
struct sai_bcm_nh {
    bool              used;
    sai_object_id_t   rif_oid;
    sai_ip_address_t  ip;            /* network byte order              */
    bcm_if_t          egress_if;     /* bcm_l3_egress_create result     */
};

struct sai_bcm_nh *sai_bcm_nh_get(sai_object_id_t nh_oid);

/* Re-program the egress objects of next hops matching (rif, ip) with a new
 * forwarding MAC, in place (BCM_L3_REPLACE keeps the egress ids stable so
 * routes do not need repointing). Called from the neighbor module when a
 * neighbor is created or its MAC changes. */
void sai_bcm_nh_refresh_mac(sai_object_id_t rif_oid,
                            const sai_ip_address_t *ip, const sai_mac_t mac);

/* ---------------------------------------------------------------------------
 * Next hop group (ECMP)
 * --------------------------------------------------------------------------- */
struct sai_bcm_nhg {
    bool              used;
    bool              hw_created;    /* ecmp object exists in hardware  */
    bcm_if_t          ecmp_intf;     /* bcm_l3_egress_ecmp_create res   */
};

struct sai_bcm_nhg_member {
    bool              used;
    sai_object_id_t   nhg_oid;
    sai_object_id_t   nh_oid;
};

struct sai_bcm_nhg *sai_bcm_nhg_get(sai_object_id_t nhg_oid);
struct sai_bcm_nhg_member *
sai_bcm_nhg_member_get(sai_object_id_t member_oid);

/* ---------------------------------------------------------------------------
 * Route / Neighbor
 * --------------------------------------------------------------------------- */
struct sai_bcm_route {
    bool              used;
    sai_object_id_t   vr_oid;
    sai_ip_prefix_t   prefix;
    sai_object_id_t   nh_oid;        /* NEXT_HOP or NEXT_HOP_GROUP oid  */
};

struct sai_bcm_neighbor {
    bool              used;
    sai_object_id_t   rif_oid;
    sai_ip_address_t  ip;
    sai_mac_t         mac;
};

/* Resolve a NEXT_HOP or NEXT_HOP_GROUP oid to a BCM l3 egress id.
 * Returns BCM_E_NONE and fills *intf, or a negative BCM error. */
int sai_bcm_nh_or_group_to_l3_intf(sai_object_id_t nh_oid, bcm_if_t *intf);

/* Find a locally tracked neighbor by (rif, ip). */
struct sai_bcm_neighbor *
sai_bcm_neighbor_find(sai_object_id_t rif_oid, const sai_ip_address_t *ip);

#endif /* SAI_BCM_L3_H */