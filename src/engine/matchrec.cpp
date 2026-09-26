#include "engine.h"
#include "SDL_mixer.h"

#ifdef WIN32
#include <objbase.h>
#include <shlobj.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <sys/stat.h>
#endif

extern bool nosound;

enum { MR_MAXW = 1920, MR_MAXH = 1080, MR_FPS = 60 };
#define MR_DIR "recordings"

VARP(matchrecord, 0, 1, 1);
static void mrprune();
VARFP(matchkeep, 1, 5, 30, { mrprune(); });
VARP(matchrecquality, 1, 2, 5);
VAR(matchrecdebug, 0, 0, 1);

static int mrw = 1280, mrh = 720, mrbitrate = 6000000, mrh264level = 32;
static int mrtexw = 0, mrtexh = 0;

static const struct mrqpreset { int w, h, bitrate, level; const char *name; } mrqtab[6] =
{
    { 1280,  720,  6000000, 32, "720p" },
    { 1280,  720,  3000000, 32, "720p low" },
    { 1280,  720,  6000000, 32, "720p" },
    { 1280,  720,  9000000, 32, "720p high" },
    { 1920, 1080,  8000000, 42, "1080p" },
    { 1920, 1080, 12000000, 42, "1080p high" }
};

static int mrqidx() { return clamp(matchrecquality, 1, 5); }

static int mrqest10min(int q)
{
    q = clamp(q, 1, 5);
    int bytes = (mrqtab[q].bitrate/8)*600 + 16000*600;
    return (bytes + 5000000)/10000000 * 10;
}

static void mrapplyquality()
{
    switch(matchrecquality)
    {
        case 1:  mrw = 1280; mrh = 720;  mrbitrate = 3000000;  mrh264level = 32; break;
        case 3:  mrw = 1280; mrh = 720;  mrbitrate = 9000000;  mrh264level = 32; break;
        case 4:  mrw = 1920; mrh = 1080; mrbitrate = 8000000;  mrh264level = 42; break;
        case 5:  mrw = 1920; mrh = 1080; mrbitrate = 12000000; mrh264level = 42; break;
        default: mrw = 1280; mrh = 720;  mrbitrate = 6000000;  mrh264level = 32; break;
    }
}

static bool mractive = false, mrwaiting = false, mrskipped = false, mrwelcomed = false;
static int mrwaitstart = 0, mrstartticks = 0, mrlastframe = -1;
static string mrfile, mrcurmap;
static GLuint mrfb = 0, mrtex = 0;

static void mrglcleanup()
{
    if(mrfb) { glDeleteFramebuffers_(1, &mrfb); mrfb = 0; }
    if(mrtex) { glDeleteTextures(1, &mrtex); mrtex = 0; }
    mrtexw = mrtexh = 0;
}

struct mrfileitem
{
    string path, label;
    time_t mtime;
    long size;
};
static vector<mrfileitem> mrfiles;
static int mrscanmillis = 0;

#ifdef WIN32

enum { MR_VQ = 3, MR_AQ = 48 };

struct mrvframe { uchar *bgra; int millis; };
static mrvframe mrvq[MR_VQ];
static int mrvhead = 0, mrvtail = 0, mrvcount = 0;

struct mraframe { uchar *data; int len, maxbytes, millis; };
static mraframe mraq[MR_AQ];
static int mrahead = 0, mratail = 0, mracount = 0;

static SDL_mutex *mrmutex = NULL;
static SDL_cond *mrcond = NULL;
static SDL_Thread *mrthread = NULL;
static volatile int mrstopflag = 0, mrreadyflag = 0, mrfailflag = 0;
static volatile int mruseNV12 = 1, mrwriterhasaudio = 0;
static volatile unsigned mrfailhr = 0;
static const char *mrfailstep = "";
static bool mraudio = false, mrup = false, mraudioreg = false;
static int mrfreq = 44100, mrchan = 2;

