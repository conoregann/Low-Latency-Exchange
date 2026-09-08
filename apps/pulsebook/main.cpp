#include <iostream>

#include "pulsebook/version.hpp"

int main() {
    std::cout << "PulseBook " << pulsebook::version() << '\n';
    return 0;
}
