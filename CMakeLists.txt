cmake_minimum_required(VERSION 3.16)
project(rokkdoxx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
  add_compile_options(/W3 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

option(ROKK_ENABLE_OPENCL "Build the OpenCL compute worker" ON)

find_package(Threads REQUIRED)

# --- generation core (Minecraft 26.2 bedrock floor) ------------------------
add_library(rokkdoxx_gen STATIC src/gen/bedrock.cpp)
target_include_directories(rokkdoxx_gen PUBLIC src)

# --- search service (orchestrator + workers + in-process handle) ----------
set(SVC_SOURCES
  src/svc/search_service.cpp
  src/svc/workers.cpp
  src/svc/client.cpp
  src/svc/pattern_io.cpp)

set(ROKK_HAVE_OPENCL FALSE)
if(ROKK_ENABLE_OPENCL)
  find_package(OpenCL QUIET)
  if(OpenCL_FOUND)
    set(ROKK_HAVE_OPENCL TRUE)
    list(APPEND SVC_SOURCES src/svc/opencl_worker.cpp)
    message(STATUS "OpenCL found: ${OpenCL_LIBRARIES}")
    # Embed the kernel text so the binary is self-contained / relocatable.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/gen/bedrock_core.h" ROKK_BEDROCK_CORE)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/cl/search_tile.cl" ROKK_SEARCH_TILE)
    configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/src/svc/kernel_sources.h.in"
      "${CMAKE_BINARY_DIR}/generated/kernel_sources.h"
      @ONLY)
  else()
    message(STATUS "OpenCL not found -- building CPU worker only")
  endif()
endif()

add_library(rokksvc STATIC ${SVC_SOURCES})
target_include_directories(rokksvc PUBLIC src)
target_link_libraries(rokksvc PUBLIC rokkdoxx_gen Threads::Threads)
if(ROKK_HAVE_OPENCL)
  target_compile_definitions(rokksvc PUBLIC ROKK_ENABLE_OPENCL)
  target_link_libraries(rokksvc PUBLIC OpenCL::OpenCL)
  target_include_directories(rokksvc PRIVATE "${CMAKE_BINARY_DIR}/generated")
endif()

# --- tools ---------------------------------------------------------------
add_executable(dump_bedrock tools/dump_bedrock.cpp)
target_link_libraries(dump_bedrock PRIVATE rokkdoxx_gen)

add_executable(rokksearch tools/rokksearch.cpp)
target_link_libraries(rokksearch PRIVATE rokksvc)

# rokktui uses a POSIX raw-terminal backend (<termios.h>); no Windows build yet.
if(NOT WIN32)
  add_executable(rokktui tools/rokktui.cpp)
  target_link_libraries(rokktui PRIVATE rokksvc)
endif()

# --- tests -------------------------------------------------------------
enable_testing()

add_executable(test_bedrock tests/test_bedrock.cpp)
target_link_libraries(test_bedrock PRIVATE rokkdoxx_gen)
add_test(NAME unit COMMAND test_bedrock)

add_executable(test_search tests/test_search.cpp)
target_link_libraries(test_search PRIVATE rokksvc)
add_test(NAME search COMMAND test_search)

if(ROKK_HAVE_OPENCL)
  add_executable(test_gpu tests/test_gpu.cpp)
  target_link_libraries(test_gpu PRIVATE rokksvc)
  add_test(NAME gpu COMMAND test_gpu)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME differential
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff_test.py
                   $<TARGET_FILE:dump_bedrock>)
endif()
