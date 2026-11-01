add_executable(hello hello/main.cpp)
ddsrpc_add_idl(hello
  ROLES "service;client"
  NAMESPACE demo
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/hello/hello.idl")
target_link_libraries(hello PRIVATE
  Qt5::Core Qt5::Network Qt5::SerialPort ddsrpc::runtime)

# A standalone consumer project with checked-in generator output.
add_subdirectory(generated_project)

if(TARGET fastdds)
  add_executable(fastdds_check fastdds_check/main.cpp)
  ddsrpc_add_idl(fastdds_check
    ROLES "service;client"
    NAMESPACE demo
    IDL "${CMAKE_CURRENT_SOURCE_DIR}/hello/hello.idl")
  target_link_libraries(fastdds_check PRIVATE
    Qt5::Core Qt5::Network Qt5::SerialPort ddsrpc::runtime)

  add_executable(fastdds_reconnect fastdds_reconnect/main.cpp)
  ddsrpc_add_idl(fastdds_reconnect
    ROLES "service;client"
    NAMESPACE demo
    IDL "${CMAKE_CURRENT_SOURCE_DIR}/fastdds_reconnect/reconnect.idl")
  target_link_libraries(fastdds_reconnect PRIVATE
    Qt5::Core Qt5::Network Qt5::SerialPort ddsrpc::runtime)
else()
  message(STATUS "Fast DDS not available: skipping fastdds examples")
endif()
