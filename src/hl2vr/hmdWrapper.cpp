#include "OpenVRDirectMode.h"

#include "hmdWrapper.h"

OpenVRDirectMode g_OpenVR;

OpenVRDirectMode *OpenVRDirectMode::Get() {
  return &g_OpenVR;
}


void DllExport dxvkInitOpenVR(vr::IVRCompositor *compositor) {
  g_OpenVR.Init(compositor);
}

void DllExport dxvkShutdownOpenVR() {
  g_OpenVR.Shutdown();
}

void DllExport dxvkSetRenderTextureSize(uint32_t width, uint32_t height, int msaa) {
  g_OpenVR.SetRenderTextureSize(width, height, msaa);
}

void DllExport dxvkStartFrame()
{
  g_OpenVR.StartFrame();
}

void DllExport dxvkEnableFoveatedRendering(bool enabled)
{
	g_OpenVR.EnableFoveatedRendering(enabled);
}

void DllExport dxvkSetFoveationParams(float centerLX, float centerLY, float centerRX, float centerRY, float radius1, float radius2, float radius3)
{
  g_OpenVR.SetFoveationParams(centerLX, centerLY, centerRX, centerRY, radius1, radius2, radius3);
}