static void mrsetfail(const char *step, HRESULT hr)
{
    mrfailstep = step;
    mrfailhr = (unsigned)hr;
}

static void mrqinit()
{
    loopi(MR_VQ)
    {
        if(!mrvq[i].bgra) mrvq[i].bgra = new uchar[MR_MAXW*MR_MAXH*4];
        mrvq[i].millis = 0;
    }
    loopi(MR_AQ)
    {
        mraq[i].data = NULL;
        mraq[i].len = mraq[i].maxbytes = mraq[i].millis = 0;
    }
    mrvhead = mrvtail = mrvcount = 0;
    mrahead = mratail = mracount = 0;
}

static void mrqfree()
{
    loopi(MR_VQ) DELETEA(mrvq[i].bgra);
    loopi(MR_AQ) DELETEA(mraq[i].data);
}

static void mrflip(uchar *dst, const uchar *src)
{
    const int stride = mrw*4;
    loopi(mrh) memcpy(dst + i*stride, src + (mrh-1-i)*stride, stride);
}

static void mrbgra2nv12(uchar *nv12, const uchar *bgra)
{
    uchar *yp = nv12;
    uchar *uv = nv12 + mrw*mrh;
    loopi(mrh)
    {
        const uchar *src = bgra + (mrh-1-i)*mrw*4;
        uchar *yd = yp + i*mrw;
        loopj(mrw)
        {
            int b = src[j*4], g = src[j*4+1], r = src[j*4+2];
            yd[j] = (uchar)clamp(((66*r + 129*g + 25*b + 128)>>8) + 16, 0, 255);
        }
    }
    for(int i = 0; i < mrh; i += 2)
    {
        const uchar *s0 = bgra + (mrh-1-i)*mrw*4;
        const uchar *s1 = bgra + (mrh-2-i)*mrw*4;
        uchar *d = uv + (i/2)*mrw;
        for(int j = 0; j < mrw; j += 2)
        {
            int b = (s0[j*4]   + s0[j*4+4] + s1[j*4]   + s1[j*4+4]) >> 2;
            int g = (s0[j*4+1] + s0[j*4+5] + s1[j*4+1] + s1[j*4+5]) >> 2;
            int r = (s0[j*4+2] + s0[j*4+6] + s1[j*4+2] + s1[j*4+6]) >> 2;
            d[j]   = (uchar)clamp(((-38*r - 74*g + 112*b + 128)>>8) + 128, 0, 255);
            d[j+1] = (uchar)clamp(((112*r - 94*g - 18*b + 128)>>8) + 128, 0, 255);
        }
    }
}

