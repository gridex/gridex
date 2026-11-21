add_executable(sqlite-smoketest sqlite-smoketest.cpp)
target_link_libraries(sqlite-smoketest PRIVATE gridex-core gridex-data)

add_executable(phase2c-smoketest phase2c-smoketest.cpp)
target_link_libraries(phase2c-smoketest PRIVATE gridex-core gridex-data gridex-domain)

add_executable(phase2d-smoketest phase2d-smoketest.cpp)
target_link_libraries(phase2d-smoketest PRIVATE gridex-core gridex-data gridex-domain)

add_executable(ai-retry-smoketest ai-retry-smoketest.cpp)
target_link_libraries(ai-retry-smoketest PRIVATE gridex-core gridex-services)

# Put test binaries under linux/bin-tests so they can be run without
# referencing the `build/` path segment. -UNDEBUG keeps assert() active even
# in Release builds since smoke tests rely on assertions for correctness checks.
set_target_properties(sqlite-smoketest phase2c-smoketest phase2d-smoketest ai-retry-smoketest PROPERTIES
    AUTOMOC OFF
    AUTORCC OFF
    AUTOUIC OFF
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin-tests
)
target_compile_options(sqlite-smoketest  PRIVATE -UNDEBUG)
target_compile_options(phase2c-smoketest PRIVATE -UNDEBUG)
target_compile_options(phase2d-smoketest PRIVATE -UNDEBUG)
target_compile_options(ai-retry-smoketest PRIVATE -UNDEBUG)
