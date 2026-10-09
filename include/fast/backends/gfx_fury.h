#pragma once
#ifdef ENABLE_FURY
#include "gfx_rendering_api.h"
#include "Bridge.h"
#include <imgui.h>
#include <map>
#include <memory>
#include <vector>

namespace Fast {
class GfxWindowBackendSDL2;
class GfxRenderingAPIFury final : public GfxRenderingAPI {
  public:
    explicit GfxRenderingAPIFury(GfxWindowBackendSDL2* window);
    ~GfxRenderingAPIFury() override;
    void Shutdown() override;
    void RenderGui(ImDrawData* data);
    void InitGui() {
        EnsureGui();
    }
    const char* GetName() override {
        return "Fury Vulkan";
    }
    int GetMaxTextureSize() override {
        return 16384;
    }
    GfxClipParameters GetClipParameters() override {
        return { true, false };
    }
    void UnloadShader(ShaderProgram*) override {
    }
    void LoadShader(ShaderProgram* program) override;
    void ClearShaderCache() override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t id0, uint64_t id1) override;
    ShaderProgram* LookupShader(uint64_t id0, uint64_t id1) override;
    void ShaderGetInfo(ShaderProgram* program, uint8_t* inputs, bool textures[2]) override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t texture) override;
    void UploadTexture(const uint8_t* rgba, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int tile, bool linear, uint32_t s, uint32_t t) override;
    void SetDepthTestAndMask(bool test, bool write) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int w, int h) override;
    void SetScissor(int x, int y, int w, int h) override;
    void SetUseAlpha(bool alpha) override;
    void DrawTriangles(float vertices[], size_t length, size_t triangles) override;
    void Init() override;
    void OnResize() override {
    }
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override {
    }
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int id, uint32_t w, uint32_t h, uint32_t samples, bool invert, bool target,
                                     bool depth, bool extract) override;
    void StartDrawToFramebuffer(int id, float noiseScale) override;
    void CopyFramebuffer(int dst, int src, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1,
                         int dy1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ClearDepthRegion(int x, int y, int w, int h) override;
    void ReadFramebufferToCPU(int id, uint32_t w, uint32_t h, uint16_t* rgba) override;
    void ResolveMSAAColorBuffer(int dst, int src) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int id) override;
    void SelectTextureFb(int id) override;
    void DeleteTexture(uint32_t texture) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override {
        return mFilter;
    }
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;
    void SetCurrentPrimDepth(float depth) override;

  private:
    struct Program {
        uint32_t id;
        uint8_t inputs;
        bool textures[2];
    };
    struct Texture {
        uint32_t width = 1, height = 1, sampler = 0, linear = 0;
    };
    struct Framebuffer {
        uint32_t width = 1, height = 1;
    };
    void Check(int result);
    void EnsureGui();
    GfxWindowBackendSDL2* mWindow;
    ShipFury* mContext = nullptr;
    ShipFuryDraw mDraw = {};
    uint32_t mTile = 0, mGuiProgram = 0, mFont = 0;
    FilteringMode mFilter = FILTER_THREE_POINT;
    std::map<std::pair<uint64_t, uint64_t>, Program> mPrograms;
    std::map<uint32_t, Texture> mTextures;
    std::vector<Framebuffer> mFramebuffers = { {} };
};
} // namespace Fast
#endif
