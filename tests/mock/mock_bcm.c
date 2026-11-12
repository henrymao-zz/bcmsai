/*
 * Mock OpenBCM backend implementation. See mock_bcm.h for details.
 */
#include "mock_bcm.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <shared/error.h>
#include <bcm/vlan.h>
#include <bcm/trunk.h>
#include <bcm/l2.h>
#include <bcm/vswitch.h>
#include <bcm/l3.h>

/* ---- L2 simulation state (defined fully below, sized here so that
 * mock_bcm_reset can clear them) ---- */
#define MOCK_BCM_MAX_PORTS    64 /* logical port ids 0..63 */
#define MOCK_BCM_VLAN_MAX     4095
#define MOCK_BCM_TRUNKS_MAX   64
#define MOCK_BCM_L2_ENTRIES   512
#define MOCK_BCM_VSWITCH_MAX  32

static uint8_t g_vlan_used[MOCK_BCM_VLAN_MAX + 1];
static uint8_t g_vlan_port[MOCK_BCM_VLAN_MAX + 1][MOCK_BCM_MAX_PORTS];
static uint8_t g_vlan_port_trunk[MOCK_BCM_VLAN_MAX + 1][MOCK_BCM_TRUNKS_MAX + 1];
static uint8_t g_trunk_used[MOCK_BCM_TRUNKS_MAX + 1];
static uint8_t g_trunk_member[MOCK_BCM_TRUNKS_MAX + 1][MOCK_BCM_MAX_PORTS];
static uint8_t g_vswitch_used[MOCK_BCM_VSWITCH_MAX + 1];
static uint8_t g_vswitch_port[MOCK_BCM_VSWITCH_MAX + 1][MOCK_BCM_MAX_PORTS];

struct mock_l2_entry {
    uint8_t used;
    uint8_t static_entry;
    uint8_t mac[6];
    int  vid;
    int  port;
    int  tgid;
    uint8_t is_trunk;
};

static struct mock_l2_entry g_l2[MOCK_BCM_L2_ENTRIES];

/* L3 simulation state (defined in the L3 section below) */
#define MOCK_BCM_L3_EGRESS_MAX  1024
#define MOCK_BCM_L3_ECMP_MAX    256
#define MOCK_BCM_L3_ROUTE_MAX   1024
#define MOCK_BCM_L3_HOST_MAX    1024

struct mock_l3_ingress { uint8_t used; bcm_vrf_t vrf; };
struct mock_l3_egress  { uint8_t used; bcm_vlan_t vlan; int port; int trunk; uint8_t mac[6]; };
struct mock_l3_ecmp    { uint8_t used; int member[MOCK_BCM_L3_EGRESS_MAX]; int member_count; };
struct mock_l3_route   { uint8_t used; uint8_t is_v6; bcm_vrf_t vrf; uint32_t subnet; uint32_t mask; uint8_t ip6_net[16]; uint8_t ip6_mask[16]; bcm_if_t intf; };
struct mock_l3_host    { uint8_t used; uint8_t is_v6; bcm_vrf_t vrf; uint32_t ip; uint8_t ip6[16]; bcm_if_t intf; };

/* RX trap / knet / field / qos mock state (defined in the P4 section) */
#define MOCK_BCM_TRAP_MAX        128
#define MOCK_BCM_KNETIF_MAX      32
#define MOCK_BCM_FIELD_GROUP_MAX 32
#define MOCK_BCM_FIELD_ENTRY_MAX 256
#define MOCK_BCM_QOS_MAP_MAX     512

struct mock_rx_trap { uint8_t used; int type; uint32_t flags; };
struct mock_knet_netif { uint8_t used; int id; char name[16]; int vlan; int port; };
struct mock_field_entry { uint8_t used; int group_id; uint8_t installed; uint8_t has_drop;
                          uint16_t ether_type; uint8_t has_ether_type;
                          uint8_t has_src_mac; uint8_t has_dst_mac;
                          uint8_t has_in_port; uint8_t in_port; };
struct mock_field_group { uint8_t used; int id; };
struct mock_qos_map { uint8_t used; int dscp; int int_pri; };

static struct mock_rx_trap     g_rx_trap[MOCK_BCM_TRAP_MAX];
static struct mock_knet_netif  g_knetif[MOCK_BCM_KNETIF_MAX];
static struct mock_field_group g_fgroup[MOCK_BCM_FIELD_GROUP_MAX];
static struct mock_field_entry g_fentry[MOCK_BCM_FIELD_ENTRY_MAX];
static struct mock_qos_map     g_qos_map[MOCK_BCM_QOS_MAP_MAX];

static struct mock_l3_ingress g_l3_ingress[MOCK_BCM_L3_EGRESS_MAX];
static struct mock_l3_egress  g_l3_egress[MOCK_BCM_L3_EGRESS_MAX];
static struct mock_l3_ecmp    g_l3_ecmp[MOCK_BCM_L3_ECMP_MAX];
static struct mock_l3_route   g_l3_route[MOCK_BCM_L3_ROUTE_MAX];
static struct mock_l3_host    g_l3_host[MOCK_BCM_L3_HOST_MAX];

/*
 * bcm_errmsg() dereferences the _shr_errmsg[] table normally provided by
 * the OpenBCM library; supply the standard messages so the adapter's
 * logging compiles and links in mock mode.
 */
char *_shr_errmsg[] = {
    "Ok",
    "Internal error",
    "Out of memory",
    "Invalid unit",
    "Invalid parameter",
    "Table empty",
    "Table full",
    "Entry not found",
    "Entry exists",
    "Operation timed out",
    "Operation still running",
    "Operation failed",
    "Operation disabled",
    "Invalid identifier",
    "No resources for operation",
    "Invalid configuration",
    "Feature unavailable",
    "Not initialized",
    "Invalid port",
    "I/O error",
    "Permission denied",
    "No handler",
    "Partial success",
    "Limit",
};

struct mock_port_state {
    int      enabled;
    int      speed;
    int      mtu;
    int      autoneg;
    int      link_forced; /* raw link, -1 = derive from enabled */
    bcm_port_phy_fec_t fec;
    uint64_t stats[MOCK_BCM_MAX_STATS];
};

static struct {
    int                     inited;      /* bcm_init()/bcm_detach() state */
    int                     unit_inited; /* bcm_init() completed        */
    struct mock_port_state  ports[MOCK_BCM_MAX_PORTS];
    int                     stat_sync_count;
    int                     stat_clear_count[MOCK_BCM_MAX_PORTS];
} g_mock;

static struct mock_port_state *mock_port(int unit, bcm_port_t port)
{
    if (unit != 0 || port < 0 || port >= MOCK_BCM_MAX_PORTS) {
        return NULL;
    }
    return &g_mock.ports[port];
}

