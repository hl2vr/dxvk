#ifndef OPENVRDIRECTMODE_H_INCLUDED
#define OPENVRDIRECTMODE_H_INCLUDED

#pragma warning (disable : 4005)

#include "VkSubmitThreadCallback.h"
#include "../dxvk/dxvk_device.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/com/com_pointer.h"
#include "openvr/openvr.hpp"

#include <mutex>
#include <condition_variable>
#include <atomic>


namespace dxvk {
class D3D9Surface;
class D3D9CommonTexture;
class D3D9DeviceEx;
}

class ID3D9VRS;

/**
* OpenVR Direct Mode render class.
*/
class OpenVRDirectMode : public VkSubmitThreadCallback
{
public:
	OpenVRDirectMode();
	~OpenVRDirectMode();

	static OpenVRDirectMode *Get();

	void Init(vr::IVRSystem* system, vr::IVRCompositor *compositor);	
	void Shutdown();

	void SetRenderTextureSize(uint32_t width, uint32_t height, int msaa);

  void OnRenderTargetChanged(dxvk::D3D9DeviceEx* device, dxvk::D3D9Surface *rt, bool isDepth = false);
	void PrePresent(dxvk::D3D9DeviceEx *device);
	void PostPresent();
	void StartFrame();
	int GetCurrentRenderTexture();
	int GetTotalStoredTextures();

  int DetermineMSAA(uint32_t width, uint32_t height);
	
	// VkSubmitThreadCallback
  virtual void PreSubmitCallback();
	virtual void PrePresentCallBack();
	virtual void PostPresentCallback();

	void EnableFoveatedRendering(bool enabled);
	void SetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3);

	void SetZRange(float zNearL, float zFarL, float zNearR, float zFarR);

private:
    void AwaitPreviousFrame();
	void UpdateFoveationMode(bool shouldEnable);
	void UpdateFoveationTexture();

	vr::IVRSystem *m_pSystem;
  vr::IVRCompositor *m_pCompositor;

	uint32_t m_nRenderWidth;
	uint32_t m_nRenderHeight;
  int m_multiSamples;

	bool m_initialised;

  vr::VRVulkanTextureData_t m_VulkanData;
  vr::VRVulkanTextureData_t m_VulkanDataDepth;
  vr::VRTextureWithPoseAndDepth_t m_VRTexture;
  dxvk::D3D9CommonTexture *m_d3d9Tex = nullptr;
  dxvk::D3D9CommonTexture *m_d3d9DepthTex = nullptr;

	std::atomic<bool>   m_textureSet = { false };
	std::atomic<bool>   m_submitCalled = { false };
	std::atomic<bool>   m_frameRunning = { false };
  std::atomic<bool>   m_timingInfoSubmitted = { false };

	std::mutex m_mutex;
	std::condition_variable m_cv;
    dxvk::D3D9DeviceEx *m_lastUsedDevice = nullptr;

	bool m_FoveatedRenderingEnabled = false;
	bool m_FoveationNeedsUpdate = false;
	float m_FoveationCenterLX = 0.5f;
	float m_FoveationCenterLY = 0.5f;
	float m_FoveationCenterRX = 0.5f;
	float m_FoveationCenterRY = 0.5f;
	float m_FoveationRadius1 = 0.2f;
	float m_FoveationRadius2 = 0.4f;
	float m_FoveationRadius3 = 0.6f;
	dxvk::Com<IDirect3DTexture9> m_vrsImage;
	UINT m_vrsImageWidth = 0;
	UINT m_vrsImageHeight = 0;
	dxvk::Com<IDirect3DDevice9> m_activeDevice = nullptr;
	dxvk::Com<ID3D9VRS> m_vrsInterface = nullptr;

	float m_zNearL, m_zFarL;
	float m_zNearR, m_zFarR;
};

#endif //OPENVRDIRECTMODE_H_INCLUDED
