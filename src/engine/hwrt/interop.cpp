// interop.cpp: the GL side of the layer.
//
// One image allocation is exported from Vulkan and imported into GL, so the
// compute shader and the composite quad address the exact same pixels with no
// copy. Two binary semaphores order the two queues against each other; neither
// API ever waits on the CPU for the other.

#include "engine.h"
#include "hwrt/hwrt.h"

// GL_EXT_memory_object / GL_EXT_semaphore and their platform companions are not
// in the glext header SDL ships, so declare the slice we use.
#define GL_TEXTURE_TILING_EXT             0x9580
#define GL_DEDICATED_MEMORY_OBJECT_EXT    0x9581
#define GL_OPTIMAL_TILING_EXT             0x9584
#define GL_LINEAR_TILING_EXT              0x9585
#define GL_HANDLE_TYPE_OPAQUE_FD_EXT      0x9586
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT   0x9587
#define GL_LAYOUT_GENERAL_EXT             0x958D
#define GL_NUM_DEVICE_UUIDS_EXT           0x9596
#define GL_DEVICE_UUID_EXT                0x9597
#define GL_UUID_SIZE_EXT                  16

typedef void (APIENTRYP PFNGLGETUNSIGNEDBYTEIVEXTPROC)(GLenum target, GLuint index, GLubyte *data);
typedef void (APIENTRYP PFNGLCREATEMEMORYOBJECTSEXTPROC)(GLsizei n, GLuint *memoryObjects);
typedef void (APIENTRYP PFNGLDELETEMEMORYOBJECTSEXTPROC)(GLsizei n, const GLuint *memoryObjects);
typedef void (APIENTRYP PFNGLMEMORYOBJECTPARAMETERIVEXTPROC)(GLuint memoryObject, GLenum pname, const GLint *params);
typedef void (APIENTRYP PFNGLTEXSTORAGEMEM2DEXTPROC)(GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, GLuint64 offset);
typedef void (APIENTRYP PFNGLGENSEMAPHORESEXTPROC)(GLsizei n, GLuint *semaphores);
typedef void (APIENTRYP PFNGLDELETESEMAPHORESEXTPROC)(GLsizei n, const GLuint *semaphores);
typedef void (APIENTRYP PFNGLSIGNALSEMAPHOREEXTPROC)(GLuint semaphore, GLuint numBufferBarriers, const GLuint *buffers, GLuint numTextureBarriers, const GLuint *textures, const GLenum *dstLayouts);
typedef void (APIENTRYP PFNGLWAITSEMAPHOREEXTPROC)(GLuint semaphore, GLuint numBufferBarriers, const GLuint *buffers, GLuint numTextureBarriers, const GLuint *textures, const GLenum *srcLayouts);
#ifdef WIN32
typedef void (APIENTRYP PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)(GLuint memory, GLuint64 size, GLenum handleType, void *handle);
typedef void (APIENTRYP PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC)(GLuint semaphore, GLenum handleType, void *handle);
#else
typedef void (APIENTRYP PFNGLIMPORTMEMORYFDEXTPROC)(GLuint memory, GLuint64 size, GLenum handleType, GLint fd);
typedef void (APIENTRYP PFNGLIMPORTSEMAPHOREFDEXTPROC)(GLuint semaphore, GLenum handleType, GLint fd);
#endif

static PFNGLGETUNSIGNEDBYTEIVEXTPROC glGetUnsignedBytei_v_ = NULL;
static PFNGLCREATEMEMORYOBJECTSEXTPROC glCreateMemoryObjects_ = NULL;
static PFNGLDELETEMEMORYOBJECTSEXTPROC glDeleteMemoryObjects_ = NULL;
static PFNGLMEMORYOBJECTPARAMETERIVEXTPROC glMemoryObjectParameteriv_ = NULL;
static PFNGLTEXSTORAGEMEM2DEXTPROC glTexStorageMem2D_ = NULL;
static PFNGLGENSEMAPHORESEXTPROC glGenSemaphores_ = NULL;
static PFNGLDELETESEMAPHORESEXTPROC glDeleteSemaphores_ = NULL;
static PFNGLSIGNALSEMAPHOREEXTPROC glSignalSemaphore_ = NULL;
static PFNGLWAITSEMAPHOREEXTPROC glWaitSemaphore_ = NULL;
#ifdef WIN32
static PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC glImportMemoryWin32Handle_ = NULL;
static PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC glImportSemaphoreWin32Handle_ = NULL;
#else
static PFNGLIMPORTMEMORYFDEXTPROC glImportMemoryFd_ = NULL;
static PFNGLIMPORTSEMAPHOREFDEXTPROC glImportSemaphoreFd_ = NULL;
#endif

hwrtinterop hwrtio;

