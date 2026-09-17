#include <filesystem>
#include <iomanip>
#include <iostream>

#include "low_latency_exchange/event_log.hpp"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << (argc > 0 ? argv[0] : "low_latency_exchange_replay") << " <event_log_path>\n";
        return 2;
    }

    const auto result = low_latency_exchange::replay_event_log(std::filesystem::path{argv[1]});
    if (!result.succeeded()) {
        std::cerr << "Replay failed after " << result.replayed_commands << " commands: "
                  << low_latency_exchange::event_log_error_name(*result.error) << '\n';
        return 1;
    }

    std::cout << "Replayed " << result.replayed_commands << " commands; state digest: 0x" << std::hex
              << result.state_digest << '\n';
    return 0;
}
