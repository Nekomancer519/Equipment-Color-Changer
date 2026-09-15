#pragma once
#include <array>
#include <cstddef>
namespace ER {
struct D3DSlots { std::size_t createPixelShader; std::array<std::size_t,7> draws; };
const D3DSlots& SdkD3DSlots();
}
