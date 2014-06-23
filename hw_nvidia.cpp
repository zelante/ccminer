#include <stdio.h>
#include <stdlib.h>
#include "hw_nvidia.h"
#include "nvapi.h" // I'll let you fix this one, I haven't included the files here, but they can be downloaded from the Nvidia website

// Link with nvapi
#pragma comment( lib, "nvapi.lib" )

//
// **** Critical section helper ****
//
namespace
{
	class CriticalSection
	{
	public:
		__forceinline CriticalSection()
		{
			InitializeCriticalSection( &m_cs );
		}

		__forceinline void enter()
		{
			EnterCriticalSection( &m_cs );
		}

		__forceinline void leave()
		{
			LeaveCriticalSection( &m_cs );
		}
	private:
		CRITICAL_SECTION m_cs;
	};

	class CriticalSectionHolder
	{
	public:
		__forceinline CriticalSectionHolder( CriticalSection& rCS ) : m_rCS( rCS )
		{
			m_rCS.enter();
		}

		__forceinline ~CriticalSectionHolder()
		{
			m_rCS.leave();
		}

	private:
		CriticalSection& m_rCS;
		CriticalSectionHolder();
		CriticalSectionHolder( const CriticalSectionHolder& );
	};
}

static CriticalSection s_hw_nvidia_cs;

#define NVAPI_MAX_USAGES_PER_GPU  34

typedef int *(*NvAPI_QueryInterface_t)(unsigned int offset);
/*typedef int (*NvAPI_Initialize_t)();
typedef int (*_NvAPI_GPU_GetFullName_t)(int *hPhysicalGpu, char name[64]);
typedef int (*NvAPI_EnumPhysicalGPUs_t)(int **handles, int *count);*/
typedef int (*NvAPI_GPU_GetUsages_t)(int *handle, unsigned int *usages);
//typedef int (*NvAPI_GPU_GetMemoryInfo_t) (int *hPhysicalGpu, NV_DISPLAY_DRIVER_MEMORY_INFO *PMemoryInfo);
//typedef int (*NvAPI_GPU_GetMemoryInfo_t) (int *hPhysicalGpu, NV_DISPLAY_DRIVER_MEMORY_INFO *PMemoryInfo);
//typedef int (*NvAPI_GPU_GetCoolerSettings_t) (NvPhysicalGpuHandle hPhysicalGpu, NvU32 coolerIndex, NV_GPU_GETCOOLER_SETTINGS *pCoolerInfo);
typedef int (*NvAPI_GPU_GetCoolerSettings_t) (NvPhysicalGpuHandle hPhysicalGpu, NvU32 coolerIndex, NV_GPU_GETCOOLER_SETTINGS *pCoolerInfo);

static NvAPI_QueryInterface_t NvAPI_QueryInterface = NULL;
static NvAPI_GPU_GetUsages_t NvAPI_GPU_GetUsages = NULL;
static NvAPI_GPU_GetCoolerSettings_t NvAPI_GPU_GetCoolerSettings = NULL;


//
// **** hw_nvidia_gettemperature ****
//
// Retrieve the temperature (in degrees) of a Nvidia GPU. -1 if not available
// Thread safe, well as long as nvapi is thread safe ;)

NvPhysicalGpuHandle nvGPUHandles[NVAPI_MAX_PHYSICAL_GPUS];
NvU32 gpuCount = 0;

int nw_nvidia_init()
{
	HMODULE hmod = LoadLibraryA("nvapi.dll");
	if (hmod == NULL)
		return -1;
	
	NvAPI_QueryInterface = (NvAPI_QueryInterface_t) GetProcAddress(hmod, "nvapi_QueryInterface");
	NvAPI_GPU_GetUsages = (NvAPI_GPU_GetUsages_t) (*NvAPI_QueryInterface)(0x189A1FDF);
	NvAPI_GPU_GetCoolerSettings = (NvAPI_GPU_GetCoolerSettings_t) (*NvAPI_QueryInterface)(0xDA141340);
   
	if (NvAPI_GPU_GetUsages == NULL || NvAPI_GPU_GetCoolerSettings == NULL)
        return -1;

	static bool s_nvapi_initialized = false;
	CriticalSectionHolder csh( s_hw_nvidia_cs );
	if( !s_nvapi_initialized )
		if( NvAPI_Initialize() != NVAPI_OK )
			return -1;
		s_nvapi_initialized = true;
	if( NvAPI_EnumPhysicalGPUs( nvGPUHandles, &gpuCount ) != NVAPI_OK ) // !TODO: cache the table for drivers >= 105.00
		return -1;
	return 0;
}

