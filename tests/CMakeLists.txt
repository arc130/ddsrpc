add_executable(ddsrpcgen_tests
  generator/import_resolver_test.cpp
  generator/lexer_test.cpp
  generator/parser_test.cpp
  generator/semantic_test.cpp
)
target_link_libraries(ddsrpcgen_tests PRIVATE ddsrpcgen_lib GTest::gtest_main Qt5::Core)
add_test(NAME ddsrpcgen_tests COMMAND ddsrpcgen_tests)
