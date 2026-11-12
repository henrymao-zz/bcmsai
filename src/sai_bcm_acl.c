/*
 * SAI ACL API on top of OpenBCM (sdk-6.5.27).
 *
 * Phase 4 scope (basic subset per PLAN.md):
 *  - ACL tables with ingress stage only; the enabled match fields
 *    (create-only SAI_ACL_TABLE_ATTR_FIELD_*) select the BCM field
 *    qualifiers via bcm_field_group_create
 *  - entries with SRC_MAC / DST_MAC / ETHER_TYPE / IN_PORT matches
 *    (data + mask) and the PACKET_ACTION action (DROP / FORWARD);
 *    programmed via bcm_field_entry_create + bcm_field_qualify_* +
 *    bcm_field_action_add + bcm_field_entry_install
 *  - counters, ranges, table groups and their members return
 *    SAI_STATUS_NOT_SUPPORTED (P5)
 */
#include "sai_bcm_internal.h"
#include "sai_bcm_l2.h"

#include <bcm/field.h>

#include <string.h>

#define SAI_BCM_MAX_ACL_TABLES   64
#define SAI_BCM_MAX_ACL_ENTRIES  512

struct sai_bcm_acl_table {
    bool                      used;
    bcm_field_group_t         group_id;
    sai_acl_stage_t           stage;
    /* enabled match fields (from SAI_ACL_TABLE_ATTR_FIELD_*) */
    bool                      field_src_mac;
    bool                      field_dst_mac;
    bool                      field_ether_type;
    bool                      field_in_port;
};

struct sai_bcm_acl_entry {
    bool              used;
    sai_object_id_t   table_oid;
    bcm_field_entry_t entry_id;
    uint32_t          priority;
    sai_packet_action_t action;
    bool              have_src_mac;
    sai_mac_t         src_mac;
    sai_mac_t         src_mask;
    bool              have_dst_mac;
    sai_mac_t         dst_mac;
    sai_mac_t         dst_mask;
    bool              have_ether_type;
    uint16_t          ether_type;
    uint16_t          ether_mask;
    bool              have_in_port;
    sai_object_id_t   in_port;
};

static struct sai_bcm_acl_table  g_acl_tables[SAI_BCM_MAX_ACL_TABLES];
static struct sai_bcm_acl_entry  g_acl_entries[SAI_BCM_MAX_ACL_ENTRIES];

/* ---------------------------------------------------------------------------
 * State accessors
 * --------------------------------------------------------------------------- */
static struct sai_bcm_acl_table *acl_table_get(sai_object_id_t oid)
{
    if (sai_bcm_oid_type(oid) != SAI_OBJECT_TYPE_ACL_TABLE) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(oid);
    if (idx >= SAI_BCM_MAX_ACL_TABLES || !g_acl_tables[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_ACL_TABLE, idx) != oid) {
        return NULL;
    }
    return &g_acl_tables[idx];
}

static struct sai_bcm_acl_entry *acl_entry_get(sai_object_id_t oid)
{
    if (sai_bcm_oid_type(oid) != SAI_OBJECT_TYPE_ACL_ENTRY) {
        return NULL;
    }
    uint64_t idx = sai_bcm_oid_index(oid);
    if (idx >= SAI_BCM_MAX_ACL_ENTRIES || !g_acl_entries[idx].used) {
        return NULL;
    }
    if (sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                           SAI_OBJECT_TYPE_ACL_ENTRY, idx) != oid) {
        return NULL;
    }
    return &g_acl_entries[idx];
}

/* ---------------------------------------------------------------------------
 * ACL tables
 * --------------------------------------------------------------------------- */
