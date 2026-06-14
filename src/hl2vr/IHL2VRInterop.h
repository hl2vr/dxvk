#pragma once
#include <cstdint>

namespace vr
{
struct HmdMatrix34_t;
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

    virtual void AwaitFrame(bool matQueueMode) = 0;

	virtual void SetHeadsetPoseUsedForRendering(const vr::HmdMatrix34_t& pose) = 0;

	virtual void EnableFoveatedRendering(bool enabled) = 0;
	virtual void SetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3) = 0;

	virtual void SetZRange(float nearZ, float farZ) = 0;
};