void mock_bcm_reset(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
    for (int i = 0; i < MOCK_BCM_MAX_PORTS; i++) {
        g_mock.ports[i].enabled = 1;
        g_mock.ports[i].speed = 10000;
        g_mock.ports[i].mtu = 1522;
        g_mock.ports[i].autoneg = 1;
        g_mock.ports[i].link_forced = -1;
        g_mock.ports[i].fec = bcmPortPhyFecNone;
    }
    memset(g_vlan_used, 0, sizeof(g_vlan_used));
    memset(g_vlan_port, 0, sizeof(g_vlan_port));
    memset(g_vlan_port_trunk, 0, sizeof(g_vlan_port_trunk));
    memset(g_trunk_used, 0, sizeof(g_trunk_used));
    memset(g_trunk_member, 0, sizeof(g_trunk_member));
    memset(g_l2, 0, sizeof(g_l2));
    memset(g_vswitch_used, 0, sizeof(g_vswitch_used));
    memset(g_vswitch_port, 0, sizeof(g_vswitch_port));
    memset(g_l3_ingress, 0, sizeof(g_l3_ingress));
    memset(g_l3_egress, 0, sizeof(g_l3_egress));
    memset(g_l3_ecmp, 0, sizeof(g_l3_ecmp));
    memset(g_l3_route, 0, sizeof(g_l3_route));
    memset(g_l3_host, 0, sizeof(g_l3_host));
    memset(g_rx_trap, 0, sizeof(g_rx_trap));
    memset(g_knetif, 0, sizeof(g_knetif));
    memset(g_fgroup, 0, sizeof(g_fgroup));
    memset(g_fentry, 0, sizeof(g_fentry));
    memset(g_qos_map, 0, sizeof(g_qos_map));
}

int mock_bcm_init_count(void)
{
    return g_mock.unit_inited;
}

void mock_bcm_set_link(int port, int up)
{
    if (port >= 0 && port < MOCK_BCM_MAX_PORTS) {
        g_mock.ports[port].link_forced = up ? 1 : 0;
    }
}

void mock_bcm_set_stat(int port, bcm_stat_val_t type, uint64_t value)
{
    if (port >= 0 && port < MOCK_BCM_MAX_PORTS &&
        (int)type >= 0 && (int)type < MOCK_BCM_MAX_STATS) {
        g_mock.ports[port].stats[type] = value;
    }
}

uint64_t mock_bcm_get_stat(int port, bcm_stat_val_t type)
{
    if (port >= 0 && port < MOCK_BCM_MAX_PORTS &&
        (int)type >= 0 && (int)type < MOCK_BCM_MAX_STATS) {
        return g_mock.ports[port].stats[type];
    }
    return 0;
}

int mock_bcm_stat_sync_count(void)
{
    return g_mock.stat_sync_count;
}

int mock_bcm_stat_clear_count(int port)
{
    if (port >= 0 && port < MOCK_BCM_MAX_PORTS) {
        return g_mock.stat_clear_count[port];
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * OpenBCM API simulation
 * --------------------------------------------------------------------------- */
int bcm_init(int unit)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (g_mock.inited) {
        return BCM_E_INIT;
    }
    g_mock.inited = 1;
    g_mock.unit_inited++;
    return BCM_E_NONE;
}

int bcm_detach(int unit)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    g_mock.inited = 0;
    return BCM_E_NONE;
}

int bcm_init_check(int unit)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    return g_mock.inited ? BCM_E_NONE : BCM_E_INIT;
}

int bcm_port_config_get(int unit, bcm_port_config_t *config)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (!g_mock.inited || config == NULL) {
        return BCM_E_INIT;
    }

    memset(config, 0, sizeof(*config));

    for (int p = 1; p <= MOCK_BCM_FRONT_PANEL_PORTS; p++) {
        BCM_PBMP_PORT_ADD(config->xe, p);
        BCM_PBMP_PORT_ADD(config->port, p);
        BCM_PBMP_PORT_ADD(config->all, p);
    }
    BCM_PBMP_PORT_ADD(config->cpu, MOCK_BCM_CPU_PORT);
    BCM_PBMP_PORT_ADD(config->all, MOCK_BCM_CPU_PORT);

    return BCM_E_NONE;
}

int bcm_port_enable_set(int unit, bcm_port_t port, int enable)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    p->enabled = enable ? 1 : 0;
    return BCM_E_NONE;
}

int bcm_port_enable_get(int unit, bcm_port_t port, int *enable)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (enable == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    *enable = p->enabled;
    return BCM_E_NONE;
}

int bcm_port_speed_set(int unit, bcm_port_t port, int speed)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if (speed != 1000 && speed != 10000 && speed != 25000 &&
        speed != 40000 && speed != 50000 && speed != 100000) {
        return BCM_E_PARAM;
    }
    p->speed = speed;
    return BCM_E_NONE;
}

int bcm_port_speed_get(int unit, bcm_port_t port, int *speed)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (speed == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    *speed = p->speed;
    return BCM_E_NONE;
}

int bcm_port_speed_max(int unit, bcm_port_t port, int *speed)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (speed == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    *speed = 100000;
    return BCM_E_NONE;
}

int bcm_port_autoneg_set(int unit, bcm_port_t port, int autoneg)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    p->autoneg = autoneg ? 1 : 0;
    return BCM_E_NONE;
}

int bcm_port_autoneg_get(int unit, bcm_port_t port, int *autoneg)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (autoneg == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    *autoneg = p->autoneg;
    return BCM_E_NONE;
}

int bcm_port_link_status_get(int unit, bcm_port_t port, int *status)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (status == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if (p->link_forced >= 0) {
        *status = p->link_forced;
    } else {
        *status = p->enabled ? 1 : 0;
    }
    return BCM_E_NONE;
}

int bcm_port_frame_max_set(int unit, bcm_port_t port, int size)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if (size < 64 || size > 9216) {
        return BCM_E_PARAM;
    }
    p->mtu = size;
    return BCM_E_NONE;
}

int bcm_port_frame_max_get(int unit, bcm_port_t port, int *size)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (size == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    *size = p->mtu;
    return BCM_E_NONE;
}

