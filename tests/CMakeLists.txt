add_executable(ddsrpcgen_tests
  generator/codegen_test.cpp
  generator/import_resolver_test.cpp
  generator/lexer_test.cpp
  generator/parser_test.cpp
  generator/semantic_test.cpp
)
target_link_libraries(ddsrpcgen_tests PRIVATE ddsrpcgen_lib GTest::gtest_main Qt5::Core)
add_test(NAME ddsrpcgen_tests COMMAND ddsrpcgen_tests)

# Compile/link smoke for the generated service and client. Task 12 replaces
# this local custom command with ddsrpc_add_idl (keeping the fixture and this
# executable, so the accepted builtin/container and hostile-name policies stay
# under the compiler). The generator's depfile is declared as a byproduct so
# Makefiles do not see an undeclared output.
set(DDSRPC_SMOKE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_smoke")
set(DDSRPC_SMOKE_IDL "${CMAKE_CURRENT_SOURCE_DIR}/idl/builtin_types.idl")
set(DDSRPC_SMOKE_DEPFILE "${DDSRPC_SMOKE_DIR}/ddsrpc-smoke.d")
set(DDSRPC_SMOKE_TYPES "${DDSRPC_SMOKE_DIR}/builtin_types.ddsrpc.types.h")
set(DDSRPC_SMOKE_SERVICE_H "${DDSRPC_SMOKE_DIR}/builtin_types.ddsrpc.service.h")
set(DDSRPC_SMOKE_SERVICE_CPP "${DDSRPC_SMOKE_DIR}/builtin_types.ddsrpc.service.cpp")
set(DDSRPC_SMOKE_CLIENT_H "${DDSRPC_SMOKE_DIR}/builtin_types.ddsrpc.client.h")
set(DDSRPC_SMOKE_CLIENT_CPP "${DDSRPC_SMOKE_DIR}/builtin_types.ddsrpc.client.cpp")
add_custom_command(
  OUTPUT
    "${DDSRPC_SMOKE_TYPES}"
    "${DDSRPC_SMOKE_SERVICE_H}"
    "${DDSRPC_SMOKE_SERVICE_CPP}"
    "${DDSRPC_SMOKE_CLIENT_H}"
    "${DDSRPC_SMOKE_CLIENT_CPP}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DDSRPC_SMOKE_DIR}"
  COMMAND $<TARGET_FILE:ddsrpcgen>
          --role all
          --out-dir "${DDSRPC_SMOKE_DIR}"
          --namespace ddsrpc_smoke
          --depfile "${DDSRPC_SMOKE_DEPFILE}"
          "${DDSRPC_SMOKE_IDL}"
  BYPRODUCTS "${DDSRPC_SMOKE_DEPFILE}"
  DEPENDS ddsrpcgen "${DDSRPC_SMOKE_IDL}"
  COMMENT "Generating ddsrpc compile-smoke sources"
  VERBATIM)
add_executable(ddsrpc_generated_compile_check
  generator/generated_header_smoke.cpp
  "${DDSRPC_SMOKE_TYPES}"
  "${DDSRPC_SMOKE_SERVICE_H}"
  "${DDSRPC_SMOKE_SERVICE_CPP}"
  "${DDSRPC_SMOKE_CLIENT_H}"
  "${DDSRPC_SMOKE_CLIENT_CPP}"
)
target_include_directories(ddsrpc_generated_compile_check PRIVATE
  "${DDSRPC_SMOKE_DIR}")
target_link_libraries(ddsrpc_generated_compile_check PRIVATE ddsrpc::runtime)
set_target_properties(ddsrpc_generated_compile_check PROPERTIES AUTOMOC ON)
add_test(NAME ddsrpc_generated_compile_check COMMAND ddsrpc_generated_compile_check)

add_executable(ddsrpc_runtime_tests
  runtime/runtime_test_main.cpp
  runtime/error_wire_test.cpp
  runtime/task_group_test.cpp
  runtime/service_base_test.cpp
  runtime/client_base_test.cpp
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
