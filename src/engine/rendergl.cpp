// rendergl.cpp: core opengl rendering stuff

#include "engine.h"

bool hasVAO = false, hasFBO = false, hasAFBO = false, hasDS = false, hasTF = false, hasTRG = false, hasTSW = false, hasS3TC = false, hasFXT1 = false, hasLATC = false, hasRGTC = false, hasAF = false, hasFBB = false, hasUBO = false, hasMBR = false;

VAR(glversion, 1, 0, 0);
VAR(glslversion, 1, 0, 0);
VAR(glcompat, 1, 0, 0);

// OpenGL 1.3
#ifdef WIN32
PFNGLACTIVETEXTUREPROC       glActiveTexture_       = NULL;

PFNGLBLENDEQUATIONEXTPROC glBlendEquation_ = NULL;
PFNGLBLENDCOLOREXTPROC    glBlendColor_    = NULL;

PFNGLTEXIMAGE3DPROC        glTexImage3D_        = NULL;
PFNGLTEXSUBIMAGE3DPROC     glTexSubImage3D_     = NULL;
PFNGLCOPYTEXSUBIMAGE3DPROC glCopyTexSubImage3D_ = NULL;

PFNGLCOMPRESSEDTEXIMAGE3DPROC    glCompressedTexImage3D_    = NULL;
PFNGLCOMPRESSEDTEXIMAGE2DPROC    glCompressedTexImage2D_    = NULL;
PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC glCompressedTexSubImage3D_ = NULL;
PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC glCompressedTexSubImage2D_ = NULL;
PFNGLGETCOMPRESSEDTEXIMAGEPROC   glGetCompressedTexImage_   = NULL;

PFNGLDRAWRANGEELEMENTSPROC glDrawRangeElements_ = NULL;
#endif

// OpenGL 2.0
#ifndef __APPLE__
PFNGLMULTIDRAWARRAYSPROC   glMultiDrawArrays_   = NULL;
PFNGLMULTIDRAWELEMENTSPROC glMultiDrawElements_ = NULL;

PFNGLBLENDFUNCSEPARATEPROC     glBlendFuncSeparate_     = NULL;
PFNGLBLENDEQUATIONSEPARATEPROC glBlendEquationSeparate_ = NULL;
PFNGLSTENCILOPSEPARATEPROC     glStencilOpSeparate_     = NULL;
PFNGLSTENCILFUNCSEPARATEPROC   glStencilFuncSeparate_   = NULL;
PFNGLSTENCILMASKSEPARATEPROC   glStencilMaskSeparate_   = NULL;

PFNGLGENBUFFERSPROC       glGenBuffers_       = NULL;
PFNGLBINDBUFFERPROC       glBindBuffer_       = NULL;
PFNGLMAPBUFFERPROC        glMapBuffer_        = NULL;
PFNGLUNMAPBUFFERPROC      glUnmapBuffer_      = NULL;
PFNGLBUFFERDATAPROC       glBufferData_       = NULL;
PFNGLBUFFERSUBDATAPROC    glBufferSubData_    = NULL;
PFNGLDELETEBUFFERSPROC    glDeleteBuffers_    = NULL;
PFNGLGETBUFFERSUBDATAPROC glGetBufferSubData_ = NULL;

PFNGLGENQUERIESPROC        glGenQueries_        = NULL;
PFNGLDELETEQUERIESPROC     glDeleteQueries_     = NULL;
PFNGLBEGINQUERYPROC        glBeginQuery_        = NULL;
PFNGLENDQUERYPROC          glEndQuery_          = NULL;
PFNGLGETQUERYIVPROC        glGetQueryiv_        = NULL;
PFNGLGETQUERYOBJECTIVPROC  glGetQueryObjectiv_  = NULL;
PFNGLGETQUERYOBJECTUIVPROC glGetQueryObjectuiv_ = NULL;

PFNGLCREATEPROGRAMPROC            glCreateProgram_            = NULL;
PFNGLDELETEPROGRAMPROC            glDeleteProgram_            = NULL;
PFNGLUSEPROGRAMPROC               glUseProgram_               = NULL;
PFNGLCREATESHADERPROC             glCreateShader_             = NULL;
PFNGLDELETESHADERPROC             glDeleteShader_             = NULL;
PFNGLSHADERSOURCEPROC             glShaderSource_             = NULL;
PFNGLCOMPILESHADERPROC            glCompileShader_            = NULL;
PFNGLGETSHADERIVPROC              glGetShaderiv_              = NULL;
PFNGLGETPROGRAMIVPROC             glGetProgramiv_             = NULL;
PFNGLATTACHSHADERPROC             glAttachShader_             = NULL;
PFNGLGETPROGRAMINFOLOGPROC        glGetProgramInfoLog_        = NULL;
PFNGLGETSHADERINFOLOGPROC         glGetShaderInfoLog_         = NULL;
PFNGLLINKPROGRAMPROC              glLinkProgram_              = NULL;
PFNGLGETUNIFORMLOCATIONPROC       glGetUniformLocation_       = NULL;
PFNGLUNIFORM1FPROC                glUniform1f_                = NULL;
PFNGLUNIFORM2FPROC                glUniform2f_                = NULL;
PFNGLUNIFORM3FPROC                glUniform3f_                = NULL;
PFNGLUNIFORM4FPROC                glUniform4f_                = NULL;
PFNGLUNIFORM1FVPROC               glUniform1fv_               = NULL;
PFNGLUNIFORM2FVPROC               glUniform2fv_               = NULL;
PFNGLUNIFORM3FVPROC               glUniform3fv_               = NULL;
PFNGLUNIFORM4FVPROC               glUniform4fv_               = NULL;
PFNGLUNIFORM1IPROC                glUniform1i_                = NULL;
PFNGLUNIFORM2IPROC                glUniform2i_                = NULL;
PFNGLUNIFORM3IPROC                glUniform3i_                = NULL;
PFNGLUNIFORM4IPROC                glUniform4i_                = NULL;
PFNGLUNIFORM1IVPROC               glUniform1iv_               = NULL;
PFNGLUNIFORM2IVPROC               glUniform2iv_               = NULL;
PFNGLUNIFORM3IVPROC               glUniform3iv_               = NULL;
PFNGLUNIFORM4IVPROC               glUniform4iv_               = NULL;
PFNGLUNIFORMMATRIX2FVPROC         glUniformMatrix2fv_         = NULL;
PFNGLUNIFORMMATRIX3FVPROC         glUniformMatrix3fv_         = NULL;
PFNGLUNIFORMMATRIX4FVPROC         glUniformMatrix4fv_         = NULL;
PFNGLBINDATTRIBLOCATIONPROC       glBindAttribLocation_       = NULL;
PFNGLGETACTIVEUNIFORMPROC         glGetActiveUniform_         = NULL;
PFNGLENABLEVERTEXATTRIBARRAYPROC  glEnableVertexAttribArray_  = NULL;
PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray_ = NULL;

PFNGLVERTEXATTRIB1FPROC           glVertexAttrib1f_           = NULL;
PFNGLVERTEXATTRIB1FVPROC          glVertexAttrib1fv_          = NULL;
PFNGLVERTEXATTRIB1SPROC           glVertexAttrib1s_           = NULL;
PFNGLVERTEXATTRIB1SVPROC          glVertexAttrib1sv_          = NULL;
PFNGLVERTEXATTRIB2FPROC           glVertexAttrib2f_           = NULL;
PFNGLVERTEXATTRIB2FVPROC          glVertexAttrib2fv_          = NULL;
PFNGLVERTEXATTRIB2SPROC           glVertexAttrib2s_           = NULL;
PFNGLVERTEXATTRIB2SVPROC          glVertexAttrib2sv_          = NULL;
PFNGLVERTEXATTRIB3FPROC           glVertexAttrib3f_           = NULL;
PFNGLVERTEXATTRIB3FVPROC          glVertexAttrib3fv_          = NULL;
PFNGLVERTEXATTRIB3SPROC           glVertexAttrib3s_           = NULL;
PFNGLVERTEXATTRIB3SVPROC          glVertexAttrib3sv_          = NULL;
PFNGLVERTEXATTRIB4FPROC           glVertexAttrib4f_           = NULL;
PFNGLVERTEXATTRIB4FVPROC          glVertexAttrib4fv_          = NULL;
PFNGLVERTEXATTRIB4SPROC           glVertexAttrib4s_           = NULL;
PFNGLVERTEXATTRIB4SVPROC          glVertexAttrib4sv_          = NULL;
PFNGLVERTEXATTRIB4BVPROC          glVertexAttrib4bv_          = NULL;
PFNGLVERTEXATTRIB4IVPROC          glVertexAttrib4iv_          = NULL;
PFNGLVERTEXATTRIB4UBVPROC         glVertexAttrib4ubv_         = NULL;
PFNGLVERTEXATTRIB4UIVPROC         glVertexAttrib4uiv_         = NULL;
PFNGLVERTEXATTRIB4USVPROC         glVertexAttrib4usv_         = NULL;
PFNGLVERTEXATTRIB4NBVPROC         glVertexAttrib4Nbv_         = NULL;
PFNGLVERTEXATTRIB4NIVPROC         glVertexAttrib4Niv_         = NULL;
PFNGLVERTEXATTRIB4NUBPROC         glVertexAttrib4Nub_         = NULL;
PFNGLVERTEXATTRIB4NUBVPROC        glVertexAttrib4Nubv_        = NULL;
PFNGLVERTEXATTRIB4NUIVPROC        glVertexAttrib4Nuiv_        = NULL;
PFNGLVERTEXATTRIB4NUSVPROC        glVertexAttrib4Nusv_        = NULL;
PFNGLVERTEXATTRIBPOINTERPROC      glVertexAttribPointer_      = NULL;

PFNGLDRAWBUFFERSPROC glDrawBuffers_ = NULL;
#endif

// OpenGL 3.0
PFNGLGETSTRINGIPROC           glGetStringi_           = NULL;
PFNGLBINDFRAGDATALOCATIONPROC glBindFragDataLocation_ = NULL;

// GL_EXT_framebuffer_object
PFNGLBINDRENDERBUFFERPROC        glBindRenderbuffer_        = NULL;
PFNGLDELETERENDERBUFFERSPROC     glDeleteRenderbuffers_     = NULL;
PFNGLGENFRAMEBUFFERSPROC         glGenRenderbuffers_        = NULL;
PFNGLRENDERBUFFERSTORAGEPROC     glRenderbufferStorage_     = NULL;
PFNGLCHECKFRAMEBUFFERSTATUSPROC  glCheckFramebufferStatus_  = NULL;
PFNGLBINDFRAMEBUFFERPROC         glBindFramebuffer_         = NULL;
PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers_      = NULL;
PFNGLGENFRAMEBUFFERSPROC         glGenFramebuffers_         = NULL;
PFNGLFRAMEBUFFERTEXTURE2DPROC    glFramebufferTexture2D_    = NULL;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer_ = NULL;
PFNGLGENERATEMIPMAPPROC          glGenerateMipmap_          = NULL;

// GL_EXT_framebuffer_blit
PFNGLBLITFRAMEBUFFERPROC         glBlitFramebuffer_         = NULL;

// GL_ARB_uniform_buffer_object
PFNGLGETUNIFORMINDICESPROC       glGetUniformIndices_       = NULL;
PFNGLGETACTIVEUNIFORMSIVPROC     glGetActiveUniformsiv_     = NULL;
PFNGLGETUNIFORMBLOCKINDEXPROC    glGetUniformBlockIndex_    = NULL;
PFNGLGETACTIVEUNIFORMBLOCKIVPROC glGetActiveUniformBlockiv_ = NULL;
PFNGLUNIFORMBLOCKBINDINGPROC     glUniformBlockBinding_     = NULL;
PFNGLBINDBUFFERBASEPROC          glBindBufferBase_          = NULL;
PFNGLBINDBUFFERRANGEPROC         glBindBufferRange_         = NULL;

// GL_ARB_map_buffer_range
PFNGLMAPBUFFERRANGEPROC         glMapBufferRange_         = NULL;
PFNGLFLUSHMAPPEDBUFFERRANGEPROC glFlushMappedBufferRange_ = NULL;

// GL_ARB_vertex_array_object
PFNGLBINDVERTEXARRAYPROC    glBindVertexArray_    = NULL;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays_ = NULL;
PFNGLGENVERTEXARRAYSPROC    glGenVertexArrays_    = NULL;
PFNGLISVERTEXARRAYPROC      glIsVertexArray_      = NULL;

void *getprocaddress(const char *name)
{
    return SDL_GL_GetProcAddress(name);
}

VARP(ati_skybox_bug, 0, 0, 1);
VAR(ati_minmax_bug, 0, 0, 1);
VAR(ati_cubemap_bug, 0, 0, 1);
VAR(intel_vertexarray_bug, 0, 0, 1);
VAR(intel_mapbufferrange_bug, 0, 0, 1);
VAR(mesa_swap_bug, 0, 0, 1);
VAR(minimizetcusage, 1, 0, 0);
VAR(useubo, 1, 0, 0);
VAR(usetexcompress, 1, 0, 0);
VAR(rtscissor, 0, 1, 1);
VAR(blurtile, 0, 1, 1);
VAR(rtsharefb, 0, 1, 1);

VAR(dbgexts, 0, 0, 1);

hashset<const char *> glexts;

void parseglexts()
{
    if(glversion >= 300)
    {
        GLint numexts = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numexts);
        loopi(numexts)
        {
            const char *ext = (const char *)glGetStringi_(GL_EXTENSIONS, i);
            glexts.add(newstring(ext));
        }
    }
    else
    {
        const char *exts = (const char *)glGetString(GL_EXTENSIONS);
        for(;;)
        {
            while(*exts == ' ') exts++;
            if(!*exts) break;
            const char *ext = exts;
            while(*exts && *exts != ' ') exts++;
            if(exts > ext) glexts.add(newstring(ext, size_t(exts-ext)));
        }
    }
}

bool hasext(const char *ext)
{
    return glexts.access(ext)!=NULL;
}

