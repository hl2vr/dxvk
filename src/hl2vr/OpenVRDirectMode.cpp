#include "../d3d9/d3d9_surface.h"
#include "OpenVRDirectMode.h"

#include "../d3d9/d3d9_device.h"
#include "../d3d9/d3d9_interfaces.h"

OpenVRDirectMode::OpenVRDirectMode() : 
	m_pCompositor(nullptr),
	m_nRenderWidth(0),
	m_nRenderHeight(0),
	m_initialised(false) {}

OpenVRDirectMode::~OpenVRDirectMode()
{
}


void OpenVRDirectMode::Init(vr::IVRCompositor *compositor)
{
  if (m_initialised)
    Shutdown();

  memset(&m_VulkanData, 0, sizeof(m_VulkanData));
  memset(&m_VRTexture, 0, sizeof(m_VRTexture));
  m_lastUsedDevice = nullptr;
  m_pCompositor = compositor;
  m_pCompositor->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
  m_initialised = true;
}

void OpenVRDirectMode::Shutdown() {
  AwaitPreviousFrame();
  m_pCompositor = nullptr;
  m_initialised = false;
  m_lastUsedDevice = nullptr;
}

void OpenVRDirectMode::SetRenderTextureSize(uint32_t width, uint32_t height, int msaa) {
  m_nRenderWidth = width;
  m_nRenderHeight = height;
  m_multiSamples = std::max(1, std::min(16, msaa));
  if (m_multiSamples == 1)
    m_multiSamples = 0;

  m_FoveationNeedsUpdate = true;
}

void OpenVRDirectMode::OnRenderTargetChanged(dxvk::D3D9DeviceEx* device, dxvk::D3D9Surface *rt) {
  // note: the device is locked as we are inside a device call, so we cannot use its public API here!

  D3DSURFACE_DESC desc;
  rt->GetDesc(&desc);

  if (desc.Width == m_nRenderWidth && desc.Height >= m_nRenderHeight) {
    m_d3d9Tex = rt->GetCommonTexture();
    m_activeDevice = nullptr;
    rt->GetDevice(&m_activeDevice);
    m_VulkanData.m_nHeight = desc.Height;
    m_VulkanData.m_nWidth = desc.Width;
    // VkPhysicalDevice
    auto dxvkDevice = device->GetDXVKDevice();
    m_VulkanData.m_pPhysicalDevice = dxvkDevice->adapter()->handle();
    // VkDevice
    m_VulkanData.m_pDevice = dxvkDevice->handle();
    // VkImage
    m_VulkanData.m_nImage = (uint64_t)rt->GetCommonTexture()->GetImage()->handle();
    // VkInstance
    m_VulkanData.m_pInstance = dxvkDevice->instance()->vki()->instance();
    // VkQueue
    m_VulkanData.m_pQueue = dxvkDevice->queues().graphics.queueHandle;
    m_VulkanData.m_nQueueFamilyIndex = dxvkDevice->queues().graphics.queueFamily;
    m_VulkanData.m_nFormat = VK_FORMAT_B8G8R8A8_UNORM;
    m_VulkanData.m_nSampleCount = m_multiSamples;

    if (m_multiSamples > 1) {
      // submitting multi-sampled textures to OpenVR seems to cause driver crashes with AMD under certain circumstances
      // so submit the resolved texture, instead. it should be resolved at the point it's submitted.
      m_VulkanData.m_nImage = (uint64_t)rt->GetCommonTexture()->GetResolveImage()->handle();
      m_VulkanData.m_nSampleCount = 1;
    }

    m_VRTexture.eType = vr::TextureType_Vulkan;
    m_VRTexture.eColorSpace = vr::ColorSpace_Auto;
    m_VRTexture.handle = &m_VulkanData;

    m_textureSet = true;
  }

  UpdateFoveationMode(m_d3d9Tex == rt->GetCommonTexture());
}

