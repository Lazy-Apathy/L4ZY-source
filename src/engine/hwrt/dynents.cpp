// dynents.cpp: mapmodels and dynents as extra TLAS instances (phase 6).
//
// One rest-pose BLAS per unique model, gathered from the existing BIH
// (mesh.xform baked into the vertices). Opaque meshes and MESH_ALPHA
// (foliage, CTF flags) share that BLAS as two geometries: the trunk stays
// VK_GEOMETRY_OPAQUE_BIT_KHR, the leaves do not, so a ray-query candidate
// loop can keep the holes in the skin. Those BLASes are rebuilt on
// allchanged() (map load, remip), where vkDeviceWaitIdle is allowed. Every
// playermodel a client can wear is copied there too, so the first stranger
// does not rebuild the skin atlas mid-frame. Animated dynents (players, bots,
// ragdolls) get a per-pose BLAS: CPU-skinned verts, ALLOW_UPDATE_BIT,
// MODE_UPDATE_KHR refit on the frame command buffer. The cache is keyed like
// skelcache (animstate / pitch / ragdoll), so eight players in the same cycle
// share one or two refits. Per frame the instance buffer is rewritten and the
// live TLAS is rebuilt on the Vulkan frame command buffer after the glready
// wait — no CPU wait, no world BLAS rebuild. Failure of this path logs once
// and leaves the world-only TLAS.
//
// customIndex 0 is the world. Anything else is an index into the per-frame
// geometry table: HWRT_GEOM_MODEL0 + cache slot for a rest-pose BLAS,
// HWRT_GEOM_ANIM0 + slot for an animated one. The lighting shader uses that to
// find the model's UVs, normals and skin layer, so mode 7 can shade a player
// with the same lights as the world instead of handing the pixels back to GL.

#include "engine.h"
#include "hwrt/hwrt.h"

int hwrtmapmodelinsts = 0, hwrtdynentinsts = 0, hwrtragdollinsts = 0, hwrtinstancecount = 0;
int hwrtdroppedinsts = 0;
int hwrtanimcount = 0, hwrtanimbuilds = 0, hwrtanimrefits = 0;
int hwrtskinlayers = 0, hwrtskinw = 0, hwrtskinh = 0, hwrtgeomcount = 0;

vec hwrtteleporthole(0, 0, 7.3f);
bool hwrthasteleporthole = false;

struct hwrtbuf
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    VkDeviceSize size;
};

struct hwrtmodelblas
{
    model *m;
    VkAccelerationStructureKHR blas;
    hwrtbuf buf;
    VkDeviceAddress addr;
    // Attributes the lighting shader reads through their device addresses. The
    // index buffer the BLAS was built from is kept rather than freed, so the
    // shader walks exactly the triangles the BLAS reports.
    hwrtbuf attrbuf, idxbuf, tribuf;
    uint ntris;
    uint nopaque; // triangles in BLAS geometry 0 (opaque). ntris-nopaque are MESH_ALPHA.
};

struct hwrtanimslot
{
    model *m;
    hwrtposekey key;
    hwrtbuf vbuf, ibuf, asbuf, attrbuf, tribuf;
    VkAccelerationStructureKHR blas;
    VkDeviceAddress addr;
    float *vmap;
    hwrtmodelvert *amap;
    uint *tmap;
    uint nverts, ntris, filledhash;
    int millis, updates;
    uchar cmd; // 0 none, 1 update, 2 build
    bool used;
};

struct hwrtdynslot
{
    hwrtbuf instbuf, tlasbuf, scratch, blasscratch, geombuf;
    VkAccelerationStructureKHR tlas;
    hwrtinstance *instmap;
    hwrtmodelgeom *geommap;
    uint32_t ninst;
    hwrtanimslot anim[HWRT_MAX_ANIM_BLAS];
};

struct hwrtdynstate
{
    hwrtmodelblas cache[HWRT_MAX_MODEL_BLAS];
    int ncache;
    hwrtdynslot slot[HWRT_FRAMES_IN_FLIGHT];
    VkCommandPool cmdpool;
    VkCommandBuffer cmd;
    // instfulllogged is separate from overflowlogged on purpose: they used to
    // share a flag, so a map that ran out of instances silenced the "out of
    // unique model BLASes" warning and hid the second cap behind the first.
    bool failed, ready, maplogged, norqlogged, overflowlogged, instfulllogged, animlogged, attrlogged;
};

static hwrtdynstate dyn;

// Model skins are per-`skin` in animmodel, not in the world diffuse array, so
// they get a second sampled 2D array. A layer index is stable until compact,
// which rewrites every triangle buffer that still points at the old list.
struct hwrtskinstate
{
    vector<GLuint> ids;
    hwrttexarray tex;
    VkSampler sampler;
    int uploaded;
    bool failed, overflowlogged;
};

static hwrtskinstate skins;
static hwrtbuf dummygeom;
static GLuint mdlenvgl = 0, mdlenvuploaded = 0;
static hwrttexarray mdlenv;
static VkSampler mdlenvsampler = VK_NULL_HANDLE;

void hwrtnoteenvmap(GLuint gltex)
{
    if(gltex) mdlenvgl = gltex;
}
// Map load preloads every playermodel. Those stay pinned so a stranger does
// not freeze the frame. Looks that were only copied because someone wore
// them (a friend colour, a one-off attachment) are dropped when nobody
// still needs them, so the atlas can shrink instead of falling back to 512.
static bool allowskinprune = true;
static vector<model *> pinnedmodels;

// Scene models are rendermodel()'d from rendergame(), not from the mapmodel
// or dynent lists: CTF flags, flying rockets, bouncing grenades. The main
// pass records them so the rest-pose BLAS can be instanced at the same
// origin + yaw + pitch GL used. Pitch matters for rockets and tumbling
// grenades; flags leave it at 0. Rifle / rocket smoke has no mesh: live
// PART_SMOKE sprites are harvested as spheres that cast sun / lamp shadows
// (CASTER) without showing up in the camera or RTAO.
enum { HWRT_MAX_SCENE_MODELS = 96 };
struct hwrtscenemodel
{
    char name[64];
    vec o;
    float yaw, pitch;
};
static hwrtscenemodel scenemodels[HWRT_MAX_SCENE_MODELS];
static int nscenemodels = 0;
static bool scenecollecting = false;

enum { HWRT_MAX_SMOKE_PUFFS = 1024 };
struct hwrtsmokepuff
{
    vec o;
    float radius;
    uchar opacity;
};
static hwrtsmokepuff smokepuffs[HWRT_MAX_SMOKE_PUFFS];
static int nsmokepuffs = 0;

// Sentinel model* so the smoke-puff sphere keeps its cache slot. Never
// dereference: it is not a real model. prunecache / cachehasunused skip it,
// and buildmodelblas will not reuse the slot (m is non-NULL).
static char hwrtbulletmarker;
static model *const hwrtbulletmdl = (model *)&hwrtbulletmarker;
static hwrtmodelblas *bulletc = NULL;
static GLuint bullettex = 0;

static bool isbulletslot(const hwrtmodelblas &c)
{
    return c.m == hwrtbulletmdl;
}

static bool isbulletname(const char *mdl)
{
    return mdl && !strcmp(mdl, "projectiles/bullet");
}

static bool isscenemdl(const char *mdl)
{
    if(!mdl || !mdl[0]) return false;
    if(!strncmp(mdl, "flags/", 6)) return true;
    if(!strncmp(mdl, "projectiles/", 12)) return true;
    return false;
}

void hwrtbeginscenemodels()
{
    nscenemodels = 0;
    nsmokepuffs = 0;
    scenecollecting = true;
}

void hwrtendscenemodels()
{
    scenecollecting = false;
}

void hwrtnotescenemodel(const char *mdl, const vec &o, float yaw, float pitch, int flags, dynent *d)
{
    if(!scenecollecting) return;
    if(!mdl || !mdl[0] || d) return;
    if(flags & (MDL_HUD | MDL_GHOST)) return;
    if(!isscenemdl(mdl)) return;
    if(nscenemodels >= HWRT_MAX_SCENE_MODELS) return;
    // Flags can be recorded twice in one pass; merge those. Flying
    // projectiles that happen to pass within half a unit of each other
    // must stay separate or two grenades become one shadow.
    if(!strncmp(mdl, "flags/", 6))
    {
        loopi(nscenemodels)
        {
            if(!strcmp(scenemodels[i].name, mdl) && scenemodels[i].o.dist(o) < 0.5f)
                return;
        }
    }
    copystring(scenemodels[nscenemodels].name, mdl, 64);
    scenemodels[nscenemodels].o = o;
    scenemodels[nscenemodels].yaw = yaw;
    scenemodels[nscenemodels].pitch = pitch;
    nscenemodels++;
}

void hwrtnotesmokepuff(const vec &o, float radius, int opacity)
{
    if(!scenecollecting) return;
    if(nsmokepuffs >= HWRT_MAX_SMOKE_PUFFS) return;
    if(radius < 0.15f) return;
    opacity = clamp(opacity, 0, 255);
    if(opacity < 5) return;
    smokepuffs[nsmokepuffs].o = o;
    smokepuffs[nsmokepuffs].radius = radius;
    smokepuffs[nsmokepuffs].opacity = uchar(opacity);
    nsmokepuffs++;
}

static bool skipdynent(dynent *d);

static bool isdefaultteleporter(model *m)
{
    return m && m->name && !strcmp(m->name, "teleporter");
}

static bool hwrtalwaysvisibleent(const extentity &e)
{
    const char *tn = entities::entname(e.type);
    return tn && !strcmp(tn, "teleport");
}

// GL rasterises the whole teleporter, including the filled blue plate. The RT
// copy drops that plate so a lamp in the hole can shine through; the metal
// rim stays and casts. Only this model: one mesh, distinguished by radius in
// the ring plane, not by a sub-object name.
static void punchteleporterdisk(vector<vec> &positions, vector<uint> &indices, vector<uint> &tris)
{
    int ntri = indices.length()/3;
    if(ntri < 1 || positions.empty()) return;

    vec bmin = positions[0], bmax = positions[0];
    loopv(positions) { bmin.min(positions[i]); bmax.max(positions[i]); }
    vec ext = vec(bmax).sub(bmin);
    int axis = 0;
    if(ext.y <= ext.x && ext.y <= ext.z) axis = 1;
    else if(ext.z <= ext.x && ext.z <= ext.y) axis = 2;
    int ua = (axis+1)%3, ub = (axis+2)%3;
    vec center = vec(bmin).add(bmax).mul(0.5f);
    float rmax = 0;
    loopv(positions)
    {
        float da = positions[i][ua] - center[ua];
        float db = positions[i][ub] - center[ub];
        rmax = max(rmax, sqrtf(da*da + db*db));
    }
    hwrtteleporthole = center;
    hwrthasteleporthole = true;
    if(rmax < 1e-3f) return;
    const float rcut = 0.40f * rmax;

    vector<uint> keepidx, keeptri;
    int dropped = 0;
    loopi(ntri)
    {
        vec c(0, 0, 0);
        loopk(3) c.add(positions[indices[i*3+k]]);
        c.mul(1.0f/3.0f);
        float da = c[ua] - center[ua], db = c[ub] - center[ub];
        if(sqrtf(da*da + db*db) < rcut) { dropped++; continue; }
        loopk(3) keepidx.add(indices[i*3+k]);
        if(tris.inrange(i)) keeptri.add(tris[i]);
    }
    if(keepidx.length() < 3)
    {
        static bool loggedempty = false;
        if(!loggedempty)
        {
            conoutf(CON_WARN, "hwrt: teleporter disk punch left no tris, keeping the full mesh");
            loggedempty = true;
        }
        return;
    }
    indices.setsize(0);
    loopv(keepidx) indices.add(keepidx[i]);
    if(tris.length())
    {
        tris.setsize(0);
        loopv(keeptri) tris.add(keeptri[i]);
    }
    static bool logged = false;
    if(!logged)
    {
        conoutf("hwrt: teleporter BLAS dropped %d disk tris (%d remain), hole %.1f %.1f %.1f",
                dropped, keepidx.length()/3, center.x, center.y, center.z);
        logged = true;
    }
}

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

static bool dynfail(const char *what, VkResult r = VK_SUCCESS)
{
    if(dyn.failed) return false;
    dyn.failed = true;
    dyn.ready = false;
    if(r != VK_SUCCESS) conoutf(CON_WARN, "hwrt: %s failed (%s), dynents not in the TLAS", what, hwrtresultstr(r));
    else conoutf(CON_WARN, "hwrt: %s, dynents not in the TLAS", what);
    return false;
}

