/*
 * Unit tests for the SAI OID encoding/decoding.
 * Links the adapter statically (no dlopen); no BCM calls needed.
 */
#include <stdio.h>
#include <string.h>

#include "sai_bcm_internal.h"

static int failures = 0;

#define CHECK(cond, ...) do {                        \
    if (!(cond)) {                                   \
        fprintf(stderr, "FAIL %s:%d: ",              \
                __FILE__, __LINE__);                 \
        fprintf(stderr, __VA_ARGS__);               \
        fprintf(stderr, "\n");                       \
        failures++;                                  \
    }                                                \
} while (0)

int main(void)
{
    /* --- encode/decode round-trip --- */
    for (uint64_t idx = 0; idx < 5; idx++) {
        for (int t = 1; t < (int)SAI_OBJECT_TYPE_MAX; t += 17) {
            sai_object_id_t oid =
                sai_bcm_oid_encode(3, (sai_object_type_t)t, idx);
            CHECK(oid != SAI_NULL_OBJECT_ID, "encoded oid must be non-null");

            uint8_t sw = 0;
            sai_object_type_t dec_t = SAI_OBJECT_TYPE_NULL;
            uint64_t dec_idx = 0;
            CHECK(sai_bcm_oid_decode(oid, &sw, &dec_t, &dec_idx),
                  "decode of valid oid");
            CHECK(sw == 3, "switch id round-trip");
            CHECK(dec_t == (sai_object_type_t)t, "type round-trip");
            CHECK(dec_idx == idx, "index round-trip");
        }
    }

    /* --- field layout --- */
    sai_object_id_t oid = sai_bcm_oid_encode(1, SAI_OBJECT_TYPE_PORT, 7);
    CHECK((oid >> 56) == 1, "switch instance in [63:56]");
    CHECK(((oid >> 48) & 0xFF) == SAI_OBJECT_TYPE_PORT,
          "type in [55:48]");
    CHECK((oid & 0x0000FFFFFFFFFFFFULL) == 7, "index in [47:0]");

    /* --- invalid inputs --- */
    CHECK(!sai_bcm_oid_decode(SAI_NULL_OBJECT_ID, NULL, NULL, NULL),
          "null oid must not decode");
    CHECK(sai_bcm_oid_type(SAI_NULL_OBJECT_ID) == SAI_OBJECT_TYPE_NULL,
          "type of null oid is NULL");
    CHECK(sai_bcm_oid_switch(SAI_NULL_OBJECT_ID) == 0,
          "switch instance of null oid is 0");

    /* switch instance 0 must be invalid */
    sai_object_id_t bad = sai_bcm_oid_encode(0, SAI_OBJECT_TYPE_PORT, 1);
    CHECK(bad != SAI_NULL_OBJECT_ID, "raw encode is not null");
    CHECK(!sai_bcm_oid_decode(bad, NULL, NULL, NULL),
          "switch instance 0 must not decode");

    /* --- index mask --- */
    oid = sai_bcm_oid_encode(1, SAI_OBJECT_TYPE_PORT, 0xFFFFFFFFFFFFULL);
    CHECK(sai_bcm_oid_index(oid) == 0xFFFFFFFFFFFFULL, "48-bit index space");

    if (failures == 0) {
        printf("test_oid: OK\n");
        return 0;
    }
    printf("test_oid: %d FAILURES\n", failures);
    return 1;
}