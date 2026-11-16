cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "VITASDK is not set.")
  endif()
endif()

project(cordless_voice_poc C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2 -ffunction-sections -fdata-sections)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions -Wno-psabi")

option(BUILD_VOICE_FULL "Build full voice+TLS+DAVE suprx for size/link check" OFF)

# Stage 0: Kernel memory pool + display overlay helper (*KERNEL)
set(TAIHEN_K_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/taihen_k_stubs")
add_custom_command(
  OUTPUT "${TAIHEN_K_STUB_DIR}/libtaihenForKernel_stub.a"
  COMMAND ${VITA_LIBS_GEN} "${CMAKE_CURRENT_SOURCE_DIR}/taihen/taihen_kernel_stubs.yml" "${TAIHEN_K_STUB_DIR}"
  COMMAND make -C "${TAIHEN_K_STUB_DIR}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/taihen/taihen_kernel_stubs.yml"
  COMMENT "Building libtaihenForKernel_stub.a"
)
add_custom_target(taihen_k_stubs ALL DEPENDS "${TAIHEN_K_STUB_DIR}/libtaihenForKernel_stub.a")

add_executable(cordless_mem
  kernel/cordless_mem.c
)
add_dependencies(cordless_mem taihen_k_stubs)
target_link_directories(cordless_mem PRIVATE
  ${TAIHEN_K_STUB_DIR}
)
target_link_libraries(cordless_mem
  -Wl,--gc-sections
  -Wl,-u,module_start
  -Wl,-u,module_stop
  -Wl,-u,cordless_mem_alloc_pool
  -Wl,-u,cordless_mem_free_pool
  -Wl,-u,cordless_mem_probe
  -Wl,-u,cordless_mem_set_overlay
  -Wl,-u,cordless_mem_get_overlay
  -nostdlib
  taihenForKernel_stub
  SceSysmemForDriver_stub
  SceSysclibForDriver_stub
  SceThreadmgrForDriver_stub
  gcc
)
vita_create_self(cordless_mem.skprx cordless_mem
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/kernel/exports.yml
  STRIPPED
  UNSAFE
)
vita_create_stubs(cordless_mem_stubs cordless_mem ${CMAKE_CURRENT_SOURCE_DIR}/kernel/exports.yml KERNEL)

# Stage 1: Minimal *main loader + hot-reload (no SceNet/SceAudioIn stubs at boot)
add_executable(cordless_voice_poc
  src/loader.c
)
target_link_libraries(cordless_voice_poc
  -Wl,--gc-sections
  -Wl,-u,module_start
  -Wl,-u,module_stop
  -nostdlib
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceKernelModulemgr_stub
  SceIofilemgr_stub
  SceCtrl_stub
  SceSysmodule_stub
  gcc
)
vita_create_self(cordless_voice_poc.suprx cordless_voice_poc
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
  STRIPPED
  UNSAFE
)

# Stage 2: Core voice PoC (loaded after 3s + SCE_SYSMODULE_NET)
add_executable(cordless_voice_core
  src/main.c
)
add_dependencies(cordless_voice_core cordless_mem_stubs)
target_link_directories(cordless_voice_core PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/cordless_mem_stubs
)
target_link_libraries(cordless_voice_core
  -Wl,--gc-sections
  -Wl,-u,module_start
  -Wl,-u,module_stop
  -nostdlib
  cordless_mem_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceIofilemgr_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCtrl_stub
  SceNet_stub
  SceNetCtl_stub
  SceSysmem_stub
  SceSysmodule_stub
  gcc
)
vita_create_self(cordless_voice_core.suprx cordless_voice_core
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
  STRIPPED
  UNSAFE
)

# Stage 3: Tiny *ALL user-mode HUD overlay (~8K)
set(TAIHEN_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/taihen_stubs")
add_custom_command(
  OUTPUT "${TAIHEN_STUB_DIR}/libtaihen_stub.a"
  COMMAND ${VITA_LIBS_GEN} "${CMAKE_CURRENT_SOURCE_DIR}/taihen/taihen_stubs.yml" "${TAIHEN_STUB_DIR}"
  COMMAND make -C "${TAIHEN_STUB_DIR}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/taihen/taihen_stubs.yml"
  COMMENT "Building libtaihen_stub.a"
)
add_custom_target(taihen_stubs ALL DEPENDS "${TAIHEN_STUB_DIR}/libtaihen_stub.a")

add_executable(cordless_hud
  src/hud.c
)
add_dependencies(cordless_hud cordless_mem_stubs taihen_stubs)
target_link_directories(cordless_hud PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/cordless_mem_stubs
  ${TAIHEN_STUB_DIR}
)
target_link_libraries(cordless_hud
  -Wl,--gc-sections
  -Wl,-u,module_start
  -Wl,-u,module_stop
  -nostdlib
  cordless_mem_stub
  taihen_stub
  gcc
)
vita_create_self(cordless_hud.suprx cordless_hud
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
  STRIPPED
  UNSAFE
)

if(BUILD_VOICE_FULL)
  set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

  add_executable(cordless_voice_full
    src/full_entry.c
    src/voice_shim.c
    src/main_gw.c
    src/pool_alloc.c
    ${ROOT_DIR}/src/net_common.c
    ${ROOT_DIR}/src/util/b64.c
    ${ROOT_DIR}/src/tls/vita_mbedtls_port.c
    ${ROOT_DIR}/src/tls/ca_bundle.c
    ${ROOT_DIR}/src/tls/ws_client.c
    ${ROOT_DIR}/src/voice/voice.c
    ${ROOT_DIR}/src/voice/voice_crypto.c
    ${ROOT_DIR}/src/voice/voice_audio.c
    ${ROOT_DIR}/src/voice/dave_frame.c
    ${ROOT_DIR}/src/voice/dave.cpp
    ${ROOT_DIR}/third_party/speexdsp/preprocess.c
    ${ROOT_DIR}/third_party/speexdsp/fftwrap.c
    ${ROOT_DIR}/third_party/speexdsp/kiss_fft.c
    ${ROOT_DIR}/third_party/speexdsp/kiss_fftr.c
    ${ROOT_DIR}/third_party/speexdsp/filterbank.c
    ${ROOT_DIR}/third_party/speexdsp/smallft.c
  )
  set_source_files_properties(
    ${ROOT_DIR}/third_party/speexdsp/preprocess.c
    ${ROOT_DIR}/third_party/speexdsp/fftwrap.c
    ${ROOT_DIR}/third_party/speexdsp/kiss_fft.c
    ${ROOT_DIR}/third_party/speexdsp/kiss_fftr.c
    ${ROOT_DIR}/third_party/speexdsp/filterbank.c
    ${ROOT_DIR}/third_party/speexdsp/smallft.c
    PROPERTIES COMPILE_FLAGS "-DHAVE_CONFIG_H -w"
  )
  target_include_directories(cordless_voice_full PRIVATE
    ${ROOT_DIR}/src
    ${ROOT_DIR}/third_party
    ${ROOT_DIR}/third_party/speexdsp
    ${ROOT_DIR}/third_party/speexdsp/include
    ${ROOT_DIR}/third_party/mbedtls/include
    ${ROOT_DIR}/third_party/mlspp/include
    ${ROOT_DIR}/third_party/mlspp/lib/bytes/include
    ${ROOT_DIR}/third_party/mlspp/lib/tls_syntax/include
    ${ROOT_DIR}/third_party/mlspp/lib/hpke/include
  )
  add_dependencies(cordless_voice_full cordless_mem_stubs)
  target_link_directories(cordless_voice_full PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/cordless_mem_stubs
  )
  target_compile_options(cordless_voice_full PRIVATE -Os)
  target_compile_definitions(cordless_voice_full PRIVATE
    CORDLESS_PLUGIN=1
    VOICE_JOIN_ENABLE=1
    VOICE_DAVE_ENABLE=1
  )
  target_link_libraries(cordless_voice_full
    -Wl,--gc-sections
    -Wl,--section-start,.init_array=0x81200000
    -Wl,-u,module_start
    -Wl,-u,module_stop
    -Wl,--wrap=malloc
    -Wl,--wrap=free
    -Wl,--wrap=calloc
    -Wl,--wrap=realloc
    -Wl,--wrap=_malloc_r
    -Wl,--wrap=_free_r
    -Wl,--wrap=_calloc_r
    -Wl,--wrap=_realloc_r
    cordless_mem_stub
    jansson
    ${ROOT_DIR}/third_party/mlspp/lib/libmlspp_core.a
    ${ROOT_DIR}/third_party/mbedtls/lib/libmbedtls.a
    ${ROOT_DIR}/third_party/mbedtls/lib/libmbedx509.a
    ${ROOT_DIR}/third_party/mbedtls/lib/libmbedcrypto.a
    opus
    z
    pthread
    SceNet_stub SceNetCtl_stub
    SceLibKernel_stub SceKernelThreadMgr_stub SceIofilemgr_stub
    SceRtc_stub SceSysmem_stub SceSysmodule_stub
    SceAudio_stub SceAudioIn_stub SceCtrl_stub
    m
  )
  vita_create_self(cordless_voice_full.suprx cordless_voice_full
    CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
    STRIPPED
    UNSAFE
  )
endif()
