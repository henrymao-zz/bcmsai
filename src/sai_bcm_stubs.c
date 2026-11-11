/*
 * Per-module SAI vtable instances for libsai-bcm.
 *
 * P0: all vtables are zero-initialized (every method pointer NULL).
 * sai_api_query() hands out pointers to these tables so the adapter host can
 * dlopen the library and enumerate the module tables. From P1 onward each
 * module file (sai_bcm_port.c, sai_bcm_fdb.c, ...) will populate its own table
 * with real bcm_* backed function pointers, and this file will be removed.
 */
#include "sai_bcm_internal.h"

#define SAI_BCM_API(ENUM, NAME) \
    const sai_##NAME##_api_t sai_bcm_##NAME##_api = {0};
#include "sai_bcm_api_tables.x"
#undef SAI_BCM_API