# libsai-bcm — Implementation Plan

A vendor SAI adapter library (`libsai`) that implements the OCP **Switch Abstraction Interface**
on top of Broadcom's **OpenBCM SDK 6.5.27**.

---

## 1. Version Selection

| Component         | Version       | Rationale                                                                 |
|-------------------|---------------|---------------------------------------------------------------------------|
| OpenBCM SDK       | `sdk-6.5.27`  | Last 6.5.x release. Multi-device support.                                |
| SAI headers (API) | **v1.6.4**    | Contemporary to 6.5.27 era (v1.6.0 = Aug 2019). Mature point release on   |
|                   |               | the `v1.6` branch. Avoids v1.7+ APIs (e.g. UDF, enhanced TAM) that 6.5.27 |
|                   |               | cannot natively serve. SAI `inc/` headers are self-contained (no runtime).|
| Build             | CMake + make  | OpenBCM ships autotools/Makefile-based build; we wrap it with CMake.      |

### 1.1 Target devices (verified against sdk-6.5.27)

The adapter is written against the unified `bcm_*` API, so the **same `libsai.so`
source** builds for either target — only the OpenBCM build macro and the link-time
archive set differ. Two targets are supported:

| Target (priority)        | Part         | Build macro       | Driver stack      | Device ID |
|--------------------------|--------------|-------------------|-------------------|-----------|
| **Initial / bring-up**   | **BCM56371** | `BCM_56370_A0=1`  | Classic **ESW**   | `0xb371`  |
| Follow-on (planned)      | Trident 3-X  | `BCM_56880_A0=1`  | **LTSW / XFS**    | n/a       |

**BCM56371** is a member of the BCM56370 silicon family ("Firebolt6"). Verified in
`sdk-6.5.27/include/soc/devids.h`:
`BCM56371_DEVICE_ID = 0xb371` (siblings: 56370=`0xb370`, 56374=`0xb374`, 56376=`0xb376`,
all sharing the Firebolt6 die). The SDK has a single build switch per family —
`BCM_56370_A0 = 1` — and the specific SKU is auto-detected at PCI attach via the
device-ID table.

### 1.2 BCM56371 build configuration

In `OpenBCM/sdk-6.5.27/make/Make.local`, enable the family:

```make
BCM_56370_A0 = 1
```

This selects the **classic ESW** dispatch path (`Make.config` lists `BCM_56370_A0`
under `ESW_CHIP_NAMES`). Per `make/Make.targets` (`ifdef BCM_56370_A0`), the build
pulls the Firebolt6 driver and its ESW dependencies:

```
libfirebolt6   libfirebolt   libbradley   libherc   libhumv
libscorpion    libtrx        libtriumph   libtriumph2  libtriumph3
libsoc_* (matching soc libs)   libcmn*   libbcm*
```

The SAI adapter links these statically into `libsai.so`; all BCM symbols are kept
internal.

### 1.3 Trident 3-X (follow-on) build configuration

For the later TD3X target, `Make.local` instead sets `BCM_56880_A0 = 1`, which
selects the **LTSW / XFS** dispatch path
(`LTSW_SPEC_XFS_CHIP_NAMES = BCM_56880_A0 BCM_56780_A0 BCM_56890_A0`) and produces
the lightweight driver libs (`libltsw*`, `libsoc_ltsw*`, `libxfs*`, TM libs).
TD3X is *not* served by `libtrident3` (that is `BCM_56870_A0` / plain TD3).

> Note: The OpenBCM upstream repo was archived on 2026-01-14 (read-only). `sdk-6.5.27`
> source tree is still available. We vendor it as a submodule/snapshot.

Because BCM56371 (classic ESW) and TD3X (LTSW) implement the *same* unified `bcm_*`
API, the SAI translation layer is stack-agnostic. The only per-target code is:
(a) link-time archive selection (CMake picks the lib set from the active macro),
(b) a small `sai_bcm_platform.c` with capability quirks (e.g. max ports, buffer
pool layout, features that differ between ESW and LTSW). Feature-parity gaps are
returned as `SAI_STATUS_NOT_SUPPORTED` — see Risks §6.

---

## 2. Repository Layout