int bcm_port_resource_set(int unit, bcm_gport_t port,
                         bcm_port_resource_t *resource)
{
    struct mock_port_state *p = mock_port(unit, (bcm_port_t)port);
    if (resource == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if (resource->speed > 0) {
        p->speed = resource->speed;
    }
    if (resource->fec_type != bcmPortPhyFecDefaultRequest) {
        p->fec = resource->fec_type;
    }
    return BCM_E_NONE;
}

int bcm_port_resource_get(int unit, bcm_gport_t port,
                         bcm_port_resource_t *resource)
{
    struct mock_port_state *p = mock_port(unit, (bcm_port_t)port);
    if (resource == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    memset(resource, 0, sizeof(*resource));
    resource->port = port;
    resource->speed = p->speed;
    resource->fec_type = p->fec;
    return BCM_E_NONE;
}

int bcm_stat_get(int unit, bcm_port_t port, bcm_stat_val_t type, uint64 *value)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (value == NULL) {
        return BCM_E_PARAM;
    }
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if ((int)type < 0 || (int)type >= MOCK_BCM_MAX_STATS) {
        return BCM_E_PARAM;
    }
    *value = p->stats[type];
    return BCM_E_NONE;
}

int bcm_stat_clear(int unit, bcm_port_t port)
{
    struct mock_port_state *p = mock_port(unit, port);
    if (p == NULL) {
        return BCM_E_PORT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    memset(p->stats, 0, sizeof(p->stats));
    g_mock.stat_clear_count[port]++;
    return BCM_E_NONE;
}

int bcm_stat_sync(int unit)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    g_mock.stat_sync_count++;
    return BCM_E_NONE;
}
/* =========================================================================
 * VLAN simulation
 * ========================================================================= */
/* Decode a gport to a mock logical port id: accepts raw port numbers and
 * BCM_GPORT_LOCAL_SET-encoded values (the adapter uses the latter). */
static int VPORT(bcm_gport_t g)
{
    if (g >= 0 && g < MOCK_BCM_MAX_PORTS) {
        return (int)g;
    }
    if (BCM_GPORT_IS_LOCAL(g)) {
        return BCM_GPORT_LOCAL_GET(g);
    }
    return -1;
}

/* g_vlan_used / g_vlan_port declared above (cleared by mock_bcm_reset) */

int bcm_vlan_create(int unit, bcm_vlan_t vid)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (vid < 1 || vid > MOCK_BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if (g_vlan_used[vid]) {
        return BCM_E_EXISTS;
    }
    g_vlan_used[vid] = 1;
    return BCM_E_NONE;
}

int bcm_vlan_destroy(int unit, bcm_vlan_t vid)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (vid < 1 || vid > MOCK_BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if (!g_vlan_used[vid]) {
        return BCM_E_NOT_FOUND;
    }
    memset(g_vlan_port[vid], 0, sizeof(g_vlan_port[vid]));
    g_vlan_used[vid] = 0;
    return BCM_E_NONE;
}

int bcm_vlan_gport_add(int unit, bcm_vlan_t vlan, bcm_gport_t port,
                      int flags)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (vlan < 1 || vlan > MOCK_BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_vlan_used[vlan]) {
        return BCM_E_NOT_FOUND;
    }
    uint8_t mode = (flags & BCM_VLAN_GPORT_ADD_UNTAGGED) ? 1 : 2;
    if (BCM_GPORT_IS_TRUNK(port)) {
        int t = BCM_GPORT_TRUNK_GET(port);
        if (t < 0 || t > MOCK_BCM_TRUNKS_MAX) {
            return BCM_E_PARAM;
        }
        g_vlan_port_trunk[vlan][t] = mode;
        return BCM_E_NONE;
    }
    int p = VPORT(port);
    if (p < 0) {
        return BCM_E_PARAM;
    }
    g_vlan_port[vlan][p] = mode;
    return BCM_E_NONE;
}

int bcm_vlan_gport_delete(int unit, bcm_vlan_t vlan, bcm_gport_t port)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (vlan < 1 || vlan > MOCK_BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_vlan_used[vlan]) {
        return BCM_E_NOT_FOUND;
    }
    if (BCM_GPORT_IS_TRUNK(port)) {
        int t = BCM_GPORT_TRUNK_GET(port);
        if (t < 0 || t > MOCK_BCM_TRUNKS_MAX) {
            return BCM_E_PARAM;
        }
        if (g_vlan_port_trunk[vlan][t] == 0) {
            return BCM_E_NOT_FOUND;
        }
        g_vlan_port_trunk[vlan][t] = 0;
        return BCM_E_NONE;
    }
    int p = VPORT(port);
    if (p < 0) {
        return BCM_E_PARAM;
    }
    if (g_vlan_port[vlan][p] == 0) {
        return BCM_E_NOT_FOUND;
    }
    g_vlan_port[vlan][p] = 0;
    return BCM_E_NONE;
}

int mock_bcm_vlan_used(int vid)
{
    return (vid >= 1 && vid <= MOCK_BCM_VLAN_MAX) ? g_vlan_used[vid] : 0;
}

int mock_bcm_vlan_member_count(int vid)
{
    int n = 0;
    if (vid < 1 || vid > MOCK_BCM_VLAN_MAX) {
        return 0;
    }
    for (int p = 0; p < MOCK_BCM_MAX_PORTS; p++) {
        if (g_vlan_port[vid][p]) {
            n++;
        }
    }
    return n;
}

int mock_bcm_vlan_member_mode(int vid, int gport)
{
    int p = VPORT(gport);
    if (vid < 1 || vid > MOCK_BCM_VLAN_MAX || p < 0) {
        return 0;
    }
    return g_vlan_port[vid][p];
}

/* =========================================================================
 * Trunk simulation
 * ========================================================================= */
#define MOCK_BCM_TRUNK_MAX MOCK_BCM_TRUNKS_MAX

void bcm_trunk_member_t_init(bcm_trunk_member_t *member)
{
    if (member == NULL) {
        return;
    }
    memset(member, 0, sizeof(*member));
}

int bcm_trunk_create(int unit, uint32 flags, bcm_trunk_t *tid)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (tid == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    (void)flags;
    for (int t = 0; t <= MOCK_BCM_TRUNK_MAX; t++) {
        if (!g_trunk_used[t]) {
            g_trunk_used[t] = 1;
            *tid = (bcm_trunk_t)t;
            return BCM_E_NONE;
        }
    }
    return BCM_E_FULL;
}

int bcm_trunk_destroy(int unit, bcm_trunk_t tid)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (tid < 0 || tid > MOCK_BCM_TRUNK_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if (!g_trunk_used[tid]) {
        return BCM_E_NOT_FOUND;
    }
    memset(g_trunk_member[tid], 0, sizeof(g_trunk_member[tid]));
    g_trunk_used[tid] = 0;
    return BCM_E_NONE;
}

int bcm_trunk_member_add(int unit, bcm_trunk_t tid,
                        bcm_trunk_member_t *member)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (tid < 0 || tid > MOCK_BCM_TRUNK_MAX || member == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_trunk_used[tid]) {
        return BCM_E_NOT_FOUND;
    }
    int p = VPORT(member->gport);
    if (p < 0) {
        return BCM_E_PARAM;
    }
    g_trunk_member[tid][p] = 1;
    return BCM_E_NONE;
}

int bcm_trunk_member_delete(int unit, bcm_trunk_t tid,
                           bcm_trunk_member_t *member)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (tid < 0 || tid > MOCK_BCM_TRUNK_MAX || member == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_trunk_used[tid]) {
        return BCM_E_NOT_FOUND;
    }
    int p = VPORT(member->gport);
    if (p < 0 || !g_trunk_member[tid][p]) {
        return BCM_E_NOT_FOUND;
    }
    g_trunk_member[tid][p] = 0;
    return BCM_E_NONE;
}

int mock_bcm_trunk_used(int tid)
{
    return (tid >= 0 && tid <= MOCK_BCM_TRUNK_MAX) ? g_trunk_used[tid] : 0;
}

