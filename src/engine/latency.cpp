// Low latency mode: a home-made take on NVIDIA Reflex for this OpenGL client.
//
// When the GPU is the bottleneck, the driver lets the CPU queue whole frames
// ahead of it: the mouse is read, the frame is recorded, then it waits in the
// driver queue (or inside the swap) while older frames finish. The mouse state
// it shows is that much older. This moves the waiting to *before* the input is
// read, so the frame starts from the freshest mouse state:
//  - a GL fence after every swap caps the queue at one frame ahead;
//  - lowlatency 1 also sleeps just long enough that the next frame's commands
//    reach the GPU as it runs out of work (measured with GL timestamps), so the
//    GPU stays busy and the frame rate is kept;
//  - lowlatency 2 waits for the GPU to finish the previous frame outright:
//    lowest latency, costs the CPU time of a frame in frame rate.
// Mouse handling is untouched (sensitivity, acceleration, raw input): only the
// moment it is read changes. Nothing here generates or reorders frames.
#include "engine.h"

VARP(lowlatency, 0, 1, 2); // On by default; Off runs the original code paths
// Work kept queued ahead of the GPU by lowlatency 1, in microseconds. Smaller
// cuts latency further but lets the GPU idle more often.
VAR(lowlatencymargin, 0, 500, 5000);

static PFNGLFENCESYNCPROC llFenceSync = NULL;
static PFNGLCLIENTWAITSYNCPROC llClientWaitSync = NULL;
static PFNGLDELETESYNCPROC llDeleteSync = NULL;
static PFNGLGENQUERIESPROC llGenQueries = NULL;
static PFNGLDELETEQUERIESPROC llDeleteQueries = NULL;
static PFNGLQUERYCOUNTERPROC llQueryCounter = NULL;
static PFNGLGETQUERYOBJECTIVPROC llGetQueryObjectiv = NULL;
static PFNGLGETQUERYOBJECTUI64VPROC llGetQueryObjectui64v = NULL;
static PFNGLGETINTEGER64VPROC llGetInteger64v = NULL;
static int llstate = 0; // 0 not tried, 1 ready, -1 unavailable

// Milliseconds on the performance counter (QueryPerformanceCounter on Windows).
double latency_now()
{
    static double freq = 0;
    if(!freq) freq = double(SDL_GetPerformanceFrequency()) / 1000.0;
    return double(SDL_GetPerformanceCounter()) / freq;
}

enum { LL_RING = 8, LL_WAITS = 16 };

struct llframe
{
    GLsync fence;
    GLuint query;
    bool pending;
    int serial, mode;
    double sample, swapstart, swapend, done, waited, slept, blocked;
    double wait0[LL_WAITS], wait1[LL_WAITS];
    int waits;
};

static llframe ring[LL_RING];
static int llserial = 0;
static double cursample = -1, curswapstart = -1, curwaited = 0, curslept = 0, curblocked = 0;
static double gpuoffset = 0, lastcalib = -1e30;
static double pacedelay = 0, gpuinterval = 0;
static double curwait0[LL_WAITS], curwait1[LL_WAITS];
static int curwaits = 0;
static int lastmode = 0, waittimeouts = 0;


static bool tracking()
{
    if(lowlatency) return true;
    return false;
}

// Waits for the GPU inside the frame, after the input was read: occlusion
// query results and Vulkan fences of earlier frames. lowlatency 1 moves that
// time before the input read. Only watched while it matters, so Off runs the
// original code paths.
bool latency_watchblocks()
{
    if(lowlatency == 1) return true;
    return false;
}

void latency_blocked(double start, double end)
{
    curblocked += end - start;
    if(curwaits < LL_WAITS) { curwait0[curwaits] = start; curwait1[curwaits] = end; curwaits++; }
    else curwait1[LL_WAITS-1] = end;
}

static bool loadgl()
{
    if(llstate) return llstate > 0;
    llFenceSync = (PFNGLFENCESYNCPROC)getprocaddress("glFenceSync");
    llClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)getprocaddress("glClientWaitSync");
    llDeleteSync = (PFNGLDELETESYNCPROC)getprocaddress("glDeleteSync");
    llGenQueries = (PFNGLGENQUERIESPROC)getprocaddress("glGenQueries");
    llDeleteQueries = (PFNGLDELETEQUERIESPROC)getprocaddress("glDeleteQueries");
    llQueryCounter = (PFNGLQUERYCOUNTERPROC)getprocaddress("glQueryCounter");
    llGetQueryObjectiv = (PFNGLGETQUERYOBJECTIVPROC)getprocaddress("glGetQueryObjectiv");
    llGetQueryObjectui64v = (PFNGLGETQUERYOBJECTUI64VPROC)getprocaddress("glGetQueryObjectui64v");
    llGetInteger64v = (PFNGLGETINTEGER64VPROC)getprocaddress("glGetInteger64v");
    bool ok = llFenceSync && llClientWaitSync && llDeleteSync && llGenQueries && llDeleteQueries &&
              llQueryCounter && llGetQueryObjectiv && llGetQueryObjectui64v && llGetInteger64v;
    if(ok)
    {
        GLuint q[LL_RING];
        llGenQueries(LL_RING, q);
        loopi(LL_RING) { memset(&ring[i], 0, sizeof(ring[i])); ring[i].query = q[i]; ring[i].serial = -1; }
    }
    llstate = ok ? 1 : -1;
    logoutf("lowlatency: GL fences and timestamps %s", ok ? "ready" : "unavailable, mode has no effect");
    return ok;
}

