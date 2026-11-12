/*
 * Shared SAI bulk operation engine.
 *
 * SAI bulk ops report per-object results in the statuses array: with
 * SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR the first failure stops the walk
 * and the remaining statuses become SAI_STATUS_NOT_EXECUTED; with
 * SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR every object is attempted. The
 * function itself returns SUCCESS when the bulk operation ran at all.
 */
#include "sai_bcm_internal.h"

void sai_bcm_bulk_mark_not_executed(sai_status_t *statuses, uint32_t from,
                                    uint32_t count)
{
    for (uint32_t i = from; i < from + count; i++) {
        statuses[i] = SAI_STATUS_NOT_EXECUTED;
    }
}

sai_status_t sai_bcm_bulk_object_create(
        sai_object_id_t switch_id, uint32_t obj_count,
        const uint32_t *attr_count, const sai_attribute_t **attr_list,
        sai_bulk_op_error_mode_t mode, sai_object_id_t *object_id,
        sai_status_t *object_statuses,
        sai_bcm_bulk_create_one_fn create_one)
{
    if (obj_count > 0 &&
        (attr_count == NULL || attr_list == NULL || object_id == NULL ||
         object_statuses == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < obj_count; i++) {
        object_id[i] = SAI_NULL_OBJECT_ID;
        sai_status_t st = create_one(&object_id[i], switch_id,
                                     attr_count[i], attr_list[i]);
        object_statuses[i] = st;
        if (st != SAI_STATUS_SUCCESS &&
            mode == SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR) {
            sai_bcm_bulk_mark_not_executed(object_statuses, i + 1,
                                           obj_count - i - 1);
            break;
        }
    }
    return SAI_STATUS_SUCCESS;
}

sai_status_t sai_bcm_bulk_object_remove(
        uint32_t obj_count, const sai_object_id_t *object_id,
        sai_bulk_op_error_mode_t mode, sai_status_t *object_statuses,
        sai_bcm_bulk_remove_one_fn remove_one)
{
    if (obj_count > 0 &&
        (object_id == NULL || object_statuses == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < obj_count; i++) {
        sai_status_t st = remove_one(object_id[i]);
        object_statuses[i] = st;
        if (st != SAI_STATUS_SUCCESS &&
            mode == SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR) {
            sai_bcm_bulk_mark_not_executed(object_statuses, i + 1,
                                           obj_count - i - 1);
            break;
        }
    }
    return SAI_STATUS_SUCCESS;
}