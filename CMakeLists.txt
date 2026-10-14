cmake_minimum_required(VERSION 3.20)

project(stella-gc VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(gc-fmt INTERFACE)
target_include_directories(gc-fmt INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/3dparty)
target_compile_definitions(gc-fmt INTERFACE FMT_HEADER_ONLY=1)

add_library(gc-core STATIC gc.cpp)
target_include_directories(gc-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(gc-core PUBLIC gc-fmt)

set(STELLA_RUNTIME_SOURCES runtime/main.c runtime/runtime.c runtime/scalar.c)

foreach(example IN ITEMS chat double history playlist)
    add_executable(${example} examples/${example}.c ${STELLA_RUNTIME_SOURCES})
    target_link_libraries(${example} PRIVATE gc-core)
    if(UNIX)
        target_link_libraries(${example} PRIVATE m)
    endif()
endforeach()

enable_testing()
add_subdirectory(test/ut)