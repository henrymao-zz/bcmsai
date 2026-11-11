# syntax=docker/dockerfile:1.6
# Build & test image for libsai-bcm.
#
# P0: builds the stub libsai.so (no OpenBCM link). From P1 onward, set
# SAI_BCM_BUILD_OPENBCM=ON and populate the OpenBCM submodule to link the
# real driver archive set.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        git \
        ca-certificates \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src/libsai-bcm

ARG SAI_BCM_TARGET=BCM56371
ENV SAI_BCM_TARGET=${SAI_BCM_TARGET}

RUN cmake -S /src/libsai-bcm -B /src/libsai-bcm/build \
        -G Ninja \
        -DSAI_BCM_TARGET=${SAI_BCM_TARGET} \
        -DSA_BCM_BUILD_OPENBCM=OFF \
    && cmake --build /src/libsai-bcm/build \
    && ctest --test-dir /src/libsai-bcm/build --output-on-failure

WORKDIR /src/libsai-bcm