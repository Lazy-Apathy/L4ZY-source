// as.cpp: static world BLAS + identity TLAS.
//
// Opaque world triangles are concatenated from every vtxarray in valist, using
// the existing readva() download (which already skips sky, alpha and materials).
// texlayer blend overlays are pulled in separately: the coplanar dirt twin is
// dropped and the grass face is mixed with the dirt using lightmap alpha.
// Indices still contain va->voffset, which several VAs share inside one VBO, so
// each index is shifted into the concatenated vertex buffer before the BLAS
// sees it. Rebuild on allchanged() (map load, remip, coop-edit commit), never
// on the frame path. vkDeviceWaitIdle is used only around that rebuild.
//
// Phase 4 also uploads per-vertex tc/lm, keeps an index SSBO, per-triangle
// slot/lmid, and CPU-copies lightmaps + opaque diffuse into sampled 2D arrays.
// Phase 5 adds a world-space normal on the same surviving shade vert. Those
// survive after the BLAS vertex/index buffers are freed.

#include "engine.h"
#include "hwrt/hwrt.h"

int hwrtworldtris = 0, hwrtworldverts = 0;
int hwrtshadeepoch = 0, hwrtshadelm = 0, hwrtshadediff = 0, hwrtshadeglow = 0;
int hwrtglowomnicount = 0;
hwrtglowomni hwrtglowomnis[HWRT_MAX_GLOWOMNI];

struct hwrtbuf
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    VkDeviceSize size;
};

// tc/lm as before, plus a world-space unit normal and the world position.
// Position lets hitlight build a flat cube-face normal (vertex normals on
// Sauer cubes often degenerate to +Z, which glued sunlight to eye height).
// pad/pad2 are the texlayer bottom UVs (same as tc when the layers share verts).
// std430: vec2+vec2+vec3+float+vec3+float.
struct hwrtshadevert { float tc[2], lm[2], n[3], pad, pos[3], pad2; };
// lmid packs the lightmap index in the low 16 bits and the bottom diffuse
// layer in the high 16 (0xFFFF = no texlayer mix).
struct hwrtshadetri { uint layer, lmid, color, pad; };
static_assert(sizeof(hwrtshadevert) == 48, "shade vert stride");
static_assert(sizeof(hwrtshadetri) == 16, "shade tri stride");

enum { HWRT_MAX_DIFFUSE = 256, HWRT_MAX_LM = 256, HWRT_MAX_TEXDIM = 1024 };

struct hwrtworld
{
    VkAccelerationStructureKHR blas, tlas;
    hwrtbuf blasbuf, tlasbuf;
    VkCommandPool cmdpool;
    VkCommandBuffer cmd;

    hwrtbuf shadevert, shadeidx, shadetri;
    hwrttexarray lm, diff, glow;
    VkSampler lmsampler, diffsampler;
    bool shadeok;
};

static hwrtworld world;

static bool ensurecmd();

static VkDeviceSize alignup(VkDeviceSize v, VkDeviceSize a)
{
    if(!a) return v;
    return (v + a - 1) & ~(a - 1);
}

static VkDeviceAddress alignedaddr(VkDeviceAddress addr, VkDeviceSize align)
{
    if(!align) return addr;
    VkDeviceSize mis = VkDeviceSize(addr % align);
    return mis ? addr + (align - mis) : addr;
}

static void destroybuf(hwrtbuf &b)
{
    if(hwrtdev.device)
    {
        if(b.buffer) vkDestroyBuffer(hwrtdev.device, b.buffer, NULL);
        if(b.memory) vkFreeMemory(hwrtdev.device, b.memory, NULL);
    }
    memset(&b, 0, sizeof(b));
}

static bool asfail(const char *what, VkResult r = VK_SUCCESS)
{
    if(r != VK_SUCCESS) conoutf(CON_WARN, "hwrt: %s failed (%s), world TLAS not built", what, hwrtresultstr(r));
    else conoutf(CON_WARN, "hwrt: %s, world TLAS not built", what);
    return false;
}

