#!/usr/bin/env python3
"""No-build host wiring regressions; these do not exercise SDL or Vulkan.

Run: python3 libultraship/tests/fury_viewport_host_tests.py
Live minimize/restore and focus-switch coverage still requires native windows.
"""
from pathlib import Path
import unittest

LUS = Path(__file__).resolve().parents[1]


def method(path, signature):
    source = (LUS / path).read_text()
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[brace + 1:end - 1]


class FuryViewportHostTests(unittest.TestCase):
    def test_snapshot_before_present_including_main_only_frames(self):
        body = method("src/fast/Fast3dGui.cpp", "void Fast3dGui::DrawFloatingWindows()")
        fury = body.split("#endif", 1)[0]
        self.assertLess(fury.index("ImGui::UpdatePlatformWindows()"), fury.index("api->PublishViewportStates()"))
        self.assertLess(fury.index("api->PublishViewportStates()"), fury.index("ImGui::RenderPlatformWindowsDefault()"))
        # Publication is outside both single-statement conditional blocks.
        self.assertIn("if (viewportsEnabled)\n            ImGui::UpdatePlatformWindows();", fury)
        self.assertIn("api->PublishViewportStates();\n        if (viewportsEnabled)", fury)

    def test_every_present_refreshes_all_surfaces_without_resetting_rendered_flags(self):
        path = "src/fast/backends/gfx_fury.cpp"
        refresh = method(path, "void GfxRenderingAPIFury::RefreshViewportStates()")
        self.assertLess(refresh.index("SDL_PumpEvents()"), refresh.index("ImGui::GetPlatformIO().Viewports"))
        self.assertNotIn("rendered =", refresh)
        for signature, present in (("void GfxRenderingAPIFury::EndFrame()", "ship_fury_present("),
                                   ("void GfxRenderingAPIFury::PresentViewport(", "ship_fury_present_surface(")):
            body = method(path, signature)
            self.assertLess(body.index("RefreshViewportStates()"), body.index(present))
        publish = method(path, "void GfxRenderingAPIFury::PublishViewportStates()")
        self.assertIn("data->rendered = false", publish)
        self.assertIn("RefreshViewportStates()", publish)

    def test_snapshot_does_not_depend_on_render_callbacks(self):
        body = method("src/fast/backends/gfx_fury.cpp", "void GfxRenderingAPIFury::RefreshViewportStates()")
        self.assertIn("ImGui::GetPlatformIO().Viewports", body)
        self.assertIn("SDL_WINDOW_MINIMIZED", body)
        self.assertIn("SDL_Vulkan_GetDrawableSize", body)
        self.assertIn("data ? data->surface : 0", body)
        self.assertIn("std::max(w, 0), std::max(h, 0)", body)
        self.assertNotIn("data->rendered = false", body)
        self.assertNotIn("DrawData", body)
        self.assertNotIn("if (minimized)", body)

    def test_mouse_polling_is_main_window_only(self):
        for name in ("GetMousePos", "GetMouseDelta", "GetMouseState"):
            body = method("src/fast/backends/gfx_sdl2.cpp", f"GfxWindowBackendSDL2::{name}(")
            self.assertIn("mFury &&", body)
            self.assertIn("SDL_GetKeyboardFocus() != mWnd", body)
            self.assertIn("SDL_GetMouseFocus() != mWnd", body)
        delta = method("src/fast/backends/gfx_sdl2.cpp", "GfxWindowBackendSDL2::GetMouseDelta(")
        self.assertLess(delta.index("SDL_GetRelativeMouseState"), delta.index("if (mFury"))

    def test_detached_motion_and_focus_transitions_drain_accumulator(self):
        body = method("src/fast/backends/gfx_sdl2.cpp", "void GfxWindowBackendSDL2::HandleSingleEvent(")
        motion = body.split("case SDL_MOUSEMOTION:", 1)[1].split("break;", 1)[0]
        self.assertIn("event.motion.windowID", motion)
        self.assertIn("SDL_GetRelativeMouseState(nullptr, nullptr)", motion)
        for event in ("FOCUS_GAINED", "FOCUS_LOST"):
            block = body.split(f"case SDL_WINDOWEVENT_{event}:", 1)[1].split("break;", 1)[0]
            self.assertIn("SDL_GetRelativeMouseState(nullptr, nullptr)", block)
        events = method("src/fast/backends/gfx_sdl2.cpp", "void GfxWindowBackendSDL2::HandleEvents()")
        self.assertIn("SDL_GetRelativeMouseState(nullptr, nullptr)", events)


if __name__ == "__main__":
    unittest.main()
