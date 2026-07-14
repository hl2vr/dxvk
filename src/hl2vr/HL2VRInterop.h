#pragma once

#include "IHL2VRInterop.h"
#include "openvr/openvr.hpp"

#include "VkSubmitThreadCallback.h"
#include <d3d9.h>
#include <d3d9_device.h>

namespace dxvk
{

class HL2VRInterop : public IHL2VRInterop, public VkSubmitThreadCallback
{
public:
	void Init(vr::IVRSystem *vrSystem, vr::IVRCompositor *vrCompositor, vr::IVROverlay *vrOverlay, vr::VROverlayHandle_t overlayHandle) override;
	void Shutdown() override;

	void SetLoadingScreenMode(bool enable) override;

	void ResetRenderTextures(uint32_t width, uint32_t height, int msaa) override;

	void StartFrame(const vr::HmdMatrix34_t& hmdPose) override;
	void AwaitFrame(bool matQueueMode) override;

	void GetHeadsetPoses(vr::TrackedDevicePose_t& hmdPose, vr::TrackedDevicePose_t& predictedHmdPose) override;

	void ModifyTextureCreationDetails(D3D9_COMMON_TEXTURE_DESC& desc);
	void OnPrePresent(D3D9DeviceEx* device);
	uint64_t GetVRSubmissionInfo(Rc<DxvkImage>& vrColorImage, Rc<DxvkImage>& vrDepthImage, vr::HmdMatrix34_t& vrHmdPose);
	void OnPostPresent(D3D9DeviceEx* device);
	void PreSubmitCallback() override;
	void PrePresentCallBack(Rc<DxvkImage> vrColorImage, Rc<DxvkImage> vrDepthImage, float* vrHmdPose) override;
	void PostPresentCallback(uint64_t vrFrameId) override;
	void OnSetRenderTarget(IDirect3DSurface9 *rt);
	void OnSetDepthStencil(IDirect3DSurface9 *depth);

	void EnableFoveatedRendering(bool enabled) override;
	void SetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3) override;

	void SetZRange(float nearZ, float farZ) override;

private:
	void UpdateFoveationMode(bool shouldEnable);
	void UpdateFoveationTexture();

	void ResolveAndTransitionTexture(IDirect3DSurface9* texture, bool isDepth);
	void FillTextureData(Rc<DxvkImage> image, vr::VRVulkanTextureData_t& data);

	std::atomic<bool> m_initialized = false;

	vr::IVRSystem* m_vrSystem = nullptr;
	vr::IVRCompositor* m_vrCompositor = nullptr;
	vr::IVROverlay* m_vrOverlay = nullptr;
	vr::VROverlayHandle_t m_overlayHandle = 0;
	std::atomic<bool> m_loadingScreenModeEnabled = false;

	Com<D3D9DeviceEx> m_device;
	Com<IDirect3DSurface9> m_colorTex;
	Com<IDirect3DSurface9> m_depthTex;
	uint32_t m_renderWidth = 0;
	uint32_t m_renderHeight = 0;
	int m_msaa = 0;

	std::atomic<uint64_t> m_framePrepared = 0;
	std::atomic<uint64_t> m_frameAwaited = 0;
	std::atomic<uint64_t> m_frameStarted = 0;
	std::atomic<uint64_t> m_frameRenderingStarted = 0;
	std::atomic<uint64_t> m_frameCompleted = 0;
	std::atomic<bool> m_timingInfoSubmitted = false;
	mutex m_frameSyncMutex;
	condition_variable m_condFrameRenderStarted;
	condition_variable m_condFramePresented;
	condition_variable m_condFrameAwaited;
	std::atomic<int> m_frameCounter = 0;

	vr::TrackedDevicePose_t m_curHeadsetPose;
	vr::TrackedDevicePose_t m_predictedHeadsetPose;
	vr::HmdMatrix34_t m_headsetPoseForRendering;
	vr::HmdMatrix34_t m_headsetPose;

	bool m_FoveatedRenderingEnabled = false;
	bool m_FoveationNeedsUpdate = false;
	float m_FoveationCenterLX = 0.5f;
	float m_FoveationCenterLY = 0.5f;
	float m_FoveationCenterRX = 0.5f;
	float m_FoveationCenterRY = 0.5f;
	float m_FoveationRadius1 = 0.3f;
	float m_FoveationRadius2 = 0.7f;
	float m_FoveationRadius3 = 0.9f;
	Com<IDirect3DTexture9> m_vrsImage;
	UINT m_vrsImageWidth = 0;
	UINT m_vrsImageHeight = 0;

	float m_nearZ = 0;
	float m_farZ = 0;
	vr::HmdMatrix44_t m_projectionLeft, m_projectionRight;
};

extern HL2VRInterop* g_hl2vr;

}
