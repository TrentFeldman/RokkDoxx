cmake_minimum_required(VERSION 3.16)
project(rokkdoxx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra)

option(ROKK_ENABLE_OPENCL "Build the OpenCL compute worker" ON)

find_package(Threads REQUIRED)

# --- generation core (Minecraft 26.2 bedrock floor) ------------------------
add_library(rokkdoxx_gen STATIC src/gen/bedrock.cpp)
target_include_directories(rokkdoxx_gen PUBLIC src)

# --- search service (orchestrator + workers + client) ---------------------
set(SVC_SOURCES
  src/svc/search_service.cpp
  src/svc/workers.cpp
  src/svc/client.cpp
  src/svc/protocol.cpp
  src/svc/daemon.cpp
  src/svc/pattern_io.cpp)

set(ROKK_HAVE_OPENCL FALSE)
if(ROKK_ENABLE_OPENCL)
  find_package(OpenCL QUIET)
  if(OpenCL_FOUND)
    set(ROKK_HAVE_OPENCL TRUE)
    list(APPEND SVC_SOURCES src/svc/opencl_worker.cpp)
    message(STATUS "OpenCL found: ${OpenCL_LIBRARIES}")
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
  target_compile_definitions(rokksvc PRIVATE
    ROKK_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

# --- tools ---------------------------------------------------------------
add_executable(dump_bedrock tools/dump_bedrock.cpp)
target_link_libraries(dump_bedrock PRIVATE rokkdoxx_gen)

add_executable(rokktui tools/rokktui.cpp)
target_link_libraries(rokktui PRIVATE rokksvc)

add_executable(rokksearch tools/rokksearch.cpp)
target_link_libraries(rokksearch PRIVATE rokksvc)

add_executable(rokkd tools/rokkd.cpp)
target_link_libraries(rokkd PRIVATE rokksvc)

# --- tests -------------------------------------------------------------
enable_testing()

add_executable(test_bedrock tests/test_bedrock.cpp)
target_link_libraries(test_bedrock PRIVATE rokkdoxx_gen)
add_test(NAME unit COMMAND test_bedrock)

add_executable(test_search tests/test_search.cpp)
target_link_libraries(test_search PRIVATE rokksvc)
add_test(NAME search COMMAND test_search)

add_executable(test_daemon tests/test_daemon.cpp)
target_link_libraries(test_daemon PRIVATE rokksvc)
add_test(NAME daemon COMMAND test_daemon)

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
