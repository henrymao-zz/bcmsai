/*
 * Mock OpenBCM backend for dev builds and CI.
 *
 * Implements exactly the real OpenBCM sdk-6.5.27 function signatures (the
 * adapter is compiled against the real headers) but simulates a
 * Firebolt6-class device: 32 front-panel XE ports (logical 1..32) plus a
 * CPU port (logical 0). Unit 0 only.
 *
 * Test-control helpers are exposed so unit tests can drive the simulated
 * device (link state, counters). When libsai-bcm is built with
 * SAI_BCM_BACKEND=openbcm, none of this code is built or linked.
 */
#ifndef MOCK_BCM_H
#define MOCK_BCM_H

#include <stdint.h>

#include <bcm/types.h>
#include <bcm/error.h>
#include <bcm/port.h>
#include <bcm/stat.h>

#define MOCK_BCM_FRONT_PANEL_PORTS 32
#define MOCK_BCM_CPU_PORT           0
#define MOCK_BCM_MAX_STATS          512

/* Reset the whole simulated device (call between test cases). */
void mock_bcm_reset(void);

/* Number of successful bcm_init calls minus bcm_detach calls. */
int mock_bcm_init_count(void);

/* Force the raw link status of a port (bypasses the enabled state). */
void mock_bcm_set_link(int port, int up);

/* Preload a counter value for a port. */
void mock_bcm_set_stat(int port, bcm_stat_val_t type, uint64_t value);

/* Read a counter value for a port. */
uint64_t mock_bcm_get_stat(int port, bcm_stat_val_t type);

/* Whether bcm_stat_sync was called since the last reset or last sync read. */
int mock_bcm_stat_sync_count(void);

/* Whether bcm_stat_clear was called for the given port. */
int mock_bcm_stat_clear_count(int port);

/* ---- VLAN ---- */
#define MOCK_BCM_VLAN_MAX 4095 /* vids 1..4095 */

int mock_bcm_vlan_used(int vid);
int mock_bcm_vlan_member_count(int vid);
/* 0 = not member, 1 = untagged, 2 = tagged */
int mock_bcm_vlan_member_mode(int vid, int gport);

/* ---- trunk ---- */
int mock_bcm_trunk_used(int tid);
int mock_bcm_trunk_member_count(int tid);
int mock_bcm_trunk_has_member(int tid, int gport);

/* ---- l2 ---- */
int mock_bcm_l2_count(void);
int mock_bcm_l2_is_static(int port, const uint8_t mac[6], int vid);
int mock_bcm_l2_dest_is_trunk(int port, const uint8_t mac[6], int vid,
                              int *tgid);

/* ---- L3 ---- */
#define MOCK_BCM_L3_EGRESS_MAX  1024
#define MOCK_BCM_L3_ECMP_MAX    256
#define MOCK_BCM_L3_ROUTE_MAX   1024
#define MOCK_BCM_L3_HOST_MAX    1024

int mock_bcm_l3_ingress_exists(int intf_id);
int mock_bcm_l3_egress_count(void);
/* 0 = gone, 1 = exists */
int mock_bcm_l3_egress_exists(int intf_id);
int mock_bcm_l3_egress_vlan(int intf_id);
int mock_bcm_l3_egress_is_trunk(int intf_id);
int mock_bcm_l3_ecmp_exists(int intf_id);
int mock_bcm_l3_ecmp_member_count(int intf_id);
int mock_bcm_l3_route_count(int vrf);
int mock_bcm_l3_host_count(int vrf);

/* ---- rx traps / knet / field / qos ---- */
#define MOCK_BCM_TRAP_MAX        128
#define MOCK_BCM_KNETIF_MAX      32
#define MOCK_BCM_FIELD_GROUP_MAX 32
#define MOCK_BCM_FIELD_ENTRY_MAX 256
#define MOCK_BCM_QOS_MAP_MAX     512

int mock_bcm_trap_used(int trap_id);
int mock_bcm_trap_count(void);
int mock_bcm_knetif_count(void);
int mock_bcm_field_group_count(void);
int mock_bcm_field_group_has_entry(int group_id);
int mock_bcm_field_entry_installed(int entry_id);
/* returns 1 when the entry carries bcmFieldActionDrop */
int mock_bcm_field_entry_drops(int entry_id);
int mock_bcm_qos_map_count(void);

/* ---- vswitch ---- */
int mock_bcm_vswitch_used(int vsi);
int mock_bcm_vswitch_member_count(int vsi);

#endif /* MOCK_BCM_H */