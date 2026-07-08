#include <windows.h>
#include <dxgi1_4.h>
#include <dxcore.h>

// MSVC's <wingdi.h> (via <windows.h>) defines "#define ERROR 0".
// We must undefine it here to prevent breaking the "LogLevel" enum inside "plat.h".
#ifdef ERROR
#undef ERROR
#endif

extern "C" {
#include "plat.h"
#include "aimdo-time.h"
}

// RESTORED: Definition of AimdoHipDeviceProp for AMD (HIP) compilation
#if defined(__HIP_PLATFORM_AMD__)
typedef union {
    struct {
        char name[256];
        char uuid[16];
        char luid[8];
    };
    void *ptr;
    unsigned long long ull;
    unsigned char bytes[4096];
} AimdoHipDeviceProp;
#endif

// Internal C++ variables for DXCore
IDXCoreAdapter* g_dxcore_adapter = NULL;
bool g_using_dxcore = false;
bool g_budget_available = false;

#ifndef M
#define M (1024 * 1024)
#endif

extern "C" bool aimdo_wddm_init(CUdevice dev)
{
    LUID cuda_luid;
    HRESULT hr;

    if (g_wddm_adapter) {
        g_wddm_adapter->Release();
        g_wddm_adapter = NULL;
    }
    if (g_dxcore_adapter) {
        g_dxcore_adapter->Release();
        g_dxcore_adapter = NULL;
    }
    g_using_dxcore = false;
    g_budget_available = false;

#if defined(__HIP_PLATFORM_AMD__)
    AimdoHipDeviceProp hip_props = {0};
    if (!g_device_get_properties || !CHECK_CU(g_device_get_properties(hip_props.bytes, dev))) {
        log(ERROR, "comfy-aimdo: failed to get HIP device properties\n");
        return false;
    }
    memcpy(&cuda_luid, hip_props.luid, sizeof(cuda_luid));
#else
    unsigned int node_mask;
    if (!CHECK_CU(cuDeviceGetLuid((char *)&cuda_luid, &node_mask, dev))) {
        log(ERROR, "comfy-aimdo: cuDeviceGetLuid failed\n");
        return false;
    }
#endif

    // Try DXCore (MCDM / compute-only)
    IDXCoreAdapterFactory* factory = NULL;
    IDXCoreAdapterList* list = NULL;
    hr = DXCoreCreateAdapterFactory(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        // Enumerate compute-only adapters using DXCore GUID attributes
        const GUID attributes[] = { DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE };
        hr = factory->CreateAdapterList(1, attributes, IID_PPV_ARGS(&list));
        if (SUCCEEDED(hr)) {
            uint32_t count = list->GetAdapterCount();
            for (uint32_t i = 0; i < count; ++i) {
                IDXCoreAdapter* adapter = NULL;
                if (SUCCEEDED(list->GetAdapter(i, IID_PPV_ARGS(&adapter)))) {
                    LUID luid;
                    // Using robust non-template GetProperty overload
                    if (SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::InstanceLuid, sizeof(luid), &luid))) {
                        if (luid.LowPart == cuda_luid.LowPart &&
                            luid.HighPart == cuda_luid.HighPart) {
                            
                            DXCoreAdapterMemoryBudgetNodeSegmentGroup nodeSegmentGroup = {};
                            nodeSegmentGroup.nodeIndex = 0;
                            nodeSegmentGroup.segmentGroup = DXCoreSegmentGroup::Local;

                            DXCoreAdapterMemoryBudget test = {};
                            // Using robust non-template QueryState overload
                            if (SUCCEEDED(adapter->QueryState(DXCoreAdapterState::AdapterMemoryBudget, 
                                                              sizeof(nodeSegmentGroup), &nodeSegmentGroup, 
                                                              sizeof(test), &test))) {
                                g_dxcore_adapter = adapter;
                                g_dxcore_adapter->AddRef();
                                g_using_dxcore = true;
                                g_budget_available = true;

                                char name[256] = {0};
                                adapter->GetProperty(DXCoreAdapterProperty::DriverDescription, sizeof(name), name);
                                log(INFO,
                                    "comfy-aimdo: DXCore adapter match: %s LUID=%08lx:%08lx\n",
                                    name,
                                    (unsigned long)cuda_luid.HighPart,
                                    (unsigned long)cuda_luid.LowPart);
                                adapter->Release();
                                list->Release();
                                factory->Release();
                                return true;
                            } else {
                                log(WARNING, "comfy-aimdo: DXCore budget query unsupported, trying WDDM\n");
                            }
                            adapter->Release();
                            break;
                        }
                    }
                    adapter->Release();
                }
            }
        }
        if (list) list->Release();
        factory->Release();
    }

    // Fallback to WDDM (DXGI)
    IDXGIFactory4* dxgiFactory = NULL;
    IDXGIAdapter1* adapter1 = NULL;
    // In C++, CreateDXGIFactory1 accepts REFIID by reference.
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)))) {
        log(ERROR, "comfy-aimdo: CreateDXGIFactory1 failed\n");
        return false;
    }

    for (UINT i = 0; dxgiFactory->EnumAdapters1(i, &adapter1) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter1->GetDesc1(&desc);
        if (desc.AdapterLuid.LowPart == cuda_luid.LowPart &&
            desc.AdapterLuid.HighPart == cuda_luid.HighPart) {
            if (SUCCEEDED(adapter1->QueryInterface(IID_PPV_ARGS(&g_wddm_adapter)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO test;
                if (SUCCEEDED(g_wddm_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &test))) {
                    char name[256];
                    if (!WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), NULL, NULL)) {
                        strcpy(name, "<unknown>");
                    }
                    log(INFO,
                        "comfy-aimdo: WDDM adapter match: %s LUID=%08lx:%08lx\n",
                        name,
                        (unsigned long)cuda_luid.HighPart,
                        (unsigned long)cuda_luid.LowPart);
                    g_budget_available = true;
                    adapter1->Release();
                    dxgiFactory->Release();
                    return true;
                } else {
                    log(WARNING, "comfy-aimdo: WDDM QueryVideoMemoryInfo failed\n");
                    g_wddm_adapter->Release();
                    g_wddm_adapter = NULL;
                }
            }
            adapter1->Release();
            break;
        }
        adapter1->Release();
    }
    dxgiFactory->Release();

    log(ERROR, "comfy-aimdo: initialization failed - no reliable budget source available\n");
    return false;
}

