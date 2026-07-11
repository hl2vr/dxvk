#pragma once

#include "IHL2VRInterop.h"

#define DllExport __declspec(dllexport)
#define DllImport __declspec(dllimport)

DllExport IHL2VRInterop* dxvkGetHL2VRInterop();