static bool createbuf(hwrtbuf &b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool deviceaddress = true)
{
    memset(&b, 0, sizeof(b));
    if(size < 1) size = 1;

    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(hwrtdev.device, &info, NULL, &b.buffer);
    if(r != VK_SUCCESS) return asfail("vkCreateBuffer", r);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(hwrtdev.device, b.buffer, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, props);
    if(memtype < 0 && (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    {
        memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(memtype < 0) memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    else if(memtype < 0)
        memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(memtype < 0) { destroybuf(b); return asfail("no memory type for an acceleration-structure buffer"); }

    VkMemoryAllocateFlagsInfo flags = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    if(deviceaddress) alloc.pNext = &flags;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &alloc, NULL, &b.memory);
    if(r != VK_SUCCESS) { destroybuf(b); return asfail("vkAllocateMemory", r); }
    r = vkBindBufferMemory(hwrtdev.device, b.buffer, b.memory, 0);
    if(r != VK_SUCCESS) { destroybuf(b); return asfail("vkBindBufferMemory", r); }

    b.size = req.size;
    if(deviceaddress)
    {
        VkBufferDeviceAddressInfo addrinfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrinfo.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(hwrtdev.device, &addrinfo);
        if(!b.address) { destroybuf(b); return asfail("vkGetBufferDeviceAddress returned 0"); }
    }
    return true;
}

static bool uploadbuf(hwrtbuf &b, const void *data, VkDeviceSize bytes)
{
    void *mapped = NULL;
    VkResult r = vkMapMemory(hwrtdev.device, b.memory, 0, bytes, 0, &mapped);
    if(r != VK_SUCCESS || !mapped) return asfail("vkMapMemory", r);
    memcpy(mapped, data, size_t(bytes));
    vkUnmapMemory(hwrtdev.device, b.memory);
    return true;
}

static bool shadefail(const char *what, VkResult r = VK_SUCCESS)
{
    static bool logged = false;
    if(!logged)
    {
        if(r != VK_SUCCESS) conoutf(CON_WARN, "hwrt: %s failed (%s), hit-shade disabled", what, hwrtresultstr(r));
        else conoutf(CON_WARN, "hwrt: %s, hit-shade disabled", what);
        logged = true;
    }
    return false;
}

static PFN_vkCmdCopyBufferToImage hwrtCmdCopyBufferToImage = NULL;
static PFN_vkCreateSampler hwrtCreateSampler = NULL;
static PFN_vkDestroySampler hwrtDestroySampler = NULL;

static bool loadshadefuncs()
{
    if(hwrtCmdCopyBufferToImage && hwrtCreateSampler && hwrtDestroySampler) return true;
    if(!vkGetDeviceProcAddr || !hwrtdev.device) return shadefail("no Vulkan device for hit-shade");
    hwrtCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)vkGetDeviceProcAddr(hwrtdev.device, "vkCmdCopyBufferToImage");
    hwrtCreateSampler = (PFN_vkCreateSampler)vkGetDeviceProcAddr(hwrtdev.device, "vkCreateSampler");
    hwrtDestroySampler = (PFN_vkDestroySampler)vkGetDeviceProcAddr(hwrtdev.device, "vkDestroySampler");
    if(!hwrtCmdCopyBufferToImage || !hwrtCreateSampler || !hwrtDestroySampler)
        return shadefail("vkCmdCopyBufferToImage/vkCreateSampler missing");
    return true;
}

void hwrtdestroytexarray(hwrttexarray &t)
{
    if(hwrtdev.device)
    {
        if(t.view) vkDestroyImageView(hwrtdev.device, t.view, NULL);
        if(t.image) vkDestroyImage(hwrtdev.device, t.image, NULL);
        if(t.memory) vkFreeMemory(hwrtdev.device, t.memory, NULL);
    }
    memset(&t, 0, sizeof(t));
}

static inline void destroytex(hwrttexarray &t) { hwrtdestroytexarray(t); }

static void destroyshade()
{
    destroybuf(world.shadevert);
    destroybuf(world.shadeidx);
    destroybuf(world.shadetri);
    destroytex(world.lm);
    destroytex(world.diff);
    destroytex(world.glow);
    world.shadeok = false;
    hwrtshadelm = hwrtshadediff = hwrtshadeglow = 0;
    hwrtglowomnicount = 0;
    hwrtshadeepoch++;
}

static bool createstorage(hwrtbuf &b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props)
{
    memset(&b, 0, sizeof(b));
    if(size < 16) size = 16;
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(hwrtdev.device, &info, NULL, &b.buffer);
    if(r != VK_SUCCESS) return shadefail("vkCreateBuffer (shade)", r);

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(hwrtdev.device, b.buffer, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, props);
    if(memtype < 0 && (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
    {
        memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if(memtype < 0) memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if(memtype < 0) { destroybuf(b); return shadefail("no memory type for a shade buffer"); }

    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &alloc, NULL, &b.memory);
    if(r != VK_SUCCESS) { destroybuf(b); return shadefail("vkAllocateMemory (shade)", r); }
    r = vkBindBufferMemory(hwrtdev.device, b.buffer, b.memory, 0);
    if(r != VK_SUCCESS) { destroybuf(b); return shadefail("vkBindBufferMemory (shade)", r); }
    b.size = req.size;
    return true;
}

static bool uploadstorage(hwrtbuf &b, const void *data, VkDeviceSize bytes)
{
    void *mapped = NULL;
    VkResult r = vkMapMemory(hwrtdev.device, b.memory, 0, bytes, 0, &mapped);
    if(r != VK_SUCCESS || !mapped) return shadefail("vkMapMemory (shade)", r);
    memcpy(mapped, data, size_t(bytes));
    vkUnmapMemory(hwrtdev.device, b.memory);
    return true;
}

bool hwrtcreatesampler(bool repeat, VkSampler &out, bool mips)
{
    out = VK_NULL_HANDLE;
    if(!loadshadefuncs()) return false;

    VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = mips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode mode = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeU = mode;
    info.addressModeV = mode;
    info.addressModeW = mode;
    info.minLod = 0;
    info.maxLod = mips ? 16.f : 0.f;
    VkResult r = hwrtCreateSampler(hwrtdev.device, &info, NULL, &out);
    if(r != VK_SUCCESS) { out = VK_NULL_HANDLE; return false; }
    return true;
}

void hwrtdestroysampler(VkSampler &s)
{
    if(s && hwrtdev.device && hwrtDestroySampler) hwrtDestroySampler(hwrtdev.device, s, NULL);
    s = VK_NULL_HANDLE;
}

static bool ensuresamplers()
{
    if(world.lmsampler && world.diffsampler) return true;
    if(!loadshadefuncs()) return false;
    if(!world.lmsampler && !hwrtcreatesampler(false, world.lmsampler))
        return shadefail("vkCreateSampler (lightmap)");
    if(!world.diffsampler && !hwrtcreatesampler(true, world.diffsampler, true))
        return shadefail("vkCreateSampler (diffuse)");
    return true;
}

static void blitstretch(const uchar *src, int sw, int sh, uchar *dst, int dw, int dh)
{
    if(sw == dw && sh == dh)
    {
        memcpy(dst, src, size_t(dw)*size_t(dh)*4);
        return;
    }
    loopi(dh) loopj(dw)
    {
        int sx = j * sw / dw;
        int sy = i * sh / dh;
        memcpy(&dst[(size_t(i)*dw + j)*4], &src[(size_t(sy)*sw + sx)*4], 4);
    }
}

static int countmips(int w, int h)
{
    int n = 1;
    while(w > 1 || h > 1)
    {
        w = max(1, w/2);
        h = max(1, h/2);
        n++;
    }
    return n;
}

static void boxmip4(const uchar *src, int sw, int sh, uchar *dst, int dw, int dh)
{
    loopi(dh) loopj(dw)
    {
        int x0 = min(j*2, sw-1), y0 = min(i*2, sh-1);
        int x1 = min(x0+1, sw-1), y1 = min(y0+1, sh-1);
        int acc[4] = {0,0,0,0}, n = 0;
        for(int y = y0; y <= y1; y++) for(int x = x0; x <= x1; x++)
        {
            const uchar *p = src + (size_t(y)*sw + x)*4;
            loopk(4) acc[k] += p[k];
            n++;
        }
        uchar *o = dst + (size_t(i)*dw + j)*4;
        loopk(4) o[k] = uchar(acc[k] / max(n, 1));
    }
}

static bool texfail(const char *what, const char *op, VkResult r = VK_SUCCESS)
{
    if(r != VK_SUCCESS) conoutf(CON_WARN, "hwrt: %s failed (%s), %s unavailable", op, hwrtresultstr(r), what);
    else conoutf(CON_WARN, "hwrt: %s, %s unavailable", op, what);
    return false;
}

static bool createimg(hwrttexarray &t, int w, int h, int layers, int miplevels)
{
    destroytex(t);
    if(w < 1) w = 1;
    if(h < 1) h = 1;
    if(layers < 1) layers = 1;
    if(miplevels < 1) miplevels = 1;

    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent.width = uint32_t(w);
    info.extent.height = uint32_t(h);
    info.extent.depth = 1;
    info.mipLevels = uint32_t(miplevels);
    info.arrayLayers = uint32_t(layers);
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(hwrtdev.device, &info, NULL, &t.image);
    if(r != VK_SUCCESS) return shadefail("vkCreateImage (shade)", r);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(hwrtdev.device, t.image, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(memtype < 0) memtype = hwrtfindmemtype(req.memoryTypeBits, 0);
    if(memtype < 0) { destroytex(t); return shadefail("no memory type for a shade image"); }

    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &alloc, NULL, &t.memory);
    if(r != VK_SUCCESS) { destroytex(t); return shadefail("vkAllocateMemory (shade image)", r); }
    r = vkBindImageMemory(hwrtdev.device, t.image, t.memory, 0);
    if(r != VK_SUCCESS) { destroytex(t); return shadefail("vkBindImageMemory (shade image)", r); }

    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = uint32_t(miplevels);
    view.subresourceRange.layerCount = uint32_t(layers);
    r = vkCreateImageView(hwrtdev.device, &view, NULL, &t.view);
    if(r != VK_SUCCESS) { destroytex(t); return shadefail("vkCreateImageView (shade)", r); }

    t.w = w;
    t.h = h;
    t.layers = layers;
    t.mips = miplevels;
    return true;
}

// Pulls a set of GL textures back with glGetTexImage and stacks them into one
// R8G8B8A8 2D array. The copy happens on the caller's command buffer and waits
// for the device, so it belongs on a rebuild / first-use path, never in a frame.
bool hwrtuploadtexarray(VkCommandBuffer cmd, const vector<GLuint> &ids, int maxdim, hwrttexarray &out, const char *what, bool mips)
{
    if(ids.empty()) return texfail(what, "no textures to copy");
    if(!loadshadefuncs() || !cmd) return texfail(what, "no command buffer to copy textures");

    GLint prev = 0, packalign = 4, packrow = 0, packbuf = 0, activetex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packalign);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &packrow);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packbuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activetex);
    glActiveTexture_(GL_TEXTURE0);
    if(glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    while(glGetError() != GL_NO_ERROR);

    int maxw = 1, maxh = 1;
    vector<int> ws, hs;
    loopv(ids)
    {
        int w = 1, h = 1;
        if(ids[i])
        {
            glBindTexture(GL_TEXTURE_2D, ids[i]);
            GLint tw = 0, th = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
            if(tw > 0) w = tw;
            if(th > 0) h = th;
        }
        ws.add(w);
        hs.add(h);
        if(w > maxw) maxw = w;
        if(h > maxh) maxh = h;
    }
    if(maxdim < 1) maxdim = HWRT_MAX_TEXDIM;
    maxw = min(maxw, maxdim);
    maxh = min(maxh, maxdim);
    int layers = ids.length();
    int miplevels = mips ? countmips(maxw, maxh) : 1;
    if(miplevels > 16) miplevels = 16;

    const VkDeviceSize layerbytes = VkDeviceSize(maxw)*VkDeviceSize(maxh)*4;
    VkDeviceSize total = 0;
    int mw = maxw, mh = maxh;
    loopi(miplevels)
    {
        total += VkDeviceSize(mw)*VkDeviceSize(mh)*4*VkDeviceSize(layers);
        mw = max(1, mw/2);
        mh = max(1, mh/2);
    }
    uchar *pixels = new uchar[size_t(total)];
    memset(pixels, 0, size_t(total));

    loopv(ids)
    {
        uchar *dst = pixels + size_t(i)*size_t(layerbytes);
        int w = ws[i], h = hs[i];
        bool ok = false;
        if(ids[i] && w > 0 && h > 0)
        {
            uchar *tmp = new uchar[size_t(w)*size_t(h)*4];
            if(tmp)
            {
                memset(tmp, 0, size_t(w)*size_t(h)*4);
                glBindTexture(GL_TEXTURE_2D, ids[i]);
                while(glGetError() != GL_NO_ERROR);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
                ok = glGetError() == GL_NO_ERROR;
                if(ok) blitstretch(tmp, w, h, dst, maxw, maxh);
                delete[] tmp;
            }
        }
        if(!ok)
        {
            // Magenta so a missing copy is obvious and does not crash.
            for(VkDeviceSize p = 0; p < layerbytes; p += 4)
            {
                dst[p+0] = 255;
                dst[p+1] = 0;
                dst[p+2] = 255;
                dst[p+3] = 255;
            }
        }
    }

    if(miplevels > 1)
    {
        int sw = maxw, sh = maxh;
        VkDeviceSize srcoff = 0;
        for(int mip = 1; mip < miplevels; mip++)
        {
            int dw = max(1, sw/2), dh = max(1, sh/2);
            VkDeviceSize srcstride = VkDeviceSize(sw)*VkDeviceSize(sh)*4;
            VkDeviceSize dststride = VkDeviceSize(dw)*VkDeviceSize(dh)*4;
            VkDeviceSize dstoff = srcoff + srcstride*VkDeviceSize(layers);
            loopi(layers)
                boxmip4(pixels + size_t(srcoff + srcstride*VkDeviceSize(i)), sw, sh,
                        pixels + size_t(dstoff + dststride*VkDeviceSize(i)), dw, dh);
            srcoff = dstoff;
            sw = dw;
            sh = dh;
        }
    }

    glBindTexture(GL_TEXTURE_2D, prev);
    glPixelStorei(GL_PACK_ALIGNMENT, packalign);
    glPixelStorei(GL_PACK_ROW_LENGTH, packrow);
    if(glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER, packbuf);
    glActiveTexture_(uint(activetex));

    if(!createimg(out, maxw, maxh, layers, miplevels))
    {
        delete[] pixels;
        return false;
    }

    hwrtbuf staging;
    if(!createstorage(staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
       !uploadstorage(staging, pixels, total))
    {
        destroybuf(staging);
        destroytex(out);
        delete[] pixels;
        return false;
    }
    delete[] pixels;

    VkResult r = vkResetCommandBuffer(cmd, 0);
    if(r != VK_SUCCESS) { destroybuf(staging); destroytex(out); return texfail(what, "vkResetCommandBuffer (textures)", r); }
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(cmd, &begin);
    if(r != VK_SUCCESS) { destroybuf(staging); destroytex(out); return texfail(what, "vkBeginCommandBuffer (textures)", r); }

    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = out.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = uint32_t(miplevels);
    barrier.subresourceRange.layerCount = uint32_t(layers);
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy copies[16];
    int ncopy = 0;
    VkDeviceSize offset = 0;
    mw = maxw;
    mh = maxh;
    loopi(miplevels)
    {
        VkBufferImageCopy &copy = copies[ncopy++];
        memset(&copy, 0, sizeof(copy));
        copy.bufferOffset = offset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = uint32_t(i);
        copy.imageSubresource.layerCount = uint32_t(layers);
        copy.imageExtent.width = uint32_t(mw);
        copy.imageExtent.height = uint32_t(mh);
        copy.imageExtent.depth = 1;
        offset += VkDeviceSize(mw)*VkDeviceSize(mh)*4*VkDeviceSize(layers);
        mw = max(1, mw/2);
        mh = max(1, mh/2);
    }
    hwrtCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(ncopy), copies);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    r = vkEndCommandBuffer(cmd);
    if(r != VK_SUCCESS) { destroybuf(staging); destroytex(out); return texfail(what, "vkEndCommandBuffer (textures)", r); }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    r = vkQueueSubmit(hwrtdev.queue, 1, &submit, VK_NULL_HANDLE);
    if(r != VK_SUCCESS) { destroybuf(staging); destroytex(out); return texfail(what, "vkQueueSubmit (textures)", r); }
    if(!hwrtwaitidle("vkDeviceWaitIdle (textures)")) { destroybuf(staging); destroytex(out); return false; }
    destroybuf(staging);
    return true;
}

static bool createcubeimg(hwrttexarray &t, int w)
{
    destroytex(t);
    if(w < 1) w = 1;
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent.width = uint32_t(w);
    info.extent.height = uint32_t(w);
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 6;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(hwrtdev.device, &info, NULL, &t.image);
    if(r != VK_SUCCESS) return texfail("model envmap", "vkCreateImage", r);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(hwrtdev.device, t.image, &req);
    int memtype = hwrtfindmemtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(memtype < 0) memtype = hwrtfindmemtype(req.memoryTypeBits, 0);
    if(memtype < 0) { destroytex(t); return texfail("model envmap", "no memory type"); }

    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &alloc, NULL, &t.memory);
    if(r != VK_SUCCESS) { destroytex(t); return texfail("model envmap", "vkAllocateMemory", r); }
    r = vkBindImageMemory(hwrtdev.device, t.image, t.memory, 0);
    if(r != VK_SUCCESS) { destroytex(t); return texfail("model envmap", "vkBindImageMemory", r); }

    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = t.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 6;
    r = vkCreateImageView(hwrtdev.device, &view, NULL, &t.view);
    if(r != VK_SUCCESS) { destroytex(t); return texfail("model envmap", "vkCreateImageView", r); }

    t.w = w;
    t.h = w;
    t.layers = 6;
    t.mips = 1;
    return true;
}