int mock_bcm_trunk_member_count(int tid)
{
    int n = 0;
    if (tid < 0 || tid > MOCK_BCM_TRUNK_MAX || !g_trunk_used[tid]) {
        return 0;
    }
    for (int p = 0; p < MOCK_BCM_MAX_PORTS; p++) {
        if (g_trunk_member[tid][p]) {
            n++;
        }
    }
    return n;
}

int mock_bcm_trunk_has_member(int tid, int gport)
{
    int p = VPORT(gport);
    if (tid < 0 || tid > MOCK_BCM_TRUNK_MAX || p < 0) {
        return 0;
    }
    return g_trunk_member[tid][p];
}

/* =========================================================================
 * L2 (FDB) simulation
 * ========================================================================= */
#define MOCK_BCM_L2_MAX MOCK_BCM_L2_ENTRIES

static struct mock_l2_entry *l2_find(const uint8_t mac[6], int vid)
{
    for (int i = 0; i < MOCK_BCM_L2_MAX; i++) {
        if (g_l2[i].used && g_l2[i].vid == vid &&
            memcmp(g_l2[i].mac, mac, 6) == 0) {
            return &g_l2[i];
        }
    }
    return NULL;
}

void bcm_l2_addr_t_init(bcm_l2_addr_t *l2addr, const bcm_mac_t mac,
                        bcm_vlan_t vid)
{
    if (l2addr == NULL) {
        return;
    }
    memset(l2addr, 0, sizeof(*l2addr));
    if (mac != NULL) {
        memcpy(l2addr->mac, mac, 6);
    }
    l2addr->vid = vid;
}

int bcm_l2_addr_add(int unit, bcm_l2_addr_t *l2addr)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (l2addr == NULL || l2addr->vid < 1 ||
        l2addr->vid > MOCK_BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_vlan_used[l2addr->vid]) {
        return BCM_E_NOT_FOUND;
    }
    struct mock_l2_entry *e = l2_find(l2addr->mac, l2addr->vid);
    if (e == NULL) {
        for (int i = 0; i < MOCK_BCM_L2_MAX; i++) {
            if (!g_l2[i].used) {
                e = &g_l2[i];
                break;
            }
        }
    }
    if (e == NULL) {
        return BCM_E_FULL;
    }
    e->used = 1;
    memcpy(e->mac, l2addr->mac, 6);
    e->vid = l2addr->vid;
    e->port = l2addr->port;
    e->tgid = l2addr->tgid;
    e->is_trunk = (l2addr->flags & BCM_L2_TRUNK_MEMBER) ? 1 : 0;
    e->static_entry = (l2addr->flags & BCM_L2_STATIC) ? 1 : 0;
    return BCM_E_NONE;
}

static void l2_delete(struct mock_l2_entry *e)
{
    memset(e, 0, sizeof(*e));
}

int bcm_l2_addr_delete(int unit, bcm_mac_t mac, bcm_vlan_t vid)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    struct mock_l2_entry *e = l2_find(mac, vid);
    if (e == NULL) {
        return BCM_E_NOT_FOUND;
    }
    l2_delete(e);
    return BCM_E_NONE;
}

int bcm_l2_addr_get(int unit, bcm_mac_t mac_addr, bcm_vlan_t vid,
                   bcm_l2_addr_t *l2addr)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (l2addr == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    struct mock_l2_entry *e = l2_find(mac_addr, vid);
    if (e == NULL) {
        return BCM_E_NOT_FOUND;
    }
    memset(l2addr, 0, sizeof(*l2addr));
    memcpy(l2addr->mac, e->mac, 6);
    l2addr->vid = e->vid;
    l2addr->port = e->port;
    l2addr->tgid = e->tgid;
    if (e->is_trunk) {
        l2addr->flags |= BCM_L2_TRUNK_MEMBER;
    }
    if (e->static_entry) {
        l2addr->flags |= BCM_L2_STATIC;
    }
    return BCM_E_NONE;
}

int bcm_l2_addr_delete_by_vlan(int unit, bcm_vlan_t vid, uint32 flags)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (vid < 1 || vid > MOCK_BCM_VLAN_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    (void)flags;
    for (int i = 0; i < MOCK_BCM_L2_MAX; i++) {
        if (g_l2[i].used && g_l2[i].vid == vid) {
            l2_delete(&g_l2[i]);
        }
    }
    return BCM_E_NONE;
}

int bcm_l2_addr_delete_by_port(int unit, bcm_module_t mod,
                              bcm_port_t port, uint32 flags)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (mod != 0) {
        return BCM_E_PARAM;
    }
    int p = VPORT(port);
    if (p < 0) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    (void)flags;
    for (int i = 0; i < MOCK_BCM_L2_MAX; i++) {
        if (g_l2[i].used && !g_l2[i].is_trunk && g_l2[i].port == p) {
            l2_delete(&g_l2[i]);
        }
    }
    return BCM_E_NONE;
}

int bcm_l2_addr_delete_by_trunk(int unit, bcm_trunk_t tid, uint32 flags)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (tid < 0 || tid > MOCK_BCM_TRUNK_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    (void)flags;
    for (int i = 0; i < MOCK_BCM_L2_MAX; i++) {
        if (g_l2[i].used && g_l2[i].is_trunk && g_l2[i].tgid == tid) {
            l2_delete(&g_l2[i]);
        }
    }
    return BCM_E_NONE;
}

int mock_bcm_l2_count(void)
{
    int n = 0;
    for (int i = 0; i < MOCK_BCM_L2_MAX; i++) {
        if (g_l2[i].used) {
            n++;
        }
    }
    return n;
}

int mock_bcm_l2_is_static(int port, const uint8_t mac[6], int vid)
{
    (void)port;
    struct mock_l2_entry *e = l2_find((uint8_t *)mac, vid);
    return (e != NULL && e->static_entry) ? 1 : 0;
}

int mock_bcm_l2_dest_is_trunk(int port, const uint8_t mac[6], int vid,
                              int *tgid)
{
    (void)port;
    struct mock_l2_entry *e = l2_find((uint8_t *)mac, vid);
    if (e == NULL) {
        return -1;
    }
    if (tgid != NULL) {
        *tgid = e->tgid;
    }
    return e->is_trunk;
}

/* =========================================================================
 * vswitch simulation (for 1D bridges)
 * ========================================================================= */
/* MOCK_BCM_VSWITCH_MAX defined above mock_bcm_reset */

int bcm_vswitch_create(int unit, bcm_vlan_t *vsi)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (vsi == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    for (int t = 0; t <= MOCK_BCM_VSWITCH_MAX; t++) {
        if (!g_vswitch_used[t]) {
            g_vswitch_used[t] = 1;
            *vsi = (bcm_vlan_t)t;
            return BCM_E_NONE;
        }
    }
    return BCM_E_FULL;
}