void OpenVRDirectMode::PrePresent(dxvk::D3D9DeviceEx *device)
{
  if (!m_initialised) {
    return;
  }

  m_submitCalled = true;
  m_lastUsedDevice = device;

  if (m_d3d9Tex != nullptr && m_textureSet) {
    // transition our render texture to proper image layout before submitting to OpenVR
    VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, m_d3d9Tex->GetImage()->info().mipLevels,
        0, m_d3d9Tex->GetImage()->info().numLayers
      };
    device->TransformImage(m_d3d9Tex, &subresources,
      m_d3d9Tex->GetImage()->info().layout,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }
}

void OpenVRDirectMode::PostPresent()
{
  m_d3d9Tex = nullptr;
}

void OpenVRDirectMode::PreSubmitCallback() {
  if (!m_timingInfoSubmitted && m_pCompositor) {
    m_pCompositor->SubmitExplicitTimingData();
    m_timingInfoSubmitted = true;
  }
}

void OpenVRDirectMode::PrePresentCallBack()
{
  if (!m_initialised) {
    return;
  }

  // Only do this if we know the PresentEx on the Device was called by the
  // client
  if (m_submitCalled && m_textureSet) {
    static vr::VRTextureBounds_t leftBounds = {0.0f, 0.0f, 0.5f, 1.0f};
    static vr::VRTextureBounds_t rightBounds = {0.5f, 0.0f, 1.0f, 1.0f};

    if (m_pCompositor && m_pCompositor->CanRenderScene()) {
      vr::EVRCompositorError error =
          m_pCompositor->Submit(vr::Eye_Left, &m_VRTexture, &leftBounds);
      if (error != vr::VRCompositorError_None) {}

      error = m_pCompositor->Submit(vr::Eye_Right, &m_VRTexture, &rightBounds);
      if (error != vr::VRCompositorError_None) {}
    }

    m_submitCalled = false;
    m_textureSet = false;
  }
}

void OpenVRDirectMode::PostPresentCallback()
{
  if (!m_initialised) {
    return;
  }

  if (m_pCompositor) {
    m_pCompositor->PostPresentHandoff();
  }

  if (m_frameRunning) {
    std::lock_guard lk(m_mutex);
    m_frameRunning = false;
    m_cv.notify_one();
  }
}

void OpenVRDirectMode::AwaitPreviousFrame() {
  if (!m_initialised || !m_pCompositor) {
    return;
  }

  if (m_lastUsedDevice) {
    // flush and synchronize with submission queue
    m_lastUsedDevice->Flush();
    m_lastUsedDevice->SynchronizeCsThread(dxvk::DxvkCsThread::SynchronizeAll);
  }

  if (m_frameRunning) {
    std::unique_lock lock (m_mutex);
    if (m_frameRunning) {
      if (m_cv.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout) {
        dxvk::Logger::warn("Waiting for previous frame completion timed out!");
      }
      m_frameRunning = false;
    }
  }
}

void OpenVRDirectMode::StartFrame()
{
  if (!m_initialised || !m_pCompositor) {
    return;
  }

  AwaitPreviousFrame();

  m_pCompositor->WaitGetPoses(nullptr, 0, nullptr, 0);

  std::lock_guard<std::mutex> lk(m_mutex);
  m_frameRunning = true;
  m_timingInfoSubmitted = false;

  UpdateFoveationTexture();
}

int OpenVRDirectMode::DetermineMSAA(uint32_t width, uint32_t height) {
  if (width == m_nRenderWidth && height == m_nRenderHeight) {
    return m_multiSamples;
  }
  return 0;
}

void OpenVRDirectMode::EnableFoveatedRendering(bool enabled) {
  m_FoveatedRenderingEnabled = enabled;
  m_FoveationNeedsUpdate = true;
}

void OpenVRDirectMode::SetFoveationParams(float centerX, float centerY, float radius1, float radius2) {
  m_FoveationCenterX = centerX;
  m_FoveationCenterY = centerY;
  m_FoveationRadius1 = radius1;
  m_FoveationRadius2 = radius2;
	m_FoveationNeedsUpdate = true;
}

