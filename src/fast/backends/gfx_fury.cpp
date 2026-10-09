#ifdef ENABLE_FURY
#include "fast/backends/gfx_fury.h"
#include "fast/backends/gfx_hlsl_shader.h"
#include "fast/backends/gfx_sdl.h"
#include "fast/interpreter.h"
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include <SDL2/SDL_syswm.h>
#include <SDL2/SDL_vulkan.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <stdexcept>

namespace Fast {
GfxRenderingAPIFury::GfxRenderingAPIFury(GfxWindowBackendSDL2* window) : mWindow(window) {
}
GfxRenderingAPIFury::~GfxRenderingAPIFury() {
    Shutdown();
}
void GfxRenderingAPIFury::Check(int result) {
    if (!result) {
        std::string error = ship_fury_error(mContext);
        SPDLOG_CRITICAL("Fury renderer: {}", error);
        throw std::runtime_error(error);
    }
}
void GfxRenderingAPIFury::Init() {
    if (mContext)
        return;
    SDL_SysWMinfo info = {};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(mWindow->GetSDLWindow(), &info))
        throw std::runtime_error(SDL_GetError());
    ShipFuryWindow window = {};
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    if (info.subsystem == SDL_SYSWM_WAYLAND) {
        window.kind = 1;
        window.display = info.info.wl.display;
        window.surface = info.info.wl.surface;
    }
#endif
#if defined(SDL_VIDEO_DRIVER_X11)
    if (info.subsystem == SDL_SYSWM_X11) {
        window.kind = 2;
        window.display = info.info.x11.display;
        window.window = info.info.x11.window;
    }
#endif
#ifdef _WIN32
    if (info.subsystem == SDL_SYSWM_WINDOWS) {
        window.kind = 3;
        window.surface = info.info.win.window;
    }
#endif
    mContext = ship_fury_create(window, std::getenv("SHIP_FURY_VALIDATION") != nullptr);
    Check(mContext && !*ship_fury_error(mContext));
}
void GfxRenderingAPIFury::Shutdown() {
    if (!mContext)
        return;
    // The SDL window must still exist while the runtime retires its swapchain.
    ship_fury_destroy(mContext);
    mContext = nullptr;
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().Fonts->SetTexID(nullptr);
}
void GfxRenderingAPIFury::LoadShader(ShaderProgram* p) {
    mDraw.program = reinterpret_cast<Program*>(p)->id;
}
void GfxRenderingAPIFury::ClearShaderCache() {
    Check(ship_fury_clear_programs(mContext));
    mPrograms.clear();
    mGuiProgram = 0;
    mDraw.program = 0;
}
ShaderProgram* GfxRenderingAPIFury::LookupShader(uint64_t a, uint64_t b) {
    auto i = mPrograms.find({ a, b });
    return i == mPrograms.end() ? nullptr : reinterpret_cast<ShaderProgram*>(&i->second);
}
ShaderProgram* GfxRenderingAPIFury::CreateAndLoadNewShader(uint64_t a, uint64_t b) {
    CCFeatures features;
    gfx_cc_get_features(a, b, &features);
    if (features.shader_id >= 0 && gfx_get_shader(features.shader_id))
        throw std::runtime_error("Fury custom Prism shader overrides are not supported yet");
    size_t floats = 0;
    std::string source =
        gfx_direct3d_common_build_shader(floats, features, false, mFilter == FILTER_THREE_POINT, mSrgbMode);
    // DX11 accepts implicit truncation here; Slang requires the declared float3 assignment.
    if (!features.opt_alpha)
        for (int i = 1; i <= features.numInputs; ++i) {
            std::string name = "input" + std::to_string(i);
            std::string from = "result." + name + " = float4(" + name + ", 1.0);";
            auto pos = source.find(from);
            if (pos != std::string::npos)
                source.replace(pos, from.size(), "result." + name + " = " + name + ";");
        }
    // Explicit Vulkan bindings preserve the fixed legacy six-texture layout without register collisions.
    for (int i = 0; i < 6; ++i) {
        std::string name = i < 2   ? "g_texture" + std::to_string(i)
                           : i < 4 ? "g_textureMask" + std::to_string(i - 2)
                                   : "g_textureBlend" + std::to_string(i - 4);
        std::string from = "Texture2D " + name;
        auto pos = source.find(from);
        if (pos != std::string::npos)
            source.insert(pos, "[[vk::binding(" + std::to_string(i) + ",0)]] ");
    }
    for (int i = 0; i < 2; ++i) {
        auto pos = source.find("SamplerState g_sampler" + std::to_string(i));
        if (pos != std::string::npos)
            source.insert(pos, "[[vk::binding(" + std::to_string(6 + i) + ",0)]] ");
    }
    const char* cb[] = { "cbuffer PerFrameCB", "cbuffer PerDrawCB", "cbuffer PerPrimDepthCB" };
    for (int i = 0; i < 3; ++i) {
        auto pos = source.find(cb[i]);
        if (pos != std::string::npos)
            source.insert(pos, "[[vk::binding(" + std::to_string(8 + i) + ",0)]] ");
    }
    std::vector<ShipFuryAttribute> attributes;
    uint32_t offset = 0;
    auto attr = [&](uint32_t n) {
        attributes.push_back({ n, offset });
        offset += n * 4;
    };
    attr(4);
    for (int i = 0; i < 2; ++i)
        if (features.usedTextures[i]) {
            attr(2);
            for (int j = 0; j < 2; ++j)
                if (features.clamp[i][j])
                    attr(1);
        }
    if (features.opt_fog)
        attr(4);
    if (features.opt_grayscale)
        attr(4);
    for (int i = 0; i < features.numInputs; ++i)
        attr(features.opt_alpha ? 4 : 3);
    auto start = source.find("PSInput VSMain(");
    auto end = source.find(") {", start);
    if (start == std::string::npos || end == std::string::npos)
        throw std::runtime_error("Missing HLSL vertex entry point");
    std::string signature = source.substr(start, end - start);
    std::regex parameter(R"(float[234]?\s+\w+\s*:\s*\w+)");
    std::string annotated;
    size_t last = 0;
    uint32_t location = 0;
    for (std::sregex_iterator i(signature.begin(), signature.end(), parameter), e; i != e; ++i) {
        annotated += signature.substr(last, i->position() - last) + "[[vk::location(" + std::to_string(location++) +
                     ")]] " + i->str();
        last = i->position() + i->length();
    }
    annotated += signature.substr(last);
    source.replace(start, end - start, annotated);
    if (offset != floats * 4 || location != attributes.size())
        throw std::runtime_error("HLSL vertex layout does not match interpreter");
    uint32_t id =
        ship_fury_program(mContext, source.data(), source.size(), attributes.data(), attributes.size(), offset);
    Check(id != 0);
    auto& p = mPrograms[{ a, b }];
    p = { id, static_cast<uint8_t>(features.numInputs), { features.usedTextures[0], features.usedTextures[1] } };
    auto result = reinterpret_cast<ShaderProgram*>(&p);
    LoadShader(result);
    return result;
}
void GfxRenderingAPIFury::ShaderGetInfo(ShaderProgram* prg, uint8_t* inputs, bool textures[2]) {
    auto& p = *reinterpret_cast<Program*>(prg);
    *inputs = p.inputs;
    textures[0] = p.textures[0];
    textures[1] = p.textures[1];
}
uint32_t GfxRenderingAPIFury::NewTexture() {
    Init();
    auto id = ship_fury_texture(mContext);
    Check(id != 0);
    mTextures[id] = {};
    return id;
}
void GfxRenderingAPIFury::SelectTexture(int tile, uint32_t id) {
    mTile = tile;
    mDraw.textures[tile] = id;
}
void GfxRenderingAPIFury::UploadTexture(const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (!w || !h)
        return;
    auto id = mDraw.textures[mTile];
    Check(ship_fury_upload(mContext, id, rgba, w, h));
    mTextures[id].width = w;
    mTextures[id].height = h;
}
void GfxRenderingAPIFury::SetSamplerParameters(int tile, bool linear, uint32_t s, uint32_t t) {
    auto& tex = mTextures[mDraw.textures[tile]];
    tex.sampler = (linear && mFilter == FILTER_LINEAR) | (s << 1) | (t << 3);
    tex.linear = linear;
}
void GfxRenderingAPIFury::SetDepthTestAndMask(bool test, bool write) {
    mDraw.depth_test = test;
    mDraw.depth_write = write;
}
void GfxRenderingAPIFury::SetZmodeDecal(bool decal) {
    mDraw.decal = decal;
}
void GfxRenderingAPIFury::SetViewport(int x, int y, int w, int h) {
    mDraw.viewport = { x, int(mFramebuffers[mDraw.framebuffer].height) - y - h, w, h };
}
void GfxRenderingAPIFury::SetScissor(int x, int y, int w, int h) {
    mDraw.scissor = { x, int(mFramebuffers[mDraw.framebuffer].height) - y - h, w, h };
}
void GfxRenderingAPIFury::SetUseAlpha(bool alpha) {
    mDraw.alpha = alpha;
}
void GfxRenderingAPIFury::SetCurrentPrimDepth(float depth) {
    mDraw.prim_depth = depth;
}
void GfxRenderingAPIFury::DrawTriangles(float v[], size_t length, size_t triangles) {
    for (int i = 0; i < 2; ++i) {
        auto& t = mTextures[mDraw.textures[i]];
        mDraw.width[i] = t.width;
        mDraw.height[i] = t.height;
        mDraw.sampler[i] = t.sampler;
        mDraw.linear[i] = t.linear;
    }
    Check(ship_fury_draw(mContext, &mDraw, v, length * sizeof(float), triangles * 3, nullptr, 0, 0));
}
void GfxRenderingAPIFury::StartFrame() {
    ++mDraw.noise_frame;
}
void GfxRenderingAPIFury::EndFrame() {
    int w, h;
    SDL_Vulkan_GetDrawableSize(mWindow->GetSDLWindow(), &w, &h);
    Check(ship_fury_present(mContext, std::max(w, 0), std::max(h, 0),
                            Ship::Context::GetRawInstance()->GetConsoleVariables()->GetInteger(CVAR_VSYNC_ENABLED, 1)));
}
int GfxRenderingAPIFury::CreateFramebuffer() {
    mFramebuffers.push_back({});
    return mFramebuffers.size() - 1;
}
void GfxRenderingAPIFury::UpdateFramebufferParameters(int id, uint32_t w, uint32_t h, uint32_t samples, bool, bool,
                                                      bool depth, bool) {
    Init();
    if (!w || !h)
        return;
    mFramebuffers.at(id) = { w, h };
    Check(ship_fury_framebuffer(mContext, id, w, h, samples, depth));
}
void GfxRenderingAPIFury::StartDrawToFramebuffer(int id, float scale) {
    mDraw.framebuffer = id;
    mDraw.noise_scale = scale;
}
void GfxRenderingAPIFury::ClearFramebuffer(bool color, bool depth) {
    Check(ship_fury_clear(mContext, mDraw.framebuffer, color, depth, nullptr));
}
void GfxRenderingAPIFury::ClearDepthRegion(int x, int y, int w, int h) {
    ShipFuryRect r = { x, int(mFramebuffers[mDraw.framebuffer].height) - y - h, w, h };
    Check(ship_fury_clear(mContext, mDraw.framebuffer, 0, 1, &r));
}
void GfxRenderingAPIFury::CopyFramebuffer(int dst, int src, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0,
                                          int dx1, int dy1) {
    Check(ship_fury_blit(mContext, dst, src, { dx0, dy0, dx1 - dx0, dy1 - dy0 }, { sx0, sy0, sx1 - sx0, sy1 - sy0 }));
}
void GfxRenderingAPIFury::ResolveMSAAColorBuffer(int dst, int src) {
    auto a = mFramebuffers.at(src), b = mFramebuffers.at(dst);
    Check(ship_fury_blit(mContext, dst, src, { 0, 0, int(b.width), int(b.height) },
                         { 0, 0, int(a.width), int(a.height) }));
}
void GfxRenderingAPIFury::ReadFramebufferToCPU(int id, uint32_t w, uint32_t h, uint16_t* rgba) {
    Check(ship_fury_read_color(mContext, id, w, h, rgba));
}
std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIFury::GetPixelDepth(int id, const std::set<std::pair<float, float>>& coordinates) {
    std::vector<int32_t> xy;
    std::vector<uint16_t> depth(coordinates.size());
    for (auto& p : coordinates) {
        xy.push_back(p.first);
        xy.push_back(int(mFramebuffers.at(id).height) - 1 - int(p.second));
    }
    Check(ship_fury_read_depth(mContext, id, xy.data(), depth.size(), depth.data()));
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> result;
    size_t i = 0;
    for (auto& p : coordinates)
        result[p] = depth[i++];
    return result;
}
void* GfxRenderingAPIFury::GetFramebufferTextureId(int id) {
    auto texture = ship_fury_framebuffer_texture(mContext, id);
    Check(texture != 0);
    return reinterpret_cast<void*>(uintptr_t(texture));
}
void GfxRenderingAPIFury::SelectTextureFb(int id) {
    auto texture = ship_fury_framebuffer_texture(mContext, id);
    Check(texture != 0);
    SelectTexture(0, texture);
    auto f = mFramebuffers.at(id);
    mTextures[texture].width = f.width;
    mTextures[texture].height = f.height;
}
void GfxRenderingAPIFury::DeleteTexture(uint32_t id) {
    Check(ship_fury_delete_texture(mContext, id));
    mTextures.erase(id);
}
void GfxRenderingAPIFury::SetTextureFilter(FilteringMode mode) {
    if (mFilter == mode)
        return;
    mFilter = mode;
    gfx_texture_cache_clear();
    gfx_shader_cache_clear();
}
void GfxRenderingAPIFury::SetSrgbMode() {
    mSrgbMode = true;
    gfx_shader_cache_clear();
}
ImTextureID GfxRenderingAPIFury::GetTextureById(int id) {
    return reinterpret_cast<ImTextureID>(uintptr_t(id));
}
void GfxRenderingAPIFury::EnsureGui() {
    Init();
    if (!mFont || !ImGui::GetIO().Fonts->IsBuilt()) {
        unsigned char* pixels;
        int w, h;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        if (!mFont)
            mFont = NewTexture();
        Check(ship_fury_upload(mContext, mFont, pixels, w, h));
        ImGui::GetIO().Fonts->SetTexID(GetTextureById(mFont));
    }
    if (mGuiProgram)
        return;
    const char* source = R"(
[[vk::binding(0,0)]] Texture2D<float4> image : register(t0);
[[vk::binding(6,0)]] SamplerState imageSampler : register(s0);
[[vk::binding(8,0)]] cbuffer Transform : register(b0) {float2 scale;float2 translate;}
struct V {float4 position:SV_Position;float2 uv:TEXCOORD0;float4 color:COLOR0;};
V VSMain([[vk::location(0)]] float2 position:POSITION,[[vk::location(1)]] float2 uv:TEXCOORD0,[[vk::location(2)]] float4 color:COLOR0){V v;v.position=float4(position*scale+translate,0,1);v.uv=uv;v.color=color;return v;}
float4 PSMain(V v):SV_Target{return v.color*image.Sample(imageSampler,v.uv);}
)";
    ShipFuryAttribute attributes[] = { { 2, offsetof(ImDrawVert, pos) },
                                       { 2, offsetof(ImDrawVert, uv) },
                                       { 0, offsetof(ImDrawVert, col) } };
    mGuiProgram = ship_fury_program(mContext, source, std::strlen(source), attributes, 3, sizeof(ImDrawVert));
    Check(mGuiProgram != 0);
}
void GfxRenderingAPIFury::RenderGui(ImDrawData* data) {
    if (data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0)
        return;
    EnsureGui();
    ShipFuryDraw state = {};
    state.program = mGuiProgram;
    state.gui = 1;
    state.alpha = 1;
    state.sampler[0] = 1 | (2 << 1) | (2 << 3);
    state.viewport = { 0, 0, int(data->DisplaySize.x * data->FramebufferScale.x),
                       int(data->DisplaySize.y * data->FramebufferScale.y) };
    state.gui_transform[0] = 2 / data->DisplaySize.x;
    state.gui_transform[1] = -2 / data->DisplaySize.y;
    state.gui_transform[2] = -1 - data->DisplayPos.x * state.gui_transform[0];
    state.gui_transform[3] = 1 - data->DisplayPos.y * state.gui_transform[1];
    for (int i = 0; i < data->CmdListsCount; ++i) {
        auto* list = data->CmdLists[i];
        for (auto& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                    cmd.UserCallback(list, &cmd);
                continue;
            }
            int x = std::max(int((cmd.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x), 0),
                y = std::max(int((cmd.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y), 0);
            int right = std::min(int((cmd.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x),
                                 state.viewport.width),
                bottom = std::min(int((cmd.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y),
                                  state.viewport.height);
            if (right <= x || bottom <= y)
                continue;
            state.scissor = { x, y, right - x, bottom - y };
            state.textures[0] = uintptr_t(cmd.GetTexID());
            Check(ship_fury_draw(mContext, &state, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert),
                                 cmd.ElemCount, list->IdxBuffer.Data + cmd.IdxOffset, sizeof(ImDrawIdx),
                                 cmd.VtxOffset));
        }
    }
}
} // namespace Fast
#endif
