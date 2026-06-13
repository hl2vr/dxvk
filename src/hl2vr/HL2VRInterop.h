#pragma once

#include "IHL2VRInterop.h"

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

	void AwaitFrame() override;

	void ModifyTextureCreationDetails(D3D9_COMMON_TEXTURE_DESC& desc);
	void OnPostPresent(D3D9DeviceEx* device);
	void OnSetRenderTarget(IDirect3DSurface9 *rt);
	void OnSetDepthStencil(IDirect3DSurface9 *depth);

private:
	bool m_initialized = false;

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
};

extern HL2VRInterop* g_hl2vr;

}
