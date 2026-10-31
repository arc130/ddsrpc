add_library(ddsrpcgen_lib STATIC
  diagnostic.cpp
  lexer.cpp
)
target_include_directories(ddsrpcgen_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ddsrpcgen_lib PUBLIC Qt5::Core)
target_compile_features(ddsrpcgen_lib PUBLIC cxx_std_17)