int bcm_vswitch_destroy(int unit, bcm_vlan_t vsi)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (vsi < 0 || vsi > MOCK_BCM_VSWITCH_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    if (!g_vswitch_used[vsi]) {
        return BCM_E_NOT_FOUND;
    }
    memset(g_vswitch_port[vsi], 0, sizeof(g_vswitch_port[vsi]));
    g_vswitch_used[vsi] = 0;
    return BCM_E_NONE;
}

int bcm_vswitch_port_add(int unit, bcm_vlan_t vsi, bcm_gport_t port)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    int p = VPORT(port);
    if (vsi < 0 || vsi > MOCK_BCM_VSWITCH_MAX || p < 0) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_vswitch_used[vsi]) {
        return BCM_E_NOT_FOUND;
    }
    g_vswitch_port[vsi][p] = 1;
    return BCM_E_NONE;
}

int bcm_vswitch_port_delete(int unit, bcm_vlan_t vsi, bcm_gport_t port)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    int p = VPORT(port);
    if (vsi < 0 || vsi > MOCK_BCM_VSWITCH_MAX || p < 0) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_vswitch_used[vsi]) {
        return BCM_E_NOT_FOUND;
    }
    g_vswitch_port[vsi][p] = 0;
    return BCM_E_NONE;
}

int mock_bcm_vswitch_used(int vsi)
{
    return (vsi >= 0 && vsi <= MOCK_BCM_VSWITCH_MAX) ? g_vswitch_used[vsi] : 0;
}

int mock_bcm_vswitch_member_count(int vsi)
{
    int n = 0;
    if (vsi < 0 || vsi > MOCK_BCM_VSWITCH_MAX || !g_vswitch_used[vsi]) {
        return 0;
    }
    for (int p = 0; p < MOCK_BCM_MAX_PORTS; p++) {
        if (g_vswitch_port[vsi][p]) {
            n++;
        }
    }
    return n;
}

/* =========================================================================
 * L3 simulation (ingress if, egress objects, ECMP, routes, hosts)
 * ========================================================================= */
static int mock_id_alloc(void *arr, size_t elem, int max, uint8_t *used_off)
{
    (void)arr; (void)elem;
    for (int i = 0; i < max; i++) {
        uint8_t *u = (uint8_t *)((char *)arr + (size_t)i * elem + *used_off);
        if (!*u) {
            *u = 1;
            return i;
        }
    }
    return -1;
}

void bcm_l3_ingress_t_init(bcm_l3_ingress_t *ing)
{
    if (ing == NULL) {
        return;
    }
    memset(ing, 0, sizeof(*ing));
}

int bcm_l3_ingress_create(int unit, bcm_l3_ingress_t *ing,
                          bcm_if_t *intf_id)
{
    if (unit != 0 || ing == NULL || intf_id == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    size_t elem = sizeof(g_l3_ingress[0]);
    uint8_t off = offsetof(struct mock_l3_ingress, used);
    int id = mock_id_alloc(g_l3_ingress, elem, MOCK_BCM_L3_EGRESS_MAX, &off);
    if (id < 0) {
        return BCM_E_FULL;
    }
    g_l3_ingress[id].vrf = ing->vrf;
    *intf_id = (bcm_if_t)id;
    return BCM_E_NONE;
}

int bcm_l3_ingress_destroy(int unit, bcm_if_t intf_id)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (intf_id < 0 || intf_id >= MOCK_BCM_L3_EGRESS_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_l3_ingress[intf_id].used) {
        return BCM_E_NOT_FOUND;
    }
    memset(&g_l3_ingress[intf_id], 0, sizeof(g_l3_ingress[intf_id]));
    return BCM_E_NONE;
}

int mock_bcm_l3_ingress_exists(int intf_id)
{
    return (intf_id >= 0 && intf_id < MOCK_BCM_L3_EGRESS_MAX)
               ? g_l3_ingress[intf_id].used
               : 0;
}

void bcm_l3_egress_t_init(bcm_l3_egress_t *egr)
{
    if (egr == NULL) {
        return;
    }
    memset(egr, 0, sizeof(*egr));
}

int bcm_l3_egress_create(int unit, uint32 flags, bcm_l3_egress_t *egr,
                         bcm_if_t *if_id)
{
    if (unit != 0 || egr == NULL || if_id == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    int id = (int)*if_id;
    if (flags & BCM_L3_REPLACE) {
        /* In-place re-program: the caller passes the existing object id. */
        if (id < 0 || id >= MOCK_BCM_L3_EGRESS_MAX || !g_l3_egress[id].used) {
            return BCM_E_NOT_FOUND;
        }
    } else {
        size_t elem = sizeof(g_l3_egress[0]);
        uint8_t off = offsetof(struct mock_l3_egress, used);
        id = mock_id_alloc(g_l3_egress, elem, MOCK_BCM_L3_EGRESS_MAX, &off);
        if (id < 0) {
            return BCM_E_FULL;
        }
    }
    g_l3_egress[id].vlan = egr->vlan;
    if (egr->flags & BCM_L3_TGID) {
        g_l3_egress[id].port = -1;
        g_l3_egress[id].trunk = egr->trunk;
    } else {
        g_l3_egress[id].port = egr->port;
        g_l3_egress[id].trunk = -1;
    }
    memcpy(g_l3_egress[id].mac, egr->mac_addr, 6);
    *if_id = (bcm_if_t)id;
    return BCM_E_NONE;
}

int bcm_l3_egress_destroy(int unit, bcm_if_t if_id)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (if_id < 0 || if_id >= MOCK_BCM_L3_EGRESS_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_l3_egress[if_id].used) {
        return BCM_E_NOT_FOUND;
    }
    memset(&g_l3_egress[if_id], 0, sizeof(g_l3_egress[if_id]));
    return BCM_E_NONE;
}

int mock_bcm_l3_egress_exists(int if_id)
{
    return (if_id >= 0 && if_id < MOCK_BCM_L3_EGRESS_MAX)
               ? g_l3_egress[if_id].used
               : 0;
}

int mock_bcm_l3_egress_count(void)
{
    int n = 0;
    for (int i = 0; i < MOCK_BCM_L3_EGRESS_MAX; i++) {
        if (g_l3_egress[i].used) {
            n++;
        }
    }
    return n;
}

int mock_bcm_l3_egress_vlan(int if_id)
{
    if (if_id >= 0 && if_id < MOCK_BCM_L3_EGRESS_MAX &&
        g_l3_egress[if_id].used) {
        return (int)g_l3_egress[if_id].vlan;
    }
    return -1;
}

int mock_bcm_l3_egress_mac(int if_id, uint8_t mac_out[6])
{
    if (if_id >= 0 && if_id < MOCK_BCM_L3_EGRESS_MAX &&
        g_l3_egress[if_id].used && mac_out != NULL) {
        memcpy(mac_out, g_l3_egress[if_id].mac, 6);
        return 1;
    }
    return 0;
}

int mock_bcm_l3_egress_is_trunk(int if_id)
{
    if (if_id >= 0 && if_id < MOCK_BCM_L3_EGRESS_MAX &&
        g_l3_egress[if_id].used) {
        return g_l3_egress[if_id].port == -1 ? 1 : 0;
    }
    return -1;
}

