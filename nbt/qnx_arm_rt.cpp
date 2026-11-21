#include <stdint.h>

// The target-only QNX sysroot omits libgcc. Clang uses this ARM EABI helper
// when the reused PCM timing code converts an unsigned 64-bit count.
extern "C" double __aeabi_ul2d(uint64_t value) {
    const uint32_t high = static_cast<uint32_t>(value >> 32);
    const uint32_t low = static_cast<uint32_t>(value);
    return static_cast<double>(high) * 4294967296.0 + static_cast<double>(low);
}
