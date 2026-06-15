#pragma once

#include "IHL2VRInterop.h"
#include "openvr/openvr.hpp"

#include <d3d9.h>
#include <d3d9_device.h>

namespace dxvk
{

class HL2VRInterop : public IHL2VRInterop
{
public:
	void Init(vr::IVRSystem *vrSystem, vr::IVRCompositor *vrCompositor) override;
	void Shutdown() override;

	void ResetRenderTextures(uint32_t width, uint32_t height, int msaa) override;

	void AwaitFrame(bool matQueueMode) override;

	void ModifyTextureCreationDetails(D3D9_COMMON_TEXTURE_DESC& desc);
	void OnPostPresent(D3D9DeviceEx* device);
	void OnSetRenderTarget(IDirect3DSurface9 *rt);
	void OnSetDepthStencil(IDirect3DSurface9 *depth);

	void SetHeadsetPoseUsedForRendering(const vr::HmdMatrix34_t &pose) override;

	void EnableFoveatedRendering(bool enabled) override;
	void SetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3) override;

	void SetZRange(float nearZ, float farZ) override;

private:
	void UpdateFoveationMode(bool shouldEnable);
	void UpdateFoveationTexture();

	std::atomic<bool> m_initialized = false;

	vr::IVRSystem* m_vrSystem = nullptr;
	vr::IVRCompositor* m_vrCompositor = nullptr;

	Com<D3D9DeviceEx> m_device;
	Com<IDirect3DSurface9> m_colorTex;
	Com<IDirect3DSurface9> m_depthTex;
	uint32_t m_renderWidth = 0;
	uint32_t m_renderHeight = 0;
	int m_msaa = 0;

	std::atomic<bool> m_frameAwaited = false;
	mutex m_frameSyncMutex;
	condition_variable m_condFramePresented;
	std::atomic<int> m_frameCounter = 0;

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
};

extern HL2VRInterop* g_hl2vr;

}
