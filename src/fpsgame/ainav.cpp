// ainav.cpp: a walk grid for the Human bots, made by this game from the map itself (no waypoint file).
//
// What it is: one column every 8 u (16 u on very large maps), one node per floor a player can stand on in
// that column, and the links a player can take between them: walking (steps up to 4.1 u, walkable slopes, and
// bumpy floors followed every 2 u), dropping off an edge, jumping onto a ledge (36 u at most) or over a gap or a
// low obstacle, jump pads (with air control), teleporters and getting out of the water. Floors are found by a ray
// and checked with the game's own collide() (so clip and the collision boxes of map models count, noclip does
// not: what is under it does); every drop, jump, pad flight, teleport and swim is simulated with the game's own
// physics (moveplayer) and linked to where the simulated player really lands. Lava and death are left out.
//
// When: only while this game plays a Human or Tiny Apa bot (botperception 1 or 2, or a bot the server asks
// to play as Tiny Apa). Classic bots, games without bots and spectators never build it. The work is spread
// over the frames (navbuildshare % of the frame time, a tenth by default, 2 ms at most per frame; NAV_WORKMAX of work
// at most for the simulations of a map); the finished grid is kept on disk, one file per map and map version (profile
// folder navcache, navcachemb MB at most, 32 by default, 0 none; oldest files go first) and loaded the next time. Coop edit: the zone around
// each change is rebuilt and joined to the rest a moment after the edits stop; an edited map is never written to
// the cache. Paths: hierarchical A* (clusters of 16x16 columns, 8x8 on small grids, joined by their entrances).
//
// The Human and Tiny Apa bots find their way on it (ainavbot.h, included at the end); they keep the waypoints as
// before while the grid is missing, still building or failed. Nothing is sent online; other clients are not concerned.

#include "game.h"
#ifndef WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

extern const float JUMPVEL, GRAVITY; // engine/physics.cpp

namespace ai
{
    using namespace game;

    extern int navbotmode(fpsent *d); // ai.cpp: 0 Classic (or not a bot of this game), 1 Human, 2 Tiny Apa

    // ---- what a link asks of the player who takes it
    enum
    {
        NAV_WALK = 0,   // walk (steps up to 4.1 u, walkable slopes)
        NAV_DROP,       // walk off an edge and land below (simulated)
        NAV_JUMP,       // jump onto a ledge higher than a step (simulated)
        NAV_GAP,        // running jump over a void (simulated)
        NAV_PADIN,      // step onto a jump pad (to the pad's launch node)
        NAV_PAD,        // flight of a jump pad, keys as stored (simulated)
        NAV_TELEIN,     // step into a teleporter (to the teleporter's node)
        NAV_TELE,       // teleport and land at the destination (simulated)
        NAV_SWIM,       // swim up and get out of the water (simulated)
        NAV_STRAFE,     // reserved: strafe jump (not produced yet)
        NAV_ROCKET,     // reserved: rocket jump (not produced yet)
        NAV_GRENADE,    // reserved: grenade jump (not produced yet)
        NAV_NUMTYPES,
        NAV_DELETED = 255
    };
    enum { NAVF_WATER = 1<<0, NAVF_SEED = 1<<1, NAVF_SPAWN = 1<<2, NAVF_SLOPE = 1<<3, NAVF_DEAD = 1<<4, NAVF_VIRTUAL = 1<<5, NAVF_ITEM = 1<<6, NAVF_EDGE = 1<<7 };
    // link timing (10 ms units): keys held from t0 to t1; t1 NAVT_LANDING: from t0 until the landing, NAVT_TAKEOFF:
    // until the feet leave the floor, NAVT_OVER: until the feet are t0 u (horizontally) from where they started
    enum { NAVT_LANDING = 255, NAVT_TAKEOFF = 254, NAVT_OVER = 253 };

    static const int NAV_VERSION = 2;            // a grid made by another version is never loaded (2: jump pads and teleporters
                                                 // are entered, never walked through)
    extern int navcachemb;                       // all cached grids together, MB (oldest files go first); 0: no cache
    static const int NAV_MAXNODES = 150000;      // at 8 u; beyond, the grid is made again at 16 u (same limit)
    static const int NAV_BIGMAP = 60000;         // a very large map (estimated floor columns at 8 u): 16 u from the start
    static const int NAV_CLUSTER = 16;           // columns per side of a cluster (hierarchical A*); half that on small grids
    static const int NAV_SMALLGRID = 6000;       // nodes: a small grid
    static const float NAV_HPAWEIGHT = 1.25f;    // weight of the distance left in the search between portals (more:
                                                 // shorter searches, longer paths: 1.25 = 13 to 34 % quicker, paths 0.2 to 0.6 % longer)
    static const double NAV_SLICEMAX = 2.0;      // ms of work per frame at most
    VARP(navbuildshare, 5, 10, 25);              // % of the time a frame takes without this work (10: the frame rate drops by
                                                 // about a tenth at most while the grid is made; 5: half that, twice as long)
    static inline double navslice() { return navbuildshare/100.0; }
    static const double NAV_SLICEPAUSE = 0.5;    // ... between two games (intermission), when frames matter less
    static const double NAV_WORKMAX = 60000;     // ms of work for a whole map: beyond, no more drops, jumps or swims are simulated
    static const float NAV_STAIR = 4.1f, NAV_SLOPEZ = 0.5f, NAV_FLOORZ = 0.867f, NAV_JUMPMAX = 18.0f, NAV_DROPMAX = 512.0f;
    static const float NAV_LEDGEMAX = 36.0f;     // a jump tried onto a ledge (or over an obstacle) this high at most (a jump: 39 u)
    static const float NAV_SWIMZ = 11.25f;       // the physics' water test point above the feet
    static const float NAV_RUN = 130.0f;         // speed of a running player on the floor

    struct navnode
    {
        vec o;                              // feet
        int col, next;                      // column, next node of the same column
        int link0;                          // first link in navlinks
        ushort nlinks;
        uchar flags;
        uchar dirdone, edges, drops, ledges, blocked, vaults; // per direction: expanded, drop or void, floor below, ledge above,
                                                              // wall, low obstacle nobody stands on (jumped over)
    };
    struct navlink
    {
        int to;
        float cost;
        uchar type, yaw, t0, t1;            // simulated links: keys towards yaw*360/256 degrees from t0 to t1
    };
    struct navedge { int from; navlink l; };

    // a simulation to run: what it starts from and which keys it holds
    enum { NJ_DROP = 0, NJ_LEDGE, NJ_GAP, NJ_PAD, NJ_TELE, NJ_SWIM, NJ_VAULT, NJ_NUM };
    struct navjob
    {
        uchar kind, yaw, t0, t1;            // keys towards yaw from t0 to t1 (as the links)
        uchar pitch;                        // NJ_SWIM: looking up, degrees
        short pad;                          // NJ_PAD / NJ_TELE: index in navpads / navteles
        int from;                           // node the link starts from
    };
    // a simulated link, kept so that an edit across its flight runs it again
    struct navsimrec { int from, to; uchar type; vec lo, hi; navjob job; };
    struct navpadinfo { int ent, vnode, flight; };
    struct navteleinfo { int ent, vnode, dest; };
    struct navslot { int key, val; };

    static void navclearclusters();
    static void navclearscc();
    static void navcacheclose();
    static bool navwanted();

    // ---- the grid
    static vector<navnode> navnodes;
    static vector<navlink> navlinks;        // packed: the links of a node follow each other
    static vector<navedge> navedges;        // links found since the last packing
    static vector<navsimrec> navsimrecs;
    static vector<navpadinfo> navpads;
    static vector<navteleinfo> navteles;
    static vector<navslot> navcolmap;       // column -> first node of the column
    static int navcolcount = 0;

    // ---- the map it is made for
    enum { NS_NONE = 0, NS_LOADING, NS_BUILDING, NS_FINISHING, NS_SAVING, NS_READY, NS_FAILED };
    static int navstate = NS_NONE;
    static string navmap = "";
    static uint navcrc = 0;
    static int navws = 0, navcellsize = 8, navcolsw = 0, navmaxnodes = NAV_MAXNODES;
    static int navedits = 0;                // world and entity edits seen since the map was loaded
    static bool navcapped = false, navlocal = false, navfromcache = false;
    static int navgeneration = 0;                  // a new value each time the grid changes (made, loaded, an edit joined): the bots make their way again
    static uint navrouteseed = 0;           // a bot's own small differences between ways of about the same length (0: none)
    VARP(navvariety, 0, 12, 30);            // how much they can be, % (the links of the search between clusters up to that much dearer for one bot)

    // ---- work in progress
    static vector<int> navqueue, navgen, navpadq, navteleq, navseedq;
    static vector<uchar> navgendirs;        // per navgen entry: the directions just expanded (their simulations to make)
    static int navqhead = 0, navgenhead = 0, navpadhead = 0, navtelehead = 0, navseedhead = 0;
    static vector<navjob> navjobs;
    static int navjobhead = 0;

    // ---- counters of the current grid (what the workshop status shows)
    struct navcounters
    {
        double work, worst, started, loadms, floodms, genms, simms[NJ_NUM], finishms[4];
        int frames, sims[NJ_NUM], simlinks[NJ_NUM], floors, collides, bad, badcached, cachebytes, cell8capped, locals, profilewalks, skipped, sizehits, bridges;
    };
    static navcounters navc;

    static inline double navnow()
    {
        static double scale = 0;
        if(!scale) scale = 1000.0/double(SDL_GetPerformanceFrequency());
        return double(SDL_GetPerformanceCounter())*scale;
    }
    static double navdeadline = 0, navlastframe = 0, navframeavg = 10, navworkavg = 0;
    static inline bool navtimeup() { return navnow() >= navdeadline; }

    template<class T> static void navfree(vector<T> &v) { vector<T> e; swap(v.buf, e.buf); swap(v.alen, e.alen); swap(v.ulen, e.ulen); }

    // ---- small maps (open addressing): int -> int, and a set of 64-bit keys
    static inline uint navhash(int k) { return uint(k)*2654435761u; }
    static void navmapinit(vector<navslot> &m, int &count, int expected)
    {
        int size = 1024;
        while(size < expected*2) size *= 2;
        navfree(m);
        m.reserve(size);
        loopi(size) { navslot &s = m.add(); s.key = -1; s.val = -1; }
        count = 0;
    }
    static int navmapfind(const vector<navslot> &m, int key)
    {
        if(m.empty()) return -1;
        uint mask = uint(m.length() - 1);
        for(uint h = navhash(key)&mask;; h = (h + 1)&mask)
        {
            const navslot &s = m[h];
            if(s.key == key) return int(h);
            if(s.key < 0) return -1;
        }
    }
    static inline int navmapget(const vector<navslot> &m, int key) { int s = navmapfind(m, key); return s >= 0 ? m[s].val : -1; }
    static void navmapset(vector<navslot> &m, int &count, int key, int val)
    {
        if(m.empty() || (count + 1)*2 > m.length())
        {
            vector<navslot> old;
            swap(old.buf, m.buf); swap(old.alen, m.alen); swap(old.ulen, m.ulen);
            navmapinit(m, count, max(old.length(), 512));
            loopv(old) if(old[i].key >= 0) navmapset(m, count, old[i].key, old[i].val);
        }
        uint mask = uint(m.length() - 1);
        for(uint h = navhash(key)&mask;; h = (h + 1)&mask)
        {
            navslot &s = m[h];
            if(s.key == key) { s.val = val; return; }
            if(s.key < 0) { s.key = key; s.val = val; count++; return; }
        }
    }
    struct navset64
    {
        vector<ullong> keys;                // key + 1, 0 = empty, ~0 = erased
        int count;
        navset64() : count(0) {}
        static uint hash(ullong k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33; return uint(k); }
        void init(int expected)
        {
            int size = 1024;
            while(size < expected*2) size *= 2;
            navfree(keys);
            keys.reserve(size);
            loopi(size) keys.add(0);
            count = 0;
        }
        void clear() { navfree(keys); count = 0; }
        bool has(ullong k) const
        {
            if(keys.empty()) return false;
            uint mask = uint(keys.length() - 1);
            for(uint h = hash(k)&mask;; h = (h + 1)&mask)
            {
                if(keys[h] == k + 1) return true;
                if(!keys[h]) return false;
            }
        }
        void add(ullong k)
        {
            if(keys.empty() || (count + 1)*2 > keys.length())
            {
                vector<ullong> old;
                swap(old.buf, keys.buf); swap(old.alen, keys.alen); swap(old.ulen, keys.ulen);
                init(max(old.length(), 512));
                loopv(old) if(old[i] && old[i] != ~0ULL) add(old[i] - 1);
            }
            if(has(k)) return;
            uint mask = uint(keys.length() - 1);
            for(uint h = hash(k)&mask;; h = (h + 1)&mask)
            {
                if(!keys[h] || keys[h] == ~0ULL) { if(!keys[h]) count++; keys[h] = k + 1; return; }
            }
        }
        void erase(ullong k)
        {
            if(keys.empty()) return;
            uint mask = uint(keys.length() - 1);
            for(uint h = hash(k)&mask;; h = (h + 1)&mask)
            {
                if(keys[h] == k + 1) { keys[h] = ~0ULL; return; } // erased slots still count as used (probing goes on)
                if(!keys[h]) return;
            }
        }
    };
    static navset64 navlinkset;             // simulated and entrance links already made (no duplicates)
    static inline ullong navlinkkey(int from, int to, int type) { return (ullong(uint(from)) << 32) | (ullong(uint(to)&0xFFFFFFF) << 4) | uint(type&15); }
    static inline void navlinkmark(int from, int to, int type) { navlinkset.add(navlinkkey(from, to, type)); }
    static inline bool navlinkseen(int from, int to, int type) { return navlinkset.has(navlinkkey(from, to, type)); }

    // ---- columns
    static inline int navcolof(float x, float y)
    {
        int ix = int(floorf(x/navcellsize)), iy = int(floorf(y/navcellsize));
        if(ix < 0 || iy < 0 || ix >= navcolsw || iy >= navcolsw) return -1;
        return iy*navcolsw + ix;
    }
    static inline float navcolx(int col) { return (col%navcolsw + 0.5f)*navcellsize; }
    static inline float navcoly(int col) { return (col/navcolsw + 0.5f)*navcellsize; }
    static inline int navcolhead(int col) { return col < 0 ? -1 : navmapget(navcolmap, col); }
    static inline void navcolsethead(int col, int head) { navmapset(navcolmap, navcolcount, col, head); }