static bool allocbuf(hwrtbuf &b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool deviceaddress = true)
{
    memset(&b, 0, sizeof(b));
    if(size < 1) size = 1;

    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(hwrtdev.device, &info, NULL, &b.buffer);
    if(r != VK_SUCCESS) { memset(&b, 0, sizeof(b)); return false; }

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
    if(memtype < 0) { destroybuf(b); return false; }

    VkMemoryAllocateFlagsInfo flags = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    if(deviceaddress) alloc.pNext = &flags;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = uint32_t(memtype);
    r = vkAllocateMemory(hwrtdev.device, &alloc, NULL, &b.memory);
    if(r != VK_SUCCESS) { destroybuf(b); return false; }
    r = vkBindBufferMemory(hwrtdev.device, b.buffer, b.memory, 0);
    if(r != VK_SUCCESS) { destroybuf(b); return false; }

    b.size = req.size;
    if(deviceaddress)
    {
        VkBufferDeviceAddressInfo addrinfo = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrinfo.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(hwrtdev.device, &addrinfo);
        if(!b.address) { destroybuf(b); return false; }
    }
    return true;
}

static bool createbuf(hwrtbuf &b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, bool deviceaddress = true)
{
    if(allocbuf(b, size, usage, props, deviceaddress)) return true;
    return dynfail("could not allocate a dynent buffer");
}

static bool uploadbuf(hwrtbuf &b, const void *data, VkDeviceSize bytes)
{
    void *mapped = NULL;
    VkResult r = vkMapMemory(hwrtdev.device, b.memory, 0, bytes, 0, &mapped);
    if(r != VK_SUCCESS || !mapped) return false;
    memcpy(mapped, data, size_t(bytes));
    vkUnmapMemory(hwrtdev.device, b.memory);
    return true;
}

static VkDeviceAddress asaddress(VkAccelerationStructureKHR as)
{
    VkAccelerationStructureDeviceAddressInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    info.accelerationStructure = as;
    return vkGetAccelerationStructureDeviceAddressKHR(hwrtdev.device, &info);
}

static bool createas(VkAccelerationStructureKHR &as, hwrtbuf &buf, VkAccelerationStructureTypeKHR type, VkDeviceSize size, bool fatal = true)
{
    size = alignup(size, 256);
    if(!allocbuf(buf, size,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        return fatal ? dynfail("could not allocate a dynent AS buffer") : false;

    VkAccelerationStructureCreateInfoKHR info = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    info.buffer = buf.buffer;
    info.offset = 0;
    info.size = size;
    info.type = type;
    VkResult r = vkCreateAccelerationStructureKHR(hwrtdev.device, &info, NULL, &as);
    if(r != VK_SUCCESS)
    {
        destroybuf(buf);
        return fatal ? dynfail("vkCreateAccelerationStructureKHR (dynent)", r) : false;
    }
    return true;
}

static bool ensurecmd()
{
    if(dyn.cmd) return true;
    VkCommandPoolCreateInfo poolinfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolinfo.queueFamilyIndex = hwrtdev.queuefamily;
    VkResult r = vkCreateCommandPool(hwrtdev.device, &poolinfo, NULL, &dyn.cmdpool);
    if(r != VK_SUCCESS) return dynfail("vkCreateCommandPool (dynent)", r);
    VkCommandBufferAllocateInfo cmdinfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cmdinfo.commandPool = dyn.cmdpool;
    cmdinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdinfo.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(hwrtdev.device, &cmdinfo, &dyn.cmd);
    if(r != VK_SUCCESS) return dynfail("vkAllocateCommandBuffers (dynent)", r);
    return true;
}

static void destroymodel(hwrtmodelblas &c)
{
    if(hwrtdev.device && c.blas) vkDestroyAccelerationStructureKHR(hwrtdev.device, c.blas, NULL);
    destroybuf(c.buf);
    destroybuf(c.attrbuf);
    destroybuf(c.idxbuf);
    destroybuf(c.tribuf);
    memset(&c, 0, sizeof(c));
}

static void destroyanim(hwrtanimslot &a)
{
    if(hwrtdev.device)
    {
        if(a.vmap && a.vbuf.memory) vkUnmapMemory(hwrtdev.device, a.vbuf.memory);
        if(a.amap && a.attrbuf.memory) vkUnmapMemory(hwrtdev.device, a.attrbuf.memory);
        if(a.tmap && a.tribuf.memory) vkUnmapMemory(hwrtdev.device, a.tribuf.memory);
    }
    a.vmap = NULL;
    a.amap = NULL;
    a.tmap = NULL;
    if(hwrtdev.device && a.blas) vkDestroyAccelerationStructureKHR(hwrtdev.device, a.blas, NULL);
    destroybuf(a.vbuf);
    destroybuf(a.ibuf);
    destroybuf(a.asbuf);
    destroybuf(a.attrbuf);
    destroybuf(a.tribuf);
    memset(&a, 0, sizeof(a));
}

static void destroyslot(hwrtdynslot &s)
{
    if(hwrtdev.device)
    {
        if(s.instmap && s.instbuf.memory) vkUnmapMemory(hwrtdev.device, s.instbuf.memory);
        if(s.geommap && s.geombuf.memory) vkUnmapMemory(hwrtdev.device, s.geombuf.memory);
    }
    s.instmap = NULL;
    s.geommap = NULL;
    if(hwrtdev.device && s.tlas) vkDestroyAccelerationStructureKHR(hwrtdev.device, s.tlas, NULL);
    s.tlas = VK_NULL_HANDLE;
    destroybuf(s.instbuf);
    destroybuf(s.tlasbuf);
    destroybuf(s.scratch);
    destroybuf(s.blasscratch);
    destroybuf(s.geombuf);
    loopi(HWRT_MAX_ANIM_BLAS) destroyanim(s.anim[i]);
    s.ninst = 0;
}

static void destroyskins()
{
    hwrtdestroytexarray(skins.tex);
    hwrtdestroysampler(skins.sampler);
    skins.ids.setsize(0);
    skins.uploaded = 0;
    skins.failed = false;
    skins.overflowlogged = false;
    hwrtskinlayers = hwrtskinw = hwrtskinh = 0;
    hwrtdestroytexarray(mdlenv);
    hwrtdestroysampler(mdlenvsampler);
    mdlenvuploaded = 0;
}

void hwrtdirtyskins()
{
    skins.uploaded = 0;
}

static long long skinbudgetbytes()
{
    int mb = hwrtskinbudget;
    if(mb < 64) mb = 64;
    return (long long)mb * 1024 * 1024;
}

// The atlas only ever has to shrink for two reasons: the layer cap is about to
// start dropping meshes, or the next upload would fall back to 512. Pruning
// merely because a model left the view is not free -- compactskins() rewrites
// layer indices, so it zeroes skins.uploaded, and syncskins() then reads every
// remaining layer back out of GL and up to Vulkan around two vkDeviceWaitIdle.
// Worse, a model that walks back in re-adds its skin on the very next frame, so
// the pair ping-pongs for the rest of the map: triforts held 65 layers and paid
// that whole round trip several times per frame, which is where its 30 fps came
// from. Both frame-path prune sites now ask this first. The on-demand prune in
// buildmodelblas(), which only runs when no cache slot is free at all, is a
// different thing and is left alone.
static bool skinsunderpressure()
{
    int nskin = skins.ids.length();
    if(nskin >= HWRT_MAX_SKINS - 8) return true;
    return (long long)(nskin + 8) * HWRT_MAX_SKINDIM * HWRT_MAX_SKINDIM * 4 > skinbudgetbytes();
}

static void destroylocked()
{
    loopi(dyn.ncache) destroymodel(dyn.cache[i]);
    dyn.ncache = 0;
    loopi(HWRT_FRAMES_IN_FLIGHT) destroyslot(dyn.slot[i]);
    destroyskins();
    dyn.ready = false;
    pinnedmodels.setsize(0);
    hwrtmapmodelinsts = hwrtdynentinsts = hwrtragdollinsts = hwrtinstancecount = hwrtgeomcount = 0;
    hwrtdroppedinsts = 0;
    hwrtteleporthole = vec(0, 0, 7.3f);
    hwrthasteleporthole = false;
    bulletc = NULL;
}

static void addneededmodel(vector<model *> &need, model *m)
{
    if(!m) return;
    loopv(need) if(need[i] == m) return;
    need.add(m);
}

static void collectneededmodels(vector<model *> &need)
{
    need.setsize(0);
    const vector<extentity *> &ents = entities::getents();
    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type == ET_MAPMODEL)
        {
            if(e.flags&EF_NOVIS) continue;
            addneededmodel(need, loadmapmodel(e.attr2));
            continue;
        }
        const char *name = entities::entmodel(e);
        if(!name || !name[0]) continue;
        addneededmodel(need, loadmodel(name));
    }
    loopi(game::numdynents())
    {
        dynent *d = game::iterdynents(i);
        if(!d || d->state == CS_SPECTATOR) continue;
        const char *name = hwrtdynentmdlname(d);
        if(name && name[0]) addneededmodel(need, loadmodel(name));
        modelattach att[8];
        int natt = hwrtdynentattach(d, att, 7);
        loopj(natt) if(att[j].name && att[j].name[0]) addneededmodel(need, loadmodel(att[j].name));
    }
    loopi(hwrtnumragdolls())
    {
        dynent *d = hwrtragdoll(i);
        if(!d) continue;
        const char *name = hwrtdynentmdlname(d);
        if(name && name[0]) addneededmodel(need, loadmodel(name));
    }
    loopi(nscenemodels)
    {
        if(isbulletname(scenemodels[i].name)) continue;
        addneededmodel(need, loadmodel(scenemodels[i].name));
    }
}

static bool modelisneeded(model *m, const vector<model *> &need)
{
    loopv(need) if(need[i] == m) return true;
    return false;
}

static void pinmodel(model *m)
{
    if(!m) return;
    loopv(pinnedmodels) if(pinnedmodels[i] == m) return;
    pinnedmodels.add(m);
}

static void collectkeepmodels(vector<model *> &need, bool keeppin)
{
    collectneededmodels(need);
    if(keeppin) loopv(pinnedmodels) addneededmodel(need, pinnedmodels[i]);
}

static bool playersready()
{
    loopi(game::numdynents())
    {
        dynent *d = game::iterdynents(i);
        if(d && (d->type == ENT_PLAYER || d->type == ENT_AI) && d->state != CS_SPECTATOR) return true;
    }
    return false;
}

static int prunecache(bool keeppin = true)
{
    if(!allowskinprune) return 0;
    if(!playersready()) return 0;
    vector<model *> need;
    collectkeepmodels(need, keeppin);
    int n = 0;
    loopi(dyn.ncache)
    {
        hwrtmodelblas &c = dyn.cache[i];
        if(!c.m || !c.addr) continue;
        if(isbulletslot(c)) continue;
        if(modelisneeded(c.m, need)) continue;
        destroymodel(c);
        n++;
    }
    if(n) dyn.overflowlogged = false;
    return n;
}

static bool cachehasunused(bool keeppin = true)
{
    if(!allowskinprune || !playersready()) return false;
    vector<model *> need;
    collectkeepmodels(need, keeppin);
    loopi(dyn.ncache)
    {
        hwrtmodelblas &c = dyn.cache[i];
        if(!c.m || !c.addr) continue;
        if(isbulletslot(c)) continue;
        if(!modelisneeded(c.m, need)) return true;
    }
    return false;
}

enum { HWRT_TRI_NONE = 0xFFu };
static uint packmodeltri(int skin, int mask, float glow, float spec)
{
    uint s = (skin < 0 || skin > 255) ? HWRT_TRI_NONE : uint(skin);
    uint m = (mask < 0 || mask > 255) ? HWRT_TRI_NONE : uint(mask);
    uint g = uint(clamp(int(glow * 32.0f + 0.5f), 0, 255));
    // modelshader maskscale.x = 0.5*mdlspec. Default spec 1 → 128.
    uint sp = uint(clamp(int(0.5f * spec * 255.0f + 0.5f), 0, 255));
    return s | (m << 8) | (g << 16) | (sp << 24);
}

static void remappackedtri(uint &packed, const int *remap, int nold)
{
    if(packed == 0xFFFFFFFFu) return;
    uint skin = packed & 0xFFu;
    uint mask = (packed >> 8) & 0xFFu;
    float glow = float((packed >> 16) & 0xFFu) / 32.0f;
    float spec = float((packed >> 24) & 0xFFu) / 255.0f * 2.0f;
    int news = (skin == HWRT_TRI_NONE || int(skin) >= nold) ? -1 : remap[skin];
    int newm = (mask == HWRT_TRI_NONE || int(mask) >= nold) ? -1 : remap[mask];
    packed = packmodeltri(news, newm, glow, spec);
}

static void markpackedused(uint packed, bool *used, int nold)
{
    if(packed == 0xFFFFFFFFu) return;
    uint skin = packed & 0xFFu;
    uint mask = (packed >> 8) & 0xFFu;
    if(int(skin) < nold) used[skin] = true;
    if(int(mask) < nold) used[mask] = true;
}

static void remaptribuf(hwrtbuf &b, uint ntris, const int *remap, int nold)
{
    if(!b.memory || !ntris || !hwrtdev.device) return;
    uint *t = NULL;
    if(vkMapMemory(hwrtdev.device, b.memory, 0, VkDeviceSize(ntris)*sizeof(uint), 0, (void **)&t) != VK_SUCCESS || !t)
        return;
    loopi(int(ntris)) remappackedtri(t[i], remap, nold);
    vkUnmapMemory(hwrtdev.device, b.memory);
}

