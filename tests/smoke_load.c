/*
 * P0 smoke test: dlopen libsai.so and verify the SAI entry points resolve
 * and that sai_api_query() hands out a non-NULL table for every module
 * (SAI_API_SWITCH .. SAI_API_SYSTEM_PORT), while SAI_API_UNSPECIFIED /
 * SAI_API_MAX are rejected.
 *
 * Usage: smoke_load <path/to/libsai.so>
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sai.h"

typedef sai_status_t (*sai_api_initialize_fn)(uint64_t,
                                              const sai_service_method_table_t *);
typedef sai_status_t (*sai_api_uninitialize_fn)(void);
typedef sai_status_t (*sai_api_query_fn)(sai_api_t, void **);
typedef sai_status_t (*sai_log_set_fn)(sai_api_t, sai_log_level_t);

static const sai_api_t k_valid_apis[] = {
    SAI_API_SWITCH,           SAI_API_PORT,
    SAI_API_FDB,              SAI_API_VLAN,
    SAI_API_VIRTUAL_ROUTER,   SAI_API_ROUTE,
    SAI_API_NEXT_HOP,         SAI_API_NEXT_HOP_GROUP,
    SAI_API_ROUTER_INTERFACE, SAI_API_NEIGHBOR,
    SAI_API_ACL,             SAI_API_HOSTIF,
    SAI_API_MIRROR,           SAI_API_SAMPLEPACKET,
    SAI_API_STP,              SAI_API_LAG,
    SAI_API_POLICER,          SAI_API_WRED,
    SAI_API_QOS_MAP,          SAI_API_QUEUE,
    SAI_API_SCHEDULER,        SAI_API_SCHEDULER_GROUP,
    SAI_API_BUFFER,           SAI_API_HASH,
    SAI_API_UDF,             SAI_API_TUNNEL,
    SAI_API_L2MC,            SAI_API_IPMC,
    SAI_API_RPF_GROUP,        SAI_API_L2MC_GROUP,
    SAI_API_IPMC_GROUP,       SAI_API_MCAST_FDB,
    SAI_API_BRIDGE,           SAI_API_TAM,
    SAI_API_SEGMENTROUTE,     SAI_API_MPLS,
    SAI_API_DTEL,            SAI_API_BFD,
    SAI_API_ISOLATION_GROUP,  SAI_API_NAT,
    SAI_API_COUNTER,          SAI_API_DEBUG_COUNTER,
    SAI_API_MACSEC,           SAI_API_SYSTEM_PORT,
};
static const size_t k_num_valid =
    sizeof(k_valid_apis) / sizeof(k_valid_apis[0]);

#define FAIL(msg, ...) do { \
    fprintf(stderr, "smoke_load: " msg "\n", ##__VA_ARGS__); \
    if (h) dlclose(h); \
    return 1; \
} while (0)

int main(int argc, char **argv)
{
    void *h = NULL;

    if (argc != 2) {
        FAIL("usage: %s <path/to/libsai.so>", argv[0]);
    }

    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        FAIL("dlopen failed: %s", dlerror());
    }

    sai_api_initialize_fn  init_fn =
        (sai_api_initialize_fn) dlsym(h, "sai_api_initialize");
    sai_api_uninitialize_fn fini_fn =
        (sai_api_uninitialize_fn) dlsym(h, "sai_api_uninitialize");
    sai_api_query_fn   query_fn =
        (sai_api_query_fn)   dlsym(h, "sai_api_query");
    sai_log_set_fn     log_fn =
        (sai_log_set_fn)     dlsym(h, "sai_log_set");

    if (!init_fn || !fini_fn || !query_fn || !log_fn) {
        FAIL("missing entry point: init=%p fini=%p query=%p log=%p",
             (void *)init_fn, (void *)fini_fn, (void *)query_fn, (void *)log_fn);
    }

    sai_status_t s = init_fn(0, NULL);
    if (s != SAI_STATUS_SUCCESS) {
        FAIL("sai_api_initialize -> 0x%x", (unsigned)s);
    }

    /* query() must reject before initialize — but we already initialized,
     * so check the UNSPECIFIED/MAX bounds instead. */
    void *junk = NULL;
    s = query_fn(SAI_API_UNSPECIFIED, &junk);
    if (s != SAI_STATUS_INVALID_PARAMETER) {
        FAIL("UNSPECIFIED should be INVALID_PARAMETER, got 0x%x", (unsigned)s);
    }
    s = query_fn(SAI_API_MAX, &junk);
    if (s != SAI_STATUS_INVALID_PARAMETER) {
        FAIL("MAX should be INVALID_PARAMETER, got 0x%x", (unsigned)s);
    }
    s = query_fn(SAI_API_SWITCH, NULL);
    if (s != SAI_STATUS_INVALID_PARAMETER) {
        FAIL("NULL out-param should be INVALID_PARAMETER, got 0x%x", (unsigned)s);
    }

    /* Every valid module must return SUCCESS + a non-NULL table. */
    for (size_t i = 0; i < k_num_valid; ++i) {
        void *tbl = NULL;
        s = query_fn(k_valid_apis[i], &tbl);
        if (s != SAI_STATUS_SUCCESS) {
            FAIL("api %u query -> 0x%x", (unsigned)k_valid_apis[i], (unsigned)s);
        }
        if (tbl == NULL) {
            FAIL("api %u returned NULL table", (unsigned)k_valid_apis[i]);
        }
    }
    if (k_num_valid != (size_t)(SAI_API_SYSTEM_PORT)) {
        FAIL("expected %u valid apis, got %zu",
             (unsigned)SAI_API_SYSTEM_PORT, k_num_valid);
    }

    /* log_set must succeed. */
    s = log_fn(SAI_API_SWITCH, SAI_LOG_LEVEL_INFO);
    if (s != SAI_STATUS_SUCCESS) {
        FAIL("sai_log_set -> 0x%x", (unsigned)s);
    }

    s = fini_fn();
    if (s != SAI_STATUS_SUCCESS) {
        FAIL("sai_api_uninitialize -> 0x%x", (unsigned)s);
    }

    /* After uninitialize, query must return UNINITIALIZED. */
    s = query_fn(SAI_API_SWITCH, &junk);
    if (s != SAI_STATUS_UNINITIALIZED) {
        FAIL("post-fini query should be UNINITIALIZED, got 0x%x", (unsigned)s);
    }

    dlclose(h);
    printf("smoke_load: OK (%zu module tables)\n", k_num_valid);
    return 0;
}