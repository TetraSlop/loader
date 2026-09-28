#pragma once
#include <windows.h>

namespace tmloader::detail
{

class ComScope final
{
public:
    ComScope();
    ~ComScope();
    ComScope(const ComScope&)                    = delete;
    auto operator=(const ComScope&) -> ComScope& = delete;
    ComScope(ComScope&&)                         = delete;
    auto operator=(ComScope&&) -> ComScope&      = delete;

private:
    const HRESULT result_;
};

struct SurfacePixels
{
    INT   pitch = 0;
    BYTE* data  = nullptr;
};

class LockedSurface final
{
public:
    explicit LockedSurface(void* surface);
    ~LockedSurface();
    LockedSurface(const LockedSurface&)                    = delete;
    auto operator=(const LockedSurface&) -> LockedSurface& = delete;
    LockedSurface(LockedSurface&&)                         = delete;
    auto operator=(LockedSurface&&) -> LockedSurface&      = delete;

    auto pixels() const noexcept -> const SurfacePixels&;
    void unlock();

private:
    void*         surface_;
    SurfacePixels pixels_;
};

} // namespace tmloader::detail
