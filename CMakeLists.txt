cmake_minimum_required(VERSION 3.16)

project(libsai_bcm
    VERSION 0.2.0
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

# Backend selection:
#   mock    - link the mock BCM backend (dev/CI; no hardware required)
#   openbcm - link the real OpenBCM SDK archives (requires
#             SAI_BCM_BUILD_OPENBCM=ON and the SDK to be built)
set(SAI_BCM_BACKEND "mock" CACHE STRING "BCM backend to link")
set_property(CACHE SAI_BCM_BACKEND PROPERTY STRINGS "mock" "openbcm")

# Whether to build the OpenBCM SDK libraries from the submodule.
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

if(SAI_BCM_BACKEND STREQUAL "mock")
    set(SAI_BCM_BACKEND_IS_MOCK ON)
elseif(SAI_BCM_BACKEND STREQUAL "openbcm")
    set(SAI_BCM_BACKEND_IS_MOCK OFF)
    if(NOT SAI_BCM_BUILD_OPENBCM)
        message(FATAL_ERROR "SAI_BCM_BACKEND=openbcm requires "
                            "SAI_BCM_BUILD_OPENBCM=ON")
    endif()
else()
    message(FATAL_ERROR "Unsupported SAI_BCM_BACKEND '${SAI_BCM_BACKEND}' "
                        "(expected 'mock' or 'openbcm')")
endif()

message(STATUS "libsai-bcm: target=${SAI_BCM_TARGET} "
                "macro=${SAI_BCM_OBCM_CHIP_MACRO} stack=${SAI_BCM_OBCM_STACK} "
                "backend=${SAI_BCM_BACKEND}")

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(SAI_INC_DIR  ${CMAKE_SOURCE_DIR}/SAI/inc)
set(OBCM_INC     ${CMAKE_SOURCE_DIR}/OpenBCM/sdk-6.5.27/include)
set(OBCM_ROOT    ${CMAKE_SOURCE_DIR}/OpenBCM/sdk-6.5.27)

if(NOT EXISTS ${SAI_INC_DIR}/sai.h)
    message(FATAL_ERROR "SAI headers not found at ${SAI_INC_DIR}. "
                        "Run: git submodule update --init --recursive")
endif()

if(NOT EXISTS ${OBCM_INC}/bcm/port.h)
    message(FATAL_ERROR "OpenBCM headers not found at ${OBCM_INC}. "
                        "Run: git submodule update --init --recursive")
endif()

if(SAI_BCM_BUILD_OPENBCM AND NOT EXISTS ${OBCM_ROOT}/Makefile)
    message(FATAL_ERROR "OpenBCM sdk-6.5.27 not found at ${OBCM_ROOT}. "
                        "Run: git submodule update --init OpenBCM")
endif()

# ---------------------------------------------------------------------------
# Adapter objects (compiled against the REAL OpenBCM headers)
# ---------------------------------------------------------------------------
add_library(sai_bcm_obj OBJECT
    src/sai_bcm_api.c
    src/sai_bcm_oid.c
    src/sai_bcm_switch.c
    src/sai_bcm_port.c
    src/sai_bcm_bridge.c
    src/sai_bcm_vlan.c
    src/sai_bcm_lag.c
    src/sai_bcm_fdb.c
    src/sai_bcm_virtual_router.c
    src/sai_bcm_router_interface.c
    src/sai_bcm_nexthop.c
    src/sai_bcm_nexthopgroup.c
    src/sai_bcm_route.c
    src/sai_bcm_neighbor.c
    src/sai_bcm_hostif.c
    src/sai_bcm_acl.c
    src/sai_bcm_qosmap.c
    src/sai_bcm_stubs.c
    src/sai_bcm_log.c
)

set_target_properties(sai_bcm_obj PROPERTIES
    C_STANDARD          11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

target_include_directories(sai_bcm_obj
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${SAI_INC_DIR}
        ${OBCM_INC})

target_compile_definitions(sai_bcm_obj PRIVATE
    SAI_BCM_TARGET_NAME="${SAI_BCM_TARGET}"
    SAI_BCM_OBCM_CHIP_MACRO="${SAI_BCM_OBCM_CHIP_MACRO}"
    SAI_BCM_OBCM_STACK="${SAI_BCM_OBCM_STACK}"
    # OpenBCM gates bcm/l3.h (and the L3 interop sections of l2.h/vlan.h)
    # behind this feature flag; the SDK build passes it for XGS targets.
    INCLUDE_L3)

if(SAI_BCM_BACKEND_IS_MOCK)
    add_library(sai_bcm_mock STATIC tests/mock/mock_bcm.c)
    target_include_directories(sai_bcm_mock PRIVATE
        ${CMAKE_SOURCE_DIR}/tests/mock ${OBCM_INC})
    target_compile_definitions(sai_bcm_mock PRIVATE INCLUDE_L3)
    set_target_properties(sai_bcm_mock PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
endif()

# ---------------------------------------------------------------------------
# libsai.so - the shipped shared object
# ---------------------------------------------------------------------------
add_library(sai_bcm SHARED $<TARGET_OBJECTS:sai_bcm_obj>)

# Hide everything except the SAI entry points (sai_*). The linker version
# script (cmake/sai_bcm.ver) is the sole export authority: it lists the SAI
# ABI symbols as global and marks everything else local, so all internal
# symbols (and mock bcm_* symbols in mock builds) stay inside the .so.
set_target_properties(sai_bcm PROPERTIES
    OUTPUT_NAME             sai
    POSITION_INDEPENDENT_CODE ON)

target_link_options(sai_bcm PRIVATE
    "LINKER:--version-script=${CMAKE_SOURCE_DIR}/cmake/sai_bcm.ver")

if(SAI_BCM_BACKEND_IS_MOCK)
    target_link_libraries(sai_bcm PRIVATE sai_bcm_mock)
else()
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
# Static adapter library for unit tests (direct access to internals)
# ---------------------------------------------------------------------------
add_library(sai_bcm_static STATIC $<TARGET_OBJECTS:sai_bcm_obj>)
if(SAI_BCM_BACKEND_IS_MOCK)
    target_link_libraries(sai_bcm_static PUBLIC sai_bcm_mock)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

function(sai_bcm_unit_test name)
    add_executable(${name} tests/${name}.c)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${SAI_INC_DIR}
        ${OBCM_INC}
        ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(${name} PRIVATE sai_bcm_static)
    target_compile_definitions(${name} PRIVATE INCLUDE_L3)
    set_target_properties(${name} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

if(SAI_BCM_BACKEND_IS_MOCK)
    sai_bcm_unit_test(test_oid)
    sai_bcm_unit_test(test_switch)
    sai_bcm_unit_test(test_port)
    sai_bcm_unit_test(test_l2)
    sai_bcm_unit_test(test_l3)
    sai_bcm_unit_test(test_p4)
endif()

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