static HRESULT mrwritesample(IMFSinkWriter *writer, DWORD stream, const uchar *data, DWORD bytes, LONGLONG time, LONGLONG duration)
{
    IMFMediaBuffer *buf = NULL;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
    if(FAILED(hr)) return hr;
    BYTE *lockp = NULL;
    hr = buf->Lock(&lockp, NULL, NULL);
    if(SUCCEEDED(hr))
    {
        memcpy(lockp, data, bytes);
        buf->Unlock();
        buf->SetCurrentLength(bytes);
    }
    IMFSample *sample = NULL;
    if(SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if(SUCCEEDED(hr))
    {
        sample->AddBuffer(buf);
        sample->SetSampleTime(time);
        sample->SetSampleDuration(duration);
        hr = writer->WriteSample(stream, sample);
        sample->Release();
    }
    buf->Release();
    return hr;
}

static HRESULT mraddvideo(IMFSinkWriter *writer, DWORD *vstream, bool nv12)
{
    IMFMediaType *outv = NULL;
    HRESULT hr = MFCreateMediaType(&outv);
    if(FAILED(hr)) return hr;
    outv->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outv->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outv->SetUINT32(MF_MT_AVG_BITRATE, mrbitrate);
    outv->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outv->SetUINT32(MF_MT_MPEG2_PROFILE, 100); // H.264 High
    outv->SetUINT32(MF_MT_MPEG2_LEVEL, mrh264level);
    MFSetAttributeSize(outv, MF_MT_FRAME_SIZE, mrw, mrh);
    MFSetAttributeRatio(outv, MF_MT_FRAME_RATE, MR_FPS, 1);
    MFSetAttributeRatio(outv, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = writer->AddStream(outv, vstream);
    outv->Release();
    if(FAILED(hr)) return hr;

    IMFMediaType *inv = NULL;
    hr = MFCreateMediaType(&inv);
    if(FAILED(hr)) return hr;
    inv->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inv->SetGUID(MF_MT_SUBTYPE, nv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
    inv->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(inv, MF_MT_FRAME_SIZE, mrw, mrh);
    MFSetAttributeRatio(inv, MF_MT_FRAME_RATE, MR_FPS, 1);
    MFSetAttributeRatio(inv, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if(nv12)
    {
        inv->SetUINT32(MF_MT_DEFAULT_STRIDE, mrw);
        inv->SetUINT32(MF_MT_SAMPLE_SIZE, mrw*mrh*3/2);
    }
    else inv->SetUINT32(MF_MT_DEFAULT_STRIDE, mrw*4);
    hr = writer->SetInputMediaType(*vstream, inv, NULL);
    inv->Release();
    return hr;
}

static HRESULT mraddaudio(IMFSinkWriter *writer, DWORD *astream)
{
    IMFMediaType *outa = NULL;
    HRESULT hr = MFCreateMediaType(&outa);
    if(FAILED(hr)) return hr;
    outa->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outa->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outa->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, mrchan);
    outa->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, mrfreq);
    outa->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outa->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
    outa->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    hr = writer->AddStream(outa, astream);
    outa->Release();
    if(FAILED(hr)) return hr;

    IMFMediaType *ina = NULL;
    hr = MFCreateMediaType(&ina);
    if(FAILED(hr)) return hr;
    ina->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ina->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    ina->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, mrchan);
    ina->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, mrfreq);
    ina->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    ina->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, mrchan*2);
    ina->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, mrfreq*mrchan*2);
    hr = writer->SetInputMediaType(*astream, ina, NULL);
    ina->Release();
    return hr;
}

static void mrreleasewriter(IMFSinkWriter **writer)
{
    if(*writer) { (*writer)->Release(); *writer = NULL; }
}

static HRESULT mropenwriter_try(IMFSinkWriter **writer, DWORD *vstream, DWORD *astream, bool *hasaudio, bool hw, bool nv12, bool audio)
{
    *writer = NULL;
    *hasaudio = false;
    *astream = 0;
    wchar_t wpath[2048];
    MultiByteToWideChar(CP_ACP, 0, mrfile, -1, wpath, 2048);

    IMFAttributes *attrs = NULL;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    if(FAILED(hr)) { mrsetfail("attrs", hr); return hr; }
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw ? TRUE : FALSE);
    attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    hr = MFCreateSinkWriterFromURL(wpath, NULL, attrs, writer);
    attrs->Release();
    if(FAILED(hr)) { mrsetfail("file", hr); return hr; }

    hr = mraddvideo(*writer, vstream, nv12);
    if(FAILED(hr)) { mrsetfail(nv12 ? "h264-nv12" : "h264-rgb", hr); return hr; }

    if(audio)
    {
        hr = mraddaudio(*writer, astream);
        if(FAILED(hr)) { mrsetfail("audio", hr); return hr; }
        *hasaudio = true;
    }
    hr = (*writer)->BeginWriting();
    if(FAILED(hr)) mrsetfail("begin", hr);
    else
    {
        mruseNV12 = nv12 ? 1 : 0;
        mrwriterhasaudio = *hasaudio ? 1 : 0;
    }
    return hr;
}

