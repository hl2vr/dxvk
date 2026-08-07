#include "HL2VRInterop.h"

#include <openvr/openvr.hpp>

namespace dxvk
{

static HL2VRInterop g_HL2VRInterop;
HL2VRInterop* g_hl2vr = &g_HL2VRInterop;

VkSubmitThreadCallback *g_pVkSubmitThreadCallback = &g_HL2VRInterop;


void HL2VRInterop::Init(vr::IVRSystem *vrSystem, vr::IVRCompositor *vrCompositor, vr::IVROverlay *vrOverlay, vr::VROverlayHandle_t overlayHandle)
{
	std::unique_lock lock(m_frameSyncMutex);
	m_vrSystem = vrSystem;
	m_vrCompositor = vrCompositor;
	m_vrCompositor->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
	m_vrOverlay = vrOverlay;
	m_overlayHandle = overlayHandle;

	m_initialized = true;
}

void HL2VRInterop::Shutdown()
{
	std::unique_lock lock(m_frameSyncMutex);
	m_initialized = false;

	m_vrSystem = nullptr;
	m_vrCompositor = nullptr;
	m_vrOverlay = nullptr;

	m_mat_colorTex = nullptr;
	m_mat_depthTex = nullptr;
	m_mat_hudTex = nullptr;
	m_vrsImage = nullptr;
	m_device = nullptr;
}

void HL2VRInterop::SetLoadingScreenMode(bool enable)
{
	m_loadingScreenModeEnabled = enable;
}

void HL2VRInterop::ResetRenderTextures(uint32_t width, uint32_t height, int msaa)
{
	m_renderWidth = width;
	m_renderHeight = height;
	m_msaa = msaa;
	if (msaa <= 1)
		m_msaa = 0;
}

void HL2VRInterop::Mat_AwaitFrame(uint64_t frameId)
{
	if (!m_initialized || m_loadingScreenModeEnabled)
		return;

	if (m_device == nullptr)
		return;

	if (m_mat_framePresented && m_submit_frameHandoffId < m_mat_frameAwaitedId)
	{
		// need to wait for the previous frame to be completed and handed off to the VR runtime before we can await the next frame
		std::unique_lock lock(m_frameSyncMutex);
		if (m_condFrameHandedOff.wait_for(lock, std::chrono::milliseconds(500), [this] { return m_submit_frameHandoffId >= m_mat_frameAwaitedId; }) == false)
		{
			Logger::warn("VR: previous frame not handed off, skipping WaitGetPoses");
			m_mat_frameAwaited = false;
			m_mat_framePresented = false;
			m_mat_frameAwaitedId = frameId;
			return;
		}
	}

	vr::TrackedDevicePose_t hmdPose, predictedHmdPose;
	m_vrCompositor->WaitGetPoses(&hmdPose, 1, &predictedHmdPose, 1);
	Mat_UpdateFoveationTexture();

	std::unique_lock lock(m_frameSyncMutex);

	m_mat_curHeadsetPose = hmdPose;
	m_mat_predictedHeadsetPose = predictedHmdPose;
	m_mat_colorTex = nullptr;
	m_mat_depthTex = nullptr;
	m_mat_hudTex = nullptr;

	m_mat_frameAwaitedId = frameId;
	m_mat_frameAwaited = true;
	m_mat_framePresented = false;
	m_condFrameAwaited.notify_one();
}

void HL2VRInterop::Mat_SetHeadsetRenderingPose(const vr::HmdMatrix34_t &pose)
{
	m_mat_headsetPoseForRendering = pose;
}

void HL2VRInterop::SyncFrameGetPoses(uint64_t frameId, vr::TrackedDevicePose_t& hmdPose, vr::TrackedDevicePose_t& predictedHmdPose)
{
	if (frameId == 0 || !m_initialized || m_loadingScreenModeEnabled)
	{
		return;
	}

	std::unique_lock lock(m_frameSyncMutex);

	if (m_mat_frameAwaitedId < frameId)
	{
		if (m_condFrameAwaited.wait_for(lock, std::chrono::milliseconds(500), [this, frameId] { return m_mat_frameAwaitedId >= frameId; }) == false)
		{
			Logger::warn("VR: waiting for previous frame WGP timed out");
		}
	}

	hmdPose = m_mat_curHeadsetPose;
	predictedHmdPose = m_mat_predictedHeadsetPose;
}

void HL2VRInterop::Mat_ModifyTextureCreationDetails(D3D9_COMMON_TEXTURE_DESC &desc)
{
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight)
	{
		desc.MultiSample = static_cast<D3DMULTISAMPLE_TYPE>(m_msaa);
	}
}

void HL2VRInterop::Mat_OnPrePresent(D3D9DeviceEx *device)
{
	if (!m_initialized)
		return;

	m_device = device;

	if (m_loadingScreenModeEnabled)
	{
		Com<IDirect3DSurface9> renderTarget;
		device->GetRenderTarget(0, &renderTarget);
		m_mat_hudTex = renderTarget;
		return;
	}

	if (!m_mat_frameAwaited)
		return;

	m_mat_framePresented = true;

	// transition our render textures to proper image layout before submitting to OpenVR
	if (m_mat_colorTex != nullptr) {
		Mat_ResolveAndTransitionTexture(m_mat_colorTex.ptr(), false);
	}
	if (m_mat_depthTex != nullptr) {
		Mat_ResolveAndTransitionTexture(m_mat_depthTex.ptr(), true);
	}
	if (m_mat_hudTex != nullptr) {
		Mat_ResolveAndTransitionTexture(m_mat_hudTex.ptr(), true);
	}
}

