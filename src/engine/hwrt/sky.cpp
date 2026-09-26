// sky.cpp: optional GL sun disk along sunlightdir (hwrtsundisk 1).
//
// Mode 7 already traces sunlightyaw/pitch. The skybox cubemap often paints a
// sun that does not move with those vars, so the disk the player looks at and
// the hard shadow disagree. Off by default: the map skybox is more natural
// than a flat stand-in. hwrtsundisk 1 skips that cubemap and draws a GL disk
// on sunlightdir instead. hwrt 0 and sunlight 0 leave the map sky alone.
// Failure logs once and does not touch the Vulkan sun ray.

#include "engine.h"

extern int sunlight, atmo;
extern float atmosundisksize;

static void drawalignsunfaces(int w, float z1clip, float z2clip, int faces)
{
    if(z1clip >= z2clip) return;

    float z1 = 2*w*(z1clip-0.5f), z2 = ceil(2*w*(z2clip-0.5f));

    gle::defvertex();
    gle::begin(GL_QUADS);

    if(faces&0x01)
    {
        gle::attribf(-w, -w, z1);
        gle::attribf(-w,  w, z1);
        gle::attribf(-w,  w, z2);
        gle::attribf(-w, -w, z2);
    }
    if(faces&0x02)
    {
        gle::attribf(w, -w, z2);
        gle::attribf(w,  w, z2);
        gle::attribf(w,  w, z1);
        gle::attribf(w, -w, z1);
    }
    if(faces&0x04)
    {
        gle::attribf(-w, -w, z2);
        gle::attribf( w, -w, z2);
        gle::attribf( w, -w, z1);
        gle::attribf(-w, -w, z1);
    }
    if(faces&0x08)
    {
        gle::attribf( w, w, z2);
        gle::attribf(-w, w, z2);
        gle::attribf(-w, w, z1);
        gle::attribf( w, w, z1);
    }
    if(z1clip <= 0 && faces&0x10)
    {
        gle::attribf(-w, -w, -w);
        gle::attribf( w, -w, -w);
        gle::attribf( w,  w, -w);
        gle::attribf(-w,  w, -w);
    }
    if(z2clip >= 1 && faces&0x20)
    {
        gle::attribf( w, -w, w);
        gle::attribf(-w, -w, w);
        gle::attribf(-w,  w, w);
        gle::attribf( w,  w, w);
    }

    xtraverts += gle::end();
}

static bool prepareskyfog()
{
    static int state = 0;
    if(state) return state > 0;
    Shader *s = useshaderbyname("skyfog");
    if(!s || s->invalid() || !s->loaded())
    {
        conoutf(CON_WARN, "hwrt: skyfog missing, leaving the lighting-view sun disk off");
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

void hwrtdrawalignsun(int farplane, float skyclip, float topclip, int faces)
{
    if(!hwrtalignsundisk() || !sunlight) return;
    // Atmosphere already puts a disk on sunlightdir. Skip the extra sprite.
    if(atmo) return;
    if(!prepareskyfog()) return;

    int w = farplane/2;
    if(w <= 0) return;

    SETSHADER(skyfog);

    matrix4 skymatrix = cammatrix, skyprojmatrix;
    skymatrix.settranslation(0, 0, 0);
    skyprojmatrix.mul(projmatrix, skymatrix);
    LOCALPARAM(skymatrix, skyprojmatrix);

    // Flat stand-in for the cubemap so a painted sun cannot lie. Keep it a
    // readable dusk blue rather than copying a possibly-black skylight.
    gle::color(vec(0.16f, 0.20f, 0.32f));
    drawalignsunfaces(w, skyclip, topclip, faces);

    float ang = atmosundisksize > 1e-3f ? atmosundisksize : 12.0f;
    float disksin = sinf(0.5f*ang*RAD);
    if(disksin <= 0) return;

    float z1 = 2*w*(skyclip-0.5f), z2 = ceil(2*w*(topclip-0.5f));
    if(sunlightdir.z*w + disksin <= z1 || sunlightdir.z*w - disksin >= z2) return;

    vec disk = sunlightcolor.tocolor().mul(max(sunlightscale, 1.0f));
    disk.mul(glaring ? 1.0f : 2.0f);
    if(max(disk.x, max(disk.y, disk.z)) < 0.35f) disk = vec(1, 0.92f, 0.62f);
    disk.min(1);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    gle::defvertex();
    gle::defcolor(4);
    gle::begin(GL_TRIANGLE_FAN);
    gle::attrib(vec(sunlightdir).mul(float(w)));
    gle::attrib(vec4(disk, 1.0f));
    vec spoke;
    spoke.orthogonal(sunlightdir);
    spoke.rescale(disksin*1.8f);
    const int n = 24;
    loopi(n+1)
    {
        vec p = vec(spoke).rotate(-2*M_PI*i/float(n), sunlightdir).add(sunlightdir).mul(float(w));
        gle::attrib(p);
        gle::attrib(vec4(disk, 0.0f));
    }
    xtraverts += gle::end();

    glDisable(GL_BLEND);
}
