// Compiles with the production controller/mapping translation units via the Python runner.
// Only application services are replaced: input blocking is off, and any window/config
// access aborts. No SDL initialization, native window, assets, or game is required.
#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"
#include "ship/controller/controldeck/ControlDeck.h"
#include "ship/controller/controldevice/controller/ControllerButton.h"
#include "ship/controller/controldevice/controller/ControllerStick.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardKeyToAxisDirectionMapping.h"
#include "ship/controller/controldevice/controller/mapping/keyboard/KeyboardKeyToButtonMapping.h"
#include "ship/window/Window.h"
#include <cstdlib>
#include <iostream>
#include <vector>

namespace Ship {
// Link-time substitutes for application services, not for controller dispatch or mappings.
Context::Context(std::string, std::string, std::string) {
}
Context::~Context() = default;
Context* Context::GetRawInstance() {
    static Context context("headless", "headless", "");
    return &context;
}
ControlDeck::ControlDeck(std::vector<CONTROLLERBUTTONS_T>, std::shared_ptr<ControllerDefaultMappings>,
                         std::unordered_map<CONTROLLERBUTTONS_T, std::string>) {
}
ControlDeck::~ControlDeck() = default;
bool ControlDeck::KeyboardGameInputBlocked() {
    return false;
}
class HeadlessDeck final : public ControlDeck {
  public:
    HeadlessDeck() : ControlDeck({}, nullptr, {}) {
    }
    void WriteToPad(void*) override {
        std::abort();
    }
};
std::shared_ptr<ControlDeck> Context::GetControlDeck() const {
    static auto deck = std::make_shared<HeadlessDeck>();
    return deck;
}
std::shared_ptr<ConsoleVariable> Context::GetConsoleVariables() const {
    std::abort();
}
std::shared_ptr<Window> Context::GetWindow() const {
    std::abort();
}
void ConsoleVariable::SetInteger(const char*, int32_t) {
    std::abort();
}
void ConsoleVariable::SetString(const char*, const char*) {
    std::abort();
}
void ConsoleVariable::ClearVariable(const char*) {
    std::abort();
}
void ConsoleVariable::Save() {
    std::abort();
}
} // namespace Ship

using namespace Ship;
static int failures = 0;
static void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

static void ButtonFocusLoss() {
    ControllerButton button(0, 1);
    std::vector<std::shared_ptr<KeyboardKeyToButtonMapping>> mappings;
    for (auto key : { LUS_KB_A, LUS_KB_B, LUS_KB_C }) {
        auto mapping = std::make_shared<KeyboardKeyToButtonMapping>(0, 1, key);
        mappings.push_back(mapping);
        button.AddButtonMapping(mapping);
        Check(button.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, key), "button key-down handled");
    }
    CONTROLLERBUTTONS_T pad = 0;
    button.UpdatePad(pad);
    Check(pad == 1, "button pressed before focus loss");
    // Focus loss supplies ALL_KEYS_UP; detached-window key-up is filtered, so send none.
    Check(button.ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN), "button all-keys-up handled");
    for (auto mapping : mappings) {
        pad = 0;
        mapping->UpdatePad(pad);
        Check(pad == 0, "every button mapping released on focus loss");
    }
    pad = 0;
    button.UpdatePad(pad);
    Check(pad == 0, "button not latched without detached-window key-up");
    Check(button.ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN), "repeated button release handled");
    Check(button.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_B), "button press after focus regain");
    Check(button.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_UP, LUS_KB_B), "button normal release after focus regain");
    pad = 0;
    button.UpdatePad(pad);
    Check(pad == 0, "button normal key-up releases");
    Check(!button.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_Z), "unmapped button key unhandled");
}