bool hwrtuploadcubemap(VkCommandBuffer cmd, GLuint gltex, hwrttexarray &out, const char *what)
{
    if(!loadshadefuncs() || !cmd) return texfail(what, "no command buffer to copy cubemap");

    const int dimcap = 256;
    int w = 4;
    uchar *pixels = NULL;
    VkDeviceSize total = 0;
    bool fromgl = false;
    if(gltex)
    {
        GLint prev2d = 0, prevcube = 0, packalign = 4, packrow = 0, packbuf = 0, activetex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev2d);
        glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &prevcube);
        glGetIntegerv(GL_PACK_ALIGNMENT, &packalign);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &packrow);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packbuf);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activetex);
        glActiveTexture_(GL_TEXTURE0);
        if(glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, gltex);
        GLint tw = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X, 0, GL_TEXTURE_WIDTH, &tw);
        if(tw > 0) w = min(tw, dimcap);
        total = VkDeviceSize(w)*VkDeviceSize(w)*4*6;
        pixels = new uchar[size_t(total)];
        memset(pixels, 0, size_t(total));
        uchar *tmp = new uchar[size_t(max(tw, 1))*size_t(max(tw, 1))*4];
        fromgl = tmp != NULL && tw > 0;
        loopi(6)
        {
            uchar *dst = pixels + size_t(i)*size_t(w)*size_t(w)*4;
            bool ok = false;
            if(fromgl)
            {
                memset(tmp, 0, size_t(tw)*size_t(tw)*4);
                while(glGetError() != GL_NO_ERROR);
                glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
                ok = glGetError() == GL_NO_ERROR;
                if(ok) blitstretch(tmp, tw, tw, dst, w, w);
            }
            if(!ok)
            {
                for(int p = 0; p < w*w; p++)
                {
                    dst[p*4+0] = 80;
                    dst[p*4+1] = 80;
                    dst[p*4+2] = 90;
                    dst[p*4+3] = 255;
                }
            }
        }
        delete[] tmp;
        glBindTexture(GL_TEXTURE_CUBE_MAP, prevcube);
        glBindTexture(GL_TEXTURE_2D, prev2d);
        glPixelStorei(GL_PACK_ALIGNMENT, packalign);
        glPixelStorei(GL_PACK_ROW_LENGTH, packrow);
        if(glBindBuffer_) glBindBuffer_(GL_PIXEL_PACK_BUFFER, packbuf);
        glActiveTexture_(uint(activetex));
    }
    if(!pixels)
    {
        w = 4;
        total = VkDeviceSize(w)*VkDeviceSize(w)*4*6;
        pixels = new uchar[size_t(total)];
        for(int p = 0; p < w*w*6; p++)
        {
            pixels[p*4+0] = 80;
            pixels[p*4+1] = 80;
            pixels[p*4+2] = 90;
            pixels[p*4+3] = 255;
        }
    }

    if(!createcubeimg(out, w))
    {
        delete[] pixels;
        return false;
    }

    hwrtbuf staging;
    if(!createstorage(staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
       !uploadstorage(staging, pixels, total))
    {
        destroybuf(staging);
        destroytex(out);
        delete[] pixels;
        return false;
    }
    delete[] pixels;

    VkResult r = vkResetCommandBuffer(cmd, 0);
    if(r != VK_SUCCESS) { destroybuf(staging); destroytex(out); return texfail(what, "vkResetCommandBuffer (cubemap)", r); }
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(cmd, &begin);
    if(r != VK_SUCCESS) { destroybuf(staging); destroytex(out); return texfail(what, "vkBeginCommandBuffer (cubemap)", r); }

    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = out.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 6;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy copy;
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 6;
    copy.imageExtent.width = uint32_t(w);
    copy.imageExtent.height = uint32_t(w);
    copy.imageExtent.depth = 1;
    hwrtCmdCopyBufferToImage(cmd, staging.buffer, out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    r = vkEndCommandBuffer(cmd);
    if(r != VK_SUCCESS) { destroybuf(staging); destroytex(out); return texfail(what, "vkEndCommandBuffer (cubemap)", r); }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    r = vkQueueSubmit(hwrtdev.queue, 1, &submit, VK_NULL_HANDLE);
    if(r != VK_SUCCESS) { destroybuf(staging); destroytex(out); return texfail(what, "vkQueueSubmit (cubemap)", r); }
    if(!hwrtwaitidle("vkDeviceWaitIdle (cubemap)")) { destroybuf(staging); destroytex(out); return false; }
    destroybuf(staging);
    return true;
}

static bool uploadtexarray(const vector<GLuint> &ids, hwrttexarray &out, bool mips = false)
{
    if(!ensurecmd()) return false;
    return hwrtuploadtexarray(world.cmd, ids, HWRT_MAX_TEXDIM, out, "hit-shade", mips);
}

static uint packcolorscale(const vec &c)
{
    return uint(uchar(clamp(c.x*255.f, 0.f, 255.f))) |
           (uint(uchar(clamp(c.y*255.f, 0.f, 255.f)))<<8) |
           (uint(uchar(clamp(c.z*255.f, 0.f, 255.f)))<<16);
}

static int findoradddiffuse(vector<GLuint> &ids, GLuint id, int &overflow)
{
    if(!id) return -1;
    loopv(ids) if(ids[i] == id) return i;
    if(ids.length() >= HWRT_MAX_DIFFUSE) { overflow++; return -1; }
    ids.add(id);
    return ids.length()-1;
}

static vec avgtexrgb(GLuint id)
{
    vec acc(0, 0, 0);
    if(!id) return acc;
    GLint prev = 0, tw = 0, th = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, id);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if(tw < 1 || th < 1) { glBindTexture(GL_TEXTURE_2D, prev); return acc; }
    uchar *tmp = new uchar[size_t(tw)*size_t(th)*4];
    if(!tmp) { glBindTexture(GL_TEXTURE_2D, prev); return acc; }
    memset(tmp, 0, size_t(tw)*size_t(th)*4);
    while(glGetError() != GL_NO_ERROR);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    bool ok = glGetError() == GL_NO_ERROR;
    glBindTexture(GL_TEXTURE_2D, prev);
    if(ok)
    {
        int step = max(1, max(tw, th)/32);
        int n = 0;
        for(int y = 0; y < th; y += step) for(int x = 0; x < tw; x += step)
        {
            uchar *p = tmp + (size_t(y)*size_t(tw) + x)*4;
            if(max(int(p[0]), max(int(p[1]), int(p[2]))) < 8) continue;
            acc.add(vec(p[0]/255.0f, p[1]/255.0f, p[2]/255.0f));
            n++;
        }
        if(n) acc.mul(1.0f/n);
    }
    delete[] tmp;
    return acc;
}