bool hwrtloadglinterop()
{
    if(glTexStorageMem2D_) return true;

    static const char *const needed[] = {
        "GL_EXT_memory_object", "GL_EXT_semaphore",
#ifdef WIN32
        "GL_EXT_memory_object_win32", "GL_EXT_semaphore_win32"
#else
        "GL_EXT_memory_object_fd", "GL_EXT_semaphore_fd"
#endif
    };
    loopi(int(sizeof(needed)/sizeof(needed[0]))) if(!hasext(needed[i]))
    {
        conoutf(CON_INIT, "hwrt: OpenGL driver has no %s, staying on OpenGL", needed[i]);
        return false;
    }

    glGetUnsignedBytei_v_ = (PFNGLGETUNSIGNEDBYTEIVEXTPROC)getprocaddress("glGetUnsignedBytei_vEXT");
    glCreateMemoryObjects_ = (PFNGLCREATEMEMORYOBJECTSEXTPROC)getprocaddress("glCreateMemoryObjectsEXT");
    glDeleteMemoryObjects_ = (PFNGLDELETEMEMORYOBJECTSEXTPROC)getprocaddress("glDeleteMemoryObjectsEXT");
    glMemoryObjectParameteriv_ = (PFNGLMEMORYOBJECTPARAMETERIVEXTPROC)getprocaddress("glMemoryObjectParameterivEXT");
    glTexStorageMem2D_ = (PFNGLTEXSTORAGEMEM2DEXTPROC)getprocaddress("glTexStorageMem2DEXT");
    glGenSemaphores_ = (PFNGLGENSEMAPHORESEXTPROC)getprocaddress("glGenSemaphoresEXT");
    glDeleteSemaphores_ = (PFNGLDELETESEMAPHORESEXTPROC)getprocaddress("glDeleteSemaphoresEXT");
    glSignalSemaphore_ = (PFNGLSIGNALSEMAPHOREEXTPROC)getprocaddress("glSignalSemaphoreEXT");
    glWaitSemaphore_ = (PFNGLWAITSEMAPHOREEXTPROC)getprocaddress("glWaitSemaphoreEXT");
    bool ok = glGetUnsignedBytei_v_ && glCreateMemoryObjects_ && glDeleteMemoryObjects_ &&
              glMemoryObjectParameteriv_ && glTexStorageMem2D_ && glGenSemaphores_ &&
              glDeleteSemaphores_ && glSignalSemaphore_ && glWaitSemaphore_;
#ifdef WIN32
    glImportMemoryWin32Handle_ = (PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)getprocaddress("glImportMemoryWin32HandleEXT");
    glImportSemaphoreWin32Handle_ = (PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC)getprocaddress("glImportSemaphoreWin32HandleEXT");
    ok = ok && glImportMemoryWin32Handle_ && glImportSemaphoreWin32Handle_;
#else
    glImportMemoryFd_ = (PFNGLIMPORTMEMORYFDEXTPROC)getprocaddress("glImportMemoryFdEXT");
    glImportSemaphoreFd_ = (PFNGLIMPORTSEMAPHOREFDEXTPROC)getprocaddress("glImportSemaphoreFdEXT");
    ok = ok && glImportMemoryFd_ && glImportSemaphoreFd_;
#endif
    if(!ok)
    {
        glTexStorageMem2D_ = NULL;
        conoutf(CON_INIT, "hwrt: OpenGL interop entry points missing, staying on OpenGL");
        return false;
    }
    return true;
}

bool hwrtglgetdeviceuuid(uint8_t *uuid)
{
    if(!glGetUnsignedBytei_v_) return false;
    GLint numuuids = 0;
    glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &numuuids);
    if(numuuids < 1) return false;
    glGetUnsignedBytei_v_(GL_DEVICE_UUID_EXT, 0, (GLubyte *)uuid);
    return true;
}

// ---------------------------------------------------------------------------
// Shared image
// ---------------------------------------------------------------------------

#ifdef WIN32
static const VkExternalMemoryHandleTypeFlagBits HWRT_MEM_HANDLE = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
static const VkExternalSemaphoreHandleTypeFlagBits HWRT_SEM_HANDLE = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
static const VkExternalMemoryHandleTypeFlagBits HWRT_MEM_HANDLE = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
static const VkExternalSemaphoreHandleTypeFlagBits HWRT_SEM_HANDLE = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

static bool checkgl(const char *what)
{
    GLenum err = glGetError();
    if(err == GL_NO_ERROR) return true;
    defformatstring(msg, "OpenGL error 0x%x %s", int(err), what);
    hwrtfail(msg);
    return false;
}

static bool checkglsoft(const char *what)
{
    GLenum err = glGetError();
    if(err == GL_NO_ERROR) return true;
    conoutf(CON_WARN, "hwrt: OpenGL error 0x%x %s", int(err), what);
    return false;
}

#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif

struct hwrtgltimer
{
    GLuint id[HWRT_FRAMES_IN_FLIGHT];
    bool pending[HWRT_FRAMES_IN_FLIGHT];
    int slot;
    bool active;
};

static hwrtgltimer glt[HWRT_GLT_COUNT];
static bool gltok = false;

static float *glttarget(int stage)
{
    switch(stage)
    {
        case HWRT_GLT_MASKW: return &hwrttime.glmaskworld;
        case HWRT_GLT_MASKS: return &hwrttime.glmaskscene;
        case HWRT_GLT_DEPTH: return &hwrttime.gldepth;
        case HWRT_GLT_COMPOSITE: return &hwrttime.glcomposite;
        case HWRT_GLT_HANDOFF: return &hwrttime.glhandoff;
        default: return NULL;
    }
}

void hwrtcleanupgltimes()
{
    if(!gltok) return;
    loopi(HWRT_GLT_COUNT) glDeleteQueries_(HWRT_FRAMES_IN_FLIGHT, glt[i].id);
    memset(glt, 0, sizeof(glt));
    gltok = false;
}

static void gltinit()
{
    if(gltok) return;
    if(!glGenQueries_ || !glBeginQuery_ || !glEndQuery_ || !glGetQueryObjectiv_ || !glGetQueryObjectuiv_) return;
    loopi(HWRT_GLT_COUNT)
    {
        glGenQueries_(HWRT_FRAMES_IN_FLIGHT, glt[i].id);
        memset(glt[i].pending, 0, sizeof(glt[i].pending));
        glt[i].slot = 0;
        glt[i].active = false;
    }
    gltok = true;
}

void hwrtbegingltime(int stage)
{
    gltinit();
    if(!gltok || stage < 0 || stage >= HWRT_GLT_COUNT) return;
    hwrtgltimer &t = glt[stage];
    if(t.active) return;
    float *dst = glttarget(stage);
    if(t.pending[t.slot] && dst)
    {
        GLint avail = 0;
        glGetQueryObjectiv_(t.id[t.slot], GL_QUERY_RESULT_AVAILABLE, &avail);
        if(avail)
        {
            GLuint ns = 0;
            glGetQueryObjectuiv_(t.id[t.slot], GL_QUERY_RESULT, &ns);
            *dst = ns / 1.0e6f;
            t.pending[t.slot] = false;
            hwrttally();
        }
    }
    if(t.pending[t.slot]) return;
    glBeginQuery_(GL_TIME_ELAPSED, t.id[t.slot]);
    t.active = true;
}

void hwrtendgltime(int stage)
{
    if(!gltok || stage < 0 || stage >= HWRT_GLT_COUNT) return;
    hwrtgltimer &t = glt[stage];
    if(!t.active) return;
    glEndQuery_(GL_TIME_ELAPSED);
    t.pending[t.slot] = true;
    t.active = false;
    t.slot = (t.slot + 1) % HWRT_FRAMES_IN_FLIGHT;
}

