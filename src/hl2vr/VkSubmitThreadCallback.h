#pragma once

#include "../dxvk/dxvk_image.h"

class VkSubmitThreadCallback {
public:
	virtual void PreSubmitCallback() = 0;
	virtual void PrePresentCallBack(dxvk::Rc<dxvk::DxvkImage> vrColorImage, dxvk::Rc<dxvk::DxvkImage> vrDepthImage, dxvk::Rc<dxvk::DxvkImage> vrHudImage, float* vrHmdPose) = 0;
	virtual void PostPresentCallback(uint64_t vrFrameId) = 0;
};
