#pragma once
#include <cstdint>

namespace vr
{
    class IVRSystem;
	class IVRCompositor;
}

class IHL2VRInterop
{
public:
    virtual ~IHL2VRInterop() = default;

    virtual void Init(vr::IVRSystem* vrSystem, vr::IVRCompositor* vrCompositor) = 0;
    virtual void Shutdown() = 0;

    virtual void ResetRenderTextures(uint32_t width, uint32_t height, int msaa) = 0;

    virtual void AwaitFrame() = 0;
};
