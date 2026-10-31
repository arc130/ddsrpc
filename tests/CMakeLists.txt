add_executable(ddsrpcgen_tests
  generator/lexer_test.cpp
)
target_link_libraries(ddsrpcgen_tests PRIVATE ddsrpcgen_lib GTest::gtest_main Qt5::Core)
add_test(NAME ddsrpcgen_tests COMMAND ddsrpcgen_tests)
