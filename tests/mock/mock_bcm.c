/*
 * Mock OpenBCM backend implementation. See mock_bcm.h for details.
 */
#include "mock_bcm.h"

#include <string.h>

#include <shared/error.h>

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

#define MOCK_BCM_MAX_PORTS 64 /* logical port ids 0..63 */

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