void OpenVRDirectMode::UpdateFoveationMode(bool shouldEnable)
{
  if (!m_FoveatedRenderingEnabled || !m_vrsInterface)
    return;

  if (shouldEnable)
    m_vrsInterface->Enable();
  else
    m_vrsInterface->Disable();
}

void OpenVRDirectMode::UpdateFoveationTexture()
{
  if (!m_activeDevice)
    return;

  m_vrsInterface = nullptr;
  m_activeDevice->QueryInterface(__uuidof(ID3D9VRS), reinterpret_cast<void**>(&m_vrsInterface));
  if (!m_vrsInterface)
    return;

  if (m_FoveationNeedsUpdate)
  {
    m_FoveationNeedsUpdate = false;
    if (m_FoveatedRenderingEnabled)
    {
      VkExtent2D minTexelSize = m_vrsInterface->GetMinTexelSize();
      VkExtent2D maxTexelSize = m_vrsInterface->GetMaxTexelSize();
      VkExtent2D desiredTexelSize;
      desiredTexelSize.width = dxvk::clamp(32u, minTexelSize.width, maxTexelSize.height);
      desiredTexelSize.height = dxvk::clamp(32u, minTexelSize.height, maxTexelSize.height);

      UINT desiredVrsImageWidth = (m_VulkanData.m_nWidth + desiredTexelSize.width - 1) / desiredTexelSize.width;
      UINT desiredVrsImageHeight = (m_VulkanData.m_nHeight + desiredTexelSize.height - 1) / desiredTexelSize.height;

      if (!m_vrsImage || desiredVrsImageWidth != m_vrsImageWidth || desiredVrsImageHeight != m_vrsImageHeight)
      {
        m_vrsImageWidth = desiredVrsImageWidth;
        m_vrsImageHeight = desiredVrsImageHeight;
        m_vrsImage = nullptr;
        dxvk::Logger::info("Creating VRS image with dimensions: " + std::to_string(m_vrsImageWidth) + "x" + std::to_string(m_vrsImageHeight));
        HRESULT result = m_activeDevice->CreateTexture(m_vrsImageWidth, m_vrsImageHeight, 1, D3DUSAGE_DYNAMIC | D3DUSAGE_VRS, D3DFMT_A8, D3DPOOL_DEFAULT, &m_vrsImage, nullptr);
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
        BYTE* data = (BYTE*)lockedRect.pBits;
        UINT halfWidth = m_vrsImageWidth / 2;
        UINT centerX1 = halfWidth * m_FoveationCenterX;
        UINT centerX2 = centerX1 + halfWidth;
        UINT centerY = m_vrsImageHeight * m_FoveationCenterY;
        UINT radius1 = m_vrsImageHeight * 0.5f * m_FoveationRadius1;
        UINT radius2 = m_vrsImageHeight * 0.5f * m_FoveationRadius2;
        for (UINT y = 0; y < m_vrsImageHeight; ++y)
        {
          for (UINT x = 0; x < m_vrsImageWidth; ++x)
          {
            UINT distSqr1 = (x - centerX1) * (x - centerX1) + (y - centerY) * (y - centerY);
            UINT distSqr2 = (x - centerX2) * (x - centerX2) + (y - centerY) * (y - centerY);
            UINT distSqr = std::min(distSqr1, distSqr2);
            if (distSqr >= radius2 * radius2)
              data[y * lockedRect.Pitch + x] = 0x0A;
            else if (distSqr >= radius1 * radius1)
              data[y * lockedRect.Pitch + x] = 0x05;
            else
              data[y * lockedRect.Pitch + x] = 0;
          }
        }
        m_vrsImage->UnlockRect(0);
      }

      m_vrsInterface->SetShadingRateImage(m_vrsImage.ptr(), desiredTexelSize);
    }
    else
    {
      m_vrsInterface->Disable();
    }
  }
}