void gl_checkextensions()
{
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    conoutf(CON_INIT, "Renderer: %s (%s)", renderer, vendor);
    conoutf(CON_INIT, "Driver: %s", version);

    bool mesa = false, intel = false, ati = false, nvidia = false;
    if(strstr(renderer, "Mesa") || strstr(version, "Mesa"))
    {
        mesa = true;
        if(strstr(renderer, "Intel")) intel = true;
    }
    else if(strstr(vendor, "NVIDIA"))
        nvidia = true;
    else if(strstr(vendor, "ATI") || strstr(vendor, "Advanced Micro Devices"))
        ati = true;
    else if(strstr(vendor, "Intel"))
        intel = true;

    uint glmajorversion, glminorversion;
    if(sscanf(version, " %u.%u", &glmajorversion, &glminorversion) != 2) glversion = 100;
    else glversion = glmajorversion*100 + glminorversion*10;

    if(glversion < 200) fatal("OpenGL 2.0 or greater is required!");

#ifdef WIN32
    glActiveTexture_ =            (PFNGLACTIVETEXTUREPROC)            getprocaddress("glActiveTexture");

    glBlendEquation_ =            (PFNGLBLENDEQUATIONPROC)            getprocaddress("glBlendEquation");
    glBlendColor_ =               (PFNGLBLENDCOLORPROC)               getprocaddress("glBlendColor");

    glTexImage3D_ =               (PFNGLTEXIMAGE3DPROC)               getprocaddress("glTexImage3D");
    glTexSubImage3D_ =            (PFNGLTEXSUBIMAGE3DPROC)            getprocaddress("glTexSubImage3D");
    glCopyTexSubImage3D_ =        (PFNGLCOPYTEXSUBIMAGE3DPROC)        getprocaddress("glCopyTexSubImage3D");

    glCompressedTexImage3D_ =     (PFNGLCOMPRESSEDTEXIMAGE3DPROC)     getprocaddress("glCompressedTexImage3D");
    glCompressedTexImage2D_ =     (PFNGLCOMPRESSEDTEXIMAGE2DPROC)     getprocaddress("glCompressedTexImage2D");
    glCompressedTexSubImage3D_ =  (PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC)  getprocaddress("glCompressedTexSubImage3D");
    glCompressedTexSubImage2D_ =  (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)  getprocaddress("glCompressedTexSubImage2D");
    glGetCompressedTexImage_ =    (PFNGLGETCOMPRESSEDTEXIMAGEPROC)    getprocaddress("glGetCompressedTexImage");

    glDrawRangeElements_ =        (PFNGLDRAWRANGEELEMENTSPROC)        getprocaddress("glDrawRangeElements");
#endif

#ifndef __APPLE__
    glMultiDrawArrays_ =          (PFNGLMULTIDRAWARRAYSPROC)          getprocaddress("glMultiDrawArrays");
    glMultiDrawElements_ =        (PFNGLMULTIDRAWELEMENTSPROC)        getprocaddress("glMultiDrawElements");

    glBlendFuncSeparate_ =        (PFNGLBLENDFUNCSEPARATEPROC)        getprocaddress("glBlendFuncSeparate");
    glBlendEquationSeparate_ =    (PFNGLBLENDEQUATIONSEPARATEPROC)    getprocaddress("glBlendEquationSeparate");
    glStencilOpSeparate_ =        (PFNGLSTENCILOPSEPARATEPROC)        getprocaddress("glStencilOpSeparate");
    glStencilFuncSeparate_ =      (PFNGLSTENCILFUNCSEPARATEPROC)      getprocaddress("glStencilFuncSeparate");
    glStencilMaskSeparate_ =      (PFNGLSTENCILMASKSEPARATEPROC)      getprocaddress("glStencilMaskSeparate");

    glGenBuffers_ =               (PFNGLGENBUFFERSPROC)               getprocaddress("glGenBuffers");
    glBindBuffer_ =               (PFNGLBINDBUFFERPROC)               getprocaddress("glBindBuffer");
    glMapBuffer_ =                (PFNGLMAPBUFFERPROC)                getprocaddress("glMapBuffer");
    glUnmapBuffer_ =              (PFNGLUNMAPBUFFERPROC)              getprocaddress("glUnmapBuffer");
    glBufferData_ =               (PFNGLBUFFERDATAPROC)               getprocaddress("glBufferData");
    glBufferSubData_ =            (PFNGLBUFFERSUBDATAPROC)            getprocaddress("glBufferSubData");
    glDeleteBuffers_ =            (PFNGLDELETEBUFFERSPROC)            getprocaddress("glDeleteBuffers");
    glGetBufferSubData_ =         (PFNGLGETBUFFERSUBDATAPROC)         getprocaddress("glGetBufferSubData");

    glGetQueryiv_ =               (PFNGLGETQUERYIVPROC)               getprocaddress("glGetQueryiv");
    glGenQueries_ =               (PFNGLGENQUERIESPROC)               getprocaddress("glGenQueries");
    glDeleteQueries_ =            (PFNGLDELETEQUERIESPROC)            getprocaddress("glDeleteQueries");
    glBeginQuery_ =               (PFNGLBEGINQUERYPROC)               getprocaddress("glBeginQuery");
    glEndQuery_ =                 (PFNGLENDQUERYPROC)                 getprocaddress("glEndQuery");
    glGetQueryObjectiv_ =         (PFNGLGETQUERYOBJECTIVPROC)         getprocaddress("glGetQueryObjectiv");
    glGetQueryObjectuiv_ =        (PFNGLGETQUERYOBJECTUIVPROC)        getprocaddress("glGetQueryObjectuiv");

    glCreateProgram_ =            (PFNGLCREATEPROGRAMPROC)            getprocaddress("glCreateProgram");
    glDeleteProgram_ =            (PFNGLDELETEPROGRAMPROC)            getprocaddress("glDeleteProgram");
    glUseProgram_ =               (PFNGLUSEPROGRAMPROC)               getprocaddress("glUseProgram");
    glCreateShader_ =             (PFNGLCREATESHADERPROC)             getprocaddress("glCreateShader");
    glDeleteShader_ =             (PFNGLDELETESHADERPROC)             getprocaddress("glDeleteShader");
    glShaderSource_ =             (PFNGLSHADERSOURCEPROC)             getprocaddress("glShaderSource");
    glCompileShader_ =            (PFNGLCOMPILESHADERPROC)            getprocaddress("glCompileShader");
    glGetShaderiv_ =              (PFNGLGETSHADERIVPROC)              getprocaddress("glGetShaderiv");
    glGetProgramiv_ =             (PFNGLGETPROGRAMIVPROC)             getprocaddress("glGetProgramiv");
    glAttachShader_ =             (PFNGLATTACHSHADERPROC)             getprocaddress("glAttachShader");
    glGetProgramInfoLog_ =        (PFNGLGETPROGRAMINFOLOGPROC)        getprocaddress("glGetProgramInfoLog");
    glGetShaderInfoLog_ =         (PFNGLGETSHADERINFOLOGPROC)         getprocaddress("glGetShaderInfoLog");
    glLinkProgram_ =              (PFNGLLINKPROGRAMPROC)              getprocaddress("glLinkProgram");
    glGetUniformLocation_ =       (PFNGLGETUNIFORMLOCATIONPROC)       getprocaddress("glGetUniformLocation");
    glUniform1f_ =                (PFNGLUNIFORM1FPROC)                getprocaddress("glUniform1f");
    glUniform2f_ =                (PFNGLUNIFORM2FPROC)                getprocaddress("glUniform2f");
    glUniform3f_ =                (PFNGLUNIFORM3FPROC)                getprocaddress("glUniform3f");
    glUniform4f_ =                (PFNGLUNIFORM4FPROC)                getprocaddress("glUniform4f");
    glUniform1fv_ =               (PFNGLUNIFORM1FVPROC)               getprocaddress("glUniform1fv");
    glUniform2fv_ =               (PFNGLUNIFORM2FVPROC)               getprocaddress("glUniform2fv");
    glUniform3fv_ =               (PFNGLUNIFORM3FVPROC)               getprocaddress("glUniform3fv");
    glUniform4fv_ =               (PFNGLUNIFORM4FVPROC)               getprocaddress("glUniform4fv");
    glUniform1i_ =                (PFNGLUNIFORM1IPROC)                getprocaddress("glUniform1i");
    glUniform2i_ =                (PFNGLUNIFORM2IPROC)                getprocaddress("glUniform2i");
    glUniform3i_ =                (PFNGLUNIFORM3IPROC)                getprocaddress("glUniform3i");
    glUniform4i_ =                (PFNGLUNIFORM4IPROC)                getprocaddress("glUniform4i");
    glUniform1iv_ =               (PFNGLUNIFORM1IVPROC)               getprocaddress("glUniform1iv");
    glUniform2iv_ =               (PFNGLUNIFORM2IVPROC)               getprocaddress("glUniform2iv");
    glUniform3iv_ =               (PFNGLUNIFORM3IVPROC)               getprocaddress("glUniform3iv");
    glUniform4iv_ =               (PFNGLUNIFORM4IVPROC)               getprocaddress("glUniform4iv");
    glUniformMatrix2fv_ =         (PFNGLUNIFORMMATRIX2FVPROC)         getprocaddress("glUniformMatrix2fv");
    glUniformMatrix3fv_ =         (PFNGLUNIFORMMATRIX3FVPROC)         getprocaddress("glUniformMatrix3fv");
    glUniformMatrix4fv_ =         (PFNGLUNIFORMMATRIX4FVPROC)         getprocaddress("glUniformMatrix4fv");
    glBindAttribLocation_ =       (PFNGLBINDATTRIBLOCATIONPROC)       getprocaddress("glBindAttribLocation");
    glGetActiveUniform_ =         (PFNGLGETACTIVEUNIFORMPROC)         getprocaddress("glGetActiveUniform");
    glEnableVertexAttribArray_ =  (PFNGLENABLEVERTEXATTRIBARRAYPROC)  getprocaddress("glEnableVertexAttribArray");
    glDisableVertexAttribArray_ = (PFNGLDISABLEVERTEXATTRIBARRAYPROC) getprocaddress("glDisableVertexAttribArray");

    glVertexAttrib1f_ =           (PFNGLVERTEXATTRIB1FPROC)           getprocaddress("glVertexAttrib1f");
    glVertexAttrib1fv_ =          (PFNGLVERTEXATTRIB1FVPROC)          getprocaddress("glVertexAttrib1fv");
    glVertexAttrib1s_ =           (PFNGLVERTEXATTRIB1SPROC)           getprocaddress("glVertexAttrib1s");
    glVertexAttrib1sv_ =          (PFNGLVERTEXATTRIB1SVPROC)          getprocaddress("glVertexAttrib1sv");
    glVertexAttrib2f_ =           (PFNGLVERTEXATTRIB2FPROC)           getprocaddress("glVertexAttrib2f");
    glVertexAttrib2fv_ =          (PFNGLVERTEXATTRIB2FVPROC)          getprocaddress("glVertexAttrib2fv");
    glVertexAttrib2s_ =           (PFNGLVERTEXATTRIB2SPROC)           getprocaddress("glVertexAttrib2s");
    glVertexAttrib2sv_ =          (PFNGLVERTEXATTRIB2SVPROC)          getprocaddress("glVertexAttrib2sv");
    glVertexAttrib3f_ =           (PFNGLVERTEXATTRIB3FPROC)           getprocaddress("glVertexAttrib3f");
    glVertexAttrib3fv_ =          (PFNGLVERTEXATTRIB3FVPROC)          getprocaddress("glVertexAttrib3fv");
    glVertexAttrib3s_ =           (PFNGLVERTEXATTRIB3SPROC)           getprocaddress("glVertexAttrib3s");
    glVertexAttrib3sv_ =          (PFNGLVERTEXATTRIB3SVPROC)          getprocaddress("glVertexAttrib3sv");
    glVertexAttrib4f_ =           (PFNGLVERTEXATTRIB4FPROC)           getprocaddress("glVertexAttrib4f");
    glVertexAttrib4fv_ =          (PFNGLVERTEXATTRIB4FVPROC)          getprocaddress("glVertexAttrib4fv");
    glVertexAttrib4s_ =           (PFNGLVERTEXATTRIB4SPROC)           getprocaddress("glVertexAttrib4s");
    glVertexAttrib4sv_ =          (PFNGLVERTEXATTRIB4SVPROC)          getprocaddress("glVertexAttrib4sv");
    glVertexAttrib4bv_ =          (PFNGLVERTEXATTRIB4BVPROC)          getprocaddress("glVertexAttrib4bv");
    glVertexAttrib4iv_ =          (PFNGLVERTEXATTRIB4IVPROC)          getprocaddress("glVertexAttrib4iv");
    glVertexAttrib4ubv_ =         (PFNGLVERTEXATTRIB4UBVPROC)         getprocaddress("glVertexAttrib4ubv");
    glVertexAttrib4uiv_ =         (PFNGLVERTEXATTRIB4UIVPROC)         getprocaddress("glVertexAttrib4uiv");
    glVertexAttrib4usv_ =         (PFNGLVERTEXATTRIB4USVPROC)         getprocaddress("glVertexAttrib4usv");
    glVertexAttrib4Nbv_ =         (PFNGLVERTEXATTRIB4NBVPROC)         getprocaddress("glVertexAttrib4Nbv");
    glVertexAttrib4Niv_ =         (PFNGLVERTEXATTRIB4NIVPROC)         getprocaddress("glVertexAttrib4Niv");
    glVertexAttrib4Nub_ =         (PFNGLVERTEXATTRIB4NUBPROC)         getprocaddress("glVertexAttrib4Nub");
    glVertexAttrib4Nubv_ =        (PFNGLVERTEXATTRIB4NUBVPROC)        getprocaddress("glVertexAttrib4Nubv");
    glVertexAttrib4Nuiv_ =        (PFNGLVERTEXATTRIB4NUIVPROC)        getprocaddress("glVertexAttrib4Nuiv");
    glVertexAttrib4Nusv_ =        (PFNGLVERTEXATTRIB4NUSVPROC)        getprocaddress("glVertexAttrib4Nusv");
    glVertexAttribPointer_ =      (PFNGLVERTEXATTRIBPOINTERPROC)      getprocaddress("glVertexAttribPointer");

    glDrawBuffers_ =              (PFNGLDRAWBUFFERSPROC)              getprocaddress("glDrawBuffers");
#endif

    if(glversion >= 300)
    {
        glGetStringi_ =            (PFNGLGETSTRINGIPROC)          getprocaddress("glGetStringi");
        glBindFragDataLocation_ =  (PFNGLBINDFRAGDATALOCATIONPROC)getprocaddress("glBindFragDataLocation");
    }

    const char *glslstr = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    uint glslmajorversion, glslminorversion;
    if(glslstr && sscanf(glslstr, " %u.%u", &glslmajorversion, &glslminorversion) == 2) glslversion = glslmajorversion*100 + glslminorversion;

    if(glslversion < 120) fatal("GLSL 1.20 or greater is required!");

    parseglexts();

    GLint val;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &val);
    hwtexsize = val;
    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &val);
    hwcubetexsize = val;

    if(glversion >= 300 || hasext("GL_ARB_texture_float") || hasext("GL_ATI_texture_float"))
    {
        hasTF = true;
        if(glversion < 300 && dbgexts) conoutf(CON_INIT, "Using GL_ARB_texture_float extension.");
        shadowmap = 1;
        extern int smoothshadowmappeel;
        smoothshadowmappeel = 1;
    }

    if(glversion >= 300 || hasext("GL_ARB_texture_rg"))
    {
        hasTRG = true;
        if(glversion < 300 && dbgexts) conoutf(CON_INIT, "Using GL_ARB_texture_rg extension.");
    }

    if(glversion >= 300 || hasext("GL_ARB_framebuffer_object"))
    {
        glBindRenderbuffer_        = (PFNGLBINDRENDERBUFFERPROC)       getprocaddress("glBindRenderbuffer");
        glDeleteRenderbuffers_     = (PFNGLDELETERENDERBUFFERSPROC)    getprocaddress("glDeleteRenderbuffers");
        glGenRenderbuffers_        = (PFNGLGENFRAMEBUFFERSPROC)        getprocaddress("glGenRenderbuffers");
        glRenderbufferStorage_     = (PFNGLRENDERBUFFERSTORAGEPROC)    getprocaddress("glRenderbufferStorage");
        glCheckFramebufferStatus_  = (PFNGLCHECKFRAMEBUFFERSTATUSPROC) getprocaddress("glCheckFramebufferStatus");
        glBindFramebuffer_         = (PFNGLBINDFRAMEBUFFERPROC)        getprocaddress("glBindFramebuffer");
        glDeleteFramebuffers_      = (PFNGLDELETEFRAMEBUFFERSPROC)     getprocaddress("glDeleteFramebuffers");
        glGenFramebuffers_         = (PFNGLGENFRAMEBUFFERSPROC)        getprocaddress("glGenFramebuffers");
        glFramebufferTexture2D_    = (PFNGLFRAMEBUFFERTEXTURE2DPROC)   getprocaddress("glFramebufferTexture2D");
        glFramebufferRenderbuffer_ = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)getprocaddress("glFramebufferRenderbuffer");
        glGenerateMipmap_          = (PFNGLGENERATEMIPMAPPROC)         getprocaddress("glGenerateMipmap");
        glBlitFramebuffer_         = (PFNGLBLITFRAMEBUFFERPROC)        getprocaddress("glBlitFramebuffer");
        hasAFBO = hasFBO = hasFBB = hasDS = true;
        if(glversion < 300 && dbgexts) conoutf(CON_INIT, "Using GL_ARB_framebuffer_object extension.");
    }
    else if(hasext("GL_EXT_framebuffer_object"))
    {
        glBindRenderbuffer_        = (PFNGLBINDRENDERBUFFERPROC)       getprocaddress("glBindRenderbufferEXT");
        glDeleteRenderbuffers_     = (PFNGLDELETERENDERBUFFERSPROC)    getprocaddress("glDeleteRenderbuffersEXT");
        glGenRenderbuffers_        = (PFNGLGENFRAMEBUFFERSPROC)        getprocaddress("glGenRenderbuffersEXT");
        glRenderbufferStorage_     = (PFNGLRENDERBUFFERSTORAGEPROC)    getprocaddress("glRenderbufferStorageEXT");
        glCheckFramebufferStatus_  = (PFNGLCHECKFRAMEBUFFERSTATUSPROC) getprocaddress("glCheckFramebufferStatusEXT");
        glBindFramebuffer_         = (PFNGLBINDFRAMEBUFFERPROC)        getprocaddress("glBindFramebufferEXT");
        glDeleteFramebuffers_      = (PFNGLDELETEFRAMEBUFFERSPROC)     getprocaddress("glDeleteFramebuffersEXT");
        glGenFramebuffers_         = (PFNGLGENFRAMEBUFFERSPROC)        getprocaddress("glGenFramebuffersEXT");
        glFramebufferTexture2D_    = (PFNGLFRAMEBUFFERTEXTURE2DPROC)   getprocaddress("glFramebufferTexture2DEXT");
        glFramebufferRenderbuffer_ = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)getprocaddress("glFramebufferRenderbufferEXT");
        glGenerateMipmap_          = (PFNGLGENERATEMIPMAPPROC)         getprocaddress("glGenerateMipmapEXT");
        hasFBO = true;
        if(dbgexts) conoutf(CON_INIT, "Using GL_EXT_framebuffer_object extension.");

        if(hasext("GL_EXT_framebuffer_blit"))
        {
            glBlitFramebuffer_     = (PFNGLBLITFRAMEBUFFERPROC)        getprocaddress("glBlitFramebufferEXT");
            hasFBB = true;
            if(dbgexts) conoutf(CON_INIT, "Using GL_EXT_framebuffer_blit extension.");
        }

        if(hasext("GL_EXT_packed_depth_stencil") || hasext("GL_NV_packed_depth_stencil"))
        {
            hasDS = true;
            if(dbgexts) conoutf(CON_INIT, "Using GL_EXT_packed_depth_stencil extension.");
        }
    }
    else fatal("Framebuffer object support is required!");

    extern int fpdepthfx;
    if(ati)
    {
        //conoutf(CON_WARN, "WARNING: ATI cards may show garbage in skybox. (use \"/ati_skybox_bug 1\" to fix)");

        minimizetcusage = 1;
        if(hasTF && hasTRG) fpdepthfx = 1;
        // On Catalyst 10.2, issuing an occlusion query on the first draw using a given cubemap texture causes a nasty crash
        ati_cubemap_bug = 1;
    }
    else if(nvidia)
    {
        reservevpparams = 10;
        rtsharefb = 0; // work-around for strange driver stalls involving when using many FBOs
        extern int filltjoints;
        if(glversion < 300 && !hasext("GL_EXT_gpu_shader4")) filltjoints = 0; // DX9 or less NV cards seem to not cause many sparklies

        if(hasTF && hasTRG) fpdepthfx = 1;
    }
    else
    {
        if(intel)
        {
#ifdef WIN32
            intel_vertexarray_bug = 1;
            // MapBufferRange is buggy on older Intel drivers on Windows
            if(glversion <= 310) intel_mapbufferrange_bug = 1;
#endif
        }

        reservevpparams = 20;

        if(mesa) mesa_swap_bug = 1;
    }

    if(glversion >= 300 || hasext("GL_ARB_map_buffer_range"))
    {
        glMapBufferRange_         = (PFNGLMAPBUFFERRANGEPROC)        getprocaddress("glMapBufferRange");
        glFlushMappedBufferRange_ = (PFNGLFLUSHMAPPEDBUFFERRANGEPROC)getprocaddress("glFlushMappedBufferRange");
        hasMBR = true;
        if(glversion < 300 && dbgexts) conoutf(CON_INIT, "Using GL_ARB_map_buffer_range.");
    }

    if(glversion >= 310 || hasext("GL_ARB_uniform_buffer_object"))
    {
        glGetUniformIndices_       = (PFNGLGETUNIFORMINDICESPROC)      getprocaddress("glGetUniformIndices");
        glGetActiveUniformsiv_     = (PFNGLGETACTIVEUNIFORMSIVPROC)    getprocaddress("glGetActiveUniformsiv");
        glGetUniformBlockIndex_    = (PFNGLGETUNIFORMBLOCKINDEXPROC)   getprocaddress("glGetUniformBlockIndex");
        glGetActiveUniformBlockiv_ = (PFNGLGETACTIVEUNIFORMBLOCKIVPROC)getprocaddress("glGetActiveUniformBlockiv");
        glUniformBlockBinding_     = (PFNGLUNIFORMBLOCKBINDINGPROC)    getprocaddress("glUniformBlockBinding");
        glBindBufferBase_          = (PFNGLBINDBUFFERBASEPROC)         getprocaddress("glBindBufferBase");
        glBindBufferRange_         = (PFNGLBINDBUFFERRANGEPROC)        getprocaddress("glBindBufferRange");

        useubo = 1;
        hasUBO = true;
        if(glversion < 310 && dbgexts) conoutf(CON_INIT, "Using GL_ARB_uniform_buffer_object extension.");
    }

    if(glversion >= 300 || hasext("GL_ARB_vertex_array_object"))
    {
        glBindVertexArray_ =    (PFNGLBINDVERTEXARRAYPROC)   getprocaddress("glBindVertexArray");
        glDeleteVertexArrays_ = (PFNGLDELETEVERTEXARRAYSPROC)getprocaddress("glDeleteVertexArrays");
        glGenVertexArrays_ =    (PFNGLGENVERTEXARRAYSPROC)   getprocaddress("glGenVertexArrays");
        glIsVertexArray_ =      (PFNGLISVERTEXARRAYPROC)     getprocaddress("glIsVertexArray");
        hasVAO = true;
        if(glversion < 300 && dbgexts) conoutf(CON_INIT, "Using GL_ARB_vertex_array_object extension.");
    }
    else if(hasext("GL_APPLE_vertex_array_object"))
    {
        glBindVertexArray_ =    (PFNGLBINDVERTEXARRAYPROC)   getprocaddress("glBindVertexArrayAPPLE");
        glDeleteVertexArrays_ = (PFNGLDELETEVERTEXARRAYSPROC)getprocaddress("glDeleteVertexArraysAPPLE");
        glGenVertexArrays_ =    (PFNGLGENVERTEXARRAYSPROC)   getprocaddress("glGenVertexArraysAPPLE");
        glIsVertexArray_ =      (PFNGLISVERTEXARRAYPROC)     getprocaddress("glIsVertexArrayAPPLE");
        hasVAO = true;
        if(dbgexts) conoutf(CON_INIT, "Using GL_APPLE_vertex_array_object extension.");
    }

    if(glversion >= 330 || hasext("GL_ARB_texture_swizzle") || hasext("GL_EXT_texture_swizzle"))
    {
        hasTSW = true;
        if(glversion < 330 && dbgexts) conoutf(CON_INIT, "Using GL_ARB_texture_swizzle extension.");
    }

    if(hasext("GL_EXT_texture_compression_s3tc"))
    {
        hasS3TC = true;
#ifdef __APPLE__
        usetexcompress = 1;
#else
        if(!mesa) usetexcompress = 2;
#endif
        if(dbgexts) conoutf(CON_INIT, "Using GL_EXT_texture_compression_s3tc extension.");
    }
    else if(hasext("GL_EXT_texture_compression_dxt1") && hasext("GL_ANGLE_texture_compression_dxt3") && hasext("GL_ANGLE_texture_compression_dxt5"))
    {
        hasS3TC = true;
        if(dbgexts) conoutf(CON_INIT, "Using GL_EXT_texture_compression_dxt1 extension.");
    }
    if(hasext("GL_3DFX_texture_compression_FXT1"))
    {
        hasFXT1 = true;
        if(mesa) usetexcompress = max(usetexcompress, 1);
        if(dbgexts) conoutf(CON_INIT, "Using GL_3DFX_texture_compression_FXT1.");
    }
    if(hasext("GL_EXT_texture_compression_latc"))
    {
        hasLATC = true;
        if(dbgexts) conoutf(CON_INIT, "Using GL_EXT_texture_compression_latc extension.");
    }
    if(glversion >= 300 || hasext("GL_ARB_texture_compression_rgtc") || hasext("GL_EXT_texture_compression_rgtc"))
    {
        hasRGTC = true;
        if(glversion < 300 && dbgexts) conoutf(CON_INIT, "Using GL_ARB_texture_compression_rgtc extension.");
    }

    if(hasext("GL_EXT_texture_filter_anisotropic"))
    {
        GLint val;
        glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &val);
        hwmaxaniso = val;
        hasAF = true;
        if(dbgexts) conoutf(CON_INIT, "Using GL_EXT_texture_filter_anisotropic extension.");
    }

    if(glversion >= 300 || hasext("GL_EXT_gpu_shader4"))
    {
        // on DX10 or above class cards (i.e. GF8 or RadeonHD) enable expensive features
        extern int grass, glare, maxdynlights, depthfxsize, blurdepthfx, texcompress;
        grass = 1;
        waterfallrefract = 1;
        glare = 1;
        maxdynlights = MAXDYNLIGHTS;
        depthfxsize = 10;
        blurdepthfx = 0;
        texcompress = max(texcompress, 1024 + 1);
    }
}

