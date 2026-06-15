#include "HL2VRInterop.h"

#include <openvr/openvr.hpp>

namespace dxvk
{

static HL2VRInterop g_HL2VRInterop;
HL2VRInterop* g_hl2vr = &g_HL2VRInterop;


void HL2VRInterop::Init(vr::IVRSystem *vrSystem, vr::IVRCompositor *vrCompositor)
{
	std::unique_lock lock(m_frameSyncMutex);
	m_vrSystem = vrSystem;
	m_vrCompositor = vrCompositor;

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
		m_device->m_d3d9Interop.LockSubmissionQueue();
		vr::TrackedDevicePose_t hmdPose;
		m_vrCompositor->WaitGetPoses(&hmdPose, 1, nullptr, 0);
		m_device->m_d3d9Interop.ReleaseSubmissionQueue();
		m_headsetPose = matQueueMode ? m_headsetPoseForRendering : hmdPose.mDeviceToAbsoluteTracking;
		m_frameAwaited = true;

		UpdateFoveationTexture();
	}

	m_colorTex = nullptr;
	m_depthTex = nullptr;
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

static HRESULT PrepareTextureForSubmission(IDirect3DDevice9Ex *device, IDirect3DSurface9 *texture, vr::VRVulkanTextureData_t &data)
{
	Com<ID3D9VkInteropDevice> vkDevice;
	device->QueryInterface(__uuidof(ID3D9VkInteropDevice), (void**) &vkDevice);
	Com<ID3D9VkInteropTexture> vkTex;
	texture->QueryInterface(__uuidof(ID3D9VkInteropTexture), (void**)&vkTex);
	VkImage image;
	VkImageCreateInfo createInfo {};
	createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	VkImageLayout curLayout;
	HRESULT hr = vkTex->GetVulkanImageInfo(&image, &curLayout, &createInfo);
	if (hr != S_OK)
	{
		return hr;
	}

	data.m_nFormat = createInfo.format;
	data.m_nWidth = createInfo.extent.width;
	data.m_nHeight = createInfo.extent.height;
	data.m_nImage = (uint64_t)image;
	data.m_nSampleCount = 1;
	vkDevice->GetSubmissionQueue(&data.m_pQueue, nullptr, &data.m_nQueueFamilyIndex);
	vkDevice->GetVulkanHandles(&data.m_pInstance, &data.m_pPhysicalDevice, &data.m_pDevice);

	VkImageSubresourceRange range;
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;
	vkDevice->TransitionTextureLayout(vkTex.ptr(), &range, curLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	return S_OK;
}

void HL2VRInterop::OnPostPresent(D3D9DeviceEx *device)
{
	std::unique_lock lock(m_frameSyncMutex);
	if (!m_initialized)
		return;

	m_device = device;

	if (m_frameAwaited)
	{
		vr::VRVulkanTextureData_t vulkanData, vulkanDepthData;
		if (m_colorTex != nullptr)
			PrepareTextureForSubmission(device, m_colorTex.ptr(), vulkanData);
		if (m_depthTex != nullptr)
			PrepareTextureForSubmission(device, m_depthTex.ptr(), vulkanDepthData);

		if (m_colorTex != nullptr)
		{
			device->m_d3d9Interop.FlushRenderingCommands();
			device->m_d3d9Interop.LockSubmissionQueue();
			vr::VRTextureWithPoseAndDepth_t textureInfo;
			textureInfo.eType = vr::TextureType_Vulkan;
			textureInfo.eColorSpace = vr::ColorSpace_Auto;
			textureInfo.handle = (void*)&vulkanData;
			textureInfo.mDeviceToAbsoluteTracking = m_headsetPose;
			int submitFlags = vr::Submit_TextureWithPose;
			if (m_depthTex != nullptr && m_farZ > m_nearZ)
			{
				submitFlags |= vr::Submit_TextureWithDepth;
				textureInfo.depth.handle = (void*)&vulkanDepthData;
				textureInfo.depth.vRange.v[0] = 0.f;
				textureInfo.depth.vRange.v[1] = 1.f;
				textureInfo.depth.mProjection = m_vrSystem->GetProjectionMatrix(vr::Eye_Left, m_nearZ, m_farZ);
			}

			vr::VRTextureBounds_t boundsLeft = { 0.f, 0.f, 0.5f, 1.f };
			m_vrCompositor->Submit(vr::Eye_Left, &textureInfo, &boundsLeft, (vr::EVRSubmitFlags)submitFlags);
			vr::VRTextureBounds_t boundsRight = { 0.5f, 0.f, 1.f, 1.f };
			if (m_depthTex != nullptr && m_farZ > m_nearZ)
			{
				textureInfo.depth.mProjection = m_vrSystem->GetProjectionMatrix(vr::Eye_Right, m_nearZ, m_farZ);
			}
			m_vrCompositor->Submit(vr::Eye_Right, &textureInfo, &boundsRight, (vr::EVRSubmitFlags)submitFlags);
			m_vrCompositor->PostPresentHandoff();

			device->m_d3d9Interop.ReleaseSubmissionQueue();
		}

		m_frameAwaited = false;
		m_condFramePresented.notify_one();
	}
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
	m_nearZ = nearZ;
	m_farZ = farZ;
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
				dxvk::Logger::info("Creating VRS image with dimensions: " + std::to_string(m_vrsImageWidth) + "x" + std::to_string(m_vrsImageHeight));
				HRESULT result = m_device->CreateTexture(m_vrsImageWidth, m_vrsImageHeight, 1, D3DUSAGE_DYNAMIC | D3DUSAGE_VRS, D3DFMT_A8, D3DPOOL_DEFAULT, &m_vrsImage, nullptr);
				if (FAILED(result))
				{
					dxvk::Logger::warn("VRS image creation failed: " + std::to_string(result));
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
