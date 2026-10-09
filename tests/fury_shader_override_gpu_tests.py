#!/usr/bin/env python3
"""Real Slang/Vulkan regression for production Prism-selected HLSL overrides.

Run: python3 libultraship/tests/fury_shader_override_gpu_tests.py [build-cmake] [renderer-build]
The second path defaults to <build-cmake>/fury-renderer-clang and must contain
lib/libfury_renderer.so. Uses a Vulkan device, but no display, ROM or swapchain.
Archive lookup and adapter ownership are isolated doubles; source generation,
ABI annotation, Slang compilation, GPU draw and readback are production code.
"""
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile

# Share extraction/doubles with the source-level regression without running it.
harness = runpy.run_path(str(Path(__file__).with_name("fury_shader_override_tests.py")))
root, lus, build = (harness[key] for key in ("root", "lus", "build"))
renderer = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else build / "fury-renderer-clang"
library = renderer / "lib/libfury_renderer.so"
if not library.is_file():
    raise SystemExit(f"Build the Fury renderer first; missing {library}")
# Keep the adapter shim, but use the actual C bridge declarations/implementation.
bridge = "static CCFeatures current;\nvoid gfx_cc_get_features(uint64_t, uint64_t, CCFeatures* f) { *f = current; }\n"
bridge += "namespace Fast {" + harness["bridge"].split("namespace Fast {", 1)[1]
tests = r'''
} // namespace Fast
static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    require(argc == 2, "expected template path");
    std::ifstream file(argv[1]);
    std::string stock((std::istreambuf_iterator<char>(file)), {});
    require(!stock.empty(), "missing stock template");
    auto& manager = *Ship::Context::GetRawInstance()->GetResourceManager();
    auto put = [&](const std::string& path, const std::string& data) {
        manager.files[path] = std::make_shared<Ship::Shader>(Ship::Shader{data});
    };
    put("shaders/directx/default.shader.hlsl", stock);
    auto custom = stock;
    auto textureType = custom.find("Texture2D g_texture0");
    require(textureType != std::string::npos, "stock texture declaration changed");
    custom.replace(textureType, 9, "Texture2D<float4>");
    auto position = custom.find("return output;");
    require(position != std::string::npos, "stock return changed");
    custom.replace(position, 14, "output.color = customColor(); return output;");
    // A real archive include contributes the custom pixel computation.
    custom.insert(custom.find('\n') + 1, "@include(\"mods/color.hlsl\")\n");
    put("mods/custom.hlsl", splicedAbi(formattedConstantBuffers(custom)));
    put("mods/color.hlsl", "@prism(type='hlsl')\n#define CUSTOM_GREEN float4(0,1,0,1)\nfloat4 customColor() { return CUSTOM_GREEN; }\n");
    auto context = std::unique_ptr<ShipFury, decltype(&ship_fury_destroy)>(
        ship_fury_create({2, nullptr, nullptr, 0}, 1), ship_fury_destroy);
    require(bool(context), "Fury creation failed");
    auto check = [&](bool ok) { require(ok, ship_fury_error(context.get())); };
    check(!*ship_fury_error(context.get()));
    check(ship_fury_framebuffer(context.get(), 1, 8, 8, 1, 1));
    uint32_t texture = ship_fury_texture(context.get());
    check(texture != 0);
    const uint8_t white[] = {255,255,255,255};
    check(ship_fury_upload(context.get(), texture, white, 1, 1));
    for (int variant = 0; variant < 8; ++variant) {
        current = {};
        current.numInputs = 1;
        current.opt_alpha = variant & 1;
        current.usedTextures[0] = current.usedTextures[1] = variant & 2;
        current.opt_fog = current.opt_grayscale = current.opt_prim_depth = variant & 4;
        current.opt_2cyc = variant & 4;
        current.opt_noise = current.opt_texture_edge = current.opt_alpha_threshold = current.opt_alpha;
        current.clamp[0][0] = current.usedTextures[0];
        current.clamp[1][1] = current.usedTextures[1];
        current.used_masks[0] = current.usedTextures[0];
        current.used_blend[1] = current.usedTextures[1];
        for (int cycle = 0; cycle < 2; ++cycle)
            for (int channel = 0; channel < 2; ++channel) {
                current.do_single[cycle][channel] = true;
                current.c[cycle][channel][3] = SHADER_INPUT_1;
            }
        std::vector<float> vertices;
        for (auto xy : {std::pair{-1.f,-1.f}, std::pair{3.f,-1.f}, std::pair{-1.f,3.f}}) {
            vertices.insert(vertices.end(), {xy.first, xy.second, .25f, 1.f});
            for (int i = 0; i < 2; ++i) if (current.usedTextures[i]) {
                vertices.insert(vertices.end(), {.5f,.5f});
                for (int j = 0; j < 2; ++j) if (current.clamp[i][j]) vertices.push_back(1);
            }
            if (current.opt_fog) vertices.insert(vertices.end(), {0,0,0,0});
            if (current.opt_grayscale) vertices.insert(vertices.end(), {0,0,0,0});
            vertices.insert(vertices.end(), {1,0,0});
            if (current.opt_alpha) vertices.push_back(1);
        }
        for (bool override : {false, true}) {
            selected = override ? "mods/custom" : nullptr;
            current.shader_id = override ? 0 : -1;
            Fast::GfxRenderingAPIFury adapter;
            adapter.mContext = context.get();
            adapter.mFilter = variant & 2 ? 2 : 0;
            adapter.mSrgbMode = variant & 4;
            adapter.CreateAndLoadNewShader(0, 0);
            ShipFuryDraw state = {};
            state.program = adapter.mPrograms.at({0,0}).id;
            state.framebuffer = 1;
            state.viewport = state.scissor = {0,0,8,8};
            // Keep the noise-enabled variants deterministic and away from the
            // random(0) half-way rounding/discard boundary.
            state.noise_frame = 17;
            state.noise_scale = 0;
            state.prim_depth = .25f;
            for (auto& id : state.textures) id = texture;
            state.width[0] = state.width[1] = state.height[0] = state.height[1] = 1;
            check(ship_fury_clear(context.get(), 1, 1, 1, nullptr));
            check(ship_fury_draw(context.get(), &state, vertices.data(), vertices.size()*sizeof(float), 3, nullptr, 0, 0));
            uint16_t pixels[64] = {};
            check(ship_fury_read_color(context.get(), 1, 8, 8, pixels));
            uint16_t expected = override ? 0x07c1 : 0xf801;
            for (auto pixel : pixels)
                require(pixel == expected, "wrong RGBA5551 output for variant " + std::to_string(variant) +
                        (override ? " override: " : " default: ") + std::to_string(pixel));
        }
    }
    // Invalid buffers must fail before reaching Slang/the runtime's fixed-size uploads.
    selected = "mods/custom";
    current.shader_id = 0;
    for (const auto& invalidBuffer : invalidConstantBuffers(stock)) {
        put("mods/custom.hlsl", invalidBuffer);
        Fast::GfxRenderingAPIFury rejected;
        rejected.mContext = context.get();
        rejected.mFilter = 2;
        bool rejectedLayout = false;
        try { rejected.CreateAndLoadNewShader(0, 0); }
        catch (const std::runtime_error& error) {
            std::string message = error.what();
            rejectedLayout = message.find("mods/custom.hlsl") != std::string::npos &&
                             message.find("constant-buffer member layout") != std::string::npos;
        }
        require(rejectedLayout && rejected.mPrograms.empty(), "invalid constant-buffer layout accepted/cached");
    }
    for (const auto& [invalidSource, expectedDiagnostic] : invalidPreprocessing(stock)) {
        put("mods/custom.hlsl", invalidSource);
        Fast::GfxRenderingAPIFury rejected;
        rejected.mContext = context.get();
        rejected.mFilter = 2;
        bool rejectedAbi = false;
        try { rejected.CreateAndLoadNewShader(0, 0); }
        catch (const std::runtime_error& error) {
            std::string message = error.what();
            rejectedAbi = message.find("mods/custom.hlsl") != std::string::npos &&
                          message.find(expectedDiagnostic) != std::string::npos;
        }
        require(rejectedAbi && rejected.mPrograms.empty(), "ABI-changing preprocessing accepted/cached");
    }
    put("mods/custom.hlsl", formattedConstantBuffers(custom));
    // Unsupported GLSL/other ABI errors remain covered by the source-level regression;
    // this verifies that actual Slang diagnostics keep the selected path too.
    put("mods/color.hlsl", "@prism(type='hlsl')\nfloat4 customColor() { return undefined_custom_identifier; }\n");
    selected = "mods/custom";
    Fast::GfxRenderingAPIFury adapter;
    adapter.mContext = context.get();
    bool failed = false;
    try { adapter.CreateAndLoadNewShader(0, 0); }
    catch (const std::runtime_error& error) {
        std::string message = error.what();
        failed = message.find("mods/custom.hlsl") != std::string::npos &&
                 message.find("undefined_custom_identifier") != std::string::npos;
    }
    require(failed && adapter.mPrograms.empty(), "missing real Slang diagnostic or cached failed program");
    std::cout << "16 real Slang/Vulkan default/custom variants: all 64 pixels match; 19 invalid constant-buffer layouts and 11 preprocessing bypasses rejected; compiler diagnostics passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix="fury-shader-gpu-tests-") as directory:
    source = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    source.write_text("".join(harness[key] for key in ("preamble", "items", "features", "generator")) +
                      bridge + harness["adapter"] + "} // namespace Fast\n" + harness["cbuffer_tests"] + "namespace Fast {\n" + tests)
    subprocess.run([
        os.environ.get("CXX", "c++"), "-std=c++20", "-DENABLE_FURY", "-O0", "-g",
        "-I" + str(harness["prism"]), "-I" + str(lus / "include"),
        "-I" + str(root / "renderer/fury"), str(source), str(harness["prism_lib"]),
        str(library), "-Wl,-rpath," + str(library.parent), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(lus / "src/fast/shaders/directx/default.shader.hlsl")], check=True, timeout=120)
