#include <cstdint>

#include "low_latency_exchange/types.hpp"
#include "test_util.hpp"

using low_latency_exchange::OrderId;
using low_latency_exchange::Price;
using low_latency_exchange::Quantity;
using low_latency_exchange::SequenceNumber;

int main() {
    bool passed = true;

    const auto price = Price::from_ticks(12'345);
    passed &= test_util::check(price.has_value(), "positive tick price is valid");
    passed &= test_util::check(price->ticks() == 12'345, "price retains its integer tick value");
    passed &= test_util::check(!Price::from_ticks(0).has_value(), "zero price is rejected");
    passed &= test_util::check(!Price::from_ticks(-1).has_value(), "negative price is rejected");

    const auto quantity = Quantity::from_units(500);
    passed &= test_util::check(quantity.has_value(), "positive quantity is valid");
    passed &= test_util::check(quantity->units() == 500, "quantity retains its unit count");
    passed &= test_util::check(!Quantity::from_units(0).has_value(), "zero quantity is rejected");

    const auto order_id = OrderId::from_value(42);
    const auto sequence = SequenceNumber::from_value(42);
    passed &= test_util::check(order_id.has_value(), "positive order id is valid");
    passed &= test_util::check(sequence.has_value(), "positive sequence number is valid");
    passed &= test_util::check(!OrderId::from_value(0).has_value(), "zero order id is rejected");
    passed &= test_util::check(!SequenceNumber::from_value(0).has_value(), "zero sequence is rejected");

    return passed ? 0 : 1;
}
