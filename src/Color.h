#pragma once
#include <array>
#include <cmath>
namespace ER {
struct Color {
    std::array<float,3> rgb{0.15f,0.4f,0.85f};
    float strength{0.8f}, brightness{1.0f};
    bool operator==(const Color&) const = default;
};
inline bool ValidColor(const Color& c) {
    for(float v:c.rgb) if(!std::isfinite(v) || v<0 || v>1) return false;
    return std::isfinite(c.strength) && c.strength>=0 && c.strength<=1 &&
        std::isfinite(c.brightness) && c.brightness>=.25f && c.brightness<=2;
}
inline std::array<float,4> TintParameters(const Color& c) {
    return {c.rgb[0]*c.brightness*c.strength,c.rgb[1]*c.brightness*c.strength,
        c.rgb[2]*c.brightness*c.strength,1-c.strength};
}
}
