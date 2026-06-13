#include "HL2VRInterop.h"

#include <openvr/openvr.hpp>

namespace dxvk
{

static HL2VRInterop g_HL2VRInterop;
HL2VRInterop* g_hl2vr = &g_HL2VRInterop;


void HL2VRInterop::Init(vr::IVRSystem *vrSystem, vr::IVRCompositor *vrCompositor)
{
	m_vrSystem = vrSystem;
	m_vrCompositor = vrCompositor;

	m_initialized = true;
}

void HL2VRInterop::Shutdown()
{
	m_initialized = false;

	m_vrSystem = nullptr;
	m_vrCompositor = nullptr;

	ResetRenderTextures(0, 0, 0);
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
	if (!m_initialized)
		return;

	if (!m_device)
		return;

	std::unique_lock lock(m_frameSyncMutex);
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
	m_device->m_d3d9Interop.LockSubmissionQueue();
	vr::TrackedDevicePose_t hmdPose;
	m_vrCompositor->WaitGetPoses(&hmdPose, 1, nullptr, 0);
	m_device->m_d3d9Interop.ReleaseSubmissionQueue();
	m_headsetPose = matQueueMode ? m_headsetPoseForRendering : hmdPose.mDeviceToAbsoluteTracking;
	m_frameAwaited = true;
}

void HL2VRInterop::ModifyTextureCreationDetails(D3D9_COMMON_TEXTURE_DESC &desc)
{
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
	if (!m_initialized)
		return;

	std::unique_lock lock(m_frameSyncMutex);
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

			vr::VRTextureBounds_t boundsLeft = { 0.f, 0.f, 0.5f, 1.f };
			m_vrCompositor->Submit(vr::Eye_Left, &textureInfo, &boundsLeft, vr::Submit_TextureWithPose);
			vr::VRTextureBounds_t boundsRight = { 0.5f, 0.f, 1.f, 1.f };
			m_vrCompositor->Submit(vr::Eye_Right, &textureInfo, &boundsRight, vr::Submit_TextureWithPose);
			m_vrCompositor->PostPresentHandoff();

			device->m_d3d9Interop.ReleaseSubmissionQueue();
		}

		m_colorTex = nullptr;
		m_depthTex = nullptr;
		m_frameAwaited = false;
		m_condFramePresented.notify_one();
	}
}

void HL2VRInterop::OnSetRenderTarget(IDirect3DSurface9 *rt)
{
	if (rt == nullptr)
		return;

	D3DSURFACE_DESC desc;
	rt->GetDesc(&desc);
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight && !m_colorTex)
	{
		m_colorTex = rt;
	}
}

void HL2VRInterop::OnSetDepthStencil(IDirect3DSurface9 *depth)
{
	if (depth == nullptr)
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

}