static void StickFocusLoss() {
    ControllerStick stick(0, LEFT_STICK);
    std::vector<std::shared_ptr<KeyboardKeyToAxisDirectionMapping>> mappings;
    const Direction directions[] = { UP, DOWN, LEFT, RIGHT, UP, RIGHT };
    const KbScancode keys[] = { LUS_KB_W, LUS_KB_S, LUS_KB_A, LUS_KB_D, LUS_KB_I, LUS_KB_L };
    for (size_t i = 0; i < 6; ++i) {
        auto mapping = std::make_shared<KeyboardKeyToAxisDirectionMapping>(0, LEFT_STICK, directions[i], keys[i]);
        mappings.push_back(mapping);
        stick.AddAxisDirectionMapping(directions[i], mapping);
        Check(stick.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, keys[i]), "stick key-down handled");
        Check(mapping->GetNormalizedAxisDirectionValue() > 0, "stick direction pressed before focus loss");
    }
    Check(stick.ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN), "stick all-keys-up handled");
    // Check individual mappings as well as the pad: opposing latched directions can cancel.
    for (auto mapping : mappings) {
        Check(mapping->GetNormalizedAxisDirectionValue() == 0, "every stick mapping released on focus loss");
    }
    int8_t x = 99, y = 99;
    stick.UpdatePad(x, y);
    Check(x == 0 && y == 0, "stick neutral without detached-window key-up");
    Check(stick.ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN), "repeated stick release handled");
    Check(stick.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_D), "stick press after focus regain");
    stick.UpdatePad(x, y);
    Check(x > 0 && y == 0, "stick output after focus regain");
    Check(stick.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_UP, LUS_KB_D), "stick normal release after focus regain");
    stick.UpdatePad(x, y);
    Check(x == 0 && y == 0, "stick normal key-up releases");
    Check(!stick.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_UP, LUS_KB_Z), "unmapped stick key unhandled");
}

static void NormalFirstHandledPolicy() {
    // Different mapping IDs with the same key distinguish first-handled from broadcast.
    ControllerButton button(0, 1);
    auto a = std::make_shared<KeyboardKeyToButtonMapping>(0, 1, LUS_KB_A);
    auto b = std::make_shared<KeyboardKeyToButtonMapping>(0, 2, LUS_KB_A);
    button.AddButtonMapping(a);
    button.AddButtonMapping(b);
    auto pressed = [](std::shared_ptr<KeyboardKeyToButtonMapping> mapping) {
        CONTROLLERBUTTONS_T pad = 0;
        mapping->UpdatePad(pad);
        return pad != 0;
    };
    Check(button.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_A), "shared button key-down handled");
    Check(pressed(a) != pressed(b), "normal button key-down affects only first handling mapping");
    a->ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_A);
    b->ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_A);
    Check(button.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_UP, LUS_KB_A), "shared button key-up handled");
    Check(pressed(a) != pressed(b), "normal button key-up retains first-handled policy");
    Check(button.ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN), "shared-button focus loss handled");
    Check(!pressed(a) && !pressed(b), "all-keys-up clears shared-key button mappings");

    // Identical keys on separate directions must retain normal first-handled semantics.
    ControllerStick stick(0, LEFT_STICK);
    auto up = std::make_shared<KeyboardKeyToAxisDirectionMapping>(0, LEFT_STICK, UP, LUS_KB_W);
    auto right = std::make_shared<KeyboardKeyToAxisDirectionMapping>(0, LEFT_STICK, RIGHT, LUS_KB_W);
    stick.AddAxisDirectionMapping(UP, up);
    stick.AddAxisDirectionMapping(RIGHT, right);
    Check(stick.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_W), "shared key-down handled");
    Check((up->GetNormalizedAxisDirectionValue() > 0) != (right->GetNormalizedAxisDirectionValue() > 0),
          "normal key-down affects only first handling direction");
    // Seed both via their real mapping API to distinguish first-only key-up from broadcast.
    up->ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_W);
    right->ProcessKeyboardEvent(LUS_KB_EVENT_KEY_DOWN, LUS_KB_W);
    Check(stick.ProcessKeyboardEvent(LUS_KB_EVENT_KEY_UP, LUS_KB_W), "shared key-up handled");
    Check((up->GetNormalizedAxisDirectionValue() > 0) != (right->GetNormalizedAxisDirectionValue() > 0),
          "normal key-up retains first-handled policy");
    Check(stick.ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN), "shared-key focus loss handled");
    Check(up->GetNormalizedAxisDirectionValue() == 0 && right->GetNormalizedAxisDirectionValue() == 0,
          "all-keys-up clears shared-key directions");
}

int main() {
    ControllerButton emptyButton(0, 1);
    ControllerStick emptyStick(0, LEFT_STICK);
    Check(!emptyButton.ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN), "empty button unhandled");
    Check(!emptyStick.ProcessKeyboardEvent(LUS_KB_EVENT_ALL_KEYS_UP, LUS_KB_UNKNOWN), "empty stick unhandled");
    ButtonFocusLoss();
    StickFocusLoss();
    NormalFirstHandledPolicy();
    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: button/stick focus-loss release, empty/unmapped events, regain, normal first-handled policy\n";
}
