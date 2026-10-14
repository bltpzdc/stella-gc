foreach(t IN ITEMS root context heap config)
    add_executable(${t}.test ${t}.cpp)
    target_include_directories(${t}.test PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/3dparty)
    target_compile_definitions(${t}.test PRIVATE FMT_HEADER_ONLY=1)
    target_link_libraries(${t}.test PRIVATE gc-fmt)
    add_test(NAME ${t}.test COMMAND ${t}.test)
endforeach()