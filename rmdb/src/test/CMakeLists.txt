find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
    NAME stage9_11_integration
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/stage9_11_integration.py $<TARGET_FILE:rmdb>
)
set_tests_properties(stage9_11_integration PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