void glext(char *ext)
{
    intret(hasext(ext) ? 1 : 0);
}
COMMAND(glext, "s");

void gl_resize()
{
    glViewport(0, 0, screenw, screenh);
}

void gl_init()
{
    glClearColor(0, 0, 0, 0);
    glClearDepth(1);
    glDepthFunc(GL_LESS);
    glDisable(GL_DEPTH_TEST);
    
    glEnable(GL_LINE_SMOOTH);
    //glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    glFrontFace(GL_CW);
    glCullFace(GL_BACK);
    glDisable(GL_CULL_FACE);

    gle::setup();

    setupshaders();

    setuptexcompress();

    gl_resize();

    hwrtinit();
    hdrout_start();
}

VAR(wireframe, 0, 0, 1);

ICOMMAND(getcamyaw, "", (), floatret(camera1->yaw));
ICOMMAND(getcampitch, "", (), floatret(camera1->pitch));
ICOMMAND(getcamroll, "", (), floatret(camera1->roll));
ICOMMAND(getcampos, "", (), 
{
    defformatstring(pos, "%s %s %s", floatstr(camera1->o.x), floatstr(camera1->o.y), floatstr(camera1->o.z));
    result(pos);
});

vec worldpos, camdir, camright, camup;

void setcammatrix()
{
    // move from RH to Z-up LH quake style worldspace
    cammatrix = viewmatrix;
    cammatrix.rotate_around_y(camera1->roll*RAD);
    cammatrix.rotate_around_x(camera1->pitch*-RAD);
    cammatrix.rotate_around_z(camera1->yaw*-RAD);
    cammatrix.translate(vec(camera1->o).neg());

    cammatrix.transposedtransformnormal(vec(viewmatrix.b), camdir);
    cammatrix.transposedtransformnormal(vec(viewmatrix.a).neg(), camright);
    cammatrix.transposedtransformnormal(vec(viewmatrix.c), camup);

    if(!drawtex)
    {
        if(raycubepos(camera1->o, camdir, worldpos, 0, RAY_CLIPMAT|RAY_SKIPFIRST) == -1)
            worldpos = vec(camdir).mul(2*worldsize).add(camera1->o); // if nothing is hit, just far away in the view direction
    }
}

void setcamprojmatrix(bool init = true, bool flush = false)
{
    if(init)
    {   
        setcammatrix();
    }

    camprojmatrix.muld(projmatrix, cammatrix);

    if(init)
    {
        invcammatrix.invert(cammatrix);
        invcamprojmatrix.invert(camprojmatrix);
    }

    GLOBALPARAM(camprojmatrix, camprojmatrix);

    if(fogging)
    {
        vec fogplane(cammatrix.c);
        fogplane.x /= projmatrix.a.x;
        fogplane.y /= projmatrix.b.y;
        fogplane.z /= projmatrix.c.w;
        GLOBALPARAMF(fogplane, fogplane.x, fogplane.y, 0, fogplane.z);               
    }
    else
    {
        vec2 lineardepthscale = projmatrix.lineardepthscale();
        GLOBALPARAMF(fogplane, 0, 0, lineardepthscale.x, lineardepthscale.y);
    }

    if(flush && Shader::lastshader) Shader::lastshader->flushparams();
}

matrix4 hudmatrix, hudmatrixstack[64];
int hudmatrixpos = 0;

void resethudmatrix()
{
    hudmatrixpos = 0;
    GLOBALPARAM(hudmatrix, hudmatrix);
}

void pushhudmatrix()
{
    if(hudmatrixpos >= 0 && hudmatrixpos < int(sizeof(hudmatrixstack)/sizeof(hudmatrixstack[0]))) hudmatrixstack[hudmatrixpos] = hudmatrix;
    ++hudmatrixpos;
}

void flushhudmatrix(bool flushparams)
{
    GLOBALPARAM(hudmatrix, hudmatrix);
    if(flushparams && Shader::lastshader) Shader::lastshader->flushparams();
}

void pophudmatrix(bool flush, bool flushparams)
{
    --hudmatrixpos;
    if(hudmatrixpos >= 0 && hudmatrixpos < int(sizeof(hudmatrixstack)/sizeof(hudmatrixstack[0])))
    {
        hudmatrix = hudmatrixstack[hudmatrixpos];
        if(flush) flushhudmatrix(flushparams);
    }
}

void pushhudscale(float sx, float sy)
{
    if(!sy) sy = sx;
    pushhudmatrix();
    hudmatrix.scale(sx, sy, 1);
    flushhudmatrix();
}

void pushhudtranslate(float tx, float ty, float sx, float sy)
{
    if(!sy) sy = sx;
    pushhudmatrix();
    hudmatrix.translate(tx, ty, 0);
    if(sy) hudmatrix.scale(sx, sy, 1);
    flushhudmatrix();
}

float curfov = 100, curavatarfov = 65, fovy, aspect;
int farplane;
VARP(zoominvel, 0, 250, 5000);
VARP(zoomoutvel, 0, 100, 5000);
VARP(zoomfov, 10, 35, 60);
VARP(fov, 10, 100, 150);
VAR(avatarzoomfov, 10, 25, 60);
VAR(avatarfov, 10, 65, 150);
FVAR(avatardepth, 0, 0.5f, 1);
FVARNP(aspect, forceaspect, 0, 0, 1e3f);

static float zoomprogress = 0;
VAR(zoom, -1, 0, 1);

void disablezoom()
{
    zoom = 0;
    zoomprogress = 0;
}

float getzoomprogress() { return zoomprogress; }

void forcezoom(float progress)
{
    progress = clamp(progress, 0.0f, 1.0f);
    if(progress <= 0.001f)
    {
        zoom = 0;
        zoomprogress = 0;
        curfov = fov;
        curavatarfov = avatarfov;
        return;
    }
    zoom = 1;
    zoomprogress = progress;
    curfov = zoomfov*zoomprogress + fov*(1 - zoomprogress);
    curavatarfov = avatarzoomfov*zoomprogress + avatarfov*(1 - zoomprogress);
}

void computezoom()
{
    if(!zoom) { zoomprogress = 0; curfov = fov; curavatarfov = avatarfov; return; }
    if(zoom > 0) zoomprogress = zoominvel ? min(zoomprogress + float(elapsedtime) / zoominvel, 1.0f) : 1;
    else
    {
        zoomprogress = zoomoutvel ? max(zoomprogress - float(elapsedtime) / zoomoutvel, 0.0f) : 0;
        if(zoomprogress <= 0) zoom = 0;
    }
    curfov = zoomfov*zoomprogress + fov*(1 - zoomprogress);
    curavatarfov = avatarzoomfov*zoomprogress + avatarfov*(1 - zoomprogress);
}