void bcm_l3_egress_ecmp_t_init(bcm_l3_egress_ecmp_t *ecmp)
{
    if (ecmp == NULL) {
        return;
    }
    memset(ecmp, 0, sizeof(*ecmp));
}

int bcm_l3_egress_ecmp_create(int unit, bcm_l3_egress_ecmp_t *ecmp,
                              int intf_count, bcm_if_t *intf_array)
{
    if (unit != 0 || ecmp == NULL || intf_count < 0 ||
        (intf_count > 0 && intf_array == NULL)) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    for (int t = 0; t < MOCK_BCM_L3_ECMP_MAX; t++) {
        if (!g_l3_ecmp[t].used) {
            g_l3_ecmp[t].used = 1;
            g_l3_ecmp[t].member_count = 0;
            for (int i = 0; i < intf_count; i++) {
                int id = (int)intf_array[i];
                if (id < 0 || id >= MOCK_BCM_L3_EGRESS_MAX ||
                    !g_l3_egress[id].used) {
                    return BCM_E_PARAM;
                }
                g_l3_ecmp[t].member[id] = 1;
                g_l3_ecmp[t].member_count++;
            }
            ecmp->ecmp_intf = (bcm_if_t)t;
            return BCM_E_NONE;
        }
    }
    return BCM_E_FULL;
}

int bcm_l3_egress_ecmp_destroy(int unit, bcm_l3_egress_ecmp_t *ecmp)
{
    if (unit != 0 || ecmp == NULL) {
        return BCM_E_PARAM;
    }
    int t = (int)ecmp->ecmp_intf;
    if (t < 0 || t >= MOCK_BCM_L3_ECMP_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_l3_ecmp[t].used) {
        return BCM_E_NOT_FOUND;
    }
    memset(&g_l3_ecmp[t], 0, sizeof(g_l3_ecmp[t]));
    return BCM_E_NONE;
}

int bcm_l3_egress_ecmp_add(int unit, bcm_l3_egress_ecmp_t *ecmp,
                          bcm_if_t intf)
{
    if (unit != 0 || ecmp == NULL) {
        return BCM_E_PARAM;
    }
    int t = (int)ecmp->ecmp_intf;
    int id = (int)intf;
    if (t < 0 || t >= MOCK_BCM_L3_ECMP_MAX || id < 0 ||
        id >= MOCK_BCM_L3_EGRESS_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_l3_ecmp[t].used || !g_l3_egress[id].used) {
        return BCM_E_NOT_FOUND;
    }
    if (g_l3_ecmp[t].member[id]) {
        return BCM_E_EXISTS;
    }
    g_l3_ecmp[t].member[id] = 1;
    g_l3_ecmp[t].member_count++;
    return BCM_E_NONE;
}

int bcm_l3_egress_ecmp_delete(int unit, bcm_l3_egress_ecmp_t *ecmp,
                             bcm_if_t intf)
{
    if (unit != 0 || ecmp == NULL) {
        return BCM_E_PARAM;
    }
    int t = (int)ecmp->ecmp_intf;
    int id = (int)intf;
    if (t < 0 || t >= MOCK_BCM_L3_ECMP_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_l3_ecmp[t].used) {
        return BCM_E_NOT_FOUND;
    }
    if (!g_l3_ecmp[t].member[id]) {
        return BCM_E_NOT_FOUND;
    }
    g_l3_ecmp[t].member[id] = 0;
    g_l3_ecmp[t].member_count--;
    return BCM_E_NONE;
}

int mock_bcm_l3_ecmp_exists(int intf_id)
{
    return (intf_id >= 0 && intf_id < MOCK_BCM_L3_ECMP_MAX)
               ? g_l3_ecmp[intf_id].used
               : 0;
}

int mock_bcm_l3_ecmp_member_count(int intf_id)
{
    if (intf_id >= 0 && intf_id < MOCK_BCM_L3_ECMP_MAX &&
        g_l3_ecmp[intf_id].used) {
        return g_l3_ecmp[intf_id].member_count;
    }
    return -1;
}

void bcm_l3_route_t_init(bcm_l3_route_t *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
}

static struct mock_l3_route *l3_route_find(const bcm_l3_route_t *info)
{
    for (int i = 0; i < MOCK_BCM_L3_ROUTE_MAX; i++) {
        struct mock_l3_route *r = &g_l3_route[i];
        if (!r->used || r->vrf != info->l3a_vrf) {
            continue;
        }
        if ((info->l3a_flags & BCM_L3_IP6) != (r->is_v6 ? BCM_L3_IP6 : 0)) {
            continue;
        }
        if (info->l3a_flags & BCM_L3_IP6) {
            if (memcmp(r->ip6_net, info->l3a_ip6_net, 16) == 0 &&
                memcmp(r->ip6_mask, info->l3a_ip6_mask, 16) == 0) {
                return r;
            }
        } else {
            if (r->subnet == info->l3a_subnet && r->mask == info->l3a_ip_mask) {
                return r;
            }
        }
    }
    return NULL;
}

