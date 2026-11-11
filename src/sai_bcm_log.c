/*
 * libsai-bcm logging + SAI status helpers.
 *
 * P0: minimal stderr logger. In P1+ this will route into OpenBCM's bsl
 * (BSL_LOG_ADD/APPL_LOG) and map _shr_err_t -> sai_status_t here.
 */
#include "sai_bcm_internal.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

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