static int findoraddglow(vector<GLuint> &ids, vector<vec> &avgs, GLuint id, int &overflow)
{
    if(!id) return -1;
    loopv(ids) if(ids[i] == id) return i;
    if(ids.length() >= HWRT_MAX_DIFFUSE) { overflow++; return -1; }
    ids.add(id);
    avgs.add(avgtexrgb(id));
    return ids.length()-1;
}

static Texture *slotglowtex(Slot *slot)
{
    if(!slot || !(slot->texmask&(1<<TEX_GLOW))) return NULL;
    loopvj(slot->sts) if(slot->sts[j].type==TEX_GLOW) return slot->sts[j].t;
    return NULL;
}

static uint packglow(int layer, const vec &color)
{
    if(layer < 0) return 0xFFu;
    return (uint(layer)&0xFFu) | (packcolorscale(color)<<8);
}

// texlayer floors are two coplanar faces: the dirt (LAYER_TOP, in va->tris)
// and the grass overlay (LAYER_BLEND, in va->blendtris). Keep one of them in
// the BLAS and mix the two diffuses with lightmap alpha, the way GL's blend
// pass does. pad/pad2 on the shade vert carry the other layer's UVs.
struct hwrttrikey { ivec a, b, c; };
static inline ivec hwrtqpos(const vec &p)
{
    return ivec(int(p.x*8.0f + (p.x>=0 ? 0.5f : -0.5f)),
                int(p.y*8.0f + (p.y>=0 ? 0.5f : -0.5f)),
                int(p.z*8.0f + (p.z>=0 ? 0.5f : -0.5f)));
}
static inline bool hwrtilex(const ivec &x, const ivec &y)
{
    if(x.x != y.x) return x.x < y.x;
    if(x.y != y.y) return x.y < y.y;
    return x.z < y.z;
}
static inline hwrttrikey hwrtmakekey(const vec &p0, const vec &p1, const vec &p2)
{
    hwrttrikey k;
    k.a = hwrtqpos(p0); k.b = hwrtqpos(p1); k.c = hwrtqpos(p2);
    if(hwrtilex(k.b, k.a)) swap(k.a, k.b);
    if(hwrtilex(k.c, k.b)) swap(k.b, k.c);
    if(hwrtilex(k.b, k.a)) swap(k.a, k.b);
    return k;
}
static inline uint hthash(const hwrttrikey &k)
{
    return hthash(k.a) ^ (hthash(k.b)*31u) ^ (hthash(k.c)*311u);
}
static inline bool htcmp(const hwrttrikey &x, const hwrttrikey &y)
{
    return x.a == y.a && x.b == y.b && x.c == y.c;
}

static uint packlmidbot(int lmid, int botlayer)
{
    uint bot = botlayer < 0 ? 0xFFFFu : (uint(botlayer) & 0xFFFFu);
    return (uint(lmid) & 0xFFFFu) | (bot << 16);
}

// The dirt twin's three BLAS vertex indices, kept while its triangle is dropped.
struct hwrttritwin { uint i[3]; };

// Hand the grass triangle the dirt twin's own UVs, corner by corner.
//
// octarender emits a layered face twice (octarender.cpp:1086-1089): the grass
// into va->blendtris with c.texture[i], the dirt into va->tris with vslot.layer
// and its own scale / rotation / offset, so the two carry DIFFERENT tc at the
// SAME positions. Matching corners by position inside one known twin pair is
// exact. Matching by position across the whole vertex array is not: every cube
// corner and edge holds several vertices at one position belonging to unrelated
// faces, and pairing those painted the dirt with a stranger's UVs.
static void copytwintc(vector<hwrtshadevert> &sverts, vector<vec> &positions,
                       hashtable<hwrttrikey, hwrttritwin> &dirttwin,
                       uint i0, uint i1, uint i2)
{
    hwrttritwin *tw = dirttwin.access(hwrtmakekey(positions[i0], positions[i1], positions[i2]));
    if(!tw) return;
    const uint top[3] = { i0, i1, i2 };
    loopi(3)
    {
        ivec q = hwrtqpos(positions[top[i]]);
        loopj(3) if(hwrtqpos(positions[tw->i[j]]) == q)
        {
            sverts[top[i]].pad = sverts[tw->i[j]].tc[0];
            sverts[top[i]].pad2 = sverts[tw->i[j]].tc[1];
            break;
        }
    }
}

enum { GLOWKIND_TEX = 0, GLOWKIND_LAVA = 1 };

struct glowcluster
{
    vec pos, n, color;
    float area;
    int ntris;
    int kind;
};
static vector<glowcluster> glowclusters;
static int hwrtlavalights = 0;

static bool slotislava(Slot *slot)
{
    if(!slot) return false;
    loopv(slot->sts)
    {
        const char *n = slot->sts[i].name;
        if(n && n[0] && strstr(n, "lava")) return true;
        Texture *t = slot->sts[i].t;
        if(t && t->name && strstr(t->name, "lava")) return true;
    }
    return false;
}

