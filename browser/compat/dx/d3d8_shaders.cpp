#include "d3d8_null.h"

#include <cstring>

namespace nulldx
{

namespace
{

// FVF codes never set bit 0 (D3DFVF_RESERVED0), so odd handles tell shaders apart in SetVertexShader.
constexpr auto ShaderHandle(DWORD id) -> DWORD
{
    return (id << 1) | 1;
}

auto CopyDeclaration(const DWORD* tokens) -> std::vector<DWORD>
{
    size_t count = 0;
    while (tokens[count] != D3DVSD_END())
    {
        const auto token = tokens[count];
        const auto type  = (token & D3DVSD_TOKENTYPEMASK) >> D3DVSD_TOKENTYPESHIFT;
        if (type == D3DVSD_TOKEN_CONSTMEM)
        {
            count += 4 * ((token & D3DVSD_CONSTCOUNTMASK) >> D3DVSD_CONSTCOUNTSHIFT);
        }
        else if (type == D3DVSD_TOKEN_EXT)
        {
            count += (token & D3DVSD_EXTCOUNTMASK) >> D3DVSD_EXTCOUNTSHIFT;
        }

        ++count;
    }

    return { tokens, tokens + count + 1 };
}

// Comments are skipped whole, since D3DXAssembleShader keeps the source text in one.
auto CopyFunction(const DWORD* tokens) -> std::vector<DWORD>
{
    if (tokens == nullptr)
    {
        return {};
    }

    size_t count = 1;
    while (tokens[count] != D3DSIO_END)
    {
        const auto token = tokens[count];
        if ((token & D3DSI_OPCODE_MASK) == D3DSIO_COMMENT)
        {
            count += (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT;
        }

        ++count;
    }

    return { tokens, tokens + count + 1 };
}

} // namespace

HRESULT NullDevice::CreateVertexShader(const DWORD* declaration, const DWORD* function, DWORD* handle, DWORD usage)
{
    if (declaration == nullptr || handle == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *handle                 = ShaderHandle(nextShader_++);
    vertexShaders_[*handle] = { CopyDeclaration(declaration), CopyFunction(function) };
    return D3D_OK;
}

HRESULT NullDevice::SetVertexShader(DWORD handle)
{
    if ((handle & 1) != 0 && vertexShaders_.count(handle) == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    Target().vertexShader = handle;
    if (auto* const mask = Recorded())
    {
        mask->vertexShader = true;
    }

    return D3D_OK;
}

HRESULT NullDevice::GetVertexShader(DWORD* handle)
{
    return Store(handle, state_.vertexShader, D3DERR_INVALIDCALL);
}

HRESULT NullDevice::DeleteVertexShader(DWORD handle)
{
    if (vertexShaders_.erase(handle) == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    if (state_.vertexShader == handle)
    {
        state_.vertexShader = 0;
    }

    return D3D_OK;
}

HRESULT NullDevice::SetVertexShaderConstant(DWORD index, const void* data, DWORD count)
{
    if (data == nullptr || index + count > kVertexConstants)
    {
        return D3DERR_INVALIDCALL;
    }

    std::memcpy(Target().vertexConstants[index].data(), data, count * 4 * sizeof(float));
    if (auto* const mask = Recorded())
    {
        for (DWORD i = index; i < index + count; ++i)
        {
            mask->vertexConstants.set(i);
        }
    }

    return D3D_OK;
}

HRESULT NullDevice::GetVertexShaderConstant(DWORD index, void* data, DWORD count)
{
    if (data == nullptr || index + count > kVertexConstants)
    {
        return D3DERR_INVALIDCALL;
    }

    std::memcpy(data, state_.vertexConstants[index].data(), count * 4 * sizeof(float));
    return D3D_OK;
}

HRESULT NullDevice::CreatePixelShader(const DWORD* function, DWORD* handle)
{
    if (function == nullptr || handle == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    *handle                = ShaderHandle(nextShader_++);
    pixelShaders_[*handle] = { {}, CopyFunction(function) };
    return D3D_OK;
}

HRESULT NullDevice::SetPixelShader(DWORD handle)
{
    if (handle != 0 && pixelShaders_.count(handle) == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    Target().pixelShader = handle;
    if (auto* const mask = Recorded())
    {
        mask->pixelShader = true;
    }

    return D3D_OK;
}

HRESULT NullDevice::GetPixelShader(DWORD* handle)
{
    return Store(handle, state_.pixelShader, D3DERR_INVALIDCALL);
}

HRESULT NullDevice::DeletePixelShader(DWORD handle)
{
    if (pixelShaders_.erase(handle) == 0)
    {
        return D3DERR_INVALIDCALL;
    }

    if (state_.pixelShader == handle)
    {
        state_.pixelShader = 0;
    }

    return D3D_OK;
}

HRESULT NullDevice::SetPixelShaderConstant(DWORD index, const void* data, DWORD count)
{
    if (data == nullptr || index + count > kPixelConstants)
    {
        return D3DERR_INVALIDCALL;
    }

    std::memcpy(Target().pixelConstants[index].data(), data, count * 4 * sizeof(float));
    if (auto* const mask = Recorded())
    {
        for (DWORD i = index; i < index + count; ++i)
        {
            mask->pixelConstants.set(i);
        }
    }

    return D3D_OK;
}

HRESULT NullDevice::GetPixelShaderConstant(DWORD index, void* data, DWORD count)
{
    if (data == nullptr || index + count > kPixelConstants)
    {
        return D3DERR_INVALIDCALL;
    }

    std::memcpy(data, state_.pixelConstants[index].data(), count * 4 * sizeof(float));
    return D3D_OK;
}

auto NullDevice::VertexShader(DWORD handle) const -> const Shader*
{
    const auto found = vertexShaders_.find(handle);
    if (found == vertexShaders_.end())
    {
        return nullptr;
    }

    return &found->second;
}

auto NullDevice::PixelShader(DWORD handle) const -> const Shader*
{
    const auto found = pixelShaders_.find(handle);
    if (found == pixelShaders_.end())
    {
        return nullptr;
    }

    return &found->second;
}

} // namespace nulldx
