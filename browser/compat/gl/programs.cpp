// Programs for draws: a vertex stage (fixed function or a translated vs.1.1) and a fragment stage (fixed function or a
// translated ps.1.x), each completed with the main() gl.h describes, linked once per combination.
#include "internal.h"

#include <cstdio>
#include <tuple>

namespace gl
{

namespace
{

// D3D8 samples at pixel centres on integer coordinates, GL half a pixel further. Targets are stored top row first
// (objects.cpp), so y flips; and D3D's 0..w depth becomes GL's -w..w.
constexpr auto kVertexMain = R"(
uniform vec2 kr_pixel;
void main()
{
    v_diffuse = vec4(1.0);
    v_specular = vec4(0.0);
    for (int i = 0; i < 8; ++i)
        v_tex[i] = vec4(0.0, 0.0, 0.0, 1.0);
    v_fog = 1.0;
    gl_PointSize = 1.0;
    vec4 p = VertexShader();
    p.xy += kr_pixel * p.w;
    gl_Position = vec4(p.x, -p.y, 2.0 * p.z - p.w, p.w);
}
)";

// D3D8 applies fog and the alpha test after the pixel shader as well. Alpha compares as the 8-bit value it is.
constexpr auto kFragmentMain = R"(
uniform int kr_alphaFunc;
uniform float kr_alphaRef;
uniform bool kr_fog;
uniform vec3 kr_fogColor;
out vec4 kr_color;
bool kr_passes(float a)
{
    float r = kr_alphaRef;
    switch (kr_alphaFunc) {
    case 1: return false;
    case 2: return a < r;
    case 3: return a == r;
    case 4: return a <= r;
    case 5: return a > r;
    case 6: return a != r;
    case 7: return a >= r;
    default: return true;
    }
}
void main()
{
    vec4 c = PixelShader();
    if (!kr_passes(floor(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5)))
        discard;
    if (kr_fog)
        c.rgb = mix(kr_fogColor, c.rgb, clamp(v_fog, 0.0, 1.0));
    kr_color = c;
}
)";

struct Program
{
    GLuint  id = 0;
    Uniform pixel;
    Uniform alphaFunc;
    Uniform alphaRef;
    Uniform fog;
    Uniform fogColor;
    Uniform c;
    Uniform pc;
};

struct VertexShaderEntry
{
    Layout      layout;
    std::string glsl;
    bool        failed = false; // a function the translator could not turn into GLSL
};

auto Compile(GLenum type, const std::string& source) -> GLuint
{
    const auto  shader = glCreateShader(type);
    const auto* text   = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    auto ok = GLint{ 0 };
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        const auto* const stage = [&]()
        {
            if (type == GL_VERTEX_SHADER)
            {
                return "vertex";
            }

            return "fragment";
        }();

        auto log = std::array<char, 2048>{};
        glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
        std::fprintf(stderr, "gl: %s shader failed: %s\n%s\n", stage, log.data(), text);
    }

    return shader;
}

auto Link(const std::string& vertex, const std::string& fragment) -> Program
{
    auto       program = Program{};
    const auto vs      = Compile(GL_VERTEX_SHADER, vertex + kVertexMain);
    const auto fs      = Compile(GL_FRAGMENT_SHADER, fragment + kFragmentMain);
    const auto id      = glCreateProgram();
    glAttachShader(id, vs);
    glAttachShader(id, fs);
    glLinkProgram(id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    auto ok = GLint{ 0 };
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        auto log = std::array<char, 2048>{};
        glGetProgramInfoLog(id, log.size(), nullptr, log.data());
        std::fprintf(stderr, "gl: program failed to link: %s\n", log.data());
        glDeleteProgram(id);
        return program;
    }

    program.id = id;
    glUseProgram(id);
    for (int stage = 0; stage < 8; ++stage)
    {
        const auto name     = std::array<char, 4>{ 's', static_cast<char>('0' + stage), 0 };
        const auto location = glGetUniformLocation(id, name.data());
        if (location >= 0)
        {
            glUniform1i(location, stage);
        }
    }

    program.pixel.location     = glGetUniformLocation(id, "kr_pixel");
    program.alphaFunc.location = glGetUniformLocation(id, "kr_alphaFunc");
    program.alphaRef.location  = glGetUniformLocation(id, "kr_alphaRef");
    program.fog.location       = glGetUniformLocation(id, "kr_fog");
    program.fogColor.location  = glGetUniformLocation(id, "kr_fogColor");
    program.c.location         = glGetUniformLocation(id, "c");
    program.pc.location        = glGetUniformLocation(id, "pc");
    return program;
}

auto VertexShaderFor(const NullDevice& device, DWORD handle) -> const VertexShaderEntry&
{
    static auto shaders = std::map<DWORD, VertexShaderEntry>{};
    if (const auto found = shaders.find(handle); found != shaders.end())
    {
        return found->second;
    }

    auto& entry = shaders[handle];
    if (const auto* const shader = device.VertexShader(handle))
    {
        const auto* const function = [&]() -> const DWORD*
        {
            if (shader->function.empty())
            {
                return nullptr;
            }

            return shader->function.data();
        }();

        auto program     = TranslateVertexShader(shader->declaration.data(), function);
        entry.layout     = { std::move(program.layout), 0 };
        entry.layout.fvf = FvfFor(entry.layout.elements);
        entry.glsl       = std::move(program.glsl);
        entry.failed     = !shader->function.empty() && entry.glsl.empty();
    }
    else
    {
        entry.failed = true;
    }

    if (entry.failed)
    {
        std::fprintf(stderr, "gl: vertex shader %lu has no GLSL; its draws are skipped\n", handle);
    }

    return entry;
}

