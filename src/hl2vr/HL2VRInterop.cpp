#include "HL2VRInterop.h"

#include <openvr/openvr.hpp>

namespace dxvk
{

static HL2VRInterop g_HL2VRInterop;
HL2VRInterop* g_hl2vr = &g_HL2VRInterop;

VkSubmitThreadCallback *g_pVkSubmitThreadCallback = &g_HL2VRInterop;


void HL2VRInterop::Init(vr::IVRSystem *vrSystem, vr::IVRCompositor *vrCompositor)
{
	std::unique_lock lock(m_frameSyncMutex);
	m_vrSystem = vrSystem;
	m_vrCompositor = vrCompositor;
	m_vrCompositor->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);

	m_initialized = true;
}

void HL2VRInterop::Shutdown()
{
	std::unique_lock lock(m_frameSyncMutex);
	m_initialized = false;

	m_vrSystem = nullptr;
	m_vrCompositor = nullptr;

	m_colorTex = nullptr;
	m_depthTex = nullptr;
	m_vrsImage = nullptr;
	m_device = nullptr;
}

void HL2VRInterop::ResetRenderTextures(uint32_t width, uint32_t height, int msaa)
{
	m_renderWidth = width;
	m_renderHeight = height;
	m_msaa = msaa;
}

void HL2VRInterop::AwaitFrame(bool matQueueMode)
{
	std::unique_lock lock(m_frameSyncMutex);
	if (!m_initialized)
		return;

	if (!m_device)
		return;

	if (m_frameAwaited)
	{
		// still awaiting previous frame's present call
		if (m_condFramePresented.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
		{
			Logger::warn("VR: Awaiting previous frame present timed out");
		}
		m_frameAwaited = false;
	}

	++m_frameCounter;
	if (m_colorTex != nullptr)
	{
		// only call WaitGetPoses if we have a color texture from the previous frame, as otherwise the Vulkan queues might be out of date
		vr::TrackedDevicePose_t hmdPose;
		m_vrCompositor->WaitGetPoses(&hmdPose, 1, nullptr, 0);
		m_headsetPose = matQueueMode ? m_headsetPoseForRendering : hmdPose.mDeviceToAbsoluteTracking;
		m_frameAwaited = true;

		UpdateFoveationTexture();
	}

	m_colorTex = nullptr;
	m_depthTex = nullptr;
	m_timingInfoSubmitted = false;
}

void HL2VRInterop::ModifyTextureCreationDetails(D3D9_COMMON_TEXTURE_DESC &desc)
{
	if (!m_initialized)
		return;

	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight)
	{
		desc.MultiSample = static_cast<D3DMULTISAMPLE_TYPE>(m_msaa);
	}
}