FVARP(zoomsens, 1e-3f, 1, 1000);
FVARP(zoomaccel, 0, 0, 1000);
VARP(zoomautosens, 0, 1, 1);
FVARP(sensitivity, 1e-3f, 3, 1000);
FVARP(sensitivityscale, 1e-3f, 1, 1000);
VARP(mousedpi, 50, 800, 32000);
VARP(invmouse, 0, 0, 1);
FVARP(mouseaccel, 0, 0, 1000);
 
VAR(thirdperson, 0, 0, 2);
FVAR(thirdpersondistance, 0, 20, 50);
FVAR(thirdpersonup, -25, 0, 25);
FVAR(thirdpersonside, -25, 0, 25);
physent *camera1 = NULL;
bool detachedcamera = false;
bool isthirdperson() { return player!=camera1 || detachedcamera || reflecting; }

void fixcamerarange()
{
    const float MAXPITCH = 90.0f;
    if(camera1->pitch>MAXPITCH) camera1->pitch = MAXPITCH;
    if(camera1->pitch<-MAXPITCH) camera1->pitch = -MAXPITCH;
    while(camera1->yaw<0.0f) camera1->yaw += 360.0f;
    while(camera1->yaw>=360.0f) camera1->yaw -= 360.0f;
}

void mousemove(int dx, int dy)
{
    if(!game::allowmouselook()) return;
    float cursens = sensitivity, curaccel = mouseaccel;
    if(zoom)
    {
        if(zoomautosens) 
        {
            cursens = float(sensitivity*zoomfov)/fov;
            curaccel = float(mouseaccel*zoomfov)/fov;
        }
        else 
        {
            cursens = zoomsens;
            curaccel = zoomaccel;
        }
    }
    if(curaccel && curtime && (dx || dy)) cursens += curaccel * sqrtf(dx*dx + dy*dy)/curtime;
    cursens /= 33.0f*sensitivityscale;
    camera1->yaw += dx*cursens;
    camera1->pitch -= dy*cursens*(invmouse ? -1 : 1);
    fixcamerarange();
    if(camera1!=player && !detachedcamera)
    {
        player->yaw = camera1->yaw;
        player->pitch = camera1->pitch;
    }
}

// Hipfire cm/360. Must match the un-zoomed branch of mousemove():
//   yaw += dx * sensitivity / (33 * sensitivityscale)
// Hipfire does NOT scale with fov. Zoom (zoomautosens / zoomsens) is ignored here.
static int hipfiredpi() { return mousedpi > 0 ? mousedpi : 800; }

static float parsedecimal(const char *s)
{
    string buf;
    copystring(buf, s ? s : "");
    for(char *c = buf; *c; c++) if(*c == ',') *c = '.';
    return parsefloat(buf);
}

static float hipfirecounts360()
{
    float s = sensitivity;
    if(s < 1e-6f) s = 1e-6f;
    return 360.0f * 33.0f * sensitivityscale / s;
}

static float hipfirecm360()
{
    return hipfirecounts360() / float(hipfiredpi()) * 2.54f;
}

static float hipfirein360()
{
    return hipfirecounts360() / float(hipfiredpi());
}

static void applycm360(float cm)
{
    if(!(cm > 0)) return;
    if(fabs(cm - hipfirecm360()) < 0.005f) return;
    setfvar("sensitivity", 360.0f * 33.0f * 2.54f * sensitivityscale / (cm * float(hipfiredpi())));
    conoutf("hipfire %.2f cm/360  (sensitivity %s)", hipfirecm360(), floatstr(sensitivity));
}

ICOMMAND(getcm360, "", (),
{
    defformatstring(buf, "%.2f", hipfirecm360());
    result(buf);
});

ICOMMAND(getin360, "", (),
{
    defformatstring(buf, "%.2f", hipfirein360());
    result(buf);
});

ICOMMAND(getcm360text, "", (),
{
    defformatstring(buf, "hipfire: %.2f cm/360  /  %.2f in/360  (not FOV-scaled)", hipfirecm360(), hipfirein360());
    result(buf);
});

ICOMMAND(setcm360, "s", (char *s), applycm360(parsedecimal(s)));

void recomputecamera()
{
    game::setupcamera();
    computezoom();
    game::demohdapplyzoom();

    bool allowthirdperson = game::allowthirdperson();
    bool shoulddetach = (allowthirdperson && thirdperson > 1) || game::detachcamera();
    if((!allowthirdperson || !thirdperson) && !shoulddetach)
    {
        camera1 = player;
        detachedcamera = false;
    }
    else
    {
        static physent tempcamera;
        camera1 = &tempcamera;
        if(detachedcamera && shoulddetach) camera1->o = player->o;
        else
        {
            *camera1 = *player;
            detachedcamera = shoulddetach;
        }
        camera1->reset();
        camera1->type = ENT_CAMERA;
        camera1->move = -1;
        camera1->eyeheight = camera1->aboveeye = camera1->radius = camera1->xradius = camera1->yradius = 2;
       
        matrix3 orient;
        orient.identity();
        orient.rotate_around_z(camera1->yaw*RAD);
        orient.rotate_around_x(camera1->pitch*RAD);
        orient.rotate_around_y(camera1->roll*-RAD);
        vec dir = vec(orient.b).neg(), side = vec(orient.a).neg(), up = orient.c;

        if(game::collidecamera()) 
        {
            movecamera(camera1, dir, thirdpersondistance, 1);
            movecamera(camera1, dir, clamp(thirdpersondistance - camera1->o.dist(player->o), 0.0f, 1.0f), 0.1f);
            if(thirdpersonup)
            {
                vec pos = camera1->o;
                float dist = fabs(thirdpersonup);
                if(thirdpersonup < 0) up.neg();
                movecamera(camera1, up, dist, 1);
                movecamera(camera1, up, clamp(dist - camera1->o.dist(pos), 0.0f, 1.0f), 0.1f);
            }
            if(thirdpersonside)
            {
                vec pos = camera1->o;
                float dist = fabs(thirdpersonside);
                if(thirdpersonside < 0) side.neg();
                movecamera(camera1, side, dist, 1);
                movecamera(camera1, side, clamp(dist - camera1->o.dist(pos), 0.0f, 1.0f), 0.1f);
            }
        }
        else 
        {
            camera1->o.add(vec(dir).mul(thirdpersondistance));
            if(thirdpersonup) camera1->o.add(vec(up).mul(thirdpersonup));
            if(thirdpersonside) camera1->o.add(vec(side).mul(thirdpersonside));
        }
    }

    setviewcell(camera1->o);
    game::demohdsample();
}

extern const matrix4 viewmatrix(vec(-1, 0, 0), vec(0, 0, 1), vec(0, -1, 0));
matrix4 cammatrix, projmatrix, camprojmatrix, invcammatrix, invcamprojmatrix, lastcamprojmatrix;

FVAR(nearplane, 0.01f, 0.54f, 2.0f);

vec calcavatarpos(const vec &pos, float dist)
{
    vec eyepos;
    cammatrix.transform(pos, eyepos);
    GLdouble ydist = nearplane * tan(curavatarfov/2*RAD), xdist = ydist * aspect;
    vec4 scrpos;
    scrpos.x = eyepos.x*nearplane/xdist;
    scrpos.y = eyepos.y*nearplane/ydist;
    scrpos.z = (eyepos.z*(farplane + nearplane) - 2*nearplane*farplane) / (farplane - nearplane);
    scrpos.w = -eyepos.z;

    vec worldpos = invcamprojmatrix.perspectivetransform(scrpos);
    vec dir = vec(worldpos).sub(camera1->o).rescale(dist);
    return dir.add(camera1->o);
}

VAR(reflectclip, 0, 6, 64);
VAR(reflectclipavatar, -64, 0, 64);

matrix4 clipmatrix, noclipmatrix;

void renderavatar()
{
    if(isthirdperson()) return;

    hwrtvelbeginavatar();
    hwrtvelbegincolourhud();
    game::renderavatar();
    hwrtvelendcolourhud();
    hwrtvelendavatar();
}

static matrix4 velavataroldproj;

void hwrtvelbeginavatar()
{
    velavataroldproj = projmatrix;
    projmatrix.perspective(curavatarfov, aspect, nearplane, farplane);
    projmatrix.scalez(avatardepth);
    hwrttemporalbeginavatarproj();
    hwrttemporalapplystored(projmatrix);
    setcamprojmatrix(false);
    hwrttemporalnoteavatar();
}

void hwrtvelendavatar()
{
    projmatrix = velavataroldproj;
    setcamprojmatrix(false);
}

void hwrtveldrawavatar()
{
    game::renderavatar();
}

FVAR(polygonoffsetfactor, -1e4f, -3.0f, 1e4f);
FVAR(polygonoffsetunits, -1e4f, -3.0f, 1e4f);
FVAR(depthoffset, -1e4f, 0.01f, 1e4f);

matrix4 nooffsetmatrix;

void enablepolygonoffset(GLenum type)
{
    if(!depthoffset)
    {
        glPolygonOffset(polygonoffsetfactor, polygonoffsetunits);
        glEnable(type);
        return;
    }
    
    bool clipped = reflectz < 1e15f && reflectclip;

    nooffsetmatrix = projmatrix;
    projmatrix.d.z += depthoffset * (clipped ? noclipmatrix.c.z : projmatrix.c.z);
    setcamprojmatrix(false, true);
}

void disablepolygonoffset(GLenum type)
{
    if(!depthoffset)
    {
        glDisable(type);
        return;
    }
    
    projmatrix = nooffsetmatrix;
    setcamprojmatrix(false, true);
}

