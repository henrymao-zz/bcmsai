#!/bin/bash
# Build the OpenBCM sdk-6.5.27 libraries for the selected target.
#
# This is the P1+ integration entry point. P0 ships a stub libsai.so that
# links nothing from OpenBCM; this script produces the archive set that the
# CMake `SAI_BCM_BUILD_OPENBCM=ON` link step consumes.
#
#   scripts/obcm_build.sh <target>
#     target = BCM56371 | TD3X
#
# Prerequisite:  git submodule update --init OpenBCM
set -euo pipefail

target="${1:-${SAI_BCM_TARGET:-BCM56371}}"
root="$(cd "$(dirname "$0")/.." && pwd)"
obcm="${root}/OpenBCM/sdk-6.5.27"

if [ ! -d "${obcm}" ]; then
    echo "obcm_build: OpenBCM not populated at ${obcm}" >&2
    echo "  run: git submodule update --init OpenBCM" >&2
    exit 1
fi

case "${target}" in
    BCM56371)
        make_local="${root}/cmake/Make.local.BCM56371" ;;
    TD3X)
        make_local="${root}/cmake/Make.local.TD3X" ;;
    *)
        echo "obcm_build: unknown target '${target}' (use BCM56371|TD3X)" >&2
        exit 1 ;;
esac

install -m 0644 "${make_local}" "${obcm}/make/Make.local"

echo "obcm_build: building ${target} in ${obcm}"
make -C "${obcm}" -j"$(nproc)"
echo "obcm_build: done. archives in ${obcm}/lib/"