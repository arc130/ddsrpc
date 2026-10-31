cmake_minimum_required(VERSION 3.16)
project(ddsrpc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Network SerialPort)

option(DDSRPC_BUILD_TESTS "Build ddsrpc tests" ON)
option(DDSRPC_BUILD_EXAMPLES "Build ddsrpc examples" OFF)

set(DDSRPC_ASYNCTASK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/AsyncTask" CACHE PATH "AsyncTask root")
set(DDSRPC_TRANSPORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Transport" CACHE PATH "Transport root")
set(ASYNCTASK_DIR "${DDSRPC_ASYNCTASK_DIR}" CACHE PATH "" FORCE)
set(TRANSPORT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TRANSPORT_BUILD_PERF OFF CACHE BOOL "" FORCE)
add_subdirectory("${DDSRPC_TRANSPORT_DIR}" third_party/transport)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
if(DDSRPC_BUILD_TESTS)
  add_subdirectory(
    "${DDSRPC_TRANSPORT_DIR}/third_party/googletest"
    third_party/googletest EXCLUDE_FROM_ALL)
endif()

enable_testing()
add_subdirectory(tools/ddsrpcgen)
if(DDSRPC_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(DDSRPC_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
  add_subdirectory(examples)
endif()
