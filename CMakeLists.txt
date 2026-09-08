cmake_minimum_required(VERSION 3.25)

project(LowLatencyExchange VERSION 0.1.0 LANGUAGES CXX)

option(LOW_LATENCY_EXCHANGE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

function(low_latency_exchange_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
        if(LOW_LATENCY_EXCHANGE_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        if(LOW_LATENCY_EXCHANGE_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(low_latency_exchange_core STATIC
    src/version.cpp
    src/order_book.cpp
)

target_compile_features(low_latency_exchange_core PUBLIC cxx_std_20)
target_include_directories(low_latency_exchange_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
low_latency_exchange_enable_warnings(low_latency_exchange_core)

add_executable(low_latency_exchange
    apps/low_latency_exchange/main.cpp
)
target_link_libraries(low_latency_exchange PRIVATE low_latency_exchange_core)
low_latency_exchange_enable_warnings(low_latency_exchange)

include(CTest)
if(BUILD_TESTING)
    add_executable(low_latency_exchange_smoke_test
        tests/smoke_test.cpp
    )
    target_link_libraries(low_latency_exchange_smoke_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_smoke_test)

    add_test(NAME low_latency_exchange.smoke COMMAND low_latency_exchange_smoke_test)

    add_executable(low_latency_exchange_types_test
        tests/types_test.cpp
    )
    target_link_libraries(low_latency_exchange_types_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_types_test)

    add_test(NAME low_latency_exchange.types COMMAND low_latency_exchange_types_test)

    add_executable(low_latency_exchange_order_command_test
        tests/order_command_test.cpp
    )
    target_link_libraries(low_latency_exchange_order_command_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_order_command_test)

    add_test(NAME low_latency_exchange.order_command COMMAND low_latency_exchange_order_command_test)

    add_executable(low_latency_exchange_order_book_test
        tests/order_book_test.cpp
    )
    target_link_libraries(low_latency_exchange_order_book_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_order_book_test)

    add_test(NAME low_latency_exchange.order_book COMMAND low_latency_exchange_order_book_test)
endif()