// GL timestamps and the CPU clock are separate; the offset is re-measured once
// a second from the tightest of three brackets.
static void calibrate()
{
    double now = latency_now();
    if(now - lastcalib < 1000) return;
    double best = 1e30, off = gpuoffset;
    loopi(3)
    {
        GLint64 g = 0;
        double a = latency_now();
        llGetInteger64v(GL_TIMESTAMP, &g);
        double b = latency_now();
        if(b - a < best) { best = b - a; off = 0.5*(a + b) - double(g)*1e-6; }
    }
    gpuoffset = off;
    lastcalib = now;
}

// Shortest recent swap call: what a swap costs when nothing is queued in front
// of it. Anything above that was time spent waiting for a queue to drain.
enum { LL_SWAPS = 128 };
static double swaps[LL_SWAPS];
static int swapn = 0, swapi = 0;
static double swapmin()
{
    double m = 1e30;
    loopi(swapn) m = min(m, swaps[i]);
    return swapn ? m : 0;
}

// Frame s+1 recorded its commands while frame s was still on the GPU: the gap
// between the end of that recording and the end of frame s is how long the new
// frame's commands sat in the queue. With V-Sync the queue sits in the swap
// instead, which then blocks. Either way, positive: the input could have been
// read that much later. Negative: the GPU ran dry. Steer the pre-input sleep so
// a small margin stays queued.
static void steer(const llframe &prev, const llframe &next)
{
    if(next.mode != 1 || lowlatency != 1) return;
    double margin = lowlatencymargin / 1000.0;
    double gpuq = prev.done - next.swapstart, queue = gpuq;
    // Only with V-Sync does a long swap mean a queue: without it, a long swap
    // is the driver doing real work, and sleeping would not shorten it. And
    // once the GPU is starved, nothing counts as queued.
    extern int curvsync;
    if(curvsync > 0 && gpuq > -margin) queue = max(queue, (next.swapend - next.swapstart) - swapmin());
    // A wait inside the frame for the previous frame is the same queue, met
    // after the input read. Only the part before the previous frame finished
    // counts: past that the frame waits on its own GPU work, which no sleep
    // before the input read can shorten.
    double early = 0;
    loopi(next.waits) early += max(0.0, min(next.wait1[i], prev.done) - max(next.wait0[i], next.sample));
    queue = max(queue, early);
    double err = queue - margin;
    // Give time back quickly when the GPU idled, take it slowly otherwise.
    pacedelay += err * (err < 0 ? 0.75 : 0.35);
    // Never sleep most of a GPU frame away, whatever the signals say.
    pacedelay = clamp(pacedelay, 0.0, gpuinterval > 0 ? 0.8*gpuinterval : 100.0);
}


static void finish(llframe &f)
{
    GLuint64 ns = 0;
    llGetQueryObjectui64v(f.query, GL_QUERY_RESULT, &ns);
    f.done = double(ns)*1e-6 + gpuoffset;
    if(f.fence) { llDeleteSync(f.fence); f.fence = NULL; }
    f.pending = false;
    const llframe &before = ring[(f.serial + LL_RING - 1) % LL_RING];
    if(before.serial == f.serial - 1 && !before.pending)
    {
        double d = f.done - before.done;
        if(d > 0 && d < 250) gpuinterval = gpuinterval > 0 ? gpuinterval + (d - gpuinterval)*0.1 : d;
    }
    // The next frame is usually recorded already; otherwise endframe steers.
    const llframe &next = ring[(f.serial + 1) % LL_RING];
    if(next.serial == f.serial + 1) steer(f, next);
}

static void harvest()
{
    for(int s = max(llserial - LL_RING, 0); s < llserial; s++)
    {
        llframe &f = ring[s % LL_RING];
        if(f.serial != s || !f.pending) continue;
        GLint avail = 0;
        llGetQueryObjectiv(f.query, GL_QUERY_RESULT_AVAILABLE, &avail);
        if(!avail) break;
        finish(f);
    }
}

