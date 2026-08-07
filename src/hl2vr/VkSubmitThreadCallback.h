#pragma once

#include "../dxvk/dxvk_image.h"

class VkSubmitThreadCallback {
public:
	virtual void Submit_PreSubmitCallback() = 0;
	virtual void Submit_PrePresentCallBack(uint64_t vrFrameId, dxvk::Rc<dxvk::DxvkImage> vrColorImage, dxvk::Rc<dxvk::DxvkImage> vrDepthImage, dxvk::Rc<dxvk::DxvkImage> vrHudImage, float* vrHmdPose) = 0;
	virtual void Submit_PostPresentCallback(uint64_t vrFrameId) = 0;
};

namespace dxvk
{
extern VkSubmitThreadCallback *g_pVkSubmitThreadCallback;
}