DWORD hw_nvidia_gettemperature( DWORD dwGPUIndex )
{
	DWORD *busid;
	// Only initialize nvapi once
	static bool s_nvapi_initialized = false;
	// Array of physical GPU handle

	// Thermal settings
	NV_GPU_THERMAL_SETTINGS temperature;


	// Ensure the index is correct
	if( dwGPUIndex > gpuCount )
		return -1;

	/*for (int i = 0; i < gpuCount; i++)
	{
		if (NvAPI_GPU_GetBusId(nvGPUHandles[dwGPUIndex], busid) == NVAPI_OK )
			if (int(busid) - 1 == i)
			{*/
				// Retrive the temperature
				ZeroMemory( &temperature, sizeof( NV_GPU_THERMAL_SETTINGS ) );
				temperature.version = NV_GPU_THERMAL_SETTINGS_VER;
				if( NvAPI_GPU_GetThermalSettings( nvGPUHandles[ dwGPUIndex ], NVAPI_THERMAL_TARGET_ALL, &temperature ) != NVAPI_OK )
					return -1;

				if( temperature.count == 0 )
					return -1;
			/*}
	}*/
	return temperature.sensor[0].currentTemp;
}

DWORD hw_nvidia_DynamicPstateInfoEx( DWORD dwGPUIndex ) {
	NV_GPU_DYNAMIC_PSTATES_INFO_EX m_DynamicPStateInfo;
	ZeroMemory( &m_DynamicPStateInfo, sizeof( NV_GPU_DYNAMIC_PSTATES_INFO_EX ) );
	m_DynamicPStateInfo.version = NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER;

	// Ensure the index is correct
	if( dwGPUIndex > gpuCount )
		return -1;

	if (NvAPI_GPU_GetDynamicPstatesInfoEx( nvGPUHandles [dwGPUIndex], &m_DynamicPStateInfo) != NVAPI_OK)
		return -1;
	return m_DynamicPStateInfo.utilization[0].percentage;
}


DWORD hw_nvidia_memory_util_prc( DWORD dwGPUIndex ) {
	NV_GPU_DYNAMIC_PSTATES_INFO_EX m_DynamicPStateInfo;
	ZeroMemory( &m_DynamicPStateInfo, sizeof( NV_GPU_DYNAMIC_PSTATES_INFO_EX ) );
	m_DynamicPStateInfo.version = NV_GPU_DYNAMIC_PSTATES_INFO_EX_VER;

	// Ensure the index is correct
	if( dwGPUIndex > gpuCount )
		return -1;

	if (NvAPI_GPU_GetDynamicPstatesInfoEx( nvGPUHandles [dwGPUIndex], &m_DynamicPStateInfo) != NVAPI_OK)
		return -1;
	return m_DynamicPStateInfo.utilization[1].percentage;
}

DWORD hw_nvidia_Pstate20 (DWORD dwGPUIndex) {
	NV_GPU_PERF_PSTATES20_INFO m_PStateInfo;
	ZeroMemory( &m_PStateInfo, sizeof(NV_GPU_PERF_PSTATES20_INFO));
	m_PStateInfo.version = NV_GPU_PERF_PSTATES_INFO_VER2;

	// Ensure the index is correct
	if( dwGPUIndex > gpuCount )
		return -1;
	//if (NvAPI_GPU_GetPstates20( nvGPUHandles [dwGPUIndex], &m_PStateInfo) != NVAPI_OK)
	if (NvAPI_GPU_GetPstates20( nvGPUHandles [dwGPUIndex], &m_PStateInfo) != NVAPI_OK)
		return -1;
	return m_PStateInfo.pstates[0].clocks[0].data.single.freq_kHz;
}

DWORD hw_nvidia_memory (DWORD dwGPUIndex)
{
	NV_DISPLAY_DRIVER_MEMORY_INFO pMemoryInfo;
	ZeroMemory(&pMemoryInfo, sizeof(NV_DISPLAY_DRIVER_MEMORY_INFO));
	pMemoryInfo.version = NV_DISPLAY_DRIVER_MEMORY_INFO_VER_2;
	NvU32 usedMemory = 0, totalMemory = 0, availableMemory = 0;

	// Ensure the index is correct
	if( dwGPUIndex > gpuCount )
		return -1;	

	if (NvAPI_GPU_GetMemoryInfo( nvGPUHandles[dwGPUIndex], &pMemoryInfo) != NVAPI_OK)
		return -1;
	usedMemory = (pMemoryInfo.dedicatedVideoMemory-pMemoryInfo.curAvailableDedicatedVideoMemory)/1024;
	return usedMemory;
}