static sai_status_t create_acl_table(sai_object_id_t *acl_table_id,
                                    sai_object_id_t switch_id,
                                    uint32_t attr_count,
                                    const sai_attribute_t *attr_list)
{
    (void)switch_id;

    if (acl_table_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_acl_stage_t stage = SAI_ACL_STAGE_INGRESS;
    struct sai_bcm_acl_table proto = {
        .used = true,
        .stage = SAI_ACL_STAGE_INGRESS,
    };

    for (uint32_t i = 0; i < attr_count; i++) {
        const sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_ACL_TABLE_ATTR_ACL_STAGE:
            stage = (sai_acl_stage_t)attr->value.s32;
            proto.stage = stage;
            break;
        case SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC:
            proto.field_src_mac = attr->value.booldata;
            break;
        case SAI_ACL_TABLE_ATTR_FIELD_DST_MAC:
            proto.field_dst_mac = attr->value.booldata;
            break;
        case SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE:
            proto.field_ether_type = attr->value.booldata;
            break;
        case SAI_ACL_TABLE_ATTR_FIELD_IN_PORT:
            proto.field_in_port = attr->value.booldata;
            break;
        default:
            /* Other fields are not supported in P4; refuse cleanly. */
            return SAI_STATUS_NOT_SUPPORTED;
        }
    }

    if (stage != SAI_ACL_STAGE_INGRESS) {
        return SAI_STATUS_NOT_SUPPORTED;
    }

    sai_bcm_lock();
    if (!g_sai_bcm_sw.created) {
        sai_bcm_unlock();
        return SAI_STATUS_UNINITIALIZED;
    }

    bcm_field_qset_t qset;
    BCM_FIELD_QSET_INIT(qset);
    if (proto.field_src_mac) {
        BCM_FIELD_QSET_ADD(qset, bcmFieldQualifySrcMac);
    }
    if (proto.field_dst_mac) {
        BCM_FIELD_QSET_ADD(qset, bcmFieldQualifyDstMac);
    }
    if (proto.field_ether_type) {
        BCM_FIELD_QSET_ADD(qset, bcmFieldQualifyEtherType);
    }
    if (proto.field_in_port) {
        BCM_FIELD_QSET_ADD(qset, bcmFieldQualifyInPort);
    }

    bcm_field_group_t group_id = 0;
    int rv = bcm_field_group_create(g_sai_bcm_sw.unit, qset, 0, &group_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_field_group_create failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }
    proto.group_id = group_id;

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_ACL_TABLE, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_ACL_TABLES) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_ACL_TABLE, idx);
        }
        (void)bcm_field_group_destroy(g_sai_bcm_sw.unit, group_id);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    g_acl_tables[idx] = proto;
    *acl_table_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                       SAI_OBJECT_TYPE_ACL_TABLE, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_acl_table(sai_object_id_t acl_table_id)
{
    sai_bcm_lock();

    struct sai_bcm_acl_table *table = acl_table_get(acl_table_id);
    if (table == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    for (uint32_t i = 0; i < SAI_BCM_MAX_ACL_ENTRIES; i++) {
        if (g_acl_entries[i].used &&
            g_acl_entries[i].table_oid == acl_table_id) {
            sai_bcm_unlock();
            return SAI_STATUS_OBJECT_IN_USE;
        }
    }

    int rv = bcm_field_group_destroy(g_sai_bcm_sw.unit, table->group_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_field_group_destroy failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint64_t idx = sai_bcm_oid_index(acl_table_id);
    memset(table, 0, sizeof(*table));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_ACL_TABLE, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_acl_table_attribute(sai_object_id_t acl_table_id,
                                           const sai_attribute_t *attr)
{
    (void)acl_table_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_acl_table_attribute(sai_object_id_t acl_table_id,
                                           uint32_t attr_count,
                                           sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_acl_table *table = acl_table_get(acl_table_id);
    if (table == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_ACL_TABLE_ATTR_ACL_STAGE:
            attr->value.s32 = table->stage;
            break;
        case SAI_ACL_TABLE_ATTR_FIELD_SRC_MAC:
            attr->value.booldata = table->field_src_mac;
            break;
        case SAI_ACL_TABLE_ATTR_FIELD_DST_MAC:
            attr->value.booldata = table->field_dst_mac;
            break;
        case SAI_ACL_TABLE_ATTR_FIELD_ETHER_TYPE:
            attr->value.booldata = table->field_ether_type;
            break;
        case SAI_ACL_TABLE_ATTR_FIELD_IN_PORT:
            attr->value.booldata = table->field_in_port;
            break;
        default:
            out = SAI_STATUS_NOT_SUPPORTED;
            break;
        }
    }

    sai_bcm_unlock();
    return out;
}

/* ---------------------------------------------------------------------------
 * ACL entries
 * --------------------------------------------------------------------------- */
static sai_status_t create_acl_entry(sai_object_id_t *acl_entry_id,
                                    sai_object_id_t switch_id,
                                    uint32_t attr_count,
                                    const sai_attribute_t *attr_list)
{
    sai_object_id_t table_oid = SAI_NULL_OBJECT_ID;
    struct sai_bcm_acl_entry proto = {0};

    (void)switch_id;

    if (acl_entry_id == NULL ||
        (attr_count > 0 && attr_list == NULL)) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (uint32_t i = 0; i < attr_count; i++) {
        const sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_ACL_ENTRY_ATTR_TABLE_ID:
            table_oid = attr->value.oid;
            break;
        case SAI_ACL_ENTRY_ATTR_PRIORITY:
            proto.priority = attr->value.u32;
            break;
        case SAI_ACL_ENTRY_ATTR_FIELD_SRC_MAC:
            if (attr->value.aclfield.enable) {
                proto.have_src_mac = true;
                memcpy(proto.src_mac, attr->value.aclfield.data.mac,
                       sizeof(sai_mac_t));
                memcpy(proto.src_mask, attr->value.aclfield.mask.mac,
                       sizeof(sai_mac_t));
            }
            break;
        case SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC:
            if (attr->value.aclfield.enable) {
                proto.have_dst_mac = true;
                memcpy(proto.dst_mac, attr->value.aclfield.data.mac,
                       sizeof(sai_mac_t));
                memcpy(proto.dst_mask, attr->value.aclfield.mask.mac,
                       sizeof(sai_mac_t));
            }
            break;
        case SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE:
            if (attr->value.aclfield.enable) {
                proto.have_ether_type = true;
                proto.ether_type = attr->value.aclfield.data.u16;
                proto.ether_mask = attr->value.aclfield.mask.u16;
            }
            break;
        case SAI_ACL_ENTRY_ATTR_FIELD_IN_PORT:
            if (attr->value.aclfield.enable) {
                proto.have_in_port = true;
                proto.in_port = attr->value.aclfield.data.oid;
            }
            break;
        case SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION:
            if (attr->value.aclaction.enable) {
                proto.action =
                    (sai_packet_action_t)attr->value.aclaction.parameter.s32;
            }
            break;
        default:
            return SAI_STATUS_NOT_SUPPORTED;
        }
    }

    if (table_oid == SAI_NULL_OBJECT_ID) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();

    struct sai_bcm_acl_table *table = acl_table_get(table_oid);
    if (table == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    /* Refuse matches the table does not support. */
    if ((proto.have_src_mac && !table->field_src_mac) ||
        (proto.have_dst_mac && !table->field_dst_mac) ||
        (proto.have_ether_type && !table->field_ether_type) ||
        (proto.have_in_port && !table->field_in_port)) {
        sai_bcm_unlock();
        return SAI_STATUS_INVALID_PARAMETER;
    }
    if (proto.action != SAI_PACKET_ACTION_DROP &&
        proto.action != SAI_PACKET_ACTION_FORWARD) {
        sai_bcm_unlock();
        return SAI_STATUS_NOT_SUPPORTED;
    }

    bcm_field_entry_t entry_id = 0;
    int rv = bcm_field_entry_create(g_sai_bcm_sw.unit, table->group_id,
                                    &entry_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_field_entry_create failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    if (proto.have_src_mac) {
        rv = bcm_field_qualify_SrcMac(g_sai_bcm_sw.unit, entry_id,
                                      proto.src_mac, proto.src_mask);
    }
    if (rv >= 0 && proto.have_dst_mac) {
        rv = bcm_field_qualify_DstMac(g_sai_bcm_sw.unit, entry_id,
                                      proto.dst_mac, proto.dst_mask);
    }
    if (rv >= 0 && proto.have_ether_type) {
        rv = bcm_field_qualify_EtherType(g_sai_bcm_sw.unit, entry_id,
                                         proto.ether_type, proto.ether_mask);
    }
    if (rv >= 0 && proto.have_in_port) {
        const struct sai_bcm_port_entry *pe =
            sai_bcm_switch_port_lookup(proto.in_port);
        if (pe == NULL) {
            rv = BCM_E_NOT_FOUND;
        } else {
            rv = bcm_field_qualify_InPort(g_sai_bcm_sw.unit, entry_id,
                                           pe->bcm_port, 0);
        }
    }
    if (rv >= 0 && proto.action == SAI_PACKET_ACTION_DROP) {
        rv = bcm_field_action_add(g_sai_bcm_sw.unit, entry_id,
                                  bcmFieldActionDrop, 0, 0);
    }
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR, "acl entry program failed: %d (%s)",
                    rv, bcm_errmsg(rv));
        (void)bcm_field_entry_destroy(g_sai_bcm_sw.unit, entry_id);
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    rv = bcm_field_entry_install(g_sai_bcm_sw.unit, entry_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_field_entry_install failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        (void)bcm_field_entry_destroy(g_sai_bcm_sw.unit, entry_id);
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint32_t idx;
    sai_status_t st = sai_bcm_idx_alloc(SAI_OBJECT_TYPE_ACL_ENTRY, &idx);
    if (st != SAI_STATUS_SUCCESS || idx >= SAI_BCM_MAX_ACL_ENTRIES) {
        if (st == SAI_STATUS_SUCCESS) {
            sai_bcm_idx_free(SAI_OBJECT_TYPE_ACL_ENTRY, idx);
        }
        (void)bcm_field_entry_destroy(g_sai_bcm_sw.unit, entry_id);
        sai_bcm_unlock();
        return (st == SAI_STATUS_SUCCESS) ? SAI_STATUS_TABLE_FULL : st;
    }

    proto.used = true;
    proto.table_oid = table_oid;
    proto.entry_id = entry_id;
    g_acl_entries[idx] = proto;
    *acl_entry_id = sai_bcm_oid_encode(SAI_BCM_SWITCH_INSTANCE_FIRST,
                                       SAI_OBJECT_TYPE_ACL_ENTRY, idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t remove_acl_entry(sai_object_id_t acl_entry_id)
{
    sai_bcm_lock();

    struct sai_bcm_acl_entry *entry = acl_entry_get(acl_entry_id);
    if (entry == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    int rv = bcm_field_entry_destroy(g_sai_bcm_sw.unit, entry->entry_id);
    if (rv < 0) {
        SAI_BCM_LOG(SAI_LOG_LEVEL_ERROR,
                    "bcm_field_entry_destroy failed: %d (%s)", rv,
                    bcm_errmsg(rv));
        sai_bcm_unlock();
        return sai_bcm_status(rv);
    }

    uint64_t idx = sai_bcm_oid_index(acl_entry_id);
    memset(entry, 0, sizeof(*entry));
    sai_bcm_idx_free(SAI_OBJECT_TYPE_ACL_ENTRY, (uint32_t)idx);

    sai_bcm_unlock();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t set_acl_entry_attribute(sai_object_id_t acl_entry_id,
                                           const sai_attribute_t *attr)
{
    (void)acl_entry_id; (void)attr;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t get_acl_entry_attribute(sai_object_id_t acl_entry_id,
                                            uint32_t attr_count,
                                            sai_attribute_t *attr_list)
{
    if (attr_count > 0 && attr_list == NULL) {
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_bcm_lock();
    struct sai_bcm_acl_entry *entry = acl_entry_get(acl_entry_id);
    if (entry == NULL) {
        sai_bcm_unlock();
        return SAI_STATUS_ITEM_NOT_FOUND;
    }

    sai_status_t out = SAI_STATUS_SUCCESS;
    for (uint32_t i = 0; i < attr_count && out == SAI_STATUS_SUCCESS; i++) {
        sai_attribute_t *attr = &attr_list[i];
        switch (attr->id) {
        case SAI_ACL_ENTRY_ATTR_TABLE_ID:
            attr->value.oid = entry->table_oid;
            break;
        case SAI_ACL_ENTRY_ATTR_PRIORITY:
            attr->value.u32 = entry->priority;
            break;
        case SAI_ACL_ENTRY_ATTR_ACTION_PACKET_ACTION:
            attr->value.aclaction.enable = true;
            attr->value.aclaction.parameter.s32 = entry->action;
            break;
        case SAI_ACL_ENTRY_ATTR_FIELD_SRC_MAC:
            attr->value.aclfield.enable = entry->have_src_mac;
            if (entry->have_src_mac) {
                memcpy(attr->value.aclfield.data.mac, entry->src_mac,
                       sizeof(sai_mac_t));
                memcpy(attr->value.aclfield.mask.mac, entry->src_mask,
                       sizeof(sai_mac_t));
            }
            break;
        case SAI_ACL_ENTRY_ATTR_FIELD_DST_MAC:
            attr->value.aclfield.enable = entry->have_dst_mac;
            if (entry->have_dst_mac) {
                memcpy(attr->value.aclfield.data.mac, entry->dst_mac,
                       sizeof(sai_mac_t));
                memcpy(attr->value.aclfield.mask.mac, entry->dst_mask,
                       sizeof(sai_mac_t));
            }
            break;
        case SAI_ACL_ENTRY_ATTR_FIELD_ETHER_TYPE:
            attr->value.aclfield.enable = entry->have_ether_type;
            if (entry->have_ether_type) {
                attr->value.aclfield.data.u16 = entry->ether_type;
                attr->value.aclfield.mask.u16 = entry->ether_mask;
            }
            break;
        case SAI_ACL_ENTRY_ATTR_FIELD_IN_PORT:
            attr->value.aclfield.enable = entry->have_in_port;
            if (entry->have_in_port) {
                attr->value.aclfield.data.oid = entry->in_port;
            }
            break;
        default:
            out = SAI_STATUS_NOT_SUPPORTED;
            break;
        }
    }

    sai_bcm_unlock();
    return out;
}

/* ---------------------------------------------------------------------------
 * Not-yet-implemented members
 * --------------------------------------------------------------------------- */
static sai_status_t acl_ns_create(sai_object_id_t *id, sai_object_id_t sw,
                                 uint32_t n, const sai_attribute_t *al)
{
    (void)id; (void)sw; (void)n; (void)al;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t acl_ns_remove(sai_object_id_t id)
{
    (void)id;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t acl_ns_set(sai_object_id_t id, const sai_attribute_t *a)
{
    (void)id; (void)a;
    return SAI_STATUS_NOT_SUPPORTED;
}

static sai_status_t acl_ns_get(sai_object_id_t id, uint32_t n,
                              sai_attribute_t *al)
{
    (void)id; (void)n; (void)al;
    return SAI_STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * vtable
 * --------------------------------------------------------------------------- */
const sai_acl_api_t sai_bcm_acl_api = {
    .create_acl_table                    = create_acl_table,
    .remove_acl_table                    = remove_acl_table,
    .set_acl_table_attribute             = set_acl_table_attribute,
    .get_acl_table_attribute             = get_acl_table_attribute,
    .create_acl_entry                    = create_acl_entry,
    .remove_acl_entry                    = remove_acl_entry,
    .set_acl_entry_attribute             = set_acl_entry_attribute,
    .get_acl_entry_attribute             = get_acl_entry_attribute,
    .create_acl_counter                  = acl_ns_create,
    .remove_acl_counter                  = acl_ns_remove,
    .set_acl_counter_attribute           = acl_ns_set,
    .get_acl_counter_attribute           = acl_ns_get,
    .create_acl_range                    = acl_ns_create,
    .remove_acl_range                    = acl_ns_remove,
    .set_acl_range_attribute             = acl_ns_set,
    .get_acl_range_attribute             = acl_ns_get,
    .create_acl_table_group              = acl_ns_create,
    .remove_acl_table_group              = acl_ns_remove,
    .set_acl_table_group_attribute       = acl_ns_set,
    .get_acl_table_group_attribute       = acl_ns_get,
    .create_acl_table_group_member       = acl_ns_create,
    .remove_acl_table_group_member       = acl_ns_remove,
    .set_acl_table_group_member_attribute = acl_ns_set,
    .get_acl_table_group_member_attribute = acl_ns_get,
};