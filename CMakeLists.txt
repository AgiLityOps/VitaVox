cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

# The app version: bump by one for every build (see below).
set(APP_VERSION "00.39")
project(vita_vox VERSION ${APP_VERSION} LANGUAGES C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Vita Vox")
set(VITA_TITLEID "VVOX00001")
# One version number everywhere (since 00.31): the in-app version, param.sfo
# APP_VER, the VPK name (vita_vox_v<APP_VERSION>.vpk), commit messages
# and the README. Format "MM.mm" (what param.sfo accepts). Every build bumps
# the last two digits: 00.31 -> 00.32 -> ... -> 00.99 -> 01.00.
if(NOT APP_VERSION MATCHES "^[0-9][0-9]\\.[0-9][0-9]$")
  message(FATAL_ERROR "APP_VERSION '${APP_VERSION}' must look like 00.31")
endif()
set(VITA_VERSION "${APP_VERSION}")
message(STATUS "Vita Vox ${APP_VERSION} (param.sfo APP_VER ${VITA_VERSION})")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1")
# ATTRIBUTE: no communication zone (0x8000, the SDK default), keep running when
# deactivated (0x8) and no system suspend (0x80000), so a book plays on in the
# background and through the power button, as ElevenMPV-A does
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE=0x88008")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wno-unused-parameter -Wno-sign-compare -std=gnu11")

set(FFMPEG_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "project-local FFmpeg prefix")
if(NOT EXISTS "${FFMPEG_PREFIX}/lib/libavcodec.a")
  message(FATAL_ERROR "FFmpeg not found at ${FFMPEG_PREFIX}. Run scripts/build_ffmpeg.sh first.")
endif()

file(GLOB SOURCES
  src/core/*.c src/ui/*.c src/vita/*.c
  vendor/cjson/cJSON.c vendor/sonic/sonic.c)

add_executable(${PROJECT_NAME} ${SOURCES})
target_compile_definitions(${PROJECT_NAME} PRIVATE APP_VERSION="${APP_VERSION}")
# 00.37: reduced-scale JPEG cover decode with the SDK's libjpeg-turbo (linked below)
target_compile_definitions(${PROJECT_NAME} PRIVATE VA_HAVE_LIBJPEG)
# Test builds for the emulator: cmake -DAUTOPILOT=ON (never in releases)
option(AUTOPILOT "scripted input from ux0:data/VitaVox/autopilot.txt" OFF)
if(AUTOPILOT)
  target_compile_definitions(${PROJECT_NAME} PRIVATE VITAVOX_AUTOPILOT)
endif()
target_include_directories(${PROJECT_NAME} PRIVATE
  ${FFMPEG_PREFIX}/include
  src/core src/ui src/vita vendor/cjson vendor/sonic vendor/stb)

target_link_libraries(${PROJECT_NAME}
  vita2d freetype png jpeg bz2 z
  ${FFMPEG_PREFIX}/lib/libavformat.a
  ${FFMPEG_PREFIX}/lib/libavcodec.a
  ${FFMPEG_PREFIX}/lib/libswresample.a
  ${FFMPEG_PREFIX}/lib/libavutil.a
  z
  m pthread
  SceDisplay_stub SceGxm_stub SceCtrl_stub SceTouch_stub SceAudio_stub
  ScePower_stub SceSysmodule_stub SceCommonDialog_stub SceAppUtil_stub SceAppMgr_stub
  SceRtc_stub SceIme_stub
  SceKernelThreadMgr_stub SceKernelModulemgr_stub SceIofilemgr_stub SceProcessmgr_stub SceLibKernel_stub SceSysmem_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}_v${APP_VERSION}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE assets/fonts/Manrope-500.ttf assets/Manrope-500.ttf
  FILE assets/fonts/Manrope-700.ttf assets/Manrope-700.ttf
  FILE assets/logo.png assets/logo.png
)

# Keep a copy of every built VPK outside build/ (which is wiped often).
# releases/ is gitignored; *.vpk is ignored as well.
set(_vpk_name ${PROJECT_NAME}_v${APP_VERSION}.vpk)
add_custom_command(TARGET ${_vpk_name}-vpk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/releases
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_BINARY_DIR}/${_vpk_name}
          ${CMAKE_SOURCE_DIR}/releases/${_vpk_name}
  COMMENT "Copying ${_vpk_name} to releases/"
)
