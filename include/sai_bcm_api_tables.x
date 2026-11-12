/*
 * Single source of truth for the 44 SAI API modules.
 *
 * Each line is:  SAI_BCM_API(SAI_API_<ENUM>, <name>, <impl>)
 *   - <name> is the token used in the C type  sai_<name>_api_t
 *             and in the instance            sai_bcm_<name>_api
 *   - <impl>  is 1 when the module has a real implementation in its own
 *             src/sai_bcm_<name>.c file, and 0 when the vtable is still a
 *             zero-initialized stub from src/sai_bcm_stubs.c.
 *
 * This file is included multiple times; the includer must #define
 * SAI_BCM_API(ENUM, NAME, IMPL) before each #include and #undef it after.
 *
 * Verified against SAI v1.6.4 (sai.h _sai_api_t, values 1..44).
 */

/* Implemented modules */
SAI_BCM_API(SAI_API_SWITCH,            switch,            1)
SAI_BCM_API(SAI_API_PORT,              port,              1)

/* Stubbed modules (implemented per-phase, see PLAN.md roadmap) */
SAI_BCM_API(SAI_API_FDB,               fdb,               1)
SAI_BCM_API(SAI_API_VLAN,              vlan,              1)
SAI_BCM_API(SAI_API_VIRTUAL_ROUTER,    virtual_router,    1)
SAI_BCM_API(SAI_API_ROUTE,             route,             1)
SAI_BCM_API(SAI_API_NEXT_HOP,          next_hop,          1)
SAI_BCM_API(SAI_API_NEXT_HOP_GROUP,    next_hop_group,    1)
SAI_BCM_API(SAI_API_ROUTER_INTERFACE,  router_interface,  1)
SAI_BCM_API(SAI_API_NEIGHBOR,          neighbor,          1)
SAI_BCM_API(SAI_API_ACL,               acl,               0)
SAI_BCM_API(SAI_API_HOSTIF,            hostif,            0)
SAI_BCM_API(SAI_API_MIRROR,            mirror,            0)
SAI_BCM_API(SAI_API_SAMPLEPACKET,     samplepacket,      0)
SAI_BCM_API(SAI_API_STP,              stp,               0)
SAI_BCM_API(SAI_API_LAG,              lag,               1)
SAI_BCM_API(SAI_API_POLICER,          policer,           0)
SAI_BCM_API(SAI_API_WRED,             wred,              0)
SAI_BCM_API(SAI_API_QOS_MAP,          qos_map,           0)
SAI_BCM_API(SAI_API_QUEUE,            queue,             0)
SAI_BCM_API(SAI_API_SCHEDULER,        scheduler,         0)
SAI_BCM_API(SAI_API_SCHEDULER_GROUP,  scheduler_group,   0)
SAI_BCM_API(SAI_API_BUFFER,           buffer,            0)
SAI_BCM_API(SAI_API_HASH,            hash,               0)
SAI_BCM_API(SAI_API_UDF,            udf,                 0)
SAI_BCM_API(SAI_API_TUNNEL,         tunnel,             0)
SAI_BCM_API(SAI_API_L2MC,           l2mc,                0)
SAI_BCM_API(SAI_API_IPMC,           ipmc,                0)
SAI_BCM_API(SAI_API_RPF_GROUP,      rpf_group,           0)
SAI_BCM_API(SAI_API_L2MC_GROUP,     l2mc_group,         0)
SAI_BCM_API(SAI_API_IPMC_GROUP,     ipmc_group,         0)
SAI_BCM_API(SAI_API_MCAST_FDB,      mcast_fdb,          0)
SAI_BCM_API(SAI_API_BRIDGE,         bridge,             1)
SAI_BCM_API(SAI_API_TAM,            tam,                 0)
SAI_BCM_API(SAI_API_SEGMENTROUTE,   segmentroute,       0)
SAI_BCM_API(SAI_API_MPLS,           mpls,               0)
SAI_BCM_API(SAI_API_DTEL,           dtel,               0)
SAI_BCM_API(SAI_API_BFD,            bfd,                 0)
SAI_BCM_API(SAI_API_ISOLATION_GROUP, isolation_group,   0)
SAI_BCM_API(SAI_API_NAT,            nat,                 0)
SAI_BCM_API(SAI_API_COUNTER,        counter,            0)
SAI_BCM_API(SAI_API_DEBUG_COUNTER,  debug_counter,      0)
SAI_BCM_API(SAI_API_MACSEC,         macsec,             0)
SAI_BCM_API(SAI_API_SYSTEM_PORT,     system_port,       0)