uint64_t HL2VRInterop::Mat_GetVRSubmissionInfo(Rc<DxvkImage>& vrColorImage, Rc<DxvkImage>& vrDepthImage, Rc<DxvkImage>& vrHudImage, vr::HmdMatrix34_t& vrHmdPose)
{
	vrColorImage = nullptr;
	vrDepthImage = nullptr;
	vrHudImage = nullptr;

	if (m_mat_colorTex != nullptr)
	{
		D3D9Surface* rt = static_cast<D3D9Surface*>(m_mat_colorTex.ptr());
		vrColorImage = rt->GetCommonTexture()->GetImage();
		if (vrColorImage->info().sampleCount > 1)
			vrColorImage = rt->GetCommonTexture()->GetResolveImage();
	}

	if (m_mat_depthTex != nullptr)
	{
		D3D9Surface* rt = static_cast<D3D9Surface*>(m_mat_depthTex.ptr());
		vrDepthImage = rt->GetCommonTexture()->GetImage();
		if (vrDepthImage->info().sampleCount > 1)
			vrDepthImage = rt->GetCommonTexture()->GetResolveImage();
	}

	if (m_mat_hudTex != nullptr)
	{
		D3D9Surface* rt = static_cast<D3D9Surface*>(m_mat_hudTex.ptr());
		vrHudImage = rt->GetCommonTexture()->GetImage();
		if (vrHudImage->info().sampleCount > 1)
			vrHudImage = rt->GetCommonTexture()->GetResolveImage();
	}

	vrHmdPose = m_mat_headsetPoseForRendering;

	return m_mat_frameAwaited ? m_mat_frameAwaitedId.load() : 0;
}

void HL2VRInterop::Mat_OnPostPresent(D3D9DeviceEx *device)
{
	m_mat_colorTex = nullptr;
	m_mat_depthTex = nullptr;
	m_mat_hudTex = nullptr;
	m_mat_frameAwaited = false;
}

void HL2VRInterop::Submit_PreSubmitCallback()
{
	if (!m_initialized || m_loadingScreenModeEnabled)
		return;

	uint64_t awaitedFrameId = m_mat_frameAwaitedId.load();
	if (awaitedFrameId > m_submit_frameSubmitId)
	{
		m_vrCompositor->SubmitExplicitTimingData();
		m_submit_frameSubmitId = awaitedFrameId;
	}
}

void HL2VRInterop::Submit_FillTextureData(Rc<DxvkImage> image, vr::VRVulkanTextureData_t &data)
{
	const auto& info = image->info();
	data.m_nHeight = info.extent.height;
	data.m_nWidth = info.extent.width;
	// VkPhysicalDevice
	auto dxvkDevice = m_device->GetDXVKDevice();
	data.m_pPhysicalDevice = dxvkDevice->adapter()->handle();
	// VkDevice
	data.m_pDevice = dxvkDevice->handle();
	// VkImage
	data.m_nImage = (uint64_t)image->handle();
	// VkInstance
	data.m_pInstance = dxvkDevice->instance()->vki()->instance();
	// VkQueue
	data.m_pQueue = dxvkDevice->queues().graphics.queueHandle;
	data.m_nQueueFamilyIndex = dxvkDevice->queues().graphics.queueFamily;
	data.m_nFormat = info.format;
	data.m_nSampleCount = info.sampleCount;
}

