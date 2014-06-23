#pragma once

#define _WIN32_WINNT 0x0502
#define WINVER 0x0502
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// DLL-friendly declarations

// Retrieve the temperature (in degrees) of a Nvidia GPU. -1 if not available
int nw_nvidia_init();
DWORD hw_nvidia_gettemperature( DWORD dwGPUIndex );
DWORD hw_nvidia_DynamicPstateInfoEx( DWORD dwGPUIndex );
DWORD hw_nvidia_Pstate20 (DWORD dwGPUIndex);
DWORD hw_nvidia_memory (DWORD dwGPUIndex);
DWORD hw_nvidia_memory_prc (DWORD dwGPUIndex);
DWORD hw_nvidia_memory_util_prc( DWORD dwGPUIndex );
DWORD hw_nvidia_clock (DWORD dwGPUIndex);
DWORD hw_nvidia_clockMemory (DWORD dwGPUIndex);
DWORD hw_nvidia_voltage (DWORD dwGPUIndex);
DWORD hw_nvidia_cooler (DWORD dwGPUIndex);
DWORD hw_nvidia_fan (DWORD dwGPUIndex);
DWORD get_bus_id(DWORD dwGPUindex);
DWORD hw_nvidia_version();
DWORD get_bus_ids();