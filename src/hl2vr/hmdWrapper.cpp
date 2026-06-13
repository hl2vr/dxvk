#include "OpenVRDirectMode.h"

#include "hmdWrapper.h"

#include "HL2VRInterop.h"

IHL2VRInterop* dxvkGetHL2VRInterop()
{
	return dxvk::g_hl2vr;
}

OpenVRDirectMode g_OpenVR;

OpenVRDirectMode *OpenVRDirectMode::Get() {
  return &g_OpenVR;
}


void DllExport dxvkInitOpenVR(vr::IVRSystem* system, vr::IVRCompositor *compositor) {
  g_OpenVR.Init(system, compositor);
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

void DllExport dxvkSetZRange(float zNearL, float zFarL, float zNearR, float zFarR)
{
	g_OpenVR.SetZRange(zNearL, zFarL, zNearR, zFarR);
}
