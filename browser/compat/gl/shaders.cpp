// vs.1.1 and ps.1.1 in GLSL ES 3.00, from the assembly text D3DXAssembleShader keeps in its 'ASMT' comment, as far as
// the client's shaders go (ZoneRenderer.cpp); anything else fails with a message. Instruction semantics follow
// Microsoft's D3D8 shader reference, and Wine's wined3d where the reference is vague (scalar instructions read the
// source's w after swizzling).
#include "gl.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace gl
{

namespace
{

auto SourceText(const DWORD* function) noexcept -> const char*
{
    if (function == nullptr || (function[1] & D3DSI_OPCODE_MASK) != D3DSIO_COMMENT || function[2] != MAKEFOURCC('A', 'S', 'M', 'T'))
    {
        return nullptr;
    }

    return reinterpret_cast<const char*>(function + 3);
}

// The fixed-function meaning of a declaration register (D3DVSDE_*), or -1.
auto FixedLocation(DWORD reg) noexcept -> int
{
    constexpr auto kLocations = std::array<int, 7>{ kPosition, kBlendWeights, -1, kNormal, kPointSize, kDiffuse, kSpecular };
    if (reg < 7)
    {
        return kLocations[reg];
    }

    if (reg < 15)
    {
        return kTexCoord0 + (reg - 7);
    }

    return -1;
}

// Elements with their vN register. A register without a fixed-function meaning gets a location only when a shader
// function reads it.
auto ParseDeclaration(const DWORD* tokens, bool programmable) -> std::vector<std::pair<DWORD, Element>>
{
    auto elements = std::vector<std::pair<DWORD, Element>>{};
    auto unplaced = std::vector<DWORD>{};
    auto stream   = uint32_t{ 0 };
    auto offset   = uint32_t{ 0 };
    for (; tokens != nullptr && *tokens != D3DVSD_END(); ++tokens)
    {
        const auto token = *tokens;
        switch ((token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT)
        {
            case D3DVSD_TOKEN_STREAM:
                stream = token & D3DVSD_STREAMNUMBERMASK;
                offset = 0;
                break;
            case D3DVSD_TOKEN_STREAMDATA:
            {
                if (token & D3DVSD_DATALOADTYPEMASK)
                {
                    offset += 4 * ((token & D3DVSD_SKIPCOUNTMASK) >> D3DVSD_SKIPCOUNTSHIFT);
                    break;
                }

                const auto reg  = token & D3DVSD_VERTEXREGMASK;
                const auto type = (token & D3DVSD_DATATYPEMASK) >> D3DVSD_DATATYPESHIFT;
                const auto size = [&]() -> uint32_t
                {
                    if (type <= D3DVSDT_FLOAT4)
                    {
                        return 4 * (type + 1);
                    }

                    if (type == D3DVSDT_SHORT4)
                    {
                        return 8;
                    }

                    return 4;
                }();

                // Element has no integer formats; the client declares only floats and D3DCOLOR.
                if (type <= D3DVSDT_D3DCOLOR)
                {
                    const auto components = [&]() -> uint32_t
                    {
                        if (type == D3DVSDT_D3DCOLOR)
                        {
                            return 4;
                        }

                        return type + 1;
                    }();

                    const auto location = FixedLocation(reg);
                    const auto element  = Element{ static_cast<Attribute>(location), stream, offset, components, type == D3DVSDT_D3DCOLOR };
                    if (location >= 0)
                    {
                        elements.push_back({ reg, element });
                    }
                    else if (programmable)
                    {
                        unplaced.push_back(elements.size());
                        elements.push_back({ reg, element });
                    }
                }
                else
                {
                    std::fprintf(stderr, "shaders: vertex data type %lu has no GL binding\n", type);
                }

                offset += size;
                break;
            }
            case D3DVSD_TOKEN_CONSTMEM:
                // The device loads these constants when the shader is set.
                tokens += 4 * ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT);
                break;
            case D3DVSD_TOKEN_EXT:
                tokens += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
                break;
            default:
                break;
        }
    }

    for (const auto index : unplaced)
    {
        auto taken = std::set<uint32_t>{};
        for (const auto& [reg, element] : elements)
        {
            if (static_cast<int>(element.attribute) >= 0)
            {
                taken.insert(element.attribute);
            }
        }

        auto location = uint32_t{ 0 };
        while (taken.count(location))
        {
            ++location;
        }

        elements[index].second.attribute = static_cast<Attribute>(location);
    }

    elements.erase(std::remove_if(elements.begin(), elements.end(), [](const auto& entry)
                                  {
                                      return entry.second.attribute >= kAttributes;
                                  }),
                   elements.end());
    return elements;
}

struct Operand
{
    char        file = 0; // r v c t, or o for outputs
    std::string output;   // pos d0 d1 t0-t7 fog
    int         index   = 0;
    std::string swizzle = "xyzw"; // a write mask on a destination
    std::string modifier;         // bx2
    bool        negate = false;
};

auto Components(const std::string& text, bool mask) -> std::string
{
    auto out = std::string{};
    for (const auto c : text)
    {
        const auto at = std::string("xyzwrgba").find(c);
        if (at == std::string::npos)
        {
            return {};
        }

        out += "xyzw"[at % 4];
    }

    // A short source swizzle repeats its last component.
    while (!mask && !out.empty() && out.size() < 4)
    {
        out += out.back();
    }

    return out;
}

auto StripModifier(std::string& text) -> std::string
{
    const auto underscore = text.find('_');
    if (underscore == std::string::npos)
    {
        return {};
    }

    auto modifier = text.substr(underscore + 1);
    text.resize(underscore);
    return modifier;
}

auto ParseOperand(std::string text, bool destination, Operand& operand) -> bool
{
    if (!text.empty() && text[0] == '-')
    {
        operand.negate = true;
        text.erase(0, 1);
    }

    const auto dot     = text.find('.');
    auto       name    = text.substr(0, dot);
    auto       swizzle = [&]() -> std::string
    {
        if (dot == std::string::npos)
        {
            return {};
        }

        return text.substr(dot + 1);
    }();

    operand.modifier = StripModifier(name);
    if (operand.modifier.empty())
    {
        operand.modifier = StripModifier(swizzle);
    }

    if (!swizzle.empty())
    {
        operand.swizzle = Components(swizzle, destination);
        if (operand.swizzle.empty())
        {
            return false;
        }
    }

    if (name.empty())
    {
        return false;
    }

    if (name[0] == 'o' && name != "o")
    {
        operand.file   = 'o';
        operand.output = name.substr(1);
        if (operand.output.size() == 2 && operand.output[0] == 't')
        {
            operand.index = operand.output[1] - '0';
        }

        static const auto kOutputs = std::set<std::string>{ "pos", "d0", "d1", "fog", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7" };
        return kOutputs.count(operand.output) != 0;
    }

    operand.file = name[0];
    if (name.size() > 1)
    {
        operand.index = std::atoi(name.c_str() + 1);
    }

    return std::string("rvct").find(operand.file) != std::string::npos;
}

constexpr auto kVertexHelpers = R"(float d3d_rsq(float x)
{
    x = abs(x);
    return x == 1.0 ? 1.0 : x == 0.0 ? uintBitsToFloat(0x7f800000u) : inversesqrt(x);
}
)";

class Translator
{
public:
    explicit Translator(bool pixel) noexcept
    : pixel_(pixel)
    {
    }

    auto Line(std::string line) -> bool
    {
        for (const auto* const comment : { ";", "//" })
        {
            const auto at = line.find(comment);
            if (at != std::string::npos)
            {
                line.resize(at);
            }
        }

        for (auto& c : line)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos)
        {
            return true;
        }

        line.erase(0, start);
        const auto space  = line.find_first_of(" \t");
        const auto opcode = line.substr(0, space);
        if (!versionSeen_ && (opcode.compare(0, 2, "vs") == 0 || opcode.compare(0, 2, "ps") == 0))
        {
            versionSeen_ = true;
            return true;
        }

        auto operands = std::vector<std::string>{};
        if (space != std::string::npos)
        {
            auto rest = line.substr(space);
            rest.erase(std::remove_if(rest.begin(), rest.end(), [](unsigned char c)
                                      {
                                          return std::isspace(c);
                                      }),
                       rest.end());
            auto from = size_t{ 0 };
            while (from <= rest.size() && !rest.empty())
            {
                const auto comma = rest.find(',', from);
                operands.push_back(rest.substr(from, comma - from));
                if (comma == std::string::npos)
                {
                    break;
                }

                from = comma + 1;
            }
        }

        return Instruction(opcode, operands);
    }

    auto Finish(const std::vector<std::pair<DWORD, Element>>& inputs) const -> std::string
    {
        auto out = std::string("#version 300 es\nprecision highp float;\nprecision highp int;\n");
        for (const auto* const varying : { "vec4 v_diffuse;", "vec4 v_specular;", "vec4 v_tex[8];", "float v_fog;" })
        {
            if (pixel_)
            {
                out += std::string("in ") + varying + "\n";
            }
            else
            {
                out += std::string("out ") + varying + "\n";
            }
        }

        if (pixel_)
        {
            out += "uniform vec4 pc[8];\n";
            for (const auto sampler : samplers_)
            {
                out += "uniform sampler2D s" + std::to_string(sampler) + ";\n";
            }

            out += "vec4 PixelShader()\n"
                   "{\n"
                   "    vec4 v0 = v_diffuse;\n"
                   "    vec4 v1 = v_specular;\n";
            for (int i = 0; i < 6; ++i)
            {
                out += "    vec4 r" + std::to_string(i) + " = vec4(0.0);\n";
                if (i < 4)
                {
                    out += "    vec4 t" + std::to_string(i) + " = vec4(0.0);\n";
                }
            }
        }
        else
        {
            out += "uniform vec4 c[96];\n";
            auto reads    = std::string{};
            auto declared = std::set<DWORD>{};
            for (const auto& [reg, element] : inputs)
            {
                const auto name = "v" + std::to_string(reg);
                out += "layout(location = " + std::to_string(element.attribute) + ") in vec4 a_" + name + ";\n";
                if (element.color)
                {
                    reads += "    vec4 " + name + " = a_" + name + ".zyxw;\n";
                }
                else
                {
                    reads += "    vec4 " + name + " = a_" + name + ";\n";
                }

                declared.insert(reg);
            }

            for (DWORD reg = 0; reg < 16; ++reg)
            {
                if (!declared.count(reg))
                {
                    reads += "    vec4 v" + std::to_string(reg) + " = vec4(0.0, 0.0, 0.0, 1.0);\n";
                }
            }

            out += kVertexHelpers;
            out += "vec4 VertexShader()\n{\n" + reads;
            for (int i = 0; i < 12; ++i)
            {
                out += "    vec4 r" + std::to_string(i) + " = vec4(0.0);\n";
            }

            out += "    vec4 oPos = vec4(0.0);\n";
        }

        for (const auto& line : body_)
        {
            out += "    " + line + "\n";
        }

        if (pixel_)
        {
            out += "    return r0;\n}\n";
        }
        else
        {
            out += "    return oPos;\n}\n";
        }

        return out;
    }

private:
    auto Register(const Operand& operand, int row) const -> std::string
    {
        const auto n = std::to_string(operand.index + row);
        if (operand.file != 'c')
        {
            return std::string(1, operand.file) + n;
        }

        if (pixel_)
        {
            return "pc[" + n + "]";
        }

        return "c[" + n + "]";
    }

    auto Source(const Operand& operand, int row = 0) const -> std::string
    {
        auto value = Register(operand, row);
        if (operand.swizzle != "xyzw")
        {
            value += "." + operand.swizzle;
        }

        if (operand.modifier == "bx2")
        {
            value = "(2.0 * " + value + " - 1.0)";
        }

        if (operand.negate)
        {
            value = "(-" + value + ")";
        }

        return value;
    }

    auto Assign(const Operand& dest, const std::string& value) const -> std::string
    {
        const auto mask = [&]() -> std::string
        {
            if (dest.swizzle == "xyzw")
            {
                return {};
            }

            return "." + dest.swizzle;
        }();

        if (dest.file != 'o')
        {
            return Register(dest, 0) + mask + " = " + value + mask + ";";
        }

        const auto& out = dest.output;
        if (out == "pos")
        {
            return "oPos" + mask + " = " + value + mask + ";";
        }

        if (out == "d0")
        {
            return "v_diffuse" + mask + " = clamp(" + value + ", 0.0, 1.0)" + mask + ";";
        }

        if (out == "d1")
        {
            return "v_specular" + mask + " = clamp(" + value + ", 0.0, 1.0)" + mask + ";";
        }

        if (out == "fog")
        {
            return "v_fog = clamp(" + value + ".x, 0.0, 1.0);";
        }

        return "v_tex[" + std::to_string(dest.index) + "]" + mask + " = " + value + mask + ";";
    }

    // m4x4, the only matrix instruction in the client's shaders (Rendering/ZoneRenderer.cpp)
    auto Matrix(const std::vector<Operand>& src) const -> std::string
    {
        auto value = std::string("vec4(");
        for (int row = 0; row < 4; ++row)
        {
            if (row > 0)
            {
                value += ", ";
            }

            value += "dot(" + Source(src[0]) + ", " + Source(src[1], row) + ")";
        }

        return value + ")";
    }

    auto Instruction(const std::string& text, const std::vector<std::string>& operands) -> bool
    {
        auto opcode    = text;
        auto modifiers = std::vector<std::string>{};
        for (auto modifier = StripModifier(opcode); !modifier.empty();)
        {
            auto rest = modifier;
            auto next = StripModifier(rest);
            modifiers.push_back(rest);
            modifier = next;
        }

        if (operands.empty())
        {
            return false;
        }

        auto dest = Operand{};
        auto src  = std::vector<Operand>(operands.size() - 1);
        if (!ParseOperand(operands[0], true, dest))
        {
            return false;
        }

        for (size_t i = 1; i < operands.size(); ++i)
        {
            if (!ParseOperand(operands[i], false, src[i - 1]))
            {
                return false;
            }
        }

        auto s = std::vector<std::string>{};
        for (const auto& operand : src)
        {
            s.push_back(Source(operand));
        }

        const auto need = [&](size_t count)
        {
            return s.size() == count;
        };

        auto value = std::string{};
        if (opcode == "mov")
        {
            if (need(1))
            {
                value = s[0];
            }
        }
        else if (opcode == "add" && need(2))
        {
            value = s[0] + " + " + s[1];
        }
        else if (opcode == "sub" && need(2))
        {
            value = s[0] + " - " + s[1];
        }
        else if (opcode == "mul" && need(2))
        {
            value = s[0] + " * " + s[1];
        }
        else if (opcode == "mad" && need(3))
        {
            value = s[0] + " * " + s[1] + " + " + s[2];
        }
        else if (opcode == "dp3" && need(2))
        {
            value = "vec4(dot(" + s[0] + ".xyz, " + s[1] + ".xyz))";
        }
        else if (opcode == "dp4" && need(2))
        {
            value = "vec4(dot(" + s[0] + ", " + s[1] + "))";
        }
        else if ((opcode == "min" || opcode == "max") && need(2))
        {
            value = opcode + "(" + s[0] + ", " + s[1] + ")";
        }
        else if (opcode == "sge" && need(2))
        {
            value = "vec4(greaterThanEqual(" + s[0] + ", " + s[1] + "))";
        }
        else if (opcode == "rsq" && need(1))
        {
            value = "vec4(d3d_rsq(" + s[0] + ".w))";
        }
        else if (opcode == "m4x4" && need(2))
        {
            value = Matrix(src);
        }
        else if (opcode == "tex" && need(0))
        {
            samplers_.insert(dest.index);
            value = "texture(s" + std::to_string(dest.index) + ", v_tex[" + std::to_string(dest.index) + "].xy)";
        }
        else if (opcode == "texcoord" && need(0))
        {
            value = "clamp(vec4(v_tex[" + std::to_string(dest.index) + "].xyz, 1.0), 0.0, 1.0)";
        }

        if (value.empty())
        {
            std::fprintf(stderr, "shaders: unsupported instruction %s\n", text.c_str());
            return false;
        }

        for (const auto& modifier : modifiers)
        {
            if (modifier == "x2" || modifier == "x4")
            {
                value = "(" + value + ") * " + modifier[1] + ".0";
            }
        }

        for (const auto& modifier : modifiers)
        {
            if (modifier == "sat")
            {
                value = "clamp(" + value + ", 0.0, 1.0)";
            }
        }

        const auto temporary = "i" + std::to_string(temporaries_++);
        body_.push_back("vec4 " + temporary + " = " + value + ";");
        body_.push_back(Assign(dest, temporary));
        return true;
    }

    bool                     pixel_;
    bool                     versionSeen_ = false;
    int                      temporaries_ = 0;
    std::set<int>            samplers_;
    std::vector<std::string> body_;
};

auto Translate(const DWORD* function, bool pixel, const std::vector<std::pair<DWORD, Element>>& inputs) -> std::string
{
    const auto* const text = SourceText(function);
    if (text == nullptr)
    {
        std::fprintf(stderr, "shaders: no assembly text in the shader tokens\n");
        return {};
    }

    auto translator = Translator(pixel);
    auto number     = 1;
    for (const auto* line = text; *line != '\0'; ++number)
    {
        const auto* end = line;
        while (*end != '\0' && *end != '\n')
        {
            ++end;
        }

        if (!translator.Line(std::string(line, end)))
        {
            std::fprintf(stderr, "shaders: line %d: %s\n", number, std::string(line, end).c_str());
            return {};
        }

        if (*end == '\0')
        {
            line = end;
        }
        else
        {
            line = end + 1;
        }
    }

    return translator.Finish(inputs);
}

} // namespace

auto TranslateVertexShader(const DWORD* declaration, const DWORD* function) -> VertexProgram
{
    auto       program = VertexProgram{};
    const auto inputs  = ParseDeclaration(declaration, function != nullptr);
    for (const auto& [reg, element] : inputs)
    {
        program.layout.push_back(element);
    }

    if (function != nullptr)
    {
        program.glsl = Translate(function, false, inputs);
    }

    return program;
}

auto TranslatePixelShader(const DWORD* function) -> std::string
{
    return Translate(function, true, {});
}

} // namespace gl
