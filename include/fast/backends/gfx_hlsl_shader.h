#pragma once
#include <cstddef>
#include <string>
struct CCFeatures;
namespace Fast {
std::string gfx_direct3d_common_build_shader(size_t& numFloats, const CCFeatures& features, bool rootSignature,
                                             bool threePoint, bool srgb);
}
