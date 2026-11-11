/*
 * SAI object-id (OID) <-> internal handle encoding for libsai-bcm.
 *
 * Layout (64-bit sai_object_id_t):
 *
 *     [63:56]  switch instance id (1..255, 0 is invalid)
 *     [55:48]  sai_object_type_t
 *     [47:0]   per-type index
 *
 * This mirrors the well-known encoding used by vendor SAI implementations on
 * top of Broadcom SDKs, where BCM handles (gports, L3 egress ids, vlan ids,
 * ...) are small integers that fit comfortably in the 48-bit index field.
 */
#ifndef SAI_BCM_OID_H
#define SAI_BCM_OID_H

#include <stdbool.h>
#include <stdint.h>

#include <sai.h>

#define SAI_BCM_OID_SW_SHIFT  56
#define SAI_BCM_OID_TYPE_SHIFT 48
#define SAI_BCM_OID_TYPE_MASK 0xFFULL
#define SAI_BCM_OID_IDX_MASK  0x0000FFFFFFFFFFFFULL

/* First switch instance id (0 is reserved/invalid). */
#define SAI_BCM_SWITCH_INSTANCE_FIRST 1

static inline sai_object_id_t sai_bcm_oid_encode(uint8_t switch_instance,
                                                  sai_object_type_t type,
                                                  uint64_t index)
{
    return ((sai_object_id_t)switch_instance << SAI_BCM_OID_SW_SHIFT) |
           ((sai_object_id_t)type << SAI_BCM_OID_TYPE_SHIFT) |
           (index & SAI_BCM_OID_IDX_MASK);
}

static inline bool sai_bcm_oid_decode(sai_object_id_t oid,
                                      uint8_t *switch_instance,
                                      sai_object_type_t *type,
                                      uint64_t *index)
{
    if (oid == SAI_NULL_OBJECT_ID) {
        return false;
    }
    uint8_t sw = (uint8_t)(oid >> SAI_BCM_OID_SW_SHIFT);
    if (sw == 0) {
        return false;
    }
    if (switch_instance != NULL) {
        *switch_instance = sw;
    }
    if (type != NULL) {
        *type = (sai_object_type_t)((oid >> SAI_BCM_OID_TYPE_SHIFT) &
                                    SAI_BCM_OID_TYPE_MASK);
    }
    if (index != NULL) {
        *index = oid & SAI_BCM_OID_IDX_MASK;
    }
    return true;
}

static inline sai_object_type_t sai_bcm_oid_type(sai_object_id_t oid)
{
    sai_object_type_t type = SAI_OBJECT_TYPE_NULL;
    (void)sai_bcm_oid_decode(oid, NULL, &type, NULL);
    return type;
}

static inline uint8_t sai_bcm_oid_switch(sai_object_id_t oid)
{
    uint8_t sw = 0;
    (void)sai_bcm_oid_decode(oid, &sw, NULL, NULL);
    return sw;
}

static inline uint64_t sai_bcm_oid_index(sai_object_id_t oid)
{
    uint64_t idx = 0;
    (void)sai_bcm_oid_decode(oid, NULL, NULL, &idx);
    return idx;
}

#endif /* SAI_BCM_OID_H */