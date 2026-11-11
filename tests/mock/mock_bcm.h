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

#endif /* MOCK_BCM_H */