// The fixed-function texture stage cascade in GLSL, after Microsoft's D3D8 documentation and the behaviour of Wine's
// wined3d fixed-function generator. Fog and the alpha test are in the main() programs.cpp appends.
#include "ff.h"

namespace gl
{

namespace
{

using namespace ff;

auto Arg(uint32_t arg, uint32_t stage) -> std::string
{
    auto source = [&]() -> std::string
    {
        switch (arg & D3DTA_SELECTMASK)
        {
            case D3DTA_DIFFUSE:
                return "v_diffuse";
            case D3DTA_TEXTURE:
                return "t" + std::to_string(stage);
            case D3DTA_TFACTOR:
                return "ff_textureFactor";
            case D3DTA_SPECULAR:
                return "v_specular";
            case D3DTA_TEMP:
                return "temp";
            default:
                return "current";
        }
    }();

    if (arg & D3DTA_ALPHAREPLICATE)
    {
        source = "vec4(" + source + ".a)";
    }

    if (arg & D3DTA_COMPLEMENT)
    {
        source = "(1.0 - " + source + ")";
    }

    return source;
}

// part is ".rgb" or ".a". The caller saturates, as each D3D8 stage does.
auto Op(uint32_t op, const std::string& part, uint32_t stage) -> std::string
{
    const auto color = part == ".rgb";
    const auto x     = [&](int n)
    {
        return "x" + std::to_string(n) + part;
    };

    const auto colorOr = [&](const char* rgb) -> std::string
    {
        if (color)
        {
            return rgb;
        }

        return x(1);
    };

    const auto texture = "t" + std::to_string(stage);
    switch (op)
    {
        case D3DTOP_SELECTARG1:
        case D3DTOP_PREMODULATE:
            return x(1);
        case D3DTOP_SELECTARG2:
            return x(2);
        case D3DTOP_MODULATE:
            return x(1) + " * " + x(2);
        case D3DTOP_MODULATE2X:
            return "2.0 * " + x(1) + " * " + x(2);
        case D3DTOP_MODULATE4X:
            return "4.0 * " + x(1) + " * " + x(2);
        case D3DTOP_ADD:
            return x(1) + " + " + x(2);
        case D3DTOP_ADDSIGNED:
            return x(1) + " + " + x(2) + " - 0.5";
        case D3DTOP_ADDSIGNED2X:
            return "2.0 * (" + x(1) + " + " + x(2) + " - 0.5)";
        case D3DTOP_SUBTRACT:
            return x(1) + " - " + x(2);
        case D3DTOP_ADDSMOOTH:
            return x(1) + " + " + x(2) + " * (1.0 - " + x(1) + ")";
        case D3DTOP_BLENDDIFFUSEALPHA:
            return "mix(" + x(2) + ", " + x(1) + ", v_diffuse.a)";
        case D3DTOP_BLENDTEXTUREALPHA:
            return "mix(" + x(2) + ", " + x(1) + ", " + texture + ".a)";
        case D3DTOP_BLENDFACTORALPHA:
            return "mix(" + x(2) + ", " + x(1) + ", ff_textureFactor.a)";
        case D3DTOP_BLENDTEXTUREALPHAPM:
            return x(1) + " + " + x(2) + " * (1.0 - " + texture + ".a)";
        case D3DTOP_BLENDCURRENTALPHA:
            return "mix(" + x(2) + ", " + x(1) + ", current.a)";
        case D3DTOP_MODULATEALPHA_ADDCOLOR:
            return colorOr("x1.rgb + x1.a * x2.rgb");
        case D3DTOP_MODULATECOLOR_ADDALPHA:
            return colorOr("x1.rgb * x2.rgb + x1.a");
        case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
            return colorOr("(1.0 - x1.a) * x2.rgb + x1.rgb");
        case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
            return colorOr("(1.0 - x1.rgb) * x2.rgb + x1.a");
        case D3DTOP_DOTPRODUCT3:
            if (color)
            {
                return "vec3(4.0 * dot(x1.rgb - 0.5, x2.rgb - 0.5))";
            }

            return "4.0 * dot(x1.rgb - 0.5, x2.rgb - 0.5)";
        case D3DTOP_MULTIPLYADD:
            return x(0) + " + " + x(1) + " * " + x(2);
        case D3DTOP_LERP:
            return "mix(" + x(2) + ", " + x(1) + ", " + x(0) + ")";
        default:
            // The client never bump maps through fixed function; the stage passes the colour through.
            return "current" + part;
    }
}

auto Sample(const Stage& stage, uint32_t i) -> std::string
{
    const auto sampler = "s" + std::to_string(i);
    const auto coord   = "v_tex[" + std::to_string(i) + "]";
    if (stage.texture == kTextureCube)
    {
        return "texture(" + sampler + ", " + coord + ".xyz)";
    }

    const auto count = stage.transform & 0xFF;
    if ((stage.transform & D3DTTFF_PROJECTED) && count == 3)
    {
        return "textureProj(" + sampler + ", " + coord + ".xyz)";
    }

    if ((stage.transform & D3DTTFF_PROJECTED) && count == 4)
    {
        return "textureProj(" + sampler + ", " + coord + ")";
    }

    return "texture(" + sampler + ", " + coord + ".xy)";
}

} // namespace

auto FixedFunctionFragment(const FixedFunctionKey& key) -> std::string
{
    const auto c = Decode(key);
    auto       s = std::string("#version 300 es\nprecision highp float;\n");
    s += "in vec4 v_diffuse;\n"
         "in vec4 v_specular;\n"
         "in vec4 v_tex[8];\n"
         "in float v_fog;\n";
    s += "uniform vec4 ff_textureFactor;\n";
    for (uint32_t i = 0; i < c.stageCount; ++i)
    {
        const auto kind = c.stages[i].texture;
        if (kind == kTexture2D)
        {
            s += "uniform sampler2D s" + std::to_string(i) + ";\n";
        }
        else if (kind == kTextureCube)
        {
            s += "uniform samplerCube s" + std::to_string(i) + ";\n";
        }
    }

    s += "vec4 PixelShader()\n{\n";
    s += "    vec4 current = v_diffuse;\n";
    s += "    vec4 temp = vec4(0.0);\n";
    for (uint32_t i = 0; i < c.stageCount; ++i)
    {
        const auto& stage = c.stages[i];
        const auto  t     = "t" + std::to_string(i);
        // An unbound texture reads as white, so an untextured draw on the default stages shows its vertex colours.
        if (stage.texture == kWhite)
        {
            s += "    vec4 " + t + " = vec4(1.0);\n";
        }
        else if (stage.texture != kNoTexture)
        {
            s += "    vec4 " + t + " = " + Sample(stage, i) + ";\n";
        }

        // An unread argument may name a texture the stage does not sample.
        const auto fetch = [&](uint32_t op, const std::array<uint32_t, 3>& args)
        {
            for (int n = 0; n < 3; ++n)
            {
                if (UsedArgs(op) & (1u << n))
                {
                    s += "        x" + std::to_string(n) + " = " + Arg(args[n], i) + ";\n";
                }
            }
        };

        s += "    {\n";
        s += "        vec4 x0, x1, x2;\n";
        fetch(stage.colorOp, stage.colorArgs);
        s += "        vec3 color = " + Op(stage.colorOp, ".rgb", i) + ";\n";
        if (stage.colorOp == D3DTOP_DOTPRODUCT3)
        {
            s += "        float alpha = color.r;\n";
        }
        else if (stage.alphaOp == D3DTOP_DISABLE)
        {
            s += "        float alpha = current.a;\n";
        }
        else
        {
            fetch(stage.alphaOp, stage.alphaArgs);
            s += "        float alpha = " + Op(stage.alphaOp, ".a", i) + ";\n";
        }

        if (stage.resultTemp)
        {
            s += "        temp = clamp(vec4(color, alpha), 0.0, 1.0);\n";
        }
        else
        {
            s += "        current = clamp(vec4(color, alpha), 0.0, 1.0);\n";
        }

        s += "    }\n";
    }

    if (c.specular)
    {
        s += "    current.rgb = min(current.rgb + v_specular.rgb, 1.0);\n";
    }

    s += "    return current;\n}\n";
    return s;
}

} // namespace gl