    static const int navdirs[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } }; // k and 7-k opposite
    static inline float navdiryaw(int k) { return -atan2f(float(navdirs[k][0]), float(navdirs[k][1]))/RAD; }
    static inline uchar navyawbyte(float yaw) { return uchar(int(floorf(yaw/360.0f*256.0f + 0.5f))&0xFF); }
    static inline float navbyteyaw(uchar b) { return b*360.0f/256.0f; }

    // ---- the simulated player: never in the player list and "dead", so that it collides with no player,
    // takes nothing, triggers nothing and makes no noise; and a thin vertical probe: what a player collides
    // with in a vertical segment (geometry, glass, clip, map model collision boxes)
    static fpsent *navbody = NULL;
    static physent *navpin = NULL;
    static fpsent *getnavbody()
    {
        if(!navbody) { navbody = new fpsent; navbody->state = CS_DEAD; navbody->ai = NULL; }
        return navbody;
    }
    bool navsimbody(const physent *d) { return d && (d == navbody || d == navpin); } // the grid's simulated players (no sound, nothing heard)
    static physent *getnavpin()
    {
        if(!navpin) { navpin = new physent; navpin->state = CS_DEAD; navpin->radius = navpin->xradius = navpin->yradius = 0.1f; }
        return navpin;
    }

    static bool navsegfree(float x, float y, float zlo, float zhi)
    {
        if(zhi - zlo < 0.05f) { float m = 0.5f*(zlo + zhi); zlo = m - 0.025f; zhi = m + 0.025f; }
        physent *t = getnavpin();
        float mid = 0.5f*(zlo + zhi);
        t->o = vec(x, y, mid);
        t->eyeheight = mid - zlo;
        t->aboveeye = zhi - mid;
        navc.collides++;
        return !collide(t, vec(0, 0, 0), 0, false) && !collideinside;
    }

    static inline float navslopetan(const vec &normal)
    {
        float nz = clamp(normal.z, 0.05f, 1.0f);
        return sqrtf(max(1 - nz*nz, 0.0f))/nz;
    }

    // the highest floor a player stands on at (x, y) between zbot and ztop: 1 found, 0 none, -1 (x, y, ztop) is inside something
    static int navfloor(float x, float y, float ztop, float zbot, float &fz, vec &normal)
    {
        navc.floors++;
        if(ztop <= zbot || !navsegfree(x, y, ztop - 0.1f, ztop)) return -1;
        vec n(0, 0, 1);
        float range = ztop - zbot, dist = rayfloor(vec(x, y, ztop), n, RAY_CLIPMAT, range);
        bool hit = dist >= 0 && dist < range;
        float zr = hit ? ztop - dist : zbot;
        // the thin probe (radius 0.1) touches a slope a little above where the ray met it
        float allow = hit ? 0.03f + 0.1f*navslopetan(n) : 0.02f;
        if(!navsegfree(x, y, zr + allow, ztop))
        { // something the ray does not see (clip, the collision box of a map model): its top
            float lo = zr + allow, hi = ztop - 0.05f;
            loopi(16)
            {
                float m = 0.5f*(lo + hi);
                if(navsegfree(x, y, m, ztop)) hi = m; else lo = m;
                if(hi - lo < 0.02f) break;
            }
            fz = hi - 0.01f;
            normal = hit && fz - zr < 1.5f ? n : vec(0, 0, 1); // still the surface the ray met (a slope)
            return 1;
        }
        if(!hit) return 0;
        // what the ray found must hold a player: noclip geometry does not; then the collision finds what is below it
        // (a spawn pad of noclip cubes over the real floor, for instance)
        if(navsegfree(x, y, zr - 0.3f, zr - 0.02f))
        {
            float lo = zbot, hi = zr - 0.02f;
            if(navsegfree(x, y, lo, hi)) return 0;
            loopi(24)
            {
                float m = 0.5f*(lo + hi);
                if(navsegfree(x, y, m, zr - 0.02f)) hi = m; else lo = m;
                if(hi - lo < 0.02f) break;
            }
            fz = hi - 0.01f;
            normal = vec(0, 0, 1);
            return 1;
        }
        fz = zr;
        normal = n;
        return 1;
    }

    // a player stands with his feet at f (the box raised by what the slope under him needs)
    static bool navstand(const vec &f, float lift)
    {
        fpsent *pr = getnavbody();
        pr->o = vec(f.x, f.y, f.z + pr->eyeheight + lift);
        navc.collides++;
        return !collide(pr, vec(0, 0, 0), 0, false) && !collideinside;
    }
    // ... or at the foot of a step (stairs, the edge of a step): the box touches the next one, the physics steps up onto it
    static inline bool navstandstep(const vec &f, float lift) { return navstand(f, lift) || (lift < NAV_STAIR && navstand(f, NAV_STAIR + 0.3f)); }
    static inline float navlift(const vec &normal) { return min(NAV_STAIR*navslopetan(normal) + 0.25f, 6.0f); }
    static inline bool navpassfree(const vec &a, const vec &b, float z)
    {
        vec m((a.x + b.x)*0.5f, (a.y + b.y)*0.5f, z);
        return navstandstep(m, 0.6f) && navstandstep(vec(b.x, b.y, z), 0.6f);
    }
    static inline bool navdeadly(const vec &f)
    {
        int m1 = lookupmaterial(vec(f.x, f.y, f.z + 1)), m2 = lookupmaterial(vec(f.x, f.y, f.z + NAV_SWIMZ));
        return ((m1|m2)&MAT_DEATH) || (m1&MATF_VOLUME) == MAT_LAVA || (m2&MATF_VOLUME) == MAT_LAVA;
    }
    static inline bool navinwater(const vec &f) { return (lookupmaterial(vec(f.x, f.y, f.z + NAV_SWIMZ))&MATF_VOLUME) == MAT_WATER; }

    // ---- nodes and links
    static int navfindnode(int col, float z, float tol = 8.0f)
    {
        for(int n = navcolhead(col); n >= 0; n = navnodes[n].next) if(fabs(navnodes[n].o.z - z) <= tol) return n;
        return -1;
    }
    static int navaddnode(int col, const vec &o, int flags)
    {
        if(navnodes.length() >= navmaxnodes) { navcapped = true; return -1; }
        int n = navnodes.length();
        navnode &nn = navnodes.add();
        nn.o = o; nn.col = col; nn.next = -1; nn.link0 = 0; nn.nlinks = 0; nn.flags = flags;
        nn.dirdone = nn.edges = nn.drops = nn.ledges = nn.blocked = nn.vaults = 0;
        if(!(flags&NAVF_VIRTUAL) && col >= 0)
        {
            nn.next = navcolhead(col);
            navcolsethead(col, n);
            navqueue.add(n);
        }
        return n;
    }

    // a node on the floor found near (x, y, fz) in column col: that point or a little aside (a player of radius
    // 4.1 must fit), nothing deadly there; -1 when nobody can stand there
    static navset64 navbadset;              // floors of a column where nobody can stand (tried once)
    static int navnodeat(int col, float x, float y, float fz, const vec &normal, int flags)
    {
        int n = navfindnode(col, fz);
        if(n >= 0) return n;
        if(normal.z < NAV_SLOPEZ) { navc.bad++; return -1; }
        bool centre = fabs(x - navcolx(col)) < 0.01f && fabs(y - navcoly(col)) < 0.01f;
        ullong badkey = (ullong(uint(col)) << 20) | ullong(uint(int(floorf(fz + 0.5f)) + 524288)&0xFFFFF);
        if(centre && navbadset.has(badkey)) { navc.badcached++; return -1; }
        static const float offs[25][2] =
        {
            { 0, 0 }, { 0.25f, 0 }, { -0.25f, 0 }, { 0, 0.25f }, { 0, -0.25f }, { 0.25f, 0.25f }, { -0.25f, 0.25f }, { 0.25f, -0.25f }, { -0.25f, -0.25f },
            { 0.44f, 0 }, { -0.44f, 0 }, { 0, 0.44f }, { 0, -0.44f }, { 0.44f, 0.25f }, { 0.44f, -0.25f }, { -0.44f, 0.25f }, { -0.44f, -0.25f },
            { 0.25f, 0.44f }, { -0.25f, 0.44f }, { 0.25f, -0.44f }, { -0.25f, -0.44f }, { 0.44f, 0.44f }, { -0.44f, 0.44f }, { 0.44f, -0.44f }, { -0.44f, -0.44f }
        };
        loopi(25)
        {
            vec f(x + offs[i][0]*navcellsize, y + offs[i][1]*navcellsize, fz), nrm = normal;
            if(i)
            {
                if(navcolof(f.x, f.y) != col) continue;
                float fz2;
                if(navfloor(f.x, f.y, fz + NAV_STAIR + 0.5f, fz - NAV_STAIR - 0.5f, fz2, nrm) <= 0 || fabs(fz2 - fz) > NAV_STAIR || nrm.z < NAV_SLOPEZ) continue;
                f.z = fz2;
            }
            if(navdeadly(f) || !navstandstep(f, navlift(nrm))) continue;
            int m = navfindnode(col, f.z);
            if(m >= 0) return m;
            int fl = flags;
            if(navinwater(f)) fl |= NAVF_WATER;
            if(nrm.z < NAV_FLOORZ) fl |= NAVF_SLOPE;
            return navaddnode(col, f, fl);
        }
        navc.bad++;
        if(centre) navbadset.add(badkey);
        return -1;
    }

    // a node where an entity stands, or where a simulated player lands
    static int navseedat(const vec &p, int flags, float above = 4, float below = 72)
    {
        int col = navcolof(p.x, p.y);
        if(col < 0) return -1;
        float fz;
        vec normal;
        int n = -1;
        if(navfloor(p.x, p.y, p.z + above, p.z - below, fz, normal) > 0) n = navnodeat(col, p.x, p.y, fz, normal, NAVF_SEED|flags);
        if(n < 0)
        {
            float cx = navcolx(col), cy = navcoly(col);
            if(navfloor(cx, cy, p.z + above, p.z - below, fz, normal) > 0) n = navnodeat(col, cx, cy, fz, normal, NAVF_SEED|flags);
        }
        if(n >= 0) navnodes[n].flags |= flags;
        return n;
    }

    // the nearest live node to feet within maxd (horizontally) and maxdz (vertically)
    static int navnearest(const vec &feet, float maxd, float maxdz)
    {
        if(navnodes.empty() || navcolsw <= 0) return -1;
        int col = navcolof(feet.x, feet.y);
        if(col < 0) return -1;
        int cx = col%navcolsw, cy = col/navcolsw, r = int(maxd/navcellsize) + 1, best = -1;
        float bd = 1e16f;
        for(int y = max(cy - r, 0); y <= min(cy + r, navcolsw - 1); y++)
            for(int x = max(cx - r, 0); x <= min(cx + r, navcolsw - 1); x++)
                for(int k = navcolhead(y*navcolsw + x); k >= 0; k = navnodes[k].next)
                {
                    const vec &o = navnodes[k].o;
                    float dx = o.x - feet.x, dy = o.y - feet.y, dz = o.z - feet.z;
                    if(fabs(dz) > maxdz || dx*dx + dy*dy > maxd*maxd) continue;
                    float d = dx*dx + dy*dy + dz*dz;
                    if(d < bd) { bd = d; best = k; }
                }
        return best;
    }

    static void navaddedge(int from, int to, int type, float cost, uchar yaw = 0, uchar t0 = 0, uchar t1 = 0)
    {
        if(from < 0 || to < 0 || from == to) return;
        navedge &e = navedges.add();
        e.from = from; e.l.to = to; e.l.cost = cost; e.l.type = type; e.l.yaw = yaw; e.l.t0 = t0; e.l.t1 = t1;
    }
    static inline float navwalkcost(const navnode &a, const navnode &b)
    {
        float d = a.o.dist(b.o);
        if((a.flags|b.flags)&NAVF_WATER) d *= 3;
        return d;
    }

    // a bumpy floor between a and the floor fz of the next column (stairs, slopes, terrain): followed every 2 u (4 u
    // at 16 u), each piece a stair or a slope a player walks on, both ways
    static bool navprofile(const vec &a, float cx, float cy, float fz)
    {
        float dx = cx - a.x, dy = cy - a.y, len = sqrtf(dx*dx + dy*dy);
        int m = max(int(ceilf(len/(navcellsize >= 16 ? 4.0f : 2.0f))), 1);
        float seg = len/m, z = a.z, room = seg*1.75f + NAV_STAIR + 0.5f;
        for(int i = 1; i <= m; i++)
        {
            float z2;
            vec n2;
            if(navfloor(a.x + dx*i/m, a.y + dy*i/m, z + room, z - room, z2, n2) <= 0) return false;
            float dz = fabs(z2 - z);
            if(dz > NAV_STAIR && (n2.z < NAV_SLOPEZ || dz > seg*navslopetan(n2) + 0.5f)) return false;
            z = z2;
        }
        return fabs(z - fz) <= 1.0f;
    }
    static void navwalkboth(int ni, int b, int k)
    {
        navaddedge(ni, b, NAV_WALK, navwalkcost(navnodes[ni], navnodes[b]));
        navaddedge(b, ni, NAV_WALK, navwalkcost(navnodes[b], navnodes[ni]));
        navnodes[b].dirdone |= 1<<(7-k);
        navc.profilewalks++;
    }

    // a threshold or a thin rail across the way (nobody stands on it, low enough to step over): walked over, to the
    // column after it, when the floor goes on under it and after it at the same level (straight directions only)
    static void navbridge(int ni, int k)
    {
        const vec a = navnodes[ni].o;
        int acol = navnodes[ni].col, x2 = acol%navcolsw + 2*navdirs[k][0], y2 = acol/navcolsw + 2*navdirs[k][1];
        if(x2 < 0 || y2 < 0 || x2 >= navcolsw || y2 >= navcolsw) return;
        float mx = (x2 - navdirs[k][0] + 0.5f)*navcellsize, my = (y2 - navdirs[k][1] + 0.5f)*navcellsize,
              cx = (x2 + 0.5f)*navcellsize, cy = (y2 + 0.5f)*navcellsize, fz, mz;
        vec n, mn;
        if(navfloor(mx, my, a.z + NAV_STAIR + 0.5f, a.z - NAV_STAIR - 0.5f, mz, mn) <= 0) return; // a hole, or a wall
        if(navfloor(cx, cy, a.z + NAV_STAIR + 0.5f, a.z - NAV_STAIR - 0.5f, fz, n) <= 0 || n.z < NAV_SLOPEZ) return;
        int c = navnodeat(y2*navcolsw + x2, cx, cy, fz, n, 0);
        if(c < 0 || c == ni) return;
        const vec co = navnodes[c].o;
        float top = max(a.z, co.z) + NAV_STAIR; // the feet a step higher: what a player steps over stays below
        loopi(3)
        {
            float t = (i + 1)*0.25f;
            if(!navstand(vec(a.x + (co.x - a.x)*t, a.y + (co.y - a.y)*t, top), 0.05f)) return;
        }
        navaddedge(ni, c, NAV_WALK, navwalkcost(navnodes[ni], navnodes[c]));
        navaddedge(c, ni, NAV_WALK, navwalkcost(navnodes[c], navnodes[ni]));
        navc.bridges++;
    }

    // ---- the flood: from a node, the 8 neighbouring columns
    static void navexpand(int ni)
    {
        vec a = navnodes[ni].o;
        int acol = navnodes[ni].col, aix = acol%navcolsw, aiy = acol/navcolsw;
        uchar done = navnodes[ni].dirdone, edges = 0, drops = 0, ledges = 0, blocked = 0, vaults = 0;
        loopk(8)
        {
            if(done&(1<<k)) continue;
            int nx = aix + navdirs[k][0], ny = aiy + navdirs[k][1];
            if(nx < 0 || ny < 0 || nx >= navcolsw || ny >= navcolsw) { blocked |= 1<<k; continue; }
            int col = ny*navcolsw + nx;
            float cx = (nx + 0.5f)*navcellsize, cy = (ny + 0.5f)*navcellsize, dxy = sqrtf((cx - a.x)*(cx - a.x) + (cy - a.y)*(cy - a.y));
            float fz;
            vec normal;
            bool high = false;
            int r = navfloor(cx, cy, a.z + NAV_STAIR + 0.5f, a.z - NAV_DROPMAX, fz, normal);
            // down a step, or down a slope a player walks on (as steep as the slope allows over that distance)
            float down = r > 0 && normal.z >= NAV_SLOPEZ ? max(5.0f, dxy*navslopetan(normal) + 0.5f) : 5.0f;
            if(r > 0 && fz < a.z - down && fz >= a.z - (dxy*1.75f + NAV_STAIR + 1) && normal.z >= NAV_SLOPEZ && navprofile(a, cx, cy, fz))
            { // lower, but walked down (and up again)
                int b = navnodeat(col, cx, cy, fz, normal, 0);
                if(b >= 0 && navnodes[b].col == col && navpassfree(a, navnodes[b].o, a.z)) { navwalkboth(ni, b, k); continue; }
            }
            if(r > 0 && fz >= a.z - down)
            { // same level: walk
                int b = navnodeat(col, cx, cy, fz, normal, 0);
                if(b >= 0 && navpassfree(a, navnodes[b].o, max(a.z, navnodes[b].o.z)))
                {
                    navaddedge(ni, b, NAV_WALK, navwalkcost(navnodes[ni], navnodes[b]));
                    if(a.z - navnodes[b].o.z <= max(NAV_STAIR + 0.5f, down - 0.5f))
                    {
                        navaddedge(b, ni, NAV_WALK, navwalkcost(navnodes[b], navnodes[ni]));
                        if(navnodes[b].col == col) navnodes[b].dirdone |= 1<<(7-k);
                    }
                }
                else high = true;
            }
            else if(r >= 0)
            { // lower floor or nothing below: an edge, if a player can walk off it
                if(navpassfree(a, vec(cx, cy, a.z), a.z))
                {
                    edges |= 1<<k;
                    if(r > 0)
                    { // the floor just below is in the grid (how to get there: the simulated drops); a floor far below
                      // only where a simulated fall really lands (not the bottom of a deadly pit)
                        drops |= 1<<k;
                        if(fz >= a.z - 64) navnodeat(col, cx, cy, fz, normal, 0);
                    }
                }
                else high = true;
            }
            else high = true;
            if(high)
            { // a ledge higher than a step: a steep slope walked up, or a jump onto it (simulated), if there is room above;
              // a low obstacle nobody stands on (a railing, a sill): maybe jumped over (simulated)
                int r2 = navfloor(cx, cy, a.z + (navcellsize < 16 ? NAV_LEDGEMAX : NAV_JUMPMAX) + 0.5f, a.z + NAV_STAIR, fz, normal); // terrain at 16 u: up to 18 u
                if(r2 <= 0 || fz <= a.z + NAV_STAIR || !navstand(vec(a.x, a.y, fz), 0.6f)) { blocked |= 1<<k; continue; }
                int b = navnodeat(col, cx, cy, fz, normal, 0);
                if(b < 0 || !navpassfree(a, navnodes[b].o, fz + 0.5f)) { blocked |= 1<<k; vaults |= 1<<k; continue; }
                if(normal.z < NAV_FLOORZ && fz - a.z <= dxy*navslopetan(normal) + 0.5f) navaddedge(ni, b, NAV_WALK, navwalkcost(navnodes[ni], navnodes[b]));
                else if(navnodes[b].col == col && navprofile(a, cx, cy, fz)) navwalkboth(ni, b, k); // walked up (and down again)
                else ledges |= 1<<k;
            }
        }
        static const int straight[4] = { 1, 3, 4, 6 };
        loopi(4) { int k = straight[i]; if(blocked&(1<<k) && !(vaults&(1<<k))) navbridge(ni, k); }
        navnode &n = navnodes[ni];
        n.dirdone = 0xFF;
        n.edges |= edges; n.drops |= drops; n.ledges |= ledges; n.blocked |= blocked; n.vaults |= vaults;
        if(edges) n.flags |= NAVF_EDGE;
        uchar fresh = uchar(~done);
        if(fresh)
        {
            navgen.add(ni);
            navgendirs.add(fresh);
        }
    }

    // ---- simulations with the game's physics
    struct navtrigger { vec o; float r; int kind, index; };   // kind 0 pad, 1 teleporter
    static vector<navtrigger> navtriggers;
    static vec navtrigmin(0, 0, 0), navtrigmax(0, 0, 0);

    struct navsimstate
    {
        bool active;
        navjob job;
        int t, maxt, inw;
        bool left;
        vec start, lo, hi;
    };
    static navsimstate navsim;

    static inline float navcostof(int type, const vec &a, const vec &b, int ms)
    {
        float d = max(a.dist(b), ms*0.13f);
        switch(type)
        {
            case NAV_DROP: return d + 4 + 0.05f*max(a.z - b.z, 0.0f);
            case NAV_JUMP: case NAV_GAP: return d + 16;
            case NAV_PAD: case NAV_TELE: return d + 8;
            case NAV_SWIM: return d*2 + 16;
        }
        return d;
    }

    // one 5 ms step of the game's own physics; the water state is kept as the physics keeps it, without its
    // splash sounds: false when the simulated player dies (death, lava, out of the world)
    static bool navphysstep(fpsent *pr, int &inw)
    {
        int mat = lookupmaterial(vec(pr->o.x, pr->o.y, pr->o.z + (3*pr->aboveeye - pr->eyeheight)/4)), vol = mat&MATF_VOLUME;
        bool water = isliquid(vol);
        if(water && !inw) pr->vel.div(8);
        pr->inwater = water ? vol : MAT_AIR;
        moveplayer(pr, 10, true, 5);
        if(inw && !water)
        {
            int m2 = lookupmaterial(vec(pr->o.x, pr->o.y, pr->o.z + (pr->aboveeye - pr->eyeheight)/2));
            inw = isliquid(m2&MATF_VOLUME) ? (m2&MATF_VOLUME) : MAT_AIR;
        }
        else inw = water ? vol : MAT_AIR;
        return !(pr->o.z < 0 || mat&MAT_DEATH || vol == MAT_LAVA);
    }

    static void navsimbegin(const navjob &j)
    {
        fpsent *pr = getnavbody();
        pr->reset();
        pr->yaw = navbyteyaw(j.yaw);
        pr->pitch = 0;
        pr->roll = 0;
        pr->lasttaunt = 0;
        navsimstate &s = navsim;
        s.active = true;
        s.job = j;
        s.t = 0;
        s.left = false;
        s.maxt = 3000;
        vec dir;
        vecfromyawpitch(pr->yaw, 0, 1, 0, dir);
        const vec &f = navnodes[j.from].o;
        switch(j.kind)
        {
            case NJ_PAD:
            {
                const extentity &e = *entities::getents()[navpads[j.pad].ent];
                pr->o = vec(f.x, f.y, f.z + pr->eyeheight);
                pr->vel = vec((int)(char)e.attr3*10.0f, (int)(char)e.attr2*10.0f, e.attr1*12.5f);
                pr->falling = vec(0, 0, 0);
                pr->physstate = PHYS_FALL;
                pr->timeinair = 1;
                s.left = true;
                s.maxt = 8000;
                break;
            }
            case NJ_TELE:
            { // the game puts the feet at the destination and the player falls from there
                const extentity &d = *entities::getents()[navteles[j.pad].dest];
                pr->o = vec(d.o).add(vec(0, 0, pr->eyeheight));
                pr->yaw = d.attr1;
                if(d.attr3 > 0) { vecfromyawpitch(pr->yaw, 0, 1, 0, dir); pr->vel = vec(dir).mul(NAV_RUN); }
                loopi(8) { if(!collide(pr, vec(0, 0, 0), 0, false) && !collideinside) break; pr->o.z += 1; }
                pr->physstate = PHYS_FALL;
                pr->timeinair = 1;
                s.left = true;
                break;
            }
            case NJ_SWIM:
                pr->o = vec(f.x, f.y, f.z + pr->eyeheight + 0.01f);
                pr->physstate = PHYS_FLOOR;
                pr->pitch = j.pitch;
                s.maxt = 6000;
                break;
            default: // on the floor, running towards yaw (a bot reaches the edge at full speed)
                pr->o = vec(f.x, f.y, f.z + pr->eyeheight + 0.01f);
                pr->physstate = PHYS_FLOOR;
                pr->vel = vec(dir).mul(NAV_RUN);
                if(j.kind == NJ_LEDGE || j.kind == NJ_GAP || j.kind == NJ_VAULT) pr->jumping = true;
                break;
        }
        s.start = pr->feetpos();
        s.lo = s.hi = s.start;
        int mat = lookupmaterial(vec(pr->o.x, pr->o.y, pr->o.z + (3*pr->aboveeye - pr->eyeheight)/4));
        s.inw = isliquid(mat&MATF_VOLUME) ? mat&MATF_VOLUME : MAT_AIR;
        navc.sims[j.kind]++;
    }

    static int navlandnode(const vec &l)
    {
        int n = navnearest(l, 6, 3);
        if(n >= 0) return n;
        return navseedat(l, 0, 4, 8);
    }
    static void navsimlink(int to, int type)
    {
        navsimstate &s = navsim;
        const navjob &j = s.job;
        if(to < 0 || to == j.from || navlinkseen(j.from, to, type)) return;
        navlinkmark(j.from, to, type);
        navaddedge(j.from, to, type, navcostof(type, navnodes[j.from].o, navnodes[to].o, s.t), j.yaw, j.t0, j.t1);
        navsimrec &r = navsimrecs.add();
        r.from = j.from; r.to = to; r.type = type; r.lo = s.lo; r.hi = s.hi; r.job = j;
        navc.simlinks[j.kind]++;
    }

    static void navpadstrategies(int p);
    // outcome: 0 nothing (time out), 1 landed at land, 2 touched trigger trig on the way, 3 died
    static void navsimend(int outcome, const vec &land, int trig)
    {
        navsimstate &s = navsim;
        const navjob &j = s.job;
        s.active = false;
        static const int types[NJ_NUM] = { NAV_DROP, NAV_JUMP, NAV_GAP, NAV_PAD, NAV_TELE, NAV_SWIM, NAV_GAP };
        int type = types[j.kind];
        if(j.kind == NJ_PAD && j.t0 == 0 && j.t1 == 0 && navpads.inrange(j.pad) && navpads[j.pad].flight < 0)
        { // the flight without keys tells how long a flight lasts: the strategies with keys follow
            navpads[j.pad].flight = s.t;
            navpadstrategies(j.pad);
        }
        if(outcome == 1)
        { // landed: away from where it started (or higher: a ledge)
            float dxy = sqrtf((land.x - s.start.x)*(land.x - s.start.x) + (land.y - s.start.y)*(land.y - s.start.y));
            if(j.kind == NJ_GAP && (land.z < s.start.z - (j.pitch ? 40 : 8) || dxy < (land.z < s.start.z - 8 ? 3.0f : 1.5f)*navcellsize)) return; // lower: aimed at, well across
            if(j.kind == NJ_VAULT && dxy < 1.5f*navcellsize) return; // still on this side (or on the obstacle)
            if(j.kind == NJ_LEDGE && land.z < s.start.z + NAV_STAIR) return;
            if(j.kind == NJ_SWIM && land.z < s.start.z + 2) return;
            if(dxy < 2 && fabs(land.z - s.start.z) < 3) return;
            navsimlink(navlandnode(land), type);
        }
        else if(outcome == 2 && navtriggers.inrange(trig))
        { // touched a jump pad or a teleporter on the way: it takes over
            const navtrigger &tr = navtriggers[trig];
            int v = tr.kind ? navteles[tr.index].vnode : navpads[tr.index].vnode;
            if(v >= 0 && v != j.from) navsimlink(v, type);
        }
    }

    // runs the current simulation until it ends (true) or the slice is used up (false: it goes on next frame)
    static bool navsimrun()
    {
        fpsent *pr = getnavbody();
        navsimstate &s = navsim;
        const navjob &j = s.job;
        int steps = 0;
        while(s.t < s.maxt)
        {
            if(!(++steps&7) && navtimeup()) return false;
            bool keys;
            switch(j.t1)
            {
                case NAVT_LANDING: keys = s.t >= j.t0*10; break;
                case NAVT_TAKEOFF: keys = !s.left; break;
                case NAVT_OVER:
                {
                    float dx = pr->o.x - s.start.x, dy = pr->o.y - s.start.y;
                    keys = dx*dx + dy*dy < float(j.t0)*j.t0;
                    break;
                }
                default: keys = s.t >= j.t0*10 && s.t < j.t1*10; break;
            }
            pr->move = keys ? 1 : 0;
            pr->strafe = 0;
            if(j.kind == NJ_SWIM)
            { // swims up (looking up); at the surface (head out of the water, body still in it) jumps out, keys held
                if(s.inw)
                {
                    pr->pitch = j.pitch;
                    if((lookupmaterial(vec(pr->o.x, pr->o.y, pr->o.z + 2))&MATF_VOLUME) != MAT_WATER) { pr->jumping = true; pr->pitch = 0; s.left = true; }
                }
                else { pr->pitch = 0; s.left = true; }
            }
            else if(j.kind == NJ_DROP && s.left && s.inw) pr->move = 0; // fell into water: sinks to the floor
            if(!navphysstep(pr, s.inw)) { navsimend(3, pr->feetpos(), -1); return true; }
            s.t += 5;
            vec f = pr->feetpos();
            s.lo.min(f); s.hi.max(f);
            // on a floor for good (not while the physics still lifts the feet up onto a ledge)
            bool ground = (pr->physstate == PHYS_FLOOR || pr->physstate == PHYS_SLOPE || pr->physstate == PHYS_STEP_DOWN) && !pr->timeinair;
            if(!s.left && j.kind != NJ_SWIM)
            {
                if(j.kind == NJ_DROP) { if(!ground && f.z < s.start.z - 2) s.left = true; }
                else if(!ground || f.z > s.start.z + 2) s.left = true;
            }
            // jump pads and teleporters on the way (not the pads during the 300 ms after a pad, as the game does)
            if(!navtriggers.empty() && f.x >= navtrigmin.x && f.y >= navtrigmin.y && f.z >= navtrigmin.z && f.x <= navtrigmax.x && f.y <= navtrigmax.y && f.z <= navtrigmax.z)
            {
                loopv(navtriggers)
                {
                    const navtrigger &tr = navtriggers[i];
                    if(!tr.kind && j.kind == NJ_PAD && s.t < 300) continue;
                    if(tr.kind && j.kind == NJ_TELE && s.t < 500) continue;
                    if(s.start.dist(tr.o) < tr.r) continue; // started inside it
                    if(f.dist(tr.o) < tr.r) { navsimend(2, f, i); return true; }
                }
            }
            if(ground && s.left && s.t > 30)
            {
                if(j.kind == NJ_SWIM && s.inw) continue;
                navsimend(1, f, -1);
                return true;
            }
        }
        navsimend(0, pr->feetpos(), -1);
        return true;
    }

    static void navaddjob(int kind, int from, float yaw, int t0, int t1, int pad = -1, uchar pitch = 0)
    {
        if(kind != NJ_PAD && kind != NJ_TELE && !navlocal && navc.work > NAV_WORKMAX) { navc.skipped++; return; }
        navjob &j = navjobs.add();
        j.kind = kind; j.from = from; j.yaw = navyawbyte(yaw); j.t0 = t0; j.t1 = t1; j.pad = pad; j.pitch = pitch;
    }

    // a jump over a void is worth simulating if there is a floor beyond it, about at the same level, within what a
    // running jump covers (about 105 u on the flat), and if the void is deep: from the bottom of a shallow one, a jump
    // reaches both sides (a drop and a jump do it)
    // 0 not worth it, 1 a floor beyond at about its level, 2 a lower one (up to 40 u: a jump down across, not the floor
    // that goes on under the edge); towards yaw (one of the 8 directions, or halfway between two), past column cx, cy
    static int navgapworth(const vec &a, float yaw, float cx, float cy)
    {
        float dx = -sinf(yaw*RAD), dy = cosf(yaw*RAD), farz = 0, fz, bz = -1e9f;
        vec n;
        bool below = navfloor(cx, cy, a.z + NAV_STAIR + 0.5f, a.z - NAV_DROPMAX, fz, n) > 0 && n.z >= NAV_SLOPEZ;
        if(below) bz = fz;
        bool found = false;
        for(float d = 16; d <= 112; d += 12)
        {
            float x = a.x + dx*d, y = a.y + dy*d;
            if(navcolof(x, y) < 0) return 0;
            int r = navfloor(x, y, a.z + NAV_JUMPMAX + 0.5f, a.z - 40, fz, n);
            if(r < 0) return 0;
            if(r > 0 && n.z >= NAV_SLOPEZ)
            {
                if(fz < a.z - 8 && (navdeadly(vec(x, y, fz)) || (below && fabs(fz - bz) <= NAV_STAIR))) continue; // the floor under the edge goes on (or death): further
                found = true; farz = fz; break;
            }
        }
        if(!found) return 0;
        if(below && bz >= max(a.z, farz) - NAV_JUMPMAX + 1 && !navdeadly(vec(cx, cy, bz))) return 0;
        return farz < a.z - 8 ? 2 : 1;
    }

    // a running jump starts at full speed (as simulated): only where there is room for a run-up behind the node, along
    // the jump and at its level (20 u at least; a player, or a bot, can only do it again from there)
    static bool navrunup(int ni, int k)
    {
        const navnode &n = navnodes[ni];
        int x = n.col%navcolsw, y = n.col/navcolsw, back = 7 - k, steps = int(ceilf(20.0f/(navcellsize*(navdirs[back][0] && navdirs[back][1] ? 1.414f : 1.0f))));
        float z = n.o.z;
        loopi(max(steps, 2))
        {
            x += navdirs[back][0]; y += navdirs[back][1];
            if(x < 0 || y < 0 || x >= navcolsw || y >= navcolsw) return false;
            int m = -1;
            for(int c = navcolhead(y*navcolsw + x); c >= 0; c = navnodes[c].next) if(fabs(navnodes[c].o.z - z) <= NAV_STAIR + 0.5f) { m = c; break; }
            if(m < 0) return false;
            z = navnodes[m].o.z;
        }
        return true;
    }

    // a running jump over a low obstacle is worth simulating if a floor (not deadly) lies beyond it
    static bool navvaultworth(const vec &a, int k, float cx, float cy)
    {
        float top, fz;
        vec n;
        if(navfloor(cx, cy, a.z + NAV_LEDGEMAX + 0.5f, a.z + NAV_STAIR, top, n) <= 0) return false;
        loopi(3)
        {
            float x = cx + navdirs[k][0]*navcellsize*(i + 1), y = cy + navdirs[k][1]*navcellsize*(i + 1);
            if(navcolof(x, y) < 0) return false;
            int r = navfloor(x, y, top + NAV_STAIR + 0.5f, a.z - NAV_DROPMAX, fz, n);
            if(r < 0) continue; // still the obstacle: further
            return r > 0 && n.z >= NAV_SLOPEZ && !navdeadly(vec(x, y, fz));
        }
        return false;
    }

    // a diagonal on a straight edge (both neighbouring sides have the same feature) adds nothing: only at corners
    static const int navdiagsides[8][2] = { { 1, 3 }, { -1, -1 }, { 1, 4 }, { -1, -1 }, { -1, -1 }, { 3, 6 }, { -1, -1 }, { 4, 6 } };
    static inline bool navdiagonly(int k, uchar mask) { return navdiagsides[k][0] >= 0 && mask&(1<<navdiagsides[k][0]) && mask&(1<<navdiagsides[k][1]); }
    // along a straight edge, one node in two is enough when its neighbour along the edge (already expanded, same
    // level) has the same feature in the same direction: what = 0 drops, 1 ledges, 2 edges (gaps), 3 low obstacles
    static bool navalongdone(const navnode &n, int k, int what)
    {
        if(navdiagsides[k][0] >= 0) return false; // diagonals: navdiagonly
        int ix = n.col%navcolsw, iy = n.col/navcolsw, along = navdirs[k][0] ? iy : ix;
        if(!(along&1)) return false;
        loopi(2)
        {
            int s = i ? 1 : -1, x = ix + (navdirs[k][0] ? 0 : s), y = iy + (navdirs[k][0] ? s : 0);
            if(x < 0 || y < 0 || x >= navcolsw || y >= navcolsw) continue;
            for(int m = navcolhead(y*navcolsw + x); m >= 0; m = navnodes[m].next)
            {
                const navnode &o = navnodes[m];
                if(o.dirdone != 0xFF || fabs(o.o.z - n.o.z) > 1.0f) continue;
                uchar mask = what == 0 ? o.drops : (what == 1 ? o.ledges : (what == 2 ? o.edges : o.vaults));
                if(mask&(1<<k)) return true;
            }
        }
        return false;
    }

    // the simulations a node needs in the directions just expanded: drops off its edges, jumps onto its ledges, out of
    // the water, over gaps
    static void navgenerate(int ni, uchar dirs)
    {
        const navnode &n = navnodes[ni];
        if(n.flags&(NAVF_DEAD|NAVF_VIRTUAL)) return;
        vec a = n.o;
        uchar edges = n.edges, drops = n.drops, ledges = n.ledges, blocked = n.blocked, vaults = n.vaults;
        bool water = (n.flags&NAVF_WATER) != 0;
        int aix = n.col%navcolsw, aiy = n.col/navcolsw;
        loopk(8)
        {
            if(!(dirs&(1<<k))) continue;
            float yaw = navdiryaw(k), cx = (aix + navdirs[k][0] + 0.5f)*navcellsize, cy = (aiy + navdirs[k][1] + 0.5f)*navcellsize, fz;
            vec nrm;
            if(drops&(1<<k) && !navdiagonly(k, drops) && !navalongdone(n, k, 0))
            {
                navaddjob(NJ_DROP, ni, yaw, 0, NAVT_TAKEOFF); // keys released at the edge
                // keys held: lands further away; worth it for a long fall only, onto another floor than the one below
                // the edge (not on the very large maps, at 16 u)
                if(navcellsize < 16 && navfloor(cx, cy, a.z + NAV_STAIR + 0.5f, a.z - NAV_DROPMAX, fz, nrm) > 0 && fz < a.z - 24)
                {
                    float fx = cx + navdirs[k][0]*navcellsize*3, fy = cy + navdirs[k][1]*navcellsize*3, fz2;
                    vec n2;
                    if(navcolof(fx, fy) < 0 || navfloor(fx, fy, fz + NAV_JUMPMAX, fz - NAV_DROPMAX, fz2, n2) <= 0 || fabs(fz2 - fz) > NAV_STAIR)
                        navaddjob(NJ_DROP, ni, yaw, 0, NAVT_LANDING);
                }
            }
            if(ledges&(1<<k) && !navdiagonly(k, ledges) && !navalongdone(n, k, 1))
            {
                float d = sqrtf((cx - a.x)*(cx - a.x) + (cy - a.y)*(cy - a.y));
                navaddjob(NJ_LEDGE, ni, yaw, int(ceilf(d + 1)), NAVT_OVER);
                // a narrow ledge (a void a column further, not a wall): keys held only a moment after the feet clear
                // its top, to stop on it
                if(navfloor(cx, cy, a.z + NAV_LEDGEMAX + 0.5f, a.z + NAV_STAIR, fz, nrm) > 0)
                {
                    float bx = cx + navdirs[k][0]*navcellsize, by = cy + navdirs[k][1]*navcellsize, fz2;
                    vec n2;
                    if(navcolof(bx, by) >= 0 && navfloor(bx, by, fz + NAV_STAIR + 0.5f, fz - NAV_STAIR - 0.5f, fz2, n2) == 0)
                    {
                        float h = clamp(fz - a.z + 1, 0.0f, JUMPVEL*JUMPVEL/(2*GRAVITY));
                        int tc = int(ceilf((JUMPVEL - sqrtf(max(JUMPVEL*JUMPVEL - 2*GRAVITY*h, 0.0f)))/GRAVITY*100));
                        navaddjob(NJ_LEDGE, ni, yaw, 0, tc + 3);
                        navaddjob(NJ_LEDGE, ni, yaw, 0, tc + 8);
                    }
                }
            }
            // a ledge a little further along that way (a narrow gap, or the foot of its wall, in between): a jump onto it, simulated
            if(!water && navcellsize < 16 && blocked&(1<<k) && !((ledges|vaults)&(1<<k)) && !navdiagonly(k, blocked))
            {
                float ux = navdirs[k][0], uy = navdirs[k][1], ul = sqrtf(ux*ux + uy*uy);
                ux /= ul; uy /= ul;
                for(float d = 14; d <= 26; d += 4)
                {
                    float fx = a.x + ux*d, fy = a.y + uy*d;
                    if(navcolof(fx, fy) < 0) break;
                    if(navfloor(fx, fy, a.z + NAV_LEDGEMAX + 0.5f, a.z + NAV_STAIR, fz, nrm) > 0 && nrm.z >= NAV_SLOPEZ && navstand(vec(fx, fy, fz), 0.6f)) { navaddjob(NJ_LEDGE, ni, yaw, 0, NAVT_LANDING); break; }
                }
            }
            // out of the water: towards a dry floor above, next to the node
            if(water && (ledges|blocked|edges)&(1<<k) && !navalongdone(n, k, 1) && navfloor(cx, cy, a.z + 72, a.z + 2, fz, nrm) > 0 && !navinwater(vec(cx, cy, fz)))
                navaddjob(NJ_SWIM, ni, yaw, 0, NAVT_LANDING, -1, 60);
            if(!water && edges&(1<<k) && !navdiagonly(k, edges) && !navalongdone(n, k, 2) && navrunup(ni, k)) { int gw = navgapworth(a, yaw, cx, cy); if(gw) navaddjob(NJ_GAP, ni, yaw, 0, NAVT_LANDING, -1, gw == 2 ? 1 : 0); }
            if(!water && vaults&(1<<k) && !navdiagonly(k, vaults) && !navalongdone(n, k, 3) && navvaultworth(a, k, cx, cy) && navrunup(ni, k)) navaddjob(NJ_VAULT, ni, yaw, 0, NAVT_LANDING);
            // halfway between two directions too: a jump across is seldom straight along the grid (fusion, fortress)
            if(!water && (edges|vaults)&(1<<k) && !navalongdone(n, k, edges&(1<<k) ? 2 : 3) && navrunup(ni, k)) loopj(2)
            {
                float jy = yaw + (j ? 22.5f : -22.5f);
                int gw = navgapworth(a, jy, cx, cy);
                if(gw) navaddjob(NJ_GAP, ni, jy, 0, NAVT_LANDING, -1, gw == 2 ? 1 : 0);
            }
        }
    }

    // jump pads: the flight without keys first, then keys towards 12 directions held over parts of the flight
    // (air control: early or late, short or long), so that every floor the pad can reach gets a landing
    static void navpadstrategies(int p)
    {
        int vnode = navpads[p].vnode, flight = clamp(navpads[p].flight, 100, 2540), T = flight/10;
        if(vnode < 0) return;
        loopi(12)
        {
            float yaw = i*30.0f;
            navaddjob(NJ_PAD, vnode, yaw, 0, NAVT_LANDING, p);
            navaddjob(NJ_PAD, vnode, yaw, 0, max(T/3, 1), p);
            navaddjob(NJ_PAD, vnode, yaw, 0, max(2*T/3, 1), p);
            navaddjob(NJ_PAD, vnode, yaw, T/3, NAVT_LANDING, p);
            navaddjob(NJ_PAD, vnode, yaw, 2*T/3, NAVT_LANDING, p);
        }
    }
    static void navpadbegin(int p)
    {
        if(!navpads.inrange(p) || navpads[p].vnode < 0) return;
        navaddjob(NJ_PAD, navpads[p].vnode, 0, 0, 0, p);
    }
    static void navtelebegin(int t)
    {
        if(!navteles.inrange(t) || navteles[t].vnode < 0 || navteles[t].dest < 0) return;
        navaddjob(NJ_TELE, navteles[t].vnode, 0, 0, NAVT_LANDING, t);
    }

    // ---- pads and teleporters: their launch nodes, what touches them on the way
    static int navlaunchnode(const extentity &e)
    {
        float fz;
        vec normal, at = e.o;
        if(navfloor(e.o.x, e.o.y, e.o.z + 2, e.o.z - 30, fz, normal) > 0) at.z = fz; else at.z -= 4;
        return navaddnode(navcolof(at.x, at.y), at, NAVF_VIRTUAL);
    }
    static void navsettriggers()
    {
        const vector<extentity *> &ents = entities::getents();
        navfree(navtriggers);
        loopv(navpads) if(navpads[i].vnode >= 0) { navtrigger &tr = navtriggers.add(); tr.o = ents[navpads[i].ent]->o; tr.r = 12; tr.kind = 0; tr.index = i; }
        loopv(navteles) if(navteles[i].vnode >= 0) { navtrigger &tr = navtriggers.add(); tr.o = ents[navteles[i].ent]->o; tr.r = 16; tr.kind = 1; tr.index = i; }
        if(navtriggers.length())
        {
            navtrigmin = navtrigmax = navtriggers[0].o;
            loopv(navtriggers) { navtrigmin.min(navtriggers[i].o); navtrigmax.max(navtriggers[i].o); }
            navtrigmin.sub(16); navtrigmax.add(16);
        }
    }
    static int navteledest(const extentity &e)
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents) if(ents[i]->type == TELEDEST && ents[i]->attr2 == e.attr1) return i;
        return -1;
    }
    static void navsetupents()
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(ents)
        {
            const extentity &e = *ents[i];
            if(e.type != JUMPPAD && e.type != TELEPORT) continue;
            if(navcolof(e.o.x, e.o.y) < 0) continue;
            if(e.type == JUMPPAD)
            {
                navpadinfo &p = navpads.add();
                p.ent = i; p.flight = -1; p.vnode = navlaunchnode(e);
                navpadq.add(navpads.length() - 1);
            }
            else
            {
                navteleinfo &t = navteles.add();
                t.ent = i; t.dest = navteledest(e); t.vnode = navlaunchnode(e);
                navteleq.add(navteles.length() - 1);
            }
        }
        navsettriggers();
    }

    static void navseedent(const extentity &e)
    {
        int flags = 0;
        if(e.type == PLAYERSTART) flags = NAVF_SPAWN;
        else if(e.type >= I_SHELLS && e.type <= I_QUAD) flags = NAVF_ITEM;
        else if(e.type != FLAG && e.type != TELEDEST && e.type != BASE && e.type != JUMPPAD && e.type != TELEPORT) return;
        navseedat(e.o, flags);
    }

    // ---- the build, a slice at a time
    static void navclearwork()
    {
        navfree(navqueue); navfree(navgen); navfree(navgendirs); navfree(navpadq); navfree(navteleq); navfree(navjobs); navfree(navseedq);
        navqhead = navgenhead = navpadhead = navtelehead = navjobhead = navseedhead = 0;
        navsim.active = false;
    }

    static void navfreegrid()
    {
        navclearwork();
        navfree(navnodes); navfree(navlinks); navfree(navedges); navfree(navsimrecs); navfree(navpads); navfree(navteles);
        navfree(navcolmap); navcolcount = 0;
        navlinkset.clear();
        navbadset.clear();
        navfree(navtriggers);
        navclearclusters();
        navclearscc();
    }

    static bool navstart(int cell)
    {
        navfreegrid();
        navlinkset.init(4096);
        navbadset.init(4096);
        navcellsize = cell;
        navws = getworldsize();
        navcolsw = navws/cell;
        if(navcolsw <= 0 || navcolsw > 8192) return false;
        navmapinit(navcolmap, navcolcount, 16384);
        navcapped = false;
        navsetupents();
        const vector<extentity *> &ents = entities::getents();
        loopv(ents) navseedq.add(i); // where players start, items, flags, pads, teleporters: seeded a slice at a time
        return true;
    }

    static bool navbuildstep() // true when everything found so far is done
    {
        for(;;)
        {
            if(navcapped) return true;
            double t0 = navnow();
            if(t0 >= navdeadline) return false;
            if(navseedhead < navseedq.length())
            {
                const vector<extentity *> &ents = entities::getents();
                int e = navseedq[navseedhead++];
                if(ents.inrange(e)) navseedent(*ents[e]);
                continue;
            }
            if(navqhead < navqueue.length())
            {
                int n = navqueue[navqhead++];
                if(!(navnodes[n].flags&NAVF_DEAD)) navexpand(n);
                navc.floodms += navnow() - t0;
                continue;
            }
            if(navsim.active)
            {
                int kind = navsim.job.kind;
                bool done = navsimrun();
                navc.simms[kind] += navnow() - t0;
                if(!done) return false;
                continue;
            }
            if(navjobhead < navjobs.length()) { navsimbegin(navjobs[navjobhead++]); continue; }
            if(navgenhead < navgen.length()) { navgenerate(navgen[navgenhead], navgendirs[navgenhead]); navgenhead++; navc.genms += navnow() - t0; continue; }
            if(navpadhead < navpadq.length()) { navpadbegin(navpadq[navpadhead++]); continue; }
            if(navtelehead < navteleq.length()) { navtelebegin(navteleq[navtelehead++]); continue; }
            return true;
        }
    }

    // ---- after the flood: entrances of pads and teleporters, links packed, components, clusters
    static void navzonelinks(const vec &o, float r, int v, int type)
    {
        int col = navcolof(o.x, o.y);
        if(col < 0 || v < 0) return;
        int cx = col%navcolsw, cy = col/navcolsw, k = int(r/navcellsize) + 1;
        for(int y = max(cy - k, 0); y <= min(cy + k, navcolsw - 1); y++)
            for(int x = max(cx - k, 0); x <= min(cx + k, navcolsw - 1); x++)
                for(int n = navcolhead(y*navcolsw + x); n >= 0; n = navnodes[n].next)
                {
                    if(navnodes[n].flags&NAVF_DEAD || navnodes[n].o.dist(o) >= r || navlinkseen(n, v, type)) continue;
                    navlinkmark(n, v, type);
                    navaddedge(n, v, type, 2);
                }
    }
    // a player within reach of a jump pad or a teleporter takes it: a walk into its zone is entering it (a link to its
    // launch node), and nothing leaves the zone but that link (a player cannot walk through it)
    static void navzonewalks()
    {
        int n = navnodes.length();
        if(navtriggers.empty() || !n) return;
        vector<int> zone;
        zone.reserve(n);
        loopi(n) zone.add(0);
        bool any = false;
        loopv(navtriggers)
        {
            const navtrigger &tr = navtriggers[i];
            int v = tr.kind ? navteles[tr.index].vnode : navpads[tr.index].vnode, col = navcolof(tr.o.x, tr.o.y);
            if(v < 0 || col < 0) continue;
            int cx = col%navcolsw, cy = col/navcolsw, k = int(tr.r/navcellsize) + 1;
            for(int y = max(cy - k, 0); y <= min(cy + k, navcolsw - 1); y++)
                for(int x = max(cx - k, 0); x <= min(cx + k, navcolsw - 1); x++)
                    for(int m = navcolhead(y*navcolsw + x); m >= 0; m = navnodes[m].next)
                        if(!(navnodes[m].flags&NAVF_DEAD) && navnodes[m].o.dist(tr.o) < tr.r) { zone[m] = i + 1; any = true; }
        }
        if(!any) return;
        vector<navedge> enter;
        #define NAVZONEWALK(FROM, LK) do { \
            if((LK).type == NAV_DELETED || (LK).type == NAV_PADIN || (LK).type == NAV_TELEIN || (FROM) >= n) break; \
            if(zone[FROM]) { (LK).type = NAV_DELETED; break; } \
            int zt = (LK).to < n ? zone[(LK).to] : 0; \
            if(!zt || (LK).type != NAV_WALK) break; \
            const navtrigger &tr = navtriggers[zt - 1]; \
            int v = tr.kind ? navteles[tr.index].vnode : navpads[tr.index].vnode, type = tr.kind ? NAV_TELEIN : NAV_PADIN; \
            float c = (LK).cost + 2; \
            (LK).type = NAV_DELETED; \
            if(v >= 0 && !navlinkseen(FROM, v, type)) { navlinkmark(FROM, v, type); navedge &e = enter.add(); e.from = (FROM); e.l.to = v; e.l.cost = c; e.l.type = type; e.l.yaw = e.l.t0 = e.l.t1 = 0; } \
        } while(0)
        loopv(navnodes) { navnode &nn = navnodes[i]; if(nn.flags&NAVF_DEAD) continue; loopj(nn.nlinks) NAVZONEWALK(i, navlinks[nn.link0 + j]); }
        loopv(navedges) NAVZONEWALK(navedges[i].from, navedges[i].l);
        #undef NAVZONEWALK
        loopv(enter) navedges.add(enter[i]);
    }
    static void navzones()
    {
        const vector<extentity *> &ents = entities::getents();
        loopv(navpads) if(navpads[i].vnode >= 0) navzonelinks(ents[navpads[i].ent]->o, 12, navpads[i].vnode, NAV_PADIN);
        loopv(navteles) if(navteles[i].vnode >= 0) navzonelinks(ents[navteles[i].ent]->o, 16, navteles[i].vnode, NAV_TELEIN);
        navzonewalks();
    }

    // the links of each node made contiguous again (the packed links still valid, then the new ones; no
    // duplicates), a slice at a time
    static vector<int> navpkstart, navpkfill;
    static vector<navlink> navpklinks;
    static int navpkphase = 0, navpkcursor = 0;
    static inline bool navlinkvalid(const navlink &l) { return l.type != NAV_DELETED && !(navnodes[l.to].flags&NAVF_DEAD); }
    static inline void navpkput(int i, const navlink &l)
    {
        int s = navpkstart[i], &f = navpkfill[i];
        if(f >= 0xFFFF) return;
        for(int k = s; k < s + f; k++) if(navpklinks[k].to == l.to && navpklinks[k].type == l.type) return;
        navpklinks[s + f++] = l;
    }
    static bool navpackstep() // true when done
    {
        int n = navnodes.length();
        for(;;)
        {
            if(navtimeup()) return false;
            switch(navpkphase)
            {
                case 0:
                    navfree(navpkstart); navfree(navpkfill); navfree(navpklinks);
                    navpkstart.reserve(n); navpkfill.reserve(n);
                    loopi(n) { navpkstart.add(0); navpkfill.add(0); }
                    navpkphase = 1; navpkcursor = 0;
                    break;
                case 1: // how many links each node keeps
                    for(int end = min(navpkcursor + 8192, n); navpkcursor < end; navpkcursor++)
                    {
                        const navnode &nn = navnodes[navpkcursor];
                        if(nn.flags&NAVF_DEAD) continue;
                        int c = 0;
                        loopj(nn.nlinks) if(navlinkvalid(navlinks[nn.link0 + j])) c++;
                        navpkstart[navpkcursor] = c;
                    }
                    if(navpkcursor >= n) { navpkphase = 2; navpkcursor = 0; }
                    break;
                case 2:
                    for(int end = min(navpkcursor + 16384, navedges.length()); navpkcursor < end; navpkcursor++)
                    {
                        const navedge &e = navedges[navpkcursor];
                        if(!(navnodes[e.from].flags&NAVF_DEAD) && navlinkvalid(e.l)) navpkstart[e.from]++;
                    }
                    if(navpkcursor >= navedges.length())
                    {
                        int sum = 0;
                        loopi(n) { int c = min(navpkstart[i], 0xFFFF); navpkstart[i] = sum; sum += c; }
                        navpklinks.reserve(sum);
                        navpklinks.advance(sum);
                        navpkphase = 3; navpkcursor = 0;
                    }
                    break;
                case 3:
                    for(int end = min(navpkcursor + 8192, n); navpkcursor < end; navpkcursor++)
                    {
                        const navnode &nn = navnodes[navpkcursor];
                        if(nn.flags&NAVF_DEAD) continue;
                        loopj(nn.nlinks) { const navlink &l = navlinks[nn.link0 + j]; if(navlinkvalid(l)) navpkput(navpkcursor, l); }
                    }
                    if(navpkcursor >= n) { navpkphase = 4; navpkcursor = 0; }
                    break;
                case 4:
                    for(int end = min(navpkcursor + 16384, navedges.length()); navpkcursor < end; navpkcursor++)
                    {
                        const navedge &e = navedges[navpkcursor];
                        if(!(navnodes[e.from].flags&NAVF_DEAD) && navlinkvalid(e.l)) navpkput(e.from, e.l);
                    }
                    if(navpkcursor >= navedges.length())
                    {
                        loopi(n) { navnodes[i].link0 = navpkstart[i]; navnodes[i].nlinks = navpkfill[i]; }
                        swap(navpklinks.buf, navlinks.buf); swap(navpklinks.alen, navlinks.alen); swap(navpklinks.ulen, navlinks.ulen);
                        navfree(navpklinks); navfree(navpkstart); navfree(navpkfill); navfree(navedges);
                        navpkphase = 0;
                        return true;
                    }
                    break;
            }
        }
    }

    // ---- strongly connected components (Tarjan, iterative, resumable): the biggest is where bots can go
    // anywhere and come back from
    static vector<int> navcomp, navsccidx, navscclow, navsccstk, navsccwork, navsccworkpi, navcompsize;
    static vector<uchar> navscconstk;
    static int navscccounter = 0, navsccroot = 0, navnumcomp = 0, navbigcomp = -1, navbigsize = 0;
    static void navclearscc()
    {
        navfree(navcomp); navfree(navsccidx); navfree(navscclow); navfree(navscconstk); navfree(navsccstk); navfree(navsccwork); navfree(navsccworkpi); navfree(navcompsize);
        navscccounter = navsccroot = navnumcomp = navbigsize = 0;
        navbigcomp = -1;
    }
    static void navsccbegin()
    {
        int n = navnodes.length();
        navfree(navcomp); navfree(navsccidx); navfree(navscclow); navfree(navscconstk); navfree(navcompsize);
        navcomp.reserve(n); navsccidx.reserve(n); navscclow.reserve(n); navscconstk.reserve(n);
        loopi(n) { navcomp.add(-1); navsccidx.add(-1); navscclow.add(0); navscconstk.add(0); }
        navsccstk.setsize(0); navsccwork.setsize(0); navsccworkpi.setsize(0);
        navscccounter = navsccroot = navnumcomp = 0;
        navbigcomp = -1; navbigsize = 0;
    }
    static bool navsccstep() // true when done
    {
        int n = navnodes.length(), steps = 0;
        for(;;)
        {
            if(navsccwork.empty())
            {
                while(navsccroot < n && (navsccidx[navsccroot] >= 0 || navnodes[navsccroot].flags&NAVF_DEAD)) navsccroot++;
                if(navsccroot >= n) break;
                int r = navsccroot;
                navsccwork.add(r); navsccworkpi.add(0);
                navsccidx[r] = navscclow[r] = navscccounter++; navsccstk.add(r); navscconstk[r] = 1;
            }
            if(!(++steps&255) && navtimeup()) return false;
            int v = navsccwork.last();
            const navnode &nv = navnodes[v];
            if(navsccworkpi.last() < nv.nlinks)
            {
                int w = navlinks[nv.link0 + navsccworkpi.last()].to;
                navsccworkpi.last()++;
                if(navsccidx[w] < 0)
                {
                    navsccidx[w] = navscclow[w] = navscccounter++; navsccstk.add(w); navscconstk[w] = 1;
                    navsccwork.add(w); navsccworkpi.add(0);
                }
                else if(navscconstk[w]) navscclow[v] = min(navscclow[v], navsccidx[w]);
            }
            else
            {
                navsccwork.pop(); navsccworkpi.pop();
                if(navsccwork.length()) { int u = navsccwork.last(); navscclow[u] = min(navscclow[u], navscclow[v]); }
                if(navscclow[v] == navsccidx[v])
                {
                    int size = 0;
                    for(;;) { int w = navsccstk.pop(); navscconstk[w] = 0; navcomp[w] = navnumcomp; size++; if(w == v) break; }
                    navcompsize.add(size);
                    if(size > navbigsize) { navbigsize = size; navbigcomp = navnumcomp; }
                    navnumcomp++;
                }
            }
        }
        navfree(navsccidx); navfree(navscclow); navfree(navscconstk); navfree(navsccstk); navfree(navsccwork); navfree(navsccworkpi);
        return true;
    }

    // ---- hierarchical A*: clusters of NAV_CLUSTER x NAV_CLUSTER columns; their entrances ("portals") are
    // joined by the shortest paths inside each cluster (made once) and by the links between clusters
    struct navcluster { vector<int> nodes, portals; };
    struct navabsedge { int to; float cost; };
    struct navportal { int node, cluster, edge0, nedges; };
    struct navabsbuild { int from, to; float cost; };
    static vector<navcluster> navclusters;
    static vector<navslot> navclustermap;
    static int navclustercount = 0;
    static vector<int> navnodecluster, navportalof;
    static vector<int> navrstart, navrfrom;  // the links inside each cluster, by the node they reach (searches towards a node)
    static vector<float> navrcost;
    static vector<navportal> navportals;
    static vector<navabsedge> navabsedges;
    static vector<navabsbuild> navabsbuilt;
    static navset64 navsegs;
    static vector<int> navlscc, navlocalof; // node -> strongly connected part inside its cluster; scratch
    static int navlscccount = 0;
    static int navclphase = 0, navclcursor = 0, navclcursor2 = 0, navclustercols = NAV_CLUSTER;
    static bool navhpaready = false;

    static void navclearclusters()
    {
        loopv(navclusters) { navfree(navclusters[i].nodes); navfree(navclusters[i].portals); }
        navfree(navclusters); navfree(navclustermap); navclustercount = 0;
        navfree(navnodecluster); navfree(navportalof); navfree(navportals); navfree(navabsedges); navfree(navabsbuilt);
        navfree(navrstart); navfree(navrfrom); navfree(navrcost);
        navfree(navlscc); navfree(navlocalof); navlscccount = 0;
        navsegs.clear();
        navclphase = navclcursor = navclcursor2 = 0;
        navhpaready = false;
    }
    static inline int navclusterkey(const vec &o)
    {
        int cw = (navcolsw + navclustercols - 1)/navclustercols;
        int ix = clamp(int(floorf(o.x/navcellsize)), 0, navcolsw - 1)/navclustercols, iy = clamp(int(floorf(o.y/navcellsize)), 0, navcolsw - 1)/navclustercols;
        return iy*cw + ix;
    }

    // strongly connected parts of one cluster (Tarjan on the links that stay inside it): an entrance per pair of
    // parts keeps every way through the cluster (from anywhere in a part, any other node of it can be reached)
    static void navclusterscc(int c)
    {
        const vector<int> &cn = navclusters[c].nodes;
        int m = cn.length();
        static vector<int> idx, low, stk, work, workpi;
        static vector<uchar> onstk;
        idx.setsize(0); low.setsize(0); onstk.setsize(0); stk.setsize(0); work.setsize(0); workpi.setsize(0);
        loopi(m) { navlocalof[cn[i]] = i; idx.add(-1); low.add(0); onstk.add(0); }
        int counter = 0;
        loopi(m)
        {
            if(idx[i] >= 0) continue;
            work.add(i); workpi.add(0);
            idx[i] = low[i] = counter++; stk.add(i); onstk[i] = 1;
            while(work.length())
            {
                int v = work.last();
                const navnode &nv = navnodes[cn[v]];
                if(workpi.last() < nv.nlinks)
                {
                    const navlink &l = navlinks[nv.link0 + workpi.last()];
                    workpi.last()++;
                    if(navnodecluster[l.to] != c) continue;
                    int w = navlocalof[l.to];
                    if(idx[w] < 0)
                    {
                        idx[w] = low[w] = counter++; stk.add(w); onstk[w] = 1;
                        work.add(w); workpi.add(0);
                    }
                    else if(onstk[w]) low[v] = min(low[v], idx[w]);
                }
                else
                {
                    work.pop(); workpi.pop();
                    if(work.length()) { int u = work.last(); low[u] = min(low[u], low[v]); }
                    if(low[v] == idx[v])
                    {
                        for(;;) { int w = stk.pop(); onstk[w] = 0; navlscc[cn[w]] = navlscccount; if(w == v) break; }
                        navlscccount++;
                    }
                }
            }
        }
        loopi(m) navlocalof[cn[i]] = -1;
    }
    static int navportalfor(int node)
    {
        if(navportalof[node] >= 0) return navportalof[node];
        int p = navportals.length();
        navportal &np = navportals.add();
        np.node = node; np.cluster = navnodecluster[node]; np.edge0 = np.nedges = 0;
        navportalof[node] = p;
        navclusters[np.cluster].portals.add(p);
        return p;
    }

    // scratch of the searches (one search at a time)
    struct navheapitem { float f; int n; };
    static void navheappush(vector<navheapitem> &h, float f, int n)
    {
        navheapitem it; it.f = f; it.n = n;
        h.add(it);
        int i = h.length() - 1;
        while(i > 0)
        {
            int p = (i - 1)/2;
            if(h[p].f <= h[i].f) break;
            swap(h[p], h[i]);
            i = p;
        }
    }
    static navheapitem navheappop(vector<navheapitem> &h)
    {
        navheapitem top = h[0], last = h.pop();
        if(h.length())
        {
            h[0] = last;
            int i = 0, n = h.length();
            for(;;)
            {
                int l = 2*i + 1, r = l + 1, m = i;
                if(l < n && h[l].f < h[m].f) m = l;
                if(r < n && h[r].f < h[m].f) m = r;
                if(m == i) break;
                swap(h[m], h[i]);
                i = m;
            }
        }
        return top;
    }
    struct navsearch
    {
        vector<float> g;
        vector<int> parent;
        vector<uint> seen;
        uint epoch;
        vector<navheapitem> heap;
        navsearch() : epoch(0) {}
        void prepare(int n)
        {
            if(g.length() != n)
            {
                navfree(g); navfree(parent); navfree(seen);
                g.reserve(n); parent.reserve(n); seen.reserve(n);
                loopi(n) { g.add(0); parent.add(-1); seen.add(0); }
                epoch = 0;
            }
            if(!++epoch) { loopi(n) seen[i] = 0; epoch = 1; }
            heap.setsize(0);
        }
        void clear() { navfree(g); navfree(parent); navfree(seen); navfree(heap); epoch = 0; }
        bool reached(int i) const { return seen[i] == epoch; }
    };
    static navsearch navfwd, navbwd, navabs;
    // links a search must not take (the links that just failed a bot: ainavbot.h sets them around its search, none
    // otherwise); a cluster with one inside is not crossed through its portals (the ways inside it went through it)
    static int navavoidn = 0, navavoidfrom[4], navavoidto[4], navavoidcl[4], navavoidcl2[4][4];
    static bool navavoidfam[4];             // ... and the links like it (navlinklike): set by ainavbot.h (botnavsafe)
    // a link from u to v like the one from a to b: a flight the same way, from beside a (16 u) to beside b (32 u): the
    // parallel jumps or drops across, along an edge
    static bool navlinklike(int a, int b, int u, int v)
    {
        if(!navnodes.inrange(a) || !navnodes.inrange(b) || !navnodes.inrange(u) || !navnodes.inrange(v)) return false;
        const vec &pa = navnodes[a].o, &pb = navnodes[b].o, &pu = navnodes[u].o, &pv = navnodes[v].o;
        float ax = pu.x - pa.x, ay = pu.y - pa.y, bx = pv.x - pb.x, by = pv.y - pb.y;
        if(ax*ax + ay*ay > 16*16 || fabs(pu.z - pa.z) > 6 || bx*bx + by*by > 32*32 || fabs(pv.z - pb.z) > 12) return false;
        float dx = pb.x - pa.x, dy = pb.y - pa.y, ex = pv.x - pu.x, ey = pv.y - pu.y, dl = sqrtf(dx*dx + dy*dy), el = sqrtf(ex*ex + ey*ey);
        return dl >= 20 && el >= 20 && dx*ex + dy*ey > 0.8f*dl*el;
    }
    static inline bool navavoided(int u, int v) { loopi(navavoidn) if((navavoidfrom[i] == u && navavoidto[i] == v) || (navavoidfam[i] && navlinklike(navavoidfrom[i], navavoidto[i], u, v))) return true; return false; }
    static inline bool navavoidedcluster(int c) { loopi(navavoidn) { if(navavoidcl[i] == c) return true; if(navavoidfam[i]) loopj(4) if(navavoidcl2[i][j] == c) return true; } return false; }

    // shortest paths from node s inside cluster c (Dijkstra), until `portals` portals of the cluster and the node
    // `until` (if any) are reached for good
    static void navclusterdijkstra(navsearch &sr, int s, int c, int portals = 0x7FFFFFFF, int until = -1)
    {
        sr.prepare(navnodes.length());
        sr.g[s] = 0; sr.parent[s] = -1; sr.seen[s] = sr.epoch;
        navheappush(sr.heap, 0, s);
        int settled = 0;
        bool found = until < 0;
        while(sr.heap.length())
        {
            navheapitem it = navheappop(sr.heap);
            int v = it.n;
            if(it.f > sr.g[v]) continue;
            if(navportalof[v] >= 0) settled++;
            if(v == until) found = true;
            if(found && settled >= portals) break;
            const navnode &nv = navnodes[v];
            loopi(nv.nlinks)
            {
                const navlink &l = navlinks[nv.link0 + i];
                if(navnodecluster[l.to] != c || (navavoidn && navavoided(v, l.to))) continue;
                float ng = sr.g[v] + l.cost;
                if(sr.seen[l.to] == sr.epoch && ng >= sr.g[l.to]) continue;
                sr.seen[l.to] = sr.epoch; sr.g[l.to] = ng; sr.parent[l.to] = v;
                navheappush(sr.heap, ng, l.to);
            }
        }
    }

    static bool navclustersstep() // true when done
    {
        int n = navnodes.length();
        for(;;)
        {
            if(navtimeup()) return false;
            switch(navclphase)
            {
                case 0: // clusters and their nodes
                {
                    navclearclusters();
                    int live = 0;
                    loopi(n) if(!(navnodes[i].flags&NAVF_DEAD)) live++;
                    navclustercols = live < NAV_SMALLGRID ? NAV_CLUSTER/2 : NAV_CLUSTER;
                    navnodecluster.reserve(n); navportalof.reserve(n); navlscc.reserve(n); navlocalof.reserve(n);
                    loopi(n) { navnodecluster.add(-1); navportalof.add(-1); navlscc.add(-1); navlocalof.add(-1); }
                    navmapinit(navclustermap, navclustercount, 1024);
                    navclphase = 1; navclcursor = 0;
                    break;
                }
                case 1:
                {
                    for(int end = min(navclcursor + 4096, n); navclcursor < end; navclcursor++)
                    {
                        const navnode &nn = navnodes[navclcursor];
                        if(nn.flags&NAVF_DEAD) continue;
                        int key = navclusterkey(nn.o), c = navmapget(navclustermap, key);
                        if(c < 0) { c = navclusters.length(); navclusters.add(); navmapset(navclustermap, navclustercount, key, c); }
                        navclusters[c].nodes.add(navclcursor);
                        navnodecluster[navclcursor] = c;
                    }
                    if(navclcursor >= n) { navclphase = 2; navclcursor = 0; }
                    break;
                }
                case 2: // strongly connected parts inside each cluster
                {
                    if(navclcursor >= navclusters.length()) { navclphase = 3; navclcursor = 0; navsegs.init(4096); break; }
                    navclusterscc(navclcursor++);
                    break;
                }
                case 3: // entrances: of the links from one part of a cluster to one part of another, one per kind and
                        // per half cluster along the border (more would only shorten some paths a little)
                {
                    for(int end = min(navclcursor + 2048, n); navclcursor < end; navclcursor++)
                    {
                        const navnode &nu = navnodes[navclcursor];
                        if(nu.flags&NAVF_DEAD) continue;
                        int cu = navnodecluster[navclcursor];
                        loopj(nu.nlinks)
                        {
                            const navlink &l = navlinks[nu.link0 + j];
                            int cv = navnodecluster[l.to];
                            if(cv == cu || cv < 0) continue;
                            int kind = l.type == NAV_WALK ? 0 : (l.type <= NAV_GAP || l.type == NAV_SWIM ? 1 : 2);
                            if(kind < 2)
                            {
                                const vec &vo = navnodes[l.to].o;
                                int ux = int(floorf(nu.o.x/navcellsize)), uy = int(floorf(nu.o.y/navcellsize)), vx = int(floorf(vo.x/navcellsize)), vy = int(floorf(vo.y/navcellsize));
                                int along = ux/navclustercols != vx/navclustercols ? uy : ux; // the coordinate along the border
                                if(ux/navclustercols != vx/navclustercols && uy/navclustercols != vy/navclustercols) along = 0; // a corner
                                along /= max(navclustercols/2, 1);
                                ullong key = (ullong(uint(navlscc[navclcursor])&0xFFFFFF) << 40) | (ullong(uint(navlscc[l.to])&0xFFFFFF) << 16) | (ullong(kind) << 14) | ullong(uint(along)&0x3FFF);
                                if(navsegs.has(key)) continue;
                                navsegs.add(key);
                            }
                            navabsbuild &e = navabsbuilt.add();
                            e.from = navportalfor(navclcursor); e.to = navportalfor(l.to); e.cost = l.cost;
                        }
                    }
                    if(navclcursor >= n) { navclphase = 4; navclcursor = navclcursor2 = 0; navsegs.clear(); }
                    break;
                }
                case 4: // inside each cluster: shortest paths between its portals
                {
                    if(navclcursor >= navclusters.length()) { navclphase = 5; navclcursor = 0; break; }
                    navcluster &c = navclusters[navclcursor];
                    if(navclcursor2 >= c.portals.length()) { navclcursor++; navclcursor2 = 0; break; }
                    int p = c.portals[navclcursor2++];
                    navclusterdijkstra(navfwd, navportals[p].node, navclcursor, c.portals.length());
                    loopv(c.portals)
                    {
                        int q = c.portals[i], qn = navportals[q].node;
                        if(q == p || !navfwd.reached(qn)) continue;
                        navabsbuild &e = navabsbuilt.add();
                        e.from = p; e.to = q; e.cost = navfwd.g[qn];
                    }
                    break;
                }
                case 5: // the links inside each cluster, by the node they reach: counted
                {
                    if(!navclcursor) { navfree(navrstart); navrstart.reserve(n + 1); loopi(n + 1) navrstart.add(0); }
                    for(int end = min(navclcursor + 4096, n); navclcursor < end; navclcursor++)
                    {
                        const navnode &nu = navnodes[navclcursor];
                        if(nu.flags&NAVF_DEAD) continue;
                        int cu = navnodecluster[navclcursor];
                        loopj(nu.nlinks) { int v = navlinks[nu.link0 + j].to; if(navnodecluster[v] == cu) navrstart[v + 1]++; }
                    }
                    if(navclcursor >= n)
                    {
                        loopi(n) navrstart[i + 1] += navrstart[i];
                        navfree(navrfrom); navfree(navrcost);
                        navrfrom.reserve(navrstart[n]); navrfrom.advance(navrstart[n]);
                        navrcost.reserve(navrstart[n]); navrcost.advance(navrstart[n]);
                        loopi(n) navlocalof[i] = navrstart[i]; // the next free place of each node
                        navclphase = 6; navclcursor = 0;
                    }
                    break;
                }
                case 6: // and filled
                {
                    for(int end = min(navclcursor + 4096, n); navclcursor < end; navclcursor++)
                    {
                        const navnode &nu = navnodes[navclcursor];
                        if(nu.flags&NAVF_DEAD) continue;
                        int cu = navnodecluster[navclcursor];
                        loopj(nu.nlinks)
                        {
                            const navlink &l = navlinks[nu.link0 + j];
                            if(navnodecluster[l.to] != cu) continue;
                            int at = navlocalof[l.to]++;
                            navrfrom[at] = navclcursor; navrcost[at] = l.cost;
                        }
                    }
                    if(navclcursor >= n) { loopi(n) navlocalof[i] = -1; navclphase = 7; navclcursor = 0; }
                    break;
                }
                case 7: // packed abstract graph
                {
                    int np = navportals.length();
                    vector<int> cnt;
                    cnt.reserve(np);
                    loopi(np) cnt.add(0);
                    loopv(navabsbuilt) cnt[navabsbuilt[i].from]++;
                    int sum = 0;
                    loopi(np) { navportals[i].edge0 = sum; sum += cnt[i]; navportals[i].nedges = 0; }
                    navfree(navabsedges);
                    navabsedges.reserve(sum);
                    navabsedges.advance(sum);
                    loopv(navabsbuilt)
                    {
                        const navabsbuild &b = navabsbuilt[i];
                        navportal &p = navportals[b.from];
                        navabsedge &e = navabsedges[p.edge0 + p.nedges++];
                        e.to = b.to; e.cost = b.cost;
                    }
                    navfree(navabsbuilt);
                    navhpaready = true;
                    navgeneration++;
                    return true;
                }
            }
        }
    }

    static inline float navvary(int k)
    {
        uint h = (uint(k) + 1u)*0x9E3779B1u ^ navrouteseed;
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
        return 1 + navvariety*0.01f*float(h&0xFFFF)/65535.0f;
    }

    // ---- paths: hierarchical A* from node s to node g; true and the cost (and the nodes, from g back to s, if
    // path is given)
    bool navroutenodes(int s, int g, vector<int> *path, float &cost)
    {
        if(!navhpaready || !navnodes.inrange(s) || !navnodes.inrange(g) || navnodes[s].flags&NAVF_DEAD || navnodes[g].flags&NAVF_DEAD) return false;
        if(s == g) { cost = 0; if(path) { path->setsize(0); path->add(g); } return true; }
        int cs = navnodecluster[s], cg = navnodecluster[g], np = navportals.length();
        if(cs < 0 || cg < 0) return false;
        // to the goal: inside its cluster, from its portals (Dijkstra on the links reversed)
        navbwd.prepare(navnodes.length());
        {
            navbwd.g[g] = 0; navbwd.parent[g] = -1; navbwd.seen[g] = navbwd.epoch;
            navheappush(navbwd.heap, 0, g);
            int need = navclusters[cg].portals.length(), settled = 0;
            while(navbwd.heap.length())
            {
                navheapitem it = navheappop(navbwd.heap);
                int v = it.n;
                if(it.f > navbwd.g[v]) continue;
                if(navportalof[v] >= 0 && ++settled >= need) break;
                for(int e = navrstart[v], end = navrstart[v + 1]; e < end; e++)
                {
                    int u = navrfrom[e];
                    if(navavoidn && navavoided(u, v)) continue;
                    float nd = navbwd.g[v] + navrcost[e];
                    if(navbwd.reached(u) && nd >= navbwd.g[u]) continue;
                    navbwd.seen[u] = navbwd.epoch; navbwd.g[u] = nd; navbwd.parent[u] = v;
                    navheappush(navbwd.heap, nd, u);
                }
            }
        }
        // from the start: inside its cluster, to its portals (and to the goal when it is there)
        navclusterdijkstra(navfwd, s, cs, navclusters[cs].portals.length(), cs == cg ? g : -1);
        float best = 1e30f;
        int bestportal = -2; // -1: the path inside the cluster
        if(cs == cg && navfwd.reached(g)) { best = navfwd.g[g]; bestportal = -1; }
        // the abstract graph
        navabs.prepare(np);
        const vec &go = navnodes[g].o;
        loopv(navclusters[cs].portals)
        {
            int p = navclusters[cs].portals[i], pn = navportals[p].node;
            if(!navfwd.reached(pn)) continue;
            navabs.seen[p] = navabs.epoch; navabs.g[p] = navfwd.g[pn]; navabs.parent[p] = -1;
            navheappush(navabs.heap, navfwd.g[pn] + navnodes[pn].o.dist(go)*NAV_HPAWEIGHT, p);
        }
        while(navabs.heap.length())
        {
            navheapitem it = navheappop(navabs.heap);
            if(it.f >= best) break;
            int p = it.n;
            const navportal &pp = navportals[p];
            if(it.f > navabs.g[p] + navnodes[pp.node].o.dist(go)*NAV_HPAWEIGHT + 1e-3f) continue;
            if(pp.cluster == cg && navbwd.reached(pp.node) && navabs.g[p] + navbwd.g[pp.node] < best) { best = navabs.g[p] + navbwd.g[pp.node]; bestportal = p; }
            loopi(pp.nedges)
            {
                const navabsedge &e = navabsedges[pp.edge0 + i];
                if(navavoidn && (navportals[e.to].cluster == pp.cluster ? navavoidedcluster(pp.cluster) : navavoided(pp.node, navportals[e.to].node))) continue;
                float ng = navabs.g[p] + (navrouteseed ? e.cost*navvary(pp.edge0 + i) : e.cost);
                if(navabs.reached(e.to) && ng >= navabs.g[e.to]) continue;
                navabs.seen[e.to] = navabs.epoch; navabs.g[e.to] = ng; navabs.parent[e.to] = p;
                navheappush(navabs.heap, ng + navnodes[navportals[e.to].node].o.dist(go)*NAV_HPAWEIGHT, e.to);
            }
        }
        if(bestportal < -1) return false;
        cost = best;
        if(!path) return true;
        // the nodes: the portals of the abstract path, refined cluster by cluster (from g back to s)
        path->setsize(0);
        if(bestportal == -1) { for(int k = g; k >= 0; k = navfwd.parent[k]) path->add(k); return true; }
        {
            vector<int> tail;
            for(int k = navportals[bestportal].node; k >= 0; k = navbwd.parent[k]) tail.add(k);
            loopvrev(tail) path->add(tail[i]);
        }
        vector<int> abs;
        for(int p = bestportal; p >= 0; p = navabs.parent[p]) abs.add(p);
        for(int k = 0; k + 1 < abs.length(); k++)
        {
            int q = navportals[abs[k]].node, p = navportals[abs[k+1]].node;
            if(navnodecluster[p] == navnodecluster[q])
            {
                navclusterdijkstra(navfwd, p, navnodecluster[p], 0, q);
                if(navfwd.reached(q)) for(int m = navfwd.parent[q]; m >= 0 && m != p; m = navfwd.parent[m]) path->add(m);
            }
            path->add(p);
        }
        navclusterdijkstra(navfwd, s, cs, 0, navportals[abs.last()].node);
        for(int m = navfwd.parent[navportals[abs.last()].node]; m >= 0; m = navfwd.parent[m]) path->add(m);
        return true;
    }

    // ---- the cache on disk: one file per map version in <profile>/navcache
    static stream *navcachefile = NULL;
    static vector<uchar> navcachebuf;
    static int navcachepos = 0;
    static string navcachename = "";
    static void navcacheclose()
    {
        DELETEP(navcachefile);
        navfree(navcachebuf);
        navcachepos = 0;
    }
    static void navcachefilename(char *dst, int len)
    {
        string m = "";
        int k = 0;
        for(const char *s = navmap; *s && k < 40; s++) m[k++] = isalnum(uchar(*s)) || *s == '-' || *s == '_' ? *s : '_';
        m[k] = 0;
        nformatstring(dst, len, "navcache%c%s-%08x.l4nav", PATHDIV, m, navcrc);
    }
    template<class T> static void navputv(vector<uchar> &b, const T &v) { T x = v; lilswap(&x, 1); b.put((const uchar *)&x, sizeof(T)); }
    template<class T> static bool navgetv(const vector<uchar> &b, int &pos, T &v)
    {
        if(pos + int(sizeof(T)) > b.length()) return false;
        memcpy(&v, &b[pos], sizeof(T)); lilswap(&v, 1); pos += sizeof(T);
        return true;
    }
    static const char NAV_MAGIC[9] = "L4ZYNAV1";

    // the file, written a piece at a time: header, nodes and their links, pads, teleporters, simulated links, end
    static int navserphase = 0, navsercursor = 0;
    static bool navcacheserializestep(vector<uchar> &b) // true when all is in b (b is written out as it grows)
    {
        switch(navserphase)
        {
            case 0:
            {
                b.put((const uchar *)NAV_MAGIC, 8);
                navputv(b, int(NAV_VERSION)); navputv(b, navcrc); navputv(b, navws); navputv(b, navcellsize);
                int nm = int(strlen(navmap)); navputv(b, nm); b.put((const uchar *)navmap, nm);
                int nl = 0;
                loopv(navnodes) nl += navnodes[i].nlinks; // the links written below (packing leaves unused room where it drops a double)
                navputv(b, navnodes.length()); navputv(b, nl);
                navserphase = 1; navsercursor = 0;
                return false;
            }
            case 1:
                for(int end = min(navsercursor + 1024, navnodes.length()); navsercursor < end; navsercursor++)
                {
                    const navnode &n = navnodes[navsercursor];
                    navputv(b, n.o.x); navputv(b, n.o.y); navputv(b, n.o.z); b.add(n.flags);
                    b.add(n.edges); b.add(n.drops); b.add(n.ledges); b.add(n.blocked); b.add(n.vaults);
                    navputv(b, n.nlinks);
                    loopj(n.nlinks)
                    {
                        const navlink &l = navlinks[n.link0 + j];
                        navputv(b, l.to); navputv(b, l.cost); b.add(l.type); b.add(l.yaw); b.add(l.t0); b.add(l.t1);
                    }
                }
                if(navsercursor >= navnodes.length())
                {
                    navputv(b, navpads.length());
                    loopv(navpads) { navputv(b, navpads[i].ent); navputv(b, navpads[i].vnode); navputv(b, navpads[i].flight); }
                    navputv(b, navteles.length());
                    loopv(navteles) { navputv(b, navteles[i].ent); navputv(b, navteles[i].vnode); navputv(b, navteles[i].dest); }
                    navputv(b, navsimrecs.length());
                    navserphase = 2; navsercursor = 0;
                }
                return false;
            case 2:
                for(int end = min(navsercursor + 4096, navsimrecs.length()); navsercursor < end; navsercursor++)
                {
                    const navsimrec &r = navsimrecs[navsercursor];
                    navputv(b, r.from); navputv(b, r.to); b.add(r.type);
                    navputv(b, r.lo.x); navputv(b, r.lo.y); navputv(b, r.lo.z); navputv(b, r.hi.x); navputv(b, r.hi.y); navputv(b, r.hi.z);
                    b.add(r.job.kind); b.add(r.job.yaw); b.add(r.job.t0); b.add(r.job.t1); b.add(r.job.pitch);
                    navputv(b, r.job.pad); navputv(b, r.job.from);
                }
                if(navsercursor < navsimrecs.length()) return false;
                b.put((const uchar *)"END!", 4);
                navserphase = 3;
                return true;
        }
        return true;
    }

    // reading, a piece at a time: 1 done, 0 to be continued, -1 not a grid of this map version (made again)
    static int navparsephase = 0, navparsepos = 0, navparsecursor = 0, navparsenn = 0, navparsenl = 0, navparsecount = 0;
    static int navcacheparsestep(const vector<uchar> &b)
    {
        int &pos = navparsepos;
        switch(navparsephase)
        {
            case 0:
            {
                if(b.length() < 8 || memcmp(b.getbuf(), NAV_MAGIC, 8)) return -1;
                pos = 8;
                int ver, ws, cell, nm;
                uint crc;
                if(!navgetv(b, pos, ver) || ver != NAV_VERSION || !navgetv(b, pos, crc) || crc != navcrc || !navgetv(b, pos, ws) || ws != navws ||
                   !navgetv(b, pos, cell) || (cell != 8 && cell != 16) || !navgetv(b, pos, nm) || nm < 0 || nm >= MAXSTRLEN || pos + nm > b.length()) return -1;
                if(int(strlen(navmap)) != nm || memcmp(&b[pos], navmap, nm)) return -1;
                pos += nm;
                if(!navgetv(b, pos, navparsenn) || !navgetv(b, pos, navparsenl) || navparsenn < 0 || navparsenn > 4*NAV_MAXNODES || navparsenl < 0) return -1;
                navcellsize = cell;
                navcolsw = navws/cell;
                navmapinit(navcolmap, navcolcount, navparsenn);
                navfree(navnodes); navfree(navlinks);
                navnodes.reserve(navparsenn); navlinks.reserve(navparsenl);
                navparsephase = 1; navparsecursor = 0;
                return 0;
            }
            case 1:
            {
                int nn = navparsenn;
                for(int end = min(navparsecursor + 2048, nn); navparsecursor < end; navparsecursor++)
                {
                    int i = navparsecursor;
                    navnode &n = navnodes.add();
                    if(!navgetv(b, pos, n.o.x) || !navgetv(b, pos, n.o.y) || !navgetv(b, pos, n.o.z) || pos + 6 > b.length()) return -1;
                    n.flags = b[pos++]; n.edges = b[pos++]; n.drops = b[pos++]; n.ledges = b[pos++]; n.blocked = b[pos++]; n.vaults = b[pos++];
                    n.dirdone = 0xFF;
                    if(!navgetv(b, pos, n.nlinks)) return -1;
                    n.link0 = navlinks.length();
                    loopj(n.nlinks)
                    {
                        navlink &l = navlinks.add();
                        if(!navgetv(b, pos, l.to) || !navgetv(b, pos, l.cost) || pos + 4 > b.length() || l.to < 0 || l.to >= nn) return -1;
                        l.type = b[pos++]; l.yaw = b[pos++]; l.t0 = b[pos++]; l.t1 = b[pos++];
                    }
                    n.col = navcolof(n.o.x, n.o.y);
                    n.next = -1;
                    if(!(n.flags&NAVF_VIRTUAL) && n.col >= 0) { n.next = navcolhead(n.col); navcolsethead(n.col, i); }
                }
                if(navparsecursor < nn) return 0;
                if(navlinks.length() != navparsenl) return -1;
                int np, nt;
                if(!navgetv(b, pos, np) || np < 0 || np > 10000) return -1;
                loopi(np)
                {
                    navpadinfo &p = navpads.add();
                    if(!navgetv(b, pos, p.ent) || !navgetv(b, pos, p.vnode) || !navgetv(b, pos, p.flight)) return -1;
                }
                if(!navgetv(b, pos, nt) || nt < 0 || nt > 10000) return -1;
                loopi(nt)
                {
                    navteleinfo &t = navteles.add();
                    if(!navgetv(b, pos, t.ent) || !navgetv(b, pos, t.vnode) || !navgetv(b, pos, t.dest)) return -1;
                }
                if(!navgetv(b, pos, navparsecount) || navparsecount < 0) return -1;
                navparsephase = 2; navparsecursor = 0;
                return 0;
            }
            case 2:
            {
                for(int end = min(navparsecursor + 4096, navparsecount); navparsecursor < end; navparsecursor++)
                {
                    navsimrec &r = navsimrecs.add();
                    if(!navgetv(b, pos, r.from) || !navgetv(b, pos, r.to) || pos + 1 > b.length()) return -1;
                    r.type = b[pos++];
                    if(!navgetv(b, pos, r.lo.x) || !navgetv(b, pos, r.lo.y) || !navgetv(b, pos, r.lo.z) || !navgetv(b, pos, r.hi.x) || !navgetv(b, pos, r.hi.y) || !navgetv(b, pos, r.hi.z) || pos + 5 > b.length()) return -1;
                    r.job.kind = b[pos++]; r.job.yaw = b[pos++]; r.job.t0 = b[pos++]; r.job.t1 = b[pos++]; r.job.pitch = b[pos++];
                    if(!navgetv(b, pos, r.job.pad) || !navgetv(b, pos, r.job.from)) return -1;
                    if(!navnodes.inrange(r.from) || !navnodes.inrange(r.to) || !navnodes.inrange(r.job.from) || r.job.kind >= NJ_NUM) return -1;
                }
                if(navparsecursor < navparsecount) return 0;
                if(pos + 4 != b.length() || memcmp(&b[pos], "END!", 4)) return -1;
                // the entities must still be those the grid was made with
                const vector<extentity *> &ents = entities::getents();
                loopv(navpads) if(!ents.inrange(navpads[i].ent) || ents[navpads[i].ent]->type != JUMPPAD || !navnodes.inrange(navpads[i].vnode)) return -1;
                loopv(navteles) if(!ents.inrange(navteles[i].ent) || ents[navteles[i].ent]->type != TELEPORT || !navnodes.inrange(navteles[i].vnode) || (navteles[i].dest >= 0 && !ents.inrange(navteles[i].dest))) return -1;
                loopv(navsimrecs) if((navsimrecs[i].job.kind == NJ_PAD && !navpads.inrange(navsimrecs[i].job.pad)) || (navsimrecs[i].job.kind == NJ_TELE && !navteles.inrange(navsimrecs[i].job.pad))) return -1;
                navparsephase = 3;
                return 1;
            }
        }
        return 1;
    }

    struct navcachefileinfo { string name; long long size, stamp; };
    static void navcachelist(vector<navcachefileinfo> &files)
    {
        if(!homedir[0]) return;
#ifdef WIN32
        defformatstring(pattern, "%snavcache\\*.l4nav", homedir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if(h == INVALID_HANDLE_VALUE) return;
        do
        {
            if(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            navcachefileinfo &f = files.add();
            copystring(f.name, fd.cFileName);
            f.size = (long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
            f.stamp = (long long)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
        } while(FindNextFileA(h, &fd));
        FindClose(h);
#else
        defformatstring(dir, "%snavcache", homedir);
        DIR *d = opendir(dir);
        if(!d) return;
        while(dirent *de = readdir(d))
        {
            size_t n = strlen(de->d_name);
            if(n < 7 || strcmp(de->d_name + n - 6, ".l4nav")) continue;
            defformatstring(full, "%s/%s", dir, de->d_name);
            struct stat st;
            if(stat(full, &st) || !S_ISREG(st.st_mode)) continue;
            navcachefileinfo &f = files.add();
            copystring(f.name, de->d_name);
            f.size = st.st_size;
            f.stamp = st.st_mtime;
        }
        closedir(d);
#endif
    }
    static bool navcacheolder(const navcachefileinfo &a, const navcachefileinfo &b) { return a.stamp < b.stamp || (a.stamp == b.stamp && strcmp(a.name, b.name) < 0); }
    // all cached grids together stay under NAV_CACHEMB: the oldest files go first, never the one just written
    static void navcacheprune(const char *keep)
    {
        vector<navcachefileinfo> files;
        navcachelist(files);
        files.sort(navcacheolder);
        long long total = 0, limit = (long long)navcachemb << 20;
        loopv(files) total += files[i].size;
        loopv(files)
        {
            if(total <= limit) break;
            if(!strcmp(files[i].name, keep)) continue;
            defformatstring(full, "%snavcache%c%s", homedir, PATHDIV, files[i].name);
            if(!remove(full)) total -= files[i].size;
        }
    }

    VARFP(navcachemb, 0, 32, 512, navcacheprune("")); // a smaller limit (0: none) takes the oldest files away at once

    // ---- the life of the grid
    enum { NP_BUILD = 0, NP_ZONES, NP_PACK, NP_SCC, NP_CLUSTERS };
    static int navphase = NP_BUILD;

    static void navbeginbuild(int cell)
    {
        navmaxnodes = NAV_MAXNODES;
        navlocal = false;
        if(!navstart(cell)) { navstate = NS_FAILED; navfreegrid(); return; }
        navstate = NS_BUILDING;
        navphase = NP_BUILD;
    }

    // a map of 8192 u or more: its floors seen from above every 64 u first, where its entities are (a slice at a
    // time); clearly more than NAV_BIGMAP floor columns at 8 u: made at 16 u at once; else at 8 u, and again at 16 u if
    // it reaches NAV_BIGMAP nodes (the grid stays complete instead of reaching NAV_MAXNODES at 8 u); smaller maps keep
    // 8 u up to NAV_MAXNODES
    static int navsizecursor = -1, navsizex0 = 0, navsizey0 = 0, navsizew = 0, navsizeh = 0;
    static void navbeginsizing()
    {
        if(navws < 8192) { navbeginbuild(8); return; }
        vec lo(navws, navws, navws), hi(0, 0, 0);
        const vector<extentity *> &ents = entities::getents();
        loopv(ents) if(ents[i]->type == PLAYERSTART || ents[i]->type == FLAG || ents[i]->type == TELEDEST || ents[i]->type == JUMPPAD || ents[i]->type == TELEPORT || (ents[i]->type >= I_SHELLS && ents[i]->type <= I_QUAD))
        {
            lo.min(ents[i]->o); hi.max(ents[i]->o);
        }
        if(lo.x > hi.x) { navbeginbuild(8); return; }
        navsizex0 = max(int(lo.x) - 256, 0)/64; navsizey0 = max(int(lo.y) - 256, 0)/64;
        navsizew = min(int(hi.x) + 256, navws - 1)/64 - navsizex0 + 1; navsizeh = min(int(hi.y) + 256, navws - 1)/64 - navsizey0 + 1;
        navsizecursor = 0;
        navc.sizehits = 0;
        navstate = NS_BUILDING;
        navphase = NP_BUILD;
    }
    static void navsizeslice()
    {
        while(navsizecursor < navsizew*navsizeh)
        {
            if(!(navsizecursor&7) && navtimeup()) return;
            vec o((navsizex0 + navsizecursor%navsizew + 0.5f)*64, (navsizey0 + navsizecursor/navsizew + 0.5f)*64, navws - 1), n(0, 0, 1);
            float d = rayfloor(o, n, RAY_CLIPMAT, navws);
            if(d > 0.5f && d < navws - 1.5f && n.z >= NAV_SLOPEZ && !navdeadly(vec(o.x, o.y, o.z - d))) navc.sizehits++; // not a death floor under the map
            navsizecursor++;
        }
        navsizecursor = -1;
        if(navc.sizehits*64 > 2*NAV_BIGMAP) { navc.cell8capped = -1; navbeginbuild(16); }
        else { navbeginbuild(8); if(navstate == NS_BUILDING) navmaxnodes = NAV_BIGMAP; }
    }

    static bool navbeginload()
    {
        if(!navcrc || navedits || !homedir[0] || !navcachemb) return false;
        string name;
        navcachefilename(name, sizeof(name));
        const char *fp = findfile(name, "rb");
        if(!fp || strncmp(fp, homedir, strlen(homedir)) || !fileexists(fp, "r")) return false;
        navcachefile = opengzfile(name, "rb"); // the name relative to the profile (openfile looks there first)
        if(!navcachefile) return false;
        copystring(navcachename, name);
        navfree(navcachebuf);
        navstate = NS_LOADING;
        return true;
    }

    static void navloadslice()
    {
        if(navcachefile)
        {
            for(;;)
            {
                uchar *dst = navcachebuf.reserve(65536).buf;
                size_t got = navcachefile->read(dst, 65536);
                navcachebuf.advance(int(got));
                if(got < 65536) { DELETEP(navcachefile); break; }
                if(navcachebuf.length() > 64<<20) { navcacheclose(); navbeginsizing(); return; }
                if(navtimeup()) return;
            }
            navc.cachebytes = navcachebuf.length();
            navparsephase = 0;
            navlinkset.init(4096);
        }
        for(;;)
        {
            double t0 = navnow();
            if(t0 >= navdeadline) return;
            int r = navcacheparsestep(navcachebuf);
            navc.loadms += navnow() - t0;
            if(r < 0) { navcacheclose(); navfreegrid(); navbeginsizing(); return; }
            if(r > 0) break;
        }
        navcacheclose();
        navfromcache = true;
        loopv(navsimrecs) navlinkmark(navsimrecs[i].from, navsimrecs[i].to, navsimrecs[i].type);
        loopv(navnodes)
        {
            const navnode &n = navnodes[i];
            loopj(n.nlinks) { const navlink &l = navlinks[n.link0 + j]; if(l.type == NAV_PADIN || l.type == NAV_TELEIN) navlinkmark(i, l.to, l.type); }
        }
        navsettriggers();
        navstate = NS_FINISHING;
        navphase = NP_SCC;
        navsccbegin();
    }

    static void navbeginsave()
    {
        navstate = NS_READY;
        if(!navcrc || navedits || navlocal || navfromcache || !homedir[0] || !navcachemb) return;
        string name;
        navcachefilename(name, sizeof(name));
        const char *fp = findfile(name, "wb"); // makes <profile>/navcache
        if(!fp || strncmp(fp, homedir, strlen(homedir))) return;
        navcachefile = opengzfile(name, "wb", NULL, 1);
        if(!navcachefile) return;
        copystring(navcachename, name);
        navfree(navcachebuf);
        navserphase = navsercursor = 0;
        navc.cachebytes = 0;
        navstate = NS_SAVING;
    }
    static void navsaveslice()
    {
        while(navcachefile)
        {
            if(navtimeup()) return;
            bool done = navcacheserializestep(navcachebuf);
            if(done || navcachebuf.length() >= 65536)
            { // a broken write leaves a file that does not read back: the grid is made again next time
                if(navcachefile->write(navcachebuf.getbuf(), navcachebuf.length()) != size_t(navcachebuf.length())) { navcacheclose(); navstate = NS_READY; return; }
                navc.cachebytes += navcachebuf.length();
                navcachebuf.setsize(0);
            }
            if(done) break;
        }
        navcacheclose();
        const char *base = strrchr(navcachename, PATHDIV);
        navcacheprune(base ? base + 1 : navcachename);
        navstate = NS_READY;
    }

    static void navfinishslice()
    {
        for(;;)
        {
            double t0 = navnow();
            if(t0 >= navdeadline) return;
            switch(navphase)
            {
                case NP_ZONES: navzones(); navc.finishms[0] += navnow() - t0; navphase = NP_PACK; navpkphase = 0; break;
                case NP_PACK:
                {
                    bool done = navpackstep();
                    navc.finishms[1] += navnow() - t0;
                    if(!done) return;
                    navclearwork(); navphase = NP_SCC; navsccbegin();
                    break;
                }
                case NP_SCC:
                {
                    bool done = navsccstep();
                    navc.finishms[2] += navnow() - t0;
                    if(!done) return;
                    navphase = NP_CLUSTERS; navclphase = 0;
                    break;
                }
                case NP_CLUSTERS:
                {
                    bool done = navclustersstep();
                    navc.finishms[3] += navnow() - t0;
                    if(!done) return;
                    if(navfromcache) navstate = NS_READY; else navbeginsave();
                    navlocal = false;
                    return;
                }
                default: navphase = NP_ZONES; break;
            }
        }
    }

    static void navbuildslice()
    {
        if(navsizecursor >= 0) { navsizeslice(); return; }
        if(!navbuildstep()) return;
        if(navcapped)
        { // too big at 8 u: made again at 16 u; still too big: no grid (the bots keep the waypoints)
            if(!navlocal)
            {
                if(navcellsize < 16) { navc.cell8capped = navnodes.length(); navbeginbuild(16); return; }
                navstate = NS_FAILED;
                navfreegrid();
                return;
            }
            navcapped = false; // a local rebuild keeps what it found
        }
        navstate = NS_FINISHING;
        navphase = NP_ZONES;
    }

    // ---- coop edit: the zones that changed are made again, a moment after the edits stop
    struct navdirty { vec lo, hi; };
    static vector<navdirty> navdirties;
    static int navlastedit = 0;
    struct naventstate { vec o; int type; };
    static vector<naventstate> naventsnap;

    static void navadddirty(const vec &lo, const vec &hi)
    {
        if(navstate == NS_NONE || navstate == NS_FAILED) return;
        navdirty &d = navdirties.add();
        d.lo = lo; d.hi = hi;
        navlastedit = totalmillis;
    }
    void navworldchanged(const ivec &bbmin, const ivec &bbmax)
    {
        navedits++;
        navadddirty(vec(bbmin), vec(bbmax));
    }
    void naventchanged(int ei)
    {
        navedits++;
        if(navstate == NS_NONE || navstate == NS_FAILED) return;
        const vector<extentity *> &ents = entities::getents();
        if(!ents.inrange(ei)) return;
        const extentity &e = *ents[ei];
        float r = e.type == MAPMODEL ? 96 : 32;
        if(naventsnap.inrange(ei) && naventsnap[ei].type != NOTUSED)
        {
            const vec &o = naventsnap[ei].o;
            navadddirty(vec(o).sub(r), vec(o).add(r));
        }
        if(e.type != NOTUSED) navadddirty(vec(e.o).sub(r), vec(e.o).add(r));
        while(naventsnap.length() <= ei) { naventstate &s = naventsnap.add(); s.type = NOTUSED; s.o = vec(0, 0, 0); }
        bool wasdest = naventsnap[ei].type == TELEDEST;
        naventsnap[ei].o = e.o; naventsnap[ei].type = e.type;
        // pads and teleporters whose entity (or destination) changed: made again
        loopv(navpads) if(navpads[i].ent == ei) navpads[i].flight = -2;
        loopv(navteles) if(navteles[i].ent == ei || navteles[i].dest == ei || wasdest || e.type == TELEDEST) navteles[i].dest = -2;
        if(e.type == JUMPPAD || e.type == TELEPORT)
        { // a new pad or teleporter
            bool known = false;
            loopv(navpads) if(navpads[i].ent == ei) known = true;
            loopv(navteles) if(navteles[i].ent == ei) known = true;
            if(!known)
            {
                if(e.type == JUMPPAD) { navpadinfo &p = navpads.add(); p.ent = ei; p.vnode = -1; p.flight = -2; }
                else { navteleinfo &t = navteles.add(); t.ent = ei; t.vnode = -1; t.dest = -2; }
            }
        }
    }
    static void navsnapents()
    {
        const vector<extentity *> &ents = entities::getents();
        navfree(naventsnap);
        loopv(ents) { naventstate &s = naventsnap.add(); s.o = ents[i]->o; s.type = ents[i]->type; }
    }

    static inline bool navinbox(const vec &p, const navdirty &d) { return p.x >= d.lo.x && p.y >= d.lo.y && p.z >= d.lo.z && p.x <= d.hi.x && p.y <= d.hi.y && p.z <= d.hi.z; }
    static inline bool navboxhit(const vec &lo, const vec &hi, const navdirty &d) { return lo.x <= d.hi.x && lo.y <= d.hi.y && lo.z <= d.hi.z && hi.x >= d.lo.x && hi.y >= d.lo.y && hi.z >= d.lo.z; }
    static void navkill(int n)
    {
        navnode &nn = navnodes[n];
        if(nn.flags&NAVF_DEAD) return;
        nn.flags |= NAVF_DEAD;
        if(nn.flags&NAVF_VIRTUAL || nn.col < 0) return;
        for(int k = navcolhead(nn.col), prev = -1; k >= 0; prev = k, k = navnodes[k].next) if(k == n)
        {
            if(prev < 0) navcolsethead(nn.col, nn.next); else navnodes[prev].next = nn.next;
            break;
        }
    }
    static void navdropwalk(int from, int to)
    {
        navnode &f = navnodes[from];
        loopj(f.nlinks) { navlink &l = navlinks[f.link0 + j]; if(l.type == NAV_WALK && l.to == to) l.type = NAV_DELETED; }
    }

    static void navbeginlocal()
    {
        navbadset.clear(); // the floors where nobody could stand may have changed
        vector<navdirty> boxes;
        loopv(navdirties)
        { // what a change can affect: a column and a half around it, a player's height and a jump above and below
            navdirty d = navdirties[i];
            d.lo.sub(vec(1.5f*navcellsize, 1.5f*navcellsize, 24)); d.hi.add(vec(1.5f*navcellsize, 1.5f*navcellsize, 24));
            boxes.add(d);
        }
        navfree(navdirties);
        navclearwork();
        navclearclusters();
        int n = navnodes.length();
        // nodes inside: gone (2); within one more column: made again (1)
        vector<uchar> touch;
        touch.reserve(n);
        loopi(n) touch.add(0);
        vector<int> border;
        loopv(boxes)
        {
            const navdirty &d = boxes[i];
            int x0 = max(int(floorf(d.lo.x/navcellsize)) - 1, 0), y0 = max(int(floorf(d.lo.y/navcellsize)) - 1, 0),
                x1 = min(int(floorf(d.hi.x/navcellsize)) + 1, navcolsw - 1), y1 = min(int(floorf(d.hi.y/navcellsize)) + 1, navcolsw - 1);
            for(int y = y0; y <= y1; y++) for(int x = x0; x <= x1; x++)
                for(int k = navcolhead(y*navcolsw + x); k >= 0; k = navnodes[k].next)
                {
                    const vec &o = navnodes[k].o;
                    if(navinbox(o, d)) touch[k] = 2;
                    else if(!touch[k] && o.z >= d.lo.z - 8 && o.z <= d.hi.z + 8) touch[k] = 1;
                }
        }
        loopi(n) if(touch[i] == 1) border.add(i);
        loopi(n) if(touch[i] == 2) navkill(i);
        // simulated links whose flight crosses a changed zone, or from a node made again: gone; simulated again
        // from their start when it stays as it is
        vector<navjob> again;
        loopv(navsimrecs)
        {
            navsimrec &r = navsimrecs[i];
            bool hit = navnodes[r.from].flags&NAVF_DEAD || navnodes[r.to].flags&NAVF_DEAD || (r.from < n && touch[r.from]);
            if(!hit) loopvj(boxes) if(navboxhit(r.lo, r.hi, boxes[j])) { hit = true; break; }
            if(!hit) continue;
            const navnode &f = navnodes[r.from];
            loopj(f.nlinks) { navlink &l = navlinks[f.link0 + j]; if(l.to == r.to && l.type == r.type) l.type = NAV_DELETED; }
            navlinkset.erase(navlinkkey(r.from, r.to, r.type)); // the simulation run again may make it again
            if(!(f.flags&NAVF_DEAD) && !(r.from < n && touch[r.from])) again.add(r.job);
            navsimrecs.removeunordered(i--);
        }
        // pads and teleporters in a changed zone or edited: made again
        const vector<extentity *> &ents = entities::getents();
        loopv(navpads)
        {
            navpadinfo &p = navpads[i];
            bool hit = p.flight == -2 || p.vnode < 0 || !ents.inrange(p.ent) || ents[p.ent]->type != JUMPPAD;
            if(!hit) loopvj(boxes) if(navinbox(ents[p.ent]->o, boxes[j])) { hit = true; break; }
            if(!hit) continue;
            if(p.vnode >= 0) navkill(p.vnode);
            p.vnode = -1;
        }
        loopv(navteles)
        {
            navteleinfo &t = navteles[i];
            bool hit = t.dest == -2 || t.vnode < 0 || !ents.inrange(t.ent) || ents[t.ent]->type != TELEPORT;
            if(!hit) loopvj(boxes) if(navinbox(ents[t.ent]->o, boxes[j]) || (ents.inrange(t.dest) && navinbox(ents[t.dest]->o, boxes[j]))) { hit = true; break; }
            if(!hit) continue;
            if(t.vnode >= 0) navkill(t.vnode);
            t.vnode = -1;
        }
        loopv(again)
        {
            const navjob &j = again[i];
            if(navnodes[j.from].flags&NAVF_DEAD) continue;
            if(j.kind == NJ_PAD && !(navpads.inrange(j.pad) && navpads[j.pad].vnode == j.from)) continue;
            if(j.kind == NJ_TELE && !(navteles.inrange(j.pad) && navteles[j.pad].vnode == j.from)) continue;
            navjobs.add(j);
        }
        // the nodes made again: their walks (and those towards them) go, the flood makes them again
        loopv(border)
        {
            int b = border[i];
            navnode &nb = navnodes[b];
            loopj(nb.nlinks) { navlink &l = navlinks[nb.link0 + j]; if(l.type == NAV_WALK) l.type = NAV_DELETED; }
            nb.dirdone = nb.edges = nb.drops = nb.ledges = nb.blocked = nb.vaults = 0;
            navqueue.add(b);
            int bix = nb.col%navcolsw, biy = nb.col/navcolsw;
            loopk(8)
            {
                int x = bix + navdirs[k][0], y = biy + navdirs[k][1];
                if(x < 0 || y < 0 || x >= navcolsw || y >= navcolsw) continue;
                for(int m = navcolhead(y*navcolsw + x); m >= 0; m = navnodes[m].next)
                {
                    if(touch[m]) continue;
                    navdropwalk(m, b);
                    navnodes[m].dirdone &= ~(1<<(7-k));
                    navqueue.add(m);
                }
            }
        }
        // seeds inside the zones (where players start, items...) and pads / teleporters made again
        loopv(ents)
        {
            bool in = false;
            loopvj(boxes) if(navinbox(ents[i]->o, boxes[j])) { in = true; break; }
            if(in) navseedent(*ents[i]);
        }
        loopv(navpads) if(navpads[i].vnode < 0 && ents.inrange(navpads[i].ent) && ents[navpads[i].ent]->type == JUMPPAD)
        {
            navpads[i].vnode = navlaunchnode(*ents[navpads[i].ent]);
            navpads[i].flight = -1;
            navpadq.add(i);
        }
        loopv(navteles) if(navteles[i].vnode < 0 && ents.inrange(navteles[i].ent) && ents[navteles[i].ent]->type == TELEPORT)
        {
            navteles[i].dest = navteledest(*ents[navteles[i].ent]);
            navteles[i].vnode = navlaunchnode(*ents[navteles[i].ent]);
            navteleq.add(i);
        }
        navsettriggers();
        navmaxnodes = navnodes.length() + NAV_MAXNODES/4;
        navcapped = false;
        navlocal = true;
        navc.locals++;
        navstate = NS_BUILDING;
        navphase = NP_BUILD;
    }

    // ---- once per frame
    static void navresetall()
    {
        navfreegrid();
        navcacheclose();
        navfwd.clear(); navbwd.clear(); navabs.clear();
        navfree(navdirties);
        navstate = NS_NONE;
        navmap[0] = 0;
        navcrc = 0;
        navws = navcolsw = 0;
        navcapped = navlocal = navfromcache = false;
        navsizecursor = -1;
        memset(&navc, 0, sizeof(navc));
        navgeneration++;
    }

    void navmapstart()
    {
        navresetall();
        navfree(naventsnap);
        navedits = 0;
    }

    void navupdate()
    {
        if(!navwanted()) { navlastframe = 0; return; } // nothing to do (a build in progress waits for a Human bot to play again)
        double now = navnow();
        if(navlastframe > 0)
        {
            double dt = now - navlastframe;
            if(dt > 0 && dt < 1000) navframeavg += (dt - navframeavg)*0.1;
        }
        navlastframe = now;
        if(navstate != NS_NONE && (strcmp(navmap, game::getclientmap()) || navws != getworldsize())) navresetall();
        // the frame without this work (its own slices would otherwise make the next ones longer and longer)
        double frame = max(navframeavg - navworkavg, 0.5);
        navdeadline = now + clamp(frame*(intermission ? NAV_SLICEPAUSE : navslice()), 0.25, NAV_SLICEMAX);
        switch(navstate)
        {
            case NS_NONE:
                navresetall();
                copystring(navmap, game::getclientmap());
                navcrc = getmapcrc();
                navws = getworldsize();
                if(naventsnap.empty()) navsnapents();
                navc.started = now;
                if(!navbeginload()) navbeginsizing();
                break;
            case NS_LOADING: navloadslice(); break;
            case NS_BUILDING: navbuildslice(); break;
            case NS_FINISHING: navfinishslice(); break;
            case NS_SAVING: navsaveslice(); break;
            case NS_READY:
                if(navdirties.length() && totalmillis - navlastedit > 250) navbeginlocal();
                else { navworkavg = 0; return; }
                break;
            case NS_FAILED: return;
        }
        double spent = navnow() - now;
        navworkavg += (spent - navworkavg)*0.1;
        navc.work += spent;
        navc.frames++;
        if(spent > navc.worst) navc.worst = spent;
    }

    bool navready() { return navstate == NS_READY && navhpaready; }

    int navnearestnode(const vec &feet, float maxdist)
    {
        if(!navready()) return -1;
        return navnearest(feet, maxdist, maxdist);
    }

#include "ainavbot.h"
}

namespace game
{
    void worldchanged(const ivec &bbmin, const ivec &bbmax) { ai::navworldchanged(bbmin, bbmax); }
}

namespace ai
{
    static bool navwanted()
    {
        loopv(players) { fpsent *d = players[i]; if(d->ai && navbotmode(d) && botnavgrid) return true; }
        return false;
    }
    void navrender() {}
}