DWORD hw_nvidia_memory_prc (DWORD dwGPUIndex)
{
	NV_DISPLAY_DRIVER_MEMORY_INFO pMemoryInfo2;
	ZeroMemory(&pMemoryInfo2, sizeof(NV_DISPLAY_DRIVER_MEMORY_INFO));
	pMemoryInfo2.version = NV_DISPLAY_DRIVER_MEMORY_INFO_VER_2;
	NvU32 usedMemory = 0, totalMemory = 0, availableMemory = 0, usedMemoryPrc = 0;

	// Ensure the index is correct
	if( dwGPUIndex > gpuCount )
		return -1;	

	if (NvAPI_GPU_GetMemoryInfo( nvGPUHandles[dwGPUIndex], &pMemoryInfo2) != NVAPI_OK)
		return -1;
	totalMemory = pMemoryInfo2.dedicatedVideoMemory / 1024;
	availableMemory = pMemoryInfo2.curAvailableDedicatedVideoMemory / 1024;
	usedMemory = totalMemory - availableMemory;
	usedMemoryPrc = 100 * usedMemory / totalMemory;
	return usedMemoryPrc;
}

DWORD hw_nvidia_cooler (DWORD dwGPUIndex)
{
	NvU32 speed = 0;

	if( dwGPUIndex > gpuCount )
		return -1;
	if (NvAPI_GPU_GetTachReading ( nvGPUHandles[dwGPUIndex], &speed) != NVAPI_OK)
		return -1;
	return speed;
}

DWORD hw_nvidia_fan (DWORD dwGPUIndex)
{
	NV_GPU_GETCOOLER_SETTINGS PCoolerSettings;
	ZeroMemory(&PCoolerSettings, sizeof(NV_GPU_GETCOOLER_SETTINGS));
	PCoolerSettings.version = NV_GPU_GETCOOLER_SETTINGS_VER;
	if( dwGPUIndex > gpuCount )
		return -1;
	if ((*NvAPI_GPU_GetCoolerSettings)(nvGPUHandles[dwGPUIndex], 0, &PCoolerSettings) != NVAPI_OK)
		return -1;
	return PCoolerSettings.cooler[0].currentLevel;
}

extern int bus_ids[8];
extern int device_map[8];
DWORD get_bus_ids()
{
	NvU32 busid;
	for (int i=0; i < gpuCount; i++)
	{
		if ( NvAPI_GPU_GetBusId(nvGPUHandles[device_map[i]], &busid) != NVAPI_OK )
			return -1;
		bus_ids[i] = busid;
	}
	return 0;
}


DWORD get_bus_id(DWORD dwGPUindex)
{
	NvU32 busid;
	if ( NvAPI_GPU_GetBusId(nvGPUHandles[dwGPUindex], &busid) != NVAPI_OK )
		return -1;
	return busid;
}

DWORD hw_nvidia_clock (DWORD dwGPUIndex)
{
	NV_GPU_CLOCK_FREQUENCIES pClockInfo;
	ZeroMemory(&pClockInfo, sizeof(NV_GPU_CLOCK_FREQUENCIES));
	pClockInfo.version = NV_GPU_CLOCK_FREQUENCIES_VER_2;


	// Ensure the index is correct
	if( dwGPUIndex > gpuCount )
		return -1;

	if (NvAPI_GPU_GetAllClockFrequencies(nvGPUHandles[dwGPUIndex], &pClockInfo) != NVAPI_OK)
		return -1;

	return pClockInfo.domain[NVAPI_GPU_PUBLIC_CLOCK_GRAPHICS].frequency/1000;
}

DWORD hw_nvidia_clockMemory (DWORD dwGPUIndex)
{
	NV_GPU_CLOCK_FREQUENCIES pClockMemory;
	ZeroMemory(&pClockMemory, sizeof(NV_GPU_CLOCK_FREQUENCIES));
	pClockMemory.version = NV_GPU_CLOCK_FREQUENCIES_VER_2;

	// Ensure the index is correct
	if( dwGPUIndex > gpuCount )
		return -1;

	if (NvAPI_GPU_GetAllClockFrequencies(nvGPUHandles[dwGPUIndex], &pClockMemory) != NVAPI_OK)
		return -1;

	return pClockMemory.domain[NVAPI_GPU_PUBLIC_CLOCK_MEMORY].frequency/1000;
}

DWORD hw_nvidia_voltage (DWORD dwGPUIndex)
{
	NV_GPU_PERF_PSTATES20_INFO pPstatesInfo;
	ZeroMemory(&pPstatesInfo, sizeof(NV_GPU_PERF_PSTATES20_INFO));

	if( dwGPUIndex > gpuCount )
		return -1;

	if (NvAPI_GPU_GetPstates20(nvGPUHandles[dwGPUIndex], &pPstatesInfo) != NVAPI_OK)
		return -1;

	return pPstatesInfo.pstates[1].baseVoltages[0].volt_uV;
}


DWORD hw_nvidia_version ()
{
	NV_DISPLAY_DRIVER_VERSION version = {0}; 
	version.version = NV_DISPLAY_DRIVER_VERSION_VER; 
	if (NvAPI_GetDisplayDriverVersion (NVAPI_DEFAULT_HANDLE, & version) != NVAPI_OK) 
		return -1;
	return version.drvVersion;
}