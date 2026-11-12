/*
 * Mock OpenBCM backend implementation. See mock_bcm.h for details.
 */
#include "mock_bcm.h"

#include <string.h>

#include <shared/error.h>
#include <bcm/vlan.h>
#include <bcm/trunk.h>
#include <bcm/l2.h>
#include <bcm/vswitch.h>

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
