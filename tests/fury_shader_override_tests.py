#!/usr/bin/env python3
"""Exercise production Prism generation/Fury annotation without building Shipwright.

Run: python3 libultraship/tests/fury_shader_override_tests.py [build-cmake]
Requires the configured build's Prism headers/static library and a C++20 compiler.
Archive lookup and the Fury compiler bridge are test doubles: this verifies source
selection, processing, layout and compiler-error propagation, not Slang or Vulkan.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

lus = Path(__file__).resolve().parents[1]
root = lus.parent
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build-cmake"
prism = build / "_deps/prism-src/src"
prism_lib = build / "_deps/prism-build/libprism.a"

# Use the portable generator and adapter method verbatim, omitting only their
# heavyweight includes. The doubles implement their archive/runtime boundaries.
generator = (lus / "src/fast/backends/gfx_direct3d11.cpp").read_text()
generator = generator.split("#if defined(ENABLE_DX11) || defined(ENABLE_FURY)\n", 1)[1]
generator = re.sub(r"^#include.*\n", "", generator, flags=re.M).rsplit("#endif", 1)[0]
adapter = (lus / "src/fast/backends/gfx_fury.cpp").read_text()
adapter = adapter.split("ShaderProgram* GfxRenderingAPIFury::CreateAndLoadNewShader", 1)[1]
adapter = "ShaderProgram* GfxRenderingAPIFury::CreateAndLoadNewShader" + adapter.split(
    "void GfxRenderingAPIFury::ShaderGetInfo", 1
)[0]
interpreter = (lus / "include/fast/interpreter.h").read_text()
features = "struct CCFeatures {" + interpreter.split("struct CCFeatures {", 1)[1].split("};", 1)[0] + "};"
items = "enum {\n    SHADER_0," + interpreter.split("enum {\n    SHADER_0,", 1)[1].split("};", 1)[0] + "};"

preamble = r'''
#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include <prism/processor.h>
#include "fast/backends/gfx_hlsl_shader.h"
#include "Bridge.h"
#define SPDLOG_ERROR(...) ((void)0)
#define RESOURCE_FORMAT_BINARY 0
namespace Ship {
enum class ResourceType { Shader };
enum class Endianness { Native };
struct ResourceInitData { uint32_t Type; Endianness ByteOrder; int Format; };
struct Shader { std::string Data; void* GetRawPointer() { return &Data; } };
struct ResourceManager {
    std::map<std::string, std::shared_ptr<Shader>> files;
    std::vector<std::string> loaded;
    auto LoadResource(const std::string& path, bool exact, std::shared_ptr<ResourceInitData>) {
        assert(exact);
        loaded.push_back(path);
        auto found = files.find(path);
        return found == files.end() ? std::shared_ptr<Shader>{} : found->second;
    }
};
struct Context {
    ResourceManager manager;
    static Context* GetRawInstance() { static Context instance; return &instance; }
    ResourceManager* GetResourceManager() { return &manager; }
};
}
static const char* selected = nullptr;
namespace Fast { const char* gfx_get_shader(int16_t) { return selected; } }
'''
bridge = r'''
static CCFeatures current;
void gfx_cc_get_features(uint64_t, uint64_t, CCFeatures* f) { *f = current; }
static std::string compiled;
static bool compilerFailure = false;
uint32_t ship_fury_program(ShipFury*, const char* source, size_t size,
                          const ShipFuryAttribute*, uint32_t, uint32_t) {
    compiled.assign(source, size);
    return compilerFailure ? 0 : 1;
}
const char* ship_fury_error(ShipFury*) { return "test Slang diagnostic"; }
namespace Fast {
struct ShaderProgram {};
struct GfxRenderingAPIFury {
    struct Program { uint32_t id; uint8_t inputs; bool textures[2]; };
    ShipFury* mContext = nullptr;
    int mFilter = 0;
    static constexpr int FILTER_THREE_POINT = 2;
    bool mSrgbMode = false;
    std::map<std::pair<uint64_t, uint64_t>, Program> mPrograms;
    void LoadShader(ShaderProgram*) {}
    ShaderProgram* CreateAndLoadNewShader(uint64_t, uint64_t);
};
'''
# Shared by the real GPU harness so both paths reject identical member ABI changes.
cbuffer_tests = r'''
static std::vector<std::string> invalidConstantBuffers(const std::string& source) {
    std::vector<std::string> result;
    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
        {"uint noise_frame;", "float4 extra; uint noise_frame;"},
        {"uint noise_frame;", "uint noise_frame : packoffset(c1);"},
        {"uint noise_frame;", "float noise_scale; uint noise_frame;"},
        {"float noise_scale;", ""},
        {"uint noise_frame;", "float noise_frame;"},
        {"uint noise_frame;", "FrameAlias noise_frame;"},
        {"float noise_scale;", "float noise_scale[1];"},
        {"uint width;", "uint height; uint width;"},
        {"uint height;", ""},
        {"bool linear_filtering;", "uint linear_filtering;"},
        {"bool linear_filtering;", "bool linear_filtering; float extra;"},
        {"textures[2]", "textures[3]"},
        {"textures[2]", "textures"},
        {"struct {", "struct TextureInfo {"},
        {"float prim_depth;", "float prim_depth : packoffset(c1);"},
        {"float prim_depth;", "float3 _pad; float prim_depth;"},
        {"float3 _pad;", ""},
        {"float3 _pad;", "float4 _pad;"},
        {"float3 _pad;", "float3 _pad; float extra;"},
    }) {
        auto changed = source;
        auto pos = changed.find(from);
        if (pos == std::string::npos) throw std::logic_error("constant-buffer test declaration changed: " + from);
        changed.replace(pos, from.size(), to);
        result.push_back(changed);
    }
    return result;
}
static std::vector<std::pair<std::string, std::string>> invalidPreprocessing(const std::string& source) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [prefix, message] : std::vector<std::pair<std::string, std::string>>{
        {"#define noise_frame extra; uint noise_frame\n", "macro redefines canonical ABI identifier"},
        {"#define noise_\\\nframe extra; uint noise_frame\n", "macro redefines canonical ABI identifier"},
        {"#define uint float\n", "macro redefines canonical ABI identifier"},
        {"#define width extra; uint width\n", "macro redefines canonical ABI identifier"},
        {"#define float3 float4\n", "macro redefines canonical ABI identifier"},
        {"#define g_texture0 g_texture1\n", "macro redefines canonical ABI identifier"},
        {"#define position transformed\n", "macro redefines canonical ABI identifier"},
        {"#include \"mods/unvalidated.hlsli\"\n", "HLSL #include is unsupported"},
        {"/* comment *\\\n/\n#define noise_frame extra; uint noise_frame\n/* end */\n", "macro redefines canonical ABI identifier"},
        {"/* comment *\\\r\n/\n#define noise_frame extra; uint noise_frame\n/* end */\n", "macro redefines canonical ABI identifier"},
        {"/* comment *\\\n/\n#include \"mods/unvalidated.hlsli\"\n/* end */\n", "HLSL #include is unsupported"},
    }) {
        auto changed = source;
        changed.insert(changed.find('\n') + 1, prefix);
        result.emplace_back(changed, message);
    }
    return result;
}
static std::string splicedAbi(std::string source) {
    for (const auto& token : {std::string("cbuffer"), std::string("noise_frame"), std::string("VSMain"), std::string("Texture2D")}) {
        auto pos = source.find(token);
        if (pos == std::string::npos) throw std::logic_error("missing splice-test token: " + token);
        source.insert(pos + 3, "\\\n");
    }
    return source;
}
static std::string formattedConstantBuffers(std::string source) {
    // Apply formatting to all three buffers, including the nested anonymous struct.
    for (const auto& name : {"PerFrameCB", "PerDrawCB", "PerPrimDepthCB"}) {
        auto begin = source.find(std::string("cbuffer ") + name);
        if (begin == std::string::npos) throw std::logic_error("missing test cbuffer");
        auto end = source.find("\n}", begin);
        if (end == std::string::npos) throw std::logic_error("missing test cbuffer close");
        auto body = source.substr(begin, end + 2 - begin);
        body = std::regex_replace(body, std::regex(R"(/\*[\s\S]*?\*/|//[^\n]*)"), " ");
        body = std::regex_replace(body, std::regex(R"(\s+)"), "\n\t");
        body = std::regex_replace(body, std::regex(R"([;{}\[\]])"), " /* ABI formatting */ $& \n");
        source.replace(begin, end + 2 - begin, body);
    }
    return source;
}
'''
tests = r'''
} // namespace Fast
static void expectError(const std::string& origin, const std::string& detail) {
    Fast::GfxRenderingAPIFury renderer;
    try {
        renderer.CreateAndLoadNewShader(0, 0);
        std::cerr << "Expected failure: " << detail << '\n';
        throw std::logic_error("Expected failure");
    } catch (const std::runtime_error& error) {
        std::string message = error.what();
        assert(message.find(origin) != std::string::npos);
        if (message.find(detail) == std::string::npos)
            std::cerr << "Expected diagnostic '" << detail << "', got: " << message << '\n';
        assert(message.find(detail) != std::string::npos);
        assert(renderer.mPrograms.empty());
    }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream file(argv[1]);
    std::string stock((std::istreambuf_iterator<char>(file)), {});
    assert(!stock.empty());
    auto& manager = *Ship::Context::GetRawInstance()->GetResourceManager();
    auto put = [&](const std::string& path, const std::string& data) {
        manager.files[path] = std::make_shared<Ship::Shader>(Ship::Shader{data});
    };
    const std::string defaultPath = "shaders/directx/default.shader.hlsl";
    put(defaultPath, stock);
    put("mods/custom.hlsl", stock + "\n// CUSTOM_OVERRIDE_MARKER\n");
    for (int variant = 0; variant < 8; ++variant) {
        current = {};
        current.shader_id = -1;
        current.numInputs = 1;
        current.opt_alpha = variant & 1;
        current.usedTextures[0] = variant & 2;
        current.usedTextures[1] = variant & 2;
        current.opt_fog = variant & 4;
        current.opt_grayscale = variant & 4;
        current.opt_prim_depth = variant & 4;
        current.opt_2cyc = variant & 4;
        current.opt_noise = current.opt_alpha;
        current.opt_texture_edge = current.opt_alpha;
        current.opt_alpha_threshold = current.opt_alpha;
        current.clamp[0][0] = current.usedTextures[0];
        current.clamp[1][1] = current.usedTextures[1];
        current.used_masks[0] = current.usedTextures[0];
        current.used_blend[1] = current.usedTextures[1];
        for (int cycle = 0; cycle < 2; ++cycle)
            for (int channel = 0; channel < 2; ++channel) {
                current.do_single[cycle][channel] = true;
                current.c[cycle][channel][3] = SHADER_INPUT_1;
            }
        for (bool custom : { false, true }) {
            selected = custom ? "mods/custom" : nullptr;
            current.shader_id = custom ? 0 : -1;
            Fast::GfxRenderingAPIFury renderer;
            renderer.mFilter = variant & 2 ? 2 : 0;
            renderer.mSrgbMode = variant & 4;
            assert(renderer.CreateAndLoadNewShader(0, 0));
            assert(manager.loaded.back() == (custom ? "mods/custom.hlsl" : defaultPath));
            assert((compiled.find("CUSTOM_OVERRIDE_MARKER") != std::string::npos) == custom);
            assert(compiled.find("[[vk::location(0)]]") != std::string::npos);
            if (current.usedTextures[0])
                assert(compiled.find("[[vk::binding(0,0)]] Texture2D g_texture0") != std::string::npos);
        }
    }
    selected = "mods/custom";
    put("mods/custom.hlsl", stock + "\n@include(\"mods/shared.hlsl\")\n");
    put("mods/shared.hlsl", "@prism(type='hlsl')\n// ARCHIVE_INCLUDE_MARKER\n");
    Fast::GfxRenderingAPIFury renderer;
    assert(renderer.CreateAndLoadNewShader(0, 0));
    assert(compiled.find("ARCHIVE_INCLUDE_MARKER") != std::string::npos);
    manager.files.erase("mods/shared.hlsl");
    expectError("mods/custom.hlsl", "mods/shared.hlsl");
    put("mods/custom.hlsl", "@prism(type='hlsl')\n@include(\"mods/custom.hlsl\")\n");
    expectError("mods/custom.hlsl", "include expansion limit exceeded");
    put("mods/custom.hlsl", "@prism(type='hlsl')\n@include(\"mods/shared.hlsl\")\n");
    put("mods/shared.hlsl", "@prism(type='hlsl')\n@include(\"mods/custom.hlsl\")\n");
    expectError("mods/custom.hlsl", "check for include cycles");
    put("mods/shared.hlsl", "@prism(type='hlsl')\n" + std::string(1024 * 1024, ' '));
    expectError("mods/custom.hlsl", "source expansion exceeds 1 MiB");
    // Cumulative archive input is bounded even when every individual file fits.
    // This does not bound Prism-generated output or expression/loop evaluation.
    put("mods/custom.hlsl", "@prism(type='hlsl')\n@include(\"mods/shared.hlsl\")\n@include(\"mods/shared.hlsl\")\n");
    put("mods/shared.hlsl", "@prism(type='hlsl')\n" + std::string(512 * 1024, ' '));
    expectError("mods/custom.hlsl", "source expansion exceeds 1 MiB");
    // An acyclic chain cannot bypass the include-count budget either.
    for (int i = 0; i < 129; ++i)
        put("mods/chain" + std::to_string(i) + ".hlsl", "@prism(type='hlsl')\n@include(\"mods/chain" +
            std::to_string(i + 1) + ".hlsl\")\n");
    put("mods/custom.hlsl", "@prism(type='hlsl')\n@include(\"mods/chain0.hlsl\")\n");
    expectError("mods/custom.hlsl", "include expansion limit exceeded");
    put("mods/custom.hlsl", std::string(1024 * 1024 + 1, ' '));
    expectError("mods/custom.hlsl", "source exceeds 1 MiB");
    // Repeated includes are legal; the bounded loader is reset after failures.
    put("mods/shared.hlsl", "@prism(type='hlsl')\n// REPEATED_INCLUDE\n");
    put("mods/custom.hlsl", stock + "\n@include(\"mods/shared.hlsl\")\n@include(\"mods/shared.hlsl\")\n");
    assert(renderer.CreateAndLoadNewShader(0, 0));
    auto repeated = compiled.find("REPEATED_INCLUDE");
    assert(repeated != std::string::npos);
    assert(compiled.find("REPEATED_INCLUDE", repeated + 1) != std::string::npos);
    put("mods/custom.hlsl", "not a Prism shader");
    expectError("mods/custom.hlsl", "Failed to process");
    put("mods/custom.hlsl", "@prism(type='hlsl')\nfloat4 PSMain() { return 1; }\n");
    expectError("mods/custom.hlsl", "vertex entry point");
    std::string wrongLayout = stock;
    auto count = wrongLayout.find("update_floats(2)");
    assert(count != std::string::npos);
    wrongLayout.replace(count, 16, "update_floats(3)");
    put("mods/custom.hlsl", wrongLayout);
    expectError("mods/custom.hlsl", "vertex layout");
    put("mods/custom.hlsl", stock);
    compilerFailure = true;
    expectError("mods/custom.hlsl", "test Slang diagnostic");
    compilerFailure = false;
    selected = nullptr;
    size_t floats = 0;
    std::string generated = Fast::gfx_direct3d_common_build_shader(floats, current, false, true, true);
    selected = "mods/custom";
    auto replace = [](std::string& text, const std::string& from, const std::string& to) {
        auto pos = text.find(from);
        assert(pos != std::string::npos);
        text.replace(pos, from.size(), to);
    };
    auto replaceParameter = [](std::string& text, const std::string& from, const std::string& to) {
        auto pos = text.find(from, text.find("VSMain"));
        assert(pos != std::string::npos);
        text.replace(pos, from.size(), to);
    };
    auto plain = [&](const std::string& text) {
        put("mods/custom.hlsl", "@prism(type='hlsl')\n" + text +
            "\n@{update_floats(" + std::to_string(floats - 4) + ")}\n");
    };
    for (const auto& invalidBuffer : invalidConstantBuffers(generated)) {
        plain(invalidBuffer);
        expectError("mods/custom.hlsl", "constant-buffer member layout");
    }
    for (const auto& [invalidSource, message] : invalidPreprocessing(generated)) {
        plain(invalidSource);
        expectError("mods/custom.hlsl", message);
    }
    // Ordinary pixel-expression macros and physically split ABI tokens remain legal.
    plain("#define CUSTOM_EXPRESSION float4(0,1,0,1)\n" + generated);
    assert(renderer.CreateAndLoadNewShader(0, 0));
    plain(splicedAbi(generated));
    assert(renderer.CreateAndLoadNewShader(0, 0));
    // Check arbitrary buffer whitespace/comments separately from descriptor formatting.
    plain(formattedConstantBuffers(generated));
    assert(renderer.CreateAndLoadNewShader(0, 0));
    std::string formatted = generated;
    replace(formatted, "PSInput VSMain(", "PSInput\n VSMain \n (\n");
    auto close = formatted.find(") {", formatted.find("VSMain"));
    assert(close != std::string::npos);
    formatted.replace(close, 3, ")\n\t{");
    replace(formatted, "Texture2D g_texture0", "[[vk::binding(0, 0)]] Texture2D < float4 >\n g_texture0");
    replace(formatted, "SamplerState g_sampler0", "[[vk::binding(6,0)]]\nSamplerState\t g_sampler0");
    replace(formatted, "cbuffer PerFrameCB", "[[vk::binding(8,0)]]\ncbuffer\t PerFrameCB");
    replace(formatted, "float4 position : POSITION", "[[vk::location(0)]] float4 position : POSITION");
    formatted += "\n// Texture2D ignored : register(t22);\n/* [[vk::binding(55,0)]] Texture2D ignored2; */\n";
    plain(formatted);
    assert(renderer.CreateAndLoadNewShader(0, 0));
    assert(compiled.find("[[vk::binding(0,0)]] [[vk::binding") == std::string::npos);
    assert(compiled.find("[[vk::location(0)]] [[vk::location") == std::string::npos);
    // Preparation is idempotent for already annotated resources/parameters.
    plain(compiled);
    assert(renderer.CreateAndLoadNewShader(0, 0));
    std::string invalid = generated;
    replace(invalid, "float4 position : POSITION", "float3 position : POSITION");
    replaceParameter(invalid, "float2 uv0 : TEXCOORD0", "float3 uv0 : TEXCOORD0");
    plain(invalid); // Same total float count and parameter count, wrong widths.
    expectError("mods/custom.hlsl", "location 0 must be float4 position");
    invalid = generated;
    replaceParameter(invalid, "float4 fog : FOG", "float4 grayscale : GRAYSCALE");
    auto grayscale = invalid.find("float4 grayscale : GRAYSCALE", invalid.find("float4 grayscale : GRAYSCALE", invalid.find("VSMain")) + 1);
    assert(grayscale != std::string::npos);
    invalid.replace(grayscale, std::string("float4 grayscale : GRAYSCALE").size(), "float4 fog : FOG");
    // This has the same widths/count but swaps semantic/name order.
    plain(invalid);
    expectError("mods/custom.hlsl", "must be float4 fog : FOG");
    invalid = generated;
    replaceParameter(invalid, "float2 uv0 : TEXCOORD0", "float2 uv0 : TEXCOORD1");
    plain(invalid);
    expectError("mods/custom.hlsl", "must be float2 uv0 : TEXCOORD0");
    invalid = formatted;
    replace(invalid, "vk::binding(0, 0)", "vk::binding(1, 0)");
    plain(invalid);
    expectError("mods/custom.hlsl", "conflicting or unsupported Vulkan binding");
    invalid = formatted;
    replace(invalid, "vk::binding(0, 0)", "vk::binding(0, 1)");
    plain(invalid);
    expectError("mods/custom.hlsl", "expected binding(0,0)");
    invalid = formatted;
    replace(invalid, "vk::location(0)", "vk::location(1)");
    plain(invalid);
    expectError("mods/custom.hlsl", "conflicting or unsupported Vulkan location");
    invalid = generated;
    replace(invalid, "register(t0)", "register(t1)");
    plain(invalid);
    expectError("mods/custom.hlsl", "conflicting register");
    for (const auto& resource : { "Texture2D<float4> extra;", "RWStructuredBuffer<float4> extra;",
                                 "SamplerComparisonState extra;", "cbuffer ExtraCB { float4 value; };",
                                 "RaytracingAccelerationStructure extra;", "RasterizerOrderedTexture2D<float4> extra;" }) {
        plain(generated + "\n" + resource);
        expectError("mods/custom.hlsl", "unsupported descriptor resource");
    }
    for (const auto& element : { "uint4", "int4", "uint", "double4", "2", "TexelAlias" }) {
        invalid = generated;
        replace(invalid, "Texture2D g_texture0", std::string("Texture2D<") + element + "> g_texture0");
        plain(invalid);
        expectError("mods/custom.hlsl", "RGBA8_UNORM textures require float scalar/vector elements");
    }
    plain(generated + "\nusing AliasTex = Texture2D<float4>; AliasTex extra;\n");
    expectError("mods/custom.hlsl", "unsupported descriptor declaration or resource alias");
    plain(generated + "\nSomeAlias extra : register(t15);\n");
    expectError("mods/custom.hlsl", "unsupported descriptor declaration or resource alias");
    invalid = generated;
    replace(invalid, "g_texture0 : register(t0)", "g_texture0[2] : register(t0)");
    plain(invalid);
    expectError("mods/custom.hlsl", "unsupported array");
    manager.files.erase("mods/custom.hlsl");
    put("mods/custom.glsl", "GLSL-only mod");
    expectError("mods/custom.hlsl", "select OpenGL");
    assert(manager.loaded.back() == "mods/custom.hlsl");
    selected = nullptr;
    manager.files.erase(defaultPath);
    expectError(defaultPath, "check f3d.o2r");
    std::cout << "16 default/custom variants and ABI/binding/include/error tests passed\n";
}
'''
def main():
    with tempfile.TemporaryDirectory(prefix="fury-shader-tests-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(preamble + items + features + generator + bridge + adapter + "} // namespace Fast\n" + cbuffer_tests + "namespace Fast {\n" + tests)
        subprocess.run([
            os.environ.get("CXX", "c++"), "-std=c++20", "-DENABLE_FURY", "-O0", "-g",
            "-I" + str(prism), "-I" + str(lus / "include"),
            "-I" + str(root / "renderer/fury"), str(source), str(prism_lib),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary), str(lus / "src/fast/shaders/directx/default.shader.hlsl")], check=True, timeout=120)


if __name__ == "__main__":
    main()
