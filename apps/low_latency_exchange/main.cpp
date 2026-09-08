#include <iostream>

#include "low_latency_exchange/version.hpp"

int main() {
    std::cout << "Low-Latency Exchange " << low_latency_exchange::version() << '\n';
    return 0;
}
