#pragma once

#define _WIN32_WINNT 0x0502
#define WINVER 0x0502
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// DLL-friendly declarations

// Retrieve the temperature (in degrees) of a Nvidia GPU. -1 if not available
int nw_nvidia_init();
DWORD hw_nvidia_gettemperature( DWORD dwGPUIndex );
unsigned int hw_nvidia_DynamicPstateInfoEx( DWORD dwGPUIndex );
unsigned int hw_nvidia_Pstate20 (DWORD dwGPUIndex);
unsigned int hw_nvidia_memory (DWORD dwGPUIndex);
unsigned int hw_nvidia_clock (DWORD dwGPUIndex);
unsigned int hw_nvidia_clockMemory (DWORD dwGPUIndex);
unsigned int hw_nvidia_voltage (DWORD dwGPUIndex);