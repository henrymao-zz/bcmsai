cmake_minimum_required(VERSION 3.16)

project(libsai_bcm
    VERSION 0.1.0
    LANGUAGES C
    DESCRIPTION "Vendor SAI adapter on top of Broadcom OpenBCM SDK 6.5.27")

# ---------------------------------------------------------------------------
# Target selection
# ---------------------------------------------------------------------------
# SAI_BCM_TARGET selects the OpenBCM device family / driver stack to build
# against. The same libsai source compiles for either; only the OpenBCM
# build macro and the link-time archive set differ.
#
#   BCM56371  -> BCM_56370_A0=1, classic ESW (Firebolt6)        [default]
#   TD3X      -> BCM_56880_A0=1, LTSW / XFS (Trident 3-X)
set(SAI_BCM_TARGET "BCM56371" CACHE STRING "OpenBCM target device family")
set_property(CACHE SAI_BCM_TARGET PROPERTY STRINGS "BCM56371" "TD3X")

# Whether to build the OpenBCM SDK libraries from the submodule.
# P0 ships a stub libsai.so that links nothing from OpenBCM yet; the real
# bcm_* integration starts in P1. Leave OFF for the P0 smoke build.
option(SAI_BCM_BUILD_OPENBCM "Build OpenBCM SDK 6.5.27 libraries" OFF)

# Map target -> OpenBCM build macro
if(SAI_BCM_TARGET STREQUAL "BCM56371")
    set(SAI_BCM_OBCM_CHIP_MACRO "BCM_56370_A0")
    set(SAI_BCM_OBCM_FAMILY     "firebolt6")
    set(SAI_BCM_OBCM_STACK      "ESW")
elseif(SAI_BCM_TARGET STREQUAL "TD3X")
    set(SAI_BCM_OBCM_CHIP_MACRO "BCM_56880_A0")
    set(SAI_BCM_OBCM_FAMILY     "trident3x")
    set(SAI_BCM_OBCM_STACK      "LTSW")
else()
    message(FATAL_ERROR "Unsupported SAI_BCM_TARGET '${SAI_BCM_TARGET}' "
                        "(expected 'BCM56371' or 'TD3X')")
endif()

message(STATUS "libsai-bcm: target=${SAI_BCM_TARGET} "
                "macro=${SAI_BCM_OBCM_CHIP_MACRO} stack=${SAI_BCM_OBCM_STACK}")

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(SAI_INC_DIR  ${CMAKE_SOURCE_DIR}/SAI/inc)
set(OBCM_ROOT    ${CMAKE_SOURCE_DIR}/OpenBCM/sdk-6.5.27)

if(NOT EXISTS ${SAI_INC_DIR}/sai.h)
    message(FATAL_ERROR "SAI headers not found at ${SAI_INC_DIR}. "
                        "Run: git submodule update --init --recursive")
endif()

if(SAI_BCM_BUILD_OPENBCM AND NOT EXISTS ${OBCM_ROOT}/Makefile)
    message(FATAL_ERROR "OpenBCM sdk-6.5.27 not found at ${OBCM_ROOT}. "
                        "Run: git submodule update --init OpenBCM")
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(sai_bcm SHARED
    src/sai_bcm_api.c
    src/sai_bcm_stubs.c
    src/sai_bcm_log.c
)

target_include_directories(sai_bcm
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${SAI_INC_DIR})

# Hide everything except the SAI entry points (sai_*). The linker version
# script (cmake/sai_bcm.ver) is the sole export authority: it lists the SAI
# ABI symbols as global and marks everything else local, so all internal
# symbols and any future BCM symbols stay hidden inside the shared object.
set_target_properties(sai_bcm PROPERTIES
    OUTPUT_NAME             sai
    C_STANDARD              11
    C_STANDARD_REQUIRED     ON
    POSITION_INDEPENDENT_CODE ON)

target_link_options(sai_bcm PRIVATE
    "LINKER:--version-script=${CMAKE_SOURCE_DIR}/cmake/sai_bcm.ver")

if(SAI_BCM_BUILD_OPENBCM)
    # P1+ integration point: link the OpenBCM archive set selected by target.
    if(SAI_BCM_TARGET STREQUAL "BCM56371")
        target_link_libraries(sai_bcm PRIVATE
            ${OBCM_ROOT}/lib/libfirebolt6.a
            ${OBCM_ROOT}/lib/libfirebolt.a
            ${OBCM_ROOT}/lib/libbradley.a
            ${OBCM_ROOT}/lib/libherc.a
            ${OBCM_ROOT}/lib/libhumv.a
            ${OBCM_ROOT}/lib/libscorpion.a
            ${OBCM_ROOT}/lib/libtrx.a
            ${OBCM_ROOT}/lib/libtriumph.a
            ${OBCM_ROOT}/lib/libtriumph2.a
            ${OBCM_ROOT}/lib/libtriumph3.a
            ${OBCM_ROOT}/lib/libbcm.a
            ${OBCM_ROOT}/lib/libcmn.a)
    else()
        target_link_libraries(sai_bcm PRIVATE
            ${OBCM_ROOT}/lib/libltsw.a
            ${OBCM_ROOT}/lib/libxfs.a
            ${OBCM_ROOT}/lib/libsoc_ltsw.a
            ${OBCM_ROOT}/lib/libbcm.a
            ${OBCM_ROOT}/lib/libcmn.a)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

add_executable(smoke_load tests/smoke_load.c)
target_link_libraries(smoke_load PRIVATE ${CMAKE_DL_LIBS})
target_include_directories(smoke_load PRIVATE ${SAI_INC_DIR})
add_test(NAME smoke_load COMMAND smoke_load
    $<TARGET_FILE:sai_bcm>)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS sai_bcm
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY SAI/inc/ DESTINATION include/sai)