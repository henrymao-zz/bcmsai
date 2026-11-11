# OpenBCM SDK 6.5.27

This directory is a git submodule pointer to:

    https://github.com/Broadcom-Network-Switching-Software/OpenBCM

The upstream repo is archived (read-only) but still cloneable. Populate it with:

    git submodule update --init OpenBCM

The adapter expects the tree at `OpenBCM/sdk-6.5.27/`.

## Target build configuration (P0/P1)

`make/Make.local` selects the active device family / driver stack. Two targets
are supported by `libsai-bcm` (see ../PLAN.md):

| Target     | `Make.local` macro | Driver stack              |
|------------|--------------------|---------------------------|
| BCM56371   | `BCM_56370_A0 = 1` | Classic ESW / Firebolt6   |
| Trident 3-X| `BCM_56880_A0 = 1` | LTSW / XFS                |

The SDK build macro is driven by the top-level CMake option `SAI_BCM_TARGET`.
The P0 smoke build does not require OpenBCM to be built (`SAI_BCM_BUILD_OPENBCM=OFF`);
the real `libfirebolt6` / `libltsw*` link step activates from P1.