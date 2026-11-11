/*
 * Zero-initialized per-module vtables for the SAI modules that are not yet
 * implemented. Modules with a real implementation (see
 * include/sai_bcm_api_tables.x, IMPL=1) define their own vtable in
 * src/sai_bcm_<module>.c and are skipped here.
 *
 * A stubbed module hands out a non-NULL table whose method pointers are
 * NULL; sai_api_query() still succeeds for those modules. As each phase of
 * PLAN.md lands, modules move out of this file.
 */
#include "sai_bcm_internal.h"

/* IMPL == 0 -> define the stub vtable; IMPL == 1 -> skip (real module). */
#define STUB0(ENUM, NAME) \
    const sai_##NAME##_api_t sai_bcm_##NAME##_api = {0};
#define STUB1(ENUM, NAME) /* implemented elsewhere */

#define SAI_BCM_API(ENUM, NAME, IMPL) STUB##IMPL(ENUM, NAME)
#include "sai_bcm_api_tables.x"
#undef SAI_BCM_API
#undef STUB0
#undef STUB1