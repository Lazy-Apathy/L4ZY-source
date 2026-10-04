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
static uint32_t fsrdevfeatures = 0; // HWRT_FSR_DEV_* enabled on the device

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

// Names packed into log lines of a readable width (the console line limit cuts the rest).
static void hwrtvklist(const char *what, const char * const *names, int n)
{
    if(n <= 0) { conoutf(CON_INIT, "hwrt vk: %s: none", what); return; }
    char line[320];
    line[0] = 0;
    int part = 0;
    loopi(n)
    {
        if(line[0] && strlen(line) + strlen(names[i]) + 2 >= sizeof(line))
        {
            conoutf(CON_INIT, "hwrt vk: %s (%d)%s: %s", what, n, part ? " cont." : "", line);
            line[0] = 0;
            part++;
        }
        if(line[0]) concatstring(line, " ", sizeof(line));
        concatstring(line, names[i], sizeof(line));
    }
    conoutf(CON_INIT, "hwrt vk: %s (%d)%s: %s", what, n, part ? " cont." : "", line);
}

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
    if(hwrtsimulating("novulkan"))
    {
        conoutf(CON_INIT, "hwrt: no Vulkan loader (%s, simulated), staying on OpenGL", libname);
        hwrtunavailable(HWRT_WHY_NOVULKAN);
        return false;
    }
    vklib = SDL_LoadObject(libname);
    if(!vklib)
    {
        conoutf(CON_INIT, "hwrt: no Vulkan loader (%s), staying on OpenGL", libname);
        hwrtunavailable(HWRT_WHY_NOVULKAN);
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
        hwrtunavailable(HWRT_WHY_DRIVER);
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
    hwrtvklist("instance extensions requested", instexts, ninst);
    if(hwrtsimulating("hanglate"))
    {
        // A driver call that never returns: the thread cannot notice it was given up.
        conoutf(CON_WARN, "hwrt vk: SAUER_HWRT_SIMULATE=hanglate, vkCreateInstance never returns (diagnostic)");
        for(;;) SDL_Delay(1000);
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
        // the loader is there but no installed driver answers for this machine
        if(r == VK_ERROR_INCOMPATIBLE_DRIVER || r == VK_ERROR_INITIALIZATION_FAILED) hwrtunavailable(HWRT_WHY_NOVULKAN);
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
    if(!numdevs) { hwrtfail("no Vulkan physical devices"); hwrtunavailable(HWRT_WHY_NOVULKAN); return false; }

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
        bool match = !found && !memcmp(idprops.deviceUUID, gluuid, VK_UUID_SIZE);
        conoutf(CON_INIT, "hwrt vk: GPU %d: %s (vendor 0x%04X, Vulkan %d.%d)%s", i, props.properties.deviceName,
                props.properties.vendorID, VK_API_VERSION_MAJOR(props.properties.apiVersion), VK_API_VERSION_MINOR(props.properties.apiVersion),
                match ? " <- the GPU OpenGL runs on, chosen" : "");
        if(!match) continue;

        hwrtdev.phys = devs[i];
        hwrtdev.apiversion = props.properties.apiVersion;
        hwrtdev.timestampperiod = props.properties.limits.timestampPeriod;
        copystring(hwrtdev.name, props.properties.deviceName, sizeof(hwrtdev.name));
        memcpy(hwrtdev.uuid, idprops.deviceUUID, VK_UUID_SIZE);
        found = true;
    }
    delete[] devs;

    if(!found)
    {
        hwrtfail("no Vulkan device matches the GPU OpenGL is running on");
        hwrtunavailable(HWRT_WHY_NOMATCH);
        return false;
    }
    if(VK_API_VERSION_MAJOR(hwrtdev.apiversion) == 1 && VK_API_VERSION_MINOR(hwrtdev.apiversion) < 1)
    {
        hwrtfail("Vulkan 1.1 or greater is required for GL interop");
        hwrtunavailable(HWRT_WHY_DRIVER);
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
        hwrtunavailable(HWRT_WHY_DRIVER);
        return false;
    }
    enabled[numenabled++] = memext;
    enabled[numenabled++] = semext;

    // Ray tracing is optional at this stage: interop and the debug composite have
    // to work on hardware without RT cores so the layer can be developed and
    // profiled there.
    bool rtexts = hasdeviceext(exts, numexts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                  hasdeviceext(exts, numexts, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                  hasdeviceext(exts, numexts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    bool wantrt = VK_API_VERSION_MINOR(hwrtdev.apiversion) >= 2 && rtexts;
    if(hwrtsimulating("nort"))
    {
        conoutf(CON_INIT, "hwrt: ray query hidden (SAUER_HWRT_SIMULATE=nort)");
        wantrt = rtexts = false;
    }
    // RT extensions on a Vulkan 1.1 driver: the card can, the driver is behind
    if(!wantrt) hwrtunavailable(rtexts ? HWRT_WHY_DRIVER : HWRT_WHY_NORT);

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
    bool shaderint64 = false, samaniso = false;
    hwrtdev.maxaniso = 0;
    if(wantrt)
    {
        asfeat.pNext = &rqfeat;
        v12.pNext = &asfeat;
        VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        query.pNext = &v12;
        vkGetPhysicalDeviceFeatures2(hwrtdev.phys, &query);
        wantrt = rqfeat.rayQuery && asfeat.accelerationStructure && v12.bufferDeviceAddress;
        shaderint64 = query.features.shaderInt64 != VK_FALSE;
        samaniso = query.features.samplerAnisotropy != VK_FALSE;
        if(!wantrt) hwrtunavailable(HWRT_WHY_NORT);
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

    // FSR 3.1 (bin64sr): its shaders read and write storage images without a
    // format qualifier; the half-precision variants also use float16/int16.
    // Only when the gateway is there, only what the GPU has, and the gateway is
    // told exactly what is on so it never picks a variant needing more.
    VkBool32 fsrreadnofmt = VK_FALSE, fsrwritenofmt = VK_FALSE, fsrint16 = VK_FALSE;
    bool fsrfp16 = false;
    fsrdevfeatures = 0;
    if(hwrtfsrgatewayloaded())
    {
        VkPhysicalDeviceVulkan12Features q12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        bool have12 = VK_API_VERSION_MINOR(hwrtdev.apiversion) >= 2;
        if(have12) query.pNext = &q12;
        vkGetPhysicalDeviceFeatures2(hwrtdev.phys, &query);
        if(query.features.shaderStorageImageReadWithoutFormat && query.features.shaderStorageImageWriteWithoutFormat)
        {
            fsrreadnofmt = fsrwritenofmt = VK_TRUE;
            fsrdevfeatures |= HWRT_FSR_DEV_FORMATLESS;
        }
        if(have12 && q12.shaderFloat16 && query.features.shaderInt16)
        {
            fsrint16 = VK_TRUE;
            fsrfp16 = true;
            fsrdevfeatures |= HWRT_FSR_DEV_FP16;
        }
    }

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
    hwrtvklist("device extensions requested", enabled, numenabled);
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    bool want12 = wantrt || hwrtdlaawantsfeatures12() || fsrfp16;
    bool want13 = hwrtdlaawantsfeatures13() && VK_API_VERSION_MINOR(hwrtdev.apiversion) >= 3;
    if(want12)
    {
        if(!wantrt)
        {
            memset(&v12, 0, sizeof(v12));
            v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        }
        hwrtdlaamergefeatures12(&v12);
        if(fsrfp16) v12.shaderFloat16 = VK_TRUE;
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
    // Anisotropic filtering of the RT world diffuse (hwrtdiffmip 2, hwrtdiffaniso).
    // Only a sampler that asks for it filters differently.
    if(wantrt && samaniso)
    {
        features.features.samplerAnisotropy = VK_TRUE;
        VkPhysicalDeviceProperties2 aprops = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        vkGetPhysicalDeviceProperties2(hwrtdev.phys, &aprops);
        hwrtdev.maxaniso = aprops.properties.limits.maxSamplerAnisotropy;
    }
    features.features.shaderStorageImageReadWithoutFormat = fsrreadnofmt;
    features.features.shaderStorageImageWriteWithoutFormat = fsrwritenofmt;
    features.features.shaderInt16 = fsrint16;
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

// Runs on the start-up worker thread (vkworker below), never on the main
// thread: nothing in here touches GL, and every step is timed and named so a
// report shows which driver call did not come back. On failure the main thread
// tears down (hwrtdestroytrace/hwrtdestroydevice); once the main thread has
// given up waiting, every step after the stuck one is skipped.
static const char *volatile vkcurstep = "";
static SDL_atomic_t vkgivenup;

static bool vkstep(const char *label, bool (*fn)(const uint8_t *), const uint8_t *arg)
{
    vkcurstep = label;
    Uint32 t = SDL_GetTicks();
    conoutf(CON_INIT, "hwrt vk: [%u ms] %s...", t, label);
    bool ok = fn(arg);
    Uint32 now = SDL_GetTicks();
    conoutf(ok ? CON_INIT : CON_WARN, "hwrt vk: [%u ms] %s %s (%u ms)", now, label, ok ? "done" : "FAILED", now - t);
    if(SDL_AtomicGet(&vkgivenup))
    {
        conoutf(CON_WARN, "hwrt vk: [%u ms] %s came back after the game stopped waiting; nothing more is done, Vulkan stays off", now, label);
        return false;
    }
    return ok;
}

static bool steploader(const uint8_t *) { return loadvulkanlibrary(); }
static bool steplayers(const uint8_t *)
{
    // The loader lists explicit and implicit layers alike; implicit ones
    // (overlays, capture and monitoring tools, the driver's own) are loaded
    // into every instance. The probe log also says where each comes from.
    PFN_vkEnumerateInstanceLayerProperties enumlayers =
        (PFN_vkEnumerateInstanceLayerProperties)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    PFN_vkEnumerateInstanceVersion enumversion =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t loaderversion = VK_API_VERSION_1_0;
    if(enumversion) enumversion(&loaderversion);
    conoutf(CON_INIT, "hwrt vk: loader Vulkan %d.%d.%d", VK_API_VERSION_MAJOR(loaderversion), VK_API_VERSION_MINOR(loaderversion), VK_API_VERSION_PATCH(loaderversion));
    if(!enumlayers) return true;
    uint32_t n = 0;
    enumlayers(&n, NULL);
    VkLayerProperties *layers = new VkLayerProperties[max(n, 1U)];
    if(enumlayers(&n, layers) != VK_SUCCESS) n = 0;
    const char *names[64];
    int nn = 0;
    loopi(int(n)) if(nn < 64) names[nn++] = layers[i].layerName;
    hwrtvklist("Vulkan layers", names, nn);
    delete[] layers;
    return true;
}
static bool stepngxgateway(const uint8_t *) { hwrtdlaainitbeforeinstance(); return true; }
static bool stepfsrgateway(const uint8_t *) { hwrtfsrloadgateway(); return true; }
static bool stepinstance(const uint8_t *) { return createinstance(); }
static bool steppick(const uint8_t *uuid) { return pickphysicaldevice(uuid); }
static bool stepdevice(const uint8_t *) { return createdevice(); }
static bool stepngx(const uint8_t *) { hwrtdlaaondevice(); return true; }
static bool stepfsr(const uint8_t *) { hwrtfsrondevice(fsrdevfeatures); return true; }
static bool steptrace(const uint8_t *) { return hwrtinittrace(); }

bool hwrtinitdevice(const uint8_t *gluuid)
{
    if(hwrtdev.ok()) return true;
    memset(&hwrtdev, 0, sizeof(hwrtdev));

    if(!vkstep("Vulkan loader", steploader, NULL)) return false;
    if(!vkstep("Vulkan layers", steplayers, NULL)) return false;
    if(!vkstep("NGX gateway", stepngxgateway, NULL)) return false;
    if(!vkstep("FSR gateway", stepfsrgateway, NULL)) return false;
    conoutf(CON_INIT, "hwrt: creating Vulkan instance");
    if(!vkstep("vkCreateInstance", stepinstance, NULL)) return false;
    if(!vkstep("GPU list", steppick, gluuid)) return false;
    if(!vkstep("vkCreateDevice", stepdevice, NULL)) return false;
    conoutf(CON_INIT, "hwrt: Vulkan device created, DLAA ondevice");
    if(!vkstep("NGX init", stepngx, NULL)) return false;
    if(!vkstep("FSR init", stepfsr, NULL)) return false;
    if(!vkstep("trace pipelines", steptrace, NULL)) return false;
    vkcurstep = "";

    conoutf(CON_INIT, "hwrt: Vulkan %d.%d on %s",
            VK_API_VERSION_MAJOR(hwrtdev.apiversion), VK_API_VERSION_MINOR(hwrtdev.apiversion), hwrtdev.name);
    conoutf(CON_INIT, "hwrt: ray query %s", hwrtdev.rayquery ? "supported" : "NOT supported (interop only)");
    return true;
}

// ---- Deferred, bounded Vulkan start-up -----------------------------------
//
// vkCreateInstance loads every Vulkan driver (ICD) and every implicit layer
// installed on the machine: overlays, capture and monitoring tools, the
// driver's own. One of them hanging or crashing used to freeze or close the
// game at every launch, even for players who never use ray tracing, because the
// device was created in gl_init only to know whether to grey the RT option.
//
//  1. gl_init (hwrtinit) only checks the GL side: interop extensions, UUID.
//  2. After config.cfg and autoexec.cfg (hwrtprefsloaded), Vulkan is wanted
//     only for hwrt 1 or an AA mode that needs it (DLAA, DLSS, FSR). Then a
//     probe process, "sauerbraten.exe -vkprobe <GL device UUID>" without a
//     window, creates an instance and a device and reports what it saw. It can
//     be killed: a hang or a crash there costs the timeout and nothing else.
//     Only if it answered does the game create its own device, on a worker
//     thread the main thread waits for with the same timeout (hwrtvkensure).
//  3. Nothing wanted (classic lighting, Native AA): no Vulkan in the game at
//     all; the probe runs a few seconds later in the background so the menus
//     grey RT/DLSS/FSR with the right reason. Choosing RT, DLAA, DLSS or FSR
//     starts Vulkan then, with the same bounded wait.
//  4. A worker that timed out cannot be stopped (it is inside the driver, maybe
//     holding the loader lock): Vulkan stays off for this run, nothing touches
//     its objects, and quitting ends the process with TerminateProcess so a lock
//     it holds cannot freeze the exit. hwrtvkstall 1 is saved: the next launch
//     does not start Vulkan by itself until the player picks RT/DLAA/DLSS/FSR.

// Seconds the probe, and then the game's own start-up, may take.
VARP(hwrtvktimeout, 3, 15, 120);
// 1 = Vulkan did not answer (or crashed) during an earlier start: do not start
// it automatically; choosing RT or a Vulkan AA mode in Graphics retries.
VARP(hwrtvkstall, 0, 0, 1);

int hwrtvkstate = HWRT_VK_IDLE;
hwrtvkproberesult hwrtvkprobe;
bool hwrtvkabandoned = false;
static int vkstallwhy = HWRT_WHY_NONE;
static string vkstallwhat = "";
static uint8_t vkgluuid[VK_UUID_SIZE];
static bool vkhaveuuid = false;
static Uint32 vkprobeat = 0;
static SDL_atomic_t vkdone, vkok;

extern bool hwrtvkonready();
extern void hwrtvksetwhy(int why);

int hwrtvktimeoutsecs() { return hwrtvktimeout; }
bool hwrtvkready() { return hwrtvkstate == HWRT_VK_READY && !hwrtvkabandoned; }
bool hwrtvkchecking() { return hwrtvkstate == HWRT_VK_IDLE || hwrtvkstate == HWRT_VK_PROBING; }
bool hwrtvkcanstart()
{
    if(hwrtvkabandoned) return false;
    switch(hwrtvkstate)
    {
        case HWRT_VK_IDLE: case HWRT_VK_PROBING: case HWRT_VK_READY: return true;
        case HWRT_VK_PROBED: return hwrtvkprobe.why == HWRT_WHY_NONE;
        default: return false;
    }
}
bool hwrtvkworkerstuck() { return hwrtvkabandoned && !SDL_AtomicGet(&vkdone); }

// "the AMD graphics driver (amdxc64.dll)" once a probe crash was traced to a
// Vulkan driver's files, "" otherwise.
static char vkfaultdesc[160] = "";
// Vendor of the GPU OpenGL runs on (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel, 0 unknown).
static int vkglvendor = 0;

const char *hwrtvkstallreason()
{
    static string msg;
    if(hwrtvkstate != HWRT_VK_TIMEOUT) return "";
    // Short enough for one menu line; the step it stopped in is in the log (vkgiveup).
    if(vkstallwhy == HWRT_WHY_CRASH && vkfaultdesc[0])
        formatstring(msg, "Vulkan crashed in %s; updating that driver may help (see log.txt).", vkfaultdesc);
    else if(vkstallwhy == HWRT_WHY_CRASH)
        copystring(msg, "Vulkan crashed while starting (an overlay, a capture tool or the graphics driver; see log.txt).");
    else
        formatstring(msg, "Vulkan did not answer within %d s (an overlay, a capture tool or the graphics driver; see log.txt).", int(hwrtvktimeout));
    return msg;
}

void hwrtvksetuuid(const uint8_t *uuid)
{
    memcpy(vkgluuid, uuid, VK_UUID_SIZE);
    vkhaveuuid = true;
    // Called from hwrtinit with the GL context current.
    const char *v = (const char *)glGetString(GL_VENDOR);
    if(!v) vkglvendor = 0;
    else if(strstr(v, "NVIDIA")) vkglvendor = 0x10DE;
    else if(strstr(v, "AMD") || strstr(v, "ATI")) vkglvendor = 0x1002;
    else if(strstr(v, "Intel")) vkglvendor = 0x8086;
    else vkglvendor = 0;
}

void hwrtvkschedule(int delayms)
{
    if(hwrtvkstate != HWRT_VK_IDLE) return;
    vkprobeat = SDL_GetTicks() + Uint32(max(delayms, 1));
    if(!vkprobeat) vkprobeat = 1;
}

static void vkgiveup(int why, const char *what)
{
    hwrtvkstate = HWRT_VK_TIMEOUT;
    vkstallwhy = why;
    copystring(vkstallwhat, what);
    hwrtvksetwhy(why);
    hwrtvkstall = 1;
    conoutf(CON_WARN, "hwrt vk: [%u ms] Vulkan given up (%s): %s", SDL_GetTicks(), vkstallwhat, hwrtvkstallreason());
    conoutf(CON_WARN, "hwrt vk: classic lighting and Native AA are kept; hwrtvkstall 1 is saved so the next start does not wait for Vulkan (choosing RT, DLAA, DLSS or FSR in Graphics retries)");
    conoutf(CON_WARN, "Ray tracing and DLAA/DLSS/FSR are off: %s Choose them again in Graphics to retry.", hwrtvkstallreason());
}

static void vkpump()
{
    SDL_PumpEvents();   // keeps the window answering Windows while we wait
    conflushthreaded();
    SDL_Delay(5);
}

#ifdef WIN32
// ---- Vulkan drivers (ICDs), and skipping the one that crashed -------------
//
// The loader loads the driver of every GPU in the machine as soon as an
// instance is asked for (vkEnumerateInstanceExtensionProperties,
// vkCreateInstance, vkEnumeratePhysicalDevices). On a laptop with a discrete
// NVIDIA GPU and an AMD or Intel integrated one, an old integrated-GPU driver
// crashing there takes the probe down although the game never renders on it.
// The same manifest can also declare an implicit layer with the same library
// (AMD: VK_LAYER_AMD_switchable_graphics in amdvlk64.dll), loaded into every
// Vulkan application whatever GPU it runs on: leaving the driver out does not
// keep that library away, switching the layer off does.
// The probe lists the drivers (manifest, vendor), every DLL it loads, and, if
// it crashes, the module the crash was in. Only after a crash or a hang is the
// probe started again, narrowest first (vkplanretry): the layer the crash was
// traced to, then every implicit layer, then, last and only with a driver for
// the GPU the game runs on next to it, the integrated GPU's driver. The first
// step after which the probe answers is kept in this process's environment
// (the game's own Vulkan start-up reads it) and remembered (hwrtvkskipdriver)
// while those exact manifests exist: a driver update installs them under a new
// folder, and they are used again. Nothing is ever left out before it failed.

// Saved: what a probe retry had to leave out, ';' separated: "fp:<hex>"
// (fingerprint of the implicit layers and display drivers; when it differs,
// the value is dropped), "layer:<name>" (that implicit layer), "layers:all"
// (every implicit layer), "<manifest path>" (that driver). A value without a
// path separator is passed as is to VK_LOADER_DRIVERS_DISABLE (a glob on driver
// manifest file names, e.g. *amd*). "" = leave nothing out.
SVARP(hwrtvkskipdriver, "");

static const char *pathbase(const char *p)
{
    const char *b = p;
    for(; *p; p++) if(*p == '\\' || *p == '/') b = p + 1;
    return b;
}

static const char *stristr(const char *s, const char *sub)
{
    size_t n = strlen(sub);
    for(; *s; s++) if(!_strnicmp(s, sub, n)) return s;
    return NULL;
}

static const char *vendorname(int v)
{
    switch(v)
    {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        default: return "other";
    }
}

// From a driver library or manifest file name, for drivers listed without an adapter.
static int vendorfromfile(const char *path)
{
    const char *b = pathbase(path);
    if(!_strnicmp(b, "nv", 2)) return 0x10DE;
    if(!_strnicmp(b, "amd", 3) || !_strnicmp(b, "ati", 3)) return 0x1002;
    if(!_strnicmp(b, "ig", 2) || !_strnicmp(b, "intel", 5)) return 0x8086;
    return 0;
}

static int readtext(const char *path, char *buf, int len)
{
    buf[0] = 0;
    FILE *f = fopen(path, "rb");
    if(!f) return -1;
    int n = int(fread(buf, 1, len - 1, f));
    fclose(f);
    buf[max(n, 0)] = 0;
    return n;
}

// First string value of KEY in a manifest (JSON read as text; \\ unescaped).
static bool jsonstring(const char *text, const char *key, char *out, int len)
{
    out[0] = 0;
    defformatstring(q, "\"%s\"", key);
    const char *p = strstr(text, q);
    if(!p) return false;
    p = strchr(p + strlen(q), ':');
    if(!p) return false;
    p = strchr(p, '"');
    if(!p) return false;
    int n = 0;
    for(p++; *p && *p != '"' && n < len - 1; p++)
    {
        if(*p == '\\' && p[1]) p++;
        out[n++] = *p;
    }
    out[n] = 0;
    return true;
}

struct vkenvpair { char name[96]; char value[64]; };

// Every "disable_environment": { "NAME": "VALUE" } of a manifest: the variables
// that switch off the implicit layers it declares.
static int manifestdisableenv(const char *text, vkenvpair *out, int maxn)
{
    int n = 0;
    for(const char *p = strstr(text, "\"disable_environment\""); p && n < maxn; p = strstr(p + 1, "\"disable_environment\""))
    {
        const char *b = strchr(p + 21, '{'), *e = b ? strchr(b, '}') : NULL;
        if(!e) break;
        const char *k = strchr(b, '"'), *ke = k ? strchr(k + 1, '"') : NULL;
        if(!ke || ke > e) continue;
        const char *v = strchr(ke + 1, '"'), *ve = v ? strchr(v + 1, '"') : NULL;
        int kl = min(int(ke - k - 1), int(sizeof(out[n].name)) - 1);
        memcpy(out[n].name, k + 1, kl);
        out[n].name[kl] = 0;
        out[n].value[0] = 0;
        if(ve && ve < e)
        {
            int vl = min(int(ve - v - 1), int(sizeof(out[n].value)) - 1);
            memcpy(out[n].value, v + 1, vl);
            out[n].value[vl] = 0;
        }
        bool dup = false;
        loopi(n) if(!strcmp(out[i].name, out[n].name)) dup = true;
        if(!dup && out[n].name[0]) n++;
    }
    return n;
}

// The loader's filter globs: "*x*" contains, "*x" ends with, "x*" starts with, "x" equals.
static bool globmatch(const char *g, int gl, const char *name)
{
    if(gl <= 0) return false;
    bool lead = g[0] == '*', trail = gl > 1 && g[gl-1] == '*';
    if(gl == 1 && lead) return true;
    const char *core = g + (lead ? 1 : 0);
    int cl = gl - (lead ? 1 : 0) - (trail ? 1 : 0);
    int nl = int(strlen(name));
    if(cl <= 0 || cl > nl) return false;
    if(lead && trail) { loopi(nl - cl + 1) if(!_strnicmp(name + i, core, cl)) return true; return false; }
    if(lead) return !_strnicmp(name + nl - cl, core, cl);
    if(trail) return !_strnicmp(name, core, cl);
    return cl == nl && !_strnicmp(name, core, cl);
}

static bool globlistmatch(const char *list, const char *name)
{
    for(const char *p = list; *p; )
    {
        const char *e = strchr(p, ',');
        int l = e ? int(e - p) : int(strlen(p));
        if(globmatch(p, l, name)) return true;
        if(!e) break;
        p = e + 1;
    }
    return false;
}

// Same driver package: the DriverStore folder of the manifest (the crashing
// module is often a helper DLL of the driver, e.g. amdxc64.dll next to amdvlk64.dll).
static bool samedriverfolder(const char *module, const char *manifest)
{
    const char *fr = stristr(manifest, "\\FileRepository\\");
    int len = 0;
    if(fr)
    {
        const char *e = strchr(fr + 16, '\\');
        len = e ? int(e - manifest) : int(strlen(manifest));
    }
    else len = int(pathbase(manifest) - manifest) - 1;   // the folder, without its trailing separator
    return len > 0 && int(strlen(module)) > len && !_strnicmp(module, manifest, len) && (module[len] == '\\' || module[len] == '/');
}

// What the last probe printed about the drivers (game side, parsed from its lines).
struct vkicdseen { int vendor; bool loaded; char manifest[MAX_PATH + 1]; };
static vkicdseen vkicds[16];
static int nvkicds = 0;
static int vkloaderver = -1;        // major*1000000 + minor*1000 + patch, -1 unknown
static char vkfaultpath[MAX_PATH + 1] = "";
static bool vkfaultfinal = false;    // from the unhandled-exception line, not a first-chance one
static bool vkdllwatch = false;      // the probe lists the DLLs it loads ("dll loaded" lines)

// Process environment changed for the skip (the probe inherits it, and the
// loader reads it in vkCreateInstance), with what to put back.
struct vkenvsave { char name[96]; bool had; char *value; };
static vkenvsave vkenvsaves[64];
static int nvkenvsaves = 0;

static void vkgetenv(const char *name, char *out, int len)
{
    DWORD n = GetEnvironmentVariableA(name, out, DWORD(len));
    if(!n || n >= DWORD(len)) out[0] = 0;
}

static void vksetenv(const char *name, const char *value)
{
    bool saved = false;
    loopi(nvkenvsaves) if(!strcmp(vkenvsaves[i].name, name)) saved = true;
    if(!saved && nvkenvsaves >= int(sizeof(vkenvsaves)/sizeof(vkenvsaves[0])))
    {
        logoutf("hwrt vk: too many variables to set, %s left as it is", name);
        return;   // never change what could not be put back
    }
    if(!saved)
    {
        vkenvsave &s = vkenvsaves[nvkenvsaves++];
        copystring(s.name, name, sizeof(s.name));
        DWORD n = GetEnvironmentVariableA(name, NULL, 0);
        s.had = n > 0;
        s.value = NULL;
        if(s.had)
        {
            s.value = new char[n + 1];
            if(!GetEnvironmentVariableA(name, s.value, n + 1)) s.value[0] = 0;
        }
    }
    SetEnvironmentVariableA(name, value);
    logoutf("hwrt vk: %s=%s", name, value);
}

static void vkrestoreenv()
{
    loopi(nvkenvsaves)
    {
        vkenvsave &s = vkenvsaves[i];
        SetEnvironmentVariableA(s.name, s.had ? s.value : NULL);
        logoutf("hwrt vk: %s %s", s.name, s.had ? "put back as it was" : "removed again");
        delete[] s.value;
        s.value = NULL;
    }
    nvkenvsaves = 0;
}

// What a failed probe is retried without, narrowest that works, at most
// three cascade steps plus a bounded search:
//  1. the implicit layers of the manifest the crash was traced to (a crash in
//     amdxc64.dll is traced to the AMD driver's manifest, which declares
//     VK_LAYER_AMD_switchable_graphics; a crash in a tool's DLL to that
//     tool's layer), by their disable_environment variables (any loader) and
//     by name in VK_LOADER_LAYERS_DISABLE (loader 1.3.234+);
//  2. every implicit layer (plus VK_LOADER_LAYERS_DISABLE=~implicit~,
//     loader 1.3.262+). If that works, each implicit layer is then tried
//     alone, most suspect first (another vendor's driver layers, then tools,
//     then the layers of the GPU the game runs on), within hwrtvksearchms
//     (each probe a few hundred ms); the first one that is enough is kept,
//     otherwise all of them;
//  3. last resort, only with a driver for the GPU the game runs on next to it:
//     the integrated GPU's whole driver (and its layers).
// Layer steps work for any player whose probe failed; leaving a driver out
// also needs the GPU the game runs on to be a discrete one that can trace.
// The result is saved with a fingerprint of the implicit layers and display
// drivers installed: when they change, nothing is left out until it fails again.

// Milliseconds the search for the single culprit layer may take (step 2).
VARP(hwrtvksearchms, 0, 10000, 60000);

enum { VKSTEP_LAYERS = 1, VKSTEP_ALLLAYERS, VKSTEP_DRIVER };
struct vkretrystep { int kind; int n; int units[48]; char manifests[8][MAX_PATH + 1]; };
static vkretrystep vksteps[3];
static int nvksteps = 0, vkstepi = -1;  // vkstepi: cascade step running now (or that worked)

// One implicit layer: name, the variable that switches it off, where it is declared.
struct vklayerunit { char name[96]; char var[96]; char value[64]; char manifest[MAX_PATH + 1]; int vendor; };
static vklayerunit vkunits[48];
static int nvkunits = 0;

static bool vkretried = false;          // the retry steps were planned for this run
static bool vkretrying = false;         // the probe running now is a retry
static bool vksearching = false;        // ... of the search for the single culprit layer
static int vksearch[48], nvksearch = 0, vksearchi = -1;
static Uint32 vksearchstart = 0;
static hwrtvkproberesult vkbestprobe;   // answer of the step that worked before the search
static int vkbesttype = -1;
static bool vkskipping = false;         // something is left out in the environment
static bool vkskipsaved = false;        // ... and it came from hwrtvkskipdriver
static int vkretrywhy = HWRT_WHY_NONE;
static string vkretrywhat = "";
static char vkskipnames[1024] = "";     // what is left out, for the messages
static char vkskipsave[1400] = "";      // hwrtvkskipdriver value on success
static char vkskipwho[160] = "";        // "the AMD graphics driver" / "the program that installed it"
static int vkskipvendor = 0;
static Uint32 vkprobetimeoutms = 0;     // 0 = hwrtvktimeout
static char vknote[600] = "";
// read-only: what was left out and why, "" when nothing was
ICOMMAND(hwrtvknote, "", (), result(vknote));

static void vkaddname(char *buf, int len, const char *sep, const char *s)
{
    if(buf[0]) concatstring(buf, sep, len);
    concatstring(buf, s, len);
}

// Implicit layer manifests registered on this machine (read-only, as the
// loader finds them on Windows: Khronos registry keys, display drivers).
static int vkimplicitmanifests(char (*out)[MAX_PATH + 1], int maxn)
{
    int n = 0;
    HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    #define VKADDMANIFEST(p) do { bool dup = false; loopj(n) if(!_stricmp(out[j], p)) dup = true; if(!dup && n < maxn) copystring(out[n++], p, MAX_PATH + 1); } while(0)
    loopk(2)
    {
        HKEY key;
        if(RegOpenKeyExA(roots[k], "SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        for(DWORD i = 0; i < 256; i++)
        {
            char valname[1024];
            DWORD vlen = sizeof(valname), type = 0, data = 0, dlen = sizeof(data);
            LONG r = RegEnumValueA(key, i, valname, &vlen, NULL, &type, (BYTE *)&data, &dlen);
            if(r == ERROR_NO_MORE_ITEMS) break;
            if(r == ERROR_SUCCESS && type == REG_DWORD && data == 0) VKADDMANIFEST(valname);
        }
        RegCloseKey(key);
    }
    loopi(16)
    {
        defformatstring(sub, "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\%04d", i);
        static char multi[8192];
        DWORD len = sizeof(multi) - 2;
        memset(multi, 0, sizeof(multi));
        if(RegGetValueA(HKEY_LOCAL_MACHINE, sub, "VulkanImplicitLayers", RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, NULL, multi, &len) != ERROR_SUCCESS) continue;
        for(const char *p = multi; strlen(p) >= 4 && p < multi + len; p += strlen(p) + 1) VKADDMANIFEST(p);
    }
    #undef VKADDMANIFEST
    return n;
}

// The next "name" key of a layer object (extension entries also have one: VK_KHR_..., VK_EXT_...).
static const char *nextlayername(const char *p)
{
    for(p = strstr(p, "\"name\""); p; p = strstr(p + 6, "\"name\""))
    {
        char n[128];
        if(!jsonstring(p, "name", n, sizeof(n))) continue;
        if(strncmp(n, "VK_", 3) || !strncmp(n, "VK_LAYER_", 9)) return p;
    }
    return NULL;
}

// Every implicit layer with its own disable variable ("name" ... "disable_environment" per layer object).
static void vkcollectunits()
{
    static char manifests[48][MAX_PATH + 1];
    int nm = vkimplicitmanifests(manifests, 48);
    nvkunits = 0;
    static char text[65536];
    loopi(nm)
    {
        if(readtext(manifests[i], text, sizeof(text)) <= 0) continue;
        int vendor = stristr(manifests[i], "\\DriverStore\\") ? vendorfromfile(manifests[i]) : 0;
        for(const char *p = nextlayername(text); p && nvkunits < 48; )
        {
            const char *next = nextlayername(p + 6);
            vklayerunit &u = vkunits[nvkunits];
            if(jsonstring(p, "name", u.name, sizeof(u.name)) && u.name[0])
            {
                const char *d = strstr(p, "\"disable_environment\"");
                if(d && (!next || d < next))
                {
                    vkenvpair pr;
                    if(manifestdisableenv(d, &pr, 1) == 1)
                    {
                        copystring(u.var, pr.name, sizeof(u.var));
                        copystring(u.value, pr.value[0] ? pr.value : "1", sizeof(u.value));
                        copystring(u.manifest, manifests[i], sizeof(u.manifest));
                        u.vendor = vendor;
                        bool dup = false;
                        loopj(nvkunits) if(!strcmp(vkunits[j].name, u.name)) dup = true;
                        if(!dup) nvkunits++;
                    }
                }
            }
            p = next;
        }
    }
}

// Fingerprint of the implicit layers and display drivers installed: a saved
// result only applies while they are the same.
static uint32_t vkfingerprint()
{
    uint32_t h = 2166136261U;
    #define VKFNV(s) for(const char *q = (s); *q; q++) { h ^= uchar(tolower(*q)); h *= 16777619U; }
    static char manifests[48][MAX_PATH + 1];
    int nm = vkimplicitmanifests(manifests, 48);
    loopi(nm) { VKFNV(manifests[i]); VKFNV("|"); }
    loopi(16)
    {
        defformatstring(sub, "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\%04d", i);
        char v[256] = "";
        DWORD len = sizeof(v);
        if(RegGetValueA(HKEY_LOCAL_MACHINE, sub, "DriverDesc", RRF_RT_REG_SZ, NULL, v, &len) != ERROR_SUCCESS) continue;
        VKFNV(v);
        len = sizeof(v);
        v[0] = 0;
        RegGetValueA(HKEY_LOCAL_MACHINE, sub, "DriverVersion", RRF_RT_REG_SZ, NULL, v, &len);
        VKFNV(v); VKFNV("|");
    }
    #undef VKFNV
    return h;
}

static bool vkloaderatleast(int v) { return vkloaderver < 0 || vkloaderver >= v; }

static void vkappendenv(const char *name, const char *add)
{
    char cur[1024], value[2048];
    vkgetenv(name, cur, sizeof(cur));
    if(cur[0]) formatstring(value, "%s,%s", cur, add);
    else copystring(value, add, sizeof(value));
    vksetenv(name, value);
}

// Switches off these layer units; their names go to NAMES.
static bool vkapplyunits(const int *u, int n, char *names, int nameslen)
{
    if(names) names[0] = 0;
    char list[1024] = "";
    loopi(n)
    {
        vksetenv(vkunits[u[i]].var, vkunits[u[i]].value);
        vkaddname(list, sizeof(list), ",", vkunits[u[i]].name);
    }
    if(!n) return false;
    // By name too (loader 1.3.234+), in case a variable is not honoured.
    if(vkloaderatleast(1003234)) vkappendenv("VK_LOADER_LAYERS_DISABLE", list);
    if(names) copystring(names, list, nameslen);
    return true;
}

// Puts one step into the environment (on top of what is there).
static bool vkapplystep(const vkretrystep &st, const char *patterns)
{
    if(st.kind == VKSTEP_LAYERS)
    {
        char names[512];
        if(!vkapplyunits(st.units, st.n, names, sizeof(names))) return false;
        defformatstring(desc, "the Vulkan layer %s", names);
        vkaddname(vkskipnames, sizeof(vkskipnames), "; ", desc);
        loopi(st.n) { defformatstring(tok, "layer:%s", vkunits[st.units[i]].name); vkaddname(vkskipsave, sizeof(vkskipsave), ";", tok); }
        vkskipping = true;
        return true;
    }
    if(st.kind == VKSTEP_ALLLAYERS)
    {
        int all[48];
        loopi(nvkunits) all[i] = i;
        bool any = vkapplyunits(all, nvkunits, NULL, 0);
        // Every implicit layer the loader finds, also those not listed above (loader 1.3.262+).
        if(vkloaderatleast(1003262)) { vkappendenv("VK_LOADER_LAYERS_DISABLE", "~implicit~"); any = true; }
        if(!any) return false;
        vkaddname(vkskipnames, sizeof(vkskipnames), "; ", "every implicit Vulkan layer (overlays, capture tools, driver layers)");
        vkaddname(vkskipsave, sizeof(vkskipsave), ";", "layers:all");
        vkskipping = true;
        return true;
    }
    // VKSTEP_DRIVER
    char names[1024] = "";
    loopi(st.n) vkaddname(names, sizeof(names), ",", pathbase(st.manifests[i]));
    if(patterns && patterns[0]) vkaddname(names, sizeof(names), ",", patterns);
    if(!names[0]) return false;
    if(vkloaderatleast(1003234))
    {
        // Compared with the manifest file name by the loader; "checked before
        // other driver environment variables", honoured when elevated too.
        vkappendenv("VK_LOADER_DRIVERS_DISABLE", names);
    }
    else
    {
        // Loader older than 1.3.234 (no driver filters): name the drivers to
        // keep instead. VK_ICD_FILENAMES is read by every loader version but
        // ignored when the game runs elevated.
        char cur[1024];
        vkgetenv("VK_ICD_FILENAMES", cur, sizeof(cur));
        if(!cur[0]) vkgetenv("VK_DRIVER_FILES", cur, sizeof(cur));
        if(cur[0]) { logoutf("hwrt vk: VK_ICD_FILENAMES/VK_DRIVER_FILES is set already, not changed"); return false; }
        char keep[2048] = "";
        loopi(nvkicds)
        {
            bool skip = false;
            loopj(st.n) if(!_stricmp(vkicds[i].manifest, st.manifests[j])) skip = true;
            if(!skip) vkaddname(keep, sizeof(keep), ";", vkicds[i].manifest);
        }
        if(!keep[0] || !st.n) { logoutf("hwrt vk: loader older than 1.3.234 and no other driver to keep"); return false; }
        vksetenv("VK_ICD_FILENAMES", keep);
    }
    // Its library also comes in through the layers its manifest declares.
    int own[48], nown = 0;
    loopi(nvkunits) loopj(st.n) if(!_stricmp(vkunits[i].manifest, st.manifests[j]) && nown < 48) own[nown++] = i;
    vkapplyunits(own, nown, NULL, 0);
    defformatstring(desc, "the Vulkan driver %s", names);
    vkaddname(vkskipnames, sizeof(vkskipnames), "; ", desc);
    loopi(st.n) vkaddname(vkskipsave, sizeof(vkskipsave), ";", st.manifests[i]);
    if(patterns && patterns[0]) vkaddname(vkskipsave, sizeof(vkskipsave), ";", patterns);
    vkskipping = true;
    return true;
}

static void vkclearskip()
{
    vkrestoreenv();
    vkskipping = false;
    vkskipnames[0] = vkskipsave[0] = 0;
}

static void vkundoskip(const char *why)
{
    if(!vkskipping && !nvkenvsaves) return;
    if(vkskipnames[0]) logoutf("hwrt vk: no longer leaving out %s (%s)", vkskipnames, why);
    vkclearskip();
    vkskipsaved = false;
}

// Who to update, for the messages: the driver a manifest belongs to, or the program.
static void vksetwho(const char *manifest)
{
    int vendor = 0;
    loopi(nvkicds) if(!_stricmp(vkicds[i].manifest, manifest)) vendor = vkicds[i].vendor;
    if(!vendor && stristr(manifest, "\\DriverStore\\")) vendor = vendorfromfile(manifest);
    vkskipvendor = vendor;
    if(vendor) formatstring(vkskipwho, "the %s graphics driver", vendorname(vendor));
    else if(manifest[0]) copystring(vkskipwho, "the program that installed it", sizeof(vkskipwho));
    else copystring(vkskipwho, "the graphics driver or the overlay/capture programs", sizeof(vkskipwho));
}

// First probe of the run: apply hwrtvkskipdriver if the machine is as it was
// ("fp:<hex>" fingerprint, "layer:<name>", "layers:all", "<manifest>" a driver,
// a name without a path a driver glob).
static void vkapplysaved()
{
    static bool done = false;
    if(done || !hwrtvkskipdriver[0]) return;
    done = true;
    static char paths[1400];
    copystring(paths, hwrtvkskipdriver, sizeof(paths));
    uint32_t fp = 0;
    bool havefp = false;
    for(char *p = paths; *p; )
    {
        char *e = strchr(p, ';');
        if(!strncmp(p, "fp:", 3)) { fp = uint32_t(strtoul(p + 3, NULL, 16)); havefp = true; }
        if(!e) break;
        p = e + 1;
    }
    if(havefp && fp != vkfingerprint())
    {
        logoutf("hwrt vk: Vulkan layers or graphics drivers changed since hwrtvkskipdriver was saved: nothing is left out until something fails again");
        setsvar("hwrtvkskipdriver", "");
        return;
    }
    vkcollectunits();
    static vkretrystep layers, drivers;
    layers.kind = VKSTEP_LAYERS; layers.n = 0;
    drivers.kind = VKSTEP_DRIVER; drivers.n = 0;
    bool all = false, stale = false;
    char patterns[512] = "", keep[1400] = "";
    for(char *p = paths; *p; )
    {
        char *e = strchr(p, ';');
        if(e) *e = 0;
        bool ok = true;
        if(!strncmp(p, "fp:", 3)) {}
        else if(!strcmp(p, "layers:all")) all = true;
        else if(!strncmp(p, "layer:", 6))
        {
            bool found = false;
            loopi(nvkunits) if(!strcmp(vkunits[i].name, p + 6) && layers.n < 48) { layers.units[layers.n++] = i; found = true; }
            if(!found) { logoutf("hwrt vk: the Vulkan layer %s is not installed any more", p + 6); ok = false; }
        }
        else if(*p)
        {
            if(!strchr(p, '\\') && !strchr(p, '/')) vkaddname(patterns, sizeof(patterns), ",", p);
            else if(GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) { logoutf("hwrt vk: %s is not installed any more: it is used again", p); ok = false; }
            else if(drivers.n < 8) copystring(drivers.manifests[drivers.n++], p, MAX_PATH + 1);
        }
        if(!ok) stale = true;
        else if(*p) vkaddname(keep, sizeof(keep), ";", p);
        if(!e) break;
        p = e + 1;
    }
    if(stale) setsvar("hwrtvkskipdriver", keep);   // only what still applies
    bool any = false;
    if(layers.n && vkapplystep(layers, NULL)) any = true;
    if(all) { vkretrystep a; a.kind = VKSTEP_ALLLAYERS; a.n = 0; if(vkapplystep(a, NULL)) any = true; }
    if((drivers.n || patterns[0]) && vkapplystep(drivers, patterns)) any = true;
    if(!any) return;
    vkskipsaved = true;
    if(drivers.n) vksetwho(drivers.manifests[0]);
    else if(layers.n == 1) vksetwho(vkunits[layers.units[0]].manifest);
    else vksetwho("");
    logoutf("hwrt vk: leaving out %s (hwrtvkskipdriver, saved after it failed during an earlier start; hwrtvkskipdriver \"\" uses it again)", vkskipnames);
}

static bool launchprobe();

// Starts the next cascade step that applies; false when none is left.
static bool vknextstep()
{
    while(++vkstepi < nvksteps)
    {
        vkclearskip();
        const vkretrystep &st = vksteps[vkstepi];
        if(!vkapplystep(st, NULL))
        {
            logoutf("hwrt vk: probe retry %d/%d does not apply here, next", vkstepi + 1, nvksteps);
            vkclearskip();
            continue;
        }
        vkretrying = true;
        vkprobetimeoutms = 0;
        logoutf("hwrt vk: [%u ms] probe retry %d/%d: without %s", SDL_GetTicks(), vkstepi + 1, nvksteps, vkskipnames);
        if(launchprobe()) return true;
        vkretrying = false;
    }
    vkclearskip();
    return false;
}

// Search for the single culprit layer: the next one alone, while time is left.
static bool vknextsearch()
{
    while(++vksearchi < nvksearch)
    {
        Uint32 used = SDL_GetTicks() - vksearchstart;
        if(used >= Uint32(hwrtvksearchms)) { logoutf("hwrt vk: layer search stopped after %u ms (hwrtvksearchms %d)", used, hwrtvksearchms); break; }
        vkclearskip();
        vkretrystep one;
        one.kind = VKSTEP_LAYERS;
        one.n = 1;
        one.units[0] = vksearch[vksearchi];
        if(!vkapplystep(one, NULL)) continue;
        vkretrying = vksearching = true;
        vkprobetimeoutms = min(Uint32(hwrtvktimeout) * 1000, max(Uint32(hwrtvksearchms) - used, 1000U));
        logoutf("hwrt vk: [%u ms] layer search %d/%d: without %s only", SDL_GetTicks(), vksearchi + 1, nvksearch, vkskipnames);
        if(launchprobe()) return true;
    }
    vkretrying = vksearching = false;
    vkprobetimeoutms = 0;
    return false;
}

// Which manifest (driver first, then implicit layer) a crashing module belongs to.
static int vkfaultmanifest(char *out, int len)
{
    out[0] = 0;
    if(!vkfaultpath[0]) return 0;
    loopi(nvkicds) if(samedriverfolder(vkfaultpath, vkicds[i].manifest)) { copystring(out, vkicds[i].manifest, len); return 1; }
    loopi(nvkunits) if(samedriverfolder(vkfaultpath, vkunits[i].manifest)) { copystring(out, vkunits[i].manifest, len); return 2; }
    return 0;
}

// After the probe crashed or hung: plan the retry steps and start the first.
// False = nothing to retry, give up as before.
static bool vkplanretry(int why, const char *what)
{
    if(vkretried) return false;
    vkretried = true;
    vkcollectunits();
    nvksteps = 0;
    vkstepi = -1;
    char faulty[MAX_PATH + 1];
    int fk = vkfaultmanifest(faulty, sizeof(faulty));
    int fvendor = fk ? vendorfromfile(faulty) : 0;
    if(fk == 1) loopi(nvkicds) if(!_stricmp(vkicds[i].manifest, faulty)) fvendor = vkicds[i].vendor;
    if(fk)
    {
        if(fk == 1) formatstring(vkfaultdesc, "the %s graphics driver (%s)", vendorname(fvendor), pathbase(vkfaultpath));
        else formatstring(vkfaultdesc, "a Vulkan layer (%s, %s)", pathbase(vkfaultpath), pathbase(faulty));
        logoutf("hwrt vk: the probe crashed in %s, part of %s %s", vkfaultpath, fk == 1 ? "the driver" : "the implicit layer", faulty);
    }
    else if(vkfaultpath[0]) logoutf("hwrt vk: the probe crashed in %s, which is not in a Vulkan driver's or layer's folder", vkfaultpath);
    loopi(nvkunits) logoutf("hwrt vk: implicit layer %s (%s, off with %s)", vkunits[i].name, pathbase(vkunits[i].manifest), vkunits[i].var);

    // Integrated GPU drivers that may be the cause when nothing was traced.
    vkretrystep igpu;
    igpu.n = 0;
    loopi(nvkicds)
    {
        int v = vkicds[i].vendor;
        if(v == vkglvendor || (v != 0x1002 && v != 0x8086)) continue;
        if(vkdllwatch && !vkicds[i].loaded)
        {
            logoutf("hwrt vk: the %s driver %s was not loaded by the probe: not the cause", vendorname(v), pathbase(vkicds[i].manifest));
            continue;
        }
        if(igpu.n < 8) copystring(igpu.manifests[igpu.n++], vkicds[i].manifest, MAX_PATH + 1);
    }

    // 1. the layers of the manifest it was traced to
    vkretrystep &s1 = vksteps[nvksteps];
    s1.kind = VKSTEP_LAYERS;
    s1.n = 0;
    if(fk) loopi(nvkunits) if(!_stricmp(vkunits[i].manifest, faulty) && s1.n < 48) s1.units[s1.n++] = i;
    if(s1.n && s1.n < nvkunits) nvksteps++;
    // 2. every implicit layer (then the search for the single one)
    vksteps[nvksteps].kind = VKSTEP_ALLLAYERS;
    vksteps[nvksteps].n = 0;
    nvksteps++;
    // 3. the integrated GPU's driver, last, if one for the GPU the game runs on is there
    bool glvdriver = false;
    loopi(nvkicds) if(vkicds[i].vendor == vkglvendor) glvdriver = true;
    vkretrystep &drv = vksteps[nvksteps];
    drv.kind = VKSTEP_DRIVER;
    drv.n = 0;
    if(fk == 1 && fvendor != vkglvendor) copystring(drv.manifests[drv.n++], faulty, MAX_PATH + 1);
    else if(!fk) loopi(igpu.n) copystring(drv.manifests[drv.n++], igpu.manifests[i], MAX_PATH + 1);
    if(drv.n && vkglvendor && glvdriver) nvksteps++;
    else logoutf("hwrt vk: no integrated-GPU driver to leave out as a last resort (%s)",
                 !vkglvendor || !glvdriver ? "no other Vulkan driver for the GPU the game runs on" : "none traced or loaded");

    // Search order for step 2: another vendor's driver layers, then tools, then the GPU's own.
    nvksearch = 0;
    loopk(3) loopi(nvkunits)
    {
        int v = vkunits[i].vendor, cls = v && v != vkglvendor ? 0 : (!v ? 1 : 2);
        if(cls != k) continue;
        bool tried = false;
        if(s1.n && nvksteps && vksteps[0].kind == VKSTEP_LAYERS) loopj(s1.n) if(s1.units[j] == i) tried = true;
        if(!tried && nvksearch < 48) vksearch[nvksearch++] = i;
    }

    vksetwho(fk ? faulty : "");
    vkretrywhy = why;
    copystring(vkretrywhat, what);
    logoutf("hwrt vk: [%u ms] the probe %s: trying it again, narrowest first (%d step%s)",
            SDL_GetTicks(), why == HWRT_WHY_CRASH ? "crashed" : "did not answer", nvksteps, nvksteps > 1 ? "s" : "");
    return vknextstep();
}

static HANDLE probeproc = NULL, probeout = NULL, probejob = NULL;
static Uint32 probestart = 0;
static char probepend[1024];
static int probependlen = 0;
static bool probegotresult = false;
static hwrtvkproberesult probenew;
static int probenewtype = -1;   // VkPhysicalDeviceType of the GPU it matched, -1 none

static void probeline(char *line)
{
    int n = int(strlen(line));
    while(n > 0 && (line[n-1] == '\r' || line[n-1] == '\n')) line[--n] = 0;
    if(!n) return;
    logoutf("hwrt vkprobe: %s", line);   // diagnostics: log only, not the game console
    const char *ic = strstr(line, " icd vendor=0x");
    if(ic)
    {
        unsigned v = 0;
        sscanf(ic + 14, "%x", &v);
        const char *m = strstr(ic, " manifest=");
        if(m && nvkicds < int(sizeof(vkicds)/sizeof(vkicds[0])))
        {
            bool dup = false;
            loopi(nvkicds) if(!_stricmp(vkicds[i].manifest, m + 10)) dup = true;
            if(!dup)
            {
                vkicds[nvkicds].vendor = int(v);
                vkicds[nvkicds].loaded = false;
                copystring(vkicds[nvkicds].manifest, m + 10, sizeof(vkicds[nvkicds].manifest));
                nvkicds++;
            }
        }
        return;
    }
    if(strstr(line, " dll notifications on")) vkdllwatch = true;
    const char *dl = strstr(line, " dll loaded ");
    if(dl)
    {
        char path[MAX_PATH + 1];
        copystring(path, dl + 12, sizeof(path));
        char *sim = strstr(path, " (simulated)");
        if(sim) *sim = 0;
        loopi(nvkicds) if(samedriverfolder(path, vkicds[i].manifest)) vkicds[i].loaded = true;
        return;
    }
    const char *lv = strstr(line, " loader Vulkan ");
    if(lv && vkloaderver < 0)
    {
        int a = 0, b = 0, c = 0;
        if(sscanf(lv + 15, "%d.%d.%d", &a, &b, &c) == 3) vkloaderver = a*1000000 + b*1000 + c;
    }
    const char *un = strstr(line, " unhandled 0x"), *fc = strstr(line, " fault (first chance) 0x");
    if(un || (fc && !vkfaultfinal))
    {
        const char *o = strrchr(line, '('), *e = o ? strrchr(o, ')') : NULL;
        if(e && e > o + 1)
        {
            int l = min(int(e - o - 1), MAX_PATH);
            memcpy(vkfaultpath, o + 1, l);
            vkfaultpath[l] = 0;
            char *sim = strstr(vkfaultpath, ", simulated");
            if(sim) *sim = 0;
            if(un) vkfaultfinal = true;
        }
    }
    const char *r = strstr(line, "result why=");
    if(!r) return;
    int why = HWRT_WHY_FAILED, rtwhy = HWRT_WHY_FAILED, rq = 0, nv = 0;
    if(sscanf(r, "result why=%d rtwhy=%d rq=%d nvidia=%d", &why, &rtwhy, &rq, &nv) != 4) return;
    memset(&probenew, 0, sizeof(probenew));
    probenew.why = why;
    probenew.rtwhy = rtwhy;
    probenew.rayquery = rq != 0;
    probenew.nvidia = nv != 0;
    const char *type = strstr(r, " type=");
    probenewtype = type ? atoi(type + 6) : -1;
    const char *name = strstr(r, " name=");
    if(name) copystring(probenew.name, name + 6, sizeof(probenew.name));
    probegotresult = true;
}

static void readprobe()
{
    if(!probeout) return;
    for(;;)
    {
        DWORD avail = 0;
        if(!PeekNamedPipe(probeout, NULL, 0, NULL, &avail, NULL) || !avail) return;
        char buf[512];
        DWORD got = 0;
        if(!ReadFile(probeout, buf, min(avail, DWORD(sizeof(buf))), &got, NULL) || !got) return;
        loopi(int(got))
        {
            char c = buf[i];
            if(c == '\n' || probependlen >= int(sizeof(probepend)) - 1)
            {
                probepend[probependlen] = 0;
                probeline(probepend);
                probependlen = 0;
                if(c == '\n') continue;
            }
            probepend[probependlen++] = c;
        }
    }
}

static void closeprobe()
{
    // Closing the job kills the probe if it is still there (stuck or exiting).
    if(probejob) { CloseHandle(probejob); probejob = NULL; }
    else if(probeproc) TerminateProcess(probeproc, 1);
    if(probeproc) { CloseHandle(probeproc); probeproc = NULL; }
    if(probeout) { CloseHandle(probeout); probeout = NULL; }
}

static void probedone();
static void vkfinishskip(int kind);

// The search found nothing narrower (or ran out of time): back to every implicit layer.
static void vksearchdone()
{
    vkclearskip();
    vkretrystep all;
    all.kind = VKSTEP_ALLLAYERS;
    all.n = 0;
    vkapplystep(all, NULL);
    probenew = vkbestprobe;
    probenewtype = vkbesttype;
    logoutf("hwrt vk: no single implicit layer is enough on its own: every implicit layer stays off");
    vkfinishskip(VKSTEP_ALLLAYERS);
}

// The probe failed (crash or hang) with what = the step: retry narrowest
// first when that can help, otherwise give up as before.
static void probefailed(int why, const char *what)
{
    if(vksearching)
    {
        vkretrying = vksearching = false;
        logoutf("hwrt vk: [%u ms] without %s only: the probe %s, not that one", SDL_GetTicks(), vkskipnames, why == HWRT_WHY_CRASH ? "crashed" : "did not answer");
        if(vknextsearch()) return;
        vksearchdone();
        probedone();   // accept the answer of the step that worked
        return;
    }
    if(vkretrying)
    {
        vkretrying = false;
        logoutf("hwrt vk: [%u ms] the probe without %s %s too", SDL_GetTicks(), vkskipnames, why == HWRT_WHY_CRASH ? "crashed" : "did not answer");
        if(vknextstep()) return;
        vkundoskip("nothing helped");
        logoutf("hwrt vk: no retry step helped; as before, with the first failure's reason");
        vkgiveup(vkretrywhy, vkretrywhat);
        return;
    }
    if(vkskipsaved)
    {
        // The saved skip did not help this time: drop it, and decide again from what this probe saw.
        vkundoskip("the probe failed with it");
        setsvar("hwrtvkskipdriver", "");
    }
    if(vkplanretry(why, what)) return;
    vkgiveup(why, what);
}

// A retry worked: tell the player, keep it for the game and the next starts.
static void vkfinishskip(int kind)
{
    vkretrying = vksearching = false;
    vkprobetimeoutms = 0;
    const char *did = vkretrywhy == HWRT_WHY_CRASH ? "crashed" : "stopped answering in";
    logoutf("hwrt vk: [%u ms] Vulkan works without %s", SDL_GetTicks(), vkskipnames);
    if(kind == VKSTEP_LAYERS)
        formatstring(vknote, "The%s from %s %s Vulkan, so it is switched off for this game. Updating %s may fix this.",
                     vkskipnames + 3, vkskipwho, did, vkskipwho);
    else if(kind == VKSTEP_ALLLAYERS)
        formatstring(vknote, "A Vulkan layer (overlay, capture tool or driver add-on) %s Vulkan, so implicit Vulkan layers are switched off for this game (list in log.txt).", did);
    else
        formatstring(vknote, "The %s graphics driver (%s) %s Vulkan, so it is skipped: ray tracing and DLSS use %s. Updating the %s graphics driver may fix this.",
                     vendorname(vkskipvendor), pathbase(vksteps[vkstepi].manifests[0]), did, probenew.name, vendorname(vkskipvendor));
    conoutf(CON_WARN, "%s", vknote);
    // Remembered while the implicit layers and display drivers stay the same.
    defformatstring(save, "fp:%08x;%s", vkfingerprint(), vkskipsave);
    setsvar("hwrtvkskipdriver", save);
    logoutf("hwrt vk: hwrtvkskipdriver \"%s\" saved for the next starts (\"\" uses everything again)", save);
}

static void probedone()
{
    if(vksearching)
    {
        if(probenew.why != HWRT_WHY_NONE)
        {
            logoutf("hwrt vk: without %s only, Vulkan is not usable: not that one", vkskipnames);
            vkretrying = vksearching = false;
            if(vknextsearch()) return;
            vksearchdone();
        }
        else
        {
            logoutf("hwrt vk: layer search: %s alone is enough (%u ms of search)", vkskipnames, SDL_GetTicks() - vksearchstart);
            vksetwho(vkunits[vksearch[vksearchi]].manifest);
            vkfinishskip(VKSTEP_LAYERS);
        }
    }
    else if(vkretrying)
    {
        vkretrying = false;
        const vkretrystep &st = vksteps[vkstepi];
        // Leaving a driver out is only worth it for a discrete GPU that can trace;
        // a layer step only has to give a usable Vulkan.
        bool good = probenew.why == HWRT_WHY_NONE &&
                    (st.kind != VKSTEP_DRIVER || (probenew.rayquery && probenewtype == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));
        if(!good)
        {
            logoutf("hwrt vk: [%u ms] the probe without %s answered, but %s (%s)", SDL_GetTicks(), vkskipnames,
                    st.kind == VKSTEP_DRIVER ? "found no discrete GPU with ray tracing for the game" : "Vulkan is not usable",
                    probenew.name[0] ? probenew.name : "no matching GPU");
            vkundoskip("it is not worth it");
            vkgiveup(vkretrywhy, vkretrywhat);
            return;
        }
        logoutf("hwrt vk: probe retry %d/%d worked: without %s", vkstepi + 1, nvksteps, vkskipnames);
        if(st.kind == VKSTEP_ALLLAYERS && nvksearch > 1 && hwrtvksearchms > 0)
        {
            // Now the narrowest: each implicit layer alone, most suspect first.
            vkbestprobe = probenew;
            vkbesttype = probenewtype;
            vksearchstart = SDL_GetTicks();
            vksearchi = -1;
            logoutf("hwrt vk: looking for the single layer at fault (%d candidates, at most %d ms)", nvksearch, hwrtvksearchms);
            if(vknextsearch()) return;
            vksearchdone();
        }
        else
        {
            if(st.kind == VKSTEP_ALLLAYERS && nvksearch == 1) vksetwho(vkunits[vksearch[0]].manifest);
            vkfinishskip(st.kind);
        }
    }
    else if(vkskipsaved && probenew.why == HWRT_WHY_NONE)
    {
        // Said on screen at the start where it was left out (vkfinishskip); afterwards the log and the menu (hwrtvknote) are enough.
        formatstring(vknote, "Left out because it failed in an earlier start: %s. Updating %s may fix this.", vkskipnames, vkskipwho);
        logoutf("hwrt vk: %s", vknote);
    }
    hwrtvkprobe = probenew;
    conoutf(CON_INIT, "hwrt vk: [%u ms] probe answered in %u ms: %s, ray query %s%s",
            SDL_GetTicks(), SDL_GetTicks() - probestart, hwrtvkprobe.name[0] ? hwrtvkprobe.name : "no matching GPU",
            hwrtvkprobe.rayquery ? "yes" : "no", hwrtvkprobe.why == HWRT_WHY_NONE ? "" : ", Vulkan not usable here");
    if(hwrtvkprobe.why == HWRT_WHY_NONE) hwrtvkstate = HWRT_VK_PROBED;
    else
    {
        hwrtunavailable(hwrtvkprobe.why);
        hwrtvkstate = HWRT_VK_FAILED;
    }
}

static void pollprobe()
{
    if(hwrtvkstate != HWRT_VK_PROBING) return;
    readprobe();
    if(probegotresult)
    {
        closeprobe();
        probedone();
        return;
    }
    if(WaitForSingleObject(probeproc, 0) == WAIT_OBJECT_0)
    {
        readprobe();
        if(probependlen) { probepend[probependlen] = 0; probeline(probepend); probependlen = 0; }
        DWORD code = 0;
        GetExitCodeProcess(probeproc, &code);
        closeprobe();
        if(probegotresult) { probedone(); return; }
        // Exception codes (0x8xxxxxxx breakpoints included), not a plain exit.
        if(code >= 0x80000000U && code != STILL_ACTIVE)
        {
            logoutf("hwrt vk: [%u ms] the probe crashed (0x%08X) after %u ms", SDL_GetTicks(), unsigned(code), SDL_GetTicks() - probestart);
            defformatstring(what, "probe process exception 0x%08X", unsigned(code));
            probefailed(HWRT_WHY_CRASH, what);
            return;
        }
        conoutf(CON_WARN, "hwrt vk: probe ended without an answer (exit code %u), staying on OpenGL", unsigned(code));
        if(vkretrying || vkskipping) { vkretrying = false; vkundoskip("the probe ended without an answer"); }
        hwrtunavailable(HWRT_WHY_FAILED);
        hwrtvkstate = HWRT_VK_FAILED;
        return;
    }
    Uint32 limit = vkprobetimeoutms ? vkprobetimeoutms : Uint32(hwrtvktimeout) * 1000;
    if(SDL_GetTicks() - probestart >= limit)
    {
        conoutf(CON_WARN, "hwrt vk: probe did not answer within %u ms, killing it (last line above shows the step it was in)", limit);
        closeprobe();
        probefailed(HWRT_WHY_TIMEOUT, "while probing, see the last \"hwrt vkprobe\" line");
    }
}

static void startprobe()
{
    if(hwrtvkstate != HWRT_VK_IDLE) return;
    vkprobeat = 0;
    if(!vkhaveuuid) return;
    vkapplysaved();
    launchprobe();
}

// Starts "sauerbraten.exe -vkprobe <UUID>" (the first one, or the retry).
// False only when a retry could not be started.
static bool launchprobe()
{
    nvkicds = 0;
    vkloaderver = -1;
    vkfaultpath[0] = 0;
    vkfaultfinal = false;
    vkdllwatch = false;
    probenewtype = -1;
    char uuidhex[2*VK_UUID_SIZE + 1];
    loopi(VK_UUID_SIZE) snprintf(&uuidhex[2*i], 3, "%02x", vkgluuid[i]);
    WCHAR exe[MAX_PATH + 1];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    exe[n < MAX_PATH ? n : MAX_PATH] = 0;
    WCHAR cmd[MAX_PATH + 96];
    _snwprintf(cmd, sizeof(cmd)/sizeof(cmd[0]), L"\"%ls\" -vkprobe %hs", exe, uuidhex);
    cmd[sizeof(cmd)/sizeof(cmd[0]) - 1] = 0;

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    BOOL ok = n > 0 && CreatePipe(&rd, &wr, &sa, 0);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    DWORD err = 0;
    if(ok)
    {
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        // Only the pipe is inherited, not the log file or any socket.
        SIZE_T attrsize = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attrsize);
        LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrsize);
        ok = attrs && InitializeProcThreadAttributeList(attrs, 1, 0, &attrsize) &&
             UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &wr, sizeof(wr), NULL, NULL);
        if(ok)
        {
            STARTUPINFOEXW si;
            memset(&si, 0, sizeof(si));
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdOutput = wr;
            si.StartupInfo.hStdError = wr;
            si.lpAttributeList = attrs;
            ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                                NULL, NULL, &si.StartupInfo, &pi);
            if(!ok) err = GetLastError();
            DeleteProcThreadAttributeList(attrs);
        }
        else err = GetLastError();
        if(attrs) HeapFree(GetProcessHeap(), 0, attrs);
        CloseHandle(wr);
    }
    else err = GetLastError();
    if(!ok)
    {
        if(rd) CloseHandle(rd);
        // No probe (blocked by security software?): Vulkan is still tried, on
        // the worker thread with the same timeout, when something needs it.
        if(vkretrying)
        {
            logoutf("hwrt vk: the Vulkan probe could not be started again (error %u)", unsigned(err));
            return false;
        }
        conoutf(CON_WARN, "hwrt vk: the Vulkan probe could not start (error %u); Vulkan will be tried directly, with the start-up timeout, when needed", unsigned(err));
        memset(&hwrtvkprobe, 0, sizeof(hwrtvkprobe));
        hwrtvkprobe.why = HWRT_WHY_NONE;
        hwrtvkprobe.rtwhy = HWRT_WHY_NONE;
        hwrtvkprobe.rayquery = hwrtvkprobe.nvidia = true;
        hwrtvkstate = HWRT_VK_PROBED;
        return true;
    }
    probejob = CreateJobObjectW(NULL, NULL);
    if(probejob)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof(li));
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(probejob, JobObjectExtendedLimitInformation, &li, sizeof(li)) ||
           !AssignProcessToJobObject(probejob, pi.hProcess))
        {
            CloseHandle(probejob);
            probejob = NULL;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    probeproc = pi.hProcess;
    probeout = rd;
    probestart = SDL_GetTicks();
    probegotresult = false;
    probependlen = 0;
    memset(&probenew, 0, sizeof(probenew));
    hwrtvkstate = HWRT_VK_PROBING;
    conoutf(CON_INIT, "hwrt vk: [%u ms] Vulkan probe started (process %u, timeout %u ms)", probestart, unsigned(pi.dwProcessId), vkprobetimeoutms ? vkprobetimeoutms : Uint32(hwrtvktimeout) * 1000);
    return true;
}
#else
static void pollprobe() {}
static void startprobe()
{
    if(hwrtvkstate != HWRT_VK_IDLE || !vkhaveuuid) return;
    memset(&hwrtvkprobe, 0, sizeof(hwrtvkprobe));
    hwrtvkprobe.rayquery = hwrtvkprobe.nvidia = true;
    hwrtvkstate = HWRT_VK_PROBED;
}
#endif

void hwrtvkpoll()
{
    conflushthreaded();
    if(vkprobeat && hwrtvkstate == HWRT_VK_IDLE && SDL_TICKS_PASSED(SDL_GetTicks(), vkprobeat)) startprobe();
    if(hwrtvkstate == HWRT_VK_PROBING) pollprobe();
}

static int SDLCALL vkworker(void *)
{
    bool ok = hwrtinitdevice(vkgluuid);
    SDL_AtomicSet(&vkok, ok ? 1 : 0);
    SDL_AtomicSet(&vkdone, 1);
    return 0;
}

bool hwrtvkensure(const char *why)
{
    if(hwrtvkabandoned) return false;
    if(hwrtvkstate == HWRT_VK_READY) return true;
    if(hwrtvkstate == HWRT_VK_IDLE) startprobe();
    if(hwrtvkstate == HWRT_VK_PROBING)
    {
        conoutf(CON_INIT, "hwrt vk: [%u ms] waiting for the Vulkan probe (%s)", SDL_GetTicks(), why);
        while(hwrtvkstate == HWRT_VK_PROBING)
        {
            pollprobe();
            if(hwrtvkstate == HWRT_VK_PROBING) vkpump();
        }
    }
    if(hwrtvkstate != HWRT_VK_PROBED || hwrtvkprobe.why != HWRT_WHY_NONE) return false;

    Uint32 start = SDL_GetTicks();
    conoutf(CON_INIT, "hwrt vk: [%u ms] starting Vulkan in the game (%s), timeout %d s", start, why, int(hwrtvktimeout));
    hwrtvkstate = HWRT_VK_STARTING;
    SDL_AtomicSet(&vkdone, 0);
    SDL_AtomicSet(&vkok, 0);
    SDL_Thread *thread = SDL_CreateThread(vkworker, "hwrtvk", NULL);
    if(!thread)
    {
        conoutf(CON_WARN, "hwrt vk: no worker thread (%s), starting Vulkan on the main thread", SDL_GetError());
        vkworker(NULL);
    }
    else while(!SDL_AtomicGet(&vkdone))
    {
        if(SDL_GetTicks() - start >= Uint32(hwrtvktimeout) * 1000)
        {
            hwrtvkabandoned = true;
            SDL_AtomicSet(&vkgivenup, 1);
            SDL_DetachThread(thread);
            conflushthreaded();
            const char *step = vkcurstep;
            defformatstring(what, "game start-up, step \"%s\"", step && step[0] ? step : "?");
            vkgiveup(HWRT_WHY_TIMEOUT, what);
            conoutf(CON_WARN, "hwrt vk: the worker thread stays inside the driver (a thread cannot be stopped safely); nothing of Vulkan is used until the game is restarted, and quitting ends the process directly");
            return false;
        }
        vkpump();
    }
    if(thread) SDL_WaitThread(thread, NULL);
    conflushthreaded();
    Uint32 took = SDL_GetTicks() - start;
    if(!SDL_AtomicGet(&vkok))
    {
        conoutf(CON_WARN, "hwrt vk: Vulkan start-up failed after %u ms, staying on OpenGL", took);
        hwrtdestroytrace();
        hwrtdestroydevice();
        hwrtunavailable(HWRT_WHY_FAILED);
        hwrtvkstate = HWRT_VK_FAILED;
        return false;
    }
    conoutf(CON_INIT, "hwrt vk: [%u ms] Vulkan ready in the game in %u ms", SDL_GetTicks(), took);
    hwrtvkstate = HWRT_VK_READY;
    if(!hwrtvkonready()) return false;
    hwrtvkstall = 0;
    return true;
}

// ---- The probe process: sauerbraten.exe -vkprobe <GL device UUID> ----------
// Started before anything else in main(): no window, no SDL, no log file, no
// profile. Everything it sees goes to stdout (a pipe the game reads), one line
// per step with the time since it started, then a "result" line, then it ends
// itself with TerminateProcess so a layer stuck in its exit cannot hold it.
#ifdef WIN32
static HANDLE poutfile = NULL;
static ULONGLONG pt0 = 0;

static void pout(const char *fmt, ...)
{
    char buf[1400];
    int n = _snprintf(buf, sizeof(buf), "+%5u ms ", unsigned(GetTickCount64() - pt0));
    if(n < 0) n = 0;
    va_list args;
    va_start(args, fmt);
    int m = _vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, args);
    va_end(args);
    if(m < 0 || m > int(sizeof(buf)) - n - 2) m = int(sizeof(buf)) - n - 2;
    n += m;
    buf[n++] = '\n';
    DWORD w = 0;
    if(poutfile) WriteFile(poutfile, buf, DWORD(n), &w, NULL);
}

// "name" and the first "disable_environment" key of a layer manifest (JSON),
// read as text: enough to tell a player which variable switches a layer off
// for one test launch.
static void manifestinfo(const char *path, char *name, int namelen, char *disable, int dislen)
{
    name[0] = disable[0] = 0;
    FILE *f = fopen(path, "rb");
    if(!f) { copystring(name, "(manifest not readable)", namelen); return; }
    static char text[65536];
    size_t len = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[len] = 0;
    struct { const char *key; char *out; int outlen; } keys[2] = { { "\"name\"", name, namelen }, { "\"disable_environment\"", disable, dislen } };
    loopk(2)
    {
        const char *p = strstr(text, keys[k].key);
        if(!p) continue;
        p += strlen(keys[k].key);
        if(k == 1) { p = strchr(p, '{'); if(!p) continue; }
        else { p = strchr(p, ':'); if(!p) continue; }
        p = strchr(p, '"');
        if(!p) continue;
        const char *e = strchr(++p, '"');
        if(!e) continue;
        int l = min(int(e - p), keys[k].outlen - 1);
        memcpy(keys[k].out, p, l);
        keys[k].out[l] = 0;
    }
}

static char poverlay[MAX_PATH + 1] = "";   // first enabled implicit layer with a disable variable
static char plastoverlay[MAX_PATH + 1] = "";   // and the last one

static void listregistrylayers(HKEY root, const char *rootname, const char *subkey, const char *kind)
{
    HKEY k;
    if(RegOpenKeyExA(root, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    for(DWORD i = 0; i < 256; i++)
    {
        char valname[1024];
        DWORD vlen = sizeof(valname), type = 0, data = 0, dlen = sizeof(data);
        LONG r = RegEnumValueA(k, i, valname, &vlen, NULL, &type, (BYTE *)&data, &dlen);
        if(r == ERROR_NO_MORE_ITEMS) break;
        if(r != ERROR_SUCCESS) continue;
        char name[128], disable[128];
        manifestinfo(valname, name, sizeof(name), disable, sizeof(disable));
        pout("%s (%s): %s [%s] name=%s%s%s", kind, rootname, valname, type == REG_DWORD && data == 0 ? "enabled" : "disabled",
             name, disable[0] ? " off_with=" : "", disable);
        if(!poverlay[0] && type == REG_DWORD && data == 0 && disable[0]) copystring(poverlay, valname, sizeof(poverlay));
        if(type == REG_DWORD && data == 0 && disable[0]) copystring(plastoverlay, valname, sizeof(plastoverlay));
    }
    RegCloseKey(k);
}

// Vulkan drivers (ICDs) as the loader finds them on Windows, one parseable
// line each ("icd vendor=0x.... ... manifest=<path>", path last): the game
// uses this list to leave out a driver that crashed the probe.
struct probeicdinfo { int vendor; char manifest[MAX_PATH + 1]; };
static probeicdinfo picds[16];
static int npicds = 0;

static void probeicd(const char *source, int vendor, const char *manifest)
{
    loopi(npicds) if(!_stricmp(picds[i].manifest, manifest)) return;
    static char text[65536];
    char lib[MAX_PATH] = "", api[32] = "?";
    if(readtext(manifest, text, sizeof(text)) > 0)
    {
        jsonstring(text, "library_path", lib, sizeof(lib));
        jsonstring(text, "api_version", api, sizeof(api));
    }
    else copystring(lib, "(manifest not readable)", sizeof(lib));
    if(!vendor) vendor = vendorfromfile(lib[0] && lib[0] != '(' ? lib : manifest);
    if(!vendor) vendor = vendorfromfile(manifest);
    if(npicds < int(sizeof(picds)/sizeof(picds[0])))
    {
        picds[npicds].vendor = vendor;
        copystring(picds[npicds].manifest, manifest, sizeof(picds[npicds].manifest));
        npicds++;
    }
    pout("icd vendor=0x%04X (%s) from %s, library %s, api %s, manifest=%s", vendor, vendorname(vendor), source, lib, api, manifest);
}

static void listregistryicds(HKEY root, const char *rootname)
{
    HKEY k;
    if(RegOpenKeyExA(root, "SOFTWARE\\Khronos\\Vulkan\\Drivers", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    for(DWORD i = 0; i < 64; i++)
    {
        char valname[1024];
        DWORD vlen = sizeof(valname), type = 0, data = 0, dlen = sizeof(data);
        LONG r = RegEnumValueA(k, i, valname, &vlen, NULL, &type, (BYTE *)&data, &dlen);
        if(r == ERROR_NO_MORE_ITEMS) break;
        if(r != ERROR_SUCCESS) continue;
        bool on = type == REG_DWORD && data == 0;
        if(!on) { pout("driver ICD (%s Khronos\\Vulkan\\Drivers): %s [disabled]", rootname, valname); continue; }
        defformatstring(src, "%s Khronos\\Vulkan\\Drivers", rootname);
        probeicd(src, 0, valname);
    }
    RegCloseKey(k);
}

static void listadapterregistry()
{
    // Drivers register their ICD and their own implicit layers under the display adapter class.
    loopi(16)
    {
        defformatstring(sub, "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\%04d", i);
        char desc[256] = "";
        DWORD len = sizeof(desc);
        if(RegGetValueA(HKEY_LOCAL_MACHINE, sub, "DriverDesc", RRF_RT_REG_SZ, NULL, desc, &len) != ERROR_SUCCESS) continue;
        char ver[64] = "";
        len = sizeof(ver);
        RegGetValueA(HKEY_LOCAL_MACHINE, sub, "DriverVersion", RRF_RT_REG_SZ, NULL, ver, &len);
        char hwid[256] = "";
        len = sizeof(hwid);
        RegGetValueA(HKEY_LOCAL_MACHINE, sub, "MatchingDeviceId", RRF_RT_REG_SZ, NULL, hwid, &len);
        unsigned vendor = 0;
        const char *ven = stristr(hwid, "VEN_");
        if(ven) sscanf(ven + 4, "%4x", &vendor);
        pout("adapter %04d: %s, vendor 0x%04X (driver %s)", i, desc, vendor, ver);
        static const char * const vals[] = { "VulkanDriverName", "VulkanImplicitLayers" };
        loopj(2)
        {
            static char multi[8192];
            len = sizeof(multi) - 2;
            memset(multi, 0, sizeof(multi));
            if(RegGetValueA(HKEY_LOCAL_MACHINE, sub, vals[j], RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, NULL, multi, &len) != ERROR_SUCCESS) continue;
            // Some drivers store these as REG_MULTI_SZ in UTF-16 behind the first
            // entry: stop at anything that is not a plausible path.
            for(const char *p = multi; strlen(p) >= 4 && p < multi + len; p += strlen(p) + 1)
            {
                if(j == 0) { defformatstring(src, "adapter %04d", i); probeicd(src, int(vendor), p); continue; }
                char name[128], disable[128];
                manifestinfo(p, name, sizeof(name), disable, sizeof(disable));
                pout("  driver implicit layer: %s name=%s%s%s", p, name, disable[0] ? " off_with=" : "", disable);
            }
        }
    }
    // Some drivers register their ICD as a software component instead.
    loopi(32)
    {
        defformatstring(sub, "SYSTEM\\CurrentControlSet\\Control\\Class\\{5c4c3332-344d-483c-8739-259e934c9cc8}\\%04d", i);
        static char multi[8192];
        DWORD len = sizeof(multi) - 2;
        memset(multi, 0, sizeof(multi));
        if(RegGetValueA(HKEY_LOCAL_MACHINE, sub, "VulkanDriverName", RRF_RT_REG_SZ | RRF_RT_REG_MULTI_SZ, NULL, multi, &len) != ERROR_SUCCESS) continue;
        for(const char *p = multi; strlen(p) >= 4 && p < multi + len; p += strlen(p) + 1)
        {
            defformatstring(src, "software component %04d", i);
            probeicd(src, 0, p);
        }
    }
}

static void fileversion(HMODULE m, char *out, int len)
{
    copystring(out, "?", len);
    HRSRC r = FindResourceA(m, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(16));
    if(!r) return;
    HGLOBAL g = LoadResource(m, r);
    const uchar *p = g ? (const uchar *)LockResource(g) : NULL;
    DWORD sz = SizeofResource(m, r);
    if(!p) return;
    for(DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= sz; i += 4)
    {
        const VS_FIXEDFILEINFO *fi = (const VS_FIXEDFILEINFO *)(p + i);
        if(fi->dwSignature != 0xFEEF04BD) continue;
        nformatstring(out, len, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                      HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        return;
    }
}

// The module a crash is in, so a report names the driver (e.g. amdxc64.dll in
// the AMD driver's folder). First-chance faults are printed too (a few): a
// driver may handle its own; the "unhandled" line is the one that ended it.
static volatile LONG pfaults = 0;

static void probefault(const char *kind, const EXCEPTION_RECORD *er)
{
    void *addr = er->ExceptionAddress;
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
        GetModuleFileNameA(m, path, MAX_PATH);
    pout("%s 0x%08X at %s+0x%llx (%s)", kind, unsigned(er->ExceptionCode), m ? pathbase(path) : "?",
         (unsigned long long)((const char *)addr - (const char *)m), path);
}

static LONG CALLBACK probeveh(EXCEPTION_POINTERS *ep)
{
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    bool fatal = c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_PRIV_INSTRUCTION ||
                 c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_IN_PAGE_ERROR || c == 0xC0000374U /* heap corruption */;
    if(fatal && InterlockedIncrement(&pfaults) <= 4) probefault("fault (first chance)", ep->ExceptionRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI probeunhandled(EXCEPTION_POINTERS *ep)
{
    probefault("unhandled", ep->ExceptionRecord);
    return EXCEPTION_EXECUTE_HANDLER;   // ends the process with the exception code
}

// For the simulations: is the faulty part of this driver left out here?
// NEEDDRIVER: the driver itself must be left out, its library included (the
// same manifest's implicit layer switched off too); otherwise its layer alone.
static bool probeskips(const char *manifest, bool needdriver)
{
    bool skip = !needdriver;
    const char *dis = getenv("VK_LOADER_DRIVERS_DISABLE");
    if(dis && globlistmatch(dis, pathbase(manifest))) skip = true;
    const char *files = getenv("VK_DRIVER_FILES");
    if(!files || !files[0]) files = getenv("VK_ICD_FILENAMES");
    if(files && files[0] && !stristr(files, manifest)) skip = true;
    if(!skip) return false;
    static char text[65536];
    if(readtext(manifest, text, sizeof(text)) <= 0) return true;
    vkenvpair pairs[8];
    int np = manifestdisableenv(text, pairs, 8);
    loopi(np) if(!getenv(pairs[i].name))
    {
        if(needdriver) pout("driver %s is left out, but its implicit layer is still on (%s not set): its library is still loaded", pathbase(manifest), pairs[i].name);
        return false;
    }
    if(!needdriver && !np) return false;   // no layer to switch off: the faulty part stays
    return true;
}

// Every DLL loaded outside the Windows folder (driver files in the
// DriverStore included), as it is loaded: the last driver library before a
// crash or a hang is in the log, and the game knows which drivers were
// really loaded (a GPU Windows reports in error is listed in the registry
// but never loaded by the Vulkan loader).
struct probeustr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct probedllnote { ULONG Flags; const probeustr *FullDllName; const probeustr *BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID (CALLBACK *probedllfn)(ULONG reason, const probedllnote *data, PVOID ctx);
typedef LONG (NTAPI *probeldrregister)(ULONG flags, probedllfn fn, PVOID ctx, PVOID *cookie);
static char probewindir[MAX_PATH] = "";

static VOID CALLBACK probedllloaded(ULONG reason, const probedllnote *data, PVOID)
{
    if(reason != 1 || !data || !data->FullDllName || !data->FullDllName->Buffer) return;   // 1 = loaded
    char path[MAX_PATH * 2];
    int n = WideCharToMultiByte(CP_ACP, 0, data->FullDllName->Buffer, data->FullDllName->Length / 2, path, sizeof(path) - 1, NULL, NULL);
    if(n <= 0) return;
    path[n] = 0;
    size_t wl = strlen(probewindir);
    if(wl && !_strnicmp(path, probewindir, wl) && !stristr(path, "\\DriverStore\\")) return;
    pout("dll loaded %s", path);
}

static void probewatchdlls()
{
    GetWindowsDirectoryA(probewindir, sizeof(probewindir));
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    probeldrregister reg = ntdll ? (probeldrregister)(void *)GetProcAddress(ntdll, "LdrRegisterDllNotification") : NULL;
    PVOID cookie = NULL;
    if(reg && reg(0, probedllloaded, NULL, &cookie) >= 0) pout("dll notifications on (DLLs outside %s, and driver files, are listed as they load)", probewindir);
    else pout("dll notifications not available");
}

static bool probeanswered = false;

static int probetype = -1;   // VkPhysicalDeviceType of the GPU OpenGL runs on

static void probeanswer(int why, int rtwhy, bool rq, bool nvidia, const char *name)
{
    if(probeanswered) return;
    probeanswered = true;
    pout("result why=%d rtwhy=%d rq=%d nvidia=%d type=%d name=%s", why, rtwhy, rq ? 1 : 0, nvidia ? 1 : 0, probetype, name ? name : "");
}

static int probeend(int why, int rtwhy, bool rq, bool nvidia, const char *name)
{
    probeanswer(why, rtwhy, rq, nvidia, name);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

static bool probehasext(const VkExtensionProperties *exts, uint32_t n, const char *name)
{
    loopi(int(n)) if(!strcmp(exts[i].extensionName, name)) return true;
    return false;
}

int hwrtvkprobemain(int argc, char **argv)
{
    pt0 = GetTickCount64();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    poutfile = GetStdHandle(STD_OUTPUT_HANDLE);
    AddVectoredExceptionHandler(1, probeveh);
    SetUnhandledExceptionFilter(probeunhandled);
    probewatchdlls();
    uint8_t gluuid[VK_UUID_SIZE];
    memset(gluuid, 0, sizeof(gluuid));
    if(argc >= 3 && strlen(argv[2]) == 2*VK_UUID_SIZE)
        loopi(VK_UUID_SIZE) { unsigned v = 0; sscanf(&argv[2][2*i], "%2x", &v); gluuid[i] = uchar(v); }
    const char *sim = getenv("SAUER_HWRT_SIMULATE");
    if(!sim) sim = "";
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        pout("probe %04d-%02d-%02d %02d:%02d:%02d, process %u, GL device %s%s%s", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             unsigned(GetCurrentProcessId()), argc >= 3 ? argv[2] : "?", sim[0] ? ", SAUER_HWRT_SIMULATE=" : "", sim);
    }
    // Variables that change what the loader loads (VK_*), as set for this run.
    {
        LPCH env = GetEnvironmentStringsA();
        if(env)
        {
            for(const char *p = env; *p; p += strlen(p) + 1) if(!strncmp(p, "VK_", 3) || !strncmp(p, "DISABLE_LAYER_", 14)) pout("env %s", p);
            FreeEnvironmentStringsA(env);
        }
    }
    listregistrylayers(HKEY_LOCAL_MACHINE, "HKLM", "SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", "implicit layer");
    listregistrylayers(HKEY_CURRENT_USER, "HKCU", "SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", "implicit layer");
    listregistryicds(HKEY_LOCAL_MACHINE, "HKLM");
    listregistryicds(HKEY_CURRENT_USER, "HKCU");
    listadapterregistry();
    pout("%d Vulkan driver(s) registered", npicds);

    if(!strcmp(sim, "novulkan"))
    {
        pout("no Vulkan loader (vulkan-1.dll, simulated)");
        return probeend(HWRT_WHY_NOVULKAN, HWRT_WHY_NOVULKAN, false, false, "");
    }
    ULONGLONG t = GetTickCount64();
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if(!lib)
    {
        pout("no Vulkan loader (vulkan-1.dll, error %u)", unsigned(GetLastError()));
        return probeend(HWRT_WHY_NOVULKAN, HWRT_WHY_NOVULKAN, false, false, "");
    }
    {
        char path[MAX_PATH] = "", ver[64];
        GetModuleFileNameA(lib, path, MAX_PATH);
        fileversion(lib, ver, sizeof(ver));
        pout("loader %s, file version %s (%u ms)", path, ver, unsigned(GetTickCount64() - t));
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if(!gipa) { pout("vkGetInstanceProcAddr missing"); return probeend(HWRT_WHY_DRIVER, HWRT_WHY_DRIVER, false, false, ""); }
#define PGLOBAL(name) PFN_##name p_##name = (PFN_##name)gipa(VK_NULL_HANDLE, #name);
    PGLOBAL(vkEnumerateInstanceVersion)
    PGLOBAL(vkEnumerateInstanceLayerProperties)
    PGLOBAL(vkEnumerateInstanceExtensionProperties)
    PGLOBAL(vkCreateInstance)
#undef PGLOBAL
    if(!p_vkCreateInstance) { pout("vkCreateInstance missing"); return probeend(HWRT_WHY_DRIVER, HWRT_WHY_DRIVER, false, false, ""); }
    uint32_t loaderver = VK_API_VERSION_1_0;
    if(p_vkEnumerateInstanceVersion) p_vkEnumerateInstanceVersion(&loaderver);
    if(!strcmp(sim, "crashigpuold"))
        pout("loader Vulkan 1.3.200 (SAUER_HWRT_SIMULATE=crashigpuold, really %d.%d.%d)", VK_API_VERSION_MAJOR(loaderver), VK_API_VERSION_MINOR(loaderver), VK_API_VERSION_PATCH(loaderver));
    else pout("loader Vulkan %d.%d.%d", VK_API_VERSION_MAJOR(loaderver), VK_API_VERSION_MINOR(loaderver), VK_API_VERSION_PATCH(loaderver));
    if(p_vkEnumerateInstanceLayerProperties)
    {
        pout("vkEnumerateInstanceLayerProperties...");
        t = GetTickCount64();
        uint32_t n = 0;
        p_vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties *layers = new VkLayerProperties[max(n, 1U)];
        if(p_vkEnumerateInstanceLayerProperties(&n, layers) != VK_SUCCESS) n = 0;
        pout("%u layer(s) (%u ms)", n, unsigned(GetTickCount64() - t));
        loopi(int(n)) pout("  layer %s %d.%d.%d impl %u: %s", layers[i].layerName, VK_API_VERSION_MAJOR(layers[i].specVersion),
                           VK_API_VERSION_MINOR(layers[i].specVersion), VK_API_VERSION_PATCH(layers[i].specVersion),
                           layers[i].implementationVersion, layers[i].description);
        delete[] layers;
    }
    if(!strcmp(sim, "crashsearch") && plastoverlay[0])
    {
        // As reported by a player: the crash is in amdxc64.dll (the AMD
        // driver's folder), but only switching off another implicit layer (here
        // the last tool layer installed) stops it.
        static char text[65536];
        vkenvpair pairs[4];
        int np = readtext(plastoverlay, text, sizeof(text)) > 0 ? manifestdisableenv(text, pairs, 4) : 0;
        const char *dis = getenv("VK_LOADER_LAYERS_DISABLE");
        bool off = (dis && strstr(dis, "~implicit~")) || (np && getenv(pairs[0].name));
        char amd[MAX_PATH + 1] = "C:\\Windows\\System32\\DriverStore\\FileRepository\\u0000000.inf_amd64_simulated\\";
        loopi(npicds) if(picds[i].vendor == 0x1002) { copystring(amd, picds[i].manifest, sizeof(amd)); amd[pathbase(amd) - amd] = 0; break; }
        if(off) pout("simulated faulty layer %s is switched off: no crash (SAUER_HWRT_SIMULATE=crashsearch)", pathbase(plastoverlay));
        else
        {
            pout("vkCreateInstance... (SAUER_HWRT_SIMULATE=crashsearch: layer %s makes the AMD driver crash)", pathbase(plastoverlay));
            pout("unhandled 0xC0000005 at amdxc64.dll+0xa5e5f (%samdxc64.dll, simulated)", amd);
            TerminateProcess(GetCurrentProcess(), 0xC0000005);
        }
    }
    if((!strcmp(sim, "crashoverlay") || !strcmp(sim, "hangoverlay")) && poverlay[0])
    {
        // A tool's implicit layer (the first one installed) that crashes or
        // hangs whenever it is loaded, unless it is switched off.
        static char text[65536];
        vkenvpair pairs[4];
        int np = readtext(poverlay, text, sizeof(text)) > 0 ? manifestdisableenv(text, pairs, 4) : 0;
        char lib[MAX_PATH] = "";
        jsonstring(text, "library_path", lib, sizeof(lib));
        const char *dis = getenv("VK_LOADER_LAYERS_DISABLE");
        bool off = (dis && strstr(dis, "~implicit~")) || (np && getenv(pairs[0].name));
        char dir[MAX_PATH + 1];
        copystring(dir, poverlay, sizeof(dir));
        dir[pathbase(dir) - dir] = 0;
        if(off) pout("simulated faulty layer %s is switched off: no %s (SAUER_HWRT_SIMULATE=%s)", pathbase(poverlay), sim[0] == 'c' ? "crash" : "hang", sim);
        else if(sim[0] == 'h')
        {
            pout("vkCreateInstance... (SAUER_HWRT_SIMULATE=hangoverlay: layer %s never returns)", pathbase(poverlay));
            for(;;) Sleep(1000);
        }
        else
        {
            pout("vkCreateInstance... (SAUER_HWRT_SIMULATE=crashoverlay: layer %s crashes)", pathbase(poverlay));
            pout("unhandled 0xC0000005 at %s+0x1234 (%s%s, simulated)", pathbase(lib), dir, pathbase(lib));
            TerminateProcess(GetCurrentProcess(), 0xC0000005);
        }
    }
    if(!strcmp(sim, "crashigpu") || !strcmp(sim, "crashigpuold") || !strcmp(sim, "crashigpulayer") || !strcmp(sim, "hangigpu"))
    {
        // crashigpu(old): only leaving out the whole driver helps; crashigpulayer
        // and hangigpu: switching off its implicit layer is enough.
        bool needdriver = !strcmp(sim, "crashigpu") || !strcmp(sim, "crashigpuold");
        // An integrated GPU's driver that crashes (or hangs) whenever it is loaded:
        // the first AMD/Intel driver found, or a made-up AMD one.
        char victim[MAX_PATH + 1] = "";
        loopi(npicds) if(picds[i].vendor == 0x1002 || picds[i].vendor == 0x8086) { copystring(victim, picds[i].manifest, sizeof(victim)); break; }
        if(!victim[0])
        {
            copystring(victim, "C:\\Windows\\System32\\DriverStore\\FileRepository\\u0000000.inf_amd64_simulated\\amd-vulkan64.json", sizeof(victim));
            pout("icd vendor=0x1002 (AMD) from simulation, library .\\amdvlk64.dll, api 1.3.280, manifest=%s", victim);
        }
        if(probeskips(victim, needdriver)) pout("simulated faulty %s of %s is left out: no %s (SAUER_HWRT_SIMULATE=%s)", needdriver ? "driver" : "layer", pathbase(victim), strcmp(sim, "hangigpu") ? "crash" : "hang", sim);
        else if(!strcmp(sim, "hangigpu"))
        {
            char dir[MAX_PATH + 1];
            copystring(dir, victim, sizeof(dir));
            dir[pathbase(dir) - dir] = 0;
            pout("vkEnumerateInstanceExtensionProperties... (SAUER_HWRT_SIMULATE=hangigpu: %s never returns)", pathbase(victim));
            pout("dll loaded %samdvlk64.dll (simulated)", dir);
            for(;;) Sleep(1000);
        }
        else
        {
            char dir[MAX_PATH + 1];
            copystring(dir, victim, sizeof(dir));
            dir[pathbase(dir) - dir] = 0;
            pout("vkEnumerateInstanceExtensionProperties... (SAUER_HWRT_SIMULATE=%s: %s crashes)", sim, pathbase(victim));
            pout("unhandled 0xC0000005 at amdxc64.dll+0xa5e5f (%samdxc64.dll, simulated)", dir);
            TerminateProcess(GetCurrentProcess(), 0xC0000005);
        }
    }
    if(p_vkEnumerateInstanceExtensionProperties)
    {
        pout("vkEnumerateInstanceExtensionProperties...");
        t = GetTickCount64();
        uint32_t n = 0;
        p_vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties *exts = new VkExtensionProperties[max(n, 1U)];
        if(p_vkEnumerateInstanceExtensionProperties(NULL, &n, exts) != VK_SUCCESS) n = 0;
        pout("%u instance extension(s) (%u ms)", n, unsigned(GetTickCount64() - t));
        delete[] exts;
    }

    if(!strcmp(sim, "hang"))
    {
        pout("vkCreateInstance... (SAUER_HWRT_SIMULATE=hang: never returns)");
        for(;;) Sleep(1000);
    }
    if(!strcmp(sim, "crash"))
    {
        pout("vkCreateInstance... (SAUER_HWRT_SIMULATE=crash: access violation)");
        *(volatile int *)NULL = 0;
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Sauerbraten";
    app.pEngineName = "cube2";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    pout("vkCreateInstance...");
    t = GetTickCount64();
    VkResult r = p_vkCreateInstance(&info, NULL, &inst);
    if(r == VK_ERROR_INCOMPATIBLE_DRIVER)
    {
        app.apiVersion = VK_API_VERSION_1_1;
        r = p_vkCreateInstance(&info, NULL, &inst);
    }
    pout("vkCreateInstance %s (%u ms)", hwrtresultstr(r), unsigned(GetTickCount64() - t));
    if(r != VK_SUCCESS)
    {
        int why = r == VK_ERROR_INCOMPATIBLE_DRIVER || r == VK_ERROR_INITIALIZATION_FAILED ? HWRT_WHY_NOVULKAN : HWRT_WHY_FAILED;
        return probeend(why, why, false, false, "");
    }
#define PINST(name) PFN_##name p_##name = (PFN_##name)gipa(inst, #name);
    PINST(vkDestroyInstance)
    PINST(vkEnumeratePhysicalDevices)
    PINST(vkGetPhysicalDeviceProperties2)
    PINST(vkGetPhysicalDeviceFeatures2)
    PINST(vkEnumerateDeviceExtensionProperties)
    PINST(vkGetPhysicalDeviceQueueFamilyProperties)
    PINST(vkCreateDevice)
    PINST(vkDestroyDevice)
#undef PINST
    if(!p_vkEnumeratePhysicalDevices || !p_vkGetPhysicalDeviceProperties2 || !p_vkGetPhysicalDeviceFeatures2 ||
       !p_vkEnumerateDeviceExtensionProperties || !p_vkGetPhysicalDeviceQueueFamilyProperties || !p_vkCreateDevice || !p_vkDestroyDevice)
    {
        pout("Vulkan instance is missing core functions");
        return probeend(HWRT_WHY_DRIVER, HWRT_WHY_DRIVER, false, false, "");
    }
    pout("vkEnumeratePhysicalDevices...");
    t = GetTickCount64();
    uint32_t numdevs = 0;
    r = p_vkEnumeratePhysicalDevices(inst, &numdevs, NULL);
    VkPhysicalDevice *devs = new VkPhysicalDevice[max(numdevs, 1U)];
    if(r == VK_SUCCESS) r = p_vkEnumeratePhysicalDevices(inst, &numdevs, devs);
    pout("%u GPU(s), %s (%u ms)", numdevs, hwrtresultstr(r), unsigned(GetTickCount64() - t));
    if(r != VK_SUCCESS || !numdevs) return probeend(HWRT_WHY_NOVULKAN, HWRT_WHY_NOVULKAN, false, false, "");

    int match = -1, why = HWRT_WHY_NOMATCH, rtwhy = HWRT_WHY_NOMATCH;
    bool rq = false, nvidia = false;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "";
    loopi(int(numdevs))
    {
        VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &idp;
        p_vkGetPhysicalDeviceProperties2(devs[i], &p2);
        const VkPhysicalDeviceProperties &pp = p2.properties;
        char drv[2*VK_MAX_DRIVER_NAME_SIZE + 4] = "";
        bool dev12 = VK_API_VERSION_MAJOR(pp.apiVersion) > 1 || VK_API_VERSION_MINOR(pp.apiVersion) >= 2;
        if(dev12)
        {
            VkPhysicalDeviceDriverProperties dp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2 q2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            q2.pNext = &dp;
            p_vkGetPhysicalDeviceProperties2(devs[i], &q2);
            nformatstring(drv, sizeof(drv), "%s %s", dp.driverName, dp.driverInfo);
        }
        uint32_t ne = 0;
        p_vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, NULL);
        VkExtensionProperties *exts = new VkExtensionProperties[max(ne, 1U)];
        if(p_vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, exts) != VK_SUCCESS) ne = 0;
        bool share = probehasext(exts, ne, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) && probehasext(exts, ne, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
        bool rtexts = probehasext(exts, ne, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && probehasext(exts, ne, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                      probehasext(exts, ne, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        delete[] exts;
        bool devrq = false;
        if(rtexts && dev12)
        {
            VkPhysicalDeviceRayQueryFeaturesKHR rqf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
            VkPhysicalDeviceAccelerationStructureFeaturesKHR asf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
            VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            asf.pNext = &rqf;
            f12.pNext = &asf;
            VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            f2.pNext = &f12;
            p_vkGetPhysicalDeviceFeatures2(devs[i], &f2);
            devrq = rqf.rayQuery && asf.accelerationStructure && f12.bufferDeviceAddress;
        }
        if(!strcmp(sim, "nort")) devrq = false;
        bool isgl = match < 0 && !memcmp(idp.deviceUUID, gluuid, VK_UUID_SIZE);
        static const char * const types[] = { "other", "integrated", "discrete", "virtual", "cpu" };
        char uuidhex[2*VK_UUID_SIZE + 1];
        loopj(VK_UUID_SIZE) snprintf(&uuidhex[2*j], 3, "%02x", idp.deviceUUID[j]);
        // driverVersion is vendor-encoded: NVIDIA 10.8.8.6 bits, others the Vulkan version layout.
        char dver[48];
        uint32_t dv = pp.driverVersion;
        if(pp.vendorID == 0x10DE) nformatstring(dver, sizeof(dver), "%u.%u (0x%08X)", (dv >> 22) & 0x3FF, (dv >> 14) & 0xFF, dv);
        else nformatstring(dver, sizeof(dver), "%u.%u.%u (0x%08X)", VK_API_VERSION_MAJOR(dv), VK_API_VERSION_MINOR(dv), VK_API_VERSION_PATCH(dv), dv);
        pout("GPU %d: %s, vendor 0x%04X (%s) device 0x%04X, %s, Vulkan %d.%d.%d, driverVersion %s, driver %s, uuid %s, GL/Vulkan sharing %s, ray query %s%s",
             i, pp.deviceName, pp.vendorID, vendorname(int(pp.vendorID)), pp.deviceID, pp.deviceType <= 4 ? types[pp.deviceType] : "?",
             VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion),
             dver, drv[0] ? drv : "?", uuidhex, share ? "yes" : "no", devrq ? "yes" : "no", isgl ? "  <- the GPU OpenGL runs on" : "");
        if(!isgl) continue;
        match = i;
        probetype = int(pp.deviceType);
        copystring(name, pp.deviceName, sizeof(name));
        nvidia = pp.vendorID == 0x10DE;
        if(!share || (VK_API_VERSION_MAJOR(pp.apiVersion) == 1 && VK_API_VERSION_MINOR(pp.apiVersion) < 1)) why = HWRT_WHY_DRIVER;
        else why = HWRT_WHY_NONE;
        rq = why == HWRT_WHY_NONE && devrq;
        // RT extensions on a Vulkan 1.1 driver: the card can, the driver is behind
        rtwhy = why != HWRT_WHY_NONE ? why : (rq ? HWRT_WHY_NONE : (rtexts && !dev12 ? HWRT_WHY_DRIVER : HWRT_WHY_NORT));
    }
    if(match < 0) pout("no Vulkan GPU matches the GPU OpenGL runs on");

    if(why == HWRT_WHY_NONE)
    {
        // A small device on that GPU, with the sharing extensions the game needs.
        uint32_t nf = 0;
        p_vkGetPhysicalDeviceQueueFamilyProperties(devs[match], &nf, NULL);
        VkQueueFamilyProperties *fams = new VkQueueFamilyProperties[max(nf, 1U)];
        p_vkGetPhysicalDeviceQueueFamilyProperties(devs[match], &nf, fams);
        int family = -1;
        loopi(int(nf)) if(fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { if(family < 0 || (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family = i; if(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) break; }
        delete[] fams;
        if(family < 0) { pout("no compute queue"); why = rtwhy = HWRT_WHY_FAILED; rq = false; }
        else
        {
            float prio = 1.0f;
            VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            qi.queueFamilyIndex = uint32_t(family);
            qi.queueCount = 1;
            qi.pQueuePriorities = &prio;
            const char *dexts[2] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
            VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
            di.queueCreateInfoCount = 1;
            di.pQueueCreateInfos = &qi;
            di.enabledExtensionCount = 2;
            di.ppEnabledExtensionNames = dexts;
            VkDevice dev = VK_NULL_HANDLE;
            pout("vkCreateDevice on GPU %d...", match);
            t = GetTickCount64();
            r = p_vkCreateDevice(devs[match], &di, NULL, &dev);
            pout("vkCreateDevice %s (%u ms)", hwrtresultstr(r), unsigned(GetTickCount64() - t));
            if(r == VK_SUCCESS)
            {
                // Answer now: the game need not wait for the teardown (it ends this process).
                probeanswer(why, rtwhy, rq, nvidia, name);
                t = GetTickCount64();
                p_vkDestroyDevice(dev, NULL);
                pout("vkDestroyDevice (%u ms)", unsigned(GetTickCount64() - t));
            }
            else { why = rtwhy = HWRT_WHY_FAILED; rq = false; }
        }
    }
    delete[] devs;
    if(p_vkDestroyInstance)
    {
        t = GetTickCount64();
        p_vkDestroyInstance(inst, NULL);
        pout("vkDestroyInstance (%u ms)", unsigned(GetTickCount64() - t));
    }
    return probeend(why, rtwhy, rq, nvidia, name);
}
#else
int hwrtvkprobemain(int, char **) { return 1; }
#endif

void hwrtdestroydevice()
{
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    hwrtdlaacleanup();
    hwrtdlaashutdown();
    hwrtfsrshutdown();
    if(hwrtdev.device)
    {
        vkDestroyDevice(hwrtdev.device, NULL);
    }
    if(hwrtdev.instance) vkDestroyInstance(hwrtdev.instance, NULL);
    memset(&hwrtdev, 0, sizeof(hwrtdev));
    unloadvulkanlibrary();
}
