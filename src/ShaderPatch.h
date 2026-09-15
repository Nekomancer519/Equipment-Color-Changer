#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ER {
struct PatchedShader {
    std::vector<std::uint8_t> bytes;
    std::uint32_t constantSlot{}, samples{};
    std::string error;
};
// Instruments RGB samples from diffuse texture t0. All other shader instructions,
// resources, alpha, lighting and outputs are preserved. Unsupported code fails closed.
PatchedShader PatchDiffuseShader(std::span<const std::uint8_t> input);
}