```
libsai-bcm/
├── SAI/                      # git submodule @ v1.6.4 (headers + meta only)
├── OpenBCM/                  # git submodule @ sdk-6.5.27
├── CMakeLists.txt
├── Makefile
├── include/                  # internal headers
│   ├── sai_bcm_internal.h
│   ├── sai_bcm_port.h
│   ├── sai_bcm_lag.h
│   ├── sai_bcm_fdb.h
│   ├── sai_bcm_route.h
│   ├── sai_bcm_neighbor.h
│   ├── sai_bcm_vlan.h
│   ├── sai_bcm_bridge.h
│   ├── sai_bcm_hostif.h
│   └── sai_bcm_oid.h         # SAI OID <-> BCM handle mapping
├── src/
│   ├── sai_bcm_api.c         # sai_api_query() / sai_api_uninitialize()
│   ├── sai_bcm_switch.c      # sai_switch_*
│   ├── sai_bcm_port.c
│   ├── sai_bcm_lag.c
│   ├── sai_bcm_fdb.c
│   ├── sai_bcm_route.c
│   ├── sai_bcm_neighbor.c
│   ├── sai_bcm_vlan.c
│   ├── sai_bcm_bridge.c
│   ├── sai_bcm_hostif.c
│   ├── sai_bcm_oid.c         # OID allocator / registry
│   └── sai_bcm_log.c         # logging + SAI_STATUS mapping
├── config/
│   ├── brcm_sai_config.c     # default config.ini parser
│   └── sai.profile
├── tests/
│   ├── unit/                 # cmocka-based unit tests (mocked BCM APIs)
│   └── sai_ptf/              # SAI PTF conformance (subset)
└── docs/
```

---

## 3. Core Design Principles

1. **Single shared object**: `libsai.so`, exporting the SAI function tables only.
   All BCM symbols kept internal (statically linked against the active target's
   driver libs: for BCM56371 — `libfirebolt6` + the ESW archive set; for TD3X —
   `libltsw*`/`libxfs*`; plus common `libbcm*`, `libcmn*`).
2. **OID registry**: SAI uses opaque `sai_object_id_t`; BCM uses integers (GPorts, LIFs,
   L3 egress IDs, etc.). A central `sai_bcm_oid.c` manages a 32-bit encoded OID:
   `[type:8][idx:24]` with a backing registry table for reverse lookup. This mirrors how
   the upstream Broadcom SAI does it.
3. **Translation layer**: Each SAI module (`port`, `fdb`, …) translates SAI attributes
   ↔ BCM API calls. Keep the translation logic in one `.c` per SAI object type.
4. **Status mapping**: Centralized `sai_bcm_log.c` converts `_shr_err_t` → `sai_status_t`.
5. **Thread model**: SAI is single-threaded by contract; we hold a single BCM unit/context.
   A big SAI lock serializes all entry points.
6. **No kernel dependencies**: Pure userspace adapter; PCIe attach handled by OpenBCM's
   `bde` (Board Device Entity). Hostif (CPU port) via BCM KNET or linux netdev.

---

## 4. Phased Roadmap (basic features first)

### Phase 0 — Bring-up (week 1–2)
- Vendor OpenBCM `sdk-6.5.27` and SAI `v1.6.4` as submodules.
- CMake skeleton; build OpenBCM libs for **BCM56371** (`BCM_56370_A0=1`, classic
  ESW / Firebolt6 path). Verify `libfirebolt6` + ESW deps are produced.
  (TD3X/LTSW build wired behind a CMake option for the follow-on phase.)
- Implement `sai_api_query()` returning stub tables for all SAI modules.
- Implement `sai_log_set` / `sai_api_uninitialize`.
- Validate the `.so` loads and `sai_api_query(SAI_API_SWITCH, ...)` returns a table.

### Phase 1 — Switch & Port (week 3–5)
- `sai_switch_api`: `create_switch` (calls `bcm_init`/`bcm_attach`), `get_attribute`
  (port count, max routes, etc.), `set_attribute` (switch shell, restart).
- Port init: walk `bcm_port_config_t`, allocate SAI port OIDs.
- `sai_port_api`: admin state, speed, MTU, fec, autoneg, `get_stats`.
- Hardware lab validation on a **BCM56371** platform.

### Phase 2 — L2 foundation (week 6–7)
- `sai_vlan_api`: VLAN create/delete, member add/remove, tagging.
- `sai_bridge_api`: 1Q/1D bridge, bridge port create, port<->VLAN binding.
- `sai_fdb_api`: `create/remove/set/get/flush` → `bcm_l2_addr_*`.
- `sai_lag_api`: LAG group + member, `bcm_trunk_*`.

