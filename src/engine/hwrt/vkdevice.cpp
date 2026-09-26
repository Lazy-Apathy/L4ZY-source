// vkdevice.cpp: opens the Vulkan loader by hand and brings up a device that can
// share memory and semaphores with the GL context.
//
// Nothing in here is allowed to be fatal: a machine with no Vulkan, no interop
// extensions or no RT cores must keep running the vanilla GL client.

#include "engine.h"
#include "hwrt/hwrt.h"

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = NULL;
#define HWRT_VK_DEFINE(name) PFN_##name name = NULL;
HWRT_VK_GLOBAL_FUNCS(HWRT_VK_DEFINE)
HWRT_VK_INSTANCE_FUNCS(HWRT_VK_DEFINE)
HWRT_VK_DEVICE_FUNCS(HWRT_VK_DEFINE)
#undef HWRT_VK_DEFINE

hwrtdevice hwrtdev;
bool hwrtfailed = false;

const char *hwrtresultstr(VkResult r)
{
    switch(r)
    {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        default: return "unknown VkResult";
    }
}

void hwrtfail(const char *what, VkResult r)
{
    hwrtfailed = true;
    conoutf(CON_WARN, "hwrt: %s failed (%s)", what, hwrtresultstr(r));
}

void hwrtfail(const char *what)
{
    hwrtfailed = true;
    conoutf(CON_WARN, "hwrt: %s", what);
}

// A queue submit only reports a fault from earlier work on the *next* queue
// call, so throwing this result away blames the operation that happened to run
// afterwards instead of the one that faulted. Marking the layer failed also
// stops the frame path from submitting to a device that is already gone: the
// game then finishes starting on plain OpenGL instead of being killed inside
// the driver.
bool hwrtwaitidle(const char *what)
{
    if(!hwrtdev.device) return false;
    VkResult r = vkDeviceWaitIdle(hwrtdev.device);
    if(r == VK_SUCCESS) return true;
    hwrtfail(what, r);
    return false;
}

static void *vklib = NULL;