void HL2VRInterop::OnPrePresent(D3D9DeviceEx *device)
{
	if (!m_initialized)
		return;

	std::unique_lock lock(m_frameSyncMutex);
	m_device = device;
	if (!m_initialized || !m_frameAwaited)
		return;

	// transition our render textures to proper image layout before submitting to OpenVR
	if (m_colorTex != nullptr) {
		auto *colorTexCommon = static_cast<D3D9Surface*>(m_colorTex.ptr())->GetCommonTexture();
		VkImageSubresourceRange subresources = {
			VK_IMAGE_ASPECT_COLOR_BIT,
			0, colorTexCommon->GetImage()->info().mipLevels,
			0, colorTexCommon->GetImage()->info().numLayers
		  };
		device->TransformImage(colorTexCommon, &subresources,
		  colorTexCommon->GetImage()->info().layout,
		  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	}
	if (m_depthTex != nullptr) {
		auto *depthTexCommon = static_cast<D3D9Surface*>(m_depthTex.ptr())->GetCommonTexture();
		VkImageSubresourceRange subresources = {
			VK_IMAGE_ASPECT_DEPTH_BIT,
			0, depthTexCommon->GetImage()->info().mipLevels,
			0, depthTexCommon->GetImage()->info().numLayers
		  };
		device->TransformImage(depthTexCommon, &subresources,
		  depthTexCommon->GetImage()->info().layout,
		  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	}
}

void HL2VRInterop::OnPostPresent(D3D9DeviceEx *device)
{
}

void HL2VRInterop::PreSubmitCallback()
{
	if (!m_initialized || !m_frameAwaited || m_timingInfoSubmitted)
		return;

	std::unique_lock lock(m_frameSyncMutex);
	if (m_initialized && m_frameAwaited && !m_timingInfoSubmitted) {
		m_vrCompositor->SubmitExplicitTimingData();
		m_timingInfoSubmitted = true;
	}
}

void HL2VRInterop::FillTextureData(IDirect3DSurface9 *surface, vr::VRVulkanTextureData_t &data)
{
	D3D9Surface* rt = static_cast<D3D9Surface*>(surface);

	const auto& info = rt->GetCommonTexture()->GetImage()->info();
	data.m_nHeight = info.extent.height;
	data.m_nWidth = info.extent.width;
	// VkPhysicalDevice
	auto dxvkDevice = m_device->GetDXVKDevice();
	data.m_pPhysicalDevice = dxvkDevice->adapter()->handle();
	// VkDevice
	data.m_pDevice = dxvkDevice->handle();
	// VkImage
	data.m_nImage = (uint64_t)rt->GetCommonTexture()->GetImage()->handle();
	// VkInstance
	data.m_pInstance = dxvkDevice->instance()->vki()->instance();
	// VkQueue
	data.m_pQueue = dxvkDevice->queues().graphics.queueHandle;
	data.m_nQueueFamilyIndex = dxvkDevice->queues().graphics.queueFamily;
	data.m_nFormat = info.format;
	data.m_nSampleCount = info.sampleCount;
}

void HL2VRInterop::PrePresentCallBack()
{
	if (!m_initialized || !m_timingInfoSubmitted || !m_frameAwaited || !m_colorTex)
		return;

	std::unique_lock lock(m_frameSyncMutex);
	if (!m_initialized || !m_timingInfoSubmitted || !m_frameAwaited)
		return;

	if (m_vrCompositor->CanRenderScene() && m_colorTex) {
		static vr::VRTextureBounds_t leftBounds = {0.0f, 0.0f, 0.5f, 1.0f};
		static vr::VRTextureBounds_t rightBounds = {0.5f, 0.0f, 1.0f, 1.0f};

		vr::VRVulkanTextureData_t colorTexData, depthTexData;
		FillTextureData(m_colorTex.ptr(), colorTexData);
		vr::VRTextureWithPoseAndDepth_t submitInfo;
		submitInfo.eType = vr::TextureType_Vulkan;
		submitInfo.eColorSpace = vr::ColorSpace_Auto;
		submitInfo.handle = (void*)&colorTexData;
		submitInfo.mDeviceToAbsoluteTracking = m_headsetPose;

		int flags = vr::Submit_TextureWithPose;
		if (m_depthTex != nullptr)
		{
			flags |= vr::Submit_TextureWithDepth;
			FillTextureData(m_depthTex.ptr(), depthTexData);
			submitInfo.depth.handle = (void*)&depthTexData;
			submitInfo.depth.mProjection = m_projectionLeft;
			submitInfo.depth.vRange.v[0] = 0;
			submitInfo.depth.vRange.v[1] = 1;
		}
		m_vrCompositor->Submit(vr::Eye_Left, &submitInfo, &leftBounds, (vr::EVRSubmitFlags)flags);

		if (m_depthTex != nullptr)
			submitInfo.depth.mProjection = m_projectionRight;
		m_vrCompositor->Submit(vr::Eye_Right, &submitInfo, &rightBounds, (vr::EVRSubmitFlags)flags);
	}
}

void HL2VRInterop::PostPresentCallback()
{
	if (!m_initialized || !m_frameAwaited)
		return;

	std::unique_lock lock(m_frameSyncMutex);
	if (!m_initialized || !m_frameAwaited)
		return;

	m_vrCompositor->PostPresentHandoff();
	m_timingInfoSubmitted = false;
	m_frameAwaited = false;
	m_condFramePresented.notify_one();
}

void HL2VRInterop::OnSetRenderTarget(IDirect3DSurface9 *rt)
{
	if (!m_initialized || rt == nullptr)
		return;

	D3DSURFACE_DESC desc;
	rt->GetDesc(&desc);
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight && !m_colorTex)
	{
		m_colorTex = rt;
	}

	UpdateFoveationMode(m_colorTex == rt);
}

void HL2VRInterop::OnSetDepthStencil(IDirect3DSurface9 *depth)
{
	if (!m_initialized || depth == nullptr)
		return;

	D3DSURFACE_DESC desc;
	depth->GetDesc(&desc);
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight && !m_depthTex)
	{
		m_depthTex = depth;
	}
}

void HL2VRInterop::SetHeadsetPoseUsedForRendering(const vr::HmdMatrix34_t &pose)
{
	m_headsetPoseForRendering = pose;
}

void HL2VRInterop::EnableFoveatedRendering(bool enabled)
{
	if (enabled != m_FoveatedRenderingEnabled)
		m_FoveationNeedsUpdate = true;
	m_FoveatedRenderingEnabled = enabled;
}

void HL2VRInterop::SetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3)
{
	m_FoveationCenterLX = centerLX;
	m_FoveationCenterLY = centerLY;
	m_FoveationCenterRX = centerRX;
	m_FoveationCenterRY = centerRY;
	m_FoveationRadius1 = radius1;
	m_FoveationRadius2 = radius2;
	m_FoveationRadius3 = radius3;
	m_FoveationNeedsUpdate = true;
}

