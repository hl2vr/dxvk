#ifndef OPENVRDIRECTMODE_H_INCLUDED
#define OPENVRDIRECTMODE_H_INCLUDED

#pragma warning (disable : 4005)

#include "VkSubmitThreadCallback.h"
#include "../dxvk/dxvk_device.h"
#include "../util/rc/util_rc_ptr.h"
#include "openvr/openvr.hpp"

#include <mutex>
#include <condition_variable>
#include <atomic>


namespace dxvk {
class D3D9Surface;
class D3D9CommonTexture;
class D3D9DeviceEx;
}

/**
* OpenVR Direct Mode render class.
*/
class OpenVRDirectMode : public VkSubmitThreadCallback
{
public:
	OpenVRDirectMode();
	~OpenVRDirectMode();

	static OpenVRDirectMode *Get();

	void Init(vr::IVRCompositor *compositor);	
	void Shutdown();

	void SetRenderTextureSize(uint32_t width, uint32_t height, int msaa);

  void OnRenderTargetChanged(dxvk::Rc<dxvk::DxvkDevice> device, dxvk::D3D9Surface *rt);
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

private:
    void AwaitPreviousFrame();

  vr::IVRCompositor *m_pCompositor;

	uint32_t m_nRenderWidth;
	uint32_t m_nRenderHeight;
  int m_multiSamples;

	bool m_initialised;

  vr::VRVulkanTextureData_t m_VulkanData;
  vr::Texture_t m_VRTexture;
  dxvk::D3D9CommonTexture *m_d3d9Tex = nullptr;

	std::atomic<bool>   m_textureSet = { false };
	std::atomic<bool>   m_submitCalled = { false };
	std::atomic<bool>   m_frameRunning = { false };
  std::atomic<bool>   m_timingInfoSubmitted = { false };

	std::mutex m_mutex;
	std::condition_variable m_cv;
    dxvk::D3D9DeviceEx *m_lastUsedDevice = nullptr;
};

#endif //OPENVRDIRECTMODE_H_INCLUDED