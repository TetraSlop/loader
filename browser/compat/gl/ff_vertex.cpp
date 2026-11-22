// The fixed-function vertex stage in GLSL: transform or pretransformed position, lighting, texture coordinates and fog,
// after Microsoft's D3D8 documentation and the behaviour of Wine's wined3d fixed-function generator.
#include "ff.h"

namespace gl
{

namespace
{

using namespace ff;

auto ParamAt(int slot) -> std::string
{
    return "ff_params[" + std::to_string(slot) + "]";
}

auto LightParamAt(uint32_t light, int param) -> std::string
{
    return ParamAt(kLightParamsStart + kLightParams * int(light) + param);
}

// 0 when the vertex has no such set.
auto CoordSize(DWORD fvf, uint32_t set) noexcept -> int
{
    if (set >= ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT))
    {
        return 0;
    }

    return static_cast<int>(TexCoordSize(fvf, set));
}

auto MaterialColor(uint32_t source, int materialSlot) -> std::string
{
    switch (source)
    {
        case kFromDiffuse:
            return "diffuse";
        case kFromSpecular:
            return "specular";
        default:
            return ParamAt(materialSlot);
    }
}

auto Lighting(const Config& c) -> std::string
{
    auto s = std::string{};
    s += "    vec3 lightAmbient = " + ParamAt(kAmbient) + ".rgb;\n";
    s += "    vec3 lightDiffuse = vec3(0.0);\n";
    s += "    vec3 lightSpecular = vec3(0.0);\n";
    if (c.specular)
    {
        // Without a local viewer the eye sits infinitely far down -z, so every vertex sees it along -z.
        if (c.localViewer)
        {
            s += "    vec3 toEye = -normalize(eye.xyz);\n";
        }
        else
        {
            s += "    vec3 toEye = vec3(0.0, 0.0, -1.0);\n";
        }
    }

    for (uint32_t i = 0; i < c.lightCount; ++i)
    {
        s += "    {\n";
        if (c.lights[i] == D3DLIGHT_DIRECTIONAL)
        {
            s += "        vec3 L = -" + LightParamAt(i, kLightDirection) + ".xyz;\n";
            s += "        float attenuation = 1.0;\n";
        }
        else
        {
            const auto atten = LightParamAt(i, kLightAttenuation);
            s += "        vec3 toLight = " + LightParamAt(i, kLightPosition) + ".xyz - eye.xyz;\n";
            s += "        float d = length(toLight);\n";
            s += "        vec3 L = toLight / d;\n";
            s += "        float attenuation = d > " + atten + ".w ? 0.0 : 1.0 / dot(" + atten + ".xyz, vec3(1.0, d, d * d));\n";
            if (c.lights[i] == D3DLIGHT_SPOT)
            {
                const auto spot = LightParamAt(i, kLightSpot);
                s += "        float rho = dot(-L, " + LightParamAt(i, kLightDirection) + ".xyz);\n";
                s += "        attenuation *= rho > " + spot + ".x ? 1.0 : rho <= " + spot + ".y ? 0.0 : pow((rho - " + spot + ".y) * " + spot + ".z, " + spot + ".w);\n";
            }
        }

        s += "        lightAmbient += attenuation * " + LightParamAt(i, kLightAmbient) + ".rgb;\n";
        s += "        float NdotL = dot(normal, L);\n";
        s += "        if (NdotL > 0.0) {\n";
        s += "            lightDiffuse += attenuation * NdotL * " + LightParamAt(i, kLightDiffuse) + ".rgb;\n";
        if (c.specular)
        {
            s += "            float NdotH = dot(normal, normalize(toEye + L));\n";
            s += "            if (NdotH > 0.0)\n";
            s += "                lightSpecular += attenuation * pow(NdotH, " + ParamAt(kDepth) + ".w) * " + LightParamAt(i, kLightSpecular) + ".rgb;\n";
        }

        s += "        }\n";
        s += "    }\n";
    }

    const auto diffuse  = MaterialColor(c.sources[0], kMaterialDiffuse);
    const auto ambient  = MaterialColor(c.sources[1], kMaterialAmbient);
    const auto emissive = MaterialColor(c.sources[3], kMaterialEmissive);
    s += "    v_diffuse = clamp(vec4(" + emissive + ".rgb + " + ambient + ".rgb * lightAmbient + " + diffuse + ".rgb * lightDiffuse, " + diffuse + ".a), 0.0, 1.0);\n";
    if (c.specular)
    {
        const auto specular = MaterialColor(c.sources[2], kMaterialSpecular);
        s += "    v_specular = clamp(vec4(" + specular + ".rgb * lightSpecular, " + specular + ".a), 0.0, 1.0);\n";
    }

    return s;
}

// What a stage's coordinates start from before its texture transform. D3D8 pads a short set with 1 right after its
// last component, so 2D coordinates meet a matrix's third row as their translation.
auto CoordSource(const Config& c, const Stage& stage, bool pretransformed, bool transformed) -> std::string
{
    if (!pretransformed)
    {
        switch (stage.coordGen)
        {
            case D3DTSS_TCI_CAMERASPACENORMAL >> 16:
                return "vec4(normal, 1.0)";
            case D3DTSS_TCI_CAMERASPACEPOSITION >> 16:
                return "vec4(eye.xyz, 1.0)";
            case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR >> 16:
                if (c.localViewer)
                {
                    return "vec4(reflect(normalize(eye.xyz), normal), 1.0)";
                }

                return "vec4(reflect(vec3(0.0, 0.0, 1.0), normal), 1.0)";
        }
    }

    const auto size = CoordSize(c.fvf, stage.coordIndex);
    const auto in   = "a_tex" + std::to_string(stage.coordIndex);
    if (size == 0)
    {
        return "vec4(0.0, 0.0, 0.0, 1.0)";
    }

    if (!transformed)
    {
        return in;
    }

    switch (size)
    {
        case 1:
            return "vec4(" + in + ".x, 1.0, 0.0, 0.0)";
        case 2:
            return "vec4(" + in + ".xy, 1.0, 0.0)";
        case 3:
            return "vec4(" + in + ".xyz, 1.0)";
        default:
            return in;
    }
}

auto FogFactor(uint32_t mode, const std::string& d) -> std::string
{
    const auto fog = ParamAt(kFogParams);
    switch (mode)
    {
        case D3DFOG_EXP:
            return "exp(-" + fog + ".w * " + d + ")";
        case D3DFOG_EXP2:
            return "exp(-(" + fog + ".w * " + d + ") * (" + fog + ".w * " + d + "))";
        default:
            return "(" + fog + ".y - " + d + ") * " + fog + ".z";
    }
}

} // namespace

