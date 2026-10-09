#pragma once
#include <cstddef>
#include <string>
#ifdef ENABLE_FURY
#include "gfx_fury_shader.h"
#endif
struct CCFeatures;
namespace Fast {
std::string gfx_direct3d_common_build_shader(size_t& numFloats, const CCFeatures& features, bool rootSignature,
                                             bool threePoint, bool srgb);
// Interpreter shader names are extensionless archive paths, shared with OpenGL.
inline std::string gfx_hlsl_shader_path(const char* shaderName) {
    return shaderName != nullptr ? std::string(shaderName) + ".hlsl" : "shaders/directx/default.shader.hlsl";
}
} // namespace Fast
