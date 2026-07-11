#include "hmdWrapper.h"

#include "HL2VRInterop.h"

IHL2VRInterop* dxvkGetHL2VRInterop()
{
	return dxvk::g_hl2vr;
}