void calcspherescissor(const vec &center, float size, float &sx1, float &sy1, float &sx2, float &sy2)
{
    vec worldpos(center), e;
    if(reflecting) worldpos.z = 2*reflectz - worldpos.z; 
    cammatrix.transform(worldpos, e); 
    if(e.z > 2*size) { sx1 = sy1 = 1; sx2 = sy2 = -1; return; }
    float zzrr = e.z*e.z - size*size,
          dx = e.x*e.x + zzrr, dy = e.y*e.y + zzrr,
          focaldist = 1.0f/tan(fovy*0.5f*RAD);
    sx1 = sy1 = -1;
    sx2 = sy2 = 1;
    #define CHECKPLANE(c, dir, focaldist, low, high) \
    do { \
        float nzc = (cz*cz + 1) / (cz dir drt) - cz, \
              pz = (d##c)/(nzc*e.c - e.z); \
        if(pz > 0) \
        { \
            float c = (focaldist)*nzc, \
                  pc = pz*nzc; \
            if(pc < e.c) low = c; \
            else if(pc > e.c) high = c; \
        } \
    } while(0)
    if(dx > 0)
    {
        float cz = e.x/e.z, drt = sqrtf(dx)/size;
        CHECKPLANE(x, -, focaldist/aspect, sx1, sx2);
        CHECKPLANE(x, +, focaldist/aspect, sx1, sx2);
    }
    if(dy > 0)
    {
        float cz = e.y/e.z, drt = sqrtf(dy)/size;
        CHECKPLANE(y, -, focaldist, sy1, sy2);
        CHECKPLANE(y, +, focaldist, sy1, sy2);
    }
}

static int scissoring = 0;
static GLint oldscissor[4];

int pushscissor(float sx1, float sy1, float sx2, float sy2)
{
    scissoring = 0;

    if(sx1 <= -1 && sy1 <= -1 && sx2 >= 1 && sy2 >= 1) return 0;

    sx1 = max(sx1, -1.0f);
    sy1 = max(sy1, -1.0f);
    sx2 = min(sx2, 1.0f);
    sy2 = min(sy2, 1.0f);

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    int sx = viewport[0] + int(floor((sx1+1)*0.5f*viewport[2])),
        sy = viewport[1] + int(floor((sy1+1)*0.5f*viewport[3])),
        sw = viewport[0] + int(ceil((sx2+1)*0.5f*viewport[2])) - sx,
        sh = viewport[1] + int(ceil((sy2+1)*0.5f*viewport[3])) - sy;
    if(sw <= 0 || sh <= 0) return 0;

    if(glIsEnabled(GL_SCISSOR_TEST))
    {
        glGetIntegerv(GL_SCISSOR_BOX, oldscissor);
        sw += sx;
        sh += sy;
        sx = max(sx, int(oldscissor[0]));
        sy = max(sy, int(oldscissor[1]));
        sw = min(sw, int(oldscissor[0] + oldscissor[2])) - sx;
        sh = min(sh, int(oldscissor[1] + oldscissor[3])) - sy;
        if(sw <= 0 || sh <= 0) return 0;
        scissoring = 2;
    }
    else scissoring = 1;

    glScissor(sx, sy, sw, sh);
    if(scissoring<=1) glEnable(GL_SCISSOR_TEST);
    
    return scissoring;
}

void popscissor()
{
    if(scissoring>1) glScissor(oldscissor[0], oldscissor[1], oldscissor[2], oldscissor[3]);
    else if(scissoring) glDisable(GL_SCISSOR_TEST);
    scissoring = 0;
}

static GLuint screenquadvbo = 0;

static void setupscreenquad()
{
    if(!screenquadvbo)
    {
        glGenBuffers_(1, &screenquadvbo);
        gle::bindvbo(screenquadvbo);
        vec2 verts[4] = { vec2(1, -1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1) };
        glBufferData_(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
        gle::clearvbo();
    }
}

static void cleanupscreenquad()
{
    if(screenquadvbo) { glDeleteBuffers_(1, &screenquadvbo); screenquadvbo = 0; }
}

void screenquad()
{
    setupscreenquad();
    gle::bindvbo(screenquadvbo);
    gle::enablevertex();
    gle::vertexpointer(sizeof(vec2), (const vec2 *)0, GL_FLOAT, 2);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gle::disablevertex();
    gle::clearvbo();
}

static LocalShaderParam screentexcoord[2] = { LocalShaderParam("screentexcoord0"), LocalShaderParam("screentexcoord1") };

static inline void setscreentexcoord(int i, float w, float h, float x = 0, float y = 0)
{
    screentexcoord[i].setf(w*0.5f, h*0.5f, x + w*0.5f, y + fabs(h)*0.5f);
}

void screenquad(float sw, float sh)
{
    setscreentexcoord(0, sw, sh);
    screenquad();
}

void screenquadflipped(float sw, float sh)
{
    setscreentexcoord(0, sw, -sh);
    screenquad();
}

void screenquad(float sw, float sh, float sw2, float sh2)
{
    setscreentexcoord(0, sw, sh);
    setscreentexcoord(1, sw2, sh2);
    screenquad();
}

void screenquadoffset(float x, float y, float w, float h)
{
    setscreentexcoord(0, w, h, x, y);
    screenquad();
}

void screenquadoffset(float x, float y, float w, float h, float x2, float y2, float w2, float h2)
{
    setscreentexcoord(0, w, h, x, y);
    setscreentexcoord(1, w2, h2, x2, y2);
    screenquad();
}

#define HUDQUAD(x1, y1, x2, y2, sx1, sy1, sx2, sy2) { \
    gle::defvertex(2); \
    gle::deftexcoord0(); \
    gle::begin(GL_TRIANGLE_STRIP); \
    gle::attribf(x2, y1); gle::attribf(sx2, sy1); \
    gle::attribf(x1, y1); gle::attribf(sx1, sy1); \
    gle::attribf(x2, y2); gle::attribf(sx2, sy2); \
    gle::attribf(x1, y2); gle::attribf(sx1, sy2); \
    gle::end(); \
}

void hudquad(float x, float y, float w, float h, float tx, float ty, float tw, float th)
{
    HUDQUAD(x, y, x+w, y+h, tx, ty, tx+tw, ty+th);
}

void hudrect(float x, float y, float w, float h, int color, int alpha)
{
    hudnotextureshader->set();
    gle::colorub((color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, uchar(clamp(alpha, 0, 255)));
    hudquad(x, y, w, h);
    hudshader->set();
}

VARR(fog, 16, 4000, 1000024);
bvec fogcolor(0x80, 0x99, 0xB3);
HVARFR(fogcolour, 0, 0x8099B3, 0xFFFFFF,
{
    fogcolor = bvec((fogcolour>>16)&0xFF, (fogcolour>>8)&0xFF, fogcolour&0xFF);
});

static float findsurface(int fogmat, const vec &v, int &abovemat)
{
    fogmat &= MATF_VOLUME;
    ivec o(v), co;
    int csize;
    do
    {
        cube &c = lookupcube(o, 0, co, csize);
        int mat = c.material&MATF_VOLUME;
        if(mat != fogmat)
        {
            abovemat = isliquid(mat) ? c.material : MAT_AIR;
            return o.z;
        }
        o.z = co.z + csize;
    }
    while(o.z < worldsize);
    abovemat = MAT_AIR;
    return worldsize;
}

static void blendfog(int fogmat, float blend, float logblend, float &start, float &end, vec &fogc)
{
    switch(fogmat&MATF_VOLUME)
    {
        case MAT_WATER:
        {
            const bvec &wcol = getwatercolor(fogmat);
            int wfog = getwaterfog(fogmat);
            fogc.madd(wcol.tocolor(), blend);
            end += logblend*min(fog, max(wfog*4, 32));
            break;
        }

        case MAT_LAVA:
        {
            const bvec &lcol = getlavacolor(fogmat);
            int lfog = getlavafog(fogmat);
            fogc.madd(lcol.tocolor(), blend);
            end += logblend*min(fog, max(lfog*4, 32));
            break;
        }

        default:
            fogc.madd(fogcolor.tocolor(), blend);
            start += logblend*(fog+64)/8;
            end += logblend*fog;
            break;
    }
}

vec oldfogcolor(0, 0, 0), curfogcolor(0, 0, 0);
float oldfogstart = 0, oldfogend = 1000000, curfogstart = 0, curfogend = 1000000;

void setfogcolor(const vec &v)
{
    GLOBALPARAM(fogcolor, v);
}

void zerofogcolor()
{
    setfogcolor(vec(0, 0, 0));
}

void resetfogcolor()
{
    setfogcolor(curfogcolor);
}

void pushfogcolor(const vec &v)
{
    oldfogcolor = curfogcolor;
    curfogcolor = v;
    resetfogcolor();
}

void popfogcolor()
{
    curfogcolor = oldfogcolor;
    resetfogcolor();
}

void setfogdist(float start, float end)
{
    GLOBALPARAMF(fogparams, 1/(end - start), end/(end - start));
}

void clearfogdist()
{
    setfogdist(0, 1000000);
}

void resetfogdist()
{
    setfogdist(curfogstart, curfogend);
}

void pushfogdist(float start, float end)
{
    oldfogstart = curfogstart;
    oldfogend = curfogend;
    curfogstart = start;
    curfogend = end;
    resetfogdist();
}

void popfogdist()
{
    curfogstart = oldfogstart;
    curfogend = oldfogend;
    resetfogdist();
}

static void resetfog()
{
    resetfogcolor();
    resetfogdist();

    glClearColor(curfogcolor.r, curfogcolor.g, curfogcolor.b, 1.0f);
}

static void setfog(int fogmat, float below = 1, int abovemat = MAT_AIR)
{
    float logscale = 256, logblend = log(1 + (logscale - 1)*below) / log(logscale);

    curfogstart = curfogend = 0;
    curfogcolor = vec(0, 0, 0);
    blendfog(fogmat, below, logblend, curfogstart, curfogend, curfogcolor);
    if(below < 1) blendfog(abovemat, 1-below, 1-logblend, curfogstart, curfogend, curfogcolor);

    resetfog();
}

static void setnofog(const vec &color = vec(0, 0, 0))
{
    curfogstart = 0;
    curfogend = 1000000;
    curfogcolor = color;

    resetfog();
}

static void blendfogoverlay(int fogmat, float blend, vec &overlay)
{
    float maxc;
    switch(fogmat&MATF_VOLUME)
    {
        case MAT_WATER:
        {
            const bvec &wcol = getwatercolor(fogmat);
            maxc = max(wcol.r, max(wcol.g, wcol.b));
            overlay.madd(vec(wcol.r, wcol.g, wcol.b).div(min(32.0f + maxc*7.0f/8.0f, 255.0f)).max(0.4f), blend);
            break;
        }

        case MAT_LAVA:
        {
            const bvec &lcol = getlavacolor(fogmat);
            maxc = max(lcol.r, max(lcol.g, lcol.b));
            overlay.madd(vec(lcol.r, lcol.g, lcol.b).div(min(32.0f + maxc*7.0f/8.0f, 255.0f)).max(0.4f), blend);
            break;
        }

        default:
            overlay.add(blend);
            break;
    }
}

void drawfogoverlay(int fogmat, float fogblend, int abovemat)
{
    SETSHADER(fogoverlay);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    vec overlay(0, 0, 0);
    blendfogoverlay(fogmat, fogblend, overlay);
    blendfogoverlay(abovemat, 1-fogblend, overlay);

    gle::color(overlay);
    screenquad();

    glDisable(GL_BLEND);
}

bool renderedgame = false;

void rendergame(bool mainpass)
{
    bool collect = mainpass && !shadowmapping && !glaring && !reflecting && !refracting;
    if(collect) hwrtbeginscenemodels();
    game::rendergame(mainpass);
    if(collect) hwrtendscenemodels();
    if(!shadowmapping) renderedgame = true;
}

VARP(skyboxglare, 0, 1, 1);

void drawglare()
{
    glaring = true;
    refracting = -1;

    pushfogcolor(vec(0, 0, 0));

    glClearColor(0, 0, 0, 1);
    glClear((skyboxglare && !shouldclearskyboxglare() ? 0 : GL_COLOR_BUFFER_BIT) | GL_DEPTH_BUFFER_BIT);

    rendergeom();

    if(skyboxglare) drawskybox(farplane, false);

    renderreflectedmapmodels();
    rendergame();
    renderavatar();

    renderwater();
    rendermaterials();
    renderalphageom();
    renderparticles();

    popfogcolor();

    refracting = 0;
    glaring = false;
}

VARP(reflectmms, 0, 1, 1);
VARR(refractsky, 0, 0, 1);

matrix4 noreflectmatrix;

void drawreflection(float z, bool refract, int fogdepth, const bvec &col)
{
    hwrthdrsetlin(false);
    reflectz = z < 0 ? 1e16f : z;
    reflecting = !refract;
    refracting = refract ? (z < 0 || camera1->o.z >= z ? -1 : 1) : 0;
    fading = waterrefract && waterfade && z>=0;
    fogging = refracting<0 && z>=0;
    refractfog = fogdepth;

    if(fogging)
    {
        pushfogdist(camera1->o.z - z, camera1->o.z - (z - max(refractfog, 1)));
        pushfogcolor(col.tocolor());
    }
    else
    {
        vec color(0, 0, 0);
        float start = 0, end = 0;
        blendfog(MAT_AIR, 1, 1, start, end, color);
        pushfogdist(start, end);
        pushfogcolor(color);
    }

    if(fading)
    {
        float scale = fogging ? -0.25f : 0.25f, offset = 2*fabs(scale) - scale*z;
        GLOBALPARAMF(waterfadeparams, scale, offset, -scale, offset + camera1->o.z*scale);
    }

    if(reflecting)
    {
        noreflectmatrix = cammatrix;
        cammatrix.reflectz(z);

        glFrontFace(GL_CCW);
    }

    if(reflectclip && z>=0)
    {
        float zoffset = reflectclip/4.0f, zclip;
        if(refracting<0)
        {
            zclip = z+zoffset;
            if(camera1->o.z<=zclip) zclip = z;
        }
        else
        {
            zclip = z-zoffset;
            if(camera1->o.z>=zclip && camera1->o.z<=z+4.0f) zclip = z;
            if(reflecting) zclip = 2*z - zclip;
        }
        plane clipplane;
        invcammatrix.transposedtransform(plane(0, 0, refracting>0 ? 1 : -1, refracting>0 ? -zclip : zclip), clipplane);
        clipmatrix.clip(clipplane, projmatrix);
        noclipmatrix = projmatrix;
        projmatrix = clipmatrix;
    }

    setcamprojmatrix(false, true);

    renderreflectedgeom(refracting<0 && z>=0 && caustics, fogging);

    if(reflecting || refracting>0 || (refracting<0 && refractsky) || z<0)
    {
        if(fading) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if(reflectclip && z>=0)
        {
            projmatrix = noclipmatrix;
            setcamprojmatrix(false, true);
        }
        drawskybox(farplane, false);
        if(reflectclip && z>=0)
        {
            projmatrix = clipmatrix;
            setcamprojmatrix(false, true);
        }
        if(fading) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    }
    else if(fading) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    renderdecals();

    if(reflectmms) renderreflectedmapmodels();
    rendergame();

    if(refracting && z>=0 && !isthirdperson() && fabs(camera1->o.z-z) <= 0.5f*(player->eyeheight + player->aboveeye))
    {   
        matrix4 oldprojmatrix = projmatrix, avatarproj;
        avatarproj.perspective(curavatarfov, aspect, nearplane, farplane);
        hwrttemporalapplystored(avatarproj);
        if(reflectclip)
        {
            matrix4 avatarclip;
            plane clipplane;
            invcammatrix.transposedtransform(plane(0, 0, refracting, reflectclipavatar/4.0f - refracting*z), clipplane);
            avatarclip.clip(clipplane, avatarproj);
            projmatrix = avatarclip;
        }
        else projmatrix = avatarproj;
        setcamprojmatrix(false, true);
        game::renderavatar();
        projmatrix = oldprojmatrix;
        setcamprojmatrix(false, true);
    }

    if(refracting) rendergrass();
    rendermaterials();
    renderalphageom(fogging);
    renderparticles();

    if(fading) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if(reflectclip && z>=0) projmatrix = noclipmatrix; 

    if(reflecting)
    {
        cammatrix = noreflectmatrix;

        glFrontFace(GL_CW);
    }

    popfogdist();
    popfogcolor();
    
    reflectz = 1e16f;
    refracting = 0;
    reflecting = fading = fogging = false;

    setcamprojmatrix(false, true);
}

// Traced water reflections (water.cpp): GL's mirror camera for the plane at
// z, exactly the setup drawreflection() gives its reflecting pass, but nothing
// is drawn here; the caller renders the particles of that plane into the
// traced image. begin = false puts the main camera back.
static matrix4 reflpartcammatrix, reflpartprojmatrix;

void hwrtreflectcamera(float z, bool begin)
{
    if(begin)
    {
        reflpartcammatrix = cammatrix;
        reflpartprojmatrix = projmatrix;
        reflectz = z;
        reflecting = true;

        vec color(0, 0, 0);
        float start = 0, end = 0;
        blendfog(MAT_AIR, 1, 1, start, end, color);
        pushfogdist(start, end);
        pushfogcolor(color);

        cammatrix.reflectz(z);
        glFrontFace(GL_CCW);
        if(reflectclip)
        {
            float zclip = z - reflectclip/4.0f;
            if(camera1->o.z>=zclip && camera1->o.z<=z+4.0f) zclip = z;
            zclip = 2*z - zclip;
            plane clipplane;
            invcammatrix.transposedtransform(plane(0, 0, -1, zclip), clipplane);
            clipmatrix.clip(clipplane, projmatrix);
            projmatrix = clipmatrix;
        }
        setcamprojmatrix(false, true);
    }
    else
    {
        cammatrix = reflpartcammatrix;
        projmatrix = reflpartprojmatrix;
        glFrontFace(GL_CW);
        popfogdist();
        popfogcolor();
        reflectz = 1e16f;
        reflecting = false;
        setcamprojmatrix(false, true);
    }
}

int drawtex = 0;

void drawcubemap(int size, const vec &o, float yaw, float pitch, const cubemapside &side, bool onlysky)
{
    drawtex = DRAWTEX_ENVMAP;

    physent *oldcamera = camera1;
    static physent cmcamera;
    cmcamera = *player;
    cmcamera.reset();
    cmcamera.type = ENT_CAMERA;
    cmcamera.o = o;
    cmcamera.yaw = yaw;
    cmcamera.pitch = pitch;
    cmcamera.roll = 0;
    camera1 = &cmcamera;
    setviewcell(camera1->o);
   
    int fogmat = lookupmaterial(o)&(MATF_VOLUME|MATF_INDEX);

    setfog(fogmat);

    int farplane = worldsize*2;

    projmatrix.perspective(90.0f, 1.0f, nearplane, farplane);
    if(!side.flipx || !side.flipy) projmatrix.scalexy(!side.flipx ? -1 : 1, !side.flipy ? -1 : 1);
    if(side.swapxy)
    {
        swap(projmatrix.a.x, projmatrix.a.y);
        swap(projmatrix.b.x, projmatrix.b.y);
        swap(projmatrix.c.x, projmatrix.c.y);
        swap(projmatrix.d.x, projmatrix.d.y);
    }
    setcamprojmatrix();

    xtravertsva = xtraverts = glde = gbatches = 0;

    visiblecubes();

    if(onlysky) drawskybox(farplane, false, true);
    else
    {
        glClear(GL_DEPTH_BUFFER_BIT);

        glEnable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);

        if(limitsky()) drawskybox(farplane, true);

        rendergeom();

        if(!limitsky()) drawskybox(farplane, false);

//      queryreflections();

        rendermapmodels();
        renderalphageom();

//      drawreflections();

//      renderwater();
//      rendermaterials();

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
    }

    camera1 = oldcamera;
    drawtex = 0;
}

VAR(modelpreviewfov, 10, 20, 100);
VAR(modelpreviewpitch, -90, -15, 90);

namespace modelpreview
{
    physent *oldcamera;
    physent camera;

    float oldaspect, oldfovy, oldfov;
    int oldfarplane;
    matrix4 oldprojmatrix;

    void start(int x, int y, int w, int h, bool background)
    {
        drawtex = DRAWTEX_MODELPREVIEW;

        glViewport(x, y, w, h);
        glScissor(x, y, w, h);
        glEnable(GL_SCISSOR_TEST);
        // Native HDR menus: the model is drawn into its own 8-bit target and
        // composed like the HUD, so it keeps its SDR look (clipped at white).
        if(hdrout_preview_begin(x, y, w, h, background))
        {
            glViewport(0, 0, w, h);
            glScissor(0, 0, w, h);
        }

        oldcamera = camera1;
        camera = *camera1;
        camera.reset();
        camera.type = ENT_CAMERA;
        camera.o = vec(0, 0, 0);
        camera.yaw = 0;
        camera.pitch = modelpreviewpitch;
        camera.roll = 0;
        camera1 = &camera;

        oldaspect = aspect;
        oldfovy = fovy;
        oldfov = curfov;
        oldfarplane = farplane;
        oldprojmatrix = projmatrix;

        aspect = w/float(h);
        fovy = modelpreviewfov;
        curfov = 2*atan2(tan(fovy/2*RAD), 1/aspect)/RAD;
        farplane = 1024;

        clearfogdist();
        zerofogcolor();
        glClearColor(0, 0, 0, 1);

        glClear((background ? GL_COLOR_BUFFER_BIT : 0) | GL_DEPTH_BUFFER_BIT);

        projmatrix.perspective(fovy, aspect, nearplane, farplane);
        setcamprojmatrix();

        glEnable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
    }

    void end()
    {
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);

        resetfogdist();
        resetfogcolor();
        glClearColor(curfogcolor.r, curfogcolor.g, curfogcolor.b, 1);

        aspect = oldaspect;
        fovy = oldfovy;
        curfov = oldfov;
        farplane = oldfarplane;

        camera1 = oldcamera;
        drawtex = 0;

        hdrout_preview_end();
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, screenw, screenh);

        projmatrix = oldprojmatrix;
        setcamprojmatrix();
    }
}

