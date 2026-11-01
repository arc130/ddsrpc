add_executable(hello hello/main.cpp)
ddsrpc_add_idl(hello
  ROLES "service;client"
  NAMESPACE demo
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/hello/hello.idl")
target_link_libraries(hello PRIVATE
  Qt5::Core Qt5::Network Qt5::SerialPort ddsrpc::runtime)