static bool compactskins()
{
    int nold = skins.ids.length();
    if(nold < 1) return false;
    bool used[HWRT_MAX_SKINS];
    loopi(HWRT_MAX_SKINS) used[i] = false;
    loopi(dyn.ncache)
    {
        hwrtmodelblas &c = dyn.cache[i];
        if(!c.addr || !c.ntris || !c.tribuf.memory) continue;
        uint *t = NULL;
        if(vkMapMemory(hwrtdev.device, c.tribuf.memory, 0, VkDeviceSize(c.ntris)*sizeof(uint), 0, (void **)&t) == VK_SUCCESS && t)
        {
            loopj(int(c.ntris)) markpackedused(t[j], used, nold);
            vkUnmapMemory(hwrtdev.device, c.tribuf.memory);
        }
    }
    loopi(HWRT_FRAMES_IN_FLIGHT) loopj(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = dyn.slot[i].anim[j];
        if(!a.tmap || !a.ntris) continue;
        loopk(int(a.ntris)) markpackedused(a.tmap[k], used, nold);
    }
    int remap[HWRT_MAX_SKINS];
    vector<GLuint> kept;
    loopi(nold)
    {
        if(!used[i]) { remap[i] = -1; continue; }
        remap[i] = kept.length();
        kept.add(skins.ids[i]);
    }
    if(kept.length() == nold) return false;
    loopi(dyn.ncache) remaptribuf(dyn.cache[i].tribuf, dyn.cache[i].ntris, remap, nold);
    loopi(HWRT_FRAMES_IN_FLIGHT) loopj(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = dyn.slot[i].anim[j];
        if(!a.tmap || !a.ntris) continue;
        loopk(int(a.ntris)) remappackedtri(a.tmap[k], remap, nold);
    }
    int ndrop = nold - kept.length();
    skins.ids.setsize(0);
    loopv(kept) skins.ids.add(kept[i]);
    skins.uploaded = 0;
    if(skins.ids.length() < HWRT_MAX_SKINS) skins.overflowlogged = false;
    conoutf("hwrt: freed %d unused model skins, %d left", ndrop, skins.ids.length());
    return true;
}

static int findoraddskin(uint texid)
{
    if(!texid) return -1;
    loopv(skins.ids) if(skins.ids[i] == GLuint(texid)) return i;
    if(skins.ids.length() >= HWRT_MAX_SKINS)
    {
        if(!skins.overflowlogged)
        {
            conoutf(CON_WARN, "hwrt: more than %d model skin layers, extra meshes stay lit by GL", int(HWRT_MAX_SKINS));
            skins.overflowlogged = true;
        }
        return -1;
    }
    skins.ids.add(GLuint(texid));
    return skins.ids.length()-1;
}

// Rebuilds the whole array whenever a new skin turns up. That is a GL readback
// plus a device wait, so it only ever runs where the rest-pose BLAS builds
// already do: model first use, never inside the traced frame.
static void syncskins()
{
    if(skins.failed || skins.ids.empty()) return;
    if(skins.uploaded == skins.ids.length() && skins.tex.view) return;
    if(!hwrtdev.ok() || !ensurecmd()) return;
    if(!skins.sampler && !hwrtcreatesampler(true, skins.sampler, true))
    {
        skins.failed = true;
        conoutf(CON_WARN, "hwrt: no sampler for model skins, models stay lit by GL");
        return;
    }
    vkDeviceWaitIdle(hwrtdev.device);
    if(allowskinprune && skinsunderpressure())
    {
        prunecache(true);
        compactskins();
        int nskin = skins.ids.length();
        if(nskin > 0 && (long long)nskin * HWRT_MAX_SKINDIM * HWRT_MAX_SKINDIM * 4 > skinbudgetbytes())
        {
            prunecache(false);
            compactskins();
        }
    }
    if(skins.ids.empty()) return;
    int dim = HWRT_MAX_SKINDIM;
    int nskin = skins.ids.length();
    if(nskin > 0 && (long long)nskin * dim * dim * 4 > skinbudgetbytes())
    {
        dim = 512;
        conoutf(CON_WARN, "hwrt: %d model skins exceed %d MB at %d, using 512",
                nskin, int(hwrtskinbudget), int(HWRT_MAX_SKINDIM));
    }
    if(!hwrtuploadtexarray(dyn.cmd, skins.ids, dim, skins.tex, "model skins", true))
    {
        skins.failed = true;
        skins.uploaded = 0;
        hwrtskinlayers = hwrtskinw = hwrtskinh = 0;
        return;
    }
    skins.uploaded = skins.ids.length();
    hwrtskinlayers = skins.tex.layers;
    hwrtskinw = skins.tex.w;
    hwrtskinh = skins.tex.h;
    conoutf("hwrt: %d model skin layers %dx%d", skins.tex.layers, skins.tex.w, skins.tex.h);
}

static void syncmdlenv()
{
    if(!hwrtdev.ok() || !ensurecmd()) return;
    GLuint want = mdlenvgl;
    if(!want && camera1) want = lookupenvmap(closestenvmap(camera1->o));
    if(mdlenv.view && mdlenvuploaded == want) return;
    if(!mdlenvsampler && !hwrtcreatesampler(false, mdlenvsampler, true))
    {
        conoutf(CON_WARN, "hwrt: no sampler for model envmap");
        return;
    }
    vkDeviceWaitIdle(hwrtdev.device);
    if(!hwrtuploadcubemap(dyn.cmd, want, mdlenv, "model envmap"))
    {
        conoutf(CON_WARN, "hwrt: model envmap copy failed");
        return;
    }
    mdlenvuploaded = want;
    if(want) conoutf("hwrt: model envmap cubemap %u", uint(want));
}

void hwrtdestroydynents()
{
    destroylocked();
    if(bullettex)
    {
        glDeleteTextures(1, &bullettex);
        bullettex = 0;
    }
    if(hwrtdev.device && dyn.cmdpool) vkDestroyCommandPool(hwrtdev.device, dyn.cmdpool, NULL);
    dyn.cmdpool = VK_NULL_HANDLE;
    dyn.cmd = VK_NULL_HANDLE;
    dyn.failed = false;
    dyn.maplogged = false;
    dyn.norqlogged = false;
    dyn.overflowlogged = false;
    dyn.animlogged = false;
    dyn.attrlogged = false;
    hwrtanimcount = hwrtanimbuilds = hwrtanimrefits = 0;
    destroybuf(dummygeom);
}

// The BIH carries positions, UVs and the skin texture, but no normals, so the
// attribute normal is left at zero and the shader falls back to the triangle's
// geometric normal. Mapmodels are hard-surface, so flat is the right answer
// there anyway; animated models bring real skinned normals of their own.
// Opaque meshes are packed first so BLAS geometry 0 can stay
// VK_GEOMETRY_OPAQUE_BIT_KHR; MESH_ALPHA (leaves, flags) follows as geometry 1
// without that bit. pad1 carries mdlalphatest (default 0.9) on the alpha verts.
static void addmodelmesh(model *m, const BIH::mesh &mesh, vector<vec> &positions, vector<uint> &indices,
                         vector<hwrtmodelvert> &attrs, vector<uint> &tris, bool alpha)
{
    if(!mesh.pos || !mesh.tris || mesh.numtris < 1) return;
    int maxv = -1;
    loopj(mesh.numtris) loopk(3) if(int(mesh.tris[j].vert[k]) > maxv) maxv = int(mesh.tris[j].vert[k]);
    if(maxv < 0) return;
    int layer = findoraddskin(mesh.tex ? uint(mesh.tex->id) : 0);
    Texture *masks = NULL;
    float glow = 3.0f;
    float spec = 1.0f;
    float envmin = 0, envmax = 0;
    GLuint envid = 0;
    float alphatest = 0.9f;
    hwrtlookupskin(m, mesh.tex, masks, glow, spec, envmin, envmax, envid, alphatest);
    if(envid) hwrtnoteenvmap(envid);
    int masklayer = findoraddskin(masks ? uint(masks->id) : 0);
    if(!alpha) alphatest = 0;
    else if(alphatest <= 0) alphatest = 0.9f;
    uint base = uint(positions.length());
    loopj(maxv+1)
    {
        vec p = mesh.xform.transform(mesh.getpos(j));
        positions.add(p);
        hwrtmodelvert &a = attrs.add();
        memset(&a, 0, sizeof(a));
        if(mesh.tc)
        {
            vec2 tc = mesh.gettc(j);
            a.tc[0] = tc.x;
            a.tc[1] = tc.y;
        }
        a.pad0[0] = envmin;
        a.pad0[1] = envmax;
        a.pad1 = alphatest;
        a.pos[0] = p.x;
        a.pos[1] = p.y;
        a.pos[2] = p.z;
    }
    loopj(mesh.numtris)
    {
        indices.add(base + mesh.tris[j].vert[0]);
        indices.add(base + mesh.tris[j].vert[1]);
        indices.add(base + mesh.tris[j].vert[2]);
        tris.add(packmodeltri(layer, masklayer, glow, spec));
    }
}

static bool gathermodelgeom(model *m, vector<vec> &positions, vector<uint> &indices,
                            vector<hwrtmodelvert> &attrs, vector<uint> &tris, uint &nopaque)
{
    nopaque = 0;
    if(!m) return false;
    m->preloadBIH();
    if(!m->bih || m->bih->nummeshes < 1) return false;
    loopi(m->bih->nummeshes)
    {
        const BIH::mesh &mesh = m->bih->meshes[i];
        if(mesh.flags & BIH::MESH_ALPHA) continue;
        addmodelmesh(m, mesh, positions, indices, attrs, tris, false);
    }
    nopaque = uint(indices.length()/3);
    loopi(m->bih->nummeshes)
    {
        const BIH::mesh &mesh = m->bih->meshes[i];
        if(!(mesh.flags & BIH::MESH_ALPHA)) continue;
        addmodelmesh(m, mesh, positions, indices, attrs, tris, true);
    }
    if(isdefaultteleporter(m))
    {
        punchteleporterdisk(positions, indices, tris);
        nopaque = uint(indices.length()/3);
    }
    uint ntris = uint(indices.length()/3);
    if(ntris > nopaque)
    {
        static bool logged = false;
        if(!logged)
        {
            conoutf("hwrt: MESH_ALPHA in BLAS (%s: %u opaque, %u alpha)",
                    m->name ? m->name : "?", nopaque, ntris - nopaque);
            logged = true;
        }
    }
    return indices.length() >= 3;
}

// Rest-pose BLAS: geometry 0 is opaque (trunk, crates), geometry 1 is
// MESH_ALPHA without VK_GEOMETRY_OPAQUE_BIT_KHR (leaves, flags). Same vertex
// and index buffers; the alpha range starts at nopaque. Animated BLASes pass
// opaqueonly so a refit keeps a single opaque geometry.
struct hwrtblastris
{
    VkAccelerationStructureGeometryKHR geo[2];
    VkAccelerationStructureBuildRangeInfoKHR range[2];
    uint32_t counts[2];
    uint32_t ngeo;
};

static void fillblastris(hwrtblastris &g, VkDeviceAddress vaddr, VkDeviceAddress iaddr, uint nverts, uint nopaque, uint ntris)
{
    memset(&g, 0, sizeof(g));
    if(nverts < 1 || ntris < 1) return;
    if(nopaque > ntris) nopaque = ntris;
    uint nalpha = ntris - nopaque;
    loopk(2)
    {
        uint32_t nprim = k ? nalpha : nopaque;
        if(!nprim) continue;
        uint32_t slot = g.ngeo;
        VkAccelerationStructureGeometryKHR &geo = g.geo[slot];
        geo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = k ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
        geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geo.geometry.triangles.vertexData.deviceAddress = vaddr;
        geo.geometry.triangles.vertexStride = 16;
        geo.geometry.triangles.maxVertex = nverts - 1;
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geo.geometry.triangles.indexData.deviceAddress = iaddr + (k ? VkDeviceAddress(nopaque)*3*sizeof(uint) : 0);
        g.counts[slot] = nprim;
        g.range[slot].primitiveCount = nprim;
        g.ngeo++;
    }
}

static void noteattrfail()
{
    if(dyn.attrlogged) return;
    conoutf(CON_WARN, "hwrt: could not allocate model attributes, those models stay lit by GL");
    dyn.attrlogged = true;
}

// Attributes ride alongside the BLAS in their own buffers and are reached from
// the shader by device address, so no arena has to be re-laid-out when a pose
// changes size. Failure is not fatal: the geometry entry simply loses its
// shade flag and the shader leaves that model to GL.
static bool createattrbufs(hwrtbuf &attrbuf, hwrtbuf &tribuf, uint nverts, uint ntris)
{
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkMemoryPropertyFlags props =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if(!allocbuf(attrbuf, VkDeviceSize(nverts)*sizeof(hwrtmodelvert), usage, props) ||
       !allocbuf(tribuf, VkDeviceSize(ntris)*sizeof(uint), usage, props))
    {
        destroybuf(attrbuf);
        destroybuf(tribuf);
        noteattrfail();
        return false;
    }
    return true;
}

