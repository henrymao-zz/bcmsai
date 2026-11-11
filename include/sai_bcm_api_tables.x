/*
 * Single source of truth for the 44 SAI API modules.
 *
 * Each line is:  SAI_BCM_API(SAI_API_<ENUM>, <name>)
 *   - <name> is the token used in the C type  sai_<name>_api_t
 *             and in the instance            sai_bcm_<name>_api
 *
 * This file is included multiple times; the includer must #define
 * SAI_BCM_API(ENUM, NAME) before each #include and #undef it after.
 *
 * Verified against SAI v1.6.4 (sai.h _sai_api_t, values 1..44).
 */

SAI_BCM_API(SAI_API_SWITCH,            switch)
SAI_BCM_API(SAI_API_PORT,              port)
SAI_BCM_API(SAI_API_FDB,               fdb)
SAI_BCM_API(SAI_API_VLAN,              vlan)
SAI_BCM_API(SAI_API_VIRTUAL_ROUTER,    virtual_router)
SAI_BCM_API(SAI_API_ROUTE,             route)
SAI_BCM_API(SAI_API_NEXT_HOP,          next_hop)
SAI_BCM_API(SAI_API_NEXT_HOP_GROUP,    next_hop_group)
SAI_BCM_API(SAI_API_ROUTER_INTERFACE,  router_interface)
SAI_BCM_API(SAI_API_NEIGHBOR,          neighbor)
SAI_BCM_API(SAI_API_ACL,               acl)
SAI_BCM_API(SAI_API_HOSTIF,            hostif)
SAI_BCM_API(SAI_API_MIRROR,            mirror)
SAI_BCM_API(SAI_API_SAMPLEPACKET,     samplepacket)
SAI_BCM_API(SAI_API_STP,              stp)
SAI_BCM_API(SAI_API_LAG,              lag)
SAI_BCM_API(SAI_API_POLICER,          policer)
SAI_BCM_API(SAI_API_WRED,             wred)
SAI_BCM_API(SAI_API_QOS_MAP,          qos_map)
SAI_BCM_API(SAI_API_QUEUE,            queue)
SAI_BCM_API(SAI_API_SCHEDULER,        scheduler)
SAI_BCM_API(SAI_API_SCHEDULER_GROUP,  scheduler_group)
SAI_BCM_API(SAI_API_BUFFER,           buffer)
SAI_BCM_API(SAI_API_HASH,            hash)
SAI_BCM_API(SAI_API_UDF,            udf)
SAI_BCM_API(SAI_API_TUNNEL,         tunnel)
SAI_BCM_API(SAI_API_L2MC,           l2mc)
SAI_BCM_API(SAI_API_IPMC,           ipmc)
SAI_BCM_API(SAI_API_RPF_GROUP,      rpf_group)
SAI_BCM_API(SAI_API_L2MC_GROUP,     l2mc_group)
SAI_BCM_API(SAI_API_IPMC_GROUP,     ipmc_group)
SAI_BCM_API(SAI_API_MCAST_FDB,      mcast_fdb)
SAI_BCM_API(SAI_API_BRIDGE,         bridge)
SAI_BCM_API(SAI_API_TAM,            tam)
SAI_BCM_API(SAI_API_SEGMENTROUTE,   segmentroute)
SAI_BCM_API(SAI_API_MPLS,           mpls)
SAI_BCM_API(SAI_API_DTEL,           dtel)
SAI_BCM_API(SAI_API_BFD,            bfd)
SAI_BCM_API(SAI_API_ISOLATION_GROUP, isolation_group)
SAI_BCM_API(SAI_API_NAT,            nat)
SAI_BCM_API(SAI_API_COUNTER,        counter)
SAI_BCM_API(SAI_API_DEBUG_COUNTER,  debug_counter)
SAI_BCM_API(SAI_API_MACSEC,         macsec)
SAI_BCM_API(SAI_API_SYSTEM_PORT,     system_port)