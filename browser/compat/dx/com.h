// IUnknown for the stand-ins' COM objects.
#pragma once

#include <windows.h>

#include <objbase.h>

#include <type_traits>

namespace nulldx
{

// value is not deduced, so braces and literals convert to T; S_OK is also D3D_OK and DS_OK.
template <class T>
auto Store(T* out, const std::common_type_t<T>& value, HRESULT error) -> HRESULT
{
    if (out == nullptr)
    {
        return error;
    }

    *out = value;
    return S_OK;
}

template <class I>
class Object : public I
{
public:
    Object() = default;

    Object(const Object&) = delete;

    Object(Object&&) = delete;

    auto operator=(const Object&) -> Object& = delete;

    auto operator=(Object&&) -> Object& = delete;

    virtual ~Object() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || Implements(riid))
        {
            *object = static_cast<I*>(this);
            this->AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++refs_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto count = --refs_;
        if (count == 0)
        {
            delete this;
        }

        return count;
    }

protected:
    virtual auto Implements(REFIID riid) const -> bool = 0;

private:
    ULONG refs_ = 1;
};

} // namespace nulldx