void HL2VRInterop::Submit_PrePresentCallBack(uint64_t vrFrameId, Rc<DxvkImage> vrColorImage, Rc<DxvkImage> vrDepthImage, Rc<DxvkImage> vrHudImage, float* vrHmdPose)
{
	if (!m_initialized)
		return;

	if (m_vrCompositor->CanRenderScene() && vrColorImage != nullptr && m_submit_frameSubmitId == vrFrameId && !m_loadingScreenModeEnabled) {
		static vr::VRTextureBounds_t leftBounds = {0.0f, 0.0f, 0.5f, 1.0f};
		static vr::VRTextureBounds_t rightBounds = {0.5f, 0.0f, 1.0f, 1.0f};

		vr::VRVulkanTextureData_t colorTexData, depthTexData;
		Submit_FillTextureData(vrColorImage, colorTexData);
		vr::VRTextureWithPoseAndDepth_t submitInfo;
		submitInfo.eType = vr::TextureType_Vulkan;
		submitInfo.eColorSpace = vr::ColorSpace_Auto;
		submitInfo.handle = (void*)&colorTexData;
		memcpy(&submitInfo.mDeviceToAbsoluteTracking.m[0][0], vrHmdPose, sizeof(submitInfo.mDeviceToAbsoluteTracking));

		int flags = vr::Submit_TextureWithPose;
		if (vrDepthImage != nullptr)
		{
			flags |= vr::Submit_TextureWithDepth;
			Submit_FillTextureData(vrDepthImage, depthTexData);
			submitInfo.depth.handle = (void*)&depthTexData;
			submitInfo.depth.mProjection = m_projectionLeft;
			submitInfo.depth.vRange.v[0] = 0;
			submitInfo.depth.vRange.v[1] = 1;
		}
		m_vrCompositor->Submit(vr::Eye_Left, &submitInfo, &leftBounds, (vr::EVRSubmitFlags)flags);

		if (vrDepthImage != nullptr)
			submitInfo.depth.mProjection = m_projectionRight;
		m_vrCompositor->Submit(vr::Eye_Right, &submitInfo, &rightBounds, (vr::EVRSubmitFlags)flags);
	}
	else
	{
		m_vrCompositor->ClearLastSubmittedFrame();
	}

	if (vrHudImage != nullptr && m_overlayHandle != 0)
	{
		vr::VRVulkanTextureData_t hudTexData;
		Submit_FillTextureData(vrHudImage, hudTexData);
		vr::Texture_t hudTexInfo;
		hudTexInfo.eType = vr::TextureType_Vulkan;
		hudTexInfo.handle = (void*)&hudTexData;
		hudTexInfo.eColorSpace = vr::ColorSpace_Auto;
		m_vrOverlay->SetOverlayTexture(m_overlayHandle, &hudTexInfo);
	}
}

void HL2VRInterop::Submit_PostPresentCallback(uint64_t vrFrameId)
{
	if (!m_initialized || vrFrameId == 0)
		return;

	if (m_submit_frameSubmitId == vrFrameId) // only do this if we submitted timing info
	{
		m_vrCompositor->PostPresentHandoff();
	}

	std::unique_lock lock(m_frameSyncMutex);
	m_submit_frameHandoffId = vrFrameId;
	m_condFrameHandedOff.notify_one();
}

void HL2VRInterop::Mat_OnSetRenderTarget(IDirect3DSurface9 *rt)
{
	if (!m_initialized || m_loadingScreenModeEnabled || rt == nullptr)
		return;

	D3DSURFACE_DESC desc;
	rt->GetDesc(&desc);
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight && m_mat_colorTex == nullptr)
	{
		m_mat_colorTex = rt;
	}
	else if (desc.Width == 1281 && desc.Height == 720 && m_mat_hudTex == nullptr)
	{
		m_mat_hudTex = rt;
	}

	UpdateFoveationMode(m_mat_colorTex == rt);
}

void HL2VRInterop::Mat_OnSetDepthStencil(IDirect3DSurface9 *depth)
{
	if (!m_initialized || m_loadingScreenModeEnabled || depth == nullptr)
		return;

	D3DSURFACE_DESC desc;
	depth->GetDesc(&desc);
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight && m_mat_depthTex == nullptr)
	{
		m_mat_depthTex = depth;
	}
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
	if (!m_initialized || !m_FoveatedRenderingEnabled || m_device == nullptr)
		return;

	if (shouldEnable)
		m_device->m_d3d9VRS.Enable();
	else
		m_device->m_d3d9VRS.Disable();
}

void HL2VRInterop::Mat_UpdateFoveationTexture()
{
	if (m_device == nullptr)
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

			if (m_vrsImage == nullptr || desiredVrsImageWidth != m_vrsImageWidth || desiredVrsImageHeight != m_vrsImageHeight)
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

			if (m_vrsImage == nullptr)
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

void HL2VRInterop::Mat_ResolveAndTransitionTexture(IDirect3DSurface9* texture, bool isDepth)
{
	auto *commonTex = static_cast<D3D9Surface*>(texture)->GetCommonTexture();
	VkImageAspectFlags aspect = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	VkImageSubresourceRange subresources = {
		aspect,
		0, commonTex->GetImage()->info().mipLevels,
		0, commonTex->GetImage()->info().numLayers
	  };
	VkImageSubresourceLayers subresLayers = {
		aspect,
		0,
		0,
		commonTex->GetImage()->info().numLayers
	};
	bool isMultisampled = commonTex->GetImage()->info().sampleCount > 1;
	auto image = commonTex->GetImage();
	auto resolveImage = isMultisampled ? commonTex->GetResolveImage() : image;
	auto resolveMode = isDepth ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_AVERAGE_BIT;
	m_device->EmitCs([image, resolveImage, subresources, subresLayers, resolveMode](DxvkContext* ctx)
	{
		if (image != resolveImage)
		{
			VkFormat format = image->info().format;

			VkImageResolve region;
			region.srcSubresource = subresLayers;
			region.srcOffset      = VkOffset3D { 0, 0, 0 };
			region.dstSubresource = subresLayers;
			region.dstOffset      = VkOffset3D { 0, 0, 0 };
			region.extent         = image->mipLevelExtent(subresLayers.mipLevel);
			ctx->resolveImage(resolveImage, image, region, format, resolveMode, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
		}

		ctx->transformImage(resolveImage, subresources, resolveImage->info().layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	});
}
}