static HRESULT mropenwriter(IMFSinkWriter **writer, DWORD *vstream, DWORD *astream, bool *hasaudio)
{
    static const struct { bool hw, nv12, audio; } tries[] = {
        { true,  true,  true  },
        { true,  true,  false },
        { false, true,  true  },
        { false, true,  false },
        { false, false, false }
    };
    HRESULT hr = E_FAIL;
    loopi(int(sizeof(tries)/sizeof(tries[0])))
    {
        if(tries[i].audio && !mraudio) continue;
        mrreleasewriter(writer);
        hr = mropenwriter_try(writer, vstream, astream, hasaudio, tries[i].hw, tries[i].nv12, tries[i].audio);
        if(SUCCEEDED(hr)) return hr;
        mrreleasewriter(writer);
        if(mrfailstep && !strcmp(mrfailstep, "file")) break;
        if(mrfailstep && !strcmp(mrfailstep, "attrs")) break;
    }
    return hr;
}

static int SDLCALL mrencoder(void *)
{
    HRESULT cohr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool couninit = SUCCEEDED(cohr);
    if(FAILED(cohr) && cohr != RPC_E_CHANGED_MODE)
    {
        mrsetfail("com", cohr);
        mrreadyflag = -1;
        return 0;
    }
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if(FAILED(hr))
    {
        mrsetfail("startup", hr);
        mrreadyflag = -1;
        if(couninit) CoUninitialize();
        return 0;
    }
    IMFSinkWriter *writer = NULL;
    DWORD vstream = 0, astream = 0;
    bool hasaudio = false;
    hr = mropenwriter(&writer, &vstream, &astream, &hasaudio);
    if(FAILED(hr) || !writer)
    {
        mrfailflag = 1;
        mrreadyflag = -1;
        mrreleasewriter(&writer);
        MFShutdown();
        if(couninit) CoUninitialize();
        return 0;
    }
    mrreadyflag = 1;

    static uchar *rawbuf = NULL, *nv12buf = NULL, *rgbout = NULL;
    if(!rawbuf) rawbuf = new uchar[MR_MAXW*MR_MAXH*4];
    if(!nv12buf) nv12buf = new uchar[MR_MAXW*MR_MAXH*3/2];
    const int framebytes = mrw*mrh*4;
    const int nv12bytes = mrw*mrh*3/2;
    const LONGLONG vdur = 10000000LL / MR_FPS;
    uchar abuf[1<<16];

    for(;;)
    {
        SDL_LockMutex(mrmutex);
        while(!mrstopflag && mrvcount<=0 && mracount<=0) SDL_CondWait(mrcond, mrmutex);
        if(mrstopflag && mrvcount<=0 && mracount<=0)
        {
            SDL_UnlockMutex(mrmutex);
            break;
        }
        bool gotv = mrvcount>0;
        bool gota = hasaudio && mracount>0;
        int vms = 0, alen = 0, ams = 0;
        if(gotv)
        {
            memcpy(rawbuf, mrvq[mrvtail].bgra, framebytes);
            vms = mrvq[mrvtail].millis;
            mrvtail = (mrvtail+1)%MR_VQ;
            mrvcount--;
        }
        if(gota)
        {
            alen = min(mraq[mratail].len, (int)sizeof(abuf));
            ams = mraq[mratail].millis;
            if(alen>0 && mraq[mratail].data) memcpy(abuf, mraq[mratail].data, alen);
            mratail = (mratail+1)%MR_AQ;
            mracount--;
        }
        SDL_UnlockMutex(mrmutex);

        if(gotv)
        {
            if(mruseNV12)
            {
                mrbgra2nv12(nv12buf, rawbuf);
                mrwritesample(writer, vstream, nv12buf, nv12bytes, (LONGLONG)vms*10000LL, vdur);
            }
            else
            {
                if(!rgbout) rgbout = new uchar[MR_MAXW*MR_MAXH*4];
                mrflip(rgbout, rawbuf);
                mrwritesample(writer, vstream, rgbout, framebytes, (LONGLONG)vms*10000LL, vdur);
            }
        }
        if(gota && alen>0)
        {
            const uchar *pcm = abuf;
            uchar *up = NULL;
            int bytes = alen;
            if(mrup)
            {
                int samples = alen / max(mrchan*2, 1);
                bytes = samples*2*mrchan*2;
                up = new uchar[bytes];
                const short *in = (const short *)abuf;
                short *out = (short *)up;
                loopi(samples) loopj(mrchan)
                {
                    short s = in[i*mrchan+j];
                    out[(i*2)*mrchan+j] = s;
                    out[(i*2+1)*mrchan+j] = s;
                }
                pcm = up;
            }
            int nsamples = bytes / max(mrchan*2, 1);
            LONGLONG dur = (LONGLONG)nsamples * 10000000LL / max(mrfreq, 1);
            mrwritesample(writer, astream, pcm, bytes, (LONGLONG)ams*10000LL, dur);
            DELETEA(up);
        }
    }

    writer->Finalize();
    writer->Release();
    MFShutdown();
    if(couninit) CoUninitialize();
    return 0;
}

