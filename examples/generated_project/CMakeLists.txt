cmake_minimum_required(VERSION 3.16)
project(ddsrpc_generated_project LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

# When this directory is built from the ddsrpc tree, ddsrpc::runtime already
# exists. Building it standalone pulls in the parent project.
if(NOT TARGET ddsrpc::runtime)
  find_package(Qt5 5.12 REQUIRED COMPONENTS Core Network SerialPort)
  set(DDSRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(DDSRPC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.."
                   "${CMAKE_CURRENT_BINARY_DIR}/ddsrpc")
endif()

# generated/ is checked in, so building this example does not run ddsrpcgen.
add_executable(generated_project_demo
  main.cpp
  generated/greeter.ddsrpc.service.cpp
  generated/greeter.ddsrpc.client.cpp)
target_include_directories(generated_project_demo PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_link_libraries(generated_project_demo PRIVATE
  Qt5::Core Qt5::Network Qt5::SerialPort ddsrpc::runtime)
