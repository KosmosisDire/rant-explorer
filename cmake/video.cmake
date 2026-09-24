# The video decoders behind a VideoFrame card, all static and C only, no assembler needed.
# openh264 and dav1d build with Meson upstream, so their decoder halves are compiled here
# from the fetched trees. libde265 has a CMake build of its own.

# H.264, BSD 2 clause. The decoder and the common code it shares with the encoder.
CPMAddPackage(NAME openh264
  GITHUB_REPOSITORY cisco/openh264
  GIT_TAG v2.6.0
  VERSION 2.6.0
  DOWNLOAD_ONLY YES)
file(GLOB OPENH264_SOURCES
     "${openh264_SOURCE_DIR}/codec/common/src/*.cpp"
     "${openh264_SOURCE_DIR}/codec/decoder/core/src/*.cpp"
     "${openh264_SOURCE_DIR}/codec/decoder/plus/src/*.cpp")
add_library(openh264_decoder STATIC ${OPENH264_SOURCES})
target_include_directories(openh264_decoder
  PUBLIC  "${openh264_SOURCE_DIR}/codec/api/wels"
  PRIVATE "${openh264_SOURCE_DIR}/codec/common/inc"
          "${openh264_SOURCE_DIR}/codec/decoder/core/inc"
          "${openh264_SOURCE_DIR}/codec/decoder/plus/inc")
if(MSVC)
  target_compile_options(openh264_decoder PRIVATE /W0)
else()
  target_compile_options(openh264_decoder PRIVATE -w)
  find_package(Threads REQUIRED)
  target_link_libraries(openh264_decoder PRIVATE Threads::Threads)
endif()

# AV1, BSD 2 clause. The template sources compile once per bit depth. config.h is what
# Meson would write for a build with no assembler: every ARCH_ flag off.
CPMAddPackage(NAME dav1d
  GITHUB_REPOSITORY videolan/dav1d
  GIT_TAG 1.5.4
  VERSION 1.5.4
  DOWNLOAD_ONLY YES)
set(DAV1D_GEN "${CMAKE_BINARY_DIR}/dav1d-config")
set(DAV1D_CONFIG "")
foreach(_flag ARCH_AARCH64 ARCH_ARM ARCH_LOONGARCH ARCH_LOONGARCH32 ARCH_LOONGARCH64
        ARCH_PPC64LE ARCH_RISCV ARCH_RV32 ARCH_RV64 ARCH_X86 ARCH_X86_32 ARCH_X86_64
        HAVE_ASM TRIM_DSP_FUNCTIONS CONFIG_LOG ENDIANNESS_BIG HAVE_GETAUXVAL
        HAVE_ELF_AUX_INFO HAVE_PTHREAD_NP_H HAVE_PTHREAD_GETAFFINITY_NP
        HAVE_PTHREAD_SETAFFINITY_NP HAVE_PTHREAD_SET_NAME_NP HAVE_DLSYM
        HAVE_MEMALIGN HAVE_ALIGNED_ALLOC)
  string(APPEND DAV1D_CONFIG "#define ${_flag} 0\n")
endforeach()
string(APPEND DAV1D_CONFIG "#define CONFIG_8BPC 1\n#define CONFIG_16BPC 1\n")
if(WIN32)
  string(APPEND DAV1D_CONFIG "#define _WIN32_WINNT 0x0601\n#define UNICODE 1\n#define _UNICODE 1\n"
         "#define _CRT_DECLARE_NONSTDC_NAMES 1\n#define HAVE_UNISTD_H 0\n"
         "#define HAVE_POSIX_MEMALIGN 0\n#define HAVE_PTHREAD_SETNAME_NP 0\n")
else()
  string(APPEND DAV1D_CONFIG "#define HAVE_UNISTD_H 1\n#define HAVE_POSIX_MEMALIGN 1\n"
         "#define HAVE_PTHREAD_SETNAME_NP 1\n")
