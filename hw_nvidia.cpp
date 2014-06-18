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

//
// **** hw_nvidia_gettemperature ****
//
// Retrieve the temperature (in degrees) of a Nvidia GPU. -1 if not available
// Thread safe, well as long as nvapi is thread safe ;)

NvPhysicalGpuHandle nvGPUHandles[NVAPI_MAX_PHYSICAL_GPUS];
NvU32 gpuCount = 0;

int nw_nvidia_init()
{
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

unsigned int hw_nvidia_DynamicPstateInfoEx( DWORD dwGPUIndex ) {
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

unsigned int hw_nvidia_Pstate20 (DWORD dwGPUIndex) {
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

unsigned int hw_nvidia_memory (DWORD dwGPUIndex)
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

extern int bus_ids[8];
extern int device_map[8];
int get_bus_ids()
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


unsigned int get_bus_id(DWORD dwGPUindex)
{
	NvU32 busid;
	if ( NvAPI_GPU_GetBusId(nvGPUHandles[dwGPUindex], &busid) != NVAPI_OK )
		return -1;
	return busid;
}

unsigned int hw_nvidia_clock (DWORD dwGPUIndex)
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

unsigned int hw_nvidia_clockMemory (DWORD dwGPUIndex)
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

unsigned int hw_nvidia_voltage (DWORD dwGPUIndex)
{
	NV_GPU_PERF_PSTATES20_INFO pPstatesInfo;
	ZeroMemory(&pPstatesInfo, sizeof(NV_GPU_PERF_PSTATES20_INFO));

	if( dwGPUIndex > gpuCount )
		return -1;

	if (NvAPI_GPU_GetPstates20(nvGPUHandles[dwGPUIndex], &pPstatesInfo) != NVAPI_OK)
		return -1;

	return pPstatesInfo.pstates[1].baseVoltages[0].volt_uV;
}

