find_package(BISON)
find_package(FLEX)

if(BISON_FOUND AND FLEX_FOUND)
    bison_target(yacc yacc.y ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.cpp
            DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.h)
    flex_target(lex lex.l ${CMAKE_CURRENT_SOURCE_DIR}/lex.yy.cpp)
    add_flex_bison_dependency(lex yacc)
    set(PARSER_SOURCES ${BISON_yacc_OUTPUT_SOURCE} ${FLEX_lex_OUTPUTS})
else()
    message(WARNING "BISON/FLEX not found, using checked-in generated parser sources.")
    set(PARSER_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/lex.yy.cpp)
endif()

set(SOURCES ${PARSER_SOURCES} ast.cpp)
add_library(parser STATIC ${SOURCES})

add_executable(test_parser test_parser.cpp)
target_link_libraries(test_parser parser)
add_test(NAME test_parser COMMAND test_parser
        WORKING_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
