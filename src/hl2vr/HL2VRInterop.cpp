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
	m_vrCompositor->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff);
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

	m_colorTex = nullptr;
	m_depthTex = nullptr;
	m_hudTex = nullptr;
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

void HL2VRInterop::StartFrame(const vr::HmdMatrix34_t& hmdPose)
{
	std::unique_lock lock(m_frameSyncMutex);
	if (!m_initialized || m_loadingScreenModeEnabled)
		return;

	// frame rendering potentially starts on the MatQueue thread; we need to make sure the previous frame is already
	// started, otherwise we could cause a race condition here
	while (m_frameRenderingStarted < m_frameStarted)
	{
		if (m_condFrameRenderStarted.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
		{
			Logger::warn("VR: previous frame has not started rendering");
			break;
		}
	}

	m_frameStarted = m_framePrepared.load();
	m_headsetPoseForRendering = hmdPose;
}

void HL2VRInterop::AwaitFrame(bool matQueueMode)
{
	std::unique_lock lock(m_frameSyncMutex);
	if (!m_initialized || m_loadingScreenModeEnabled)
		return;

	if (m_device == nullptr)
		return;

	uint64_t targetAwaitFrameId = m_framePrepared;
	if (matQueueMode && targetAwaitFrameId > 0)
		--targetAwaitFrameId;

	if (targetAwaitFrameId > 0 && m_frameCompleted < targetAwaitFrameId)
	{
		// still awaiting previous frame's WaitGetPoses call
		while (m_frameCompleted < targetAwaitFrameId)
		{
			if (m_condFramePresented.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
			{
				Logger::warn("VR: Awaiting previous frame present timed out");
				break;
			}
		}
	}

	vr::TrackedDevicePose_t curHmdPose, predictedHmdPose;
	m_vrCompositor->GetLastPoses(&curHmdPose, 1, &predictedHmdPose, 1);
	m_curHeadsetPose = curHmdPose;
	m_predictedHeadsetPose = predictedHmdPose;

	m_frameAwaited = m_frameCompleted.load();
	++m_framePrepared;

	m_condFrameAwaited.notify_one();
}

void HL2VRInterop::GetHeadsetPoses(vr::TrackedDevicePose_t& hmdPose, vr::TrackedDevicePose_t& predictedHmdPose)
{
	hmdPose = m_curHeadsetPose;
	predictedHmdPose = m_predictedHeadsetPose;
}

void HL2VRInterop::ModifyTextureCreationDetails(D3D9_COMMON_TEXTURE_DESC &desc)
{
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight)
	{
		desc.MultiSample = static_cast<D3DMULTISAMPLE_TYPE>(m_msaa);
	}
}

void HL2VRInterop::OnPrePresent(D3D9DeviceEx *device)
{
	if (!m_initialized)
		return;

	m_device = device;

	if (m_loadingScreenModeEnabled)
	{
		Com<IDirect3DSurface9> renderTarget;
		device->GetRenderTarget(0, &renderTarget);
		if (renderTarget != nullptr && m_vrOverlay && m_overlayHandle)
		{
			vr::VRVulkanTextureData_t vulkanTextureData;
			FillTextureData(static_cast<D3D9Surface*>(renderTarget.ptr())->GetCommonTexture()->GetImage(), vulkanTextureData);
			vr::Texture_t textureData;
			textureData.eType = vr::TextureType_Vulkan;
			textureData.eColorSpace = vr::ColorSpace_Auto;
			textureData.handle = (void*)&vulkanTextureData;

			m_vrOverlay->SetOverlayTexture(m_overlayHandle, &textureData);
		}
		return;
	}

	if (!m_initialized || !m_frameAwaited)
		return;

	// transition our render textures to proper image layout before submitting to OpenVR
	if (m_colorTex != nullptr) {
		ResolveAndTransitionTexture(m_colorTex.ptr(), false);
	}
	if (m_depthTex != nullptr) {
		ResolveAndTransitionTexture(m_depthTex.ptr(), true);
	}
	if (m_hudTex != nullptr) {
		ResolveAndTransitionTexture(m_hudTex.ptr(), true);
	}
}

uint64_t HL2VRInterop::GetVRSubmissionInfo(Rc<DxvkImage>& vrColorImage, Rc<DxvkImage>& vrDepthImage, Rc<DxvkImage>& vrHudImage, vr::HmdMatrix34_t& vrHmdPose)
{
	vrColorImage = nullptr;
	vrDepthImage = nullptr;
	vrHudImage = nullptr;

	if (m_colorTex != nullptr)
	{
		D3D9Surface* rt = static_cast<D3D9Surface*>(m_colorTex.ptr());
		vrColorImage = rt->GetCommonTexture()->GetImage();
		if (vrColorImage->info().sampleCount > 1)
			vrColorImage = rt->GetCommonTexture()->GetResolveImage();
	}

	if (m_depthTex != nullptr)
	{
		D3D9Surface* rt = static_cast<D3D9Surface*>(m_depthTex.ptr());
		vrDepthImage = rt->GetCommonTexture()->GetImage();
		if (vrDepthImage->info().sampleCount > 1)
			vrDepthImage = rt->GetCommonTexture()->GetResolveImage();
	}

	if (m_hudTex != nullptr)
	{
		D3D9Surface* rt = static_cast<D3D9Surface*>(m_hudTex.ptr());
		vrHudImage = rt->GetCommonTexture()->GetImage();
		if (vrHudImage->info().sampleCount > 1)
			vrHudImage = rt->GetCommonTexture()->GetResolveImage();
	}

	vrHmdPose = m_headsetPose;

	return m_frameRenderingStarted.load();
}

void HL2VRInterop::OnPostPresent(D3D9DeviceEx *device)
{
	m_colorTex = nullptr;
	m_depthTex = nullptr;
	m_hudTex = nullptr;
}

void HL2VRInterop::PreSubmitCallback()
{
	if (m_initialized && m_frameAwaited && !m_timingInfoSubmitted) {
		m_vrCompositor->SubmitExplicitTimingData();
		m_timingInfoSubmitted = true;
	}
}

void HL2VRInterop::FillTextureData(Rc<DxvkImage> image, vr::VRVulkanTextureData_t &data)
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

void HL2VRInterop::PrePresentCallBack(Rc<DxvkImage> vrColorImage, Rc<DxvkImage> vrDepthImage, Rc<DxvkImage> vrHudImage, float* vrHmdPose)
{
	if (!m_initialized || !m_timingInfoSubmitted || vrColorImage == nullptr)
		return;

	if (m_vrCompositor->CanRenderScene()) {
		static vr::VRTextureBounds_t leftBounds = {0.0f, 0.0f, 0.5f, 1.0f};
		static vr::VRTextureBounds_t rightBounds = {0.5f, 0.0f, 1.0f, 1.0f};

		vr::VRVulkanTextureData_t colorTexData, depthTexData;
		FillTextureData(vrColorImage, colorTexData);
		vr::VRTextureWithPoseAndDepth_t submitInfo;
		submitInfo.eType = vr::TextureType_Vulkan;
		submitInfo.eColorSpace = vr::ColorSpace_Auto;
		submitInfo.handle = (void*)&colorTexData;
		memcpy(&submitInfo.mDeviceToAbsoluteTracking.m[0][0], vrHmdPose, sizeof(submitInfo.mDeviceToAbsoluteTracking));

		int flags = vr::Submit_TextureWithPose;
		if (vrDepthImage != nullptr)
		{
			flags |= vr::Submit_TextureWithDepth;
			FillTextureData(vrDepthImage, depthTexData);
			submitInfo.depth.handle = (void*)&depthTexData;
			submitInfo.depth.mProjection = m_projectionLeft;
			submitInfo.depth.vRange.v[0] = 0;
			submitInfo.depth.vRange.v[1] = 1;
		}
		m_vrCompositor->Submit(vr::Eye_Left, &submitInfo, &leftBounds, (vr::EVRSubmitFlags)flags);

		if (vrDepthImage != nullptr)
			submitInfo.depth.mProjection = m_projectionRight;
		m_vrCompositor->Submit(vr::Eye_Right, &submitInfo, &rightBounds, (vr::EVRSubmitFlags)flags);

		if (vrHudImage != nullptr && m_overlayHandle != 0)
		{
			vr::VRVulkanTextureData_t hudTexData;
			FillTextureData(vrHudImage, hudTexData);
			vr::Texture_t hudTexInfo;
			hudTexInfo.eType = vr::TextureType_Vulkan;
			hudTexInfo.handle = (void*)&hudTexData;
			hudTexInfo.eColorSpace = vr::ColorSpace_Auto;
			m_vrOverlay->SetOverlayTexture(m_overlayHandle, &hudTexInfo);
		}
	}
}

void HL2VRInterop::PostPresentCallback(uint64_t vrFrameId)
{
	if (!m_initialized)
		return;

	if (m_frameAwaited < m_frameCompleted)
	{
		std::unique_lock lock(m_frameSyncMutex);
		m_condFrameAwaited.wait_for(lock, std::chrono::milliseconds(250), [this] { return m_frameAwaited >= m_frameCompleted; });
	}

	m_vrCompositor->WaitGetPoses(nullptr, 0, nullptr, 0);

	std::unique_lock lock(m_frameSyncMutex);
	m_timingInfoSubmitted = false;
	m_frameCompleted = vrFrameId;
	m_condFramePresented.notify_one();
}

void HL2VRInterop::OnSetRenderTarget(IDirect3DSurface9 *rt)
{
	if (!m_initialized || m_loadingScreenModeEnabled || rt == nullptr)
		return;

	D3DSURFACE_DESC desc;
	rt->GetDesc(&desc);
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight && m_colorTex == nullptr)
	{
		m_colorTex = rt;

		// on first set of our render texture, consider this the start of our frame rendering
		UpdateFoveationTexture();
		std::unique_lock lock(m_frameSyncMutex);
		m_frameRenderingStarted = m_frameStarted.load();
		m_headsetPose = m_headsetPoseForRendering;
		m_condFrameRenderStarted.notify_one();
	}
	else if (desc.Width == 1281 && desc.Height == 720 && m_hudTex == nullptr)
	{
		m_hudTex = rt;
	}

	UpdateFoveationMode(m_colorTex == rt);
}

void HL2VRInterop::OnSetDepthStencil(IDirect3DSurface9 *depth)
{
	if (!m_initialized || m_loadingScreenModeEnabled || depth == nullptr)
		return;

	D3DSURFACE_DESC desc;
	depth->GetDesc(&desc);
	if (desc.Width == m_renderWidth && desc.Height == m_renderHeight && m_depthTex == nullptr)
	{
		m_depthTex = depth;
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

void HL2VRInterop::UpdateFoveationTexture()
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

void HL2VRInterop::ResolveAndTransitionTexture(IDirect3DSurface9* texture, bool isDepth)
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