static void SDLCALL mraudiocb(int, void *stream, int len, void *)
{
    if(!mractive || mrstopflag) return;
    SDL_LockMutex(mrmutex);
    if(mracount < MR_AQ && len > 0)
    {
        mraframe &a = mraq[mrahead];
        if(a.maxbytes < len)
        {
            DELETEA(a.data);
            a.data = new uchar[len];
            a.maxbytes = len;
        }
        memcpy(a.data, stream, len);
        a.len = len;
        a.millis = max(int(SDL_GetTicks()) - mrstartticks, 0);
        mrahead = (mrahead+1)%MR_AQ;
        mracount++;
        SDL_CondSignal(mrcond);
    }
    SDL_UnlockMutex(mrmutex);
}

static void mrsanitize(char *dst, const char *src, int maxlen)
{
    int n = 0;
    for(const char *s = src; *s && n < maxlen-1; s++)
    {
        uchar c = (uchar)*s;
        if(isalnum(c) || c=='-' || c=='_') dst[n++] = char(c);
        else if(c==' ' || c=='.' || c=='/') dst[n++] = '-';
    }
    dst[n] = 0;
    if(!dst[0]) copystring(dst, "map", maxlen);
}

static void mrbuildname()
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    string stamp, mapname;
    if(lt) strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", lt);
    else copystring(stamp, "now");
    mrsanitize(mapname, game::getclientmap(), 64);
    if(homedir[0])
    {
        defformatstring(absdir, "%s%s", homedir, MR_DIR);
        createdir(absdir);
    }
    else createdir(MR_DIR);
    defformatstring(rel, "%s%c%s_%s.mp4", MR_DIR, PATHDIV, stamp, mapname);
    const char *found = findfile(rel, "w");
    copystring(mrfile, found ? found : rel);
    path(mrfile);
}

static bool mrstartthread()
{
    mrqinit();
    mrstopflag = mrfailflag = 0;
    mrreadyflag = 0;
    mrfailhr = 0;
    mrfailstep = "";
    mrwriterhasaudio = 0;
    mruseNV12 = 1;
    if(!mrmutex) mrmutex = SDL_CreateMutex();
    if(!mrcond) mrcond = SDL_CreateCond();
    mrthread = SDL_CreateThread(mrencoder, "matchrec", NULL);
    if(!mrthread) return false;
    int waited = 0;
    while(mrreadyflag==0 && waited < 8000)
    {
        SDL_Delay(10);
        waited += 10;
    }
    return mrreadyflag==1;
}

