add_executable(ddsrpcgen_tests
  generator/codegen_test.cpp
  generator/import_resolver_test.cpp
  generator/lexer_test.cpp
  generator/parser_test.cpp
  generator/semantic_test.cpp
)
target_link_libraries(ddsrpcgen_tests PRIVATE ddsrpcgen_lib GTest::gtest_main Qt5::Core)
add_test(NAME ddsrpcgen_tests COMMAND ddsrpcgen_tests)

add_executable(ddsrpc_runtime_tests
  runtime/runtime_test_main.cpp
  runtime/error_wire_test.cpp
  runtime/task_group_test.cpp
  runtime/service_base_test.cpp
)
# AsyncTask 头文件来自根级 ${DDSRPC_ASYNCTASK_DIR}（即 transport 实际链接的
# d231d64 子模块）；Transport 自带的 third_party/AsyncTask 在本工作树未检出。
target_include_directories(ddsrpc_runtime_tests PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/src/ddsrpc"
  "${DDSRPC_ASYNCTASK_DIR}/coro"
  "${DDSRPC_TRANSPORT_DIR}/include"
)
target_link_libraries(ddsrpc_runtime_tests PRIVATE
  ddsrpc_runtime GTest::gtest Qt5::Core Qt5::Network)
add_test(NAME ddsrpc_runtime_tests COMMAND ddsrpc_runtime_tests)
