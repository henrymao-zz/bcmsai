# libsai-bcm

Vendor **SAI** adapter built on top of Broadcom's **OpenBCM SDK 6.5.27**.

Implements the OCP Switch Abstraction Interface (**SAI v1.6.4**) on top of
Broadcom's OpenBCM unified `bcm_*` API. The same `libsai.so` source builds for
either supported target; only the OpenBCM build macro and the link-time archive
set differ.

See [PLAN.md](./PLAN.md) for the full design and roadmap.

## Status: Phase 0 (bring-up)

| What | State |
|---|---|
| SAI v1.6.4 headers vendored (`SAI/inc/`) | done |
| CMake build with `SAI_BCM_TARGET` selector | done |
| Loadable `libsai.so` exporting the 8 SAI entry points | done |
| `sai_api_query()` dispatch over 44 zero-init module vtables | done |
| `dlopen` smoke test + CTest | done |
| Docker + CI | done |
| Real `bcm_*` integration (P1+) | not started |

Only the 8 SAI ABI symbols (`sai_api_initialize`, `sai_api_query`,
`sai_api_uninitialize`, `sai_log_set`, `sai_object_type_query`,
`sai_switch_id_query`, `sai_dbg_generate_dump`,
`sai_object_type_get_availability`) are exported; everything else is local to
the shared object (enforced by `cmake/sai_bcm.ver`).

## Targets

| Target | Part | `Make.local` macro | Stack |
|---|---|---|---|
| **initial** | **BCM56371** (Firebolt6, `0xb371`) | `BCM_56370_A0=1` | Classic **ESW** |
| follow-on | Trident 3-X | `BCM_56880_A0=1` | **LTSW / XFS** |

## Build (P0, stubs only)

```sh
cmake -S . -B build -DSAI_BCM_TARGET=BCM56371 -DSA_BCM_BUILD_OPENBCM=OFF
cmake --build build
ctest --test-dir build --output-on-failure
./build/smoke_load ./build/libsai.so
```

## Build (P1+, with OpenBCM linked)

```sh
git submodule update --init --recursive        # fetches OpenBCM sdk-6.5.27
./scripts/obcm_build.sh BCM56371                # builds libfirebolt6 + ESW set
cmake -S . -B build -DSAI_BCM_TARGET=BCM56371 -DSA_BCM_BUILD_OPENBCM=ON
cmake --build build
```

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

## Layout

```
SAI/                      vendored SAI v1.6.4 inc/ headers (submodule)
OpenBCM/                  OpenBCM sdk-6.5.27 (submodule, P1+)
include/                  internal headers (X-macro module table)
src/sai_bcm_api.c         SAI entry points + api_query dispatch
src/sai_bcm_stubs.c       44 zero-init module vtables (P0; replaced per-module in P1)
src/sai_bcm_log.c         logging + status mapping
tests/smoke_load.c        dlopen + 44-table enumeration test
cmake/                    version script + per-target Make.local drivers
scripts/obcm_build.sh     OpenBCM SDK build driver (P1+)
```