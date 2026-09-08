#include <cstdlib>
#include <string_view>

#include "low_latency_exchange/version.hpp"

int main() {
    return low_latency_exchange::version() == std::string_view{"0.1.0"} ? EXIT_SUCCESS : EXIT_FAILURE;
}
