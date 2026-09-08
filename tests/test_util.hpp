#pragma once

#include <iostream>
#include <string_view>

namespace test_util {

inline bool check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "failed: " << description << '\n';
    }
    return condition;
}

}  // namespace test_util