static void mrstopthread()
{
    if(mraudioreg)
    {
        Mix_UnregisterEffect(MIX_CHANNEL_POST, mraudiocb);
        mraudioreg = false;
    }
    mraudio = false;
    if(mrthread)
    {
        mrstopflag = 1;
        if(mrmutex && mrcond)
        {
            SDL_LockMutex(mrmutex);
            SDL_CondSignal(mrcond);
            SDL_UnlockMutex(mrmutex);
        }
        SDL_WaitThread(mrthread, NULL);
        mrthread = NULL;
    }
    mractive = false;
    mrwaiting = false;
}

static bool mrfileless(const mrfileitem &a, const mrfileitem &b) { return a.mtime < b.mtime; }

static void mrscan(bool force = false)
{
    if(!force && mrscanmillis && totalmillis - mrscanmillis < 400) return;
    mrscanmillis = totalmillis ? totalmillis : 1;
    mrfiles.setsize(0);
    vector<char *> names;
    if(homedir[0])
    {
        defformatstring(absdir, "%s%s", homedir, MR_DIR);
        listdir(absdir, false, "mp4", names);
    }
    else listdir(MR_DIR, true, "mp4", names);
    loopv(names)
    {
        mrfileitem &it = mrfiles.add();
        defformatstring(rel, "%s/%s.mp4", MR_DIR, names[i]);
        const char *found = findfile(rel, "r");
        copystring(it.path, found ? found : rel);
        formatstring(it.label, "%s.mp4", names[i]);
        it.mtime = 0;
        it.size = 0;
        struct stat st;
        if(stat(it.path, &st)==0)
        {
            it.mtime = st.st_mtime;
            it.size = (long)st.st_size;
        }
        if(mractive && !strcasecmp(it.path, mrfile)) mrfiles.pop();
        DELETEA(names[i]);
    }
    mrfiles.sort(mrfileless);
}

static void mrprune()
{
    mrscan(true);
    while(mrfiles.length() > matchkeep)
    {
        mrfileitem it = mrfiles.remove(0);
        remove(it.path);
    }
}

static void mrstart()
{
    if(mractive || !matchrecord) return;
    mrapplyquality();
    mrbuildname();
    mrstartticks = SDL_GetTicks();
    mrlastframe = -1;
    mrup = false;
    mrfreq = 44100;
    mrchan = 2;
    mraudio = false;
    if(!nosound)
    {
        int freq = 0, chans = 0;
        Uint16 fmt = 0;
        if(Mix_QuerySpec(&freq, &fmt, &chans)>0 && freq>0 && chans>0 && SDL_AUDIO_BITSIZE(fmt)==16 && !SDL_AUDIO_ISFLOAT(fmt))
        {
            mrchan = chans;
            if(freq==22050) { mrfreq = 44100; mrup = true; mraudio = true; }
            else if(freq==44100 || freq==48000) { mrfreq = freq; mraudio = true; }
            else mraudio = false;
        }
    }
    if(!mrstartthread())
    {
        conoutf(CON_ERROR, "match recording failed (%s 0x%08X)", mrfailstep[0] ? mrfailstep : "encoder", mrfailhr);
        mrstopthread();
        if(mrfile[0]) remove(mrfile);
        mrskipped = true;
        return;
    }
    if(!mrwriterhasaudio) mraudio = false;
    if(mraudio && Mix_RegisterEffect(MIX_CHANNEL_POST, mraudiocb, NULL, NULL)==0)
    {
        mraudio = false;
        if(matchrecdebug) conoutf(CON_WARN, "match recording: game sound unavailable");
    }
    else if(mraudio) mraudioreg = true;
    mractive = true;
    mrwaiting = false;
    mrskipped = false;
    copystring(mrcurmap, game::getclientmap());
    if(!mrwelcomed)
    {
        mrwelcomed = true;
        conoutf("auto-recording this match. options \f0record\f7 to extract / keep count");
    }
    else if(matchrecdebug) conoutf("recording match: %s", mrfile);
}