static vec lavacolourvec(int mat = MAT_LAVA)
{
    const bvec &lc = getlavacolor(mat);
    vec col(lc.x/255.0f, lc.y/255.0f, lc.z/255.0f);
    if(col.squaredlen() < 1e-4f) col = vec(1.00f, 0.25f, 0.00f);
    return col;
}

static void addglowcluster(const vec &c, const vec &nrm, float area, const vec &col, int kind)
{
    if(area < 8.0f) return;
    loopv(glowclusters)
    {
        glowcluster &g = glowclusters[i];
        if(g.kind != kind) continue;
        vec mean = vec(g.pos).mul(1.0f/max(g.ntris, 1));
        vec meancol = vec(g.color).mul(1.0f/max(g.ntris, 1));
        if(mean.dist(c) < 40.0f && meancol.dist(col) < 0.45f)
        {
            g.pos.add(c);
            g.n.add(vec(nrm).mul(area));
            g.color.add(col);
            g.area += area;
            g.ntris++;
            return;
        }
    }
    if(glowclusters.length() >= HWRT_MAX_GLOWOMNI) return;
    glowcluster &g = glowclusters.add();
    g.pos = c;
    g.n = vec(nrm).mul(area);
    g.color = col;
    g.area = area;
    g.ntris = 1;
    g.kind = kind;
}

static void emitworldtri(vector<vec> &positions, vector<uint> &indices, vector<hwrtshadetri> &stris,
                         uint i0, uint i1, uint i2, const hwrtshadetri &st,
                         int glowlayer, const vec &glowcol, const vec &glowavg, Slot *slot)
{
    indices.add(i0); indices.add(i1); indices.add(i2);
    stris.add(st);
    bool lava = slotislava(slot);
    if(glowlayer < 0 && !lava) return;
    if(i0 >= uint(positions.length()) || i1 >= uint(positions.length()) || i2 >= uint(positions.length())) return;
    vec a = positions[i0], b = positions[i1], c = positions[i2];
    vec cen = vec(a).add(b).add(c).mul(1.0f/3.0f);
    vec nrm;
    nrm.cross(vec(b).sub(a), vec(c).sub(a));
    float area = nrm.magnitude()*0.5f;
    if(area > 1e-4f) nrm.mul(1.0f/nrm.magnitude());
    vec col;
    int kind;
    if(lava)
    {
        col = lavacolourvec();
        kind = GLOWKIND_LAVA;
    }
    else
    {
        col = vec(glowavg).mul(glowcol);
        if(col.squaredlen() < 1e-4f) col = glowcol;
        kind = GLOWKIND_TEX;
    }
    addglowcluster(cen, nrm, area, col, kind);
}

static int slotdiffuselayer(Slot *slot, vector<GLuint> &diffids, int &diffoverflow)
{
    Texture *tex = (!slot || slot->sts.empty()) ? notexture : slot->sts[0].t;
    if(tex && (tex->type&Texture::TYPE)==Texture::CUBEMAP) tex = notexture;
    GLuint id = tex ? tex->id : (notexture ? notexture->id : 0);
    return findoradddiffuse(diffids, id, diffoverflow);
}

static hwrtshadetri makeshadestri(const elementset &e, vector<GLuint> &diffids, int &diffoverflow,
                                 vector<GLuint> &glowids, vector<vec> &glowavgs, int &glowoflow,
                                 int botlayer, int &glowlayer, vec &glowcol, vec &glowavg)
{
    extern bool brightengeom, editmode;
    extern int fullbright;
    VSlot &vs = lookupvslot(e.texture, false);
    Slot *slot = vs.slot;
    int layer = slotdiffuselayer(slot, diffids, diffoverflow);
    int lmid = e.lmid;
    if(botlayer < 0 && brightengeom && (e.lmid < LMID_RESERVED || (fullbright && editmode)))
        lmid = LMID_BRIGHT;
    Texture *gtex = slotglowtex(slot);
    glowlayer = -1;
    glowcol = vs.glowcolor;
    glowavg = vec(1, 1, 1);
    if(gtex && gtex->id)
    {
        glowlayer = findoraddglow(glowids, glowavgs, gtex->id, glowoflow);
        if(glowlayer >= 0 && glowavgs.inrange(glowlayer)) glowavg = glowavgs[glowlayer];
    }
    hwrtshadetri st;
    st.layer = layer < 0 ? 0xFFFFFFFFu : uint(layer);
    st.lmid = packlmidbot(lmid, botlayer);
    st.color = packcolorscale(vs.colorscale);
    st.pad = packglow(glowlayer, glowcol);
    return st;
}

static void gatherlavaomnis()
{
    // Walk every lava face. m.skip is a GL draw-batch stride (consecutive
    // same-orient strips), not a merge: using it dropped the large nappes
    // and kept a leftover 4-cube edge that then failed the area gate.
    loopv(valist)
    {
        vtxarray *va = valist[i];
        if(!va || !va->matbuf || va->matsurfs <= 0) continue;
        loopj(va->matsurfs)
        {
            materialsurface &m = va->matbuf[j];
            if((m.material&MATF_VOLUME) != MAT_LAVA || m.visible != MATSURF_VISIBLE)
                continue;
            int dim = dimension(m.orient);
            vec cen(m.o.x, m.o.y, m.o.z);
            cen[C[dim]] += m.csize * 0.5f;
            cen[R[dim]] += m.rsize * 0.5f;
            vec nrm(0, 0, 0);
            nrm[dim] = dimcoord(m.orient) ? 1.0f : -1.0f;
            float area = float(m.csize) * float(m.rsize);
            addglowcluster(cen, nrm, area, lavacolourvec(m.material), GLOWKIND_LAVA);
        }
    }
}

static void finalizeglowomnis()
{
    hwrtglowomnicount = 0;
    hwrtlavalights = 0;
    loopv(glowclusters)
    {
        glowcluster &g = glowclusters[i];
        bool lava = g.kind == GLOWKIND_LAVA;
        float minarea = lava ? 8.0f : 32.0f;
        if(g.ntris < 1 || g.area < minarea) continue;
        if(hwrtglowomnicount >= HWRT_MAX_GLOWOMNI) break;
        vec pos = vec(g.pos).mul(1.0f/g.ntris);
        vec n = g.n;
        if(n.magnitude() > 1e-4f) n.normalize();
        else n = vec(0, 0, 1);
        pos.add(vec(n).mul(8));
        vec col = vec(g.color).mul(1.0f/g.ntris);
        col.mul(lava ? 1.00f : 0.55f);
        float radius = lava
            ? clamp(48.0f + sqrtf(max(g.area, 0.0f))*0.55f, 56.0f, 120.0f)
            : clamp(24.0f + sqrtf(max(g.area, 0.0f))*0.65f, 32.0f, 96.0f);
        hwrtglowomni &o = hwrtglowomnis[hwrtglowomnicount++];
        o.pos[0] = pos.x; o.pos[1] = pos.y; o.pos[2] = pos.z;
        o.radius = radius;
        o.color[0] = col.x; o.color[1] = col.y; o.color[2] = col.z;
        o.flags = lava ? 3.0f : 2.0f;
        if(lava) hwrtlavalights++;
    }
}

