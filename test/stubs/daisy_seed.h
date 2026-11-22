/* Native-test stand-in: the two Seed pins rear_midi.cpp names. */
#pragma once
#include <cstdint>
namespace daisy {
using Pin = uint32_t;
namespace seed { static constexpr Pin D13 = 13, D14 = 14; }
}