/* Apparently this is still too small for all common graphics VRAM spikes.
* However we can't pad too much on the smaller cards, and its not the end
* of the world if we page out a little bit because it will adapt and correct
* quickly.
*/

/* FIXME: This should be 0 if sysmem fallback is disabled by the user */
#define BUDGET_HEADROOM (512 * 1024 * 1024)
#define CUDA_BUDGET_HEADROOM (192 * 1024 * 1024)

extern "C" bool poll_budget_deficit(const char** prevailing_deficit_method)
{
    uint64_t now = GET_TICK();
    if (now - wddm_timestamp_last_check < 2000) {
        return true;
    }
    wddm_timestamp_last_check = now;
    total_vram_last_check = total_vram_usage;

    if (!g_budget_available) {
        log(ERROR, "comfy-aimdo: poll_budget_deficit called but no budget available\n");
        return false;
    }

    ssize_t deficit = 0;
    bool ok = false;

    if (g_using_dxcore && g_dxcore_adapter) {
        DXCoreAdapterMemoryBudgetNodeSegmentGroup nodeSegmentGroup = {};
        nodeSegmentGroup.nodeIndex = 0;
        nodeSegmentGroup.segmentGroup = DXCoreSegmentGroup::Local;

        DXCoreAdapterMemoryBudget budget = {};
        HRESULT hr = g_dxcore_adapter->QueryState(DXCoreAdapterState::AdapterMemoryBudget,
                                                   sizeof(nodeSegmentGroup), &nodeSegmentGroup,
                                                   sizeof(budget), &budget);
        if (SUCCEEDED(hr)) {
            deficit = (ssize_t)(total_vram_usage + BUDGET_HEADROOM) - (ssize_t)budget.budget;
            log(DEBUG,
                "%s: DXCore budget=%zu MB usage=%zu MB reservation=%zu MB available=%zu MB deficit=%zd MB\n",
                __func__,
                (size_t)(budget.budget / M),
                (size_t)(budget.currentUsage / M),
                (size_t)(budget.currentReservation / M),
                (size_t)(budget.availableForReservation / M),
                deficit / (ssize_t)M);
            *prevailing_deficit_method = "DXCore budget";
            ok = true;
        } else {
            log(ERROR, "comfy-aimdo: DXCore QueryState failed (0x%08lx)\n", hr);
            return false;
        }
    } else if (g_wddm_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info;
        if (SUCCEEDED(g_wddm_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            deficit = (ssize_t)(total_vram_usage + BUDGET_HEADROOM) - (ssize_t)info.Budget;
            log(DEBUG,
                "%s: WDDM budget=%zu MB usage=%zu MB reservation=%zu MB available=%zu MB deficit=%zd MB\n",
                __func__,
                (size_t)(info.Budget / M),
                (size_t)(info.CurrentUsage / M),
                (size_t)(info.CurrentReservation / M),
                (size_t)(info.AvailableForReservation / M),
                deficit / (ssize_t)M);
            *prevailing_deficit_method = "WDDM budget";
            ok = true;
        } else {
            log(ERROR, "comfy-aimdo: WDDM QueryVideoMemoryInfo failed\n");
            return false;
        }
    } else {
        log(ERROR, "comfy-aimdo: no adapter for budget query\n");
        return false;
    }
    
    size_t free_vram = 0, total_vram = 0;
    if (CHECK_CU(cuMemGetInfo(&free_vram, &total_vram))) {
        ssize_t deficit_cuda = (ssize_t)(CUDA_BUDGET_HEADROOM / 2) - (ssize_t)free_vram;
        
        log(DEBUG,
            "%s: cuMemGetInfo free=%zu MB total=%zu MB deficit_cuda=%zd MB\n",
            __func__, free_vram / M, total_vram / M, deficit_cuda / (ssize_t)M);
        
        if (deficit_cuda > deficit) {
            deficit = deficit_cuda;
            *prevailing_deficit_method = "cuMemGetInfo (Windows)";
        }
    }

    deficit_sync = deficit;
    return ok;
}

extern "C" void aimdo_wddm_cleanup()
{
    if (g_wddm_adapter) {
        g_wddm_adapter->Release();
        g_wddm_adapter = NULL;
    }
    if (g_dxcore_adapter) {
        g_dxcore_adapter->Release();
        g_dxcore_adapter = NULL;
    }
    g_using_dxcore = false;
    g_budget_available = false;
}