void HL2VRInterop::SetZRange(float nearZ, float farZ)
{
	if (!m_initialized)
		return;

	if (nearZ != m_nearZ || farZ != m_farZ)
	{
		m_nearZ = nearZ;
		m_farZ = farZ;
		m_projectionLeft = m_vrSystem->GetProjectionMatrix(vr::Eye_Left, m_nearZ, m_farZ);
		m_projectionRight = m_vrSystem->GetProjectionMatrix(vr::Eye_Right, m_nearZ, m_farZ);
	}
}

void HL2VRInterop::UpdateFoveationMode(bool shouldEnable)
{
	if (!m_initialized || !m_FoveatedRenderingEnabled || !m_device)
		return;

	if (shouldEnable)
		m_device->m_d3d9VRS.Enable();
	else
		m_device->m_d3d9VRS.Disable();
}

void HL2VRInterop::UpdateFoveationTexture()
{
	if (!m_device)
		return;

	auto* vrsInterface = &m_device->m_d3d9VRS;

	if (m_FoveationNeedsUpdate)
	{
		m_FoveationNeedsUpdate = false;
		if (m_FoveatedRenderingEnabled)
		{
			VkExtent2D minTexelSize = vrsInterface->GetMinTexelSize();
			VkExtent2D maxTexelSize = vrsInterface->GetMaxTexelSize();
			VkExtent2D desiredTexelSize;
			desiredTexelSize.width = dxvk::clamp(16u, minTexelSize.width, maxTexelSize.height);
			desiredTexelSize.height = dxvk::clamp(16u, minTexelSize.height, maxTexelSize.height);

			UINT desiredVrsImageWidth = (m_renderWidth + desiredTexelSize.width - 1) / desiredTexelSize.width;
			UINT desiredVrsImageHeight = (m_renderHeight + desiredTexelSize.height - 1) / desiredTexelSize.height;

			if (!m_vrsImage || desiredVrsImageWidth != m_vrsImageWidth || desiredVrsImageHeight != m_vrsImageHeight)
			{
				m_vrsImageWidth = desiredVrsImageWidth;
				m_vrsImageHeight = desiredVrsImageHeight;
				m_vrsImage = nullptr;
				Logger::info("Creating VRS image with dimensions: " + std::to_string(m_vrsImageWidth) + "x" + std::to_string(m_vrsImageHeight));
				HRESULT result = m_device->CreateTexture(m_vrsImageWidth, m_vrsImageHeight, 1, D3DUSAGE_DYNAMIC | D3DUSAGE_VRS, D3DFMT_A8, D3DPOOL_DEFAULT, &m_vrsImage, nullptr);
				if (FAILED(result))
				{
					Logger::warn("VRS image creation failed: " + std::to_string(result));
				}
			}

			if (!m_vrsImage)
				return;

			D3DLOCKED_RECT lockedRect;
			if (m_vrsImage->LockRect(0, &lockedRect, nullptr, D3DLOCK_DISCARD) == D3D_OK)
			{
				BYTE *data = (BYTE *)lockedRect.pBits;
				UINT halfWidth = m_vrsImageWidth / 2;
				UINT centerX1 = halfWidth * m_FoveationCenterLX;
				UINT centerX2 = halfWidth * m_FoveationCenterRX + halfWidth;
				UINT centerY1 = m_vrsImageHeight * m_FoveationCenterLY;
				UINT centerY2 = m_vrsImageHeight * m_FoveationCenterRY;
				UINT radius1 = m_vrsImageHeight * 0.5f * m_FoveationRadius1;
				UINT radius2 = m_vrsImageHeight * 0.5f * m_FoveationRadius2;
				UINT radius3 = m_vrsImageHeight * 0.5f * m_FoveationRadius3;
				for (UINT y = 0; y < m_vrsImageHeight; ++y)
				{
					for (UINT x = 0; x < m_vrsImageWidth; ++x)
					{
						UINT distSqr1 = (x - centerX1) * (x - centerX1) + (y - centerY1) * (y - centerY1);
						UINT distSqr2 = (x - centerX2) * (x - centerX2) + (y - centerY2) * (y - centerY2);
						UINT distSqr = std::min(distSqr1, distSqr2);
						UINT idx = y * lockedRect.Pitch + x;
						if (distSqr >= radius3 * radius3)
						{
							data[idx] = 0x0A; // 4x4 block
						}
						else if (distSqr >= radius2 * radius2)
						{
							data[idx] = 0x05; // 2x2 block
						}
						else if (distSqr >= radius1 * radius1)
						{
							UINT horz = std::min(std::fabs(x - centerX1), fabs(x - centerX2));
							UINT vert = std::min(std::fabs(y - centerY1), fabs(y - centerY2));
							if (vert <= horz)
								data[idx] = 0x01; // 1x2 block
							else
								data[idx] = 0x04; // 2x1 block
						}
						else
						{
							data[idx] = 0;
						}
					}
				}
				m_vrsImage->UnlockRect(0);
			}

			vrsInterface->SetShadingRateImage(m_vrsImage.ptr(), desiredTexelSize);
		}
		else
		{
			vrsInterface->Disable();
		}
	}
}

}