static void mrfinish()
{
    if(!mractive)
    {
        mrwaiting = false;
        return;
    }
    int duration = max(int(SDL_GetTicks()) - mrstartticks, 0);
    string finished;
    copystring(finished, mrfile);
    mrcurmap[0] = 0;
    mrstopthread();
    mrglcleanup();
    if(duration < 8000)
    {
        remove(finished);
        if(matchrecdebug) conoutf("dropped short recording (%d ms)", duration);
    }
    else mrprune();
}

#else

static void mrscan(bool = false) {}
static void mrstart()
{
    conoutf(CON_ERROR, "match recording requires Windows");
    mrskipped = true;
    mrwaiting = false;
}
static void mrfinish() { mractive = false; mrwaiting = false; }
static void mrstopthread() { mractive = false; }
static void mrprune() {}
static void mrqfree() {}

#endif

namespace matchrec
{
    void stop() { mrfinish(); }

    void cleanup()
    {
        mrfinish();
#ifdef WIN32
        mrqfree();
        if(mrmutex) { SDL_DestroyMutex(mrmutex); mrmutex = NULL; }
        if(mrcond) { SDL_DestroyCond(mrcond); mrcond = NULL; }
#endif
        mrglcleanup();
    }

    void onstartgame(bool skip)
    {
        const char *map = game::getclientmap();
        if(mractive && map[0] && mrcurmap[0] && !strcasecmp(map, mrcurmap))
        {
            // Same map sent again (round restart / join handshake): keep this file.
            mrskipped = false;
            mrwaiting = false;
            return;
        }
        mrfinish();
        mrskipped = false;
        mrwaiting = false;
        if(!matchrecord || skip || mainmenu)
        {
            mrskipped = true;
            return;
        }
        mrwaiting = true;
        mrwaitstart = totalmillis;
    }

    void onintermission()
    {
        // Keep recording through the scoreboard. The file closes on a real map
        // change or disconnect, so one match stays one video.
        if(!mractive)
        {
            mrwaiting = false;
            mrskipped = true;
        }
    }
    void ondisconnect() { mrfinish(); mrskipped = true; }

    void onselfspawn()
    {
        if(mrwaiting && !mrskipped) mrstart();
    }

    void onselfspectator(bool spec)
    {
        // Stay waiting while spectating: a mid-match join starts in spec,
        // then records as soon as you actually enter the game.
        if(!spec && mrwaiting && !mrskipped) mrstart();
    }

    void capture()
    {
        if(!matchrecord)
        {
            if(mractive) mrfinish();
            return;
        }
        if(mrwaiting && !mractive && !mrskipped)
        {
            // Wait a moment: on join the player is briefly "alive" before the
            // spectator packet arrives, which used to create a tiny extra file.
            if(player && player->state!=CS_SPECTATOR && totalmillis - mrwaitstart > 2000) mrstart();
        }
        if(!mractive) return;
#ifdef WIN32
        if(mrfailflag)
        {
            conoutf(CON_ERROR, "match recording stopped (encoder error)");
            mrfinish();
            return;
        }
        int now = max(int(SDL_GetTicks()) - mrstartticks, 0);
        int frame = (now * MR_FPS) / 1000;
        if(frame <= mrlastframe) return;
        mrlastframe = frame;
        if(!mrmutex) return;

        SDL_LockMutex(mrmutex);
        bool full = mrvcount >= MR_VQ;
        mrvframe *slot = full ? NULL : &mrvq[mrvhead];
        SDL_UnlockMutex(mrmutex);
        if(!slot || !slot->bgra) return;

        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        if(mrtex && (mrtexw!=mrw || mrtexh!=mrh)) mrglcleanup();
        if(hasFBB && hasFBO && (screenw!=mrw || screenh!=mrh))
        {
            if(!mrtex)
            {
                glGenTextures(1, &mrtex);
                glBindTexture(GL_TEXTURE_2D, mrtex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, mrw, mrh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                mrtexw = mrw;
                mrtexh = mrh;
            }
            if(!mrfb)
            {
                glGenFramebuffers_(1, &mrfb);
                glBindFramebuffer_(GL_FRAMEBUFFER, mrfb);
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mrtex, 0);
                glBindFramebuffer_(GL_FRAMEBUFFER, 0);
            }
            glBindFramebuffer_(GL_READ_FRAMEBUFFER, 0);
            glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, mrfb);
            glBlitFramebuffer_(0, 0, screenw, screenh, 0, 0, mrw, mrh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer_(GL_FRAMEBUFFER, mrfb);
            glReadPixels(0, 0, mrw, mrh, GL_BGRA, GL_UNSIGNED_BYTE, slot->bgra);
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);
        }
        else if(screenw==mrw && screenh==mrh)
            glReadPixels(0, 0, mrw, mrh, GL_BGRA, GL_UNSIGNED_BYTE, slot->bgra);
        else
        {
            memset(slot->bgra, 0, mrw*mrh*4);
            glReadPixels(0, 0, min(screenw, mrw), min(screenh, mrh), GL_BGRA, GL_UNSIGNED_BYTE, slot->bgra);
        }

