#pragma once

#include "../dxvk/dxvk_image.h"

class VkSubmitThreadCallback {
public:
	virtual void PreSubmitCallback() = 0;
	virtual void PrePresentCallBack(dxvk::Rc<dxvk::DxvkImage> vrColorImage, dxvk::Rc<dxvk::DxvkImage> vrDepthImage) = 0;
	virtual void PostPresentCallback() = 0;
};