static void dropall()
{
    loopi(LL_RING)
    {
        llframe &f = ring[i];
        if(f.fence) { llDeleteSync(f.fence); f.fence = NULL; }
        if(f.pending)
        {
            GLuint64 ns = 0;
            llGetQueryObjectui64v(f.query, GL_QUERY_RESULT, &ns);
            f.pending = false;
        }
        f.serial = -1;
    }
    pacedelay = 0;
}

// Sleep() alone rounds to the millisecond: a high resolution waitable timer
// takes the bulk, a short spin the rest.
#ifdef WIN32
typedef HANDLE (WINAPI *CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
static HANDLE lltimer = NULL;
static bool lltimertried = false;
#endif
static void sleepuntil(double target)
{
#ifdef WIN32
    if(!lltimertried)
    {
        lltimertried = true;
        CreateWaitableTimerExW_t create = (CreateWaitableTimerExW_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExW");
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+), plain timer otherwise.
        if(create) lltimer = create(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
        if(!lltimer && create) lltimer = create(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
#endif
    for(;;)
    {
        double left = target - latency_now();
        if(left <= 0) return;
        if(left > 1.0)
        {
#ifdef WIN32
            if(lltimer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -LONGLONG((left - 0.5) * 10000.0);
                if(SetWaitableTimer(lltimer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(lltimer, 200); continue; }
            }
#endif
            SDL_Delay(Uint32(left - 1.0));
            continue;
        }
        SDL_Delay(0);
    }
}

// Top of the main loop, before the frame limiter and before any input is read.
void latency_wait()
{
    curwaited = curslept = 0;
    if(lowlatency != lastmode)
    {
        if(llstate > 0 && !tracking()) dropall();
        pacedelay = 0;
        lastmode = lowlatency;
    }
    if(!tracking() || llstate <= 0) return;
    if(lowlatency)
    {
        int cap = lowlatency >= 2 ? llserial - 1 : llserial - 2;
        if(cap >= 0)
        {
            llframe &f = ring[cap % LL_RING];
            if(f.serial == cap && f.fence)
            {
                double t0 = latency_now();
                GLenum r = llClientWaitSync(f.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);
                curwaited = latency_now() - t0;
                if(r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED)
                {
                    if(waittimeouts++ < 4) logoutf("lowlatency: GPU wait gave up after %.1f ms (0x%X)", curwaited, unsigned(r));
                    pacedelay = 0;
                }
            }
        }
    }
    harvest();
    if(lowlatency == 1 && pacedelay > 0.02)
    {
        double t0 = latency_now();
        sleepuntil(t0 + pacedelay);
        curslept = latency_now() - t0;
    }
}

// Right before the input is pumped: the moment the frame's mouse state is taken.
void latency_sample()
{
    cursample = latency_now();
    curblocked = 0;
    curwaits = 0;
}

void latency_swapstart()
{
    curswapstart = latency_now();
}

// After the swap (SDL or HDR Present) returned.
void latency_endframe()
{
    if(!tracking() || !loadgl()) return;
    calibrate();
    llframe &f = ring[llserial % LL_RING];
    if(f.pending) finish(f);
    if(f.fence) { llDeleteSync(f.fence); f.fence = NULL; }
    f.serial = llserial;
    f.mode = lowlatency;
    f.sample = cursample;
    f.swapstart = curswapstart;
    f.swapend = latency_now();
    f.done = 0;
    f.waited = curwaited;
    f.slept = curslept;
    f.blocked = curblocked;
    f.waits = curwaits;
    loopi(curwaits) { f.wait0[i] = curwait0[i]; f.wait1[i] = curwait1[i]; }
    swaps[swapi] = f.swapend - f.swapstart;
    swapi = (swapi + 1) % LL_SWAPS;
    if(swapn < LL_SWAPS) swapn++;
    llQueryCounter(f.query, GL_TIMESTAMP);
    f.fence = llFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    f.pending = true;
    // The previous frame already finished before this one was recorded: the
    // GPU was waiting on the CPU, which steer() reads as negative slack.
    const llframe &prev = ring[(llserial + LL_RING - 1) % LL_RING];
    if(prev.serial == llserial - 1 && !prev.pending) steer(prev, f);
    llserial++;
    // A stall (map load, alt-tab) is not a queue to steer against.
    if(f.swapend - f.sample > 250) pacedelay = 0;
}

void latency_cleanup()
{
    if(llstate > 0)
    {
        dropall();
        GLuint q[LL_RING];
        loopi(LL_RING) q[i] = ring[i].query;
        llDeleteQueries(LL_RING, q);
    }
    llstate = 0;
    llserial = 0;
}
