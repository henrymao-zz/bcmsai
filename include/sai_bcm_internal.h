/*
 * libsai-bcm internal header.
 *
 * P0 bring-up: defines the global adapter state and declares the per-module
 * stub vtables. No BCM calls are made yet; every vtable is zero-initialized
 * (all method pointers NULL). sai_api_query() hands out pointers to these
 * tables so the adapter host can dlopen the library and enumerate the module
 * tables (the smoke_load contract).
 */
#ifndef SAI_BCM_INTERNAL_H
#define SAI_BCM_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>

#include <sai.h>

/* Extern declarations for the 44 per-module vtable instances.
 * sai_bcm_api_tables.x is included multiple times; define the consumer
 * macro before each inclusion and #undef it after. */
#define SAI_BCM_API(ENUM, NAME) \
    extern const sai_##NAME##_api_t sai_bcm_##NAME##_api;
#include "sai_bcm_api_tables.x"
#undef SAI_BCM_API

/* Build-time target metadata (set by CMake). */
#ifndef SAI_BCM_TARGET_NAME
#define SAI_BCM_TARGET_NAME "BCM56371"
#endif
#ifndef SAI_BCM_OBCM_CHIP_MACRO
#define SAI_BCM_OBCM_CHIP_MACRO "BCM_56370_A0"
#endif
#ifndef SAI_BCM_OBCM_STACK
#define SAI_BCM_OBCM_STACK "ESW"
#endif

/* Internal log + status helpers. */
void sai_bcm_log_init(sai_log_level_t level);
sai_log_level_t sai_bcm_log_level(void);
void sai_bcm_log(sai_log_level_t level, const char *file, int line,
                 const char *fmt, ...);

#define SAI_BCM_LOG(lvl, ...) \
    sai_bcm_log((lvl), __FILE__, __LINE__, __VA_ARGS__)

#endif /* SAI_BCM_INTERNAL_H */