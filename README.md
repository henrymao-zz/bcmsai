# libsai-bcm

Vendor **SAI** adapter built on top of Broadcom's **OpenBCM SDK 6.5.27**.

Implements the OCP Switch Abstraction Interface (**SAI v1.6.4**) on top of
Broadcom's OpenBCM unified `bcm_*` API. The same `libsai.so` source builds for
either supported target; only the OpenBCM build macro and the link-time archive
set differ.

See [PLAN.md](./PLAN.md) for the full design and roadmap.

## Status

| What | State |
|---|---|
| SAI v1.6.4 + OpenBCM submodules pinned | done |
| CMake build with `SAI_BCM_TARGET` selector | done |
| `libsai.so` exporting exactly the 8 SAI entry points | done |
| `sai_api_query()` dispatch over all 44 module vtables | done |
| OID encoding `[switch:8][type:8][index:48]` + tests | done |
| BCM error -> SAI status mapping | done |
| **P1 Switch API** (`bcm_init`/`bcm_detach`, port enumeration via `bcm_port_config_get`, port number/list, profile, src mac, notifications) | done |
| **P1 Port API** (admin/speed/MTU/autoneg/FEC get+set, oper status, stats get/clear via `bcm_stat_*`) | done |
| Mock BCM backend (32 XE ports + CPU, VLAN/trunk/L2/vswitch sim) + unit tests | done |
| **P2 Bridge API** (default 1Q bridge + per-port bridge ports at init, 1D via `bcm_vswitch_*`) | done |
| **P2 VLAN API** (create/remove, members via bridge ports + `bcm_vlan_gport_*`, tagging, member list) | done |
| **P2 LAG API** (`bcm_trunk_create/destroy/member_add/delete`, PORT_LIST) | done |
| **P2 FDB API** (`bcm_l2_addr_*` add/remove/set/get, flush by vlan/port/trunk) | done |
| **P3 Virtual Router** (internal vrf pool — OpenBCM has no vrf create; default VR via `SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID`) | done |
| **P3 Router Interface** (PORT/VLAN RIFs via `bcm_l3_ingress_create`) | done |
| **P3 Next Hop / Group** (`bcm_l3_egress_create`, lazy ECMP via `bcm_l3_egress_ecmp_*`) | done |
| **P3 Route / Neighbor** (`bcm_l3_route_add/delete`, `bcm_l3_host_add/delete`) | done |
| **P4 Hostif** (trap groups + CPU queue, traps via `bcm_rx_trap_*` w/ curated type map, KNET netdevs via `bcm_knet_netif_*`) | done |
| **P4 ACL basic** (ingress tables via `bcm_field_group_create`, entries: src/dst-mac, ethertype, in-port + drop/forward) | done |
| **P4 QoS minimal** (DSCP->TC maps via `bcm_qos_map_add/delete`) | done |
| ACL counters/ranges/groups, user-defined traps, packet I/O, schedulers/WRED/buffers | P5, not started |
| Real OpenBCM archive link (`SAI_BCM_BACKEND=openbcm`) | wired, needs SDK build |

Only the 8 SAI ABI symbols (`sai_api_initialize`, `sai_api_query`,
`sai_api_uninitialize`, `sai_log_set`, `sai_object_type_query`,
`sai_switch_id_query`, `sai_dbg_generate_dump`,
`sai_object_type_get_availability`) are exported; everything else (including
the mock `bcm_*` backend in dev builds) is local to the shared object
(`cmake/sai_bcm.ver`).

## Targets

| Target | Part | `Make.local` macro | Stack |
|---|---|---|---|
| **initial** | **BCM56371** (Firebolt6, `0xb371`) | `BCM_56370_A0=1` | Classic **ESW** |
| follow-on | Trident 3-X | `BCM_56880_A0=1` | **LTSW / XFS** |

## Backends

| Backend | What it links | Use |
|---|---|---|
| `mock` (default) | `tests/mock/mock_bcm.c` simulated device (32 XE ports + CPU) | dev + CI; unit tests run without hardware |
| `openbcm` | real OpenBCM sdk-6.5.27 archives (needs `SAI_BCM_BUILD_OPENBCM=ON`) | production |

In both modes the adapter is **compiled against the real OpenBCM headers**
from the `OpenBCM/sdk-6.5.27/include` submodule tree, so declarations are
always checked against the actual SDK.

## Submodules

```sh
git submodule update --init --recursive
```

- `SAI` → opencomputeproject/SAI @ **v1.6.4** (`b2487f0`)
- `OpenBCM` → Broadcom-Network-Switching-Software/OpenBCM @ **master** (`6d2330ce`, tree contains `sdk-6.5.27`)