static bool loadvulkanlibrary()
{
    if(vkGetInstanceProcAddr) return true;
#ifdef WIN32
    const char *libname = "vulkan-1.dll";
#elif defined(__APPLE__)
    const char *libname = "libvulkan.1.dylib";
#else
    const char *libname = "libvulkan.so.1";
#endif
    vklib = SDL_LoadObject(libname);
    if(!vklib)
    {
        conoutf(CON_INIT, "hwrt: no Vulkan loader (%s), staying on OpenGL", libname);
        return false;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_LoadFunction(vklib, "vkGetInstanceProcAddr");
    if(!vkGetInstanceProcAddr)
    {
        hwrtfail("vkGetInstanceProcAddr missing from the Vulkan loader");
        SDL_UnloadObject(vklib);
        vklib = NULL;
        return false;
    }

    const char *missing = NULL;
#define HWRT_VK_LOADGLOBAL(name) \
    name = (PFN_##name)vkGetInstanceProcAddr(VK_NULL_HANDLE, #name); \
    if(!name) missing = #name;
    HWRT_VK_GLOBAL_FUNCS(HWRT_VK_LOADGLOBAL)
#undef HWRT_VK_LOADGLOBAL
    if(missing)
    {
        defformatstring(msg, "Vulkan loader is missing %s", missing);
        hwrtfail(msg);
        return false;
    }
    return true;
}

static void unloadvulkanlibrary()
{
    if(!vklib) return;
    SDL_UnloadObject(vklib);
    vklib = NULL;
    vkGetInstanceProcAddr = NULL;
#define HWRT_VK_CLEAR(name) name = NULL;
    HWRT_VK_GLOBAL_FUNCS(HWRT_VK_CLEAR)
    HWRT_VK_INSTANCE_FUNCS(HWRT_VK_CLEAR)
    HWRT_VK_DEVICE_FUNCS(HWRT_VK_CLEAR)
#undef HWRT_VK_CLEAR
}

static bool createinstance()
{
    // Vulkan 1.1 folds the external memory/semaphore capability queries into
    // core, 1.2 folds in what VK_KHR_acceleration_structure needs. Asking for
    // 1.2 on a 1.1-only loader is rejected, so ask for 1.2 and fall back.
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Sauerbraten";
    app.pEngineName = "cube2";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;

    const char *instexts[32];
    int ninst = hwrtdlaacollectinstanceexts(instexts, 32);
    if(ninst > 0)
    {
        info.enabledExtensionCount = uint32_t(ninst);
        info.ppEnabledExtensionNames = instexts;
    }

    VkResult r = vkCreateInstance(&info, NULL, &hwrtdev.instance);
    if(r == VK_ERROR_INCOMPATIBLE_DRIVER)
    {
        app.apiVersion = VK_API_VERSION_1_1;
        r = vkCreateInstance(&info, NULL, &hwrtdev.instance);
    }
    if(r != VK_SUCCESS)
    {
        hwrtfail("vkCreateInstance", r);
        return false;
    }

    const char *missing = NULL;
#define HWRT_VK_LOADINSTANCE(name) \
    name = (PFN_##name)vkGetInstanceProcAddr(hwrtdev.instance, #name); \
    if(!name) missing = #name;
    HWRT_VK_INSTANCE_FUNCS(HWRT_VK_LOADINSTANCE)
#undef HWRT_VK_LOADINSTANCE
    if(missing)
    {
        defformatstring(msg, "Vulkan instance is missing %s", missing);
        hwrtfail(msg);
        return false;
    }
    return true;
}

static bool hasdeviceext(const VkExtensionProperties *exts, uint32_t numexts, const char *name)
{
    loopi(int(numexts)) if(!strcmp(exts[i].extensionName, name)) return true;
    return false;
}

// Picks the physical device GL is already rendering on. On this class of machine
// (discrete NVIDIA next to an AMD/Intel iGPU) grabbing the wrong one produces
// allocations that import cleanly and then render nothing, so refuse instead.
static bool pickphysicaldevice(const uint8_t *gluuid)
{
    uint32_t numdevs = 0;
    HWRTCHECK(vkEnumeratePhysicalDevices(hwrtdev.instance, &numdevs, NULL), "vkEnumeratePhysicalDevices");
    if(!numdevs) { hwrtfail("no Vulkan physical devices"); return false; }

    VkPhysicalDevice *devs = new VkPhysicalDevice[numdevs];
    VkResult r = vkEnumeratePhysicalDevices(hwrtdev.instance, &numdevs, devs);
    if(r != VK_SUCCESS) { delete[] devs; hwrtfail("vkEnumeratePhysicalDevices", r); return false; }

    bool found = false;
    loopi(int(numdevs))
    {
        VkPhysicalDeviceIDProperties idprops = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &idprops;
        vkGetPhysicalDeviceProperties2(devs[i], &props);
        if(memcmp(idprops.deviceUUID, gluuid, VK_UUID_SIZE)) continue;

        hwrtdev.phys = devs[i];
        hwrtdev.apiversion = props.properties.apiVersion;
        hwrtdev.timestampperiod = props.properties.limits.timestampPeriod;
        copystring(hwrtdev.name, props.properties.deviceName, sizeof(hwrtdev.name));
        memcpy(hwrtdev.uuid, idprops.deviceUUID, VK_UUID_SIZE);
        found = true;
        break;
    }
    delete[] devs;

    if(!found)
    {
        hwrtfail("no Vulkan device matches the GPU OpenGL is running on");
        return false;
    }
    if(VK_API_VERSION_MAJOR(hwrtdev.apiversion) == 1 && VK_API_VERSION_MINOR(hwrtdev.apiversion) < 1)
    {
        hwrtfail("Vulkan 1.1 or greater is required for GL interop");
        return false;
    }
    return true;
}

static bool createdevice()
{
    uint32_t numexts = 0;
    HWRTCHECK(vkEnumerateDeviceExtensionProperties(hwrtdev.phys, NULL, &numexts, NULL), "vkEnumerateDeviceExtensionProperties");
    VkExtensionProperties *exts = new VkExtensionProperties[max(numexts, 1U)];
    VkResult r = vkEnumerateDeviceExtensionProperties(hwrtdev.phys, NULL, &numexts, exts);
    if(r != VK_SUCCESS) { delete[] exts; hwrtfail("vkEnumerateDeviceExtensionProperties", r); return false; }

    const char *enabled[32];
    int numenabled = 0;
#ifdef WIN32
    const char *memext = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
               *semext = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
#else
    const char *memext = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
               *semext = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
#endif
    if(!hasdeviceext(exts, numexts, memext) || !hasdeviceext(exts, numexts, semext))
    {
        delete[] exts;
        hwrtfail("driver has no external memory/semaphore support");
        return false;
    }
    enabled[numenabled++] = memext;
    enabled[numenabled++] = semext;

    // Ray tracing is optional at this stage: interop and the debug composite have
    // to work on hardware without RT cores so the layer can be developed and
    // profiled there.
    bool wantrt = VK_API_VERSION_MINOR(hwrtdev.apiversion) >= 2 &&
                  hasdeviceext(exts, numexts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                  hasdeviceext(exts, numexts, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                  hasdeviceext(exts, numexts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    const char *slexts[24];
    int nslexts = hwrtdlaacollectdeviceexts(slexts, 24);
    loopi(nslexts)
    {
        if(!hasdeviceext(exts, numexts, slexts[i]))
        {
            conoutf(CON_INIT, "hwrt: NGX device extension %s not present, skipping", slexts[i]);
            continue;
        }
        bool already = false;
        loopj(numenabled) if(!strcmp(enabled[j], slexts[i])) { already = true; break; }
        if(!already && numenabled < 32) enabled[numenabled++] = slexts[i];
    }
    delete[] exts;

    VkPhysicalDeviceRayQueryFeaturesKHR rqfeat = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asfeat = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    bool shaderint64 = false;
    if(wantrt)
    {
        asfeat.pNext = &rqfeat;
        v12.pNext = &asfeat;
        VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        query.pNext = &v12;
        vkGetPhysicalDeviceFeatures2(hwrtdev.phys, &query);
        wantrt = rqfeat.rayQuery && asfeat.accelerationStructure && v12.bufferDeviceAddress;
        shaderint64 = query.features.shaderInt64 != VK_FALSE;
    }
    if(wantrt)
    {
        const char *rtexts[3] = {
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
        };
        loopi(3)
        {
            bool already = false;
            loopj(numenabled) if(!strcmp(enabled[j], rtexts[i])) { already = true; break; }
            if(!already && numenabled < 32) enabled[numenabled++] = rtexts[i];
        }
        // Keep only what the acceleration structure and ray query paths need.
        memset(&v12, 0, sizeof(v12));
        v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        v12.bufferDeviceAddress = VK_TRUE;
        v12.descriptorIndexing = VK_TRUE;
        v12.pNext = &asfeat;
        memset(&asfeat, 0, sizeof(asfeat));
        asfeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        asfeat.accelerationStructure = VK_TRUE;
        asfeat.pNext = &rqfeat;
        memset(&rqfeat, 0, sizeof(rqfeat));
        rqfeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        rqfeat.rayQuery = VK_TRUE;
    }
    hwrtdev.rayquery = wantrt;

    uint32_t numfamilies = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(hwrtdev.phys, &numfamilies, NULL);
    if(!numfamilies) { hwrtfail("no Vulkan queue families"); return false; }
    VkQueueFamilyProperties *families = new VkQueueFamilyProperties[numfamilies];
    vkGetPhysicalDeviceQueueFamilyProperties(hwrtdev.phys, &numfamilies, families);
    int family = -1;
    uint32_t tsb = 0;
    uint32_t familyqcount = 1;
    loopi(int(numfamilies))
    {
        if(!(families[i].queueFlags&VK_QUEUE_COMPUTE_BIT)) continue;
        // A universal queue keeps acceleration structure builds on the same
        // timeline as the tracing dispatch later on.
        if(families[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)
        {
            family = i;
            tsb = families[i].timestampValidBits;
            familyqcount = families[i].queueCount;
            break;
        }
        if(family < 0)
        {
            family = i;
            tsb = families[i].timestampValidBits;
            familyqcount = families[i].queueCount;
        }
    }
    delete[] families;
    if(family < 0) { hwrtfail("no Vulkan compute queue"); return false; }
    hwrtdev.queuefamily = uint32_t(family);
    hwrtdev.timestampbits = tsb;

    uint32_t wantq = 1 + hwrtdlaaextraqueues();
    if(wantq > familyqcount) wantq = familyqcount;
    if(wantq < 1) wantq = 1;
    hwrtdev.queuecount = wantq;
    hwrtdev.slcomputeqindex = wantq > 1 ? 1 : 0;
    float qprio[2] = { 1.0f, 1.0f };
    VkDeviceQueueCreateInfo queueinfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueinfo.queueFamilyIndex = hwrtdev.queuefamily;
    queueinfo.queueCount = wantq;
    queueinfo.pQueuePriorities = qprio;

    VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueinfo;
    info.enabledExtensionCount = numenabled;
    info.ppEnabledExtensionNames = enabled;
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    bool want12 = wantrt || hwrtdlaawantsfeatures12();
    bool want13 = hwrtdlaawantsfeatures13() && VK_API_VERSION_MINOR(hwrtdev.apiversion) >= 3;
    if(want12)
    {
        if(!wantrt)
        {
            memset(&v12, 0, sizeof(v12));
            v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        }
        hwrtdlaamergefeatures12(&v12);
        if(want13)
        {
            hwrtdlaamergefeatures13(&v13);
            // Keep RT's asfeat chain: v12 -> asfeat -> rqfeat. Insert v13 at the end.
            if(wantrt)
            {
                rqfeat.pNext = &v13;
            }
            else v12.pNext = &v13;
        }
        features.pNext = &v12;
    }
    if(wantrt)
        features.features.shaderInt64 = shaderint64 ? VK_TRUE : VK_FALSE;
    info.pNext = &features;

    HWRTCHECK(vkCreateDevice(hwrtdev.phys, &info, NULL, &hwrtdev.device), "vkCreateDevice");

    const char *missing = NULL;
#define HWRT_VK_LOADDEVICE(name) \
    name = (PFN_##name)vkGetDeviceProcAddr(hwrtdev.device, #name); \
    if(!name) missing = #name;
    HWRT_VK_DEVICE_CORE_FUNCS(HWRT_VK_LOADDEVICE)
#undef HWRT_VK_LOADDEVICE
    if(missing)
    {
        defformatstring(msg, "Vulkan device is missing %s", missing);
        hwrtfail(msg);
        return false;
    }
    if(wantrt)
    {
        missing = NULL;
#define HWRT_VK_LOADAS(name) \
        name = (PFN_##name)vkGetDeviceProcAddr(hwrtdev.device, #name); \
        if(!name) missing = #name;
        HWRT_VK_AS_FUNCS(HWRT_VK_LOADAS)
#undef HWRT_VK_LOADAS
        if(!vkGetBufferDeviceAddress)
            vkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(hwrtdev.device, "vkGetBufferDeviceAddressKHR");
        if(vkGetBufferDeviceAddress && missing && !strcmp(missing, "vkGetBufferDeviceAddress")) missing = NULL;
        if(!vkGetBufferDeviceAddress) missing = "vkGetBufferDeviceAddress";
        if(missing)
        {
            defformatstring(msg, "Vulkan device is missing %s", missing);
            hwrtfail(msg);
            return false;
        }

        VkPhysicalDeviceAccelerationStructurePropertiesKHR asprops = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props2.pNext = &asprops;
        vkGetPhysicalDeviceProperties2(hwrtdev.phys, &props2);
        hwrtdev.scratchalign = asprops.minAccelerationStructureScratchOffsetAlignment;
        if(!hwrtdev.scratchalign) hwrtdev.scratchalign = 256;
    }

    vkGetDeviceQueue(hwrtdev.device, hwrtdev.queuefamily, 0, &hwrtdev.queue);
    vkGetPhysicalDeviceMemoryProperties(hwrtdev.phys, &hwrtdev.memprops);
    return true;
}

int hwrtfindmemtype(uint32_t typebits, VkMemoryPropertyFlags props)
{
    loopi(int(hwrtdev.memprops.memoryTypeCount))
    {
        if(!(typebits&(1U<<i))) continue;
        if((hwrtdev.memprops.memoryTypes[i].propertyFlags&props) == props) return i;
    }
    return -1;
}

bool hwrtinitdevice(const uint8_t *gluuid)
{
    if(hwrtdev.ok()) return true;
    memset(&hwrtdev, 0, sizeof(hwrtdev));

    if(!loadvulkanlibrary()) return false;
    hwrtdlaainitbeforeinstance();
    conoutf(CON_INIT, "hwrt: creating Vulkan instance");
    if(!createinstance() || !pickphysicaldevice(gluuid) || !createdevice())
    {
        hwrtdestroydevice();
        return false;
    }
    conoutf(CON_INIT, "hwrt: Vulkan device created, DLAA ondevice");
    hwrtdlaaondevice();

    conoutf(CON_INIT, "hwrt: Vulkan %d.%d on %s",
            VK_API_VERSION_MAJOR(hwrtdev.apiversion), VK_API_VERSION_MINOR(hwrtdev.apiversion), hwrtdev.name);
    conoutf(CON_INIT, "hwrt: ray query %s", hwrtdev.rayquery ? "supported" : "NOT supported (interop only)");
    return true;
}

void hwrtdestroydevice()
{
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    hwrtdlaacleanup();
    hwrtdlaashutdown();
    if(hwrtdev.device)
    {
        vkDestroyDevice(hwrtdev.device, NULL);
    }
    if(hwrtdev.instance) vkDestroyInstance(hwrtdev.instance, NULL);
    memset(&hwrtdev, 0, sizeof(hwrtdev));
    unloadvulkanlibrary();
}