static bool buildshade(const vector<hwrtshadevert> &sverts, const vector<uint> &indices,
                       const vector<hwrtshadetri> &stris, const vector<GLuint> &diffids,
                       const vector<GLuint> &glowids)
{
    destroyshade();
    if(!loadshadefuncs() || !ensuresamplers()) return false;
    if(sverts.empty() || indices.length() < 3 || stris.empty())
        return shadefail("no shade geometry");

    const VkDeviceSize vertbytes = VkDeviceSize(sverts.length())*sizeof(hwrtshadevert);
    const VkDeviceSize idxbytes = VkDeviceSize(indices.length())*sizeof(uint);
    const VkDeviceSize tribytes = VkDeviceSize(stris.length())*sizeof(hwrtshadetri);
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if(!createstorage(world.shadevert, vertbytes, usage, props) ||
       !createstorage(world.shadeidx, idxbytes, usage, props) ||
       !createstorage(world.shadetri, tribytes, usage, props) ||
       !uploadstorage(world.shadevert, sverts.getbuf(), vertbytes) ||
       !uploadstorage(world.shadeidx, indices.getbuf(), idxbytes) ||
       !uploadstorage(world.shadetri, stris.getbuf(), tribytes))
    {
        destroyshade();
        return false;
    }

    vector<GLuint> lmids;
    loopv(lightmaptexs)
    {
        if(lmids.length() >= HWRT_MAX_LM) break;
        lmids.add(lightmaptexs[i].id);
    }
    if(lmids.empty())
    {
        destroyshade();
        return shadefail("no lightmap textures to copy");
    }
    vector<GLuint> diffs;
    loopv(diffids) diffs.add(diffids[i]);
    if(diffs.empty() && notexture && notexture->id) diffs.add(notexture->id);
    if(diffs.empty())
    {
        destroyshade();
        return shadefail("no diffuse textures to copy");
    }

    // Lightmaps stay lod0 (lumel grid). World diffuse gets a mip chain so
    // hitlight can textureGrad from the pixel footprint; skins already upload
    // with mips on their own path and must not share this call.
    if(!uploadtexarray(lmids, world.lm) || !uploadtexarray(diffs, world.diff, true))
    {
        destroyshade();
        return false;
    }
    if(glowids.length())
    {
        if(!uploadtexarray(glowids, world.glow))
        {
            // Glow missing is not fatal: faces just stay unlit extras, not magenta.
            destroytex(world.glow);
            hwrtshadeglow = 0;
            conoutf(CON_WARN, "hwrt: world glow textures failed to copy, glow faces stay dark");
        }
        else hwrtshadeglow = world.glow.layers;
    }
    else hwrtshadeglow = 0;

    world.shadeok = true;
    hwrtshadelm = world.lm.layers;
    hwrtshadediff = world.diff.layers;
    hwrtshadeepoch++;
    conoutf("hwrt: hit-shade %d tris, %d diffuse layers %dx%d mips %d, %d lightmaps %dx%d, %d glow layers, %d glow lights (%d lava)",
            stris.length(), world.diff.layers, world.diff.w, world.diff.h, world.diff.mips,
            world.lm.layers, world.lm.w, world.lm.h, hwrtshadeglow, hwrtglowomnicount, hwrtlavalights);
    return true;
}