static hwrtmodelblas *findcache(model *m)
{
    loopi(dyn.ncache) if(dyn.cache[i].m == m) return &dyn.cache[i];
    return NULL;
}

static void rememberempty(model *m)
{
    if(!m || dyn.ncache >= HWRT_MAX_MODEL_BLAS) return;
    if(findcache(m)) return;
    hwrtmodelblas &slot = dyn.cache[dyn.ncache];
    memset(&slot, 0, sizeof(slot));
    slot.m = m;
    dyn.ncache++;
}

static hwrtmodelblas *buildmodelblas(model *m)
{
    if(dyn.failed || !m) return NULL;
    hwrtmodelblas *hit = findcache(m);
    if(hit) return hit->addr ? hit : NULL;
    if(dyn.ncache >= HWRT_MAX_MODEL_BLAS)
    {
        bool full = true;
        loopi(dyn.ncache) if(!dyn.cache[i].m) { full = false; break; }
        if(full && !(allowskinprune && hwrtdev.ok()))
        {
            if(!dyn.overflowlogged)
            {
                conoutf(CON_WARN, "hwrt: more than %d unique model BLASes, extras dropped", int(HWRT_MAX_MODEL_BLAS));
                dyn.overflowlogged = true;
            }
            return NULL;
        }
    }
    if(!hwrtdev.ok() || !hwrtdev.rayquery || !vkCreateAccelerationStructureKHR) return NULL;
    if(!ensurecmd()) return NULL;

    static vector<vec> positions;
    static vector<uint> indices;
    static vector<hwrtmodelvert> attrs;
    static vector<uint> tris;
    positions.setsize(0);
    indices.setsize(0);
    attrs.setsize(0);
    tris.setsize(0);
    uint nopaque = 0;
    if(!gathermodelgeom(m, positions, indices, attrs, tris, nopaque))
    {
        rememberempty(m);
        return NULL;
    }

    const uint nverts = uint(positions.length());
    const uint nidx = uint(indices.length());
    const uint ntris = nidx/3;
    const VkDeviceSize vertstride = 16;
    const VkDeviceSize vertbytes = VkDeviceSize(nverts)*vertstride;
    const VkDeviceSize idxbytes = VkDeviceSize(nidx)*sizeof(uint);

    hwrtbuf vbuf, ibuf, scratch;
    memset(&vbuf, 0, sizeof(vbuf));
    memset(&ibuf, 0, sizeof(ibuf));
    memset(&scratch, 0, sizeof(scratch));

    const VkBufferUsageFlags geomusage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if(!allocbuf(vbuf, vertbytes, geomusage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
       !allocbuf(ibuf, idxbytes, geomusage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }

    float *vmap = NULL;
    VkResult r = vkMapMemory(hwrtdev.device, vbuf.memory, 0, vertbytes, 0, (void **)&vmap);
    if(r != VK_SUCCESS || !vmap)
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
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
        return NULL;
    }

    hwrtblastris geoms;
    fillblastris(geoms, vbuf.address, ibuf.address, nverts, nopaque, ntris);
    if(!geoms.ngeo)
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }

    VkAccelerationStructureBuildGeometryInfoKHR blasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    blasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasinfo.geometryCount = geoms.ngeo;
    blasinfo.pGeometries = geoms.geo;

    VkAccelerationStructureBuildSizesInfoKHR blassizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(hwrtdev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &blasinfo, geoms.counts, &blassizes);

    hwrtmodelblas *slotp = NULL;
    loopi(dyn.ncache) if(!dyn.cache[i].m) { slotp = &dyn.cache[i]; break; }
    if(!slotp && dyn.ncache < HWRT_MAX_MODEL_BLAS) slotp = &dyn.cache[dyn.ncache];
    if(!slotp && allowskinprune)
    {
        vkDeviceWaitIdle(hwrtdev.device);
        prunecache(true);
        loopi(dyn.ncache) if(!dyn.cache[i].m) { slotp = &dyn.cache[i]; break; }
        if(!slotp)
        {
            prunecache(false);
            loopi(dyn.ncache) if(!dyn.cache[i].m) { slotp = &dyn.cache[i]; break; }
        }
        if(!slotp && dyn.ncache < HWRT_MAX_MODEL_BLAS) slotp = &dyn.cache[dyn.ncache];
    }
    if(!slotp)
    {
        if(!dyn.overflowlogged)
        {
            conoutf(CON_WARN, "hwrt: more than %d unique model BLASes, extras dropped", int(HWRT_MAX_MODEL_BLAS));
            dyn.overflowlogged = true;
        }
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }
    hwrtmodelblas &slot = *slotp;
    memset(&slot, 0, sizeof(slot));
    if(!createas(slot.blas, slot.buf, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, blassizes.accelerationStructureSize, false))
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }

    VkDeviceSize scralign = hwrtdev.scratchalign ? hwrtdev.scratchalign : 256;
    VkDeviceSize scratchbytes = alignup(blassizes.buildScratchSize, scralign) + scralign;
    if(!allocbuf(scratch, scratchbytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }

    blasinfo.dstAccelerationStructure = slot.blas;
    blasinfo.scratchData.deviceAddress = alignedaddr(scratch.address, scralign);

    const VkAccelerationStructureBuildRangeInfoKHR *blasranges = geoms.range;

    r = vkResetCommandBuffer(dyn.cmd, 0);
    if(r != VK_SUCCESS)
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(dyn.cmd, &begin);
    if(r != VK_SUCCESS)
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    vkCmdBuildAccelerationStructuresKHR(dyn.cmd, 1, &blasinfo, &blasranges);
    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(dyn.cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    r = vkEndCommandBuffer(dyn.cmd);
    if(r != VK_SUCCESS)
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &dyn.cmd;
    r = vkQueueSubmit(hwrtdev.queue, 1, &submit, VK_NULL_HANDLE);
    if(r != VK_SUCCESS)
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    if(!hwrtwaitidle("vkDeviceWaitIdle (model BLAS)"))
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    destroybuf(vbuf);
    destroybuf(scratch);

    slot.m = m;
    slot.ntris = ntris;
    slot.nopaque = nopaque > ntris ? ntris : nopaque;
    slot.addr = asaddress(slot.blas);
    if(!slot.addr)
    {
        destroybuf(ibuf);
        destroymodel(slot);
        return NULL;
    }

    // The index buffer outlives the build so the shader can walk the same
    // triangles; the positions do not, because the attribute buffer carries a
    // copy for the geometric-normal fallback.
    slot.idxbuf = ibuf;
    if(createattrbufs(slot.attrbuf, slot.tribuf, nverts, ntris))
    {
        if(!uploadbuf(slot.attrbuf, attrs.getbuf(), VkDeviceSize(nverts)*sizeof(hwrtmodelvert)) ||
           !uploadbuf(slot.tribuf, tris.getbuf(), VkDeviceSize(ntris)*sizeof(uint)))
        {
            destroybuf(slot.attrbuf);
            destroybuf(slot.tribuf);
            noteattrfail();
        }
    }
    if(&slot == &dyn.cache[dyn.ncache]) dyn.ncache++;
    return &slot;
}

// Rifle is hitscan: there is no packages/ model. A unit sphere occupies one
// rest-pose slot and is scaled onto each live smoke sprite. CASTER so the
// camera / RTAO / sky miss it; sun and lamps still see it.
static int ensurebrassskin()
{
    if(!bullettex)
    {
        glGenTextures(1, &bullettex);
        uchar px[8*8*3];
        loopi(8*8) { px[i*3] = 72; px[i*3+1] = 72; px[i*3+2] = 72; }
        createtexture(int(bullettex), 8, 8, px, 3, 1, GL_RGB);
    }
    return findoraddskin(bullettex);
}

static void gensphere(vector<vec> &positions, vector<uint> &indices,
                      vector<hwrtmodelvert> &attrs, vector<uint> &tris,
                      int skin, float r, int slices, int stacks)
{
    uint packed = packmodeltri(skin, -1, 0, 1.0f);
    for(int i = 0; i <= stacks; i++)
    {
        float phi = PI * (i / float(stacks));
        float z = r * cosf(phi);
        float rr = r * sinf(phi);
        loopj(slices)
        {
            float theta = PI2 * (j / float(slices));
            vec p(rr * cosf(theta), rr * sinf(theta), z);
            vec n = p;
            if(!n.iszero()) n.normalize();
            else n = vec(0, 0, z > 0 ? 1.0f : -1.0f);
            positions.add(p);
            hwrtmodelvert &a = attrs.add();
            memset(&a, 0, sizeof(a));
            a.tc[0] = j / float(slices);
            a.tc[1] = i / float(stacks);
            a.n[0] = n.x; a.n[1] = n.y; a.n[2] = n.z;
            a.pos[0] = p.x; a.pos[1] = p.y; a.pos[2] = p.z;
        }
    }
    for(int i = 0; i < stacks; i++)
    {
        loopj(slices)
        {
            uint i0 = uint(i*slices + j);
            uint i1 = uint(i*slices + ((j+1)%slices));
            uint i2 = uint((i+1)*slices + j);
            uint i3 = uint((i+1)*slices + ((j+1)%slices));
            indices.add(i0); indices.add(i2); indices.add(i1); tris.add(packed);
            indices.add(i1); indices.add(i2); indices.add(i3); tris.add(packed);
        }
    }
}

static hwrtmodelblas *buildbulletblas()
{
    if(bulletc && bulletc->addr) return bulletc;
    if(dyn.failed) return NULL;
    if(!hwrtdev.ok() || !hwrtdev.rayquery || !vkCreateAccelerationStructureKHR) return NULL;
    if(!ensurecmd()) return NULL;

    int skin = ensurebrassskin();
    static vector<vec> positions;
    static vector<uint> indices;
    static vector<hwrtmodelvert> attrs;
    static vector<uint> tris;
    positions.setsize(0);
    indices.setsize(0);
    attrs.setsize(0);
    tris.setsize(0);
    gensphere(positions, indices, attrs, tris, skin, 1.0f, 8, 6);
    uint nopaque = uint(indices.length()/3);
    const uint nverts = uint(positions.length());
    const uint nidx = uint(indices.length());
    const uint ntris = nidx/3;
    if(ntris < 1) return NULL;

    const VkDeviceSize vertstride = 16;
    const VkDeviceSize vertbytes = VkDeviceSize(nverts)*vertstride;
    const VkDeviceSize idxbytes = VkDeviceSize(nidx)*sizeof(uint);

    hwrtbuf vbuf, ibuf, scratch;
    memset(&vbuf, 0, sizeof(vbuf));
    memset(&ibuf, 0, sizeof(ibuf));
    memset(&scratch, 0, sizeof(scratch));

    const VkBufferUsageFlags geomusage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if(!allocbuf(vbuf, vertbytes, geomusage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
       !allocbuf(ibuf, idxbytes, geomusage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }

    float *vmap = NULL;
    VkResult r = vkMapMemory(hwrtdev.device, vbuf.memory, 0, vertbytes, 0, (void **)&vmap);
    if(r != VK_SUCCESS || !vmap)
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
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
        return NULL;
    }

    hwrtblastris geoms;
    fillblastris(geoms, vbuf.address, ibuf.address, nverts, nopaque, ntris);
    if(!geoms.ngeo)
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }

    VkAccelerationStructureBuildGeometryInfoKHR blasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    blasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasinfo.geometryCount = geoms.ngeo;
    blasinfo.pGeometries = geoms.geo;

    VkAccelerationStructureBuildSizesInfoKHR blassizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(hwrtdev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &blasinfo, geoms.counts, &blassizes);

    hwrtmodelblas *slotp = NULL;
    loopi(dyn.ncache) if(!dyn.cache[i].m) { slotp = &dyn.cache[i]; break; }
    if(!slotp && dyn.ncache < HWRT_MAX_MODEL_BLAS) slotp = &dyn.cache[dyn.ncache];
    if(!slotp)
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }
    hwrtmodelblas &slot = *slotp;
    memset(&slot, 0, sizeof(slot));
    if(!createas(slot.blas, slot.buf, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, blassizes.accelerationStructureSize, false))
    {
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }

    VkDeviceSize scralign = hwrtdev.scratchalign ? hwrtdev.scratchalign : 256;
    VkDeviceSize scratchbytes = alignup(blassizes.buildScratchSize, scralign) + scralign;
    if(!allocbuf(scratch, scratchbytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf);
        return NULL;
    }

    blasinfo.dstAccelerationStructure = slot.blas;
    blasinfo.scratchData.deviceAddress = alignedaddr(scratch.address, scralign);
    const VkAccelerationStructureBuildRangeInfoKHR *blasranges = geoms.range;

    r = vkResetCommandBuffer(dyn.cmd, 0);
    if(r != VK_SUCCESS)
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(dyn.cmd, &begin);
    if(r != VK_SUCCESS)
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    vkCmdBuildAccelerationStructuresKHR(dyn.cmd, 1, &blasinfo, &blasranges);
    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(dyn.cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    r = vkEndCommandBuffer(dyn.cmd);
    if(r != VK_SUCCESS)
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &dyn.cmd;
    r = vkQueueSubmit(hwrtdev.queue, 1, &submit, VK_NULL_HANDLE);
    if(r != VK_SUCCESS)
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    if(!hwrtwaitidle("vkDeviceWaitIdle (bullet BLAS)"))
    {
        destroymodel(slot);
        destroybuf(vbuf); destroybuf(ibuf); destroybuf(scratch);
        return NULL;
    }
    destroybuf(vbuf);
    destroybuf(scratch);

    slot.m = hwrtbulletmdl;
    slot.ntris = ntris;
    slot.nopaque = ntris;
    slot.addr = asaddress(slot.blas);
    if(!slot.addr)
    {
        destroybuf(ibuf);
        destroymodel(slot);
        return NULL;
    }
    slot.idxbuf = ibuf;
    if(createattrbufs(slot.attrbuf, slot.tribuf, nverts, ntris))
    {
        if(!uploadbuf(slot.attrbuf, attrs.getbuf(), VkDeviceSize(nverts)*sizeof(hwrtmodelvert)) ||
           !uploadbuf(slot.tribuf, tris.getbuf(), VkDeviceSize(ntris)*sizeof(uint)))
        {
            destroybuf(slot.attrbuf);
            destroybuf(slot.tribuf);
            noteattrfail();
        }
    }
    if(&slot == &dyn.cache[dyn.ncache]) dyn.ncache++;
    bulletc = &slot;
    conoutf("hwrt: rifle smoke puff BLAS (%u tris)", ntris);
    return bulletc;
}

static bool ensuretlasbuffers()
{
    if(dyn.slot[0].tlas) return true;
    if(!hwrtdev.ok() || !hwrtdev.rayquery) return dynfail("no ray query for dynent TLAS");

    const VkBufferUsageFlags instusage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkDeviceSize instbytes = VkDeviceSize(HWRT_MAX_INSTANCES)*sizeof(hwrtinstance);

    loopi(HWRT_FRAMES_IN_FLIGHT)
    {
        hwrtdynslot &s = dyn.slot[i];
        if(!createbuf(s.instbuf, instbytes, instusage,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            destroylocked();
            return false;
        }
        VkResult r = vkMapMemory(hwrtdev.device, s.instbuf.memory, 0, instbytes, 0, (void **)&s.instmap);
        if(r != VK_SUCCESS || !s.instmap)
        {
            destroylocked();
            return dynfail("vkMapMemory (dynent instances)", r);
        }
        memset(s.instmap, 0, size_t(instbytes));

        // The geometry table is per frame in flight for the same reason the
        // instances are: the previous frame may still be reading last frame's
        // addresses.
        const VkDeviceSize geombytes = VkDeviceSize(HWRT_MAX_GEOMS)*sizeof(hwrtmodelgeom);
        if(!createbuf(s.geombuf, geombytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            destroylocked();
            return false;
        }
        r = vkMapMemory(hwrtdev.device, s.geombuf.memory, 0, geombytes, 0, (void **)&s.geommap);
        if(r != VK_SUCCESS || !s.geommap)
        {
            destroylocked();
            return dynfail("vkMapMemory (model geometry table)", r);
        }
        memset(s.geommap, 0, size_t(geombytes));
    }

    VkAccelerationStructureGeometryKHR tgeo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.arrayOfPointers = VK_FALSE;
    tgeo.geometry.instances.data.deviceAddress = dyn.slot[0].instbuf.address;

    VkAccelerationStructureBuildGeometryInfoKHR tlasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tlasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tlasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasinfo.geometryCount = 1;
    tlasinfo.pGeometries = &tgeo;

    uint32_t instcount = HWRT_MAX_INSTANCES;
    VkAccelerationStructureBuildSizesInfoKHR tlassizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(hwrtdev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &tlasinfo, &instcount, &tlassizes);

    VkDeviceSize scralign = hwrtdev.scratchalign ? hwrtdev.scratchalign : 256;
    VkDeviceSize scratchbytes = alignup(tlassizes.buildScratchSize, scralign) + scralign;

    loopi(HWRT_FRAMES_IN_FLIGHT)
    {
        hwrtdynslot &s = dyn.slot[i];
        if(!createas(s.tlas, s.tlasbuf, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, tlassizes.accelerationStructureSize) ||
           !createbuf(s.scratch, scratchbytes,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            destroylocked();
            return false;
        }
    }
    return true;
}

// Ray mask bits. SHADE_MODELS primaries ask for WORLD|MODEL so they can
// light a player without hitting the first-person body (SELF). MASK_MODELS
// primaries ask for WORLD only. Shadow and sky rays from the world ask for
// ALL, so that body still casts. Opaque BLAS triangles still commit in
// hardware; MESH_ALPHA is a separate non-opaque geometry and is accepted
// or refused in the shader from skins.a.
static void writeinstance(hwrtinstance &inst, const matrix4x3 &xf, uint32_t custom, VkDeviceAddress blas, uint32_t raymask, uint32_t extraflags = 0)
{
    memset(&inst, 0, sizeof(inst));
    inst.transform[0] = xf.a.x; inst.transform[1] = xf.b.x; inst.transform[2] = xf.c.x; inst.transform[3] = xf.d.x;
    inst.transform[4] = xf.a.y; inst.transform[5] = xf.b.y; inst.transform[6] = xf.c.y; inst.transform[7] = xf.d.y;
    inst.transform[8] = xf.a.z; inst.transform[9] = xf.b.z; inst.transform[10] = xf.c.z; inst.transform[11] = xf.d.z;
    inst.custommask = ((raymask & 0xFFu) << 24) | (custom & 0xFFFFFFu);
    uint32_t flags = uint32_t(VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR) | extraflags;
    inst.sbtflags = flags << 24;
    inst.reference = blas;
}

static void instanceat(hwrtinstance &inst, const vec &o, float yaw, float pitch, uint32_t custom, VkDeviceAddress blas, uint32_t raymask = HWRT_RAYMASK_MODEL, uint32_t extraflags = 0, float sx = 1, float sy = 1, float sz = 1)
{
    // Same composition as animmodel::render: T * Rz(yaw) * Ry(-pitch).
    // matrix4x3::rotate() replaces the whole matrix (translation included), so
    // pitch has to be rotate_around_y, which keeps d. Uniform or axis scale
    // after that stretches local X/Y/Z (smoke trails stretch local X = +X mesh).
    matrix4x3 xf;
    xf.identity();
    xf.settranslation(o);
    xf.rotate_around_z(yaw*RAD);
    if(pitch != 0) xf.rotate_around_y(-pitch*RAD);
    if(sx != 1 || sy != 1 || sz != 1) xf.scale(sx, sy, sz);
    writeinstance(inst, xf, custom, blas, raymask, extraflags);
}

static hwrtmodelblas *blasforname(const char *name, bool build)
{
    if(!name || !name[0]) return NULL;
    model *m = loadmodel(name);
    if(!m) return NULL;
    hwrtmodelblas *c = findcache(m);
    if(c) return c->addr ? c : NULL;
    return build ? buildmodelblas(m) : NULL;
}

static hwrtmodelblas *blasformapmodel(int idx, bool build)
{
    model *m = loadmapmodel(idx);
    if(!m) return NULL;
    hwrtmodelblas *c = findcache(m);
    if(c) return c->addr ? c : NULL;
    return build ? buildmodelblas(m) : NULL;
}

static void noteanimfull()
{
    if(dyn.animlogged) return;
    conoutf(CON_WARN, "hwrt: more than %d animated BLASes this frame, extras use the rest pose", int(HWRT_MAX_ANIM_BLAS));
    dyn.animlogged = true;
}

static bool poseeq(const hwrtposekey &a, const hwrtposekey &b)
{
    return a.m == b.m && a.hash == b.hash && a.ragdoll == b.ragdoll && a.worldspace == b.worldspace;
}

static bool ensureblasscratch(hwrtdynslot &s, VkDeviceSize need)
{
    VkDeviceSize scralign = hwrtdev.scratchalign ? hwrtdev.scratchalign : 256;
    need = alignup(need, scralign) + scralign;
    if(s.blasscratch.buffer && s.blasscratch.size >= need) return true;
    destroybuf(s.blasscratch);
    return allocbuf(s.blasscratch, need,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}

static bool createanimblas(hwrtanimslot &a, uint nverts, uint ntris)
{
    const VkDeviceSize vertstride = 16;
    const VkDeviceSize vertbytes = VkDeviceSize(nverts)*vertstride;
    const VkDeviceSize idxbytes = VkDeviceSize(ntris*3)*sizeof(uint);
    const VkBufferUsageFlags geomusage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR|
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if(!allocbuf(a.vbuf, vertbytes, geomusage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
       !allocbuf(a.ibuf, idxbytes, geomusage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        destroyanim(a);
        return false;
    }
    VkResult r = vkMapMemory(hwrtdev.device, a.vbuf.memory, 0, vertbytes, 0, (void **)&a.vmap);
    if(r != VK_SUCCESS || !a.vmap)
    {
        destroyanim(a);
        return false;
    }

    // Attributes are re-skinned every frame along with the positions, so they
    // stay mapped like the vertex buffer does.
    if(createattrbufs(a.attrbuf, a.tribuf, nverts, ntris))
    {
        if(vkMapMemory(hwrtdev.device, a.attrbuf.memory, 0, VkDeviceSize(nverts)*sizeof(hwrtmodelvert), 0, (void **)&a.amap) != VK_SUCCESS ||
           vkMapMemory(hwrtdev.device, a.tribuf.memory, 0, VkDeviceSize(ntris)*sizeof(uint), 0, (void **)&a.tmap) != VK_SUCCESS)
        {
            a.amap = NULL;
            a.tmap = NULL;
            destroybuf(a.attrbuf);
            destroybuf(a.tribuf);
            noteattrfail();
        }
    }

    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo.geometry.triangles.vertexData.deviceAddress = a.vbuf.address;
    geo.geometry.triangles.vertexStride = vertstride;
    geo.geometry.triangles.maxVertex = nverts - 1;
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geo.geometry.triangles.indexData.deviceAddress = a.ibuf.address;

    VkAccelerationStructureBuildGeometryInfoKHR blasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    blasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR|
                     VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    blasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasinfo.geometryCount = 1;
    blasinfo.pGeometries = &geo;

    uint32_t primcount = ntris;
    VkAccelerationStructureBuildSizesInfoKHR blassizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(hwrtdev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &blasinfo, &primcount, &blassizes);
    if(!createas(a.blas, a.asbuf, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, blassizes.accelerationStructureSize, false))
    {
        destroyanim(a);
        return false;
    }
    a.nverts = nverts;
    a.ntris = ntris;
    a.addr = asaddress(a.blas);
    if(!a.addr)
    {
        destroyanim(a);
        return false;
    }
    a.updates = 0;
    return true;
}

static void writeanimverts(hwrtanimslot &a, const hwrtskingeom &geom, bool first)
{
    if(!a.vmap) return;
    loopi(int(a.nverts))
    {
        a.vmap[i*4 + 0] = geom.positions[i].x;
        a.vmap[i*4 + 1] = geom.positions[i].y;
        a.vmap[i*4 + 2] = geom.positions[i].z;
        a.vmap[i*4 + 3] = 0;
    }
    if(first && a.ibuf.memory) uploadbuf(a.ibuf, geom.indices.getbuf(), VkDeviceSize(a.ntris*3)*sizeof(uint));

    if(a.amap)
    {
        bool hasnorm = geom.norms.length() >= int(a.nverts), hastc = geom.tcs.length() >= int(a.nverts);
        loopi(int(a.nverts))
        {
            hwrtmodelvert &v = a.amap[i];
            const vec &p = geom.positions[i];
            v.tc[0] = hastc ? geom.tcs[i].x : 0;
            v.tc[1] = hastc ? geom.tcs[i].y : 0;
            vec n = hasnorm ? geom.norms[i] : vec(0, 0, 0);
            v.n[0] = n.x; v.n[1] = n.y; v.n[2] = n.z;
            v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
            v.pad0[0] = 0;
            v.pad0[1] = 0;
            v.pad1 = 0;
            v.pad2 = 0;
            if(geom.envscales.length() > i)
            {
                v.pad0[0] = geom.envscales[i].x;
                v.pad0[1] = geom.envscales[i].y;
            }
        }
    }
    // Skin layers only depend on the mesh list, so they are written on the
    // first fill and left alone while the pose changes underneath them.
    if(first && a.tmap)
    {
        bool hasskin = geom.skins.length() >= int(a.ntris);
        bool hasmask = geom.masks.length() >= int(a.ntris);
        bool hasglow = geom.glows.length() >= int(a.ntris);
        bool hasspec = geom.specs.length() >= int(a.ntris);
        loopi(int(a.ntris))
        {
            int layer = hasskin ? findoraddskin(geom.skins[i]) : -1;
            int mask = hasmask ? findoraddskin(geom.masks[i]) : -1;
            float glow = hasglow ? geom.glows[i] : 3.0f;
            float spec = hasspec ? geom.specs[i] : 1.0f;
            a.tmap[i] = packmodeltri(layer, mask, glow, spec);
        }
    }
}

static hwrtanimslot *claimanim(hwrtdynslot &s, model *m, const hwrtposekey &key)
{
    loopi(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = s.anim[i];
        if(a.addr && poseeq(a.key, key))
        {
            a.used = true;
            a.millis = lastmillis;
            a.key = key;
            return &a;
        }
    }
    hwrtanimslot *best = NULL;
    loopi(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = s.anim[i];
        if(a.used) continue;
        if(a.m == m && a.addr)
        {
            if(!best || a.millis < best->millis) best = &a;
        }
    }
    if(best)
    {
        best->used = true;
        best->key = key;
        best->millis = lastmillis;
        return best;
    }
    loopi(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = s.anim[i];
        if(!a.used && !a.addr)
        {
            a.used = true;
            a.m = m;
            a.key = key;
            a.millis = lastmillis;
            return &a;
        }
    }
    loopi(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = s.anim[i];
        if(a.used) continue;
        if(!best || a.millis < best->millis) best = &a;
    }
    if(!best)
    {
        noteanimfull();
        return NULL;
    }
    destroyanim(*best);
    best->used = true;
    best->m = m;
    best->key = key;
    best->millis = lastmillis;
    return best;
}

static bool fillanim(hwrtdynslot &s, hwrtanimslot &a, const hwrtskingeom &geom)
{
    uint nverts = uint(geom.positions.length());
    uint nidx = uint(geom.indices.length());
    uint ntris = nidx/3;
    if(nverts < 1 || ntris < 1) return false;

    if(a.addr && (a.nverts != nverts || a.ntris != ntris))
    {
        model *m = a.m;
        hwrtposekey key = a.key;
        bool used = a.used;
        int millis = a.millis;
        destroyanim(a);
        a.m = m;
        a.key = key;
        a.used = used;
        a.millis = millis;
    }

    bool first = !a.addr;
    if(first && !createanimblas(a, nverts, ntris)) return false;
    writeanimverts(a, geom, first);

    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geo.geometry.triangles.vertexData.deviceAddress = a.vbuf.address;
    geo.geometry.triangles.vertexStride = 16;
    geo.geometry.triangles.maxVertex = a.nverts - 1;
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geo.geometry.triangles.indexData.deviceAddress = a.ibuf.address;

    VkAccelerationStructureBuildGeometryInfoKHR blasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    blasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR|
                     VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    blasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasinfo.geometryCount = 1;
    blasinfo.pGeometries = &geo;
    uint32_t primcount = a.ntris;
    VkAccelerationStructureBuildSizesInfoKHR blassizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(hwrtdev.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &blasinfo, &primcount, &blassizes);
    VkDeviceSize need = first ? blassizes.buildScratchSize : max(blassizes.buildScratchSize, blassizes.updateScratchSize);
    if(!ensureblasscratch(s, need))
    {
        if(first) destroyanim(a);
        return false;
    }

    if(first)
    {
        a.cmd = 2;
        a.updates = 0;
    }
    else
    {
        a.updates++;
        a.cmd = (a.updates >= HWRT_ANIM_REBUILD_EVERY) ? 2 : 1;
    }
    return a.addr != 0;
}

static bool isfirstpersonbody(dynent *d);
static bool isfreezeoccluder(dynent *d);
enum { HWRT_FREEZESELF_HASH = 0x46525A31u };
static bool freezeselfgeomok = false, freezeselfxfok = false;
static matrix4x3 freezeselfxf;
static hwrtskingeom freezeselfgeom;
static float freezeselfcamyaw = 0, freezeselfcampitch = 0;
static int freezeselfmovedlog = 0;

static void clearfreezeself()
{
    freezeselfgeomok = false;
    freezeselfxfok = false;
    freezeselfgeom.reset();
    freezeselfmovedlog = 0;
}

VARF(hwrtfreezeself, 0, 0, 1, {
    if(!hwrtfreezeself)
    {
        clearfreezeself();
        conoutf("\f2Ombre du corps : libre (elle suit tes mouvements)");
    }
    else conoutf("\f2Ombre du corps : figee — tourne ou vole (edition), la tache doit rester au meme endroit");
});

ICOMMAND(hwrtfreeze, "", (), { hwrtfreezeself = hwrtfreezeself ? 0 : 1; var_hwrtfreezeself(); });

ICOMMAND(hwrtfreezestatus, "", (), {
    defformatstring(msg, "want=%d xf=%d geom=%d verts=%d", hwrtfreezeself, freezeselfxfok ? 1 : 0, freezeselfgeomok ? 1 : 0, freezeselfgeom.positions.length());
    result(msg);
});

static void copygeom(hwrtskingeom &dst, const hwrtskingeom &src)
{
    dst.reset();
    loopv(src.positions) dst.positions.add(src.positions[i]);
    loopv(src.norms) dst.norms.add(src.norms[i]);
    loopv(src.tcs) dst.tcs.add(src.tcs[i]);
    loopv(src.envscales) dst.envscales.add(src.envscales[i]);
    loopv(src.indices) dst.indices.add(src.indices[i]);
    loopv(src.skins) dst.skins.add(src.skins[i]);
    loopv(src.masks) dst.masks.add(src.masks[i]);
    loopv(src.glows) dst.glows.add(src.glows[i]);
    loopv(src.specs) dst.specs.add(src.specs[i]);
}

static hwrtanimslot *animfordynent(hwrtdynslot &s, dynent *d, model *m, float pitch)
{
    if(!d || !m) return NULL;
    if(hwrtfreezeself && freezeselfgeomok && isfreezeoccluder(d))
    {
        hwrtposekey key;
        memset(&key, 0, sizeof(key));
        key.m = m;
        key.hash = HWRT_FREEZESELF_HASH;
        hwrtanimslot *slot = claimanim(s, m, key);
        if(!slot) return NULL;
        if(slot->addr && slot->filledhash == key.hash && slot->nverts) return slot;
        if(freezeselfgeom.positions.length() < 1 || freezeselfgeom.indices.length() < 3) return NULL;
        if(!fillanim(s, *slot, freezeselfgeom)) return NULL;
        slot->filledhash = key.hash;
        return slot->addr ? slot : NULL;
    }
    hwrtposekey key;
    if(!hwrtskinmodel(d, m, pitch, NULL, key)) return NULL;

    bool latch = hwrtfreezeself && !freezeselfgeomok && isfreezeoccluder(d);
    if(!latch) loopi(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = s.anim[i];
        if(a.used && a.addr && poseeq(a.key, key)) return &a;
    }

    hwrtanimslot *slot = claimanim(s, m, key);
    if(!slot) return NULL;
    if(!latch && slot->addr && slot->filledhash == key.hash && slot->key.ragdoll == key.ragdoll && slot->nverts)
        return slot;

    static hwrtskingeom geom;
    geom.reset();
    if(!hwrtskinmodel(d, m, pitch, &geom, key)) return NULL;
    if(geom.positions.length() < 1 || geom.indices.length() < 3) return NULL;
    if(!fillanim(s, *slot, geom)) return NULL;
    slot->filledhash = key.hash;
    if(hwrtfreezeself && !freezeselfgeomok && isfreezeoccluder(d))
    {
        copygeom(freezeselfgeom, geom);
        freezeselfgeomok = true;
        conoutf("hwrt: latched FP occluder geom verts=%d tris=%d pitch=%.1f animinterp=%d",
                geom.positions.length(), geom.indices.length()/3, pitch,
                d->animinterp[0].lastmodel ? 1 : 0);
    }
    return slot->addr ? slot : NULL;
}

static void budgetrebuilds(hwrtdynslot &s)
{
    int nbuild = 0;
    loopi(HWRT_MAX_ANIM_BLAS) if(s.anim[i].used && s.anim[i].cmd == 2 && s.anim[i].updates > 0) nbuild++;
    while(nbuild > HWRT_ANIM_REBUILDS_PER_FRAME)
    {
        int newest = -1, newestu = 0x7fffffff;
        loopi(HWRT_MAX_ANIM_BLAS)
        {
            hwrtanimslot &a = s.anim[i];
            if(!a.used || a.cmd != 2 || a.updates < 1) continue;
            if(a.updates < newestu) { newestu = a.updates; newest = i; }
        }
        if(newest < 0) break;
        s.anim[newest].cmd = 1;
        nbuild--;
    }
}

static bool skipdynent(dynent *d)
{
    if(!d) return true;
    if(d->state == CS_SPECTATOR || d->state == CS_SPAWNING) return true;
    // A dead body GL is hiding must not be traced either: mode 7 would shade
    // it and hand back a corpse the rasteriser deliberately left out.
    if(!hwrtdynentvisible(d)) return true;
    return false;
}

// First-person body: keep it in the TLAS so it still occludes shadow / sun /
// sky rays, but tag it SELF so a primary ray cannot see the inside of the
// skull. Use the thirdperson cvar, not isthirdperson(): that flag is also
// true for a death camera, a detached edit cam, and the first frames of a
// map, and tagging the body MODEL then lets the sky eat it (the 3D pit).
extern int thirdperson;
static bool isfirstpersonbody(dynent *d)
{
    if(!d) return false;
    if(d->state == CS_DEAD || d->state == CS_SPECTATOR || d->state == CS_SPAWNING) return false;
    if(thirdperson) return false;
    if(d == player || (physent *)d == camera1) return true;
    dynent *hide = hwrtcameradynent();
    return hide && d == hide;
}

static bool isfreezeoccluder(dynent *d)
{
    if(isfirstpersonbody(d)) return true;
    // Edit flight uses a detached cam (isthirdperson), so the local body is
    // not SELF. Still freeze that body so you can fly up and look down at it.
    if(!d) return false;
    if(d->state != CS_EDITING) return false;
    return d == player || (physent *)d == camera1;
}

static void notefull()
{
    if(dyn.instfulllogged) return;
    conoutf(CON_WARN, "hwrt: more than %d TLAS instances, extras dropped", int(HWRT_MAX_INSTANCES));
    dyn.instfulllogged = true;
}

// The game provides a strong definition (player / monster / movable names).
// This weak fallback keeps the overlay linking against an unpatched fpsgame.
__attribute__((weak)) const char *hwrtdynentmdlname(dynent *d)
{
    (void)d;
    return NULL;
}

// Pickups spin and bob in the raster pass. The game owns that animation and
// provides a strong definition; this fallback is the static entity origin.
__attribute__((weak)) void hwrtentxform(const extentity &e, vec &o, float &yaw)
{
    o = e.o;
    yaw = 0;
}

__attribute__((weak)) dynent *hwrtcameradynent()
{
    return NULL;
}

// An unpatched fpsgame draws every dynent it iterates and keeps no second
// corpse list, so the fallbacks are "visible" and "none".
__attribute__((weak)) bool hwrtdynentvisible(dynent *d)
{
    (void)d;
    return true;
}

__attribute__((weak)) int hwrtnumragdolls()
{
    return 0;
}

__attribute__((weak)) dynent *hwrtragdoll(int i)
{
    (void)i;
    return NULL;
}

__attribute__((weak)) void hwrtpreloadplayermodels()
{
}

static int hwrtpreloadn = 0;

void hwrtpreloadmodel(const char *name)
{
    if(!name || !name[0]) return;
    if(dyn.failed || !hwrtdev.ok() || !hwrtdev.rayquery) return;
    model *m = loadmodel(name);
    if(m) pinmodel(m);
    blasforname(name, true);
    hwrtpreloadn++;
    renderprogress(min(hwrtpreloadn/40.0f, 1.0f), "preparing characters...");
}

__attribute__((weak)) int hwrtdynentattach(dynent *d, modelattach *dst, int maxa)
{
    (void)d; (void)dst; (void)maxa;
    return 0;
}

// Respawn / slot reuse: a new occupant of the same dynent pointer must not
// inherit the previous occupant's skinned pose. Weak fallback has no
// lifesequence, so velocity history keys on pointer + model + teleport.
__attribute__((weak)) int hwrtdynentseq(dynent *d)
{
    (void)d;
    return 0;
}

__attribute__((weak)) int hwrtdynentuid(dynent *d)
{
    (void)d;
    return 0;
}

// customIndex for a rest-pose BLAS is its cache slot, for an animated one its
// pool slot, both offset past the world's 0. That is what lets the lighting
// shader tell a player from a crate and find the right attributes.
static uint32_t customforcache(const hwrtmodelblas *c)
{
    if(!c) return 0;
    return uint32_t(HWRT_GEOM_MODEL0 + int(c - dyn.cache));
}

static uint32_t customforanim(const hwrtdynslot &s, const hwrtanimslot *a)
{
    if(!a) return 0;
    return uint32_t(HWRT_GEOM_ANIM0 + int(a - s.anim));
}

static void writegeom(hwrtdynslot &s, uint32_t custom, const hwrtbuf &attr, const hwrtbuf &idx, const hwrtbuf &tri, uint ntris, uint nopaque)
{
    if(!s.geommap || custom < 1 || custom >= HWRT_MAX_GEOMS) return;
    hwrtmodelgeom &g = s.geommap[custom];
    bool ok = attr.address && idx.address && tri.address && ntris;
    g.verts = ok ? attr.address : 0;
    g.indices = ok ? idx.address : 0;
    g.tris = ok ? tri.address : 0;
    g.ntris = ok ? ntris : 0;
    g.flags = 0;
    if(ok)
    {
        if(nopaque > ntris) nopaque = ntris;
        g.flags = HWRT_GEOM_SHADE | (nopaque << HWRT_GEOM_NOPAQUE_SHIFT);
    }
    hwrtgeomcount++;
    static bool loggedok = false, loggedfail = false;
    if(ok && !loggedok)
    {
        conoutf("hwrt: model geom custom %u ntris %u", custom, ntris);
        loggedok = true;
    }
    else if(!ok && !loggedfail)
    {
        conoutf(CON_WARN, "hwrt: model geom custom %u missing attrs (ntris %u), that instance stays lit by GL", custom, ntris);
        loggedfail = true;
    }
}

// GL rasters ENT_PLAYER with MDL_FULLBRIGHT. The lighting shader uses that
// flag for the menu look (studio fill + spec + a modest visibility floor),
// not GL's flattening 1.5 plate. The table is memset every frame, so this
// cannot go stale onto a crate that happens to reuse the slot next frame.
static void markfullbright(hwrtdynslot &s, uint32_t custom)
{
    if(!s.geommap || custom < 1 || custom >= HWRT_MAX_GEOMS) return;
    s.geommap[custom].flags |= HWRT_GEOM_FULLBRIGHT;
}

static uint32_t addinstance(hwrtinstance *dst, uint32_t n, const vec &o, float yaw, float pitch, const hwrtmodelblas *c)
{
    if(!c || !c->addr) return n;
    if(n >= HWRT_MAX_INSTANCES) { notefull(); return n; }
    uint32_t mask = HWRT_RAYMASK_MODEL;
    if(isdefaultteleporter(c->m)) mask |= HWRT_RAYMASK_PORTAL;
    instanceat(dst[n], o, yaw, pitch, customforcache(c), c->addr, mask);
    return n + 1;
}

static uint32_t addaniminstance(hwrtinstance *dst, uint32_t n, dynent *d, hwrtdynslot &s, hwrtanimslot *anim, const hwrtmodelblas *fallback, uint32_t raymask)
{
    if(n >= HWRT_MAX_INSTANCES) { notefull(); return n; }
    // Third-person players are FORCE_NO_OPAQUE so skipInst can ignore the
    // mesh that launched the ray (opaque tris commit in hardware). The
    // first-person body stays opaque: it is SELF, never shaded, and must
    // still stain the floor along the sun / lamps.
    uint32_t extra = (d->type == ENT_PLAYER && !isfirstpersonbody(d))
        ? uint32_t(VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR) : 0;
    float yaw = d->yaw, pitch = 0;
    if(d->type == ENT_PLAYER || d->type == ENT_AI) yaw += 90;
    // Pitch stays off the instance. GL's world matrix is T * Rz(yaw);
    // look-pitch is per-bone in the skeleton (and a part-local rotate),
    // not a whole-body Ry. Putting pitch here made the legs tilt with the
    // camera — a different silhouette as soon as anyone looked up.
    matrix4x3 xf;
    xf.identity();
    if(!(anim && anim->addr && anim->key.worldspace))
    {
        xf.settranslation(d->feetpos());
        xf.rotate_around_z(yaw*RAD);
        if(pitch != 0) xf.rotate_around_y(-pitch*RAD);
    }
    if(isfreezeoccluder(d) && hwrtfreezeself)
    {
        if(freezeselfxfok) xf = freezeselfxf;
        else
        {
            freezeselfxf = xf;
            freezeselfxfok = true;
            freezeselfcamyaw = camera1 ? camera1->yaw : d->yaw;
            freezeselfcampitch = camera1 ? camera1->pitch : d->pitch;
            vec feet = d->feetpos();
            conoutf("hwrt: froze FP occluder feet=(%.2f %.2f %.2f) instyaw=%.1f d_yaw=%.1f d_pitch=%.1f cam_yaw=%.1f cam_pitch=%.1f animinterp=%d geom=%d verts=%d",
                    feet.x, feet.y, feet.z, yaw, d->yaw, d->pitch,
                    freezeselfcamyaw, freezeselfcampitch,
                    d->animinterp[0].lastmodel ? 1 : 0,
                    freezeselfgeomok ? 1 : 0,
                    freezeselfgeom.positions.length());
        }
        if(freezeselfxfok && camera1 && freezeselfmovedlog < 2)
        {
            float dyaw = fabs(camera1->yaw - freezeselfcamyaw);
            float dpitch = fabs(camera1->pitch - freezeselfcampitch);
            if(dyaw > 12.f || dpitch > 12.f)
            {
                conoutf("hwrt: freeze held; view dyaw=%.1f dpitch=%.1f live body yaw=%.1f pitch=%.1f",
                        dyaw, dpitch, d->yaw, d->pitch);
                freezeselfmovedlog++;
            }
        }
    }
    if(anim && anim->addr)
    {
        uint32_t custom = customforanim(s, anim);
        if(d->type == ENT_PLAYER) markfullbright(s, custom);
        writeinstance(dst[n], xf, custom, anim->addr, raymask, extra);
        return n + 1;
    }
    if(!fallback || !fallback->addr) return n;
    uint32_t custom = customforcache(fallback);
    if(d->type == ENT_PLAYER) markfullbright(s, custom);
    writeinstance(dst[n], xf, custom, fallback->addr, raymask, extra);
    return n + 1;
}

// One dynent, from either list: its animated pose if there is a slot for it,
// the rest pose otherwise. A corpse gets no rest-pose fallback — a standing
// figure where GL drew a body on the floor is worse than the composite
// painting over it, which is what happened before it was traced at all.
static uint32_t adddynent(hwrtdynslot &s, hwrtinstance *dst, uint32_t n, dynent *d)
{
    const char *name = hwrtdynentmdlname(d);
    if(!name || !name[0]) return n;
    model *m = loadmodel(name);
    hwrtmodelblas *c = m ? findcache(m) : NULL;
    if(c && !c->addr) c = NULL;
    float pitch = (d->type == ENT_PLAYER || d->type == ENT_AI) ? d->pitch : 0;
    hwrtanimslot *anim = m ? animfordynent(s, d, m, pitch) : NULL;
    if(d->ragdoll && (!anim || !anim->addr)) return n;
    if(!anim && !c) return n;
    if(anim && anim->addr) writegeom(s, customforanim(s, anim), anim->attrbuf, anim->ibuf, anim->tribuf, anim->ntris, anim->ntris);
    uint32_t raymask = isfirstpersonbody(d) ? HWRT_RAYMASK_SELF : HWRT_RAYMASK_MODEL;
    if(d == player)
    {
        static int lastfp = -1;
        int fp = isfirstpersonbody(d) ? 1 : 0;
        if(fp != lastfp)
        {
            conoutf("hwrt: local player mask %s (fp=%d thirdcvar=%d camsplit=%d state=%d anim=%d)",
                    raymask == HWRT_RAYMASK_SELF ? "SELF" : "MODEL",
                    fp, thirdperson, isthirdperson() ? 1 : 0, int(d->state),
                    (anim && anim->addr) ? 1 : 0);
            lastfp = fp;
        }
    }
    return addaniminstance(dst, n, d, s, anim, c, raymask);
}

// A mapmodel waiting for a slot. Mapmodels are the only class that can
// outnumber the instance budget, so they are the only class that queues:
// everything else is written the moment it is found and is never cut.
struct hwrtqueuedmap
{
    vec o;
    float yaw, pitch, dist;
    const hwrtmodelblas *c;
};

// Same yaw / pitch as animmodel::render: attr1 plus mdlspin / mdlyaw / mdlpitch
// at this frame's lastmillis. The lighting view shades mapmodels (alpha 0.5
// is kept by the composite), so a spinning fan that omitted this stayed still
// on screen while the velocity pass used the GL spin.
static void mapmodelpose(const extentity &e, const hwrtmodelblas *c, float &yaw, float &pitch)
{
    yaw = float(e.attr1);
    pitch = 0;
    model *m = c && c->m ? c->m : loadmapmodel(e.attr2);
    if(!m) return;
    yaw += m->spinyaw * lastmillis / 1000.0f + m->offsetyaw;
    pitch += m->offsetpitch + m->spinpitch * lastmillis / 1000.0f;
}

struct hwrtnearerfirst
{
    bool operator()(const hwrtqueuedmap &x, const hwrtqueuedmap &y) const { return x.dist < y.dist; }
};

static uint32_t gatherinstances(hwrtdynslot &s, hwrtinstance *dst, int *nmap, int *ndyn, int *nrag)
{
    uint32_t n = 0;
    int mapn = 0, dynn = 0, ragn = 0;
    VkDeviceAddress worldaddr = hwrtworldblasaddr();
    if(!worldaddr) return 0;

    matrix4x3 ident;
    ident.identity();
    writeinstance(dst[n++], ident, 0, worldaddr, HWRT_RAYMASK_WORLD);

    // The table is rewritten from scratch every frame: a slot that lost its
    // BLAS this frame must not keep a stale address alive.
    hwrtgeomcount = 0;
    if(s.geommap)
    {
        memset(s.geommap, 0, sizeof(hwrtmodelgeom)*HWRT_MAX_GEOMS);
        // Slot 0 is never a model (customIndex 0 is the world). ntris here is
        // the uploaded skin-layer count so the shader can refuse a layer
        // instead of sampling the placeholder descriptor.
        s.geommap[0].ntris = (skins.tex.view && skins.tex.layers > 0) ? uint32_t(skins.tex.layers) : 0;
    }
    loopi(dyn.ncache)
    {
        hwrtmodelblas &c = dyn.cache[i];
        if(c.addr) writegeom(s, customforcache(&c), c.attrbuf, c.idxbuf, c.tribuf, c.ntris, c.nopaque);
    }

    static vector<hwrtqueuedmap> queued;
    queued.setsize(0);
    vec eye = camera1 ? camera1->o : vec(0, 0, 0);

    const vector<extentity *> &ents = entities::getents();
    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type == ET_MAPMODEL)
        {
            if(e.flags&EF_NOVIS) continue;
            hwrtmodelblas *c = blasformapmodel(e.attr2, false);
            if(!c || !c->addr) continue;
            hwrtqueuedmap &q = queued.add();
            q.o = e.o;
            mapmodelpose(e, c, q.yaw, q.pitch);
            q.dist = eye.squaredist(e.o);
            q.c = c;
            continue;
        }
        const char *name = entities::entmodel(e);
        if(!name || !name[0]) continue;
        // GL draws teleports even when !spawned(); pickups still need a spawn.
        if(!e.spawned() && !hwrtalwaysvisibleent(e)) continue;
        hwrtmodelblas *c = blasforname(name, false);
        if(!c) continue;
        vec o;
        float yaw;
        hwrtentxform(e, o, yaw);
        uint32_t next = addinstance(dst, n, o, yaw, 0, c);
        if(next > n) dynn++;
        n = next;
    }

    loopi(game::numdynents())
    {
        dynent *d = game::iterdynents(i);
        if(skipdynent(d)) continue;
        uint32_t next = adddynent(s, dst, n, d);
        if(next > n) dynn++;
        n = next;
    }

    // rendergame() draws game::ragdolls straight after the dynents, and those
    // copies live nowhere in numdynents(). They are already in world space, so
    // the instance is identity and the pose is the corpse GL is drawing.
    loopi(hwrtnumragdolls())
    {
        dynent *d = hwrtragdoll(i);
        if(!d) continue;
        uint32_t next = adddynent(s, dst, n, d);
        if(next > n) ragn++;
        n = next;
    }

    loopi(nscenemodels)
    {
        hwrtmodelblas *c = isbulletname(scenemodels[i].name) ? bulletc : blasforname(scenemodels[i].name, false);
        if(!c) continue;
        uint32_t next = addinstance(dst, n, scenemodels[i].o, scenemodels[i].yaw, scenemodels[i].pitch, c);
        if(next > n) dynn++;
        n = next;
    }
    // Smoke sprites: one scaled sphere per puff. CASTER so primary / RTAO /
    // sky miss it. FORCE_NO_OPAQUE so a shadow ray can keep going when the
    // puff's opacity test fails — otherwise the BLAS commits a 100% block
    // and the trail never dissipates. Opacity lives in customIndex bits 8-15.
    if(bulletc && bulletc->addr) loopi(nsmokepuffs)
    {
        if(n >= HWRT_MAX_INSTANCES) { notefull(); break; }
        float rad = smokepuffs[i].radius;
        uint32_t geom = customforcache(bulletc) & 0xFFu;
        uint32_t packed = geom | (uint32_t(smokepuffs[i].opacity) << 8);
        instanceat(dst[n], smokepuffs[i].o, 0, 0, packed, bulletc->addr,
                   HWRT_RAYMASK_CASTER, uint32_t(VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR),
                   rad, rad, rad);
        n++;
        dynn++;
    }
    if(nscenemodels)
    {
        static bool logged = false;
        if(!logged)
        {
            conoutf("hwrt: %d scene model instance(s) in TLAS (%s)", nscenemodels, scenemodels[0].name);
            logged = true;
        }
    }

    // Mapmodels go in last, nearest first. They used to go in first, in the
    // order the mapper happened to place them, which cost twice over on a
    // packed map. cmvalley filled all 1024 slots with scenery and logged
    // `1023 mapmodels + 0 dynents`: nobody in the match was traced at all.
    // And the scenery it kept was arbitrary, so the trees it dropped could be
    // the ones under your nose — a ray aimed at a dropped tree carries on and
    // lights whatever stands behind it, which reads as trees showing through
    // other trees. Sorting cannot make room, but it decides *where* an overrun
    // shows: at the horizon, where a tree the rays miss is a few pixels wide.
    int budget = min(int(hwrtmaxinsts), int(HWRT_MAX_INSTANCES)) - int(n);
    if(budget < 0) budget = 0;
    if(queued.length() > budget) queued.sort(hwrtnearerfirst());
    loopv(queued)
    {
        if(mapn >= budget) break;
        uint32_t next = addinstance(dst, n, queued[i].o, queued[i].yaw, queued[i].pitch, queued[i].c);
        if(next == n) break;
        n = next;
        mapn++;
    }
    hwrtdroppedinsts = queued.length() - mapn;

    if(nmap) *nmap = mapn;
    if(ndyn) *ndyn = dynn;
    if(nrag) *nrag = ragn;
    return n;
}

static void syncmodels(bool build)
{
    const vector<extentity *> &ents = entities::getents();
    loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type == ET_MAPMODEL)
        {
            if(e.flags&EF_NOVIS) continue;
            blasformapmodel(e.attr2, build);
            continue;
        }
        const char *name = entities::entmodel(e);
        if(!name || !name[0]) continue;
        if(!e.spawned() && !hwrtalwaysvisibleent(e)) continue;
        blasforname(name, build);
    }
    loopi(game::numdynents())
    {
        dynent *d = game::iterdynents(i);
        if(skipdynent(d)) continue;
        const char *name = hwrtdynentmdlname(d);
        if(name && name[0]) blasforname(name, build);
    }
    // A corpse can outlive the player model it was wearing (team change, a
    // disconnect), so its skin has to be reached from this list too.
    loopi(hwrtnumragdolls())
    {
        dynent *d = hwrtragdoll(i);
        if(!d) continue;
        const char *name = hwrtdynentmdlname(d);
        if(name && name[0]) blasforname(name, build);
    }
    loopi(nscenemodels)
    {
        if(!scenemodels[i].name[0] || isbulletname(scenemodels[i].name)) continue;
        blasforname(scenemodels[i].name, build);
    }
}

bool hwrthasdynents()
{
    return dyn.ready && !dyn.failed && dyn.slot[0].tlas != VK_NULL_HANDLE;
}

bool hwrtmodelshadeready()
{
    return dyn.ready && !dyn.failed && dyn.slot[0].geombuf.buffer != VK_NULL_HANDLE;
}

static bool ensuredummygeom()
{
    if(dummygeom.buffer) return true;
    const VkDeviceSize bytes = VkDeviceSize(HWRT_MAX_GEOMS)*sizeof(hwrtmodelgeom);
    if(!allocbuf(dummygeom, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false))
        return false;
    void *mapped = NULL;
    if(vkMapMemory(hwrtdev.device, dummygeom.memory, 0, bytes, 0, &mapped) == VK_SUCCESS && mapped)
    {
        memset(mapped, 0, size_t(bytes));
        vkUnmapMemory(hwrtdev.device, dummygeom.memory);
    }
    return true;
}

void hwrtwritemodelbindings(VkDescriptorSet set, int slot)
{
    if(!set) return;
    VkBuffer geombuf = VK_NULL_HANDLE;
    if(slot >= 0 && slot < HWRT_FRAMES_IN_FLIGHT && dyn.slot[slot].geombuf.buffer)
        geombuf = dyn.slot[slot].geombuf.buffer;
    else if(ensuredummygeom()) geombuf = dummygeom.buffer;
    if(!geombuf) return;

    VkDescriptorBufferInfo ginfo = { geombuf, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo sinfo = {};
    if(skins.tex.view && skins.sampler)
    {
        sinfo.sampler = skins.sampler;
        sinfo.imageView = skins.tex.view;
        sinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    else
    {
        sinfo.sampler = hwrtworlddiffusesampler();
        sinfo.imageView = hwrtworlddiffuseview();
        sinfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if(!sinfo.imageView || !sinfo.sampler) return;

    VkWriteDescriptorSet writes[3];
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 7;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &ginfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 8;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &sinfo;
    int nwrite = 2;
    VkDescriptorImageInfo einfo = {};
    if(mdlenv.view && mdlenvsampler)
    {
        einfo.sampler = mdlenvsampler;
        einfo.imageView = mdlenv.view;
        einfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = set;
        writes[2].dstBinding = 11;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].pImageInfo = &einfo;
        nwrite = 3;
    }
    vkUpdateDescriptorSets(hwrtdev.device, nwrite, writes, 0, NULL);
}

VkAccelerationStructureKHR hwrtgetslottlas(int slot)
{
    if(slot < 0 || slot >= HWRT_FRAMES_IN_FLIGHT) return VK_NULL_HANDLE;
    return dyn.slot[slot].tlas;
}

void hwrtsyncdynents()
{
    if(dyn.failed || !dyn.ready) return;
    if(!hwrtdev.ok() || !hwrtdev.rayquery) return;
    if(!bulletc) buildbulletblas();
    syncmodels(true);
    if(allowskinprune && skinsunderpressure() && cachehasunused(true))
    {
        vkDeviceWaitIdle(hwrtdev.device);
        if(prunecache(true) > 0) compactskins();
    }
    // Same place the rest-pose BLASes build, and for the same reason: the skin
    // upload reads back GL textures and waits on the device, so it must not
    // happen once the traced frame has started recording. A skin first seen
    // while skinning frame N therefore lands in the array at frame N+1.
    syncskins();
    syncmdlenv();
}

void hwrtrebuilddynents()
{
    clearfreezeself();
    if(!hwrtdev.ok() || !hwrtdev.rayquery || !vkCreateAccelerationStructureKHR)
    {
        if(!dyn.norqlogged)
        {
            conoutf(CON_WARN, "hwrt: no ray query, dynents not in the TLAS");
            dyn.norqlogged = true;
        }
        return;
    }
    destroylocked();
    dyn.failed = false;
    dyn.maplogged = false;
    dyn.overflowlogged = false;
    dyn.animlogged = false;
    if(!hwrtworldblasaddr()) return;
    if(!ensuretlasbuffers()) return;
    // The default 8-triangle world at boot is not a map: copying every
    // playermodel there triples the work and has blown the GPU. Wait until
    // a real map has a BLAS. Player looks first, so a packed map cannot
    // spend the 64 BLAS slots on crates and leave the first stranger as a hitch.
    if(hwrtworldtris > 64)
    {
        renderprogress(0, "preparing characters...");
        hwrtpreloadn = 0;
        // CTF flags are not mapmodels. Pin them before a packed map
        // (flagstone: 140+ unique mmodels) fills the skin atlas, or the
        // JPEG cloth shades as an untextured white sheet.
        hwrtpreloadmodel("flags/red");
        hwrtpreloadmodel("flags/blue");
        hwrtpreloadmodel("flags/neutral");
        hwrtpreloadmodel("projectiles/grenade");
        hwrtpreloadmodel("projectiles/rocket");
        buildbulletblas();
        hwrtpreloadplayermodels();
    }
    syncmodels(true);
    if(dyn.failed)
    {
        destroylocked();
        return;
    }
    dyn.ready = true;
    allowskinprune = false;
    syncskins();
    allowskinprune = true;
}

bool hwrtpreparedynents(int slot)
{
    if(dyn.failed || !dyn.ready) return false;
    if(!hwrtdev.rayquery)
    {
        if(!dyn.norqlogged)
        {
            conoutf(CON_WARN, "hwrt: no ray query, dynents not in the TLAS");
            dyn.norqlogged = true;
        }
        return false;
    }
    if(slot < 0 || slot >= HWRT_FRAMES_IN_FLIGHT) return false;
    hwrtdynslot &s = dyn.slot[slot];
    if(!s.instmap || s.tlas == VK_NULL_HANDLE) return false;
    if(!hwrtworldblasaddr()) return false;

    loopi(HWRT_MAX_ANIM_BLAS)
    {
        s.anim[i].used = false;
        s.anim[i].cmd = 0;
    }

    int mapn = 0, dynn = 0, ragn = 0;
    uint32_t n = gatherinstances(s, s.instmap, &mapn, &dynn, &ragn);
    if(n < 1) return false;
    budgetrebuilds(s);
    int nanim = 0, nbuild = 0, nrefit = 0;
    loopi(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = s.anim[i];
        if(!a.used || !a.addr) continue;
        nanim++;
        if(a.cmd == 2) nbuild++;
        else if(a.cmd == 1) nrefit++;
    }
    s.ninst = n;
    hwrtinstancecount = int(n);
    hwrtmapmodelinsts = mapn;
    hwrtdynentinsts = dynn;
    hwrtragdollinsts = ragn;
    hwrtanimcount = nanim;
    hwrtanimbuilds = nbuild;
    hwrtanimrefits = nrefit;
    if(!dyn.maplogged)
    {
        conoutf("hwrt: %d mapmodel instances, %d dynents, %d animated BLASes, model shading %s",
                mapn, dynn, nanim, hwrtmodelshadeready() ? "on" : "off");
        if(hwrtdroppedinsts > 0)
            conoutf(CON_WARN, "hwrt: %d mapmodels past hwrtmaxinsts %d, farthest dropped",
                    hwrtdroppedinsts, min(int(hwrtmaxinsts), int(HWRT_MAX_INSTANCES)));
        dyn.maplogged = true;
    }
    return true;
}

void hwrtrecordtlas(VkCommandBuffer cmd, int slot)
{
    if(!cmd || slot < 0 || slot >= HWRT_FRAMES_IN_FLIGHT) return;
    hwrtdynslot &s = dyn.slot[slot];
    if(!s.tlas || !s.instbuf.address || s.ninst < 1)
    {
        hwrtwritestamp(cmd, slot, HWRT_TS_BLAS);
        hwrtwritestamp(cmd, slot, HWRT_TS_TLAS);
        return;
    }

    VkDeviceSize scralign = hwrtdev.scratchalign ? hwrtdev.scratchalign : 256;

    VkMemoryBarrier hostbarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    hostbarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostbarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR|VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &hostbarrier, 0, NULL, 0, NULL);

    loopi(HWRT_MAX_ANIM_BLAS)
    {
        hwrtanimslot &a = s.anim[i];
        if(!a.used || !a.addr || !a.cmd || !a.vbuf.address || !s.blasscratch.address) continue;

        VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geo.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geo.geometry.triangles.vertexData.deviceAddress = a.vbuf.address;
        geo.geometry.triangles.vertexStride = 16;
        geo.geometry.triangles.maxVertex = a.nverts - 1;
        geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geo.geometry.triangles.indexData.deviceAddress = a.ibuf.address;

        VkAccelerationStructureBuildGeometryInfoKHR blasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        blasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        blasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR|
                         VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        if(a.cmd == 2)
        {
            blasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            a.updates = 0;
        }
        else
        {
            blasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            blasinfo.srcAccelerationStructure = a.blas;
        }
        blasinfo.dstAccelerationStructure = a.blas;
        blasinfo.geometryCount = 1;
        blasinfo.pGeometries = &geo;
        blasinfo.scratchData.deviceAddress = alignedaddr(s.blasscratch.address, scralign);

        VkAccelerationStructureBuildRangeInfoKHR blasrange = {};
        blasrange.primitiveCount = a.ntris;
        const VkAccelerationStructureBuildRangeInfoKHR *blasranges = &blasrange;
        vkCmdBuildAccelerationStructuresKHR(cmd, 1, &blasinfo, &blasranges);

        VkMemoryBarrier asbarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        asbarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        asbarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             0, 1, &asbarrier, 0, NULL, 0, NULL);
    }

    hwrtwritestamp(cmd, slot, HWRT_TS_BLAS);

    VkAccelerationStructureGeometryKHR tgeo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.arrayOfPointers = VK_FALSE;
    tgeo.geometry.instances.data.deviceAddress = s.instbuf.address;

    VkAccelerationStructureBuildGeometryInfoKHR tlasinfo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    tlasinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    tlasinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasinfo.geometryCount = 1;
    tlasinfo.pGeometries = &tgeo;
    tlasinfo.dstAccelerationStructure = s.tlas;
    tlasinfo.scratchData.deviceAddress = alignedaddr(s.scratch.address, scralign);

    VkAccelerationStructureBuildRangeInfoKHR tlasrange = {};
    tlasrange.primitiveCount = s.ninst;
    const VkAccelerationStructureBuildRangeInfoKHR *tlasranges = &tlasrange;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tlasinfo, &tlasranges);

    hwrtwritestamp(cmd, slot, HWRT_TS_TLAS);

    VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
}
