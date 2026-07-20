add_executable(cad-parser-tests parser_tests.cpp)
target_link_libraries(cad-parser-tests PRIVATE cad_parser)
target_include_directories(cad-parser-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(cad-parser-tests PRIVATE -Wall -Wextra -pedantic)

add_test(NAME cad-parser-regression COMMAND cad-parser-tests)
