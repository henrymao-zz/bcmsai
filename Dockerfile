# syntax=docker/dockerfile:1.6
# Build & test image for libsai-bcm.
#
# Default build compiles against the real OpenBCM sdk-6.5.27 headers from the
# submodule and links the mock BCM backend (no hardware required). From P1+
# onward, production builds set SAI_BCM_BACKEND=openbcm +
# SAI_BCM_BUILD_OPENBCM=ON to link the real driver archives.
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
ARG SAI_BCM_BACKEND=mock
ENV SAI_BCM_TARGET=${SAI_BCM_TARGET}

RUN cmake -S /src/libsai-bcm -B /src/libsai-bcm/build \
        -G Ninja \
        -DSAI_BCM_TARGET=${SAI_BCM_TARGET} \
        -DSAI_BCM_BACKEND=${SAI_BCM_BACKEND} \
        -DSA_BCM_BUILD_OPENBCM=OFF \
    && cmake --build /src/libsai-bcm/build \
    && ctest --test-dir /src/libsai-bcm/build --output-on-failure

WORKDIR /src/libsai-bcm