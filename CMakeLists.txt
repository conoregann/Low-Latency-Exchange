cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

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

if(APPLE AND EXISTS "/opt/homebrew")
    list(PREPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

find_package(Boost REQUIRED)

add_library(low_latency_exchange_core STATIC
    src/version.cpp
    src/event_log.cpp
    src/order_book.cpp
    src/market_data.cpp
    src/protocol.cpp
    src/tcp_gateway.cpp
)

target_compile_features(low_latency_exchange_core PUBLIC cxx_std_20)
target_include_directories(low_latency_exchange_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(low_latency_exchange_core PUBLIC Boost::boost)
low_latency_exchange_enable_warnings(low_latency_exchange_core)

add_executable(low_latency_exchange
    apps/low_latency_exchange/main.cpp
)
target_link_libraries(low_latency_exchange PRIVATE low_latency_exchange_core)
low_latency_exchange_enable_warnings(low_latency_exchange)

add_executable(low_latency_exchange_replay
    apps/low_latency_exchange/replay_main.cpp
)
target_link_libraries(low_latency_exchange_replay PRIVATE low_latency_exchange_core)
low_latency_exchange_enable_warnings(low_latency_exchange_replay)

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

    add_executable(low_latency_exchange_property_test
        tests/property_test.cpp
    )
    target_link_libraries(low_latency_exchange_property_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_property_test)

    add_test(NAME low_latency_exchange.property COMMAND low_latency_exchange_property_test)

    add_executable(low_latency_exchange_differential_test
        tests/differential_test.cpp
    )
    target_link_libraries(low_latency_exchange_differential_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_differential_test)

    add_test(NAME low_latency_exchange.differential COMMAND low_latency_exchange_differential_test)

    add_executable(low_latency_exchange_fuzz_test
        tests/fuzz_test.cpp
    )
    target_link_libraries(low_latency_exchange_fuzz_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_fuzz_test)

    add_test(NAME low_latency_exchange.fuzz COMMAND low_latency_exchange_fuzz_test)

    add_executable(low_latency_exchange_protocol_test
        tests/protocol_test.cpp
    )
    target_link_libraries(low_latency_exchange_protocol_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_protocol_test)

    add_test(NAME low_latency_exchange.protocol COMMAND low_latency_exchange_protocol_test)

    add_executable(low_latency_exchange_spsc_queue_test
        tests/spsc_queue_test.cpp
    )
    target_link_libraries(low_latency_exchange_spsc_queue_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_spsc_queue_test)

    add_test(NAME low_latency_exchange.spsc_queue COMMAND low_latency_exchange_spsc_queue_test)

    add_executable(low_latency_exchange_market_data_test
        tests/market_data_test.cpp
    )
    target_link_libraries(low_latency_exchange_market_data_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_market_data_test)

    add_test(NAME low_latency_exchange.market_data COMMAND low_latency_exchange_market_data_test)

    add_executable(low_latency_exchange_event_log_test
        tests/event_log_test.cpp
    )
    target_link_libraries(low_latency_exchange_event_log_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_event_log_test)

    add_test(NAME low_latency_exchange.event_log COMMAND low_latency_exchange_event_log_test)

    add_executable(low_latency_exchange_gateway_test
        tests/gateway_test.cpp
    )
    target_link_libraries(low_latency_exchange_gateway_test PRIVATE low_latency_exchange_core)
    low_latency_exchange_enable_warnings(low_latency_exchange_gateway_test)

    add_test(NAME low_latency_exchange.gateway COMMAND low_latency_exchange_gateway_test)
endif()
