cmake_minimum_required(VERSION 3.25)

project(PulseBook VERSION 0.1.0 LANGUAGES CXX)

option(PULSEBOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

function(pulsebook_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
        if(PULSEBOOK_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        if(PULSEBOOK_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(pulsebook_core STATIC
    src/version.cpp
)

target_compile_features(pulsebook_core PUBLIC cxx_std_20)
target_include_directories(pulsebook_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
pulsebook_enable_warnings(pulsebook_core)

add_executable(pulsebook
    apps/pulsebook/main.cpp
)
target_link_libraries(pulsebook PRIVATE pulsebook_core)
pulsebook_enable_warnings(pulsebook)

include(CTest)
if(BUILD_TESTING)
    add_executable(pulsebook_smoke_test
        tests/smoke_test.cpp
    )
    target_link_libraries(pulsebook_smoke_test PRIVATE pulsebook_core)
    pulsebook_enable_warnings(pulsebook_smoke_test)

    add_test(NAME pulsebook.smoke COMMAND pulsebook_smoke_test)
endif()
