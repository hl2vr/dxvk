#pragma once
#include <cstdint>

namespace vr
{
	struct HmdMatrix34_t;
	struct TrackedDevicePose_t;
	class IVRSystem;
	class IVRCompositor;
	class IVROverlay;
	typedef uint64_t VROverlayHandle_t;
}

class IHL2VRInterop
{
public:
    virtual ~IHL2VRInterop() = default;

    virtual void Init(vr::IVRSystem* vrSystem, vr::IVRCompositor* vrCompositor, vr::IVROverlay* vrOverlay, vr::VROverlayHandle_t loadingScreenOverlay) = 0;
    virtual void Shutdown() = 0;

	virtual void SetLoadingScreenMode(bool enable) = 0;

    virtual void ResetRenderTextures(uint32_t width, uint32_t height, int msaa) = 0;

	virtual void StartFrame(const vr::HmdMatrix34_t& hmdPose) = 0;
    virtual void AwaitFrame(bool matQueueMode) = 0;

	virtual void GetHeadsetPoses(vr::TrackedDevicePose_t& hmdPose, vr::TrackedDevicePose_t& predictedHmdPose) = 0;

	virtual void EnableFoveatedRendering(bool enabled) = 0;
	virtual void SetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3) = 0;

	virtual void SetZRange(float nearZ, float farZ) = 0;
};
