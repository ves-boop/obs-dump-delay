# Dump Delay for OBS Studio - Copyright (C) 2026 Valerio Enrico Salvo (Ves)
# SPDX-License-Identifier: GPL-2.0-or-later
# Based on obs-plugintemplate (https://github.com/obsproject/obs-plugintemplate).

cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs OBS::obs-frontend-api)

# FFmpeg headers/import libs come from the obs-deps package the template
# downloads into .deps/. At runtime the plugin uses the FFmpeg DLLs that ship
# with OBS, so build against the obs-deps version matching your OBS release.
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h REQUIRED)
find_library(AVFORMAT_LIBRARY avformat REQUIRED)
find_library(AVCODEC_LIBRARY avcodec REQUIRED)
find_library(AVUTIL_LIBRARY avutil REQUIRED)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY})

# Qt for the dock; also from obs-deps, matching the Qt that ships with OBS.
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/dump-delay-control.c
    src/dump-delay-ui.cpp
    src/dump-delay-output.c
    src/dump-delay-service.c
    src/dump-delay.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
