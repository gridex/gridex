cmake_minimum_required(VERSION 3.24)

project(gridex
    VERSION 0.1.0
    DESCRIPTION "Gridex — AI-Native Database IDE (Linux)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network Concurrent Svg)

qt_standard_project_setup()

option(GRIDEX_BUILD_TESTS "Build smoke tests" ON)

add_subdirectory(src/Core)
add_subdirectory(src/Data)
add_subdirectory(src/Services)
add_subdirectory(src/Domain)
add_subdirectory(src/App)
add_subdirectory(src/Presentation)

if(GRIDEX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

qt_add_executable(gridex src/main.cpp resources/gridex.qrc)

target_link_libraries(gridex PRIVATE
    gridex-core
    gridex-data
    gridex-services
    gridex-domain
    gridex-app
    gridex-presentation
    Qt6::Widgets
)

target_compile_definitions(gridex PRIVATE
    GRIDEX_VERSION="${PROJECT_VERSION}"
)

set_target_properties(gridex PROPERTIES
    WIN32_EXECUTABLE OFF
    MACOSX_BUNDLE OFF
)

install(TARGETS gridex RUNTIME DESTINATION bin)
