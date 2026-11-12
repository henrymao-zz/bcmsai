/*
 * Generic per-type index allocator for SAI object ids.
 *
 * Ports are handled directly by the switch state (index == SAI port index);
 * VLAN ids use the VLAN id itself as the oid index. Everything else
 * (vlan member, bridge, bridge port, lag, lag member, fdb entry) allocates
 * a small integer index from here, which becomes the [47:0] field of the
 * encoded oid (see sai_bcm_oid.h).
 *
 * Indices are never reused within a running switch instance (monotonic
 * allocation, freed indices are skipped), which keeps oid semantics simple.
 */
#include "sai_bcm_internal.h"

#include <string.h>

#define SAI_BCM_IDX_MAX     4096
#define SAI_BCM_IDX_BITMAP_BYTES (SAI_BCM_IDX_MAX / 8)

static struct {
    uint8_t bitmap[SAI_OBJECT_TYPE_MAX][SAI_BCM_IDX_BITMAP_BYTES];
    uint16_t count[SAI_OBJECT_TYPE_MAX];
} g_idx;

sai_status_t sai_bcm_idx_alloc(sai_object_type_t type, uint32_t *idx)
{
    if (type <= SAI_OBJECT_TYPE_NULL || type >= SAI_OBJECT_TYPE_MAX ||
        idx == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }
    if (g_idx.count[type] >= SAI_BCM_IDX_MAX) {
        return SAI_STATUS_TABLE_FULL;
    }

    for (uint32_t i = 0; i < SAI_BCM_IDX_MAX; i++) {
        uint8_t *byte = &g_idx.bitmap[type][i / 8];
        uint8_t bit = (uint8_t)(1u << (i % 8));
        if ((*byte & bit) == 0) {
            *byte |= bit;
            g_idx.count[type]++;
            *idx = i;
            return SAI_STATUS_SUCCESS;
        }
    }
    return SAI_STATUS_TABLE_FULL;
}

void sai_bcm_idx_free(sai_object_type_t type, uint32_t idx)
{
    if (type <= SAI_OBJECT_TYPE_NULL || type >= SAI_OBJECT_TYPE_MAX ||
        idx >= SAI_BCM_IDX_MAX) {
        return;
    }
    uint8_t *byte = &g_idx.bitmap[type][idx / 8];
    uint8_t bit = (uint8_t)(1u << (idx % 8));
    if (*byte & bit) {
        *byte &= (uint8_t)~bit;
        g_idx.count[type]--;
    }
}

bool sai_bcm_idx_valid(sai_object_type_t type, uint32_t idx)
{
    if (type <= SAI_OBJECT_TYPE_NULL || type >= SAI_OBJECT_TYPE_MAX ||
        idx >= SAI_BCM_IDX_MAX) {
        return false;
    }
    return (g_idx.bitmap[type][idx / 8] & (1u << (idx % 8))) != 0;
}

uint16_t sai_bcm_idx_count(sai_object_type_t type)
{
    if (type <= SAI_OBJECT_TYPE_NULL || type >= SAI_OBJECT_TYPE_MAX) {
        return 0;
    }
    return g_idx.count[type];
}

void sai_bcm_idx_reset_all(void)
{
    memset(&g_idx, 0, sizeof(g_idx));
}