auto FixedFunctionVertex(const FixedFunctionKey& key) -> std::string
{
    const auto c              = Decode(key);
    const auto pretransformed = (c.fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    const auto hasNormal      = !pretransformed && (c.fvf & D3DFVF_NORMAL);
    const auto hasDiffuse     = (c.fvf & D3DFVF_DIFFUSE) != 0;
    const auto hasSpecular    = (c.fvf & D3DFVF_SPECULAR) != 0;

    auto s = std::string("#version 300 es\nprecision highp float;\n");
    s += "layout(location = " + std::to_string(kPosition) + ") in vec4 a_position;\n";
    if (hasNormal)
    {
        s += "layout(location = " + std::to_string(kNormal) + ") in vec4 a_normal;\n";
    }

    if (hasDiffuse)
    {
        s += "layout(location = " + std::to_string(kDiffuse) + ") in vec4 a_diffuse;\n";
    }

    if (hasSpecular)
    {
        s += "layout(location = " + std::to_string(kSpecular) + ") in vec4 a_specular;\n";
    }

    const auto sets = (c.fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (uint32_t n = 0; n < sets && n < 8; ++n)
    {
        s += "layout(location = " + std::to_string(kTexCoord0 + n) + ") in vec4 a_tex" + std::to_string(n) + ";\n";
    }

    s += "out vec4 v_diffuse;\n"
         "out vec4 v_specular;\n"
         "out vec4 v_tex[8];\n"
         "out float v_fog;\n";
    s += "uniform mat4 ff_worldView;\n"
         "uniform mat4 ff_projection;\n"
         "uniform mat3 ff_normalMatrix;\n";
    s += "uniform vec4 ff_params[" + std::to_string(kLightParamsStart + kLightParams * c.lightCount) + "];\n";
    s += "uniform mat4 ff_texture[8];\n";

    s += "vec4 VertexShader()\n{\n";
    // D3DCOLOR comes in as its bytes lie, BGRA.
    if (hasDiffuse)
    {
        s += "    vec4 diffuse = a_diffuse.zyxw;\n";
    }
    else
    {
        s += "    vec4 diffuse = vec4(1.0);\n";
    }

    if (hasSpecular)
    {
        s += "    vec4 specular = a_specular.zyxw;\n";
    }
    else
    {
        s += "    vec4 specular = vec4(0.0);\n";
    }

    if (pretransformed)
    {
        // Screen space back to D3D clip space, with w = 1 / rhw so attributes interpolate in perspective.
        const auto viewport = ParamAt(kViewport);
        const auto depth    = ParamAt(kDepth);
        s += "    float w = a_position.w != 0.0 ? 1.0 / a_position.w : 1.0;\n";
        s += "    vec4 position = vec4(vec3((a_position.x - " + viewport + ".x) * " + viewport + ".z - 1.0, 1.0 - (a_position.y - " + viewport + ".y) * " + viewport + ".w, (a_position.z - " + depth + ".x) * " + depth + ".y) * w, w);\n";
    }
    else
    {
        s += "    vec4 eye = ff_worldView * vec4(a_position.xyz, 1.0);\n";
        s += "    vec4 position = ff_projection * eye;\n";
        if (!hasNormal)
        {
            s += "    vec3 normal = vec3(0.0);\n";
        }
        else if (c.normalize)
        {
            s += "    vec3 normal = normalize(ff_normalMatrix * a_normal.xyz);\n";
        }
        else
        {
            s += "    vec3 normal = ff_normalMatrix * a_normal.xyz;\n";
        }
    }

    if (c.lighting)
    {
        s += Lighting(c);
    }
    else
    {
        if (hasDiffuse)
        {
            s += "    v_diffuse = diffuse;\n";
        }

        if (hasSpecular)
        {
            s += "    v_specular = specular;\n";
        }
    }

    for (uint32_t i = 0; i < c.stageCount; ++i)
    {
        const auto& stage = c.stages[i];
        if (stage.texture != kTexture2D && stage.texture != kTextureCube)
        {
            continue;
        }

        const auto count       = stage.transform & 0xFF;
        const auto transformed = count >= 1 && count <= 4;
        const auto source      = CoordSource(c, stage, pretransformed, transformed);
        const auto index       = std::to_string(i);
        if (transformed)
        {
            s += "    v_tex[" + index + "] = ff_texture[" + index + "] * " + source + ";\n";
        }
        else
        {
            s += "    v_tex[" + index + "] = " + source + ";\n";
        }
    }

    if (c.tableFog != D3DFOG_NONE)
    {
        // Table fog is per pixel in D3D8; linear is exact per vertex as long as it is left unclamped here.
        s += "    float fogDistance = " + ParamAt(kDepth) + ".z > 0.5 ? position.w : position.z / position.w;\n";
        s += "    v_fog = " + FogFactor(c.tableFog, "fogDistance") + ";\n";
    }
    else if (c.vertexFog != D3DFOG_NONE)
    {
        if (c.rangeFog)
        {
            s += "    float fogDistance = length(eye.xyz);\n";
        }
        else
        {
            s += "    float fogDistance = eye.z;\n";
        }

        s += "    v_fog = clamp(" + FogFactor(c.vertexFog, "fogDistance") + ", 0.0, 1.0);\n";
    }
    else if (c.fog && hasSpecular)
    {
        // With no fog mode, or a pretransformed vertex, the fog factor rides in the specular alpha.
        s += "    v_fog = specular.a;\n";
    }

    s += "    return position;\n}\n";
    return s;
}

} // namespace gl