### Phase 3 — L3 foundation (week 8–10)
- `sai_virtual_router_api`: VRF create/delete → `bcm_vrf_create`.
- `sai_router_interface_api`: RIF over port/VLAN/LAG → `bcm_l3_ingress_*`.
- `sai_neighbor_api`: neighbor entries → `bcm_l3_host_add` / `bcm_neighbor_*`.
- `sai_route_api`: IPv4/IPv6 routes → `bcm_l3_route_add`, ECMP next-hop groups,
  `sai_next_hop_api` (`bcm_l3_egress_create`).

### Phase 4 — Host interface & misc (week 11–12)
- `sai_hostif_api`: trap groups, hostif table, KNET netdev for CPU traffic.
- `sai_acl_api` (basic subset): ingress port/ether-type ACL → `bcm_field_*`.
- `sai_qos_api` (minimal): scheduler groups, WRED, buffer pools — defer advanced
  features to phase 5+.

### Phase 5+ — Hardening (ongoing)
- PTF conformance suite (subset from `SAI/ptf`).
- Unit tests with cmocka; CI on GitHub Actions with a containerized OpenBCM build.
- Stats polling, warm-boot (`sai_switch_set_attribute(SAI_SWITCH_ATTR_RESTART_WARM)`).
- TAM, UDF, debug counters — only if/when ported to SAI v1.7.

---

## 5. Build & Test Strategy

- **Build**: top-level `CMakeLists.txt` builds OpenBCM libs first (out-of-tree).
  A CMake option `SAI_BCM_TARGET` (`BCM56371` default, later `TD3X`) selects the
  `Make.local` device macro and the link archive set:
  - `BCM56371` → `BCM_56370_A0=1`, link `libfirebolt6` + ESW libs.
  - `TD3X`     → `BCM_56880_A0=1`, link `libltsw*`/`libxfs*`.
  Then compiles `src/*.c` against `SAI/inc` and `sdk-6.5.27/include`, and produces
  `libsai.so`.
- **Unit tests**: cmocka; mock the `bcm_*` API surface via link-time seams so tests
  run without hardware.
- **Conformance**: SAI PTF (`SAI/ptf`) on a physical switch; run the
  `ptf_sai_port`, `ptf_sai_vlan`, `ptf_sai_fdb`, `ptf_sai_l3` suites first.
- **CI**: Docker image with OpenBCM built for `x86_64-linux` `bde` simulator; compile +
  unit tests on every push.

---

## 6. Risks & Mitigations

| Risk                                              | Mitigation                                                    |
|---------------------------------------------------|---------------------------------------------------------------|
| OpenBCM repo archived (no new fixes)              | Vendor a snapshot; maintain local patches under `patches/`.   |
| SDK 6.5.27 lacks some SAI v1.6 capabilities       | Return `SAI_STATUS_NOT_SUPPORTED` where BCM can't back it.    |
| **BCM56371 is a smaller Firebolt6 device**        | Capabilities (port count, table scale, ACL/field, QoS depth)  |
| (fewer ports/tables than TD3X)                    | are narrower than TD3X. Query limits via `bcm_*`/SOC props at |
|                                                   | init and advertise real values through SAI switch/port attrs. |
| **ESW vs LTSW driver-stack divergence**           | Adapter targets the unified `bcm_*` API only. Keep a thin     |
| (BCM56371=ESW `libfirebolt6`; TD3X=LTSW)          | `sai_bcm_platform.c` for per-target quirks; stub gaps as      |
|                                                   | `SAI_STATUS_NOT_SUPPORTED`. Validate parity per phase.        |
| OID/handle mapping complexity                     | Reuse the well-known type/idx encoding; cover with unit tests.|
| No hardware access in CI                          | bde simulator + cmocka mocks; reserve lab for integration.    |
| KNET vs userspace hostif choice                   | Start with KNET netdev (matches SONiC expectation).           |

---

## 7. First-week concrete tasks

1. `git submodule add` OpenBCM (checkout `sdk-6.5.27`) and SAI (`v1.6.4`).
2. Write `CMakeLists.txt` with a `SAI_BCM_TARGET=BCM56371` option that drives
   `Make.local` (`BCM_56370_A0=1`) and builds the Firebolt6 + ESW lib set.
3. Create `src/sai_bcm_api.c` + `src/sai_bcm_switch.c` stubs; produce a loadable
   `libsai.so` whose `sai_api_query` returns populated (stubbed) vtables.
4. Add a `tests/smoke_load.c` that `dlopen`s the lib and asserts all module tables
   are returned.
5. Stand up the Docker CI image.