auto PixelShaderFor(const NullDevice& device, DWORD handle) -> const std::string&
{
    static auto shaders = std::map<DWORD, std::string>{};
    if (const auto found = shaders.find(handle); found != shaders.end())
    {
        return found->second;
    }

    const auto* const shader = device.PixelShader(handle);
    auto&             glsl   = shaders[handle];
    if (shader != nullptr)
    {
        glsl = TranslatePixelShader(shader->function.data());
    }

    return glsl;
}

auto FvfLayoutFor(DWORD fvf) -> const Layout&
{
    static auto layouts = std::map<DWORD, Layout>{};
    if (const auto found = layouts.find(fvf); found != layouts.end())
    {
        return found->second;
    }

    return layouts[fvf] = { FvfLayout(fvf), fvf };
}

} // namespace

auto UseProgram(const NullDevice& device) -> const Layout*
{
    const auto&        state        = device.State();
    const Layout*      layout       = nullptr;
    const std::string* vertexShader = nullptr;
    if ((state.vertexShader & 1) != 0)
    {
        const auto& entry = VertexShaderFor(device, state.vertexShader);
        if (entry.failed)
        {
            return nullptr;
        }

        layout = &entry.layout;
        if (!entry.glsl.empty())
        {
            vertexShader = &entry.glsl;
        }
    }
    else
    {
        layout = &FvfLayoutFor(state.vertexShader);
    }

    const auto* const pixelShader = [&]() -> const std::string*
    {
        if (state.pixelShader != 0)
        {
            return &PixelShaderFor(device, state.pixelShader);
        }

        return nullptr;
    }();

    if (pixelShader != nullptr && pixelShader->empty())
    {
        return nullptr;
    }

    const auto fixedFunction = vertexShader == nullptr || pixelShader == nullptr;
    const auto fvf           = [&]() -> DWORD
    {
        if (vertexShader == nullptr)
        {
            return layout->fvf;
        }

        return 0;
    }();

    const auto key = [&]() -> FixedFunctionKey
    {
        if (fixedFunction)
        {
            return KeyFor(state, fvf);
        }

        return {};
    }();

    static auto programs = std::map<std::tuple<DWORD, DWORD, FixedFunctionKey>, Program>{};
    const auto  id       = [&]()
    {
        if (vertexShader != nullptr)
        {
            return std::make_tuple(state.vertexShader, state.pixelShader, key);
        }

        return std::make_tuple(DWORD{ 0 }, state.pixelShader, key);
    }();

    auto found = programs.find(id);
    if (found == programs.end())
    {
        const auto vertex = [&]() -> std::string
        {
            if (vertexShader != nullptr)
            {
                return *vertexShader;
            }

            return FixedFunctionVertex(key);
        }();

        const auto fragment = [&]() -> std::string
        {
            if (pixelShader != nullptr)
            {
                return *pixelShader;
            }

            return FixedFunctionFragment(key);
        }();

        found = programs.emplace(id, Link(vertex, fragment)).first;
    }

    auto& program = found->second;
    if (program.id == 0)
    {
        return nullptr;
    }

    static auto current = GLuint{ 0 };
    if (current != program.id)
    {
        current = program.id;
        glUseProgram(current);
    }

    const auto& viewport = state.viewport;
    const auto  pixel    = std::array<float, 2>{ 1.0f / std::max<DWORD>(viewport.Width, 1), -1.0f / std::max<DWORD>(viewport.Height, 1) };
    if (program.pixel.Changed(pixel))
    {
        glUniform2fv(program.pixel.location, 1, pixel.data());
    }

    const auto& rs        = state.renderStates;
    const auto  alphaFunc = [&]() -> GLint
    {
        if (rs[D3DRS_ALPHATESTENABLE])
        {
            return static_cast<GLint>(rs[D3DRS_ALPHAFUNC]);
        }

        return D3DCMP_ALWAYS;
    }();

    if (program.alphaFunc.Changed(alphaFunc))
    {
        glUniform1i(program.alphaFunc.location, alphaFunc);
    }

    const auto alphaRef = static_cast<float>(rs[D3DRS_ALPHAREF] & 0xFF);
    if (program.alphaRef.Changed(alphaRef))
    {
        glUniform1f(program.alphaRef.location, alphaRef);
    }

    const auto fog = static_cast<GLint>(rs[D3DRS_FOGENABLE] != 0);
    if (program.fog.Changed(fog))
    {
        glUniform1i(program.fog.location, fog);
    }

    const auto fogColor = Rgba(rs[D3DRS_FOGCOLOR]);
    if (program.fogColor.Changed(fogColor))
    {
        glUniform3fv(program.fogColor.location, 1, fogColor.data());
    }

    if (program.c.Changed(state.vertexConstants))
    {
        glUniform4fv(program.c.location, nulldx::kVertexConstants, state.vertexConstants[0].data());
    }

    if (program.pc.Changed(state.pixelConstants))
    {
        glUniform4fv(program.pc.location, nulldx::kPixelConstants, state.pixelConstants[0].data());
    }

    if (fixedFunction)
    {
        FixedFunctionUniforms(program.id, state, fvf);
    }

    return layout;
}

} // namespace gl