endif()
file(CONFIGURE OUTPUT "${DAV1D_GEN}/config.h" CONTENT "${DAV1D_CONFIG}")
file(CONFIGURE OUTPUT "${DAV1D_GEN}/vcs_version.h" CONTENT "#define DAV1D_VERSION \"1.5.4\"\n")

set(_d "${dav1d_SOURCE_DIR}/src")
set(DAV1D_SOURCES cdf.c cpu.c ctx.c data.c decode.c dequant_tables.c getbits.c intra_edge.c
    itx_1d.c lf_mask.c lib.c log.c mem.c msac.c obu.c pal.c picture.c qm.c ref.c refmvs.c
    scan.c tables.c thread_task.c warpmv.c wedge.c)
set(DAV1D_TEMPLATES cdef_apply_tmpl.c cdef_tmpl.c fg_apply_tmpl.c filmgrain_tmpl.c
    ipred_prepare_tmpl.c ipred_tmpl.c itx_tmpl.c lf_apply_tmpl.c loopfilter_tmpl.c
    looprestoration_tmpl.c lr_apply_tmpl.c mc_tmpl.c recon_tmpl.c)
list(TRANSFORM DAV1D_SOURCES PREPEND "${_d}/")
list(TRANSFORM DAV1D_TEMPLATES PREPEND "${_d}/")
if(WIN32)
  list(APPEND DAV1D_SOURCES "${_d}/win32/thread.c")
endif()

set(DAV1D_INCLUDES "${DAV1D_GEN}" "${dav1d_SOURCE_DIR}" "${dav1d_SOURCE_DIR}/include"
    "${dav1d_SOURCE_DIR}/include/dav1d")
if(MSVC)
  list(APPEND DAV1D_INCLUDES "${dav1d_SOURCE_DIR}/include/compat/msvc")
endif()
foreach(_bits 8 16)
  add_library(dav1d_bits${_bits} OBJECT ${DAV1D_TEMPLATES})
  target_compile_definitions(dav1d_bits${_bits} PRIVATE BITDEPTH=${_bits})
  target_include_directories(dav1d_bits${_bits} PRIVATE ${DAV1D_INCLUDES})
endforeach()
add_library(dav1d STATIC ${DAV1D_SOURCES}
  $<TARGET_OBJECTS:dav1d_bits8> $<TARGET_OBJECTS:dav1d_bits16>)
target_include_directories(dav1d PRIVATE ${DAV1D_INCLUDES}
                           PUBLIC "${dav1d_SOURCE_DIR}/include")
foreach(_t dav1d dav1d_bits8 dav1d_bits16)
  set_target_properties(${_t} PROPERTIES C_STANDARD 11)
  if(MSVC)
    target_compile_options(${_t} PRIVATE /W0)
  else()
    target_compile_options(${_t} PRIVATE -w)
    target_compile_definitions(${_t} PRIVATE _GNU_SOURCE)
  endif()
endforeach()
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(dav1d PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# H.265, LGPL 3. Its own CMake: the library only, no SDL player and no encoder.
CPMAddPackage(NAME libde265
  GITHUB_REPOSITORY strukturag/libde265
  GIT_TAG v1.0.16
  VERSION 1.0.16
  EXCLUDE_FROM_ALL YES
  OPTIONS "BUILD_SHARED_LIBS OFF" "ENABLE_SDL OFF" "ENABLE_DECODER OFF" "ENABLE_ENCODER OFF")
target_include_directories(de265 INTERFACE "$<BUILD_INTERFACE:${libde265_SOURCE_DIR}>"
                           "$<BUILD_INTERFACE:${libde265_BINARY_DIR}>")
target_compile_definitions(de265 INTERFACE LIBDE265_STATIC_BUILD)
# Its warnings muted, those raised inside the MSVC standard headers too, which set their own level.
foreach(_t de265 x86 x86_sse encoder algo)
  if(TARGET ${_t} AND MSVC)
    target_compile_options(${_t} PRIVATE /W0 /wd4244 /wd4267)
  elseif(TARGET ${_t})
    target_compile_options(${_t} PRIVATE -w)
  endif()
endforeach()
