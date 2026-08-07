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

	void Mat_AwaitFrame(uint64_t frameId) override;
	void Mat_SetHeadsetRenderingPose(const vr::HmdMatrix34_t &pose) override;

	void SyncFrameGetPoses(uint64_t frameId, vr::TrackedDevicePose_t& hmdPose, vr::TrackedDevicePose_t& predictedHmdPose) override;

	void Mat_ModifyTextureCreationDetails(D3D9_COMMON_TEXTURE_DESC& desc);
	void Mat_OnPrePresent(D3D9DeviceEx* device);
	uint64_t Mat_GetVRSubmissionInfo(Rc<DxvkImage>& vrColorImage, Rc<DxvkImage>& vrDepthImage, Rc<DxvkImage>& vrHudImage, vr::HmdMatrix34_t& vrHmdPose);
	void Mat_OnPostPresent(D3D9DeviceEx* device);
	void Submit_PreSubmitCallback() override;
	void Submit_PrePresentCallBack(uint64_t vrFrameId, Rc<DxvkImage> vrColorImage, Rc<DxvkImage> vrDepthImage, Rc<DxvkImage> vrHudImage, float* vrHmdPose) override;
	void Submit_PostPresentCallback(uint64_t vrFrameId) override;
	void Mat_OnSetRenderTarget(IDirect3DSurface9 *rt);
	void Mat_OnSetDepthStencil(IDirect3DSurface9 *depth);

	void EnableFoveatedRendering(bool enabled) override;
	void SetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3) override;

	void SetZRange(float nearZ, float farZ) override;

private:
	void UpdateFoveationMode(bool shouldEnable);
	void Mat_UpdateFoveationTexture();

	void Mat_ResolveAndTransitionTexture(IDirect3DSurface9* texture, bool isDepth);
	void Submit_FillTextureData(Rc<DxvkImage> image, vr::VRVulkanTextureData_t& data);

	std::atomic<bool> m_initialized = false;

	vr::IVRSystem* m_vrSystem = nullptr;
	vr::IVRCompositor* m_vrCompositor = nullptr;
	vr::IVROverlay* m_vrOverlay = nullptr;
	vr::VROverlayHandle_t m_overlayHandle = 0;
	std::atomic<bool> m_loadingScreenModeEnabled = false;

	Com<D3D9DeviceEx> m_device;
	Com<IDirect3DSurface9> m_mat_colorTex;
	Com<IDirect3DSurface9> m_mat_depthTex;
	Com<IDirect3DSurface9> m_mat_hudTex;
	uint32_t m_renderWidth = 0;
	uint32_t m_renderHeight = 0;
	int m_msaa = 0;

	mutex m_frameSyncMutex;

	std::atomic<uint64_t> m_mat_frameAwaitedId = 0;
	std::atomic<uint64_t> m_submit_frameSubmitId = 0;
	std::atomic<uint64_t> m_submit_frameHandoffId = 0;
	bool m_mat_frameAwaited = false;
	bool m_mat_framePresented = false;
	condition_variable m_condFrameAwaited;
	condition_variable m_condFrameHandedOff;

	vr::TrackedDevicePose_t m_mat_curHeadsetPose;
	vr::TrackedDevicePose_t m_mat_predictedHeadsetPose;
	vr::HmdMatrix34_t m_mat_headsetPoseForRendering;

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
