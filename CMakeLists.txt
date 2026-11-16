cmake_minimum_required(VERSION 3.15)
project(MODX_LiveSet_Controller VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(MSVC)
  # Static runtime: no VC++ redistributable needed on other machines.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(MODX_COPY_AFTER_BUILD "Copy the .clap into your user CLAP folder after building" OFF)
option(MODX_SYSEX_WITH_FRAMING "Send SysEx buffer including F0 ... F7 (see README)" ON)
set(CLAP_SDK_DIR "" CACHE PATH "Path to a local clap SDK checkout (leave empty to download)")

# ---- CLAP headers ----
if(CLAP_SDK_DIR)
  add_library(clap INTERFACE)
  target_include_directories(clap INTERFACE "${CLAP_SDK_DIR}/include")
else()
  include(FetchContent)
  FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.9
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(clap)
endif()

# ---- Plugin ----
add_library(modx_liveset MODULE src/plugin.cpp)
target_link_libraries(modx_liveset PRIVATE clap)
if(MODX_SYSEX_WITH_FRAMING)
  target_compile_definitions(modx_liveset PRIVATE MODX_SYSEX_WITH_FRAMING=1)
else()
  target_compile_definitions(modx_liveset PRIVATE MODX_SYSEX_WITH_FRAMING=0)
endif()

set_target_properties(modx_liveset PROPERTIES
  OUTPUT_NAME "MODX Live Set Controller"
  PREFIX ""
  SUFFIX ".clap")

if(APPLE)
  set_target_properties(modx_liveset PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "clap"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.harleyganiere.modx-liveset-controller"
    MACOSX_BUNDLE_BUNDLE_NAME "MODX Live Set Controller"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
endif()

if(MODX_COPY_AFTER_BUILD)
  if(WIN32)
    set(_dest "$ENV{LOCALAPPDATA}/Programs/Common/CLAP")
  elseif(APPLE)
    set(_dest "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")
  else()
    set(_dest "$ENV{HOME}/.clap")
  endif()
  if(APPLE)
    add_custom_command(TARGET modx_liveset POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "$<TARGET_BUNDLE_DIR:modx_liveset>"
              "${_dest}/MODX Live Set Controller.clap"
      COMMENT "Copying to ${_dest}")
  else()
    add_custom_command(TARGET modx_liveset POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_dest}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:modx_liveset>" "${_dest}"
      COMMENT "Copying to ${_dest}")
  endif()
endif()
