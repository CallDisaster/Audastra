#pragma once

#include <array>
#include <cmath>
#include <optional>

namespace War3Map::MapData::ShadowNative {

using Vec3 = std::array<float, 3>;

// 695C20的F32栈存储边界；其余乘加使用double。显式double操作避免
// C++先以float相减/相乘，以及任意front-face或t>=0裁剪改写原规则。
// caster与地形receiver共用同一数学；这不是任意x87环境的逐位模拟器。
inline std::optional<float> IntersectSigned(const Vec3& origin, const Vec3& direction,
    const std::array<Vec3, 3>& vertices) {
    const auto& a = vertices[0];
    const auto& b = vertices[1];
    const auto& c = vertices[2];
    const double e1x = static_cast<double>(b[0]) - a[0];
    const float e1y = static_cast<float>(static_cast<double>(b[1]) - a[1]);
    const float e1z = static_cast<float>(static_cast<double>(b[2]) - a[2]);
    const float e2x = static_cast<float>(static_cast<double>(c[0]) - a[0]);
    const float e2y = static_cast<float>(static_cast<double>(c[1]) - a[1]);
    const float e2z = static_cast<float>(static_cast<double>(c[2]) - a[2]);
    const double px = static_cast<double>(e2z) * direction[1] - static_cast<double>(e2y) * direction[2];
    const double py = static_cast<double>(e2x) * direction[2] - static_cast<double>(e2z) * direction[0];
    const double pz = static_cast<double>(e2y) * direction[0] - static_cast<double>(e2x) * direction[1];
    const float pzStored = static_cast<float>(pz);
    const double determinant = pz * e1z + py * e1y + px * e1x;
    if (std::abs(determinant) < 0x1p-22) return std::nullopt;
    const float inverse = static_cast<float>(1.0 / determinant);
    const double tx = static_cast<double>(origin[0]) - a[0];
    const double ty = static_cast<double>(origin[1]) - a[1];
    const double tz = static_cast<double>(origin[2]) - a[2];
    const float qx = static_cast<float>(ty * e1z - tz * e1y);
    const float qy = static_cast<float>(tz * e1x - tx * e1z);
    const float qz = static_cast<float>(tx * e1y - ty * e1x);
    const float u = static_cast<float>((tz * pzStored + ty * py + tx * px) * inverse);
    const float v = static_cast<float>((static_cast<double>(qx) * direction[0]
        + static_cast<double>(qz) * direction[2] + static_cast<double>(qy) * direction[1]) * inverse);
    if (u < 0 || u > 1 || v < 0 || v > 1 || static_cast<double>(u) + v > 1) return std::nullopt;
    return static_cast<float>((static_cast<double>(qz) * e2z + static_cast<double>(qy) * e2y
        + static_cast<double>(qx) * e2x) * inverse);
}

} // namespace War3Map::MapData::ShadowNative
