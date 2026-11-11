/*
 * libsai-bcm logging + status helpers.
 *
 * sai_bcm_status() maps OpenBCM error codes (_shr_err_t / BCM_E_*) to SAI
 * status codes. sai_bcm_log() is a minimal stderr logger; in later phases it
 * can be routed into OpenBCM's bsl.
 */
#include "sai_bcm_internal.h"

#include <stdio.h>
#include <stdarg.h>

static sai_log_level_t g_level = SAI_LOG_LEVEL_WARN;

void sai_bcm_log_init(sai_log_level_t level)
{
    g_level = level;
}

sai_log_level_t sai_bcm_log_level(void)
{
    return g_level;
}

void sai_bcm_log(sai_log_level_t level, const char *file, int line,
                 const char *fmt, ...)
{
    static const char *names[] = {
        "DEBUG", "INFO", "NOTICE", "WARN", "ERROR", "CRITICAL"
    };

    if (level < g_level) {
        return;
    }
    if (level < SAI_LOG_LEVEL_DEBUG || level > SAI_LOG_LEVEL_CRITICAL) {
        level = SAI_LOG_LEVEL_ERROR;
    }

    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[libsai-bcm] %-8s %s:%d: ", names[level], file, line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

sai_status_t sai_bcm_status(int bcm_rv)
{
    if (bcm_rv >= 0) {
        return SAI_STATUS_SUCCESS;
    }

    switch (bcm_rv) {
    case BCM_E_PARAM:
    case BCM_E_UNIT:
    case BCM_E_PORT:
    case BCM_E_BADID:
    case BCM_E_CONFIG:
        return SAI_STATUS_INVALID_PARAMETER;
    case BCM_E_INIT:
        return SAI_STATUS_UNINITIALIZED;
    case BCM_E_UNAVAIL:
    case BCM_E_DISABLED:
    case BCM_E_NO_HANDLER:
        return SAI_STATUS_NOT_SUPPORTED;
    case BCM_E_MEMORY:
        return SAI_STATUS_NO_MEMORY;
    case BCM_E_NOT_FOUND:
        return SAI_STATUS_ITEM_NOT_FOUND;
    case BCM_E_EXISTS:
        return SAI_STATUS_ITEM_ALREADY_EXISTS;
    case BCM_E_FULL:
    case BCM_E_RESOURCE:
        return SAI_STATUS_TABLE_FULL;
    case BCM_E_BUSY:
        return SAI_STATUS_FAILURE;
    default:
        return SAI_STATUS_FAILURE;
    }
}