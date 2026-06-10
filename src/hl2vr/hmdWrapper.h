#pragma once

#include "../../include/openvr/openvr.hpp"

#define DllExport __declspec(dllexport)
#define DllImport __declspec(dllimport)

// Initialize our OpenVR handling in dxvk by passing the Compositor interface
void DllExport dxvkInitOpenVR(vr::IVRCompositor *compositor);
// Shut down OpenVR handling in dxvk
void DllExport dxvkShutdownOpenVR();

// Signals to the dxvk side that we are going to create a new set of render textures with
// the given size and MSAA level.
void DllExport dxvkSetRenderTextureSize(uint32_t width, uint32_t height, int msaa);

// Called at the start of the frame to wait for OpenVR readiness (WaitGetPoses)
// must be done within DXVK since WaitGetPoses must be called from the render thread
void  DllExport dxvkStartFrame();

void DllExport dxvkEnableFoveatedRendering(bool enabled);

void DllExport dxvkSetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3);