vec calcmodelpreviewpos(const vec &radius, float &yaw)
{
    yaw = fmod(lastmillis/10000.0f*360.0f, 360.0f);
    float dist = 1.15f*max(radius.magnitude2()/aspect, radius.magnitude())/sinf(fovy/2*RAD);
    return vec(0, dist, 0).rotate_around_x(camera1->pitch*RAD);
}

GLuint minimaptex = 0;
vec minimapcenter(0, 0, 0), minimapradius(0, 0, 0), minimapscale(0, 0, 0);

void clearminimap()
{
    if(minimaptex) { glDeleteTextures(1, &minimaptex); minimaptex = 0; }
}

VARR(minimapheight, 0, 0, 2<<16);
bvec minimapcolor(0, 0, 0);
HVARFR(minimapcolour, 0, 0, 0xFFFFFF,
{
    minimapcolor = bvec((minimapcolour>>16)&0xFF, (minimapcolour>>8)&0xFF, minimapcolour&0xFF);
});
VARR(minimapclip, 0, 0, 1);
VARFP(minimapsize, 7, 8, 10, { if(minimaptex) drawminimap(); });

void bindminimap()
{
    glBindTexture(GL_TEXTURE_2D, minimaptex);
}

void clipminimap(ivec &bbmin, ivec &bbmax, cube *c = worldroot, const ivec &co = ivec(0, 0, 0), int size = worldsize>>1)
{
    loopi(8)
    {
        ivec o(i, co, size);
        if(c[i].children) clipminimap(bbmin, bbmax, c[i].children, o, size>>1);
        else if(!isentirelysolid(c[i]) && (c[i].material&MATF_CLIP)!=MAT_CLIP)
        {
            loopk(3) bbmin[k] = min(bbmin[k], o[k]);
            loopk(3) bbmax[k] = max(bbmax[k], o[k] + size);
        }
    }
}

void drawminimap()
{
    hwrthdrsetlin(false);
    if(!game::needminimap()) { clearminimap(); return; }

    renderprogress(0, "generating mini-map...", 0, !renderedframe);

    int size = 1<<minimapsize, sizelimit = min(hwtexsize, min(screenw, screenh));
    while(size > sizelimit) size /= 2;
    if(!minimaptex) glGenTextures(1, &minimaptex);

    ivec bbmin(worldsize, worldsize, worldsize), bbmax(0, 0, 0);
    loopv(valist)
    {
        vtxarray *va = valist[i];
        loopk(3)
        {
            if(va->geommin[k]>va->geommax[k]) continue;
            bbmin[k] = min(bbmin[k], va->geommin[k]);
            bbmax[k] = max(bbmax[k], va->geommax[k]);
        }
    }
    if(minimapclip)
    {
        ivec clipmin(worldsize, worldsize, worldsize), clipmax(0, 0, 0);
        clipminimap(clipmin, clipmax);
        loopk(2) bbmin[k] = max(bbmin[k], clipmin[k]);
        loopk(2) bbmax[k] = min(bbmax[k], clipmax[k]); 
    }
 
    minimapradius = vec(bbmax).sub(vec(bbmin)).mul(0.5f); 
    minimapcenter = vec(bbmin).add(minimapradius);
    minimapradius.x = minimapradius.y = max(minimapradius.x, minimapradius.y);
    minimapscale = vec((0.5f - 1.0f/size)/minimapradius.x, (0.5f - 1.0f/size)/minimapradius.y, 1.0f);

    drawtex = DRAWTEX_MINIMAP;

    physent *oldcamera = camera1;
    static physent cmcamera;
    cmcamera = *player;
    cmcamera.reset();
    cmcamera.type = ENT_CAMERA;
    cmcamera.o = vec(minimapcenter.x, minimapcenter.y, max(minimapcenter.z + minimapradius.z + 1, float(minimapheight)));
    cmcamera.yaw = 0;
    cmcamera.pitch = -90;
    cmcamera.roll = 0;
    camera1 = &cmcamera;
    setviewcell(vec(-1, -1, -1));

    projmatrix.ortho(-minimapradius.x, minimapradius.x, -minimapradius.y, minimapradius.y, 0, camera1->o.z + 1);
    projmatrix.a.mul(-1);
    setcamprojmatrix();

    setnofog(minimapcolor.tocolor());

    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    glViewport(0, 0, size, size);

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    glFrontFace(GL_CCW);

    xtravertsva = xtraverts = glde = gbatches = 0;

    visiblecubes(false);
    queryreflections();
    drawreflections();

    loopi(minimapheight > 0 && minimapheight < minimapcenter.z + minimapradius.z ? 2 : 1)
    {
        if(i)
        {
            glClear(GL_DEPTH_BUFFER_BIT);
            camera1->o.z = minimapheight;
            setcamprojmatrix();
        }
        rendergeom();
        rendermapmodels();
        renderwater();
        rendermaterials();
        renderalphageom();
    }

    glFrontFace(GL_CW);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glViewport(0, 0, screenw, screenh);

    camera1 = oldcamera;
    drawtex = 0;

    glBindTexture(GL_TEXTURE_2D, minimaptex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5, 0, 0, size, size, 0);
    setuptexparameters(minimaptex, NULL, 3, 1, GL_RGB5, GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    GLfloat border[4] = { minimapcolor.x/255.0f, minimapcolor.y/255.0f, minimapcolor.z/255.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glBindTexture(GL_TEXTURE_2D, 0);
}

bool deferdrawtextures = false;

void drawtextures()
{
    if(minimized) { deferdrawtextures = true; return; }
    deferdrawtextures = false;
    genenvmaps();
    drawminimap();
}

GLuint motiontex = 0;
int motionw = 0, motionh = 0, lastmotion = 0;

void cleanupmotionblur()
{
    if(motiontex) { glDeleteTextures(1, &motiontex); motiontex = 0; }
    motionw = motionh = 0;
    lastmotion = 0;
}

VARFP(motionblur, 0, 0, 1, { if(!motionblur) cleanupmotionblur(); });
VARP(motionblurmillis, 1, 5, 1000);
FVARP(motionblurscale, 0, 0.5f, 1);

void addmotionblur()
{
    int pw = hwrtfbw(), ph = hwrtfbh();
    if(!motionblur || max(pw, ph) > hwtexsize) return;

    if(game::ispaused()) { lastmotion = 0; return; }

    if(!motiontex || motionw != pw || motionh != ph)
    {
        if(!motiontex) glGenTextures(1, &motiontex);
        motionw = pw;
        motionh = ph;
        lastmotion = 0;
        createtexture(motiontex, motionw, motionh, NULL, 3, 0, GL_RGB);
    }

    glBindTexture(GL_TEXTURE_2D, motiontex);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    SETSHADER(screenrect);

    gle::colorf(1, 1, 1, lastmotion ? pow(motionblurscale, max(float(lastmillis - lastmotion)/motionblurmillis, 1.0f)) : 0);
    screenquad(1, 1);

    glDisable(GL_BLEND);

    if(lastmillis - lastmotion >= motionblurmillis)
    {
        lastmotion = lastmillis - lastmillis%motionblurmillis;

        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pw, ph);
    }
}

VARP(lookao, 0, 1, 1);
FVARP(lookaoscale, 0, 0.48f, 1);
FVARP(lookaoradius, 0.4f, 1.15f, 3);

static GLuint lookaocolor = 0, lookaodepth = 0;
static int lookaow = 0, lookaoh = 0;

void cleanuplookao()
{
    if(lookaocolor) { glDeleteTextures(1, &lookaocolor); lookaocolor = 0; }
    if(lookaodepth) { glDeleteTextures(1, &lookaodepth); lookaodepth = 0; }
    lookaow = lookaoh = 0;
}

void addlookao()
{
    if(!lookao || minimized) return;
    int pw = hwrtfbw(), ph = hwrtfbh();
    if(pw < 8 || ph < 8) return;

    if(!lookaocolor) glGenTextures(1, &lookaocolor);
    if(!lookaodepth) glGenTextures(1, &lookaodepth);
    if(lookaow != pw || lookaoh != ph)
    {
        lookaow = pw;
        lookaoh = ph;
        createtexture(lookaocolor, lookaow, lookaoh, NULL, 3, 0, GL_RGB);
        glBindTexture(GL_TEXTURE_2D, lookaodepth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, lookaow, lookaoh, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    }

    glBindTexture(GL_TEXTURE_2D, lookaocolor);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pw, ph);
    glBindTexture(GL_TEXTURE_2D, lookaodepth);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pw, ph);

    Shader *s = useshaderbyname("lookssao");
    if(!s || s->invalid()) return;
    s->set();

    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, lookaocolor);
    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lookaodepth);
    glActiveTexture_(GL_TEXTURE0);

    LOCALPARAMF(postfxscale, 1.0f/pw, 1.0f/ph);
    LOCALPARAMF(lookaoproj, nearplane, float(farplane), 1.0f/pw, 1.0f/ph);
    LOCALPARAMF(lookaoparams, lookaoscale, lookaoradius, 0, 0);
    screenquad(1, 1);
}

// Ground Truth Ambient Occlusion, classic lighting only (XeGTAO port, see
// glsl.cfg). Runs after the world and the mapmodels, before the players:
// its depth never holds a player or the first-person gun, and its result is
// applied to the lightmapped world alone, on the ambient and sky part of
// each lumel. Off by default; with RT lighting the bake is skipped and RT
// has its own sky visibility, so it does nothing there.
void cleanupgtao();
VARFP(gtao, 0, 1, 2, cleanupgtao());
VAR(gtaodebug, 0, 0, 3);
FVAR(gtaolowradius, 1, 5, 64);
FVAR(gtaohighradius, 1, 10, 64);
VAR(gtaolowslices, 1, 2, 8);
VAR(gtaohighslices, 1, 3, 8);
VAR(gtaolowsteps, 1, 3, 8);
VAR(gtaohighsteps, 1, 4, 8);
FVAR(gtaopower, 0.5f, 1, 3);
FVAR(gtaomaxscreen, 0.02f, 0.12f, 0.5f);
FVAR(gtaofadestart, 0, 600, 1e4f);
FVAR(gtaofadelength, 1, 400, 1e4f);
FVAR(gtaodenoisetol, 0, 2, 64);

#define GTAO_MIPS 5
static GLuint gtaodepthtex = 0, gtaolintex = 0, gtaorawtex = 0, gtaoaotex = 0;
static GLuint gtaolinfbo[GTAO_MIPS] = { 0 }, gtaorawfbo = 0, gtaoaofbo = 0;
static int gtaow = 0, gtaoh = 0;
static bool gtaoapplied = false;

void cleanupgtao()
{
    if(gtaodepthtex) { glDeleteTextures(1, &gtaodepthtex); gtaodepthtex = 0; }
    if(gtaolintex) { glDeleteTextures(1, &gtaolintex); gtaolintex = 0; }
    if(gtaorawtex) { glDeleteTextures(1, &gtaorawtex); gtaorawtex = 0; }
    if(gtaoaotex) { glDeleteTextures(1, &gtaoaotex); gtaoaotex = 0; }
    loopi(GTAO_MIPS) if(gtaolinfbo[i]) { glDeleteFramebuffers_(1, &gtaolinfbo[i]); gtaolinfbo[i] = 0; }
    if(gtaorawfbo) { glDeleteFramebuffers_(1, &gtaorawfbo); gtaorawfbo = 0; }
    if(gtaoaofbo) { glDeleteFramebuffers_(1, &gtaoaofbo); gtaoaofbo = 0; }
    gtaow = gtaoh = 0;
}

