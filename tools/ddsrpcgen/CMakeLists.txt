add_library(ddsrpcgen_lib STATIC
  diagnostic.cpp
  import_resolver.cpp
  lexer.cpp
  parser.cpp
  schema.cpp
  semantic.cpp
)
target_include_directories(ddsrpcgen_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ddsrpcgen_lib PUBLIC Qt5::Core)
target_compile_features(ddsrpcgen_lib PUBLIC cxx_std_17)
# ast.h uses `signals` and `slots` as member names, which Qt defines as macros
# in its QObject headers unless QT_NO_KEYWORDS is set. The generator and its
# consumers are Qt Core-only and do not use the keyword macros.
target_compile_definitions(ddsrpcgen_lib PUBLIC QT_NO_KEYWORDS)
