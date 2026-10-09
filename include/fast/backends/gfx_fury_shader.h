#pragma once
#include <algorithm>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace Fast::detail {
struct HlslVertexParameter {
    uint32_t components;
    std::string name;
    std::string semantic;
};

// This is the Fast3D template ABI, not arbitrary HLSL reflection. VSMain must
// return PSInput and take the interpreter's exact float parameters, names and
// semantics in order. Resources must be ordinary, non-array declarations with
// the canonical names/types below; Texture2D payloads must be float scalar/vector
// types matching the runtime's RGBA8_UNORM textures. Aliases/macros and extra
// descriptors are not supported. Existing numeric Vulkan annotations/registers must match the ABI.
// Constant-buffer members must use the exact template declarations/order: the
// runtime uploads 16/32/16 bytes, without reflecting custom member offsets.
// Pixel computations may change freely. Comments and whitespace are immaterial.
// This validates the generated ABI, not a sandbox: Prism templates/expressions
// are trusted mod code, like OpenGL overrides. Archive/include byte budgets do
// not bound template loop expansion, expression evaluation or compiler work.
inline void PrepareFuryHlsl(std::string& source, const std::string& sourcePath,
                            const std::vector<HlslVertexParameter>& expected) {
    auto fail = [&](const std::string& error) {
        throw std::runtime_error("Prism HLSL shader '" + sourcePath + "': " + error +
                                 "; follow the Fast3D HLSL template ABI (or select OpenGL for GLSL mods)");
    };
    // HLSL splices physical lines BEFORE recognizing comments. Retain a mapping
    // to original offsets so split tokens/comment delimiters are validated as
    // the compiler sees them, while annotations still edit the original source.
    std::string code;
    std::vector<size_t> originalOffsets;
    for (size_t pos = 0; pos < source.size(); ++pos) {
        if (source[pos] == '\\' && pos + 1 < source.size()) {
            if (source[pos + 1] == '\n') {
                ++pos;
                continue;
            }
            if (source[pos + 1] == '\r' && pos + 2 < source.size() && source[pos + 2] == '\n') {
                pos += 2;
                continue;
            }
        }
        code.push_back(source[pos]);
        originalOffsets.push_back(pos);
    }
    const std::string spliced = code;
    std::regex ignored(R"(/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*")");
    for (std::sregex_iterator i(spliced.begin(), spliced.end(), ignored), end; i != end; ++i)
        std::fill(code.begin() + i->position(), code.begin() + i->position() + i->length(), ' ');
    // Prism @include has already expanded archive sources. Subsequent HLSL
    // preprocessing must not change the checked ABI declarations.
    const std::string& directives = code;
    std::regex includeDirective(R"(^\s*#\s*include\b)", std::regex::multiline);
    if (std::regex_search(directives, includeDirective))
        fail("HLSL #include is unsupported; use Prism @include so declarations are validated");
    std::vector<std::string> reserved = {
        "uint",
        "uint2",
        "uint3",
        "uint4",
        "float",
        "float1",
        "float2",
        "float3",
        "float4",
        "bool",
        "struct",
        "cbuffer",
        "Texture2D",
        "SamplerState",
        "register",
        "vk",
        "binding",
        "location",
        "PSInput",
        "VSMain",
        "PSMain",
        "PerFrameCB",
        "PerDrawCB",
        "PerPrimDepthCB",
        "noise_frame",
        "noise_scale",
        "width",
        "height",
        "linear_filtering",
        "textures",
        "prim_depth",
        "_pad",
        "g_texture0",
        "g_texture1",
        "g_textureMask0",
        "g_textureMask1",
        "g_textureBlend0",
        "g_textureBlend1",
        "g_sampler0",
        "g_sampler1",
    };
    for (const auto& parameter : expected) {
        reserved.push_back(parameter.name);
        reserved.push_back(parameter.semantic);
    }
    std::regex defineDirective(R"(^\s*#\s*define\s+([A-Za-z_]\w*))", std::regex::multiline);
    for (std::sregex_iterator i(directives.begin(), directives.end(), defineDirective), end; i != end; ++i)
        if (std::find(reserved.begin(), reserved.end(), (*i)[1].str()) != reserved.end())
            fail("macro redefines canonical ABI identifier '" + (*i)[1].str() + "'");
    struct Edit {
        size_t pos;
        std::string text;
    };
    std::vector<Edit> edits;
    auto annotation = [&](const std::string& attributes, const std::string& kind, uint32_t value) {
        if (attributes.find_first_not_of(" \t\r\n") == std::string::npos)
            return false;
        std::regex allowed("\\s*\\[\\[\\s*vk\\s*::\\s*" + kind + "\\s*\\(\\s*" + std::to_string(value) +
                           (kind == "binding" ? "\\s*,\\s*0" : "") + "\\s*\\)\\s*\\]\\]\\s*");
        if (!std::regex_match(attributes, allowed))
            fail("conflicting or unsupported Vulkan " + kind + " annotation; expected " + kind + "(" +
                 std::to_string(value) + (kind == "binding" ? ",0)" : ")"));
        return true;
    };

    std::regex vertex(R"(\bPSInput\s+VSMain\s*\()");
    std::smatch match;
    if (!std::regex_search(code, match, vertex))
        fail("missing PSInput VSMain vertex entry point");
    size_t begin = match.position() + match.length();
    size_t end = begin;
    int depth = 1;
    for (; end < code.size(); ++end) {
        if (code[end] == '(')
            ++depth;
        if (code[end] == ')' && --depth == 0)
            break;
    }
    if (end == code.size() || code.find_first_not_of(" \t\r\n", end + 1) == std::string::npos ||
        code[code.find_first_not_of(" \t\r\n", end + 1)] != '{')
        fail("VSMain must be a function definition with the canonical signature");
    if (std::regex_search(code.cbegin() + end + 1, code.cend(), vertex))
        fail("multiple VSMain definitions are unsupported");
    std::regex parameter(R"(\s*((?:\[\[[^\]]*\]\]\s*)*)(float([234]?))\s+(\w+)\s*:\s*(\w+)\s*)");
    size_t parameterStart = begin;
    size_t index = 0;
    depth = 0;
    for (size_t pos = begin; pos <= end; ++pos) {
        if (pos < end && code[pos] == '(')
            ++depth;
        if (pos < end && code[pos] == ')')
            --depth;
        if (pos != end && (code[pos] != ',' || depth != 0))
            continue;
        std::string text = code.substr(parameterStart, pos - parameterStart);
        std::smatch p;
        if (index >= expected.size() || !std::regex_match(text, p, parameter))
            fail("unsupported VSMain parameter at location " + std::to_string(index));
        const auto& want = expected[index];
        uint32_t components = p[3].str().empty() ? 1 : uint32_t(p[3].str()[0] - '0');
        if (components != want.components || p[4] != want.name || p[5] != want.semantic)
            fail("vertex layout at location " + std::to_string(index) + " must be float" +
                 (want.components == 1 ? "" : std::to_string(want.components)) + " " + want.name + " : " +
                 want.semantic);
        if (!annotation(p[1], "location", uint32_t(index)))
            edits.push_back(
                { parameterStart + size_t(p.position(2)), "[[vk::location(" + std::to_string(index) + ")]] " });
        ++index;
        parameterStart = pos + 1;
    }
    if (index != expected.size())
        fail("vertex layout parameter count does not match interpreter");

    struct Resource {
        const char* name;
        const char* type;
        uint32_t binding;
        char reg;
        uint32_t slot;
    };
    const Resource resources[] = {
        { "g_texture0", "Texture2D", 0, 't', 0 },      { "g_texture1", "Texture2D", 1, 't', 1 },
        { "g_textureMask0", "Texture2D", 2, 't', 2 },  { "g_textureMask1", "Texture2D", 3, 't', 3 },
        { "g_textureBlend0", "Texture2D", 4, 't', 4 }, { "g_textureBlend1", "Texture2D", 5, 't', 5 },
        { "g_sampler0", "SamplerState", 6, 's', 0 },   { "g_sampler1", "SamplerState", 7, 's', 1 },
        { "PerFrameCB", "cbuffer", 8, 'b', 0 },        { "PerDrawCB", "cbuffer", 9, 'b', 1 },
        { "PerPrimDepthCB", "cbuffer", 10, 'b', 2 },
    };
    bool seen[11] = {};
    // Helper functions may pass textures/samplers as ordinary parameters; these
    // are not global descriptor declarations and must not be assigned bindings.
    std::vector<int> parentheses(code.size());
    depth = 0;
    for (size_t pos = 0; pos < code.size(); ++pos) {
        parentheses[pos] = depth;
        if (code[pos] == '(')
            ++depth;
        if (code[pos] == ')')
            --depth;
    }
    std::string unchecked = code;
    const std::string resourceType =
        R"((?:RW|RasterizerOrdered)?Texture\w*|(?:RW|RasterizerOrdered)?(?:StructuredBuffer|ByteAddressBuffer|Buffer)|AppendStructuredBuffer|ConsumeStructuredBuffer|ConstantBuffer|Sampler\w*|FeedbackTexture\w*|RaytracingAccelerationStructure|SubpassInput\w*|[ct]buffer)";
    std::regex declaration("((?:\\[\\[[^\\]]*\\]\\]\\s*)*)\\b(" + resourceType +
                           ")\\b(\\s*<[^;{}]*>)?\\s+(\\w+)([^;{}]*)([;{])");
    for (std::sregex_iterator i(code.begin(), code.end(), declaration), finish; i != finish; ++i) {
        const auto& d = *i;
        if (parentheses[size_t(d.position(2))] != 0)
            continue;
        auto resource =
            std::find_if(std::begin(resources), std::end(resources), [&](const auto& r) { return d[4] == r.name; });
        if (resource == std::end(resources) || d[2] != resource->type)
            fail("unsupported descriptor resource '" + d[4].str() + "' of type " + d[2].str() +
                 "; only canonical Fast3D Texture2D, SamplerState and cbuffer resources are supported");
        if (seen[resource->binding])
            fail("duplicate descriptor resource '" + d[4].str() + "'");
        seen[resource->binding] = true;
        bool buffer = d[2] == "cbuffer";
        if ((buffer ? d[6] != "{" : d[6] != ";") || (!d[3].str().empty() && d[2] != "Texture2D"))
            fail("unsupported descriptor declaration for '" + d[4].str() + "'");
        if (d[2] == "Texture2D" && !d[3].str().empty() &&
            !std::regex_match(d[3].str(), std::regex(R"(\s*<\s*float[1-4]?\s*>\s*)")))
            fail("unsupported Texture2D element type for '" + d[4].str() +
                 "'; RGBA8_UNORM textures require float scalar/vector elements");
        if (buffer) {
            // Include nested anonymous struct braces when extracting the body.
            // Matching the whole masked body excludes packoffset, aliases,
            // qualifiers, extra/missing members and noncanonical arrays.
            size_t bodyBegin = size_t(d.position(6)) + 1;
            size_t bodyEnd = bodyBegin;
            int braces = 1;
            for (; bodyEnd < code.size(); ++bodyEnd) {
                if (code[bodyEnd] == '{')
                    ++braces;
                if (code[bodyEnd] == '}' && --braces == 0)
                    break;
            }
            const char* layout =
                resource->binding == 8 ? R"(\s*uint\s+noise_frame\s*;\s*float\s+noise_scale\s*;\s*)"
                : resource->binding == 9
                    ? R"(\s*struct\s*\{\s*uint\s+width\s*;\s*uint\s+height\s*;\s*bool\s+linear_filtering\s*;\s*\}\s*textures\s*\[\s*2\s*\]\s*;\s*)"
                    : R"(\s*float\s+prim_depth\s*;\s*float3\s+_pad\s*;\s*)";
            if (bodyEnd == code.size() ||
                !std::regex_match(code.substr(bodyBegin, bodyEnd - bodyBegin), std::regex(layout)))
                fail("constant-buffer member layout for '" + d[4].str() +
                     "' must match the canonical template exactly");
        }
        std::string tail = d[5];
        if (tail.find_first_not_of(" \t\r\n") != std::string::npos) {
            std::regex reg("\\s*:\\s*register\\s*\\(\\s*" + std::string(1, resource->reg) + "\\s*" +
                           std::to_string(resource->slot) + "\\s*(?:,\\s*space0\\s*)?\\)\\s*");
            if (!std::regex_match(tail, reg))
                fail("conflicting register or unsupported array/declaration for '" + d[4].str() + "'");
        }
        if (!annotation(d[1], "binding", resource->binding))
            edits.push_back({ size_t(d.position(2)), "[[vk::binding(" + std::to_string(resource->binding) + ",0)]] " });
        std::fill(unchecked.begin() + d.position(), unchecked.begin() + d.position() + d.length(), ' ');
    }
    // Resource aliases or declaration forms not recognized above must not acquire
    // accidental Slang-assigned bindings. Reject leftover descriptor type tokens
    // outside helper-function parameters, even without an explicit register.
    std::regex descriptorToken("\\b(?:" + resourceType + ")\\b");
    for (std::sregex_iterator i(unchecked.begin(), unchecked.end(), descriptorToken), finish; i != finish; ++i)
        if (parentheses[size_t(i->position())] == 0)
            fail("unsupported descriptor declaration or resource alias '" + i->str() + "'");
    std::regex unhandled(R"(\bregister\s*\(|\bvk\s*::\s*binding\s*\()");
    if (std::regex_search(unchecked, unhandled))
        fail("unsupported descriptor declaration or resource alias with binding/register");
    std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.pos > b.pos; });
    for (const auto& edit : edits)
        source.insert(originalOffsets.at(edit.pos), edit.text);
}
} // namespace Fast::detail