int bcm_l3_route_add(int unit, bcm_l3_route_t *info)
{
    if (unit != 0 || info == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    struct mock_l3_route *r = l3_route_find(info);
    if (r == NULL) {
        for (int i = 0; i < MOCK_BCM_L3_ROUTE_MAX; i++) {
            if (!g_l3_route[i].used) {
                r = &g_l3_route[i];
                break;
            }
        }
    }
    if (r == NULL) {
        return BCM_E_FULL;
    }
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->is_v6 = (info->l3a_flags & BCM_L3_IP6) ? 1 : 0;
    r->vrf = info->l3a_vrf;
    r->subnet = info->l3a_subnet;
    r->mask = info->l3a_ip_mask;
    memcpy(r->ip6_net, info->l3a_ip6_net, 16);
    memcpy(r->ip6_mask, info->l3a_ip6_mask, 16);
    r->intf = info->l3a_intf;
    return BCM_E_NONE;
}

int bcm_l3_route_delete(int unit, bcm_l3_route_t *info)
{
    if (unit != 0 || info == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    struct mock_l3_route *r = l3_route_find(info);
    if (r == NULL) {
        return BCM_E_NOT_FOUND;
    }
    memset(r, 0, sizeof(*r));
    return BCM_E_NONE;
}

int mock_bcm_l3_route_count(int vrf)
{
    int n = 0;
    for (int i = 0; i < MOCK_BCM_L3_ROUTE_MAX; i++) {
        if (g_l3_route[i].used && g_l3_route[i].vrf == vrf) {
            n++;
        }
    }
    return n;
}

void bcm_l3_host_t_init(bcm_l3_host_t *host)
{
    if (host == NULL) {
        return;
    }
    memset(host, 0, sizeof(*host));
}

static struct mock_l3_host *l3_host_find(const bcm_l3_host_t *host)
{
    for (int i = 0; i < MOCK_BCM_L3_HOST_MAX; i++) {
        struct mock_l3_host *h = &g_l3_host[i];
        if (!h->used || h->vrf != host->l3a_vrf) {
            continue;
        }
        if ((host->l3a_flags & BCM_L3_IP6) != (h->is_v6 ? BCM_L3_IP6 : 0)) {
            continue;
        }
        if (host->l3a_flags & BCM_L3_IP6) {
            if (memcmp(h->ip6, host->l3a_ip6_addr, 16) == 0) {
                return h;
            }
        } else {
            if (h->ip == host->l3a_ip_addr) {
                return h;
            }
        }
    }
    return NULL;
}

int bcm_l3_host_add(int unit, bcm_l3_host_t *info)
{
    if (unit != 0 || info == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    struct mock_l3_host *h = l3_host_find(info);
    if (h == NULL) {
        for (int i = 0; i < MOCK_BCM_L3_HOST_MAX; i++) {
            if (!g_l3_host[i].used) {
                h = &g_l3_host[i];
                break;
            }
        }
    }
    if (h == NULL) {
        return BCM_E_FULL;
    }
    memset(h, 0, sizeof(*h));
    h->used = 1;
    h->is_v6 = (info->l3a_flags & BCM_L3_IP6) ? 1 : 0;
    h->vrf = info->l3a_vrf;
    h->ip = info->l3a_ip_addr;
    memcpy(h->ip6, info->l3a_ip6_addr, 16);
    h->intf = info->l3a_intf;
    return BCM_E_NONE;
}

int bcm_l3_host_delete(int unit, bcm_l3_host_t *info)
{
    if (unit != 0 || info == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    struct mock_l3_host *h = l3_host_find(info);
    if (h == NULL) {
        return BCM_E_NOT_FOUND;
    }
    memset(h, 0, sizeof(*h));
    return BCM_E_NONE;
}

int mock_bcm_l3_host_count(int vrf)
{
    int n = 0;
    for (int i = 0; i < MOCK_BCM_L3_HOST_MAX; i++) {
        if (g_l3_host[i].used && g_l3_host[i].vrf == vrf) {
            n++;
        }
    }
    return n;
}

/* =========================================================================
 * RX trap / KNET / field (ACL) / QoS map simulation
 * ========================================================================= */

/* SAL declares sal_memset weak (sal/core/libc.h); the SDK library normally
 * provides it. Supply the trivial definition for mock builds. */
void *sal_memset(void *s, int c, size_t n)
{
    return memset(s, c, n);
}
#include <bcm/rx.h>
#include <bcm/knet.h>
#include <bcm/field.h>
#include <bcm/qos.h>

void bcm_rx_trap_config_t_init(bcm_rx_trap_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
}

int bcm_rx_trap_type_create(int unit, int flags, bcm_rx_trap_t type,
                           int *trap_id)
{
    if (unit != 0 || trap_id == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    (void)flags;
    for (int t = 0; t < MOCK_BCM_TRAP_MAX; t++) {
        if (!g_rx_trap[t].used) {
            g_rx_trap[t].used = 1;
            g_rx_trap[t].type = type;
            g_rx_trap[t].flags = 0;
            *trap_id = t;
            return BCM_E_NONE;
        }
    }
    return BCM_E_FULL;
}

int bcm_rx_trap_type_destroy(int unit, int trap_id)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (trap_id < 0 || trap_id >= MOCK_BCM_TRAP_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_rx_trap[trap_id].used) {
        return BCM_E_NOT_FOUND;
    }
    memset(&g_rx_trap[trap_id], 0, sizeof(g_rx_trap[trap_id]));
    return BCM_E_NONE;
}

int bcm_rx_trap_set(int unit, int trap_id, bcm_rx_trap_config_t *config)
{
    if (unit != 0 || config == NULL) {
        return BCM_E_PARAM;
    }
    if (trap_id < 0 || trap_id >= MOCK_BCM_TRAP_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_rx_trap[trap_id].used) {
        return BCM_E_NOT_FOUND;
    }
    g_rx_trap[trap_id].flags = config->flags;
    return BCM_E_NONE;
}

int mock_bcm_trap_used(int trap_id)
{
    return (trap_id >= 0 && trap_id < MOCK_BCM_TRAP_MAX)
               ? g_rx_trap[trap_id].used
               : 0;
}

int mock_bcm_trap_count(void)
{
    int n = 0;
    for (int t = 0; t < MOCK_BCM_TRAP_MAX; t++) {
        if (g_rx_trap[t].used) {
            n++;
        }
    }
    return n;
}

void bcm_knet_netif_t_init(bcm_knet_netif_t *netif)
{
    if (netif == NULL) {
        return;
    }
    memset(netif, 0, sizeof(*netif));
}

int bcm_knet_netif_create(int unit, bcm_knet_netif_t *netif)
{
    if (unit != 0 || netif == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    for (int t = 0; t < MOCK_BCM_KNETIF_MAX; t++) {
        if (!g_knetif[t].used) {
            g_knetif[t].used = 1;
            g_knetif[t].id = t;
            snprintf(g_knetif[t].name, sizeof(g_knetif[t].name), "%s",
                     netif->name);
            g_knetif[t].vlan = netif->vlan;
            g_knetif[t].port = netif->port;
            netif->id = t;
            return BCM_E_NONE;
        }
    }
    return BCM_E_FULL;
}

int bcm_knet_netif_destroy(int unit, int netif_id)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (netif_id < 0 || netif_id >= MOCK_BCM_KNETIF_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_knetif[netif_id].used) {
        return BCM_E_NOT_FOUND;
    }
    memset(&g_knetif[netif_id], 0, sizeof(g_knetif[netif_id]));
    return BCM_E_NONE;
}

int mock_bcm_knetif_count(void)
{
    int n = 0;
    for (int t = 0; t < MOCK_BCM_KNETIF_MAX; t++) {
        if (g_knetif[t].used) {
            n++;
        }
    }
    return n;
}

int bcm_field_group_create(int unit, bcm_field_qset_t qset, int pri,
                          bcm_field_group_t *group)
{
    if (unit != 0 || group == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    (void)pri;
    for (int t = 0; t < MOCK_BCM_FIELD_GROUP_MAX; t++) {
        if (!g_fgroup[t].used) {
            g_fgroup[t].used = 1;
            g_fgroup[t].id = t;
            (void)qset;
            *group = (bcm_field_group_t)t;
            return BCM_E_NONE;
        }
    }
    return BCM_E_FULL;
}

int bcm_field_group_destroy(int unit, bcm_field_group_t group)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (group < 0 || group >= MOCK_BCM_FIELD_GROUP_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_fgroup[group].used) {
        return BCM_E_NOT_FOUND;
    }
    for (int e = 0; e < MOCK_BCM_FIELD_ENTRY_MAX; e++) {
        if (g_fentry[e].used && g_fentry[e].group_id == group) {
            return BCM_E_BUSY;
        }
    }
    memset(&g_fgroup[group], 0, sizeof(g_fgroup[group]));
    return BCM_E_NONE;
}

int bcm_field_entry_create(int unit, bcm_field_group_t group,
                          bcm_field_entry_t *entry)
{
    if (unit != 0 || entry == NULL) {
        return BCM_E_PARAM;
    }
    if (group < 0 || group >= MOCK_BCM_FIELD_GROUP_MAX ||
        !g_fgroup[group].used) {
        return BCM_E_NOT_FOUND;
    }
    for (int e = 0; e < MOCK_BCM_FIELD_ENTRY_MAX; e++) {
        if (!g_fentry[e].used) {
            g_fentry[e].used = 1;
            g_fentry[e].group_id = group;
            g_fentry[e].installed = 0;
            g_fentry[e].has_drop = 0;
            *entry = (bcm_field_entry_t)e;
            return BCM_E_NONE;
        }
    }
    return BCM_E_FULL;
}

int bcm_field_entry_destroy(int unit, bcm_field_entry_t entry)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (entry < 0 || entry >= MOCK_BCM_FIELD_ENTRY_MAX) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited || !g_fentry[entry].used) {
        return BCM_E_NOT_FOUND;
    }
    memset(&g_fentry[entry], 0, sizeof(g_fentry[entry]));
    return BCM_E_NONE;
}

int bcm_field_entry_install(int unit, bcm_field_entry_t entry)
{
    if (unit != 0) {
        return BCM_E_UNIT;
    }
    if (entry < 0 || entry >= MOCK_BCM_FIELD_ENTRY_MAX ||
        !g_fentry[entry].used) {
        return BCM_E_NOT_FOUND;
    }
    g_fentry[entry].installed = 1;
    return BCM_E_NONE;
}

int bcm_field_qualify_EtherType(int unit, bcm_field_entry_t entry,
                               uint16 data, uint16 mask)
{
    if (entry < 0 || entry >= MOCK_BCM_FIELD_ENTRY_MAX ||
        !g_fentry[entry].used) {
        return BCM_E_NOT_FOUND;
    }
    (void)mask;
    g_fentry[entry].has_ether_type = 1;
    g_fentry[entry].ether_type = data;
    return BCM_E_NONE;
}

int bcm_field_qualify_SrcMac(int unit, bcm_field_entry_t entry,
                            bcm_mac_t data, bcm_mac_t mask)
{
    if (entry < 0 || entry >= MOCK_BCM_FIELD_ENTRY_MAX ||
        !g_fentry[entry].used) {
        return BCM_E_NOT_FOUND;
    }
    (void)data; (void)mask;
    g_fentry[entry].has_src_mac = 1;
    return BCM_E_NONE;
}

int bcm_field_qualify_DstMac(int unit, bcm_field_entry_t entry,
                            bcm_mac_t data, bcm_mac_t mask)
{
    if (entry < 0 || entry >= MOCK_BCM_FIELD_ENTRY_MAX ||
        !g_fentry[entry].used) {
        return BCM_E_NOT_FOUND;
    }
    (void)data; (void)mask;
    g_fentry[entry].has_dst_mac = 1;
    return BCM_E_NONE;
}

int bcm_field_qualify_InPort(int unit, bcm_field_entry_t entry,
                            bcm_port_t data, bcm_port_t mask)
{
    if (entry < 0 || entry >= MOCK_BCM_FIELD_ENTRY_MAX ||
        !g_fentry[entry].used) {
        return BCM_E_NOT_FOUND;
    }
    (void)mask;
    g_fentry[entry].has_in_port = 1;
    g_fentry[entry].in_port = data;
    return BCM_E_NONE;
}

int bcm_field_action_add(int unit, bcm_field_entry_t entry,
                        bcm_field_action_t action, uint32 param0,
                        uint32 param1)
{
    if (entry < 0 || entry >= MOCK_BCM_FIELD_ENTRY_MAX ||
        !g_fentry[entry].used) {
        return BCM_E_NOT_FOUND;
    }
    (void)param0; (void)param1;
    if (action == bcmFieldActionDrop) {
        g_fentry[entry].has_drop = 1;
        return BCM_E_NONE;
    }
    return BCM_E_UNAVAIL;
}

int mock_bcm_field_group_count(void)
{
    int n = 0;
    for (int t = 0; t < MOCK_BCM_FIELD_GROUP_MAX; t++) {
        if (g_fgroup[t].used) {
            n++;
        }
    }
    return n;
}

int mock_bcm_field_group_has_entry(int group_id)
{
    for (int e = 0; e < MOCK_BCM_FIELD_ENTRY_MAX; e++) {
        if (g_fentry[e].used && g_fentry[e].group_id == group_id) {
            return 1;
        }
    }
    return 0;
}

int mock_bcm_field_entry_installed(int entry_id)
{
    if (entry_id >= 0 && entry_id < MOCK_BCM_FIELD_ENTRY_MAX) {
        return g_fentry[entry_id].installed;
    }
    return 0;
}

int mock_bcm_field_entry_drops(int entry_id)
{
    if (entry_id >= 0 && entry_id < MOCK_BCM_FIELD_ENTRY_MAX) {
        return g_fentry[entry_id].has_drop;
    }
    return 0;
}

void bcm_qos_map_t_init(bcm_qos_map_t *map)
{
    if (map == NULL) {
        return;
    }
    memset(map, 0, sizeof(*map));
}

int bcm_qos_map_add(int unit, uint32 flags, bcm_qos_map_t *map, int map_id)
{
    if (unit != 0 || map == NULL) {
        return BCM_E_PARAM;
    }
    if (!g_mock.inited) {
        return BCM_E_INIT;
    }
    (void)flags; (void)map_id;
    for (int t = 0; t < MOCK_BCM_QOS_MAP_MAX; t++) {
        if (!g_qos_map[t].used) {
            g_qos_map[t].used = 1;
            g_qos_map[t].dscp = map->dscp;
            g_qos_map[t].int_pri = map->int_pri;
            return BCM_E_NONE;
        }
    }
    return BCM_E_FULL;
}

int bcm_qos_map_delete(int unit, uint32 flags, bcm_qos_map_t *map,
                      int map_id)
{
    if (unit != 0 || map == NULL) {
        return BCM_E_PARAM;
    }
    (void)flags; (void)map_id;
    for (int t = 0; t < MOCK_BCM_QOS_MAP_MAX; t++) {
        if (g_qos_map[t].used && g_qos_map[t].dscp == map->dscp &&
            g_qos_map[t].int_pri == map->int_pri) {
            memset(&g_qos_map[t], 0, sizeof(g_qos_map[t]));
            return BCM_E_NONE;
        }
    }
    return BCM_E_NOT_FOUND;
}

int mock_bcm_qos_map_count(void)
{
    int n = 0;
    for (int t = 0; t < MOCK_BCM_QOS_MAP_MAX; t++) {
        if (g_qos_map[t].used) {
            n++;
        }
    }
    return n;
}