On slow/unstable links a full `OpenBCM` clone may fail mid-transfer
(`GnuTLS recv error` / `early EOF`). Workaround: partial clone + sparse
checkout of just the SDK tree:

```sh
git config --global http.version HTTP/1.1
git submodule update --init SAI                     # small, clones fine
git clone --depth 1 --filter=blob:none --no-checkout \
    https://github.com/Broadcom-Network-Switching-Software/OpenBCM OpenBCM
git -C OpenBCM sparse-checkout set sdk-6.5.27
git -C OpenBCM checkout master
git submodule init && git submodule absorbgitdirs
```

The partial/sparse population is a local working-tree concern only — the
recorded gitlink SHA is identical either way.

## Build (mock backend, no hardware needed)

```sh
git submodule update --init --recursive
cmake -S . -B build -DSAI_BCM_TARGET=BCM56371 -DSAI_BCM_BACKEND=mock
cmake --build build
ctest --test-dir build --output-on-failure
./build/smoke_load ./build/libsai.so
```

## Build (P1+, with real OpenBCM archives)

```sh
git submodule update --init --recursive        # fetches OpenBCM sdk-6.5.27
./scripts/obcm_build.sh BCM56371                # builds libfirebolt6 + ESW set
cmake -S . -B build \
    -DSAI_BCM_TARGET=BCM56371 \
    -DSAI_BCM_BACKEND=openbcm \
    -DSA_BCM_BUILD_OPENBCM=ON
cmake --build build
```

## Layout

```
SAI/                      SAI v1.6.4 headers (submodule)
OpenBCM/                  OpenBCM sdk-6.5.27 (submodule)
include/sai_bcm_api_tables.x  X-macro registry of the 44 SAI modules
include/sai_bcm_oid.h     OID encode/decode ([switch:8][type:8][index:48])
include/sai_bcm_internal.h  shared state, lock, cross-module accessors
src/sai_bcm_api.c         SAI entry points + api_query dispatch + queries
src/sai_bcm_switch.c      Switch API: create/remove/attrs (bcm_init/config_get)
src/sai_bcm_port.c         Port API: attrs + stats (bcm_port_*/bcm_stat_*)
src/sai_bcm_bridge.c       Bridge API: default 1Q bridge, 1D via bcm_vswitch_*
src/sai_bcm_vlan.c         VLAN API: bcm_vlan_*, members via bridge ports
src/sai_bcm_lag.c          LAG API: bcm_trunk_*
src/sai_bcm_fdb.c          FDB API: bcm_l2_addr_*, flush by vlan/port/trunk
src/sai_bcm_virtual_router.c  VR API: internal vrf pool (OpenBCM vrfs are implicit ids)
src/sai_bcm_router_interface.c RIF API: bcm_l3_ingress_create (PORT/VLAN RIFs)
src/sai_bcm_nexthop.c      NH API: bcm_l3_egress_create (IP next hops)
src/sai_bcm_nexthopgroup.c NHG API: lazy ECMP via bcm_l3_egress_ecmp_*
src/sai_bcm_route.c        Route API: bcm_l3_route_add/delete (v4+v6)
src/sai_bcm_neighbor.c     Neighbor API: bcm_l3_host_add/delete
src/sai_bcm_hostif.c       Hostif API: bcm_rx_trap_*, bcm_knet_netif_*
src/sai_bcm_acl.c          ACL API (basic): bcm_field_*
src/sai_bcm_qosmap.c       QoS map API (minimal): bcm_qos_map_*
src/sai_bcm_oid.c          per-type object index allocator
src/sai_bcm_stubs.c        zero-init vtables for not-yet-implemented modules
src/sai_bcm_log.c          logging + bcm_err -> sai_status mapping
tests/mock/mock_bcm.c     mock BCM backend (dev/CI only)
tests/test_oid.c          OID encoding unit tests
tests/test_switch.c       Switch API tests over the mock backend
tests/test_port.c         Port API tests over the mock backend
tests/test_l2.c           VLAN/Bridge/LAG/FDB tests over the mock backend
tests/test_l3.c           VR/RIF/NH/NHG/Route/Neighbor tests over the mock backend
tests/test_p4.c           Hostif/ACL/QoS-map tests over the mock backend
tests/smoke_load.c        dlopen + 44-table enumeration ABI test
cmake/                    version script + per-target Make.local drivers
scripts/obcm_build.sh     OpenBCM SDK build driver (P1+)
```