static void gtaotexparams(GLenum minfilter)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minfilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool gtaofbo(GLuint &fbo, GLuint tex, int level)
{
    if(!fbo) glGenFramebuffers_(1, &fbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, level);
    return glCheckFramebufferStatus_(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static bool setupgtao(int w, int h)
{
    if(gtaow == w && gtaoh == h && gtaoaofbo) return true;
    cleanupgtao();
    glGenTextures(1, &gtaodepthtex);
    glBindTexture(GL_TEXTURE_2D, gtaodepthtex);
    gtaotexparams(GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);

    glGenTextures(1, &gtaolintex);
    glBindTexture(GL_TEXTURE_2D, gtaolintex);
    gtaotexparams(GL_NEAREST_MIPMAP_NEAREST);
    loopi(GTAO_MIPS) glTexImage2D(GL_TEXTURE_2D, i, GL_R32F, max(w>>i, 1), max(h>>i, 1), 0, GL_RED, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GTAO_MIPS-1);

    glGenTextures(1, &gtaorawtex);
    glBindTexture(GL_TEXTURE_2D, gtaorawtex);
    gtaotexparams(GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

    glGenTextures(1, &gtaoaotex);
    glBindTexture(GL_TEXTURE_2D, gtaoaotex);
    gtaotexparams(GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);

    bool ok = true;
    loopi(GTAO_MIPS) ok = gtaofbo(gtaolinfbo[i], gtaolintex, i) && ok;
    ok = gtaofbo(gtaorawfbo, gtaorawtex, 0) && ok;
    ok = gtaofbo(gtaoaofbo, gtaoaotex, 0) && ok;
    if(!ok)
    {
        static bool gtaowarned = false;
        if(!gtaowarned)
        {
            gtaowarned = true;
            logoutf("gtao: render targets incomplete, ambient occlusion disabled");
            conoutf(CON_WARN, "Ambient occlusion could not start on this graphics card; it stays off.");
        }
        cleanupgtao();
        return false;
    }
    gtaow = w;
    gtaoh = h;
    return true;
}

// The pass the frame actually gets: never with RT lighting (no bake to
// darken), never without GLSL 1.30 (texelFetch / textureLod).
static bool gtaousable()
{
    if(!gtao || minimized || glslversion < 130 || !hasFBO) return false;
    if(wireframe && editmode) return false;
    if(hwrtnobake()) return false;
    return true;
}

static void gtaopass(Shader *s, GLuint fbo, int w, int h)
{
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    s->set();
}

void rendergtao()
{
    gtaoapplied = false;
    if(!gtaousable()) return;
    int w = hwrtfbw(), h = hwrtfbh();
    if(w < 8 || h < 8) return;
    Shader *linshader = useshaderbyname("gtaolinear"), *mipshader = useshaderbyname("gtaomip"),
           *mainshader = useshaderbyname("gtaomain"), *denoiseshader = useshaderbyname("gtaodenoise"),
           *applyshader = useshaderbyname("gtaoapply");
    if(!linshader || linshader->invalid() || !mipshader || mipshader->invalid() ||
       !mainshader || mainshader->invalid() || !denoiseshader || denoiseshader->invalid() ||
       !applyshader || applyshader->invalid())
        return;

    GLint prevfb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevfb);
    if(!setupgtao(w, h)) { glBindFramebuffer_(GL_FRAMEBUFFER, prevfb); return; }
    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);

    // World + mapmodels depth, players not drawn yet.
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gtaodepthtex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

    bool depthtest = glIsEnabled(GL_DEPTH_TEST) != 0, cull = glIsEnabled(GL_CULL_FACE) != 0,
         stencil = glIsEnabled(GL_STENCIL_TEST) != 0, blend = glIsEnabled(GL_BLEND) != 0;
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);

    gtaopass(linshader, gtaolinfbo[0], w, h);
    LOCALPARAMF(gtaoproj, nearplane, float(farplane), 0, 0);
    screenquad();

    bool high = gtao >= 2;
    float radius = high ? gtaohighradius : gtaolowradius;
    float falloffrange = 0.615f*radius, fallofffrom = radius*(1 - 0.615f);
    float falloffmul = -1.0f/falloffrange, falloffadd = fallofffrom/falloffrange + 1;
    {
        // XeGTAO_DepthMIPFilter: 0.75 x radius
        float mr = 0.75f*radius, mrange = 0.615f*mr, mfrom = mr*(1 - 0.615f);
        glBindTexture(GL_TEXTURE_2D, gtaolintex);
        for(int i = 1; i < GTAO_MIPS; i++)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, i-1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, i-1);
            gtaopass(mipshader, gtaolinfbo[i], max(w>>i, 1), max(h>>i, 1));
            LOCALPARAMF(gtaomipparams, -1.0f/mrange, mfrom/mrange + 1, 0, 0);
            screenquad();
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GTAO_MIPS-1);
    }

    // View position from the (possibly jittered) projection this frame uses.
    vec4 gtaoview(1.0f/projmatrix.a.x, 1.0f/projmatrix.b.y, projmatrix.c.x, projmatrix.c.y);
    float pixview = 2.0f/(projmatrix.b.y*h);
    gtaopass(mainshader, gtaorawfbo, w, h);
    LOCALPARAM(gtaoview, gtaoview);
    LOCALPARAMF(gtaosize, w, h, 1.0f/w, 1.0f/h);
    LOCALPARAMF(gtaoradius, radius, falloffmul, falloffadd, pixview);
    LOCALPARAMF(gtaoparams, high ? gtaohighslices : gtaolowslices, high ? gtaohighsteps : gtaolowsteps, gtaomaxscreen*h, gtaopower);
    LOCALPARAMF(gtaofade, gtaofadestart, 1.0f/gtaofadelength, float(farplane)*0.999f, 0);
    screenquad();

    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gtaolintex);
    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gtaorawtex);
    gtaopass(denoiseshader, gtaoaofbo, w, h);
    LOCALPARAM(gtaoview, gtaoview);
    LOCALPARAMF(gtaosize, w, h, 1.0f/w, 1.0f/h);
    LOCALPARAMF(gtaodenoiseparams, gtaodenoisetol, pixview, 0.05f, 0);
    screenquad();

    glBindFramebuffer_(GL_FRAMEBUFFER, prevfb);
    glViewport(0, 0, w, h);

    if(gtaodebug == 1 || gtaodebug == 3)
    {
        Shader *showshader = useshaderbyname("gtaoshow");
        if(showshader && !showshader->invalid())
        {
            glBindTexture(GL_TEXTURE_2D, gtaodebug == 3 ? gtaorawtex : gtaoaotex);
            showshader->set();
            screenquad();
        }
    }
    else
    {
        // Multiplier on the lightmapped world, over the depth it wrote.
        glActiveTexture_(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, gtaoaotex);
        glActiveTexture_(GL_TEXTURE0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_FALSE);
        glEnable(GL_CULL_FACE);
        if(gtaodebug != 2)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        applyshader->set();
        extern bvec skylightcolor;
        LOCALPARAMF(gtaosky, skylightcolor.x/255.0f, skylightcolor.y/255.0f, skylightcolor.z/255.0f, 0);
        LOCALPARAMF(gtaoscreen, 1.0f/w, 1.0f/h, gtaodebug == 2 ? 1 : 0, 0);
        extern void rendergtaogeom();
        rendergtaogeom();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glDisable(GL_BLEND);
        glActiveTexture_(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture_(GL_TEXTURE0);
    }

    if(depthtest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(stencil) glEnable(GL_STENCIL_TEST);
    if(blend) glEnable(GL_BLEND);
    gtaoapplied = true;
}

static GLuint looktaacolor = 0, looktaadepth = 0, looktaahist = 0;
static int looktaaw = 0, looktaah = 0;
static bool looktaaready = false;
static uint looktaaframe = 0;

void cleanuplooktaa()
{
    if(looktaacolor) { glDeleteTextures(1, &looktaacolor); looktaacolor = 0; }
    if(looktaadepth) { glDeleteTextures(1, &looktaadepth); looktaadepth = 0; }
    if(looktaahist) { glDeleteTextures(1, &looktaahist); looktaahist = 0; }
    looktaaw = looktaah = 0;
    looktaaready = false;
}

VARFP(looktaa, 0, 0, 1, {
    looktaaready = false;
    if(!looktaa) cleanuplooktaa();
    // Not while config.cfg loads: the shaders are not loaded yet ("no such postfx
    // shader: fxaa" at every start); glsl.cfg runs LOOK_apply once they are.
    if(!initing && identexists("LOOK_apply")) execute("LOOK_apply");
});

// FXAA in Native, off by default. The pass also averages texture detail that is
// not an edge, so the scene looked softer than stock Sauerbraten. Drawn only
// when this is 1, Temporal AA is off and DLAA/DLSS/FSR is not the image shown.
VARFP(lookfxaa, 0, 0, 1, {
    if(!initing && identexists("LOOK_apply")) execute("LOOK_apply");
});

static bool looktaausable()
{
    if(!looktaa || minimized) return false;
    Shader *s = useshaderbyname("looktaa");
    return s && !s->invalid();
}

static inline float looktaahalton(int i, int base)
{
    float f = 1, r = 0;
    while(i > 0)
    {
        f /= base;
        r += f * (i % base);
        i /= base;
    }
    return r;
}

// One-pixel Halton (2,3) offset on the main camera only. Applied after
// projmatrix.perspective() in gl_drawframe, before setcamprojmatrix().
// Avatar / cubemap / preview / polygon-offset rebuild or save/restore
// projmatrix themselves. Reflections clip the already-jittered matrix,
// which keeps the water sample on the same sub-pixel as the world.
//
// The offset belongs on column c (c.x / c.y), not d. Sauer's perspective
// has d = (0, 0, 2nf/(n-f), 0); adding to d.x shifts NDC by d.x/(-z) and
// would jitter near geometry more than far. c.x * z / w, with w = -z,
// is a depth-independent NDC shift, and RT primary rays pick it up
// through invcamprojmatrix (push constants stay 128 bytes).
static void applylooktaajitter()
{
    // Trial jitter owns the main-view Halton offset. Applying both would
    // double the subpixel shift and mix two independent sequences.
    if(hwrtdlaajitteractive()) return;
    if(!looktaausable() || screenw < 1 || screenh < 1) return;
    looktaaframe++;
    int i = int(looktaaframe % 16) + 1;
    float jx = (looktaahalton(i, 2) - 0.5f) * 2.0f / screenw;
    float jy = (looktaahalton(i, 3) - 0.5f) * 2.0f / screenh;
    projmatrix.c.x -= jx;
    projmatrix.c.y -= jy;
}

void addlooktaa()
{
    if(hwrtdlaajitteractive())
    {
        looktaaready = false;
        return;
    }
    if(!looktaausable())
    {
        looktaaready = false;
        return;
    }

    if(!looktaacolor) glGenTextures(1, &looktaacolor);
    if(!looktaadepth) glGenTextures(1, &looktaadepth);
    if(!looktaahist) glGenTextures(1, &looktaahist);
    if(looktaaw != screenw || looktaah != screenh)
    {
        looktaaw = screenw;
        looktaah = screenh;
        looktaaready = false;
        createtexture(looktaacolor, looktaaw, looktaah, NULL, 3, 0, GL_RGB);
        createtexture(looktaahist, looktaaw, looktaah, NULL, 3, 1, GL_RGB);
        glBindTexture(GL_TEXTURE_2D, looktaadepth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, looktaaw, looktaah, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    }

    glBindTexture(GL_TEXTURE_2D, looktaacolor);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, screenw, screenh);
    glBindTexture(GL_TEXTURE_2D, looktaadepth);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, screenw, screenh);

    Shader *s = useshaderbyname("looktaa");
    if(!s || s->invalid()) return;
    s->set();

    glActiveTexture_(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, looktaacolor);
    glActiveTexture_(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, looktaadepth);
    glActiveTexture_(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, looktaahist);
    glActiveTexture_(GL_TEXTURE0);

    matrix4 invcur;
    invcur.invert(camprojmatrix);
    LOCALPARAM(invcamproj, invcur);
    LOCALPARAM(prevcamproj, lastcamprojmatrix);
    LOCALPARAMF(looktaaproj, nearplane, float(farplane), 1.0f/screenw, 1.0f/screenh);
    LOCALPARAMF(looktaaparams, looktaaready ? 1.0f : 0.0f, 0, 0, 0);
    screenquad(1, 1);

    glBindTexture(GL_TEXTURE_2D, looktaahist);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, screenw, screenh);
    looktaaready = true;
    lastcamprojmatrix = camprojmatrix;
}

bool dopostfx = false;

void invalidatepostfx()
{
    dopostfx = false;
}

int xtraverts, xtravertsva;

void gl_drawframe()
{
    extern void hdrout_framebegin();
    hdrout_framebegin();
    if(deferdrawtextures) drawtextures();
    hwrtgenskycube();

    updatedynlights();
    hwrtapplyvelspin();
    hwrtscenebegin();
    hwrttemporalbeginframe();
    hwrtvelensureresources();

    int w = hwrtfbw(), h = hwrtfbh();
    aspect = forceaspect ? forceaspect : screenw/float(screenh);
    fovy = 2*atan2(tan(curfov/2*RAD), aspect)/RAD;
    
    int fogmat = lookupmaterial(camera1->o)&(MATF_VOLUME|MATF_INDEX), abovemat = MAT_AIR;
    float fogblend = 1.0f, causticspass = 0.0f;
    if(isliquid(fogmat&MATF_VOLUME))
    {
        float z = findsurface(fogmat, camera1->o, abovemat) - WATER_OFFSET;
        if(camera1->o.z < z + 1) fogblend = min(z + 1 - camera1->o.z, 1.0f);
        else fogmat = abovemat;
        if(caustics && (fogmat&MATF_VOLUME)==MAT_WATER && camera1->o.z < z)
            causticspass = min(z - camera1->o.z, 1.0f);
    }
    else fogmat = MAT_AIR;    
    setfog(fogmat, fogblend, abovemat);
    if(fogmat!=MAT_AIR)
    {
        float blend = abovemat==MAT_AIR ? fogblend : 1.0f;
        fovy += blend*sinf(lastmillis/1000.0)*2.0f;
        aspect += blend*sinf(lastmillis/1000.0+M_PI)*0.1f;
    }

    farplane = worldsize*2;

    projmatrix.perspective(fovy, aspect, nearplane, farplane);
    hwrttemporalapplymain();
    applylooktaajitter();
    setcamprojmatrix();
    hwrttemporalaftercam();
    hwrtnotecamproj();

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    xtravertsva = xtraverts = glde = gbatches = 0;

    visiblecubes();
    
    glClear(GL_DEPTH_BUFFER_BIT|(wireframe && editmode ? GL_COLOR_BUFFER_BIT : 0));
    hwrtvelbeginframe();

    if(wireframe && editmode) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE); 

    if(limitsky()) drawskybox(farplane, true);

    hwrtvelmark();
    rendergeom(causticspass);
    hwrtvelstencilsnap("after_geom");

    extern int outline;
    if(!wireframe && editmode && outline) renderoutline();

    queryreflections();

    generategrass();

    hwrtvelkeep();
    if(!limitsky()) drawskybox(farplane, false);

    // The RT composite is a fullscreen quad carrying the traced alpha: 1
    // wherever a primary ray hit the BLAS. Decals, water, grass, materials and
    // alpha geometry are deliberately not in the BLAS, so a ray passes straight
    // through them and hands back the lit surface behind; the model mask only
    // protects what rendermapmodels() / rendergame() rasterised. So the
    // composite goes here, before everything GL still draws over the world, and
    // the depth snapshots that feed the mask bracket the two model passes.
    // Glare, reflections and refraction render the scene into their own
    // targets, which the composite never touches, so they still follow the bake.
    // Velocity stencil: 1 = covered (opaque world / rigid mapmodels),
    // 2 = excluded (particles, water, unsupported models, attached guns),
    // 3 = colour-visible skinned players, 0 = empty (sky). Unmark before
    // water / particles. The later object / character velocity pass may write
    // on 0, 1 or 3 but must not replace 2. The gun is drawn after the image.
    hwrtsnapworlddepth();
    hwrtvelmark();
    rendermapmodels();
    hwrtvelstencilsnap("after_models");
    rendergtao();
    hwrtvelunmark();
    rendergame(true);
    hwrtvelstencilsnap("after_game");
    hwrtvelkeep();
    hwrtsnapscenedepth();
    hwrtrender();
    hwrtvelstencilsnap("after_composite");

    // Vanilla runs decals before the model passes. They never write depth
    // (glDepthMask(GL_FALSE) plus a polygon offset), so an opaque model covers
    // them either way and moving them down is the same image with hwrt 0 --
    // and the only order that survives the composite.
    renderdecals(true);

    if(wireframe && editmode) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    drawglaretex();
    drawdepthfxtex();
    drawreflections();
    drawtracedreflectionparticles();

    if(wireframe && editmode) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    hwrtvelunmark();
    renderwater();
    rendergrass();

    rendermaterials();
    renderalphageom();

    if(wireframe && editmode) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    hwrtveldraweffects();
    renderparticles(true);
    hwrtdlaabiascapture();
    hwrtvelstencilsnap("after_particles");

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);

    hwrtgenvelocity();
    if(hwrthdractive())
    {
        static int noted = 0;
        if(!noted)
        {
            noted = 1;
            conoutf(CON_WARN, "hdr lot2: lookao, looktaa et motionblur exclus. glare non ajoute; la cible scene FP16 reste en memoire.");
        }
    }
    else
    {
        addmotionblur();
        // RT traces the sky occlusion itself; lookao on top of it darkened every flat floor and wall seen
        // at a slant, most at 45-130 units, a ring that follows the player (rt-cercle-joueur).
        if(!gtaoapplied && !hwrtnobake()) addlookao();
        addlooktaa();
    }
    // Colour gun after the velocity image (which now includes the hudgun
    // under the avatar FOV) and before the overlay, so the colour history
    // and proof shots see the same weapon the vectors describe.
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    renderavatar();
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    // Stamps go into the linear scene before the temporal copy, so DLSS sees
    // the same uniform highlights the native read measures.
    hwrthdrstampifprobe();
    // HUD-less 3D image (gun in, reticle/UI out) for DLAA evaluate.
    hwrttemporalcapture();
    hwrtdlaaframe();
    hwrtsceneend();
    hwrtdlaaflushshot();
    hwrtdrawveldebug();
    hwrtflushvelshot();
    if(!hwrthdractive()) addglare();
    if(isliquid(fogmat&MATF_VOLUME)) drawfogoverlay(fogmat, fogblend, abovemat);
    renderpostfx();

    gl_drawhud();
    hwrtdlaaafterhud();

    lastcamprojmatrix = camprojmatrix;
    renderedgame = false;
}

void gl_drawmainmenu()
{
    hwrthdrsuspend();
    xtravertsva = xtraverts = glde = gbatches = 0;

    renderbackground(NULL, NULL, NULL, NULL, true, true);
    renderpostfx();

    gl_drawhud();
}

VARNP(damagecompass, usedamagecompass, 0, 1, 1);
VARP(damagecompassfade, 1, 1000, 10000);
VARP(damagecompasssize, 1, 30, 100);
VARP(damagecompassalpha, 1, 25, 100);
VARP(damagecompassmin, 1, 25, 1000);
VARP(damagecompassmax, 1, 200, 1000);