static bool ensurecmd()
{
    if(world.cmd) return true;
    VkCommandPoolCreateInfo poolinfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolinfo.queueFamilyIndex = hwrtdev.queuefamily;
    VkResult r = vkCreateCommandPool(hwrtdev.device, &poolinfo, NULL, &world.cmdpool);
    if(r != VK_SUCCESS) return asfail("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cmdinfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdinfo.commandPool = world.cmdpool;
    cmdinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdinfo.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(hwrtdev.device, &cmdinfo, &world.cmd);
    if(r != VK_SUCCESS) return asfail("vkAllocateCommandBuffers", r);
    return true;
}

static void destroyaslocked()
{
    if(hwrtdev.device)
    {
        if(world.tlas) vkDestroyAccelerationStructureKHR(hwrtdev.device, world.tlas, NULL);
        if(world.blas) vkDestroyAccelerationStructureKHR(hwrtdev.device, world.blas, NULL);
    }
    world.tlas = VK_NULL_HANDLE;
    world.blas = VK_NULL_HANDLE;
    destroybuf(world.tlasbuf);
    destroybuf(world.blasbuf);
    destroyshade();
    hwrtworldtris = hwrtworldverts = 0;
    hwrtbindtlas();
}

static bool gatherworld(vector<vec> &positions, vector<uint> &indices,
                        vector<hwrtshadevert> &sverts, vector<hwrtshadetri> &stris,
                        vector<GLuint> &diffids, int &diffoverflow,
                        vector<GLuint> &glowids, int &glowoflow, int &nblend)
{
    glowclusters.setsize(0);
    hwrtglowomnicount = 0;
    gatherlavaomnis();
    nblend = 0;
    vector<vec> glowavgs;
    loopv(valist)
    {
        vtxarray *va = valist[i];
        if(!va || !va->verts || (!va->tris && !va->blendtris)) continue;

        ushort *edata = NULL;
        vertex *vdata = NULL;
        if(!va->tris)
        {
            if(!va->vbuf) continue;
            vdata = new vertex[va->verts];
            gle::bindvbo(va->vbuf);
            glGetBufferSubData_(GL_ARRAY_BUFFER, va->voffset*sizeof(vertex), va->verts*sizeof(vertex), vdata);
            gle::clearvbo();
        }
        else if(!readva(va, edata, vdata)) continue;

        bool ok = true;
        int nidx = 3*va->tris;
        loopj(nidx)
        {
            uint idx = edata[j];
            if(idx < va->voffset || uint(idx - va->voffset) >= uint(va->verts)) { ok = false; break; }
        }
        ushort *blendata = NULL;
        int nblendidx = 3*va->blendtris;
        if(ok && nblendidx > 0)
        {
            if(!va->ebuf) ok = false;
            else
            {
                blendata = new ushort[nblendidx];
                gle::bindebo(va->ebuf);
                glGetBufferSubData_(GL_ELEMENT_ARRAY_BUFFER, (size_t)va->edata + nidx*sizeof(ushort), nblendidx*sizeof(ushort), blendata);
                gle::clearebo();
                loopj(nblendidx)
                {
                    uint idx = blendata[j];
                    if(idx < va->voffset || uint(idx - va->voffset) >= uint(va->verts)) { ok = false; break; }
                }
            }
        }
        if(!ok)
        {
            conoutf(CON_WARN, "hwrt: skipping a vertex array with indices outside voffset");
            delete[] edata;
            delete[] blendata;
            delete[] vdata;
            continue;
        }

        uint base = uint(positions.length());
        loopj(va->verts)
        {
            positions.add(vdata[j].pos);
            hwrtshadevert sv;
            sv.tc[0] = vdata[j].tc.x;
            sv.tc[1] = vdata[j].tc.y;
            sv.lm[0] = float(vdata[j].lm.x) / 32767.0f;
            sv.lm[1] = float(vdata[j].lm.y) / 32767.0f;
            // readva() returns the VBO, which stores packed normals after
            // genverts()' GL_BYTE flip. Undo that and unpack with the same
            // map bvec used when they were authored.
            bvec4 pn = vdata[j].norm;
            pn.flip();
            vec n = pn.tonormal();
            if(n.magnitude() > 1e-6f) n.normalize();
            else n = vec(0, 0, 1);
            sv.n[0] = n.x;
            sv.n[1] = n.y;
            sv.n[2] = n.z;
            sv.pad = sv.tc[0];
            sv.pos[0] = vdata[j].pos.x;
            sv.pos[1] = vdata[j].pos.y;
            sv.pos[2] = vdata[j].pos.z;
            sv.pad2 = sv.tc[1];
            sverts.add(sv);
        }
        int counted = 0;
        if(va->eslist) loopk(va->texs) counted += va->eslist[k].length[1];
        int blendcounted = 0;
        if(va->eslist) loopk(va->blends) blendcounted += va->eslist[va->texs + k].length[1];
        hashtable<hwrttrikey, uchar> twins;
        hashtable<hwrttrikey, hwrttritwin> dirttwin;
        bool useblend = blendata && va->eslist && blendcounted == nblendidx && (nblendidx%3)==0;
        if(useblend)
        {
            loopj(nblendidx/3)
            {
                uint i0 = uint(blendata[j*3+0] - va->voffset);
                uint i1 = uint(blendata[j*3+1] - va->voffset);
                uint i2 = uint(blendata[j*3+2] - va->voffset);
                twins[hwrtmakekey(vdata[i0].pos, vdata[i1].pos, vdata[i2].pos)] = 1;
            }
        }
        if(va->eslist && counted == nidx && (nidx%3)==0)
        {
            int cursor = 0;
            loopk(va->texs)
            {
                elementset &e = va->eslist[k];
                int n = e.length[1];
                int glowlayer = -1;
                vec glowcol(1, 1, 1), glowavg(1, 1, 1);
                hwrtshadetri st = makeshadestri(e, diffids, diffoverflow, glowids, glowavgs, glowoflow, -1, glowlayer, glowcol, glowavg);
                loopj(n/3)
                {
                    uint i0 = base + uint(edata[cursor+0] - va->voffset);
                    uint i1 = base + uint(edata[cursor+1] - va->voffset);
                    uint i2 = base + uint(edata[cursor+2] - va->voffset);
                    cursor += 3;
                    if(useblend)
                    {
                        hwrttrikey key = hwrtmakekey(positions[i0], positions[i1], positions[i2]);
                        if(twins.access(key))
                        {
                            // Coplanar dirt face: it leaves the BLAS, but keep
                            // its corners so the grass can read its UVs.
                            hwrttritwin tw;
                            tw.i[0] = i0; tw.i[1] = i1; tw.i[2] = i2;
                            dirttwin[key] = tw;
                            continue;
                        }
                    }
                    emitworldtri(positions, indices, stris, i0, i1, i2, st, glowlayer, glowcol, glowavg, lookupvslot(e.texture, false).slot);
                }
            }
        }
        else
        {
            loopj(nidx) indices.add(base + uint(edata[j] - va->voffset));
            hwrtshadetri st;
            memset(&st, 0, sizeof(st));
            st.layer = 0xFFFFFFFFu;
            st.lmid = packlmidbot(0, -1);
            st.pad = 0xFFu;
            loopj(va->tris) stris.add(st);
        }
        if(useblend)
        {
            int cursor = 0;
            loopk(va->blends)
            {
                elementset &e = va->eslist[va->texs + k];
                int n = e.length[1];
                int botlayer = -1;
                VSlot &vs = lookupvslot(e.texture, false);
                if(vs.layer) botlayer = slotdiffuselayer(lookupvslot(vs.layer, false).slot, diffids, diffoverflow);
                int glowlayer = -1;
                vec glowcol(1, 1, 1), glowavg(1, 1, 1);
                hwrtshadetri st = makeshadestri(e, diffids, diffoverflow, glowids, glowavgs, glowoflow, botlayer, glowlayer, glowcol, glowavg);
                loopj(n/3)
                {
                    uint i0 = base + uint(blendata[cursor+0] - va->voffset);
                    uint i1 = base + uint(blendata[cursor+1] - va->voffset);
                    uint i2 = base + uint(blendata[cursor+2] - va->voffset);
                    cursor += 3;
                    if(botlayer >= 0) copytwintc(sverts, positions, dirttwin, i0, i1, i2);
                    emitworldtri(positions, indices, stris, i0, i1, i2, st, glowlayer, glowcol, glowavg, lookupvslot(e.texture, false).slot);
                    nblend++;
                }
            }
        }

        delete[] edata;
        delete[] blendata;
        delete[] vdata;
    }
    finalizeglowomnis();
    return indices.length() >= 3 && positions.length() >= 3;
}

static bool createas(VkAccelerationStructureKHR &as, hwrtbuf &buf, VkAccelerationStructureTypeKHR type, VkDeviceSize size)
{
    size = alignup(size, 256);
    if(!createbuf(buf, size,
                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        return false;

    VkAccelerationStructureCreateInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    info.buffer = buf.buffer;
    info.offset = 0;
    info.size = size;
    info.type = type;
    VkResult r = vkCreateAccelerationStructureKHR(hwrtdev.device, &info, NULL, &as);
    if(r != VK_SUCCESS) { destroybuf(buf); return asfail("vkCreateAccelerationStructureKHR", r); }
    return true;
}

static VkDeviceAddress asaddress(VkAccelerationStructureKHR as)
{
    VkAccelerationStructureDeviceAddressInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    info.accelerationStructure = as;
    return vkGetAccelerationStructureDeviceAddressKHR(hwrtdev.device, &info);
}

bool hwrthasworld()
{
    return world.tlas != VK_NULL_HANDLE;
}

VkAccelerationStructureKHR hwrtgettlas()
{
    return world.tlas;
}

VkDeviceAddress hwrtworldblasaddr()
{
    if(world.blas == VK_NULL_HANDLE || !vkGetAccelerationStructureDeviceAddressKHR) return 0;
    return asaddress(world.blas);
}

void hwrtdestroyworld()
{
    if(hwrtdev.device) vkDeviceWaitIdle(hwrtdev.device);
    hwrtdestroydynents();
    destroyaslocked();
    if(hwrtdev.device)
    {
        if(world.lmsampler && hwrtDestroySampler) hwrtDestroySampler(hwrtdev.device, world.lmsampler, NULL);
        if(world.diffsampler && hwrtDestroySampler) hwrtDestroySampler(hwrtdev.device, world.diffsampler, NULL);
        if(world.cmdpool) vkDestroyCommandPool(hwrtdev.device, world.cmdpool, NULL);
    }
    world.lmsampler = VK_NULL_HANDLE;
    world.diffsampler = VK_NULL_HANDLE;
    world.cmdpool = VK_NULL_HANDLE;
    world.cmd = VK_NULL_HANDLE;
}

// Classic lighting on a map load: free the previous map's acceleration
// structures so a later switch to RT rebuilds this map (hwrthasworld false).
void hwrtdropworld()
{
    if(!hwrtdev.ok() || hwrtfailed) return;
    if(!hwrthasworld()) return;
    if(!hwrtwaitidle("vkDeviceWaitIdle (classic map load)")) return;
    hwrtdestroydynents();
    destroyaslocked();
}

void hwrtrebuildworld()
{
    if(!hwrtdev.ok() || !hwrtdev.rayquery || !vkCreateAccelerationStructureKHR) return;
    // Nothing to rebuild on a device that is already gone, and touching it
    // again is what turns a reported loss into a killed process.
    if(hwrtfailed) return;

    if(!hwrtwaitidle("vkDeviceWaitIdle (before world rebuild)")) return;
    hwrtdestroydynents();
    destroyaslocked();
    hwrtnotelightsrebuild();

    vector<vec> positions;
    vector<uint> indices;
    vector<hwrtshadevert> sverts;
    vector<hwrtshadetri> stris;
    vector<GLuint> diffids;
    int diffoverflow = 0, glowoflow = 0, nblend = 0;
    vector<GLuint> glowids;
    if(!gatherworld(positions, indices, sverts, stris, diffids, diffoverflow, glowids, glowoflow, nblend))
    {
        hwrtworldtris = hwrtworldverts = 0;
        return;
    }

    if(!ensurecmd()) { destroyaslocked(); return; }

    const uint nverts = uint(positions.length());
    const uint nidx = uint(indices.length());
    const uint ntris = nidx/3;
    const VkDeviceSize vertstride = 16;
    const VkDeviceSize vertbytes = VkDeviceSize(nverts)*vertstride;
    const VkDeviceSize idxbytes = VkDeviceSize(nidx)*sizeof(uint);

    hwrtbuf vbuf, ibuf, instbuf, scratch;
    memset(&vbuf, 0, sizeof(vbuf));
    memset(&ibuf, 0, sizeof(ibuf));
    memset(&instbuf, 0, sizeof(instbuf));
    memset(&scratch, 0, sizeof(scratch));

    const VkBufferUsageFlags geomusage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if(!createbuf(vbuf, vertbytes, geomusage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
       !createbuf(ibuf, idxbytes, geomusage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        destroybuf(vbuf); destroybuf(ibuf);
        destroyaslocked();
        return;
    }

    float *vmap = NULL;
    VkResult r = vkMapMemory(hwrtdev.device, vbuf.memory, 0, vertbytes, 0, (void **)&vmap);
    if(r != VK_SUCCESS || !vmap)
    {
        asfail("vkMapMemory", r);
        destroybuf(vbuf); destroybuf(ibuf);
        destroyaslocked();
        return;
    }
    loopi(int(nverts))
    {
        vmap[i*4 + 0] = positions[i].x;
        vmap[i*4 + 1] = positions[i].y;
        vmap[i*4 + 2] = positions[i].z;
        vmap[i*4 + 3] = 0;
    }
    vkUnmapMemory(hwrtdev.device, vbuf.memory);
    if(!uploadbuf(ibuf, indices.getbuf(), idxbytes))
    {
        destroybuf(vbuf); destroybuf(ibuf);
        destroyaslocked();
        return;
    }

    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo.geometry.triangles.vertexData.deviceAddress = vbuf.address;
    geo.geometry.triangles.vertexStride = vertstride;
    geo.geometry.triangles.maxVertex = nverts - 1;
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geo.geometry.triangles.indexData.deviceAddress = ibuf.address;

    VkAccelerationStructureBuildGeometryInfoKHR blasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    blasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasinfo.geometryCount = 1;
    blasinfo.pGeometries = &geo;

    uint32_t primcount = ntris;
    VkAccelerationStructureBuildSizesInfoKHR blassizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(hwrtdev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &blasinfo, &primcount, &blassizes);

    if(!createas(world.blas, world.blasbuf, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, blassizes.accelerationStructureSize))
    {
        destroybuf(vbuf); destroybuf(ibuf);
        destroyaslocked();
        return;
    }

    VkDeviceSize scralign = hwrtdev.scratchalign ? hwrtdev.scratchalign : 256;
    VkDeviceSize scratchbytes = alignup(blassizes.buildScratchSize, scralign) + scralign;
    if(!createbuf(scratch, scratchbytes,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
    {
        destroybuf(vbuf); destroybuf(ibuf);
        destroyaslocked();
        return;
    }

    blasinfo.dstAccelerationStructure = world.blas;

    VkAccelerationStructureBuildRangeInfoKHR blasrange = {};
    blasrange.primitiveCount = ntris;
    const VkAccelerationStructureBuildRangeInfoKHR *blasranges = &blasrange;

    // World-only fallback TLAS: one identity instance. Phase 6 rebuilds a
    // larger live TLAS on the frame command buffer when dynents are available.
    hwrtinstance inst;
    memset(&inst, 0, sizeof(inst));
    inst.transform[0] = 1;
    inst.transform[5] = 1;
    inst.transform[10] = 1;
    inst.custommask = uint32_t(HWRT_RAYMASK_ALL) << 24;
    inst.sbtflags = uint32_t(VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR) << 24;
    inst.reference = asaddress(world.blas);
    if(!inst.reference)
    {
        asfail("BLAS device address is 0");
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        destroyaslocked();
        return;
    }
    if(!createbuf(instbuf, sizeof(inst), geomusage,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
       !uploadbuf(instbuf, &inst, sizeof(inst)))
    {
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch); destroybuf(instbuf);
        destroyaslocked();
        return;
    }

    VkAccelerationStructureGeometryKHR tgeo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.arrayOfPointers = VK_FALSE;
    tgeo.geometry.instances.data.deviceAddress = instbuf.address;

    VkAccelerationStructureBuildGeometryInfoKHR tlasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tlasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasinfo.geometryCount = 1;
    tlasinfo.pGeometries = &tgeo;

    uint32_t instcount = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlassizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(hwrtdev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasinfo, &instcount, &tlassizes);

    if(!createas(world.tlas, world.tlasbuf, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlassizes.accelerationStructureSize))
    {
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch); destroybuf(instbuf);
        destroyaslocked();
        return;
    }

    VkDeviceSize tscratch = alignup(tlassizes.buildScratchSize, scralign) + scralign;
    if(tscratch > scratch.size)
    {
        destroybuf(scratch);
        if(!createbuf(scratch, tscratch,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            destroybuf(vbuf); destroybuf(ibuf); destroybuf(instbuf);
            destroyaslocked();
            return;
        }
    }

    tlasinfo.dstAccelerationStructure = world.tlas;

    // Both builds share this scratch buffer, ordered by the barrier recorded
    // between them, so both addresses are taken here: after the last point
    // where the buffer can still be reallocated. Taking the BLAS one earlier
    // left it pointing into freed memory every time the TLAS needed a bigger
    // buffer than the BLAS, which is the normal case on a small world. The
    // build then scribbled outside its allocation and the driver dropped the
    // device at random, most often while something else was busy on the GPU.
    blasinfo.scratchData.deviceAddress = alignedaddr(scratch.address, scralign);
    tlasinfo.scratchData.deviceAddress = alignedaddr(scratch.address, scralign);

    VkAccelerationStructureBuildRangeInfoKHR tlasrange = {};
    tlasrange.primitiveCount = 1;
    const VkAccelerationStructureBuildRangeInfoKHR *tlasranges = &tlasrange;

    r = vkResetCommandBuffer(world.cmd, 0);
    if(r != VK_SUCCESS)
    {
        asfail("vkResetCommandBuffer", r);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch); destroybuf(instbuf);
        destroyaslocked();
        return;
    }
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(world.cmd, &begin);
    if(r != VK_SUCCESS)
    {
        asfail("vkBeginCommandBuffer", r);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch); destroybuf(instbuf);
        destroyaslocked();
        return;
    }

    vkCmdBuildAccelerationStructuresKHR(world.cmd, 1, &blasinfo, &blasranges);

    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(world.cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         0, 1, &barrier, 0, NULL, 0, NULL);

    vkCmdBuildAccelerationStructuresKHR(world.cmd, 1, &tlasinfo, &tlasranges);

    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(world.cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);

    r = vkEndCommandBuffer(world.cmd);
    if(r != VK_SUCCESS)
    {
        asfail("vkEndCommandBuffer", r);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch); destroybuf(instbuf);
        destroyaslocked();
        return;
    }

    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &world.cmd;
    r = vkQueueSubmit(hwrtdev.queue, 1, &submit, VK_NULL_HANDLE);
    if(r != VK_SUCCESS)
    {
        asfail("vkQueueSubmit (AS build)", r);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch); destroybuf(instbuf);
        destroyaslocked();
        return;
    }
    if(!hwrtwaitidle("vkDeviceWaitIdle (AS build)"))
    {
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch); destroybuf(instbuf);
        destroyaslocked();
        return;
    }

    destroybuf(vbuf);
    destroybuf(ibuf);
    destroybuf(instbuf);
    destroybuf(scratch);

    hwrtworldverts = int(nverts);
    hwrtworldtris = int(ntris);
    hwrtbindtlas();
    conoutf("hwrt: world BLAS %d triangles, %d verts", hwrtworldtris, hwrtworldverts);
    if(nblend) conoutf("hwrt: %d texlayer faces mixed from lightmap alpha", nblend);

    if(sverts.length() != int(nverts) || stris.length() != int(ntris))
        shadefail("shade vertex/triangle counts disagree with the BLAS");
    else
    {
        if(diffoverflow)
            conoutf(CON_WARN, "hwrt: %d opaque textures over the %d diffuse cap, those faces go magenta", diffoverflow, int(HWRT_MAX_DIFFUSE));
        if(glowoflow)
            conoutf(CON_WARN, "hwrt: %d glow textures over the %d cap, those faces have no glow", glowoflow, int(HWRT_MAX_DIFFUSE));
        buildshade(sverts, indices, stris, diffids, glowids);
    }
    hwrtnotelightsrebuild();
    hwrtupdatelights();
    hwrtrebuilddynents();
}

bool hwrthasshade()
{
    return world.shadeok && world.tlas != VK_NULL_HANDLE &&
           world.shadevert.buffer && world.shadeidx.buffer && world.shadetri.buffer &&
           world.lm.view && world.diff.view && world.lmsampler && world.diffsampler;
}

void hwrtwriteshadebindings(VkDescriptorSet set)
{
    if(!set || !hwrthasshade()) return;

    VkDescriptorBufferInfo vinfo = { world.shadevert.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo iinfo = { world.shadeidx.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo tinfo = { world.shadetri.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo lminfo = {};
    lminfo.sampler = world.lmsampler;
    lminfo.imageView = world.lm.view;
    lminfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo diffinfo = {};
    diffinfo.sampler = world.diffsampler;
    diffinfo.imageView = world.diff.view;
    diffinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[5];
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 2;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &vinfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 3;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &iinfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 4;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &tinfo;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 5;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &lminfo;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = set;
    writes[4].dstBinding = 6;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo = &diffinfo;
    vkUpdateDescriptorSets(hwrtdev.device, 5, writes, 0, NULL);
}

VkImageView hwrtworlddiffuseview() { return world.diff.view; }
VkSampler hwrtworlddiffusesampler() { return world.diffsampler; }
VkImageView hwrtworldglowview() { return world.glow.view ? world.glow.view : world.diff.view; }
VkSampler hwrtworldglowsampler() { return world.diffsampler; }

void hwrtwritelightgeombindings(VkDescriptorSet set)
{
    if(!set || !hwrthasshade()) return;

    VkDescriptorBufferInfo vinfo = { world.shadevert.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo iinfo = { world.shadeidx.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo tinfo = { world.shadetri.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo diffinfo = {};
    diffinfo.sampler = world.diffsampler;
    diffinfo.imageView = world.diff.view;
    diffinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo glowinfo = {};
    glowinfo.sampler = hwrtworldglowsampler();
    glowinfo.imageView = hwrtworldglowview();
    glowinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(!glowinfo.imageView || !glowinfo.sampler) return;
    VkDescriptorImageInfo lminfo = {};
    lminfo.sampler = world.lmsampler ? world.lmsampler : world.diffsampler;
    lminfo.imageView = world.lm.view ? world.lm.view : world.diff.view;
    lminfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(!lminfo.imageView || !lminfo.sampler) return;

    VkWriteDescriptorSet writes[6];
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 2;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &vinfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 3;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &iinfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 4;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &tinfo;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 5;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &diffinfo;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = set;
    writes[4].dstBinding = 10;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo = &glowinfo;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = set;
    writes[5].dstBinding = 12;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &lminfo;
    vkUpdateDescriptorSets(hwrtdev.device, 6, writes, 0, NULL);
}