static bool createimage(int w, int h)
{
    VkExternalMemoryImageCreateInfo extinfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extinfo.handleTypes = HWRT_MEM_HANDLE;

    VkImageCreateInfo imginfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imginfo.pNext = &extinfo;
    imginfo.imageType = VK_IMAGE_TYPE_2D;
    imginfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imginfo.extent.width = w;
    imginfo.extent.height = h;
    imginfo.extent.depth = 1;
    imginfo.mipLevels = 1;
    imginfo.arrayLayers = 1;
    imginfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imginfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imginfo.usage = VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imginfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imginfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    HWRTCHECK(vkCreateImage(hwrtdev.device, &imginfo, NULL, &hwrtio.image), "vkCreateImage");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(hwrtdev.device, hwrtio.image, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(memtype < 0) { hwrtfail("no device local memory type for the shared image"); return false; }

    // NVIDIA requires a dedicated allocation for exported images, and it is the
    // only layout GL's importer understands anyway.
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = hwrtio.image;

    VkExportMemoryAllocateInfo exportinfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exportinfo.handleTypes = HWRT_MEM_HANDLE;
    exportinfo.pNext = &dedicated;

#ifdef WIN32
    VkExportMemoryWin32HandleInfoKHR win32info = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    win32info.dwAccess = GENERIC_ALL;
    exportinfo.pNext = &win32info;
    win32info.pNext = &dedicated;
#endif

    VkMemoryAllocateInfo allocinfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocinfo.pNext = &exportinfo;
    allocinfo.allocationSize = req.size;
    allocinfo.memoryTypeIndex = uint32_t(memtype);
    HWRTCHECK(vkAllocateMemory(hwrtdev.device, &allocinfo, NULL, &hwrtio.memory), "vkAllocateMemory");
    hwrtio.memorysize = req.size;
    HWRTCHECK(vkBindImageMemory(hwrtdev.device, hwrtio.image, hwrtio.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo viewinfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewinfo.image = hwrtio.image;
    viewinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewinfo.format = imginfo.format;
    viewinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewinfo.subresourceRange.levelCount = 1;
    viewinfo.subresourceRange.layerCount = 1;
    HWRTCHECK(vkCreateImageView(hwrtdev.device, &viewinfo, NULL, &hwrtio.view), "vkCreateImageView");
    return true;
}

static bool importimage(int w, int h)
{
#ifdef WIN32
    VkMemoryGetWin32HandleInfoKHR getinfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    getinfo.memory = hwrtio.memory;
    getinfo.handleType = HWRT_MEM_HANDLE;
    HANDLE handle = NULL;
    HWRTCHECK(vkGetMemoryWin32HandleKHR(hwrtdev.device, &getinfo, &handle), "vkGetMemoryWin32HandleKHR");
#else
    VkMemoryGetFdInfoKHR getinfo = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    getinfo.memory = hwrtio.memory;
    getinfo.handleType = HWRT_MEM_HANDLE;
    int handle = -1;
    HWRTCHECK(vkGetMemoryFdKHR(hwrtdev.device, &getinfo, &handle), "vkGetMemoryFdKHR");
#endif

    while(glGetError() != GL_NO_ERROR);
    glCreateMemoryObjects_(1, &hwrtio.glmemory);
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameteriv_(hwrtio.glmemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    if(!checkgl("tagging the memory object as dedicated")) return false;
#ifdef WIN32
    glImportMemoryWin32Handle_(hwrtio.glmemory, hwrtio.memorysize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    hwrtio.memhandle = handle;
#else
    // The fd path transfers ownership to GL, so it must not be closed here.
    glImportMemoryFd_(hwrtio.glmemory, hwrtio.memorysize, GL_HANDLE_TYPE_OPAQUE_FD_EXT, handle);
#endif
    if(!checkgl("importing the Vulkan allocation")) return false;

    glActiveTexture_(GL_TEXTURE0);
    glGenTextures(1, &hwrtio.gltex);
    glBindTexture(GL_TEXTURE_2D, hwrtio.gltex);
    // Must be set before the storage is attached, and must agree with the
    // VK_IMAGE_TILING_OPTIMAL the image was created with.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTexStorageMem2D_(GL_TEXTURE_2D, 1, GL_RGBA16F, w, h, hwrtio.glmemory, 0);
    if(!checkgl("attaching the imported storage to a texture")) return false;
    // Single mip level, so the default mipmapped minification filter would leave
    // the texture incomplete.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLint immutable = 0, gotw = 0, goth = 0, format = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &gotw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &goth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
    glBindTexture(GL_TEXTURE_2D, 0);
    conoutf(CON_INIT, "hwrt: shared texture %dx%d R16G16B16A16_SFLOAT / GL_RGBA16F, immutable %d, internal format 0x%x, %.1f MB",
            int(gotw), int(goth), int(immutable), int(format), hwrtio.memorysize/(1024.0f*1024.0f));

    return checkgl("finishing the imported texture");
}

// ---------------------------------------------------------------------------
// Traced water reflections: a second shared image the lighting pass writes the
// water reflection into (hitlight.comp waterReflection: rgb, then group *
// 65536 + reflected path length, hence 32-bit float). GL draws the particles
// of each traced plane into it and the water shader samples it, nearest, in
// screen space. Same NT-handle rules as the result image; a failure here only
// leaves the water on GL's planar pass.
// ---------------------------------------------------------------------------

struct hwrtreflshared
{
    VkImage image;
    VkDeviceMemory memory;
    VkDeviceSize memorysize;
    VkImageView view;
    GLuint glmemory, gltex;
#ifdef WIN32
    HANDLE memhandle;
#endif
    bool written; // the lighting pass wrote it this frame
};
static hwrtreflshared refl;
// A failure here must not take the RT layer down with it.
#define REFLCHECK(call, what) \
    do { \
        VkResult _reflres = (call); \
        if(_reflres != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: %s failed (%s)", what, hwrtresultstr(_reflres)); return false; } \
    } while(0)

bool hwrtrefllive() { return refl.gltex && refl.written; }
VkImage hwrtreflimage() { return refl.image; }
VkImageView hwrtreflview() { return refl.view; }
GLuint hwrtreflgltex() { return refl.gltex; }
void hwrtsetreflwritten(bool on) { refl.written = on && refl.gltex; }

static void destroyreflimage()
{
    refl.written = false;
    if(refl.gltex) { glDeleteTextures(1, &refl.gltex); refl.gltex = 0; }
    if(refl.glmemory) { glDeleteMemoryObjects_(1, &refl.glmemory); refl.glmemory = 0; }
    if(hwrtdev.device)
    {
        if(refl.view) vkDestroyImageView(hwrtdev.device, refl.view, NULL);
        if(refl.image) vkDestroyImage(hwrtdev.device, refl.image, NULL);
        if(refl.memory) vkFreeMemory(hwrtdev.device, refl.memory, NULL);
    }
    refl.view = VK_NULL_HANDLE;
    refl.image = VK_NULL_HANDLE;
    refl.memory = VK_NULL_HANDLE;
    refl.memorysize = 0;
#ifdef WIN32
    if(refl.memhandle) { CloseHandle(refl.memhandle); refl.memhandle = NULL; }
#endif
}

static bool createreflimage(int w, int h)
{
    VkExternalMemoryImageCreateInfo extinfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extinfo.handleTypes = HWRT_MEM_HANDLE;
    VkImageCreateInfo imginfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imginfo.pNext = &extinfo;
    imginfo.imageType = VK_IMAGE_TYPE_2D;
    imginfo.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    imginfo.extent.width = w;
    imginfo.extent.height = h;
    imginfo.extent.depth = 1;
    imginfo.mipLevels = 1;
    imginfo.arrayLayers = 1;
    imginfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imginfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imginfo.usage = VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imginfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imginfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    REFLCHECK(vkCreateImage(hwrtdev.device, &imginfo, NULL, &refl.image), "vkCreateImage (reflection)");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(hwrtdev.device, refl.image, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(memtype < 0) return false;
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = refl.image;
    VkExportMemoryAllocateInfo exportinfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exportinfo.handleTypes = HWRT_MEM_HANDLE;
    exportinfo.pNext = &dedicated;
#ifdef WIN32
    VkExportMemoryWin32HandleInfoKHR win32info = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    win32info.dwAccess = GENERIC_ALL;
    exportinfo.pNext = &win32info;
    win32info.pNext = &dedicated;
#endif
    VkMemoryAllocateInfo allocinfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocinfo.pNext = &exportinfo;
    allocinfo.allocationSize = req.size;
    allocinfo.memoryTypeIndex = uint32_t(memtype);
    REFLCHECK(vkAllocateMemory(hwrtdev.device, &allocinfo, NULL, &refl.memory), "vkAllocateMemory (reflection)");
    refl.memorysize = req.size;
    REFLCHECK(vkBindImageMemory(hwrtdev.device, refl.image, refl.memory, 0), "vkBindImageMemory (reflection)");
    VkImageViewCreateInfo viewinfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewinfo.image = refl.image;
    viewinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewinfo.format = imginfo.format;
    viewinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewinfo.subresourceRange.levelCount = 1;
    viewinfo.subresourceRange.layerCount = 1;
    REFLCHECK(vkCreateImageView(hwrtdev.device, &viewinfo, NULL, &refl.view), "vkCreateImageView (reflection)");
    return true;
}

static bool importreflimage(int w, int h)
{
#ifdef WIN32
    VkMemoryGetWin32HandleInfoKHR getinfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    getinfo.memory = refl.memory;
    getinfo.handleType = HWRT_MEM_HANDLE;
    HANDLE handle = NULL;
    REFLCHECK(vkGetMemoryWin32HandleKHR(hwrtdev.device, &getinfo, &handle), "vkGetMemoryWin32HandleKHR (reflection)");
#else
    VkMemoryGetFdInfoKHR getinfo = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    getinfo.memory = refl.memory;
    getinfo.handleType = HWRT_MEM_HANDLE;
    int handle = -1;
    REFLCHECK(vkGetMemoryFdKHR(hwrtdev.device, &getinfo, &handle), "vkGetMemoryFdKHR (reflection)");
#endif
    while(glGetError() != GL_NO_ERROR);
    glCreateMemoryObjects_(1, &refl.glmemory);
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameteriv_(refl.glmemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
#ifdef WIN32
    glImportMemoryWin32Handle_(refl.glmemory, refl.memorysize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    refl.memhandle = handle;
#else
    glImportMemoryFd_(refl.glmemory, refl.memorysize, GL_HANDLE_TYPE_OPAQUE_FD_EXT, handle);
#endif
    if(!checkglsoft("importing the reflection image")) return false;
    glActiveTexture_(GL_TEXTURE0);
    glGenTextures(1, &refl.gltex);
    glBindTexture(GL_TEXTURE_2D, refl.gltex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTexStorageMem2D_(GL_TEXTURE_2D, 1, GL_RGBA32F, w, h, refl.glmemory, 0);
    bool ok = checkglsoft("attaching the reflection storage");
    // Nearest: the alpha is an id and a length, a blend of two is neither.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    if(ok) conoutf(CON_INIT, "hwrt: reflection image %dx%d RGBA32F, %.1f MB", w, h, refl.memorysize/(1024.0f*1024.0f));
    return ok;
}

// ---------------------------------------------------------------------------
// Shared R32F depth (GL window depth copied in, Vulkan samples it)
// ---------------------------------------------------------------------------

static GLuint gldepthsrc = 0, gldepthfbo = 0;
static int gldepthsrcw = 0, gldepthsrch = 0;

static void destroylanddepthgl()
{
    if(gldepthfbo) { glDeleteFramebuffers_(1, &gldepthfbo); gldepthfbo = 0; }
    if(gldepthsrc) { glDeleteTextures(1, &gldepthsrc); gldepthsrc = 0; }
    gldepthsrcw = gldepthsrch = 0;
}

// The GL half only. Leaves the Vulkan image and view alive so the lighting
// pipeline's depth descriptor still points at something valid; depthok() is
// false without the GL texture, so nothing ever reads it.
static void destroyshareddepthgl()
{
    destroylanddepthgl();
    if(hwrtio.gldepthtex) { glDeleteTextures(1, &hwrtio.gldepthtex); hwrtio.gldepthtex = 0; }
    if(hwrtio.gldepthmemory) { glDeleteMemoryObjects_(1, &hwrtio.gldepthmemory); hwrtio.gldepthmemory = 0; }
    hwrtio.depthcopied = false;
#ifdef WIN32
    if(hwrtio.depthmemhandle) { CloseHandle(hwrtio.depthmemhandle); hwrtio.depthmemhandle = NULL; }
#endif
}

static void destroyshareddepth()
{
    destroyshareddepthgl();
    if(hwrtdev.device)
    {
        if(hwrtio.depthview) vkDestroyImageView(hwrtdev.device, hwrtio.depthview, NULL);
        if(hwrtio.depthimage) vkDestroyImage(hwrtdev.device, hwrtio.depthimage, NULL);
        if(hwrtio.depthmemory) vkFreeMemory(hwrtdev.device, hwrtio.depthmemory, NULL);
    }
    hwrtio.depthview = VK_NULL_HANDLE;
    hwrtio.depthimage = VK_NULL_HANDLE;
    hwrtio.depthmemory = VK_NULL_HANDLE;
    hwrtio.depthmemorysize = 0;
}

static bool createdepthimage(int w, int h)
{
    VkExternalMemoryImageCreateInfo extinfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extinfo.handleTypes = HWRT_MEM_HANDLE;

    VkImageCreateInfo imginfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imginfo.pNext = &extinfo;
    imginfo.imageType = VK_IMAGE_TYPE_2D;
    imginfo.format = VK_FORMAT_R32_SFLOAT;
    imginfo.extent.width = w;
    imginfo.extent.height = h;
    imginfo.extent.depth = 1;
    imginfo.mipLevels = 1;
    imginfo.arrayLayers = 1;
    imginfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imginfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // COLOR_ATTACHMENT because GL draws the copied window depth into it.
    imginfo.usage = VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imginfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imginfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(hwrtdev.device, &imginfo, NULL, &hwrtio.depthimage);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: shared depth vkCreateImage failed (%s)", hwrtresultstr(r)); return false; }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(hwrtdev.device, hwrtio.depthimage, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(memtype < 0) { conoutf(CON_WARN, "hwrt: no device local memory type for shared depth"); return false; }

    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = hwrtio.depthimage;

    VkExportMemoryAllocateInfo exportinfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exportinfo.handleTypes = HWRT_MEM_HANDLE;
    exportinfo.pNext = &dedicated;

#ifdef WIN32
    VkExportMemoryWin32HandleInfoKHR win32info = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    win32info.dwAccess = GENERIC_ALL;
    exportinfo.pNext = &win32info;
    win32info.pNext = &dedicated;
#endif

    VkMemoryAllocateInfo allocinfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocinfo.pNext = &exportinfo;
    allocinfo.allocationSize = req.size;
    allocinfo.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &allocinfo, NULL, &hwrtio.depthmemory);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: shared depth vkAllocateMemory failed (%s)", hwrtresultstr(r)); return false; }
    hwrtio.depthmemorysize = req.size;
    r = vkBindImageMemory(hwrtdev.device, hwrtio.depthimage, hwrtio.depthmemory, 0);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: shared depth vkBindImageMemory failed (%s)", hwrtresultstr(r)); return false; }

    VkImageViewCreateInfo viewinfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewinfo.image = hwrtio.depthimage;
    viewinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewinfo.format = imginfo.format;
    viewinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewinfo.subresourceRange.levelCount = 1;
    viewinfo.subresourceRange.layerCount = 1;
    r = vkCreateImageView(hwrtdev.device, &viewinfo, NULL, &hwrtio.depthview);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: shared depth vkCreateImageView failed (%s)", hwrtresultstr(r)); return false; }
    return true;
}

static bool importdepthimage(int w, int h)
{
#ifdef WIN32
    VkMemoryGetWin32HandleInfoKHR getinfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    getinfo.memory = hwrtio.depthmemory;
    getinfo.handleType = HWRT_MEM_HANDLE;
    HANDLE handle = NULL;
    VkResult r = vkGetMemoryWin32HandleKHR(hwrtdev.device, &getinfo, &handle);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: shared depth vkGetMemoryWin32HandleKHR failed (%s)", hwrtresultstr(r)); return false; }
#else
    VkMemoryGetFdInfoKHR getinfo = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    getinfo.memory = hwrtio.depthmemory;
    getinfo.handleType = HWRT_MEM_HANDLE;
    int handle = -1;
    VkResult r = vkGetMemoryFdKHR(hwrtdev.device, &getinfo, &handle);
    if(r != VK_SUCCESS) { conoutf(CON_WARN, "hwrt: shared depth vkGetMemoryFdKHR failed (%s)", hwrtresultstr(r)); return false; }
#endif

    while(glGetError() != GL_NO_ERROR);
    glCreateMemoryObjects_(1, &hwrtio.gldepthmemory);
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameteriv_(hwrtio.gldepthmemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    if(!checkglsoft("tagging the depth memory object as dedicated")) return false;
#ifdef WIN32
    glImportMemoryWin32Handle_(hwrtio.gldepthmemory, hwrtio.depthmemorysize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    hwrtio.depthmemhandle = handle;
#else
    glImportMemoryFd_(hwrtio.gldepthmemory, hwrtio.depthmemorysize, GL_HANDLE_TYPE_OPAQUE_FD_EXT, handle);
#endif
    if(!checkglsoft("importing the shared depth allocation")) return false;

    glActiveTexture_(GL_TEXTURE0);
    glGenTextures(1, &hwrtio.gldepthtex);
    glBindTexture(GL_TEXTURE_2D, hwrtio.gldepthtex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTexStorageMem2D_(GL_TEXTURE_2D, 1, GL_R32F, w, h, hwrtio.gldepthmemory, 0);
    if(!checkglsoft("attaching the imported depth storage")) return false;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    conoutf(CON_INIT, "hwrt: shared depth %dx%d R32F, %.1f MB", w, h, hwrtio.depthmemorysize/(1024.0f*1024.0f));
    return checkglsoft("finishing the imported depth texture");
}

static bool setupdepthcopy(int w, int h)
{
    if(!gldepthsrc) glGenTextures(1, &gldepthsrc);
    if(!gldepthfbo) glGenFramebuffers_(1, &gldepthfbo);
    if(gldepthsrcw != w || gldepthsrch != h)
    {
        glBindTexture(GL_TEXTURE_2D, gldepthsrc);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        gldepthsrcw = w;
        gldepthsrch = h;
    }
    glBindFramebuffer_(GL_FRAMEBUFFER, gldepthfbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hwrtio.gldepthtex, 0);
    GLenum status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if(status != GL_FRAMEBUFFER_COMPLETE)
    {
        conoutf(CON_WARN, "hwrt: shared depth FBO incomplete (0x%x)", int(status));
        return false;
    }
    return checkglsoft("setting up the depth copy targets");
}

bool hwrtcopydepth()
{
    hwrtio.depthcopied = false;
    if(!hwrtio.depthok() || !gldepthsrc || !gldepthfbo) return false;

    GLint prevfb = 0, prevactive = 0, prevtex = 0;
    GLboolean blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevactive);
    glActiveTexture_(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex);

    // Copy from the currently bound 3D target (internal scene FBO in Quality,
    // window framebuffer otherwise). Origin remains bottom-left.
    glBindTexture(GL_TEXTURE_2D, gldepthsrc);
    hwrtbegingltime(HWRT_GLT_DEPTH);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, hwrtio.w, hwrtio.h);

    glDisable(GL_BLEND);
    glBindFramebuffer_(GL_FRAMEBUFFER, gldepthfbo);
    SETSHADER(screenrect);
    glBindTexture(GL_TEXTURE_2D, gldepthsrc);
    gle::colorf(1, 1, 1, 1);
    screenquad(1, 1);
    hwrtendgltime(HWRT_GLT_DEPTH);

    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    glBindTexture(GL_TEXTURE_2D, prevtex);
    glActiveTexture_(GLenum(prevactive));
    if(blend) glEnable(GL_BLEND);

    if(!checkglsoft("copying window depth into the shared R32F")) return false;
    hwrtio.depthcopied = true;
    return true;
}

// ---------------------------------------------------------------------------
// Model mask (GL only, no Vulkan involved)
// ---------------------------------------------------------------------------
//
// The composite is a full-screen quad drawn after rendergame(), so an opaque
// mode 6 / 7 result paints over every model GL already rasterised. Two
// snapshots of the same window depth buffer, one taken before the model passes
// and one after, differ at exactly the pixels a model won. Mode 6 discards
// all of those. Mode 7 only discards a *world* hit there (flags, alpha cloth
// that never entered the TLAS) and keeps a shaded model hit. Water, materials,
// grass and particles come after the second snapshot and keep whatever they
// did before.
//
// This pair cannot tell whether a *model* hit is the surface GL drew or one the
// ray reached through an alpha-tested mesh, because neither snapshot knows how
// far away the traced hit is. That test needs the hit distance, so it lives in
// the lighting shader instead (hwrtdepthmask, the shared R32F above).

static GLuint glmaskworld = 0, glmaskscene = 0;
static int glmaskw = 0, glmaskh = 0;
static bool maskworldok = false, masksceneok = false;

static void destroymask()
{
    if(glmaskworld) { glDeleteTextures(1, &glmaskworld); glmaskworld = 0; }
    if(glmaskscene) { glDeleteTextures(1, &glmaskscene); glmaskscene = 0; }
    glmaskw = glmaskh = 0;
    maskworldok = masksceneok = false;
}

static bool setupmask(int w, int h)
{
    if(w <= 0 || h <= 0) return false;
    if(glmaskworld && glmaskscene && glmaskw == w && glmaskh == h) return true;
    if(!glmaskworld) glGenTextures(1, &glmaskworld);
    if(!glmaskscene) glGenTextures(1, &glmaskscene);
    GLuint texs[2] = { glmaskworld, glmaskscene };
    loopi(2)
    {
        glBindTexture(GL_TEXTURE_2D, texs[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glmaskw = w;
    glmaskh = h;
    if(checkglsoft("allocating the model mask depth snapshots")) return true;
    destroymask();
    return false;
}

static bool snapdepth(GLuint tex, int stage)
{
    GLint prevfb = 0, prevactive = 0, prevtex = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevactive);
    glActiveTexture_(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevtex);

    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    glBindTexture(GL_TEXTURE_2D, tex);
    hwrtbegingltime(stage);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, glmaskw, glmaskh);
    hwrtendgltime(stage);

    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    glBindTexture(GL_TEXTURE_2D, prevtex);
    glActiveTexture_(GLenum(prevactive));
    return checkglsoft("snapshotting the window depth for the model mask");
}

void hwrtsnapworlddepth()
{
    maskworldok = masksceneok = false;
    if(!hwrtmasksmodels()) return;
    if(!setupmask(hwrtfbw(), hwrtfbh())) return;
    maskworldok = snapdepth(glmaskworld, HWRT_GLT_MASKW);
}

void hwrtsnapscenedepth()
{
    if(!maskworldok) return;
    masksceneok = snapdepth(glmaskscene, HWRT_GLT_MASKS);
}

static Shader *maskshader()
{
    static Shader *s = NULL;
    if(!s) s = lookupshaderbyname("hwrtcompositemask");
    return s;
}

// The trace asks this too: if the composite cannot mask, primary rays must keep
// committing model hits or the models get painted over.
bool hwrtmaskready()
{
    return masksceneok && glmaskw == hwrtio.w && glmaskh == hwrtio.h && maskshader() != NULL;
}

void hwrtcleanupmask()
{
    destroymask();
}

#ifdef WIN32
static bool createsemaphore(VkSemaphore &vksem, GLuint &glsem, HANDLE &keep)
#else
static bool createsemaphore(VkSemaphore &vksem, GLuint &glsem)
#endif
{
    VkExportSemaphoreCreateInfo exportinfo = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    exportinfo.handleTypes = HWRT_SEM_HANDLE;
#ifdef WIN32
    VkExportSemaphoreWin32HandleInfoKHR win32info = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
    win32info.dwAccess = GENERIC_ALL;
    exportinfo.pNext = &win32info;
#endif
    VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    info.pNext = &exportinfo;
    HWRTCHECK(vkCreateSemaphore(hwrtdev.device, &info, NULL, &vksem), "vkCreateSemaphore");

#ifdef WIN32
    VkSemaphoreGetWin32HandleInfoKHR getinfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR };
    getinfo.semaphore = vksem;
    getinfo.handleType = HWRT_SEM_HANDLE;
    HANDLE handle = NULL;
    HWRTCHECK(vkGetSemaphoreWin32HandleKHR(hwrtdev.device, &getinfo, &handle), "vkGetSemaphoreWin32HandleKHR");
    glGenSemaphores_(1, &glsem);
    glImportSemaphoreWin32Handle_(glsem, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, handle);
    keep = handle;
#else
    VkSemaphoreGetFdInfoKHR getinfo = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
    getinfo.semaphore = vksem;
    getinfo.handleType = HWRT_SEM_HANDLE;
    int handle = -1;
    HWRTCHECK(vkGetSemaphoreFdKHR(hwrtdev.device, &getinfo, &handle), "vkGetSemaphoreFdKHR");
    glGenSemaphores_(1, &glsem);
    glImportSemaphoreFd_(glsem, GL_HANDLE_TYPE_OPAQUE_FD_EXT, handle);
#endif

    return checkgl("importing a Vulkan semaphore");
}

bool hwrtcreateshared(int w, int h)
{
    hwrtdestroyshared();
    if(w <= 0 || h <= 0) return false;

    hwrtio.w = w;
    hwrtio.h = h;
    hwrtio.skipcomposite = true;
    if(!createimage(w, h) || !importimage(w, h) ||
#ifdef WIN32
       !createsemaphore(hwrtio.vkglready, hwrtio.glglready, hwrtio.glreadyhandle) ||
       !createsemaphore(hwrtio.vkvkdone, hwrtio.glvkdone, hwrtio.vkdonehandle))
#else
       !createsemaphore(hwrtio.vkglready, hwrtio.glglready) ||
       !createsemaphore(hwrtio.vkvkdone, hwrtio.glvkdone))
#endif
    {
        hwrtdestroyshared();
        return false;
    }
    if(!createdepthimage(w, h) || !importdepthimage(w, h) || !setupdepthcopy(w, h))
    {
        conoutf(CON_WARN, "hwrt: shared depth unavailable, RTAO and the mode 7 depth mask disabled");
        destroyshareddepthgl();
    }
    if(!createreflimage(w, h) || !importreflimage(w, h))
    {
        conoutf(CON_WARN, "hwrt: reflection image unavailable, traced water reflections disabled");
        destroyreflimage();
    }
    return true;
}

void hwrtdestroyshared()
{
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    hwrtunbindshared();
    destroyshareddepth();
    destroyreflimage();

    if(hwrtio.glvkdone) { glDeleteSemaphores_(1, &hwrtio.glvkdone); hwrtio.glvkdone = 0; }
    if(hwrtio.glglready) { glDeleteSemaphores_(1, &hwrtio.glglready); hwrtio.glglready = 0; }
    if(hwrtio.gltex) { glDeleteTextures(1, &hwrtio.gltex); hwrtio.gltex = 0; }
    if(hwrtio.glmemory) { glDeleteMemoryObjects_(1, &hwrtio.glmemory); hwrtio.glmemory = 0; }

    if(hwrtdev.device)
    {
        if(hwrtio.vkvkdone) vkDestroySemaphore(hwrtdev.device, hwrtio.vkvkdone, NULL);
        if(hwrtio.vkglready) vkDestroySemaphore(hwrtdev.device, hwrtio.vkglready, NULL);
        if(hwrtio.view) vkDestroyImageView(hwrtdev.device, hwrtio.view, NULL);
        if(hwrtio.image) vkDestroyImage(hwrtdev.device, hwrtio.image, NULL);
        if(hwrtio.memory) vkFreeMemory(hwrtdev.device, hwrtio.memory, NULL);
    }
    hwrtio.vkvkdone = VK_NULL_HANDLE;
    hwrtio.vkglready = VK_NULL_HANDLE;
    hwrtio.view = VK_NULL_HANDLE;
    hwrtio.image = VK_NULL_HANDLE;
    hwrtio.memory = VK_NULL_HANDLE;
    hwrtio.memorysize = 0;
    hwrtio.w = hwrtio.h = 0;
    hwrtio.skipcomposite = false;
#ifdef WIN32
    // Now that both APIs are done with the objects, release our references.
    if(hwrtio.memhandle) { CloseHandle(hwrtio.memhandle); hwrtio.memhandle = NULL; }
    if(hwrtio.glreadyhandle) { CloseHandle(hwrtio.glreadyhandle); hwrtio.glreadyhandle = NULL; }
    if(hwrtio.vkdonehandle) { CloseHandle(hwrtio.vkdonehandle); hwrtio.vkdonehandle = NULL; }
#endif
}

// ---------------------------------------------------------------------------
// Per-frame handoff
// ---------------------------------------------------------------------------

void hwrtsignalgl()
{
    GLuint texs[3];
    GLenum layouts[3];
    texs[0] = hwrtio.gltex;
    layouts[0] = GL_LAYOUT_GENERAL_EXT;
    int n = 1;
    if(hwrtio.depthcopied && hwrtio.gldepthtex)
    {
        texs[1] = hwrtio.gldepthtex;
        layouts[1] = GL_LAYOUT_GENERAL_EXT;
        n = 2;
    }
    // Reflections off: the image is never written nor read, so it is not
    // handed over at all (trace.cpp skips its barrier the same frame).
    if(refl.gltex && hwrtreflections)
    {
        texs[n] = refl.gltex;
        layouts[n] = GL_LAYOUT_GENERAL_EXT;
        n++;
    }
    glSignalSemaphore_(hwrtio.glglready, 0, NULL, n, texs, layouts);
    // The signal has to reach the driver before the Vulkan submit that waits on
    // it is queued, otherwise the wait can outlive a signal still sitting in the
    // GL command buffer and the two queues deadlock. glFlush does not block.
    glFlush();
    hwrtio.depthcopied = false;
}

void hwrtwaitgl()
{
    GLuint texs[2] = { hwrtio.gltex, refl.gltex };
    GLenum layouts[2] = { GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT };
    glWaitSemaphore_(hwrtio.glvkdone, 0, NULL, refl.gltex && hwrtreflections ? 2 : 1, texs, layouts);
}

// Reads the shared texture back through GL. Slow and only meant for the console
// command, but it is the one measurement that separates "Vulkan never wrote" from
// "the composite draws the wrong thing".
void hwrtprobeshared()
{
    if(!hwrtio.ok()) { conoutf("hwrt: no shared image to probe"); return; }

    int w = hwrtio.w, h = hwrtio.h;
    uchar *pixels = new uchar[size_t(w)*size_t(h)*4];
    memset(pixels, 0xAB, size_t(w)*size_t(h)*4);
    while(glGetError() != GL_NO_ERROR);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hwrtio.gltex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum err = glGetError();

    GLint activetex = 0, colorarray = -1;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activetex);
    static PFNGLGETVERTEXATTRIBIVPROC getvertexattribiv = NULL;
    if(!getvertexattribiv) getvertexattribiv = (PFNGLGETVERTEXATTRIBIVPROC)getprocaddress("glGetVertexAttribiv");
    if(getvertexattribiv) getvertexattribiv(gle::ATTRIB_COLOR, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &colorarray);

    // The corner patch is drawn over the border, so sampling near the origin
    // would never see the border. The top edge is the one place only the border
    // writes.
    struct { const char *name; int x, y; } probes[] = {
        { "top edge", w/2, h - 2 },
        { "corner patch", w/16, h/16 },
        { "centre", w/2, h/2 },
    };
    conoutf("hwrt probe: %dx%d, glGetTexImage err 0x%x, active unit %d, colour array %d",
            w, h, int(err), int(activetex - GL_TEXTURE0), int(colorarray));
    loopi(int(sizeof(probes)/sizeof(probes[0])))
    {
        const uchar *p = &pixels[(size_t(probes[i].y)*w + probes[i].x)*4];
        conoutf("hwrt probe: %-12s (%4d,%4d) rgba %3d %3d %3d %3d", probes[i].name, probes[i].x, probes[i].y,
                p[0], p[1], p[2], p[3]);
    }
    delete[] pixels;
    // The engine log is block buffered, and a probe run usually ends in a kill.
    if(getlogfile()) fflush(getlogfile());
}

void hwrtresultsize(int *w, int *h)
{
    if(w) *w = hwrtio.w;
    if(h) *h = hwrtio.h;
}

bool hwrtcopyresult(float *dst, int *w, int *h, int *glerr)
{
    if(w) *w = 0;
    if(h) *h = 0;
    if(glerr) *glerr = 0;
    if(!dst || !hwrtio.gltex || hwrtio.w < 8 || hwrtio.h < 8) return false;
    static GLuint tex = 0, fbo = 0;
    static int tw = 0, th = 0;
    if(!tex) glGenTextures(1, &tex);
    if(!fbo) glGenFramebuffers_(1, &fbo);
    if(tw != hwrtio.w || th != hwrtio.h)
    {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, hwrtio.w, hwrtio.h, 0, GL_RGBA, GL_FLOAT, NULL);
        glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        tw = hwrtio.w;
        th = hwrtio.h;
    }
    while(glGetError() != GL_NO_ERROR);
    GLint prevfbo = 0, prevvp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfbo);
    glGetIntegerv(GL_VIEWPORT, prevvp);
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    if(glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        glBindFramebuffer_(GL_FRAMEBUFFER, prevfbo);
        if(glerr) *glerr = int(glGetError());
        return false;
    }
    glViewport(0, 0, hwrtio.w, hwrtio.h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    SETSHADER(screenrect);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hwrtio.gltex);
    gle::colorf(1, 1, 1, 1);
    screenquad(1, 1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glReadPixels(0, 0, hwrtio.w, hwrtio.h, GL_RGBA, GL_FLOAT, dst);
    GLenum err = glGetError();
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfbo);
    glViewport(prevvp[0], prevvp[1], prevvp[2], prevvp[3]);
    if(glerr) *glerr = int(err);
    if(err != GL_NO_ERROR) return false;
    if(w) *w = hwrtio.w;
    if(h) *h = hwrtio.h;
    return true;
}

int hwrtresultglformat()
{
    if(!hwrtio.gltex) return 0;
    GLint prev = 0, fmt = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, hwrtio.gltex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    glBindTexture(GL_TEXTURE_2D, prev);
    return int(fmt);
}

void hwrtcomposite(bool replace, float alpha)
{
    hwrtvelkeep();
    if(!replace)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    // Masked variant discards the pixels a model won, so GL keeps its own
    // raster there. Missing shader means the plain composite, same as before.
    bool masked = hwrtmaskready();
    if(masked) maskshader()->set();
    else SETSHADER(screenrect);
    if(masked)
    {
        glActiveTexture_(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, glmaskscene);
        glActiveTexture_(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, glmaskworld);
    }
    // The unit the last pass left active is not ours to assume; screenrect reads
    // tex0.
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hwrtio.gltex);
    gle::colorf(1, 1, 1, alpha);
    screenquad(1, 1);
    if(masked)
    {
        glActiveTexture_(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture_(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture_(GL_TEXTURE0);
    }
    if(!replace) glDisable(GL_BLEND);
}

extern float rtaoscale;

void hwrtcompositeao()
{
    if(rtaoscale > 0)
    {
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
        SETSHADER(screenrect);
        glActiveTexture_(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hwrtio.gltex);
        gle::colorf(1, 1, 1, 1);
        screenquad(1, 1);
        glDisable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    else hwrtcomposite(false, 1.0f);
}