        slot->millis = now;
        SDL_LockMutex(mrmutex);
        mrvhead = (mrvhead+1)%MR_VQ;
        mrvcount++;
        SDL_CondSignal(mrcond);
        SDL_UnlockMutex(mrmutex);
#endif
    }
}

#ifdef WIN32
static void mrpickfolder(char *dst, int dstlen)
{
    dst[0] = 0;
    BROWSEINFOA bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = NULL;
    bi.lpszTitle = "Copy this recording to...";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if(!pidl) return;
    SHGetPathFromIDListA(pidl, dst);
    CoTaskMemFree(pidl);
    (void)dstlen;
}

ICOMMAND(nummatchrecs, "", (), { mrscan(); intret(mrfiles.length()); });
ICOMMAND(matchrecname, "i", (int *i),
{
    mrscan();
    if(!mrfiles.inrange(*i)) { result(""); return; }
    mrfileitem &it = mrfiles[*i];
    defformatstring(s, "%s  (%d MB)", it.label, max(int((it.size+500000)/1000000), 0));
    result(s);
});
ICOMMAND(matchrecextract, "i", (int *i),
{
    mrscan(true);
    if(!mrfiles.inrange(*i)) { conoutf(CON_WARN, "no recording there"); return; }
    mrfileitem it = mrfiles[*i];
    string destfolder;
    mrpickfolder(destfolder, sizeof(destfolder));
    if(!destfolder[0]) return;
    defformatstring(dest, "%s\\%s", destfolder, it.label);
    if(CopyFileA(it.path, dest, FALSE))
        conoutf("copied to %s (original still in the game folder)", dest);
    else
        conoutf(CON_ERROR, "could not copy the video");
});
#else
ICOMMAND(nummatchrecs, "", (), intret(0));
ICOMMAND(matchrecname, "i", (int *i), result(""));
ICOMMAND(matchrecextract, "i", (int *i), conoutf(CON_ERROR, "match recording requires Windows"));
#endif

ICOMMAND(matchrecest, "", (),
{
    defformatstring(s, "~%d MB / 10 min", mrqest10min(mrqidx()));
    result(s);
});

ICOMMAND(matchrecrecording, "", (), intret(mractive ? 1 : 0));

ICOMMAND(togglematchrecord, "", (),
{
    matchrecord = matchrecord ? 0 : 1;
    if(!matchrecord)
    {
        mrfinish();
        conoutf("match recording off");
    }
    else conoutf("match recording on");
});

ICOMMAND(matchrecqname, "", (),
{
    int q = mrqidx();
    defformatstring(s, "%s  (~%d MB / 10 min)", mrqtab[q].name, mrqest10min(q));
    result(s);
});