float damagedirs[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

void damagecompass(int n, const vec &loc)
{
    if(!usedamagecompass || minimized) return;
    vec delta(loc);
    delta.sub(camera1->o); 
    float yaw = 0, pitch;
    if(delta.magnitude() > 4)
    {
        vectoyawpitch(delta, yaw, pitch);
        yaw -= camera1->yaw;
    }
    if(yaw >= 360) yaw = fmod(yaw, 360);
    else if(yaw < 0) yaw = 360 - fmod(-yaw, 360);
    int dir = (int(yaw+22.5f)%360)/45;
    damagedirs[dir] += max(n, damagecompassmin)/float(damagecompassmax);
    if(damagedirs[dir]>1) damagedirs[dir] = 1;
}

void drawdamagecompass(int w, int h)
{
    hudnotextureshader->set();

    int dirs = 0;
    float size = damagecompasssize/100.0f*min(h, w)/2.0f;
    loopi(8) if(damagedirs[i]>0)
    {
        if(!dirs)
        {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            gle::colorf(1, 0, 0, damagecompassalpha/100.0f);
            gle::defvertex();
            gle::begin(GL_TRIANGLES);
        }
        dirs++;

        float logscale = 32,
              scale = log(1 + (logscale - 1)*damagedirs[i]) / log(logscale),
              offset = -size/2.0f-min(h, w)/4.0f;
        matrix4x3 m;
        m.identity();
        m.settranslation(w/2, h/2, 0);
        m.rotate_around_z(i*45*RAD);
        m.translate(0, offset, 0);
        m.scale(size*scale);

        gle::attrib(m.transform(vec2(1, 1)));
        gle::attrib(m.transform(vec2(-1, 1)));
        gle::attrib(m.transform(vec2(0, 0)));

        // fade in log space so short blips don't disappear too quickly
        scale -= float(curtime)/damagecompassfade;
        damagedirs[i] = scale > 0 ? (pow(logscale, scale) - 1) / (logscale - 1) : 0;
    }
    if(dirs) gle::end();
}

int damageblendmillis = 0;

VARFP(damagescreen, 0, 1, 1, { if(!damagescreen) damageblendmillis = 0; });
VARP(damagescreenfactor, 1, 7, 100);
VARP(damagescreenalpha, 1, 45, 100);
VARP(damagescreenfade, 0, 125, 1000);
VARP(damagescreenmin, 1, 10, 1000);
VARP(damagescreenmax, 1, 100, 1000);

void damageblend(int n)
{
    if(!damagescreen || minimized) return;
    if(lastmillis > damageblendmillis) damageblendmillis = lastmillis;
    damageblendmillis += clamp(n, damagescreenmin, damagescreenmax)*damagescreenfactor;
}

void drawdamagescreen(int w, int h)
{
    if(lastmillis >= damageblendmillis) return;

    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    hudshader->set();

    static Texture *damagetex = NULL;
    if(!damagetex) damagetex = textureload("packages/hud/damage.png", 3);

    glBindTexture(GL_TEXTURE_2D, damagetex->id);
    float fade = damagescreenalpha/100.0f;
    if(damageblendmillis - lastmillis < damagescreenfade)
        fade *= float(damageblendmillis - lastmillis)/damagescreenfade;
    gle::colorf(fade, fade, fade, fade);

    hudquad(0, 0, w, h);
}

void cleardamagescreen()
{
    damageblendmillis = 0;
    loopi(8) damagedirs[i] = 0;
}

VAR(hidestats, 0, 0, 1);
VAR(hidehud, 0, 0, 1);

VARP(crosshairsize, 0, 15, 50);
VARP(cursorsize, 0, 30, 50);
VARP(crosshairfx, 0, 1, 1);
VARP(crosshaircolors, 0, 1, 1);
HVARP(crosshaircolour, 0, 0xFFFFFF, 0xFFFFFF); // resting colour; hit/teammate/health tints still override

static vec crosshairbase()
{
    return vec(((crosshaircolour>>16)&0xFF)/255.0f, ((crosshaircolour>>8)&0xFF)/255.0f, (crosshaircolour&0xFF)/255.0f);
}

// crosshairhex "#FF8800" / "ff8800" / "0xFF8800" sets it; no argument returns "FF8800"
ICOMMAND(crosshairhex, "s", (char *s),
{
    const char *p = s;
    while(*p==' ' || *p=='#') p++;
    if(p[0]=='0' && (p[1]=='x' || p[1]=='X')) p += 2;
    if(*p)
    {
        char *end = NULL;
        long c = strtol(p, &end, 16);
        if(end == p + 6 && !*end) crosshaircolour = int(c);
        else conoutf(CON_ERROR, "\f4crosshair colour: 6 hex digits, e.g. #FF8800");
    }
    defformatstring(hex, "%06X", crosshaircolour);
    result(hex);
});

#define MAXCROSSHAIRS 4
static Texture *crosshairs[MAXCROSSHAIRS] = { NULL, NULL, NULL, NULL };

void loadcrosshair(const char *name, int i)
{
    if(i < 0 || i >= MAXCROSSHAIRS) return;
	crosshairs[i] = name ? textureload(name, 3, true) : notexture;
    if(crosshairs[i] == notexture) 
    {
        name = game::defaultcrosshair(i);
        if(!name) name = "data/crosshair.png";
        crosshairs[i] = textureload(name, 3, true);
    }
}

void loadcrosshair_(const char *name, int *i)
{
	loadcrosshair(name, *i);
}

COMMANDN(loadcrosshair, loadcrosshair_, "si");

ICOMMAND(getcrosshair, "i", (int *i), 
{
    const char *name = "";
    if(*i >= 0 && *i < MAXCROSSHAIRS)
    {
        name = crosshairs[*i] ? crosshairs[*i]->name : game::defaultcrosshair(*i);
        if(!name) name = "data/crosshair.png";
    }
    result(name);
});
 
void writecrosshairs(stream *f)
{
    loopi(MAXCROSSHAIRS) if(crosshairs[i] && crosshairs[i]!=notexture)
        f->printf("loadcrosshair %s %d\n", escapestring(crosshairs[i]->name), i);
    f->printf("\n");
}

static void drawcrosshairtex(Texture *crosshair, const vec &color, float x, float y, float chsize)
{
    if(!crosshair || chsize <= 0) return;
    if(crosshair->type&Texture::ALPHA) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else glBlendFunc(GL_ONE, GL_ONE);
    glBindTexture(GL_TEXTURE_2D, crosshair->id);
    hudshader->set();
    gle::color(color);
    hudquad(x, y, chsize, chsize);
}

void drawcrosshair(int w, int h)
{
    bool menucursor = g3d_windowhit(true, false) || (overlaycursor() && !game::demobarcursor());
    bool democursor = game::demobarcursor();
    if(!menucursor && !democursor && (hidehud || mainmenu)) return;

    if(!menucursor)
    {
        vec base = crosshairbase(), color = base;
        int index = game::selectcrosshair(color);
        if(index < 0 && democursor) { index = 0; color = base; }
        if(index >= 0)
        {
            if(!crosshairfx) index = 0;
            if(!crosshairfx || !crosshaircolors) color = base;
            Texture *crosshair = crosshairs[index];
            if(!crosshair)
            {
                loadcrosshair(NULL, index);
                crosshair = crosshairs[index];
            }
            float chsize = crosshairsize*w/900.0f;
            drawcrosshairtex(crosshair, color, 0.5f*w - chsize/2.0f, 0.5f*h - chsize/2.0f, chsize);
        }
    }

    if(menucursor || democursor)
    {
        static Texture *cursor = NULL;
        if(!cursor) cursor = textureload("data/guicursor.png", 3, true);
        float cx, cy, chsize = cursorsize*w/900.0f;
        g3d_cursorpos(cx, cy);
        drawcrosshairtex(cursor, vec(1, 1, 1), cx*w, cy*h, chsize);
    }
}

VARP(wallclock, 0, 0, 1);
VARP(wallclock24, 0, 0, 1);
VARP(wallclocksecs, 0, 0, 1);

static time_t walltime = 0;

VARP(showfps, 0, 1, 1);
VARP(showfpsrange, 0, 0, 1);
VAR(showeditstats, 0, 0, 1);
VAR(statrate, 1, 200, 1000);

FVARP(conscale, 1e-3f, 0.33f, 1e3f);

void gl_drawhud()
{
    g3d_render();

    int w = screenw, h = screenh;
    if(forceaspect) w = int(ceil(h*forceaspect));

    if(editmode && !hidehud && !mainmenu)
    {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        renderblendbrush();

        rendereditcursor();

        glDepthMask(GL_TRUE);
        glDisable(GL_DEPTH_TEST);
    }

    gettextres(w, h);

    hudmatrix.ortho(0, w, h, 0, -1, 1);
    resethudmatrix();
    
    gle::colorf(1, 1, 1);

    extern int debugsm;
    if(debugsm)
    {
        extern void viewshadowmap();
        viewshadowmap();
    }

    extern int debugglare;
    if(debugglare)
    {
        extern void viewglaretex();
        viewglaretex();
    }

    extern int debugdepthfx;
    if(debugdepthfx)
    {
        extern void viewdepthfxtex();
        viewdepthfxtex();
    }

    glEnable(GL_BLEND);
   
    extern void debugparticles();
    debugparticles();
 
    if(!mainmenu)
    {
        drawdamagescreen(w, h);
        drawdamagecompass(w, h);
    }

    hudshader->set();

    int conw = int(w/conscale), conh = int(h/conscale), abovehud = conh - FONTH, limitgui = abovehud;
    // the HUD editor also works from the main menu: the HUD is drawn over its background
    if(!hidehud && (!mainmenu || hudeditactive()))
    {
        hwrtdrawtimes(conw, conh);
        if(!hidestats)
        {
            pushhudmatrix();
            hudmatrix.scale(conscale, conscale, 1);
            flushhudmatrix();

            int roffset = 0;
            hudbegin("stats", "FPS and clock");
            if(showfps)
            {
                static int lastfps = 0, prevfps[3] = { 0, 0, 0 }, curfps[3] = { 0, 0, 0 };
                if(totalmillis - lastfps >= statrate)
                {
                    memcpy(prevfps, curfps, sizeof(prevfps));
                    lastfps = totalmillis - (totalmillis%statrate);
                }
                int nextfps[3];
                getfps(nextfps[0], nextfps[1], nextfps[2]);
                loopi(3) if(prevfps[i]==curfps[i]) curfps[i] = nextfps[i];
                if(showfpsrange) draw_textf("fps %d+%d-%d", conw-7*FONTH, conh-FONTH*3/2, curfps[0], curfps[1], curfps[2]);
                else draw_textf("fps %d", conw-5*FONTH, conh-FONTH*3/2, curfps[0]);
                hudrect(conw-(showfpsrange ? 7 : 5)*FONTH, conh-FONTH*3/2, (showfpsrange ? 7 : 5)*FONTH - FONTH/2, FONTH);
                int ngxshow = hwrtngxmodeapplied();
                // 1 DLAA, 2-4 DLSS Q/B/P, 5 FSR Native, 6-8 FSR Q/B/P
                bool fsrshow = ngxshow >= 5;
                int srshow = fsrshow ? ngxshow - 4 : ngxshow;
                int labelw = srshow >= 2 ? 13 : (fsrshow ? 8 : 5);
                if(srshow >= 2) draw_textf("%s-%c %dx%d", conw-labelw*FONTH, conh-FONTH*3/2 - FONTH, fsrshow ? "FSR" : "DLSS", srshow==2?'Q':(srshow==3?'B':'P'), hwrtrenderw(), hwrtrenderh());
                else if(srshow == 1) draw_textf(fsrshow ? "FSR Native" : "DLAA", conw-labelw*FONTH, conh-FONTH*3/2 - FONTH);
                if(ngxshow) hudrect(conw-labelw*FONTH, conh-FONTH*3/2 - FONTH, labelw*FONTH - FONTH/2, FONTH);
                roffset += FONTH;
                if(ngxshow) roffset += FONTH;
            }

            if(wallclock)
            {
                if(!walltime) { walltime = time(NULL); walltime -= totalmillis/1000; if(!walltime) walltime++; }
                time_t walloffset = walltime + totalmillis/1000;
                struct tm *localvals = localtime(&walloffset);
                static string buf;
                if(localvals && strftime(buf, sizeof(buf), wallclocksecs ? (wallclock24 ? "%H:%M:%S" : "%I:%M:%S%p") : (wallclock24 ? "%H:%M" : "%I:%M%p"), localvals))
                {
                    // hack because not all platforms (windows) support %P lowercase option
                    // also strip leading 0 from 12 hour time
                    char *dst = buf;
                    const char *src = &buf[!wallclock24 && buf[0]=='0' ? 1 : 0];
                    while(*src) *dst++ = tolower(*src++);
                    *dst++ = '\0'; 
                    draw_text(buf, conw-5*FONTH, conh-FONTH*3/2-roffset);
                    hudrect(conw-5*FONTH, conh-FONTH*3/2-roffset, 5*FONTH - FONTH/2, FONTH);
                    roffset += FONTH;
                }
            }
                       
            hudend();

            if(editmode || showeditstats)
            {
                static int laststats = 0, prevstats[8] = { 0, 0, 0, 0, 0, 0, 0 }, curstats[8] = { 0, 0, 0, 0, 0, 0, 0 };
                if(totalmillis - laststats >= statrate)
                {
                    memcpy(prevstats, curstats, sizeof(prevstats));
                    laststats = totalmillis - (totalmillis%statrate);
                }
                int nextstats[8] =
                {
                    vtris*100/max(wtris, 1),
                    vverts*100/max(wverts, 1),
                    xtraverts/1024,
                    xtravertsva/1024,
                    glde,
                    gbatches,
                    getnumqueries(),
                    rplanes
                };
                loopi(8) if(prevstats[i]==curstats[i]) curstats[i] = nextstats[i];

                abovehud -= 2*FONTH;
                draw_textf("wtr:%dk(%d%%) wvt:%dk(%d%%) evt:%dk eva:%dk", FONTH/2, abovehud, wtris/1024, curstats[0], wverts/1024, curstats[1], curstats[2], curstats[3]);
                draw_textf("ond:%d va:%d gl:%d(%d) oq:%d lm:%d rp:%d pvs:%d", FONTH/2, abovehud+FONTH, allocnodes*8, allocva, curstats[4], curstats[5], curstats[6], lightmaps.length(), curstats[7], getnumviewcells());
                limitgui = abovehud;
            }

            if(editmode)
            {
                abovehud -= FONTH;
                draw_textf("cube %s%d%s", FONTH/2, abovehud, selchildcount<0 ? "1/" : "", abs(selchildcount), showmat && selchildmat > 0 ? getmaterialdesc(selchildmat, ": ") : "");

                if(char *editinfo = execidentstr("edithud"))
                {
                    if(editinfo[0])
                    {
                        int tw, th;
                        text_bounds(editinfo, tw, th);
                        th += FONTH-1; th -= th%FONTH;
                        abovehud -= max(th, FONTH);
                        draw_text(editinfo, FONTH/2, abovehud);
                    }
                    DELETEA(editinfo);
                }
            }
            else if(char *gameinfo = execidentstr("gamehud"))
            {
                if(gameinfo[0])
                {
                    int tw, th;
                    text_bounds(gameinfo, tw, th);
                    th += FONTH-1; th -= th%FONTH;
                    roffset += max(th, FONTH);
                    draw_text(gameinfo, conw-max(5*FONTH, 2*FONTH+tw), conh-FONTH/2-roffset);
                }
                DELETEA(gameinfo);
            }

            pophudmatrix();
        }

        if(hidestats || (!editmode && !showeditstats))
        {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            game::gameplayhud(w, h);
            limitgui = abovehud = min(abovehud, int(conh*game::abovegameplayhud(w, h)));
        }

        rendertexturepanel(w, h);
    }

    glDisable(GL_BLEND);

    g3d_limitscale((2*limitgui - conh) / float(conh));
    g3d_render2d();

    glEnable(GL_BLEND);

    hudmatrix.ortho(0, w, h, 0, -1, 1);
    resethudmatrix();

    pushhudmatrix();
    hudmatrix.scale(conscale, conscale, 1);
    flushhudmatrix();
    abovehud -= rendercommand(FONTH/2, abovehud - FONTH/2, conw-FONTH);
    extern int fullconsole, fullchat;
    if(!hidehud || fullconsole || fullchat || game::fullkilllog) renderconsole(conw, conh, abovehud - FONTH/2);
    pophudmatrix();

    extern void drawhudeditor(int w, int h);
    glEnable(GL_BLEND);
    drawhudeditor(w, h);
    glDisable(GL_BLEND);
    glEnable(GL_BLEND);
    drawcrosshair(w, h);
    hdrout_drawlabel();

    glDisable(GL_BLEND);
}

void cleanupgl()
{
    hdrout_shutdown();
    latency_cleanup();
    hwrtcleanup();

    cleanupmotionblur();
    cleanuplooktaa();
    cleanupvelocity();
    cleanuptemporal();

    clearminimap();

    cleanupscreenquad();

    gle::cleanup();
}


