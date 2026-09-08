#include <cstdlib>
#include <string_view>

#include "pulsebook/version.hpp"

int main() {
    return pulsebook::version() == std::string_view{"0.1.0"} ? EXIT_SUCCESS : EXIT_FAILURE;
}
