// Demo analysis (/demoanalyse): indications of cheating read from demos, on
// this computer only. Nothing is sent and nothing changes in a game: the demo
// files (.dmo, and the .dmohd of this client when there is one) are only read.
// Off by default (demoanalyse 0): then nothing runs and no file is written.
//
// Four groups of checks, the measures and rules of the studies made on many
// recorded games (same numbers as the study tools):
//  1. hitbox (where the shooter's game stopped a hit: on the edge of the body
//     it saw), shot clock (a game that runs too fast), speed and teleports;
//  2. aim: snap (a jump of the view that ends on the shot), tracking (the view
//     stays on a moving enemy), trigger (the shot leaves as the view reaches
//     the body), each only when it repeats and stands far from the players;
//  3. hidden enemies: the view kept on an enemy behind a wall (A), shots at a
//     hidden enemy (C), the view following a hidden enemy who moves (F),
//     waiting for someone (B, information only); never an alert against a
//     single enemy (a duel: knowing where he is is normal).
//  4. walls, lag, flying: hits through walls on an enemy the shooter could
//     not see, frags right after his own gaps in the positions (lag switch),
//     in the air with a fall that never grows (flying); jumps the game's
//     gravity cannot make and the body inside walls are only "to look at".
// Indications, not proof: a demo shows neither the screen nor the game used.
// Each also off by default: the player stats (what each player did in the
// recorded games and the analyzed demos, by name, mode and map: the page
// Player stats), the automatic analysis of each recorded game at its end (a
// line only when a player is worth a look), and the local demos worth a look
// shown in another colour in the list of the demos until they are checked
// (while the demo analysis is on, the automatic clean-up keeps them until then).
//
// The work runs in a background thread. The line of sight tests need the map
// of the demo, loaded by the game: they are done on the main thread with the
// game's own test (raycube, as the game decides what a player sees), a few ms
// per frame at most (demoanalysemsframe), so the game never stops. Group 4
// asks the same way for the walls and floors (the shot's test, the collision
// of a player).

#ifdef DEMOANALYSE_STANDALONE
#include "cube.h"
#else
#include "game.h"
#endif

namespace demoanalyse
{
    // ------------------------------------------------------------ numbers
    // The studies were made with numpy: the same operations in the same order
    // (pairwise sums, linear interpolation, percentiles), in double precision.
    static const double DPI = 3.141592653589793;
    static const double DRAD = DPI/180.0;
    static const double DNAN = __builtin_nan("");
    static const double DINF = __builtin_inf();

    static inline bool isnand(double x) { ullong u; memcpy(&u, &x, 8); return (u&0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (u&0x000FFFFFFFFFFFFFULL); }
    static inline bool isinfd(double x) { ullong u; memcpy(&u, &x, 8); return (u&0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL; }
    static inline bool finited(double x) { return !isnand(x) && !isinfd(x); }
    static inline double dmin(double a, double b) { return a < b ? a : b; }
    static inline double dmax(double a, double b) { return a > b ? a : b; }
    static inline double dclamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
    // np.fmin / np.fmax ignore a NaN
    static inline double dfmin(double a, double b) { return isnand(a) ? b : (isnand(b) ? a : (a < b ? a : b)); }

    // numpy pairwise summation (add.reduce of float64)
    static double pwsum(const double *a, int n)
    {
        if(n < 8)
        {
            double r = 0;
            for(int i = 0; i < n; i++) r += a[i];
            return r;
        }
        if(n <= 128)
        {
            double r[8];
            for(int k = 0; k < 8; k++) r[k] = a[k];
            int i;
            for(i = 8; i < n - (n%8); i += 8) for(int k = 0; k < 8; k++) r[k] += a[i+k];
            double res = ((r[0]+r[1])+(r[2]+r[3])) + ((r[4]+r[5])+(r[6]+r[7]));
            for(; i < n; i++) res += a[i];
            return res;
        }
        int n2 = n/2;
        n2 -= n2%8;
        return pwsum(a, n2) + pwsum(a+n2, n-n2);
    }

    typedef vector<double> dlist;

    static double npmean(const double *a, int n) { return n > 0 ? pwsum(a, n)/n : DNAN; }

    static bool dless(const double &a, const double &b) { return a < b; }

    // sorted copy without NaN; false when a NaN is present (numpy then gives NaN)
    static bool sortedcopy(const double *a, int n, dlist &s)
    {
        s.setsize(0);
        loopi(n) { if(isnand(a[i])) return false; s.add(a[i]); }
        s.sort(dless);
        return true;
    }

    static double npmedian(const double *a, int n)
    {
        if(n <= 0) return DNAN;
        dlist s;
        if(!sortedcopy(a, n, s)) return DNAN;
        if(n&1) return s[n/2];
        return (s[n/2-1] + s[n/2])/2.0;
    }
    static double npmedian(const dlist &v) { return npmedian(v.getbuf(), v.length()); }

    // np.percentile, method 'linear' (numpy _lerp)
    static double sortedpercentile(const dlist &s, double q)
    {
        int n = s.length();
        if(n <= 0) return DNAN;
        double vi = (n-1)*(q/100.0);
        int lo, hi;
        if(vi >= n-1) lo = hi = n-1;
        else if(vi < 0) lo = hi = 0;
        else { lo = int(floor(vi)); hi = lo+1; }
        double g = vi - (vi >= n-1 ? n-1 : (vi < 0 ? 0 : floor(vi)));
        if(vi >= n-1) g = vi - floor(vi);
        double a = s[lo], b = s[hi], d = b-a;
        double r = a + d*g;
        if(g >= 0.5) r = b - d*(1-g);
        return r;
    }
    static double nppercentile(const double *a, int n, double q)
    {
        dlist s;
        if(n <= 0 || !sortedcopy(a, n, s)) return DNAN;
        return sortedpercentile(s, q);
    }
    static double nppercentile(const dlist &v, double q) { return nppercentile(v.getbuf(), v.length(), q); }

    // np.searchsorted (side left / right) on a sorted array
    static int ssleft(const double *a, int n, double v)
    {
        int lo = 0, hi = n;
        while(lo < hi) { int m = (lo+hi)>>1; if(a[m] < v) lo = m+1; else hi = m; }
        return lo;
    }
    static int ssright(const double *a, int n, double v)
    {
        int lo = 0, hi = n;
        while(lo < hi) { int m = (lo+hi)>>1; if(a[m] <= v) lo = m+1; else hi = m; }
        return lo;
    }
    static int ssleft(const dlist &a, double v) { return ssleft(a.getbuf(), a.length(), v); }
    static int ssright(const dlist &a, double v) { return ssright(a.getbuf(), a.length(), v); }

    // np.interp (xp increasing; a repeated xp gives its last value)
    static double npinterp(double x, const double *xp, const double *fp, int n)
    {
        if(n <= 0) return DNAN;
        if(isnand(x)) return x;
        if(x > xp[n-1]) return fp[n-1];
        if(x < xp[0]) return fp[0];
        int j = ssright(xp, n, x) - 1;
        if(j >= n-1) return fp[n-1];
        if(xp[j] == x) return fp[j];
        double slope = (fp[j+1] - fp[j])/(xp[j+1] - xp[j]);
        double r = slope*(x - xp[j]) + fp[j];
        if(isnand(r))
        {
            r = slope*(x - xp[j+1]) + fp[j+1];
            if(isnand(r) && fp[j] == fp[j+1]) r = fp[j];
        }
        return r;
    }

    // stable argsort (np.argsort kind='stable') of n keys
    static void stableargsort(const double *k, int n, vector<int> &idx)
    {
        idx.setsize(0);
        loopi(n) idx.add(i);
        if(n < 2) return;
        vector<int> tmp;
        tmp.reserve(n);
        loopi(n) tmp.add(0);
        int *a = idx.getbuf(), *b = tmp.getbuf();
        for(int w = 1; w < n; w *= 2)
        {
            for(int lo = 0; lo < n; lo += 2*w)
            {
                int mid = min(lo+w, n), hi = min(lo+2*w, n), i = lo, j = mid, o = lo;
                while(i < mid && j < hi) b[o++] = k[a[j]] < k[a[i]] ? a[j++] : a[i++];
                while(i < mid) b[o++] = a[i++];
                while(j < hi) b[o++] = a[j++];
            }
            swap(a, b);
        }
        if(a != idx.getbuf()) memcpy(idx.getbuf(), a, n*sizeof(int));
    }

    // np.unwrap of angles in radians, the result divided by RAD (degrees)
    static void unwraprad(const double *p, int n, double *out)
    {
        if(n <= 0) return;
        out[0] = p[0];
        double cum = 0;
        for(int i = 1; i < n; i++)
        {
            double dd = p[i]-p[i-1];
            double m = fmod(dd + DPI, 2*DPI);
            if(m < 0) m += 2*DPI;
            double ddmod = m - DPI;
            if(ddmod == -DPI && dd > 0) ddmod = DPI;
            double corr = ddmod - dd;
            if(fabs(dd) < DPI) corr = 0;
            cum += corr;
            out[i] = p[i] + cum;
        }
        loopi(n) out[i] /= DRAD;
    }

    // np.unwrap of angles in degrees, done in radians as the studies did:
    // unwrap(deg*RAD)/RAD
    static void unwrapdeg(const double *deg, int n, double *out)
    {
        if(n <= 0) return;
        double *p = new double[n];
        loopi(n) p[i] = deg[i]*DRAD;
        unwraprad(p, n, out);
        delete[] p;
    }

    // round half to even (Python round, np.round)
    static inline double roundeven(double x) { return nearbyint(x); }

    struct dv3
    {
        double x, y, z;
        dv3() {}
        dv3(double a, double b, double c) : x(a), y(b), z(c) {}
        double &operator[](int i) { return (&x)[i]; }
        double operator[](int i) const { return (&x)[i]; }
        dv3 operator+(const dv3 &o) const { return dv3(x+o.x, y+o.y, z+o.z); }
        dv3 operator-(const dv3 &o) const { return dv3(x-o.x, y-o.y, z-o.z); }
        dv3 operator*(double k) const { return dv3(x*k, y*k, z*k); }
        dv3 operator/(double k) const { return dv3(x/k, y/k, z/k); }
        double dot(const dv3 &o) const { return x*o.x + y*o.y + z*o.z; }
        double norm() const { return sqrt(x*x + y*y + z*z); }
    };

    // numpy: arccos(clip(sum(a*b), -1, 1)) / RAD
    static inline double angledeg(const dv3 &a, const dv3 &b) { return acos(dclamp(a.x*b.x + a.y*b.y + a.z*b.z, -1, 1))/DRAD; }
    static inline dv3 dirvec(double yaw, double pitch)
    {
        double y = yaw*DRAD, p = pitch*DRAD;
        return dv3(-sin(y)*cos(p), cos(y)*cos(p), sin(p));
    }
    static inline dv3 unitvec(const dv3 &v, double minlen) { double n = v.norm(); return v/(n > minlen ? n : minlen); }

    // ------------------------------------------------------------ demo reader
    // Message layouts of the stock client (client.cpp parsemessages and
    // parsepositions, protocol 260), read as data only; chat text is skipped
    // and never kept. Same reader as the studies: a demo file can hold several
    // games one after the other (the clock starts again near 0).
    enum
    {
        N_CONNECT_ = 0, N_SERVINFO_, N_WELCOME_, N_INITCLIENT_, N_POS_, N_TEXT_, N_SOUND_, N_CDIS_,
        N_SHOOT_, N_EXPLODE_, N_SUICIDE_, N_DIED_, N_DAMAGE_, N_HITPUSH_, N_SHOTFX_, N_EXPLODEFX_,
        N_TRYSPAWN_, N_SPAWNSTATE_, N_SPAWN_, N_FORCEDEATH_, N_GUNSELECT_, N_TAUNT_,
        N_MAPCHANGE_, N_MAPVOTE_, N_TEAMINFO_, N_ITEMSPAWN_, N_ITEMPICKUP_, N_ITEMACC_, N_TELEPORT_, N_JUMPPAD_,
        N_PING_, N_PONG_, N_CLIENTPING_, N_TIMEUP_, N_FORCEINTERMISSION_, N_SERVMSG_, N_ITEMLIST_, N_RESUME_,
        N_EDITMODE_, N_EDITENT_, N_EDITF_, N_EDITT_, N_EDITM_, N_FLIP_, N_COPY_, N_PASTE_, N_ROTATE_, N_REPLACE_,
        N_DELCUBE_, N_REMIP_, N_EDITVSLOT_, N_UNDO_, N_REDO_, N_NEWMAP_, N_GETMAP_, N_SENDMAP_, N_CLIPBOARD_, N_EDITVAR_,
        N_MASTERMODE_, N_KICK_, N_CLEARBANS_, N_CURRENTMASTER_, N_SPECTATOR_, N_SETMASTER_, N_SETTEAM_,
        N_BASES_, N_BASEINFO_, N_BASESCORE_, N_REPAMMO_, N_BASEREGEN_, N_ANNOUNCE_,
        N_LISTDEMOS_, N_SENDDEMOLIST_, N_GETDEMO_, N_SENDDEMO_, N_DEMOPLAYBACK_, N_RECORDDEMO_, N_STOPDEMO_, N_CLEARDEMOS_,
        N_TAKEFLAG_, N_RETURNFLAG_, N_RESETFLAG_, N_INVISFLAG_, N_TRYDROPFLAG_, N_DROPFLAG_, N_SCOREFLAG_, N_INITFLAGS_,
        N_SAYTEAM_, N_CLIENT_, N_AUTHTRY_, N_AUTHKICK_, N_AUTHCHAL_, N_AUTHANS_, N_REQAUTH_,
        N_PAUSEGAME_, N_GAMESPEED_, N_ADDBOT_, N_DELBOT_, N_INITAI_, N_FROMAI_, N_BOTLIMIT_, N_BOTBALANCE_,
        N_MAPCRC_, N_CHECKMAPS_, N_SWITCHNAME_, N_SWITCHMODEL_, N_SWITCHTEAM_,
        N_INITTOKENS_, N_TAKETOKEN_, N_EXPIRETOKENS_, N_DROPTOKENS_, N_DEPOSITTOKENS_, N_STEALTOKENS_,
        N_SERVCMD_, N_DEMOPACKET_, N_HDPOS_, N_SENDDEMOHD_
    };

    enum
    {
        E_MAPCHANGE = 0, E_FORCEDEATH, E_INITCLIENT, E_SWITCHNAME, E_CDIS, E_SPAWN, E_SPAWNSTATE,
        E_SHOT, E_EXPLODE, E_DAMAGE, E_HITPUSH, E_DIED, E_GUNSELECT, E_RESUME, E_ITEMACC, E_PING,
        E_TIMEUP, E_EDITMODE, E_SPECTATOR, E_SETTEAM, E_DROPFLAG, E_SCOREFLAG, E_RETURNFLAG,
        E_TAKEFLAG, E_INITAI, E_TELEPORT, E_JUMPPAD, E_SOUND
    };

    // one event of the demo; fields by type:
    //  mapchange: s1 map, a mode          shot: cn, a gun, b id, fr, to
    //  damage: cn target, a actor          died: cn victim, a actor, b frags
    //  spawn / spawnstate: cn, a gun       resume: cn, a state, b gun
    //  initclient: cn, s1 name, s2 team    switchname: cn, s1 name
    //  initai: cn, a skill, b owner, s1 name, s2 team
    //  setteam: cn, s2 team                ping: cn, a ping
    //  timeup: a remain                    editmode: cn, a value
    //  spectator: cn, a value              gunselect: cn, a gun
    //  itemacc: cn, a item                 sound: cn, a sound
    //  explode: cn, a gun, b id            hitpush: cn target, a gun, b damage
    //  forcedeath, cdis, flags, teleport, jumppad: cn
    struct devent
    {
        int t, seg, type, cn, a, b, s1, s2;
        double fr[3], to[3];
    };

    // one N_POS (position of a player at the time of the packet)
    struct dposx
    {
        int t, seg, cn;
        float o[3];
        short yaw, pitch;
        uchar phys, seq;
        double vel[3], fvec[3];
    };

    struct rawdemo
    {
        int version, protocol;
        vector<devent> ev;
        vector<dposx> pos;
        vector<char> pool;
        vector<int> seglast, segpackets;
        int nsegs, lastall, errors;
        bool truncated;

        rawdemo() : version(0), protocol(0), nsegs(0), lastall(0), errors(0), truncated(false) {}
        const char *str(int i) const { return i >= 0 && i < pool.length() ? &pool[i] : ""; }
        int addstr(const char *s, int len)
        {
            int i = pool.length();
            loopk(len) pool.add(s[k]);
            pool.add('\0');
            return i;
        }
    };

    struct dabuf
    {
        const uchar *d;
        int i, n;
        bool over;
        dabuf(const uchar *d, int i, int n) : d(d), i(i), n(n), over(false) {}
        int rem() const { return n - i; }
        int get() { if(i >= n) { over = true; return 0; } return d[i++]; }
        int getint()
        {
            int c = get();
            if(c > 127) c -= 256;
            if(c == -128) { int v = get(), b = get(); if(b > 127) b -= 256; return v | (b*256); }
            if(c == -127) { uint v = uint(get()); v |= uint(get())<<8; v |= uint(get())<<16; v |= uint(get())<<24; return int(v); }
            return c;
        }
        int getuint()
        {
            int v = get();
            if(v&0x80)
            {
                v += (get()<<7) - 0x80;
                if(v&(1<<14)) v += (get()<<14) - (1<<14);
                if(v&(1<<21)) v += (get()<<21) - (1<<21);
                if(v&(1<<28)) v |= int(0xF0000000U);
            }
            return v;
        }
        int getstring(rawdemo &rd)
        {
            char buf[MAXTRANS];
            int len = 0;
            for(;;)
            {
                if(!rem()) break;
                int c = getint();
                if(c == 0) break;
                if(len < int(sizeof(buf))-1) buf[len++] = char(c&0xFF);
            }
            return rd.addstr(buf, len);
        }
        void skipstring() { for(;;) { if(!rem()) break; if(getint() == 0) break; } }
        void getfloat() { if(i + 4 > n) { over = true; i = n; return; } i += 4; }
        dabuf sub(int ln) { ln = max(0, min(ln, rem())); dabuf b(d, i, i + ln); i += ln; return b; }
    };

    struct demoparser
    {
        rawdemo &rd;
        int seg, t, segmode;
        demoparser(rawdemo &rd) : rd(rd), seg(0), t(0), segmode(-1) {}

        devent &addev(int type, int cn)
        {
            devent &e = rd.ev.add();
            memset(&e, 0, sizeof(e));
            e.t = t; e.seg = seg; e.type = type; e.cn = cn; e.s1 = e.s2 = -1;
            return e;
        }

        void getstate(dabuf &b, int &gun) { loopi(5) b.getint(); gun = b.getint(); loopi(6) b.getint(); }

        void readpos(dabuf &b)
        {
            dposx &p = rd.pos.add();
            p.t = t; p.seg = seg;
            p.cn = b.getuint();
            int phys = b.get();
            int flags = b.getuint();
            loopk(3)
            {
                int v = b.get();
                v |= b.get()<<8;
                if(flags&(1<<k))
                {
                    v |= b.get()<<16;
                    if(v&0x800000) v -= 1<<24;
                }
                p.o[k] = float(v/16.0);
            }
            int dir = b.get();
            dir |= b.get()<<8;
            p.yaw = short(dir%360);
            p.pitch = short(clamp(dir/360, 0, 180) - 90);
            b.get(); // roll
            int mag = b.get();
            if(flags&(1<<3)) mag |= b.get()<<8;
            int vd = b.get();
            vd |= b.get()<<8;
            double vyaw = (vd%360)*DRAD, vpit = (clamp(vd/360, 0, 180) - 90)*DRAD;
            p.vel[0] = -sin(vyaw)*cos(vpit)*mag;
            p.vel[1] = cos(vyaw)*cos(vpit)*mag;
            p.vel[2] = sin(vpit)*mag;
            p.fvec[0] = p.fvec[1] = p.fvec[2] = 0;
            if(flags&(1<<4))
            {
                int fall = b.get();
                if(flags&(1<<5)) fall |= b.get()<<8;
                if(flags&(1<<6))
                {
                    int fd = b.get();
                    fd |= b.get()<<8;
                    double fyaw = (fd%360)*DRAD, fpit = (clamp(fd/360, 0, 180) - 90)*DRAD;
                    p.fvec[0] = -sin(fyaw)*cos(fpit)*fall;
                    p.fvec[1] = cos(fyaw)*cos(fpit)*fall;
                    p.fvec[2] = sin(fpit)*fall;
                }
                else p.fvec[2] = -double(fall);
            }
            p.phys = uchar(phys&7);
            p.seq = uchar((phys>>3)&1);
        }

        void positions(dabuf &b)
        {
            while(b.rem() > 0)
            {
                int typ = b.getint();
                if(typ == N_DEMOPACKET_) continue;
                if(typ == N_POS_) readpos(b);
                else if(typ == N_TELEPORT_) { int cn = b.getint(); b.getint(); b.getint(); addev(E_TELEPORT, cn); }
                else if(typ == N_JUMPPAD_) { int cn = b.getint(); b.getint(); addev(E_JUMPPAD, cn); }
                else { rd.errors++; return; }
                if(b.over) return;
            }
        }

        void messages(dabuf &b, int cn)
        {
            while(b.rem() > 0)
            {
                int typ = b.getint();
                switch(typ)
                {
                    case N_DEMOPACKET_: continue;
                    case N_SERVINFO_: b.getint(); b.getint(); b.getint(); b.getint(); b.skipstring(); b.skipstring(); break;
                    case N_WELCOME_: case N_REMIP_: break;
                    case N_TAUNT_: if(cn < 0) return; break;
                    case N_PAUSEGAME_: case N_GAMESPEED_: b.getint(); b.getint(); break;
                    case N_CLIENT_: { int ccn = b.getint(); int ln = b.getuint(); dabuf q = b.sub(ln); messages(q, ccn); break; }
                    case N_SOUND_: { if(cn < 0) return; int s = b.getint(); addev(E_SOUND, cn).a = s; break; }
                    case N_TEXT_: if(cn < 0) return; b.skipstring(); break;
                    case N_SAYTEAM_: b.getint(); b.skipstring(); break;
                    case N_MAPCHANGE_:
                    {
                        int m = b.getstring(rd); int mode = b.getint(); b.getint();
                        devent &e = addev(E_MAPCHANGE, -1); e.s1 = m; e.a = mode;
                        segmode = mode;
                        break;
                    }
                    case N_FORCEDEATH_: { int c = b.getint(); addev(E_FORCEDEATH, c); break; }
                    case N_ITEMLIST_: for(;;) { int v = b.getint(); if(v < 0 || b.over) break; b.getint(); } break;
                    case N_INITCLIENT_:
                    {
                        int c = b.getint(); int nm = b.getstring(rd); int tm = b.getstring(rd); b.getint();
                        devent &e = addev(E_INITCLIENT, c); e.s1 = nm; e.s2 = tm;
                        break;
                    }
                    case N_SWITCHNAME_:
                    {
                        int nm = b.getstring(rd);
                        if(cn >= 0) addev(E_SWITCHNAME, cn).s1 = nm;
                        break;
                    }
                    case N_SWITCHMODEL_: b.getint(); break;
                    case N_CDIS_: { int c = b.getint(); addev(E_CDIS, c); break; }
                    case N_SPAWN_: { int gun; getstate(b, gun); addev(E_SPAWN, cn).a = gun; break; }
                    case N_SPAWNSTATE_: { int c = b.getint(); int gun; getstate(b, gun); addev(E_SPAWNSTATE, c).a = gun; break; }
                    case N_SHOTFX_:
                    {
                        int c = b.getint(), gun = b.getint(), id = b.getint();
                        double fr[3], to[3];
                        loopk(3) fr[k] = b.getint()/16.0;
                        loopk(3) to[k] = b.getint()/16.0;
                        devent &e = addev(E_SHOT, c); e.a = gun; e.b = id;
                        loopk(3) { e.fr[k] = fr[k]; e.to[k] = to[k]; }
                        break;
                    }
                    case N_EXPLODEFX_: { int c = b.getint(), gun = b.getint(), id = b.getint(); devent &e = addev(E_EXPLODE, c); e.a = gun; e.b = id; break; }
                    case N_DAMAGE_:
                    {
                        int tc = b.getint(), ac = b.getint(), dmg = b.getint(); b.getint(); b.getint();
                        devent &e = addev(E_DAMAGE, tc); e.a = ac; e.b = dmg;
                        break;
                    }
                    case N_HITPUSH_:
                    {
                        int tc = b.getint(), gun = b.getint(), dmg = b.getint(); loopi(3) b.getint();
                        devent &e = addev(E_HITPUSH, tc); e.a = gun; e.b = dmg;
                        break;
                    }
                    case N_DIED_:
                    {
                        int vc = b.getint(), ac = b.getint(), fr = b.getint(); b.getint();
                        devent &e = addev(E_DIED, vc); e.a = ac; e.b = fr;
                        break;
                    }
                    case N_TEAMINFO_:
                        for(;;)
                        {
                            int s = b.getstring(rd);
                            bool empty = !rd.pool[s];
                            rd.pool.setsize(s);
                            if(b.over || empty) break;
                            b.getint();
                            if(b.over) break;
                        }
                        break;
                    case N_GUNSELECT_: { if(cn < 0) return; int g = b.getint(); addev(E_GUNSELECT, cn).a = g; break; }
                    case N_RESUME_:
                        for(;;)
                        {
                            int c = b.getint();
                            if(b.over || c < 0) break;
                            int st = b.getint(); loopi(4) b.getint();
                            int gun; getstate(b, gun);
                            devent &e = addev(E_RESUME, c); e.a = st; e.b = gun;
                        }
                        break;
                    case N_ITEMSPAWN_: b.getint(); break;
                    case N_ITEMACC_: { int i = b.getint(), c = b.getint(); addev(E_ITEMACC, c).a = i; break; }
                    case N_CLIPBOARD_: case N_UNDO_: case N_REDO_: { b.getint(); b.getint(); int pl = b.getint(); b.sub(max(pl, 0)); break; }
                    case N_EDITF_: case N_EDITT_: case N_EDITM_: case N_FLIP_: case N_COPY_: case N_PASTE_: case N_ROTATE_: case N_REPLACE_: case N_DELCUBE_: case N_EDITVSLOT_:
                    {
                        if(cn < 0) return;
                        loopi(13) b.getint();
                        if(typ == N_EDITF_) { b.getint(); b.getint(); }
                        else if(typ == N_EDITT_ || typ == N_EDITVSLOT_ || typ == N_REPLACE_)
                        {
                            b.getint(); b.getint();
                            if(typ == N_REPLACE_) b.getint();
                            if(b.rem() < 2) return;
                            int ex = b.d[b.i] | (b.d[b.i+1]<<8);
                            b.i += 2;
                            b.sub(ex);
                        }
                        else if(typ == N_EDITM_) { b.getint(); b.getint(); }
                        else if(typ == N_ROTATE_) b.getint();
                        break;
                    }
                    case N_EDITENT_: if(cn < 0) return; loopi(10) b.getint(); break;
                    case N_EDITVAR_:
                    {
                        if(cn < 0) return;
                        int vt = b.getint(); b.skipstring();
                        if(vt == 0) b.getint();
                        else if(vt == 1) b.getfloat();
                        else if(vt == 2) b.skipstring();
                        break;
                    }
                    case N_PONG_: b.getint(); break;
                    case N_CLIENTPING_: { if(cn < 0) return; int p = b.getint(); addev(E_PING, cn).a = p; break; }
                    case N_TIMEUP_: { int r = b.getint(); addev(E_TIMEUP, -1).a = r; break; }
                    case N_SERVMSG_: b.skipstring(); break;
                    case N_SENDDEMOLIST_: { int n = b.getint(); loopi(max(n, 0)) { b.skipstring(); if(b.over) break; } break; }
                    case N_DEMOPLAYBACK_: b.getint(); b.getint(); break;
                    case N_CURRENTMASTER_: b.getint(); for(;;) { int m = b.getint(); if(m < 0 || b.over) break; b.getint(); } break;
                    case N_MASTERMODE_: b.getint(); break;
                    case N_EDITMODE_: { int v = b.getint(); addev(E_EDITMODE, cn).a = v; break; }
                    case N_SPECTATOR_: { int s = b.getint(), v = b.getint(); addev(E_SPECTATOR, s).a = v; break; }
                    case N_SETTEAM_: { int w = b.getint(); int tm = b.getstring(rd); b.getint(); addev(E_SETTEAM, w).s2 = tm; break; }
                    case N_INITFLAGS_:
                    {
                        b.getint(); b.getint(); int nf = b.getint();
                        loopi(max(nf, 0))
                        {
                            b.getint(); b.getint(); int owner = b.getint(); b.getint();
                            if(owner < 0) { int dr = b.getint(); if(dr) { b.getint(); b.getint(); b.getint(); } }
                            if(segmode == 15 || segmode == 16 || segmode == 19) { int ht = b.getint(); if(ht >= 0) b.getint(); }
                            if(b.over) break;
                        }
                        break;
                    }
                    case N_DROPFLAG_: { int c = b.getint(); b.getint(); b.getint(); loopi(3) b.getint(); addev(E_DROPFLAG, c); break; }
                    case N_SCOREFLAG_: { int c = b.getint(); loopi(8) b.getint(); addev(E_SCOREFLAG, c); break; }
                    case N_RETURNFLAG_: { int c = b.getint(); b.getint(); b.getint(); addev(E_RETURNFLAG, c); break; }
                    case N_TAKEFLAG_: { int c = b.getint(); b.getint(); b.getint(); addev(E_TAKEFLAG, c); break; }
                    case N_RESETFLAG_: loopi(5) b.getint(); break;
                    case N_INVISFLAG_: b.getint(); b.getint(); break;
                    case N_BASEINFO_: b.getint(); b.skipstring(); b.skipstring(); b.getint(); b.getint(); break;
                    case N_BASEREGEN_: loopi(5) b.getint(); break;
                    case N_BASES_: { int nb = b.getint(); loopi(max(nb, 0)) { b.getint(); b.skipstring(); b.skipstring(); b.getint(); b.getint(); if(b.over) break; } break; }
                    case N_BASESCORE_: b.getint(); b.skipstring(); b.getint(); break;
                    case N_REPAMMO_: b.getint(); b.getint(); break;
                    case N_ANNOUNCE_: b.getint(); break;
                    case N_NEWMAP_: b.getint(); break;
                    case N_REQAUTH_: b.skipstring(); break;
                    case N_AUTHCHAL_: b.skipstring(); b.getint(); b.skipstring(); break;
                    case N_INITAI_:
                    {
                        int bn = b.getint(), on = b.getint(); b.getint(); int sk = b.getint(); b.getint();
                        int nm = b.getstring(rd), tm = b.getstring(rd);
                        devent &e = addev(E_INITAI, bn); e.a = sk; e.b = on; e.s1 = nm; e.s2 = tm;
                        break;
                    }
                    case N_INITTOKENS_:
                    {
                        b.getint(); b.getint(); int nt = b.getint();
                        loopi(max(nt, 0)) { loopk(6) b.getint(); if(b.over) break; }
                        for(;;) { int c = b.getint(); if(c < 0 || b.over) break; b.getint(); }
                        break;
                    }
                    case N_TAKETOKEN_: loopi(3) b.getint(); break;
                    case N_EXPIRETOKENS_: for(;;) { int i = b.getint(); if(b.over || i < 0) break; } break;
                    case N_DROPTOKENS_: b.getint(); loopi(3) b.getint(); for(;;) { int i = b.getint(); if(i < 0 || b.over) break; b.getint(); b.getint(); } break;
                    case N_STEALTOKENS_: loopi(5) b.getint(); loopi(3) b.getint(); for(;;) { int i = b.getint(); if(i < 0 || b.over) break; b.getint(); } break;
                    case N_DEPOSITTOKENS_: loopi(6) b.getint(); break;
                    case N_SERVCMD_: b.skipstring(); break;
                    case N_HDPOS_:
                    {
                        int n = b.getint();
                        loopi(max(0, min(n, 32))) { loopk(5) b.getint(); loopk(6) b.getfloat(); }
                        break;
                    }
                    default:
                        rd.errors++;
                        return;
                }
                if(b.over) { rd.errors++; return; }
            }
        }

        // the whole decompressed .dmo
        bool parse(const uchar *data, int n)
        {
            if(n < 24) return false;
            if(memcmp(data, "SAUERBRATEN_DEMO", 16)) return false;
            memcpy(&rd.version, data+16, 4);
            memcpy(&rd.protocol, data+20, 4);
            lilswap(&rd.version, 1);
            lilswap(&rd.protocol, 1);
            int i = 24, tmax = -1;
            seg = 0;
            segmode = -1;
            rd.seglast.add(0);
            rd.segpackets.add(0);
            while(i + 12 <= n)
            {
                int st[3];
                memcpy(st, data+i, 12);
                lilswap(st, 3);
                i += 12;
                int ln = st[2];
                if(ln < 0 || i + ln > n) { rd.truncated = true; break; }
                t = st[0];
                if(t < tmax - 1000)
                {
                    seg++; tmax = t; segmode = -1;
                    rd.seglast.add(0);
                    rd.segpackets.add(0);
                }
                tmax = max(tmax, t);
                rd.segpackets[seg]++;
                rd.seglast[seg] = max(rd.seglast[seg], t);
                rd.lastall = max(rd.lastall, t);
                dabuf b(data, i, i + ln);
                i += ln;
                if(st[1] == 0) positions(b);
                else if(st[1] == 1) messages(b, -1);
            }
            rd.nsegs = seg + 1;
            return true;
        }
    };

    // ------------------------------------------------------------ .dmohd
    // HD sidecar of this client: header of 220 bytes, then samples of 32 bytes
    // {int t; uchar cn, flags, gun, state; float yaw, pitch, roll, x, y, z}; the
    // visibility records (cn 255) are left out (demohd.cpp writehdsample).
    #define DA_HD_OWN 1
    #define DA_HD_EXT_CN 255
    struct hdsamp { int t; int cn; uchar flags; float yaw, pitch, x, y, z; };
    struct hddata
    {
        bool ok;
        int version, rec_cn;
        vector<hdsamp> s;
        hddata() : ok(false), version(0), rec_cn(-1) {}
    };

    static bool parsehd(const uchar *data, int n, hddata &hd)
    {
        hd.ok = false;
        hd.s.setsize(0);
        if(n < 220 || memcmp(data, "SAUERHD", 7)) return false;
        ushort ver;
        memcpy(&ver, data+8, 2);
        lilswap(&ver, 1);
        int rec;
        memcpy(&rec, data+20, 4);
        lilswap(&rec, 1);
        hd.version = ver;
        hd.rec_cn = rec;
        int ns = (n - 220)/32;
        hd.s.reserve(ns);
        for(int k = 0; k < ns; k++)
        {
            const uchar *p = data + 220 + k*32;
            if(p[4] == DA_HD_EXT_CN) continue;
            hdsamp &s = hd.s.add();
            memcpy(&s.t, p, 4);
            lilswap(&s.t, 1);
            s.cn = p[4];
            s.flags = p[5];
            float f[6];
            memcpy(f, p+8, 24);
            lilswap(f, 6);
            s.yaw = f[0]; s.pitch = f[1]; s.x = f[3]; s.y = f[4]; s.z = f[5];
        }
        hd.ok = true;
        return true;
    }

    // ------------------------------------------------------------ small maps
    // int key -> int value, sorted keys (players, keys cn + 1000 x k)
    struct imap
    {
        vector<int> k, v;
        int find(int key) const
        {
            int lo = 0, hi = k.length();
            while(lo < hi) { int m = (lo+hi)>>1; if(k[m] < key) lo = m+1; else hi = m; }
            return lo < k.length() && k[lo] == key ? lo : -1;
        }
        bool has(int key) const { return find(key) >= 0; }
        int get(int key, int def) const { int i = find(key); return i >= 0 ? v[i] : def; }
        void set(int key, int val)
        {
            int lo = 0, hi = k.length();
            while(lo < hi) { int m = (lo+hi)>>1; if(k[m] < key) lo = m+1; else hi = m; }
            if(lo < k.length() && k[lo] == key) { v[lo] = val; return; }
            k.insert(lo, key);
            v.insert(lo, val);
        }
        void setdefault(int key, int val) { if(!has(key)) set(key, val); }
        int length() const { return k.length(); }
        void clear() { k.setsize(0); v.setsize(0); }
    };

    // ------------------------------------------------------------ one game
    // lire_dmo(path, partie) of the studies: the events and positions of one
    // game of the file, and the names, teams and bots as the reader kept them
    struct seggame
    {
        int seg, last_t, map, mode;
        bool hasmap;
        vector<int> ev, pos;
        imap names, teams, ai, aiowner;
        seggame() : seg(0), last_t(0), map(-1), mode(-1), hasmap(false) {}
    };

    static void buildseg(const rawdemo &rd, int k, seggame &g)
    {
        g.seg = k;
        g.last_t = k >= 0 && k < rd.seglast.length() ? rd.seglast[k] : 0;
        loopv(rd.ev) if(rd.ev[i].seg == k)
        {
            const devent &e = rd.ev[i];
            g.ev.add(i);
            switch(e.type)
            {
                case E_INITCLIENT: g.names.set(e.cn, e.s1); g.teams.set(e.cn, e.s2); break;
                case E_SWITCHNAME: g.names.set(e.cn, e.s1); break;
                case E_SETTEAM: g.teams.set(e.cn, e.s2); break;
                case E_INITAI: g.names.set(e.cn, e.s1); g.teams.set(e.cn, e.s2); g.ai.set(e.cn, e.a); g.aiowner.set(e.cn, e.b); break;
                case E_MAPCHANGE: g.map = e.s1; g.mode = e.a; g.hasmap = true; break;
            }
        }
        loopv(rd.pos) if(rd.pos[i].seg == k) g.pos.add(i);
    }

    static const char * const modenames[] = { "ffa", "coop edit", "teamplay", "instagib", "insta team", "efficiency",
        "effic team", "tactics", "tac team", "capture", "regen capture", "ctf", "insta ctf", "protect",
        "insta protect", "hold", "insta hold", "effic ctf", "effic protect", "effic hold", "collect",
        "insta collect", "effic collect" };
    static const char *modename(int mode) { return mode >= 0 && mode < int(sizeof(modenames)/sizeof(modenames[0])) ? modenames[mode] : "?"; }
    static bool isinsta(int mode) { return mode == 3 || mode == 4 || mode == 12 || mode == 14 || mode == 16 || mode == 21; }
    static bool isteammode(int mode) { return mode == 2 || mode == 4 || mode == 6 || (mode >= 8 && mode <= 22); }

    // a game of the file: t_fin (next map), events up to it, map and mode, the
    // previous game for names and bots (charger() of the studies)
    struct gamebase
    {
        const rawdemo *rd;
        int partie, nparties, t_fin, map, mode;
        seggame d;
        vector<int> ev;        // events (indices) with t <= t_fin
        vector<int> cdiscn;    // cn having N_CDIS
        vector<dlist> cdist;   // their times
        imap cdisidx;
        // names / teams / bots after replaying the keyed events
        imap names, teams, ai, aiowner;
        imap rawnames;         // d.names (with the previous game) for the fallback
        gamebase() : rd(NULL), partie(0), nparties(1), t_fin(0), map(-1), mode(-1) {}

        // a cn taken again by another client after a disconnection (N_CDIS) is
        // another player: key cn + 1000 x k
        int cle(int cn, double t) const
        {
            if(cn < 0) return cn;
            int i = cdisidx.get(cn, -1);
            if(i < 0) return cn;
            return cn + 1000*ssleft(cdist[i], t);
        }
    };

    static void buildgame(const rawdemo &rd, int partie, gamebase &g)
    {
        g.rd = &rd;
        g.partie = partie;
        g.nparties = rd.nsegs;
        buildseg(rd, partie, g.d);
        vector<int> mc;
        loopv(g.d.ev) if(rd.ev[g.d.ev[i]].type == E_MAPCHANGE) mc.add(rd.ev[g.d.ev[i]].t);
        g.t_fin = g.d.last_t;
        loopv(mc) if(mc[i] > mc[0] + 1000) { g.t_fin = mc[i]; break; }
        loopv(g.d.ev) if(rd.ev[g.d.ev[i]].t <= g.t_fin) g.ev.add(g.d.ev[i]);
        g.map = -1; g.mode = -1;
        bool found = false;
        loopv(g.d.ev) if(rd.ev[g.d.ev[i]].type == E_MAPCHANGE) { g.map = rd.ev[g.d.ev[i]].s1; g.mode = rd.ev[g.d.ev[i]].a; found = true; break; }
        g.rawnames = g.d.names;
        imap rawteams = g.d.teams;
        if(partie > 0)
        {
            seggame tout;
            buildseg(rd, partie-1, tout);
            if(!found && tout.hasmap) { g.map = tout.map; g.mode = tout.mode; found = true; }
            loopv(tout.names.k) g.rawnames.setdefault(tout.names.k[i], tout.names.v[i]);
        }
        // N_CDIS times (events up to t_fin, in order)
        loopv(g.ev)
        {
            const devent &e = rd.ev[g.ev[i]];
            if(e.type != E_CDIS) continue;
            int idx = g.cdisidx.get(e.cn, -1);
            if(idx < 0) { idx = g.cdiscn.length(); g.cdiscn.add(e.cn); g.cdist.add(); g.cdisidx.set(e.cn, idx); }
            g.cdist[idx].add(e.t);
        }
        // names, teams, bots of the keyed events, then the reader's names
        loopv(g.ev)
        {
            const devent &e = rd.ev[g.ev[i]];
            int c = g.cle(e.cn, e.t);
            switch(e.type)
            {
                case E_INITCLIENT: g.names.set(c, e.s1); g.teams.set(c, e.s2); break;
                case E_SWITCHNAME: g.names.set(c, e.s1); break;
                case E_SETTEAM: g.teams.set(c, e.s2); break;
                case E_INITAI: g.names.set(c, e.s1); g.teams.set(c, e.s2); g.ai.set(c, e.a); g.aiowner.set(c, g.cle(e.b, e.t)); break;
            }
        }
        loopv(g.rawnames.k) g.names.setdefault(g.rawnames.k[i], g.rawnames.v[i]);
    }

    // keys of the events: cn, and the other player fields (victim, actor, owner)
    static inline int evkey(const gamebase &g, const devent &e) { return g.cle(e.cn, e.t); }
    static inline int evkeya(const gamebase &g, const devent &e) { return g.cle(e.a, e.t); }

    // the entities of the map that push a player, and the flags of the teams (map file, read by the game)
    struct mapents
    {
        bool ok;
        vector<dv3> tele, dest, pad;
        vector<dv3> flag;            // flags and bases of the two teams (player stats)
        vector<int> flagteam;        // 1 good, 2 evil
        mapents() : ok(false) {}
    };

    static bool pres(const dv3 &p, const vector<dv3> &ents, double r)
    {
        loopv(ents) { dv3 d = p - ents[i]; if(d.norm() < r) return true; }
        return false;
    }

    // ------------------------------------------------------------ group 1
    // hitbox (where the shooter's game stopped a hit), shot clock, speed and
    // teleports: demoanalyse.py of the first study
    static const int DELAI[7] = { 250, 1400, 100, 800, 1500, 600, 500 };      // attackdelay (game.h)
    static const double RECUL[7] = { 0, 50, 17.5, 25, 75, 25, 17.5 };        // kickamount x 2.5 (weapon.cpp)
    static inline bool untrait(int g) { return g == 2 || g == 4 || g == 6; }  // chaingun, rifle, pistol
    static const double R_CORPS = 4.1, YEUX = 14.0, V_SOL = 130.0, V_AIR = 169.0;

    enum { M_HITBOX = 0, M_CLOCK, M_EARLY, M_SPEED, M_MOVE, M_JUMP, M_SNAP, M_TRIGGER, M_HIDDENAIM, M_HIDDENSHOT, M_WAIT,
           M_WALL, M_LAG, M_AIR, M_JUMPF, M_NOCLIP, M_FOLLOW, NUMMOMENTS };

    struct moment
    {
        double t;
        int key, type, other;      // other: victim / enemy key (-1)
        double v1, v2, v3;         // values for the text, by type
    };

    struct g1shot { double t; int gun, id; double fr[3], to[3]; };

    struct g1player
    {
        int key, nshots, npos, frags, deaths;
        bool bot, recorder, edit;
        double ping;               // NAN = none
        // hitbox
        int hb_touches, hb_hors;
        double hb_part, hb_med, hb_medfav, hb_q90fav;
        // clock
        int ca_paires, ca_early;
        double ca_worst, ca_avance, ca_retard, ca_slope, ca_q10, ca_q90;
        // speed
        bool vi_ok;
        int vi_n, vi_hors, vi_trop_n, vi_trop_ep, vi_fen_dep, vi_sauts, vi_fen_rap;
        double vi_vmaxsol, vi_vmaxair, vi_q999sol, vi_trop_ms, vi_depmax, vi_depq999, vi_deprapide_ms, vi_rapmed, vi_rapq10, vi_rapq90, vi_pasmed;
    };

    struct g1params
    {
        double tol_corps, marge_v, excuse_ms, sol_ms, fen_r_ms, min_r_u, saut_u, v_dep_max, avance_ms;
        int fen;
        g1params() : tol_corps(1.0), marge_v(8.0), excuse_ms(3000), sol_ms(300), fen_r_ms(2000), min_r_u(150.0), saut_u(64.0),
                     v_dep_max(220.0), avance_ms(1000), fen(50) {}
    };

    struct g1result
    {
        int rec, t_fin;
        bool hdok, hdpresent, entsok;
        double rtt_rec, ping_rec, recalS, recalR;
        int hdchoice, hdsegs, nhd;
        double hdbest;
        vector<g1player> players;
        vector<moment> moments;
        g1result() : rec(-1), t_fin(0), hdok(false), hdpresent(false), entsok(false), rtt_rec(DNAN), ping_rec(0), recalS(0), recalR(20),
                     hdchoice(-1), hdsegs(0), nhd(0), hdbest(DNAN) {}
    };

    // sorted times of each key
    struct timelists
    {
        imap idx;
        vector<dlist> l;
        dlist &get(int key)
        {
            int i = idx.get(key, -1);
            if(i < 0) { i = l.length(); l.add(); idx.set(key, i); }
            return l[i];
        }
        const dlist *find(int key) const { int i = idx.get(key, -1); return i >= 0 ? &l[i] : NULL; }
        void sortall() { loopv(l) l[i].sort(dless); }
    };

    // an event in ]a, b] ?
    static bool entre(const timelists &tl, int key, double a, double b)
    {
        const dlist *x = tl.find(key);
        if(!x || x->empty()) return false;
        int i = ssright(*x, a);
        return i < x->length() && (*x)[i] <= b;
    }

    // positions of a key, sorted by time (stable)
    struct g1pos
    {
        int key;
        dlist t;
        vector<dv3> o, v, f;
        vector<int> ph, sq;
    };

    // track of a player for the hitbox: eye positions sorted by time
    struct g1piste
    {
        dlist t, x, y, z;
        void xy(double tq, double &X, double &Y) const { X = npinterp(tq, t.getbuf(), x.getbuf(), t.length()); Y = npinterp(tq, t.getbuf(), y.getbuf(), t.length()); }
        double trou(double tq) const { int i = ssright(t, tq); if(i <= 0) return 1e9; return tq - t[i-1]; }
    };

    static void makepiste(const double *t, const double *x, const double *y, const double *z, int n, g1piste &p)
    {
        vector<int> o;
        stableargsort(t, n, o);
        loopv(o) { p.t.add(t[o[i]]); p.x.add(x[o[i]]); p.y.add(y[o[i]]); p.z.add(z[o[i]]); }
    }

    struct g1touch
    {
        double t, base, kC, D, h0, hmin;
        int tireur, cible, gun;
        bool groupeS, hors;
        double h[161];
    };

    // the piece of the .dmohd that goes with this game (samples whose clock
    // follows the recorder's positions of the game); first study: only the
    // recorder's own track is compared
    static void hdsegments(const hddata &hd, vector<int> &seg, int &nseg)
    {
        seg.setsize(0);
        int s = 0;
        loopv(hd.s)
        {
            if(i > 0 && hd.s[i].t < hd.s[i-1].t - 1000) s++;
            seg.add(s);
        }
        nseg = hd.s.length() ? s+1 : 0;
    }

    static double hdmatch(const hddata &hd, const vector<int> &seg, int k, int cn, const dlist &pt, const dlist &px, const dlist &py, double ptmin, double ptmax)
    {
        dlist e;
        loopv(hd.s)
        {
            const hdsamp &s = hd.s[i];
            if(seg[i] != k || s.cn != cn || s.t < ptmin || s.t > ptmax) continue;
            double X = npinterp(s.t, pt.getbuf(), px.getbuf(), pt.length()) - double(s.x);
            double Y = npinterp(s.t, pt.getbuf(), py.getbuf(), pt.length()) - double(s.y);
            e.add(hypot(X, Y));
        }
        if(e.length() < 50) return DNAN;
        return npmedian(e);
    }

    static void g1analyse(const rawdemo &rd, const hddata *hd, int partie, const mapents &ents, const g1params &P, g1result &out)
    {
        gamebase g;
        buildgame(rd, partie, g);
        out.t_fin = g.t_fin;
        double t_fin = g.t_fin;
        // ---- .dmohd piece of this game
        vector<int> hdsel;          // indices of the HD samples kept
        bool hdkept = false;
        out.hdpresent = hd && hd->ok;
        if(hd && hd->ok && hd->version == 1)
        {
            vector<int> seg; int nseg;
            hdsegments(*hd, seg, nseg);
            out.hdsegs = nseg;
            int rc = hd->rec_cn, choix = -1;
            double best = 1e9;
            dlist pt, px, py;
            loopv(g.d.pos) { const dposx &p = rd.pos[g.d.pos[i]]; if(p.cn == rc) { pt.add(p.t); px.add(p.o[0]); py.add(p.o[1]); } }
            if(pt.length() && hd->s.length())
            {
                double ptmin = pt[0], ptmax = pt[0];
                loopv(pt) { ptmin = dmin(ptmin, pt[i]); ptmax = dmax(ptmax, pt[i]); }
                loopk(nseg)
                {
                    double e = hdmatch(*hd, seg, k, rc, pt, px, py, ptmin, ptmax);
                    if(!isnand(e) && e < best) { best = e; choix = k; }
                }
            }
            out.hdbest = best;
            if(choix >= 0 && best <= 16)
            {
                hdkept = true;
                out.hdchoice = choix;
                loopv(hd->s) if(seg[i] == choix) hdsel.add(i);
            }
        }
        double t0_partie = 0;
        if(g.ev.length()) { t0_partie = rd.ev[g.ev[0]].t; loopv(g.ev) t0_partie = dmin(t0_partie, rd.ev[g.ev[i]].t); }
        // ---- positions (keyed), t <= t_fin, at least 2
        imap posidx;
        vector<g1pos> pos;
        {
            imap tmpidx;
            vector<vector<int> > lists;
            loopv(g.d.pos)
            {
                const dposx &p = rd.pos[g.d.pos[i]];
                int key = g.cle(p.cn, p.t);
                int li = tmpidx.get(key, -1);
                if(li < 0) { li = lists.length(); lists.add(); tmpidx.set(key, li); }
                lists[li].add(g.d.pos[i]);
            }
            loopv(tmpidx.k)
            {
                vector<int> &L = lists[tmpidx.v[i]];
                vector<int> keep;
                loopvj(L) if(rd.pos[L[j]].t <= t_fin) keep.add(L[j]);
                if(keep.length() < 2) continue;
                dlist t;
                loopvj(keep) t.add(rd.pos[keep[j]].t);
                vector<int> o;
                stableargsort(t.getbuf(), t.length(), o);
                g1pos &gp = pos.add();
                gp.key = tmpidx.k[i];
                loopvj(o)
                {
                    const dposx &p = rd.pos[keep[o[j]]];
                    gp.t.add(p.t);
                    gp.o.add(dv3(p.o[0], p.o[1], p.o[2]));
                    gp.v.add(dv3(p.vel[0], p.vel[1], p.vel[2]));
                    gp.f.add(dv3(p.fvec[0], p.fvec[1], p.fvec[2]));
                    gp.ph.add(p.phys);
                    gp.sq.add(p.seq);
                }
                posidx.set(gp.key, pos.length()-1);
            }
        }
        // ---- recorder (local demo) or server demo
        int rec = -1;
        if(hdkept && hd->rec_cn >= 0) rec = hd->rec_cn;
        else
        {
            imap egaln, egalk;
            loopv(g.ev)
            {
                const devent &e = rd.ev[g.ev[i]];
                if(e.type != E_SHOT) continue;
                int c = evkey(g, e);
                egaln.set(c, egaln.get(c, 0) + 1);
                egalk.set(c, egalk.get(c, 0) + (e.t == e.b ? 1 : 0));
            }
            vector<int> exacts, rapides;
            loopv(egaln.k)
            {
                int c = egaln.k[i], n = egaln.v[i], k = egalk.get(c, 0);
                if(n >= 3 && double(k)/n >= 0.9 && !g.ai.has(c)) exacts.add(c);
            }
            loopv(pos)
            {
                if(pos[i].t.length() <= 200) continue;
                dlist d;
                for(int j = 1; j < pos[i].t.length(); j++) d.add(pos[i].t[j] - pos[i].t[j-1]);
                if(npmedian(d) < 20 && !g.ai.has(pos[i].key)) rapides.add(pos[i].key);
            }
            if(exacts.length() == 1) rec = exacts[0];
            else if(rapides.length() == 1) rec = rapides[0];
        }
        out.rec = rec;
        // HD samples keyed by N_CDIS like the positions
        vector<int> hdkey;
        loopv(hdsel)
        {
            const hdsamp &s = hd->s[hdsel[i]];
            int c = s.cn, idx = g.cdisidx.get(c, -1);
            hdkey.add(idx >= 0 ? c + 1000*ssleft(g.cdist[idx], s.t) : c);
        }
        out.nhd = hdsel.length();
        bool hd_ok = hdkept && hdsel.length() > 0 && rec >= 0;
        out.hdok = hd_ok;
        out.entsok = ents.ok;
        #define LOCAL(c) ((c) == rec || (g.aiowner.has(c) && g.aiowner.get(c, -999) == rec && rec >= 0))
        // ---- events by player
        imap shotidx;
        vector<vector<g1shot> > tirs;
        timelists morts, vies, pouss, tele;
        struct ping_t { double t, p; };
        imap pingidx;
        vector<vector<ping_t> > pings;
        imap frags, deces, edit;
        struct degat_t { double t; int target, actor; g1shot s; };
        vector<degat_t> degats;
        struct degatl_t { double t; int target, actor; };
        vector<degatl_t> degats_locaux;
        double t_inter = t_fin + 1;
        imap lastshot; vector<g1shot> lastshots; // dernier_tir
        loopv(g.ev)
        {
            const devent &e = rd.ev[g.ev[i]];
            double t = e.t;
            int c = evkey(g, e);
            switch(e.type)
            {
                case E_SHOT:
                {
                    g1shot s; s.t = t; s.gun = e.a; s.id = e.b;
                    loopk(3) { s.fr[k] = e.fr[k]; s.to[k] = e.to[k]; }
                    int si = shotidx.get(c, -1);
                    if(si < 0) { si = tirs.length(); tirs.add(); shotidx.set(c, si); }
                    tirs[si].add(s);
                    int li = lastshot.get(c, -1);
                    if(li < 0) { li = lastshots.length(); lastshots.add(); }
                    lastshots[li] = s;
                    lastshot.set(c, li);
                    if(e.a >= 0 && e.a < 7 && RECUL[e.a] > 0) pouss.get(c).add(t);
                    break;
                }
                case E_EXPLODE: { int li = lastshot.get(c, -1); if(li >= 0) lastshots[li].t = -1e30; break; }
                case E_DIED:
                {
                    int a = evkeya(g, e);
                    morts.get(c).add(t);
                    deces.set(c, deces.get(c, 0) + 1);
                    if(a != c) frags.set(a, frags.get(a, 0) + 1);
                    break;
                }
                case E_FORCEDEATH: morts.get(c).add(t); break;
                case E_SPAWN: case E_SPAWNSTATE: vies.get(c).add(t); pouss.get(c).add(t); break;
                case E_TELEPORT: case E_JUMPPAD: tele.get(c).add(t); pouss.get(c).add(t); break;
                case E_DAMAGE:
                {
                    int a = evkeya(g, e);
                    pouss.get(c).add(t);
                    if(a != c && a >= 0)
                    {
                        if(LOCAL(a)) { degatl_t &d = degats_locaux.add(); d.t = t; d.target = c; d.actor = a; }
                        else
                        {
                            int li = lastshot.get(a, -1);
                            if(li >= 0 && lastshots[li].t > -1e29 && t - lastshots[li].t <= 5)
                            {
                                degat_t &d = degats.add(); d.t = t; d.target = c; d.actor = a; d.s = lastshots[li];
                            }
                        }
                    }
                    break;
                }
                case E_PING:
                {
                    int pi = pingidx.get(c, -1);
                    if(pi < 0) { pi = pings.length(); pings.add(); pingidx.set(c, pi); }
                    ping_t &p = pings[pi].add(); p.t = t; p.p = e.a;
                    break;
                }
                case E_EDITMODE: if(e.a) edit.set(c, 1); break;
                case E_TIMEUP: if(e.a <= 0 && t > 1000) t_inter = dmin(t_inter, t); break;
            }
        }
        morts.sortall(); vies.sortall(); pouss.sortall(); tele.sortall();
        // ping of a player near t (median of +-5 s), of the owner for a bot
        auto pingat = [&](int c, double t) -> double
        {
            if(g.aiowner.has(c)) c = g.aiowner.get(c, c);
            int pi = pingidx.get(c, -1);
            if(pi < 0 || pings[pi].empty()) return 0.0;
            dlist close, all;
            loopvj(pings[pi]) { all.add(pings[pi][j].p); if(pings[pi][j].t >= t - 5000 && pings[pi][j].t <= t + 5000) close.add(pings[pi][j].p); }
            return close.length() ? npmedian(close) : npmedian(all);
        };
        // ---- players
        vector<int> joueurs;
        loopv(pos) joueurs.add(pos[i].key);
        loopv(shotidx.k) if(!posidx.has(shotidx.k[i])) joueurs.add(shotidx.k[i]);
        joueurs.sort();
        imap pidx;
        loopv(joueurs)
        {
            int c = joueurs[i];
            g1player &r = out.players.add();
            memset(&r, 0, sizeof(r));
            r.key = c;
            r.bot = g.ai.has(c);
            int pc = g.aiowner.has(c) ? g.aiowner.get(c, c) : c;
            int pi = pingidx.get(pc, -1);
            if(pi >= 0) { dlist pl; loopvj(pings[pi]) pl.add(pings[pi][j].p); r.ping = roundeven(npmedian(pl)); }
            else r.ping = DNAN;
            r.frags = frags.get(c, 0);
            r.deaths = deces.get(c, 0);
            int si = shotidx.get(c, -1);
            r.nshots = si >= 0 ? tirs[si].length() : 0;
            int pp = posidx.get(c, -1);
            r.npos = pp >= 0 ? pos[pp].t.length() : 0;
            r.recorder = c == rec;
            r.edit = edit.has(c);
            pidx.set(c, out.players.length()-1);
        }
        // ================= 2. CLOCK
        loopv(joueurs)
        {
            int c = joueurs[i];
            g1player &r = out.players[i];
            int si = shotidx.get(c, -1);
            vector<g1shot> none;
            const vector<g1shot> &s = si >= 0 ? tirs[si] : none;
            int paires = 0;
            struct viol_t { double t; int gun, did; };
            vector<viol_t> viol;
            for(int k = 0; k+1 < s.length(); k++)
            {
                const g1shot &a = s[k], &b = s[k+1];
                if(entre(morts, c, a.t - 1, b.t) || entre(vies, c, a.t, b.t)) continue;
                if(!(a.gun >= 0 && a.gun < 7)) continue;
                int did = b.id - a.id;
                if(did < 0 || did > 60000) continue;
                paires++;
                if(did < DELAI[a.gun]) { viol_t &v = viol.add(); v.t = b.t; v.gun = a.gun; v.did = did; }
            }
            r.ca_paires = paires;
            r.ca_early = viol.length();
            r.ca_worst = DNAN;
            loopvj(viol) { double w = viol[j].did - DELAI[viol[j].gun]; if(isnand(r.ca_worst) || w < r.ca_worst) r.ca_worst = w; }
            r.ca_avance = r.ca_retard = r.ca_slope = r.ca_q10 = r.ca_q90 = DNAN;
            if(s.length() >= 3)
            {
                int n = s.length();
                dlist t, idd, o;
                loopvj(s) { t.add(s[j].t); idd.add(s[j].id); o.add(s[j].t - s[j].id); }
                double base = o[0];
                for(int j = 1; j < min(5, n); j++) base = dmin(base, o[j]);
                double avmax = 0, remax = 0;
                int kfirst = -1;
                loopj(n)
                {
                    double av = dmax(base - o[j], 0), re = dmax(o[j] - base, 0);
                    avmax = dmax(avmax, av); remax = dmax(remax, re);
                    if(kfirst < 0 && av >= P.avance_ms) kfirst = j;
                }
                r.ca_avance = roundeven(avmax);
                r.ca_retard = roundeven(remax);
                if(avmax >= P.avance_ms && kfirst >= 0)
                {
                    moment &m = out.moments.add();
                    m.t = t[kfirst]; m.key = c; m.type = M_CLOCK; m.other = -1;
                    m.v1 = dmax(base - o[kfirst], 0); m.v2 = avmax; m.v3 = 0;
                }
                if(n >= 15 && s[n-1].t - s[0].t >= 60000)
                {
                    dlist ts, is;
                    if(n > 400)
                    {
                        double step = (n-1)/399.0;
                        loopj(400) { int q = j == 399 ? n-1 : int(j*step); ts.add(t[q]); is.add(idd[q]); }
                    }
                    else { ts = t; is = idd; }
                    dlist pentes;
                    loopj(ts.length()) loopk(ts.length())
                    {
                        double dt = ts[k] - ts[j];
                        if(dt >= 20000) pentes.add((is[k] - is[j])/dt);
                    }
                    if(pentes.length() >= 20)
                    {
                        r.ca_slope = npmedian(pentes);
                        r.ca_q10 = nppercentile(pentes, 10);
                        r.ca_q90 = nppercentile(pentes, 90);
                    }
                }
            }
            loopvj(viol)
            {
                moment &m = out.moments.add();
                m.t = viol[j].t; m.key = c; m.type = M_EARLY; m.other = -1;
                m.v1 = viol[j].did; m.v2 = viol[j].gun; m.v3 = DELAI[viol[j].gun];
            }
        }
        // ================= 3. SPEED / TELEPORT
        bool coopedit = g.mode == 1;
        loopv(joueurs)
        {
            int c = joueurs[i];
            g1player &r = out.players[i];
            int pp = posidx.get(c, -1);
            r.vi_ok = false;
            r.vi_vmaxsol = r.vi_vmaxair = r.vi_q999sol = r.vi_depmax = r.vi_depq999 = r.vi_rapmed = r.vi_rapq10 = r.vi_rapq90 = r.vi_pasmed = DNAN;
            if(pp < 0 || coopedit || edit.has(c)) continue;
            r.vi_ok = true;
            const g1pos &p = pos[pp];
            int n = p.t.length();
            const double *t = p.t.getbuf();
            vector<char> actif, sol, air_recent, excuse, trop;
            dlist vh, debut_sol;
            loopj(n)
            {
                actif.add(t[j] >= t0_partie + 1000 && t[j] < t_inter);
                vh.add(hypot(p.v[j].x, p.v[j].y));
                sol.add(p.ph[j] == 3 || p.ph[j] == 4 || p.ph[j] == 5 || p.ph[j] == 6);
            }
            double g0 = DNAN;
            loopj(n)
            {
                if(sol[j])
                {
                    if(isnand(g0) || (j > 0 && t[j] - t[j-1] > 500)) g0 = t[j];
                    debut_sol.add(g0);
                }
                else { g0 = DNAN; debut_sol.add(DINF); }
            }
            double dernier_air = -1e18;
            loopj(n) { if(!sol[j]) dernier_air = t[j]; air_recent.add((t[j] - dernier_air) <= P.sol_ms); }
            dlist pe;
            const dlist *pl = pouss.find(c);
            if(pl) pe = *pl;
            vector<char> pres_tp;
            loopj(n) pres_tp.add(0);
            if(ents.ok)
            {
                loopj(n)
                {
                    bool jp = pres(p.o[j], ents.pad, 16.0), tp = pres(p.o[j], ents.tele, 24.0);
                    pres_tp[j] = tp;
                    if(jp || tp) pe.add(t[j]);
                }
                pe.sort(dless);
            }
            loopj(n)
            {
                int ix = ssright(pe, t[j]) - 1;
                double tE = ix >= 0 ? pe[ix] : -1e18;
                bool ex = (t[j] - tE) <= P.excuse_ms && !(sol[j] && (t[j] - dmax(debut_sol[j], tE)) >= P.sol_ms);
                excuse.add(ex);
                double borne = (sol[j] && !air_recent[j] ? V_SOL : V_AIR) + P.marge_v;
                trop.add(vh[j] > borne && !ex && actif[j]);
            }
            struct ep_t { double a, b, v; bool s; };
            vector<ep_t> ep;
            loopj(n) if(trop[j])
            {
                if(ep.length() && t[j] - ep.last().b <= 500) { ep.last().b = t[j]; ep.last().v = dmax(ep.last().v, vh[j]); }
                else { ep_t &e = ep.add(); e.a = t[j]; e.b = t[j]; e.v = vh[j]; e.s = sol[j]; }
            }
            int m1 = n-1;
            dlist dt, dxy, d3, pred, u3;
            vector<char> valide, arrivee;
            loopj(m1)
            {
                dt.add(t[j+1] - t[j]);
                dxy.add(hypot(p.o[j+1].x - p.o[j].x, p.o[j+1].y - p.o[j].y));
                d3.add((p.o[j+1] - p.o[j]).norm());
                dv3 ua = p.v[j] + p.f[j], ub = p.v[j+1] + p.f[j+1];
                double ux = 0.5*(ub.x + ua.x), uy = 0.5*(ub.y + ua.y);
                pred.add(hypot(ux, uy)*dt[j]/1000.0);
                u3.add(dmax(ub.norm(), ua.norm()));
                double a = t[j], b = t[j+1];
                valide.add(p.sq[j] == p.sq[j+1] && !entre(morts, c, a-1, b) && !entre(vies, c, a-300, b+300) && !entre(tele, c, a-300, b+300));
            }
            dlist permis;
            loopj(m1) permis.add((dmax(u3[j], 200.0) + 100.0)*(dt[j] + 100.0)/1000.0 + 32.0);
            loopj(m1)
            {
                bool arr = false;
                if(ents.ok) arr = pres(p.o[j+1], ents.dest, 40.0) || pres_tp[j] || pres_tp[j+1];
                arrivee.add(arr);
                valide[j] = valide[j] && actif[j+1] && actif[j];
            }
            vector<int> sauts;
            loopj(m1) if(valide[j] && !arrivee[j] && d3[j] > P.saut_u && d3[j] > permis[j]) sauts.add(j);
            dlist rr;
            {
                double accD = 0, accP = 0, accT = 0;
                loopj(m1)
                {
                    if(!valide[j] || dt[j] > 500 || dt[j] <= 0) { accD = accP = accT = 0; continue; }
                    accD += dxy[j]; accP += pred[j]; accT += dt[j];
                    if(accT >= P.fen_r_ms)
                    {
                        if(accP >= P.min_r_u) rr.add(accD/accP);
                        accD = accP = accT = 0;
                    }
                }
            }
            struct dep_t { double a, b, v; };
            vector<dep_t> rap_dep;
            {
                int i0 = -1;
                double accD = 0, accT = 0;
                loopj(m1)
                {
                    if(!valide[j] || dt[j] > 500 || dt[j] <= 0) { i0 = -1; accD = accT = 0; continue; }
                    if(i0 < 0) i0 = j;
                    accD += dxy[j]; accT += dt[j];
                    if(accT >= 500)
                    {
                        double ta = t[i0], tb = t[j+1];
                        int k = ssleft(pe, ta - 1500);
                        if(!(k < pe.length() && pe[k] <= tb)) { dep_t &d = rap_dep.add(); d.a = ta; d.b = tb; d.v = accD/accT*1000.0; }
                        i0 = -1; accD = accT = 0;
                    }
                }
            }
            dlist vdep;
            loopvj(rap_dep) vdep.add(rap_dep[j].v);
            vector<dep_t> dep_rapide;
            loopvj(rap_dep) if(rap_dep[j].v > P.v_dep_max) dep_rapide.add(rap_dep[j]);
            dlist tropv;
            loopj(n) if(trop[j]) tropv.add(dmin(j+1 < n ? t[j+1] - t[j] : 33.0, 100.0));
            double trop_ms = pwsum(tropv.getbuf(), tropv.length());
            dlist vsol, vair;
            int nhors = 0, ntrop = 0;
            loopj(n)
            {
                bool hors = !excuse[j] && actif[j];
                if(hors) nhors++;
                if(trop[j]) ntrop++;
                if(hors && sol[j] && !air_recent[j]) vsol.add(vh[j]);
                if(hors && !(sol[j] && !air_recent[j])) vair.add(vh[j]);
            }
            r.vi_n = n;
            r.vi_hors = nhors;
            if(vsol.length()) { double mx = vsol[0]; loopvj(vsol) mx = dmax(mx, vsol[j]); r.vi_vmaxsol = mx; }
            if(vair.length()) { double mx = vair[0]; loopvj(vair) mx = dmax(mx, vair[j]); r.vi_vmaxair = mx; }
            if(vsol.length() > 50) r.vi_q999sol = nppercentile(vsol, 99.9);
            r.vi_trop_n = ntrop;
            r.vi_trop_ep = ep.length();
            r.vi_trop_ms = roundeven(trop_ms);
            if(vdep.length()) { double mx = vdep[0]; loopvj(vdep) mx = dmax(mx, vdep[j]); r.vi_depmax = mx; }
            if(vdep.length() > 100) r.vi_depq999 = nppercentile(vdep, 99.9);
            double rapide = 0;
            loopvj(dep_rapide) rapide += dep_rapide[j].b - dep_rapide[j].a;
            r.vi_deprapide_ms = roundeven(rapide);
            r.vi_fen_dep = vdep.length();
            r.vi_sauts = sauts.length();
            r.vi_fen_rap = rr.length();
            if(rr.length()) r.vi_rapmed = npmedian(rr);
            if(rr.length() >= 5) { r.vi_rapq10 = nppercentile(rr, 10); r.vi_rapq90 = nppercentile(rr, 90); }
            r.vi_pasmed = npmedian(dt);
            loopvj(ep)
            {
                moment &m = out.moments.add();
                m.t = ep[j].a; m.key = c; m.type = M_SPEED; m.other = ep[j].s ? 1 : 0;
                m.v1 = ep[j].v; m.v2 = ep[j].s ? V_SOL : V_AIR; m.v3 = ep[j].b - ep[j].a;
            }
            vector<dep_t> epd;
            vector<double> epdur;
            loopvj(dep_rapide)
            {
                const dep_t &d = dep_rapide[j];
                if(epd.length() && d.a - epd.last().b <= 1000) { epd.last().b = d.b; epd.last().v = dmax(epd.last().v, d.v); epdur.last() += d.b - d.a; }
                else { epd.add(d); epdur.add(d.b - d.a); }
            }
            loopvj(epd)
            {
                moment &m = out.moments.add();
                m.t = epd[j].a; m.key = c; m.type = M_MOVE; m.other = -1;
                m.v1 = epd[j].v; m.v2 = epdur[j]; m.v3 = epd[j].b;
            }
            loopvj(sauts)
            {
                int q = sauts[j];
                moment &m = out.moments.add();
                m.t = t[q+1]; m.key = c; m.type = M_JUMP; m.other = -1;
                m.v1 = d3[q]; m.v2 = dt[q]; m.v3 = permis[q];
            }
        }
        // ================= 1. HITBOX
        imap pisteidx, pistedmoidx;
        vector<g1piste> pistes;
        loopv(pos)
        {
            const g1pos &p = pos[i];
            dlist x, y, z;
            loopvj(p.o) { x.add(p.o[j].x); y.add(p.o[j].y); z.add(p.o[j].z + YEUX); }
            g1piste &gp = pistes.add();
            makepiste(p.t.getbuf(), x.getbuf(), y.getbuf(), z.getbuf(), p.t.length(), gp);
            pistedmoidx.set(p.key, pistes.length()-1);
        }
        if(hd_ok)
        {
            imap keys;
            loopv(hdkey) keys.set(hdkey[i], keys.get(hdkey[i], 0) + 1);
            loopv(keys.k)
            {
                int c = keys.k[i];
                if(keys.v[i] <= 10) continue;
                dlist t, x, y, z;
                loopvj(hdsel) if(hdkey[j] == c) { const hdsamp &s = hd->s[hdsel[j]]; t.add(s.t); x.add(s.x); y.add(s.y); z.add(s.z); }
                g1piste &gp = pistes.add();
                makepiste(t.getbuf(), x.getbuf(), y.getbuf(), z.getbuf(), t.length(), gp);
                pisteidx.set(c, pistes.length()-1);
            }
        }
        loopv(pos) if(!pisteidx.has(pos[i].key)) pisteidx.set(pos[i].key, pistedmoidx.get(pos[i].key, -1));
        // round trip of the recorder: shot (alone) -> damage
        dlist delais;
        loopv(degats_locaux)
        {
            const degatl_t &d = degats_locaux[i];
            int si = shotidx.get(d.actor, -1);
            if(si < 0) continue;
            const vector<g1shot> &L = tirs[si];
            int found = -1, nfound = 0;
            loopvj(L) if(untrait(L[j].gun) && d.t - 600 <= L[j].t && L[j].t <= d.t) { found = j; nfound++; }
            if(nfound != 1) continue;
            bool avant = false;
            loopvj(L) if(L[found].t - 400 < L[j].t && L[j].t < L[found].t) { avant = true; break; }
            if(!avant) delais.add(d.t - L[found].t);
        }
        double rtt = delais.length() >= 5 ? npmedian(delais) : DNAN;
        double ping_rec = 0.0;
        if(rec >= 0)
        {
            if(pingidx.has(rec)) ping_rec = pingat(rec, t_fin/2);
            else if(!isnand(rtt)) ping_rec = rtt;
            int ri = pidx.get(rec, -1);
            if(ri >= 0) out.players[ri].ping = roundeven(ping_rec);
        }
        out.rtt_rec = rtt;
        out.ping_rec = ping_rec;
        double att = !isnand(rtt) ? rtt : ping_rec;
        loopv(degats_locaux)
        {
            const degatl_t &d = degats_locaux[i];
            int si = shotidx.get(d.actor, -1);
            if(si < 0) continue;
            const vector<g1shot> &L = tirs[si];
            int best = -1;
            double bd = 0;
            loopvj(L)
            {
                double dd = fabs(d.t - att - L[j].t);
                if(dd <= 80 && (best < 0 || dd < bd)) { best = j; bd = dd; }
            }
            if(best >= 0) { degat_t &dg = degats.add(); dg.t = d.t; dg.target = d.target; dg.actor = d.actor; dg.s = L[best]; }
        }
        double DS[161];
        loopi(161) DS[i] = -100 + 5*i;
        vector<g1touch *> touches;
        struct vu_t { int tireur; double t; int id; };
        vector<vu_t> vus;
        loopv(degats)
        {
            const degat_t &dg = degats[i];
            const g1shot &best = dg.s;
            int cible = dg.target, tireur = dg.actor;
            if(!untrait(best.gun) || !pisteidx.has(cible)) continue;
            bool seen = false;
            loopvj(vus) if(vus[j].tireur == tireur && vus[j].t == best.t && vus[j].id == best.id) { seen = true; break; }
            if(seen) continue;
            vu_t &v = vus.add(); v.tireur = tireur; v.t = best.t; v.id = best.id;
            double ts = best.t;
            int pvi = pisteidx.get(cible, -1);
            int pdi = pistedmoidx.get(cible, -1);
            if(pvi != pdi && pdi >= 0)
            {
                const g1piste &pv = pistes[pvi];
                int k0 = ssleft(pv.t, ts - 800), k1 = ssleft(pv.t, ts + 150);
                bool bad = pv.t[0] > ts - 800 || pv.t[pv.t.length()-1] < ts + 150;
                if(!bad && k1 > k0)
                {
                    int a = max(k0-1, 0), b = min(k1+1, pv.t.length());
                    double mx = -1e30;
                    for(int j = a+1; j < b; j++) mx = dmax(mx, pv.t[j] - pv.t[j-1]);
                    if(b - a >= 2 && mx > 100) bad = true;
                }
                if(bad) pvi = pdi;
            }
            if(pistes[pvi].trou(ts) > 400) continue;
            g1touch *x = new g1touch;
            const dlist *vv = vies.find(cible);
            int jv = -1;
            if(vv && vv->length()) jv = ssright(*vv, ts) - 1;
            bool anyfinite = false;
            loopj(161)
            {
                double X, Y;
                pistes[pvi].xy(ts - DS[j], X, Y);
                double h = hypot(best.to[0] - X, best.to[1] - Y);
                if(jv >= 0 && ts - DS[j] < (*vv)[jv] + 30) h = DINF;
                x->h[j] = h;
                if(finited(h)) anyfinite = true;
            }
            if(!anyfinite) { delete x; continue; }
            int ot = g.aiowner.has(tireur) ? g.aiowner.get(tireur, tireur) : tireur;
            int oc = g.aiowner.has(cible) ? g.aiowner.get(cible, cible) : cible;
            if(ot == oc && !LOCAL(tireur)) { delete x; continue; }
            bool gS = LOCAL(tireur);
            double base = gS ? 0.0 : pingat(tireur, ts) + (LOCAL(cible) ? ping_rec : 0.0);
            if(pvi != pdi && pdi >= 0)
            {
                double tq = ts - (base + (gS ? 0 : 20));
                double ax, ay, bx, by;
                pistes[pvi].xy(tq, ax, ay);
                pistes[pdi].xy(tq, bx, by);
                if(hypot(ax - bx, ay - by) > 24)
                {
                    pvi = pdi;
                    anyfinite = false;
                    loopj(161)
                    {
                        double X, Y;
                        pistes[pvi].xy(ts - DS[j], X, Y);
                        double h = hypot(best.to[0] - X, best.to[1] - Y);
                        if(vv && vv->length() && jv >= 0 && ts - DS[j] < (*vv)[jv] + 30) h = DINF;
                        x->h[j] = h;
                        if(finited(h)) anyfinite = true;
                    }
                    if(!anyfinite) { delete x; continue; }
                }
            }
            x->kC = (!gS && LOCAL(cible)) ? 2.0 : 1.0;
            x->t = ts; x->tireur = tireur; x->cible = cible; x->gun = best.gun; x->groupeS = gS; x->base = base;
            touches.add(x);
        }
        // network delay fitted per group, never on the measured shooter's own hits
        auto score = [&](const vector<g1touch *> &sel, double C, double &s1, double &s2)
        {
            dlist hh;
            loopvj(sel) hh.add(npinterp(sel[j]->base + sel[j]->kC*C, DS, sel[j]->h, 161));
            int k = 0;
            dlist dev;
            loopvj(hh) { if(hh[j] <= R_CORPS + P.tol_corps) k++; dev.add(fabs(hh[j] - R_CORPS)); }
            s1 = double(k)/hh.length();
            s2 = -npmedian(dev);
        };
        auto ajuste = [&](const vector<g1touch *> &sel, bool gS) -> double
        {
            if(sel.length() < 10) return gS ? 0 : 20;
            double bestC = -50, b1 = -1, b2 = 0;
            for(int C = -50; C <= 150; C += 5)
            {
                double s1, s2;
                score(sel, C, s1, s2);
                if(C == -50 || s1 > b1 || (s1 == b1 && s2 > b2)) { bestC = C; b1 = s1; b2 = s2; }
            }
            return bestC;
        };
        imap recalS, recalR;
        loopv(joueurs)
        {
            int c = joueurs[i];
            vector<g1touch *> sS, sR;
            loopvj(touches) if(touches[j]->tireur != c) (touches[j]->groupeS ? sS : sR).add(touches[j]);
            recalS.set(c, int(ajuste(sS, true)));
            recalR.set(c, int(ajuste(sR, false)));
        }
        {
            vector<g1touch *> sS, sR;
            loopvj(touches) (touches[j]->groupeS ? sS : sR).add(touches[j]);
            out.recalS = ajuste(sS, true);
            out.recalR = ajuste(sR, false);
        }
        imap paridx;
        vector<vector<g1touch *> > par;
        loopv(touches)
        {
            g1touch *x = touches[i];
            double C = x->groupeS ? recalS.get(x->tireur, 0) : recalR.get(x->tireur, 20);
            double D = x->base + x->kC*C;
            double h0 = npinterp(D, DS, x->h, 161);
            double hmin = DINF;
            bool any = false;
            loopj(161) if(DS[j] >= D - P.fen && DS[j] <= D + P.fen) { if(!any || x->h[j] < hmin) hmin = x->h[j]; any = true; }
            if(!any) hmin = h0;
            if(!finited(hmin)) continue;
            if(!finited(h0)) h0 = hmin;
            x->D = D; x->h0 = h0; x->hmin = hmin; x->hors = hmin > R_CORPS + P.tol_corps;
            int pi = paridx.get(x->tireur, -1);
            if(pi < 0) { pi = par.length(); par.add(); paridx.set(x->tireur, pi); }
            par[pi].add(x);
            if(x->hors)
            {
                moment &m = out.moments.add();
                m.t = x->t; m.key = x->tireur; m.type = M_HITBOX; m.other = x->cible;
                m.v1 = h0; m.v2 = hmin; m.v3 = x->gun;
            }
        }
        loopv(joueurs)
        {
            g1player &r = out.players[i];
            int pi = paridx.get(joueurs[i], -1);
            r.hb_touches = r.hb_hors = 0;
            r.hb_part = r.hb_med = r.hb_medfav = r.hb_q90fav = DNAN;
            if(pi < 0) continue;
            vector<g1touch *> &L = par[pi];
            dlist hm, h0;
            loopvj(L) { hm.add(L[j]->hmin); h0.add(L[j]->h0); if(L[j]->hors) r.hb_hors++; }
            r.hb_touches = L.length();
            if(L.length())
            {
                r.hb_part = double(r.hb_hors)/L.length();
                r.hb_med = npmedian(h0);
                r.hb_medfav = npmedian(hm);
                if(L.length() >= 5) r.hb_q90fav = nppercentile(hm, 90);
            }
        }
        touches.deletecontents();
        #undef LOCAL
    }

    // ------------------------------------------------------------ aim tracks
    // partie.py of the second study: aim tracks of each player (key)
    //  dmo: the demo's positions, aim rounded down to the degree (+0.5)
    //  hd: exact HD track (the player's own samples, or the recorder's bots)
    //  aff: what the recorder's screen showed of the others (local .dmohd)
    enum { NAT_DMO = 0, NAT_HD, NAT_HDBOT, NAT_AFF };
    static const double DESSUS = 1.0, RAYON = 4.1;
    static const double RAD2DEG = 180.0/DPI;
    static inline double degrees(double r) { return r*RAD2DEG; }   // math.degrees, np.degrees

    // round(x, n) of Python (half to even on the exact value) for n <= 3
    static double pyround(double x, int n)
    {
        if(!finited(x)) return x;
        if(n <= 0) return nearbyint(x);
        static const double p10[4] = { 1, 10, 100, 1000 };
        long double v = (long double)x*(long double)p10[n];
        long double r = nearbyintl(v);
        return double(r)/p10[n];
    }

    struct piste
    {
        dlist t, yaw, pitch;
        vector<dv3> eye, dir;
        double pas;
        int nature;
        piste() : pas(0), nature(NAT_DMO) {}
        int n() const { return t.length(); }
        dv3 eye_at(double tq) const { return dv3(interp1(tq, 0), interp1(tq, 1), interp1(tq, 2)); }
        double interp1(double tq, int k) const
        {
            // np.interp on one coordinate of the eye
            int m = t.length();
            if(m <= 0) return DNAN;
            const double *xp = t.getbuf();
            if(isnand(tq)) return tq;
            if(tq > xp[m-1]) return eye[m-1][k];
            if(tq < xp[0]) return eye[0][k];
            int j = ssright(xp, m, tq) - 1;
            if(j >= m-1) return eye[m-1][k];
            if(xp[j] == tq) return eye[j][k];
            double slope = (eye[j+1][k] - eye[j][k])/(xp[j+1] - xp[j]);
            double r = slope*(tq - xp[j]) + eye[j][k];
            if(isnand(r)) { r = slope*(tq - xp[j+1]) + eye[j+1][k]; if(isnand(r) && eye[j][k] == eye[j+1][k]) r = eye[j][k]; }
            return r;
        }
        void ypr_at(double tq, double &y, double &p) const
        {
            y = npinterp(tq, t.getbuf(), yaw.getbuf(), t.length());
            p = npinterp(tq, t.getbuf(), pitch.getbuf(), t.length());
        }
        dv3 dir_at(double tq) const { double y, p; ypr_at(tq, y, p); return dirvec(y, p); }
        double trou(double tq) const
        {
            int m = t.length();
            int i = ssright(t, tq);
            i = max(1, min(i, m-1));
            return t[i] - t[i-1];
        }
        bool exact() const { return nature == NAT_HD || nature == NAT_HDBOT; }
    };

    static void makepiste2(const double *t, const dv3 *eye, const double *yaw, const double *pitch, int n, int nature, piste &p)
    {
        vector<int> o;
        stableargsort(t, n, o);
        dlist ys;
        loopi(n)
        {
            bool keep = i == n-1 || t[o[i+1]] != t[o[i]];
            if(!keep) continue;
            p.t.add(t[o[i]]);
            p.eye.add(eye[o[i]]);
            ys.add(yaw[o[i]]);
            p.pitch.add(pitch[o[i]]);
        }
        int m = p.t.length();
        p.yaw.setsize(0);
        loopi(m) p.yaw.add(0);
        unwrapdeg(ys.getbuf(), m, p.yaw.getbuf());
        loopi(m) p.dir.add(dirvec(p.yaw[i], p.pitch[i]));
        dlist d;
        for(int i = 1; i < m; i++) d.add(p.t[i] - p.t[i-1]);
        p.pas = d.length() ? npmedian(d) : 0.0;
        p.nature = nature;
    }

    struct p2shot { double t; int gun, id; dv3 fr, to; };
    struct mortev { double t; int victim, actor; };
    struct degev { double t; int target, actor; };

    struct partie
    {
        gamebase g;
        bool insta, equipes;
        int rec, source;              // source: 0 server, 1 local (.dmohd), 2 server (.dmohd), 3 local (shot clock), 4 local (positions)
        double t_fin;
        imap teamidx;
        vector<dlist> teamt;
        vector<vector<int> > teamn;
        vector<piste> pistes;         // all tracks
        imap pdmo, phd, paff;         // key -> index in pistes
        imap tiridx;
        vector<vector<p2shot> > tirs;
        timelists morts, vies;
        vector<mortev> morts_ev;
        vector<degev> degats;
        struct ping_t { double t, p; };
        imap pingidx;
        vector<vector<ping_t> > pings;
        imap frags, deces;
        imap sautidx;
        vector<dlist> sauts;          // jumps and landings from the positions (third study)
        vector<int> joueurs;
        int hdchoice;
        double hdbest;

        partie() : insta(false), equipes(false), rec(-1), source(0), t_fin(0), hdchoice(-1), hdbest(DNAN) {}

        const piste *dmo(int k) const { int i = pdmo.get(k, -1); return i >= 0 ? &pistes[i] : NULL; }
        const piste *hd(int k) const { int i = phd.get(k, -1); return i >= 0 ? &pistes[i] : NULL; }
        const piste *aff(int k) const { int i = paff.get(k, -1); return i >= 0 ? &pistes[i] : NULL; }
        const vector<p2shot> *shots(int k) const { int i = tiridx.get(k, -1); return i >= 0 ? &tirs[i] : NULL; }

        double ping(int k) const
        {
            if(g.aiowner.has(k)) k = g.aiowner.get(k, k);
            int pi = pingidx.get(k, -1);
            if(pi < 0 || pings[pi].empty()) return 0.0;
            dlist all;
            loopv(pings[pi]) all.add(pings[pi][i].p);
            return npmedian(all);
        }
        double pingat(int k, double t) const
        {
            if(g.aiowner.has(k)) k = g.aiowner.get(k, k);
            int pi = pingidx.get(k, -1);
            if(pi < 0 || pings[pi].empty()) return 0.0;
            dlist all, close;
            loopv(pings[pi]) { all.add(pings[pi][i].p); if(pings[pi][i].t >= t - 5000 && pings[pi][i].t <= t + 5000) close.add(pings[pi][i].p); }
            return close.length() ? npmedian(close) : npmedian(all);
        }
        int team_at(int k, double t) const
        {
            int i = teamidx.get(k, -1);
            if(i < 0) return g.teams.get(k, -1);
            int j = ssright(teamt[i], t) - 1;
            return j >= 0 ? teamn[i][j] : -1;
        }
        bool meme_equipe(int a, int b, double t) const
        {
            if(!equipes) return false;
            int ta = team_at(a, t);
            if(ta < 0) return false;
            int tb = team_at(b, t);
            if(tb < 0) return false;
            return !strcmp(g.rd->str(ta), g.rd->str(tb));
        }
        // alive at tq: last spawn <= tq and no death between
        bool vivant(int k, double tq) const
        {
            const dlist *v = vies.find(k);
            const dlist *m = morts.find(k);
            double v0;
            const double *vb; int vn;
            if(!v || v->empty())
            {
                const piste *p = dmo(k);
                v0 = p ? p->t[0] : 0.0;
                vb = &v0; vn = 1;
            }
            else { vb = v->getbuf(); vn = v->length(); }
            int j = ssright(vb, vn, tq) - 1;
            if(j < 0) return false;
            double dv = vb[j];
            if(m && m->length())
            {
                int i = ssright(*m, dv);
                double nm = i < m->length() ? (*m)[i] : DINF;
                if(nm <= tq) return false;
            }
            return true;
        }
        const piste *visee(int k) const { const piste *p = hd(k); return p ? p : dmo(k); }
        // the target's track as the shooter saw it (before the shift D)
        const piste *corps(int cible, int tireur) const
        {
            const piste *h = hd(cible);
            if(h && h->nature == NAT_HD) return h;
            if(tireur == rec && aff(cible)) return aff(cible);
            if(h) return h;
            return dmo(cible);
        }
    };

    // hits: instagib = frag, else damage of a hitscan weapon <= 450 ms after the shot; key (shooter, shot index) -> target
    struct touchmap
    {
        imap idx;   // shooter*65536 + shot index -> target
        void build(const partie &P)
        {
            idx.clear();
            if(P.insta)
            {
                loopv(P.morts_ev)
                {
                    const mortev &m = P.morts_ev[i];
                    int a = m.actor, v = m.victim;
                    if(a == v || a < 0 || P.meme_equipe(a, v, m.t)) continue;
                    const vector<p2shot> *L = P.shots(a);
                    if(!L) continue;
                    int c = -1;
                    loopvj(*L) if(0 <= m.t - (*L)[j].t && m.t - (*L)[j].t <= 450) c = j;
                    if(c >= 0) idx.set(a*65536 + c, v);
                }
            }
            else
            {
                loopv(P.degats)
                {
                    const degev &d = P.degats[i];
                    int a = d.actor, v = d.target;
                    if(a == v || a < 0 || P.meme_equipe(a, v, d.t)) continue;
                    const vector<p2shot> *L = P.shots(a);
                    if(!L) continue;
                    int c = -1;
                    loopvj(*L)
                    {
                        const p2shot &s = (*L)[j];
                        if(0 <= d.t - s.t && d.t - s.t <= 450 && (s.gun == 1 || s.gun == 2 || s.gun == 4 || s.gun == 6)) c = j;
                    }
                    if(c >= 0) idx.set(a*65536 + c, v);
                }
            }
        }
        int get(int a, int i) const { return idx.get(a*65536 + i, -1); }
        bool has(int a, int i) const { return idx.has(a*65536 + i); }
    };

    static void buildpartie(const rawdemo &rd, const hddata *hd, int part, partie &P)
    {
        gamebase &g = P.g;
        buildgame(rd, part, g);
        P.t_fin = g.t_fin;
        P.insta = isinsta(g.mode);
        P.equipes = isteammode(g.mode);
        // team events (initclient, initai, setteam) per key
        loopv(g.ev)
        {
            const devent &e = rd.ev[g.ev[i]];
            if(e.type != E_INITCLIENT && e.type != E_INITAI && e.type != E_SETTEAM) continue;
            int c = evkey(g, e);
            int ti = P.teamidx.get(c, -1);
            if(ti < 0) { ti = P.teamt.length(); P.teamt.add(); P.teamn.add(); P.teamidx.set(c, ti); }
            P.teamt[ti].add(e.t);
            P.teamn[ti].add(e.s2);
        }
        // positions keyed, t <= t_fin
        imap posli;
        vector<vector<int> > posl;
        loopv(g.d.pos)
        {
            const dposx &p = rd.pos[g.d.pos[i]];
            if(p.t > g.t_fin) continue;
            int key = g.cle(p.cn, p.t);
            int li = posli.get(key, -1);
            if(li < 0) { li = posl.length(); posl.add(); posli.set(key, li); }
            posl[li].add(g.d.pos[i]);
        }
        loopv(posli.k)
        {
            const vector<int> &L = posl[posli.v[i]];
            int key = posli.k[i];
            // jumps and landings from the positions (everybody: the recorder of a local demo gets no N_SOUND of his own)
            {
                dlist t;
                loopvj(L) t.add(rd.pos[L[j]].t);
                vector<int> o;
                stableargsort(t.getbuf(), t.length(), o);
                dlist ev_;
                for(int j = 0; j+1 < o.length(); j++)
                {
                    const dposx &a = rd.pos[L[o[j]]], &b = rd.pos[L[o[j+1]]];
                    bool sa = a.phys >= 2 && a.phys <= 6, sb = b.phys >= 2 && b.phys <= 6;
                    if(sa && b.phys == 1 && b.vel[2] > 30) ev_.add(b.t);
                    else if(a.phys == 1 && sb) ev_.add(b.t);
                }
                P.sauts.add(ev_);
                P.sautidx.set(key, P.sauts.length()-1);
            }
            if(L.length() < 20) continue;
            dlist t, y, pt;
            vector<dv3> eye;
            loopvj(L)
            {
                const dposx &p = rd.pos[L[j]];
                t.add(p.t);
                eye.add(dv3(p.o[0], p.o[1], double(p.o[2]) + YEUX));
                y.add(p.yaw + 0.5);
                pt.add(p.pitch + 0.5);
            }
            piste &pp = P.pistes.add();
            makepiste2(t.getbuf(), eye.getbuf(), y.getbuf(), pt.getbuf(), t.length(), NAT_DMO, pp);
            P.pdmo.set(key, P.pistes.length()-1);
        }
        // events
        loopv(g.ev)
        {
            const devent &e = rd.ev[g.ev[i]];
            int c = evkey(g, e);
            double t = e.t;
            switch(e.type)
            {
                case E_SHOT:
                {
                    int si = P.tiridx.get(c, -1);
                    if(si < 0) { si = P.tirs.length(); P.tirs.add(); P.tiridx.set(c, si); }
                    p2shot &s = P.tirs[si].add();
                    s.t = t; s.gun = e.a; s.id = e.b;
                    s.fr = dv3(e.fr[0], e.fr[1], e.fr[2]);
                    s.to = dv3(e.to[0], e.to[1], e.to[2]);
                    break;
                }
                case E_DIED:
                {
                    int a = evkeya(g, e);
                    P.morts.get(c).add(t);
                    mortev &m = P.morts_ev.add(); m.t = t; m.victim = c; m.actor = a;
                    P.deces.set(c, P.deces.get(c, 0) + 1);
                    if(a != c) P.frags.set(a, P.frags.get(a, 0) + 1);
                    break;
                }
                case E_FORCEDEATH: P.morts.get(c).add(t); break;
                case E_SPAWN: case E_SPAWNSTATE: P.vies.get(c).add(t); break;
                case E_DAMAGE: { degev &d = P.degats.add(); d.t = t; d.target = c; d.actor = evkeya(g, e); break; }
                case E_PING:
                {
                    int pi = P.pingidx.get(c, -1);
                    if(pi < 0) { pi = P.pings.length(); P.pings.add(); P.pingidx.set(c, pi); }
                    partie::ping_t &p = P.pings[pi].add(); p.t = t; p.p = e.a;
                    break;
                }
            }
        }
        P.morts.sortall(); P.vies.sortall();
        // recorder and the HD piece of this game (best over the candidates)
        bool hdkept = false;
        vector<int> hdsel;
        if(hd && hd->ok && hd->version == 1)
        {
            vector<int> seg; int nseg;
            hdsegments(*hd, seg, nseg);
            int rc = hd->rec_cn, choix = -1;
            double best = 1e9;
            vector<int> cands;
            if(rc >= 0) cands.add(rc);
            else
            {
                imap cs;
                loopv(hd->s) cs.set(hd->s[i].cn, 1);
                loopv(cs.k) cands.add(cs.k[i]);
            }
            loopk(nseg) loopvj(cands)
            {
                int c = cands[j];
                dlist pt, px, py;
                loopv(g.d.pos) { const dposx &p = rd.pos[g.d.pos[i]]; if(p.cn == c) { pt.add(p.t); px.add(p.o[0]); py.add(p.o[1]); } }
                if(pt.empty()) continue;
                double ptmin = pt[0], ptmax = pt[0];
                loopv(pt) { ptmin = dmin(ptmin, pt[i]); ptmax = dmax(ptmax, pt[i]); }
                double e = hdmatch(*hd, seg, k, c, pt, px, py, ptmin, ptmax);
                if(!isnand(e) && e < best) { best = e; choix = k; }
            }
            P.hdbest = best;
            if(choix >= 0 && best <= 16)
            {
                hdkept = true;
                P.hdchoice = choix;
                loopv(hd->s) if(seg[i] == choix) hdsel.add(i);
            }
        }
        if(hdkept && hd->rec_cn >= 0) { P.rec = hd->rec_cn; P.source = 1; }
        else if(hdkept) { P.rec = -1; P.source = 2; }
        else
        {
            imap egaln, egalk;
            loopv(g.ev)
            {
                const devent &e = rd.ev[g.ev[i]];
                if(e.type != E_SHOT) continue;
                int c = evkey(g, e);
                egaln.set(c, egaln.get(c, 0) + 1);
                egalk.set(c, egalk.get(c, 0) + (e.t == e.b ? 1 : 0));
            }
            vector<int> exacts, rapides;
            loopv(egaln.k)
            {
                int c = egaln.k[i], n = egaln.v[i], k = egalk.get(c, 0);
                if(n >= 3 && double(k)/n >= 0.9 && !g.ai.has(c)) exacts.add(c);
            }
            loopv(P.pdmo.k)
            {
                const piste &p = P.pistes[P.pdmo.v[i]];
                if(p.n() > 200 && p.pas < 20 && !g.ai.has(P.pdmo.k[i])) rapides.add(P.pdmo.k[i]);
            }
            if(exacts.length() == 1) { P.rec = exacts[0]; P.source = 3; }
            else if(rapides.length() == 1) { P.rec = rapides[0]; P.source = 4; }
        }
        // HD tracks (keyed by N_CDIS)
        if(hdsel.length())
        {
            vector<int> keys;
            loopv(hdsel)
            {
                const hdsamp &s = hd->s[hdsel[i]];
                int c = s.cn, idx = g.cdisidx.get(c, -1);
                keys.add(idx >= 0 ? c + 1000*ssleft(g.cdist[idx], s.t) : c);
            }
            imap kc;
            loopv(keys) kc.set(keys[i], kc.get(keys[i], 0) + 1);
            loopv(kc.k)
            {
                int k = kc.k[i];
                if(kc.v[i] < 50) continue;
                dlist t, y, p;
                vector<dv3> eye;
                int own = 0, n = 0;
                loopvj(hdsel) if(keys[j] == k)
                {
                    const hdsamp &s = hd->s[hdsel[j]];
                    t.add(s.t); eye.add(dv3(s.x, s.y, s.z)); y.add(s.yaw); p.add(s.pitch);
                    if(s.flags&DA_HD_OWN) own++;
                    n++;
                }
                double ownf = double(own)/n;
                bool botlocal = g.ai.has(k) && P.rec >= 0 && g.aiowner.get(k, -999) == P.rec;
                piste &pp = P.pistes.add();
                if(ownf >= 0.9 || botlocal)
                {
                    makepiste2(t.getbuf(), eye.getbuf(), y.getbuf(), p.getbuf(), t.length(), ownf >= 0.9 ? NAT_HD : NAT_HDBOT, pp);
                    P.phd.set(k, P.pistes.length()-1);
                }
                else
                {
                    makepiste2(t.getbuf(), eye.getbuf(), y.getbuf(), p.getbuf(), t.length(), NAT_AFF, pp);
                    P.paff.set(k, P.pistes.length()-1);
                }
            }
        }
        loopv(P.pdmo.k) P.joueurs.add(P.pdmo.k[i]);
        loopv(P.tiridx.k) if(!P.pdmo.has(P.tiridx.k[i])) P.joueurs.add(P.tiridx.k[i]);
        P.joueurs.sort();
    }

    // ------------------------------------------------------------ group 2: snap and tracking
    // visee.py, groupe2.py and regles.py of the second study
    static inline bool vise(int g) { return g == 1 || g == 4; }   // shotgun, rifle: 'to' is on the exact aim line

    static bool rayon(const dv3 &fr, const dv3 &to, dv3 &u)
    {
        dv3 d = to - fr;
        double n = d.norm();
        if(n <= 1e-6) return false;
        u = d/n;
        return true;
    }

    static inline dv3 centre(const dv3 &e) { return dv3(e.x, e.y, e.z + (-YEUX*0.3)); }

    // angle to the closest of 5 points of the body (measure.py aim_error)
    static double erreur_corps(const dv3 &eye, const dv3 &d, const dv3 &te)
    {
        static const double dz[5] = { DESSUS, 0.0, -YEUX*0.45, -YEUX*0.7, -YEUX*0.95 };
        dv3 q0 = te - eye;
        double best = 999.0;
        loopi(5)
        {
            dv3 q = q0;
            q.z += dz[i];
            double n = q.norm();
            dv3 qn = q/(n > 1e-6 ? n : 1e-6);
            best = dmin(best, angledeg(qn, d));
        }
        return best;
    }

    // shift (ms) between the shot times and the aim track where the track fits the exact shot lines
    static double decalage_piste(const partie &P, const piste &pi, int tireur, double &ecart, int &nused)
    {
        ecart = DNAN;
        nused = 0;
        const vector<p2shot> *all = P.shots(tireur);
        vector<int> L;
        if(all) loopv(*all) if(vise((*all)[i].gun)) L.add(i);
        if(L.length() > 400)
        {
            vector<int> s;
            double step = (L.length()-1)/399.0;
            loopi(400) s.add(L[i == 399 ? L.length()-1 : int(i*step)]);
            L = s;
        }
        static const int NT = 201;
        vector<dlist> rows;
        loopv(L)
        {
            const p2shot &s = (*all)[L[i]];
            dv3 u;
            if(!rayon(s.fr, s.to, u)) continue;
            int nok = 0;
            dlist a;
            loopj(NT)
            {
                double tq = s.t + (-200.0 + 2*j);
                bool ok = tq >= pi.t[0] && tq <= pi.t[pi.n()-1];
                if(ok) { nok++; a.add(angledeg(pi.dir_at(tq), u)); }
                else a.add(999);
            }
            if(nok < 10) continue;
            rows.add(a);
        }
        nused = rows.length();
        if(rows.length() < 5) return 0.0;
        int bk = 0;
        double bm = 0;
        loopj(NT)
        {
            dlist col;
            loopv(rows) col.add(rows[i][j]);
            double m = npmedian(col);
            if(j == 0 || m < bm) { bm = m; bk = j; }
        }
        ecart = bm;
        return -200.0 + 2*bk;
    }

    // shift D (ms) of the targets that centres the shooter's hits best
    static bool choisir_D(const partie &P, int tireur, const touchmap &T, int &Dout, int &nh)
    {
        const vector<p2shot> *all = P.shots(tireur);
        struct h_t { int i, c; };
        vector<h_t> H;
        if(all) loopv(*all)
        {
            int c = T.get(tireur, i);
            int g = (*all)[i].gun;
            if(c >= 0 && (g == 1 || g == 4 || g == 6)) { h_t &h = H.add(); h.i = i; h.c = c; }
        }
        nh = H.length();
        if(H.length() < 3) return false;
        double bestv = 0;
        int bestD = -60;
        for(int D = -60; D <= 400; D += 10)
        {
            dlist e;
            loopv(H)
            {
                const p2shot &s = (*all)[H[i].i];
                const piste *corps = P.corps(H[i].c, tireur);
                if(!corps) continue;
                double tq = s.t - D;
                if(tq < corps->t[0] || tq > corps->t[corps->n()-1] || corps->trou(tq) > 150) continue;
                dv3 u;
                if(!rayon(s.fr, s.to, u)) continue;
                e.add(erreur_corps(s.fr, u, corps->eye_at(tq)));
            }
            double v = e.length() && e.length() >= 0.9*H.length() ? npmedian(e) : 99;
            if(D == -60 || v < bestv) { bestv = v; bestD = D; }
        }
        Dout = bestD;
        return bestv < 99;
    }

    // target of a shot: the one hit, else the living enemy closest to the line (in half-widths)
    static int cible_du_tir(const partie &P, int tireur, double ts, const dv3 &fr, const dv3 &u, double D, int touche)
    {
        if(touche >= 0) return touche;
        int best = -1;
        double bs = 0;
        loopv(P.joueurs)
        {
            int o = P.joueurs[i];
            if(o == tireur || P.meme_equipe(tireur, o, ts)) continue;
            const piste *corps = P.corps(o, tireur);
            if(!corps) continue;
            double tq = ts - D;
            if(tq < corps->t[0] || tq > corps->t[corps->n()-1] || corps->trou(tq) > 150) continue;
            if(!P.vivant(o, tq)) continue;
            dv3 e = corps->eye_at(tq);
            double dist = (e - fr).norm();
            double half = degrees(atan2(RAYON, dmax(dist, 1)));
            dv3 rc;
            if(!rayon(fr, centre(e), rc)) continue;
            double err = angledeg(rc, u);
            double sc = err/dmax(half, 0.05);
            if(best < 0 || sc < bs) { bs = sc; best = o; }
        }
        return best;
    }

    struct snaprow
    {
        double t, J1, dt_saut, pas, iso, frac, pose, piste_rayon, retour, eh, v_max, ecart_oeil, piste_rayon_seg;
        int gun, touche, cible;
        bool hasretour, haseh, hasoeil;
    };

    // snap measures of one shot at track time s with exact line u (None: not measurable)
    static bool snap_tir(const piste &pi, double s, const dv3 &u, const dlist *apparitions, const dv3 &fr, snaprow &x)
    {
        const dlist &t = pi.t;
        int n = t.length();
        if(apparitions && apparitions->length())
        {
            int j = ssright(*apparitions, s + 60) - 1;
            if(j >= 0 && s - (*apparitions)[j] < 400) return false;
        }
        int i0 = ssleft(t, s - 150), i1 = ssright(t, s);
        if(i0 < 3 || i1 + 3 >= n || i1 - i0 < 2) return false;
        double lim = pi.pas < 20 ? dmax(2.0*pi.pas, 50.0) : 100.0;
        int a0 = i0 - 3, b0 = i1 + 3;   // slice [a0, b0)
        for(int k = a0+1; k < b0; k++) if(t[k] - t[k-1] > lim) return false;
        for(int k = a0+1; k < b0; k++) if((pi.eye[k] - pi.eye[k-1]).norm() > 40) return false;
        int na = b0 - a0 - 1;
        dlist a, dts, r;
        loopk(na)
        {
            a.add(angledeg(pi.dir[a0+k], pi.dir[a0+k+1]));
            dts.add(t[a0+k+1] - t[a0+k]);
            r.add(a[k]/dmax(dts[k], 0.5)*1000.0);
        }
        int lo = 3, hi = 3 + (i1 - i0);
        if(hi > na - 2) hi = na - 2;
        if(hi <= lo) return false;
        int j = lo;
        for(int k = lo+1; k < hi; k++) if(a[k] > a[j]) j = k;
        double J1 = a[j];
        double iso;
        if(pi.pas < 20)
        {
            double vm = 0; bool any = false;
            for(int k = max(0, j-2); k < j; k++) { vm = any ? dmax(vm, r[k]) : r[k]; any = true; }
            for(int k = j+1; k < min(j+3, na); k++) { vm = any ? dmax(vm, r[k]) : r[k]; any = true; }
            iso = r[j]/dmax(any ? vm : 0, 20.0);
        }
        else
        {
            double vm = 0; bool any = false;
            for(int k = max(0, j-2); k < j; k++) { vm = any ? dmax(vm, a[k]) : a[k]; any = true; }
            for(int k = j+1; k < min(j+3, na); k++) { vm = any ? dmax(vm, a[k]) : a[k]; any = true; }
            iso = a[j]/dmax(any ? vm : 0, 0.66);
        }
        double chemin = pwsum(&a[lo], hi - lo);
        double frac = chemin > 1e-6 ? J1/chemin : 0.0;
        int k_after = i0 - 3 + j + 1;
        double pose = angledeg(pi.dir[k_after], u);
        double piste_rayon = angledeg(pi.dir_at(s), u);
        int kk = ssright(t, s);
        double ta = t[max(kk-2, 0)], tb = t[min(kk+1, n-1)];
        double step = (tb - ta)/40.0, prs = 999;
        loopk(41)
        {
            double tq;
            if(k == 40) tq = tb;
            else if(step == 0) tq = (k/40.0)*(tb - ta) + ta;
            else tq = k*step + ta;
            double ang = angledeg(pi.dir_at(tq), u);
            prs = k == 0 ? ang : dmin(prs, ang);
        }
        x.ecart_oeil = (pi.eye_at(s) - fr).norm();
        x.hasoeil = true;
        int k_before = k_after - 1;
        if(t[n-1] >= s + 60)
        {
            dv3 d60 = pi.dir_at(s + 60);
            x.retour = J1 > 1e-6 ? 1 - angledeg(d60, pi.dir[k_before])/dmax(J1, 1e-6) : 0.0;
            x.hasretour = true;
        }
        else x.hasretour = false;
        double vmax = r[lo];
        for(int k = lo+1; k < hi; k++) vmax = dmax(vmax, r[k]);
        dlist dsort = dts;
        x.J1 = J1; x.dt_saut = dts[j]; x.pas = npmedian(dsort); x.iso = iso; x.frac = frac; x.pose = pose;
        x.piste_rayon = piste_rayon; x.v_max = vmax; x.piste_rayon_seg = prs;
        return true;
    }

    // np.convolve(np.gradient(x, h), ones(k)/k, 'same') on a grid
    static void deriv(const dlist &x, double h, int k, dlist &out)
    {
        int n = x.length();
        dlist dx;
        loopi(n)
        {
            if(n < 2) { dx.add(0); continue; }
            if(i == 0) dx.add((x[1] - x[0])/h);
            else if(i == n-1) dx.add((x[n-1] - x[n-2])/h);
            else dx.add((x[i+1] - x[i-1])/(2.0*h));
        }
        out.setsize(0);
        double w = 1.0/k;
        int left = k/2;
        loopi(n)
        {
            double s = 0;
            for(int j = 0; j < k; j++)
            {
                int q = i + j - left;
                if(q < 0 || q >= n) continue;
                s += dx[q]*w;
            }
            out.add(s);
        }
    }

    // np.arange(start, stop, step) of floats (numpy fills start + i*delta, delta = (start + step) - start)
    static void nparange(double start, double stop, double step, dlist &out)
    {
        out.setsize(0);
        double len = ceil((stop - start)/step);
        if(!(len > 0)) return;
        int n = int(len);
        double delta = (start + step) - start;
        loopi(n) out.add(i == 0 ? start : (i == 1 ? start + step : start + i*delta));
    }

    struct suiviwin { double t, duree, err_half_med, retard_eq, vang_med; int cible; };
    struct suiviout { double proche_s, sur_s; vector<suiviwin> win; suiviout() : proche_s(0), sur_s(0) {} };

    // tracking: time the view stays on a moving living enemy (as the shooter saw him)
    static void suivi(const partie &P, int tireur, const piste &pi, double D, suiviout &out, const volatile bool *cancel)
    {
        const double pas = 10.0, min_ms = 400.0, max_half = 3.0, min_vang = 25.0;
        double t0 = dmax(pi.t[0], 0.0), t1 = dmin(pi.t[pi.n()-1], P.t_fin);
        if(t1 - t0 < 2000) return;
        bool exact = pi.exact();
        int k_l = exact ? 3 : 7;
        dlist g;
        nparange(t0, t1, pas, g);
        int n = g.length();
        vector<char> ok_t;
        vector<dv3> eye, d;
        dlist yaw;
        loopi(n)
        {
            ok_t.add(P.vivant(tireur, g[i]) && pi.trou(g[i]) <= (exact ? 60 : 120));
            eye.add(pi.eye_at(g[i]));
            double y, p;
            pi.ypr_at(g[i], y, p);
            yaw.add(y);
            d.add(dirvec(y, p));
        }
        dlist w_a;
        deriv(yaw, pas/1000.0, k_l, w_a);
        (void)w_a;
        loopv(P.joueurs)
        {
            if(cancel && *cancel) return;
            int o = P.joueurs[i];
            if(o == tireur) continue;
            const piste *corps = P.corps(o, tireur);
            if(!corps) continue;
            vector<char> ok;
            int nok = 0;
            double cmin = corps->t[0], cmax = corps->t[corps->n()-1];
            loopj(n)
            {
                double gq = g[j] - D;
                bool v = ok_t[j] && gq >= cmin && gq <= cmax && P.vivant(o, gq) && corps->trou(gq) <= 150;
                ok.add(v);
            }
            if(P.equipes)
            {
                vector<char> tm;
                for(int j = 0; j < n; j += 50) tm.add(!P.meme_equipe(tireur, o, g[j]));
                loopj(n) ok[j] = ok[j] && tm[j/50];
            }
            loopj(n) if(ok[j]) nok++;
            if(nok < min_ms/pas) continue;
            dlist dist, half, err, az;
            vector<dv3> vn;
            loopj(n)
            {
                dv3 te = centre(corps->eye_at(g[j] - D));
                dv3 v = te - eye[j];
                double dd = v.norm();
                dist.add(dd);
                dv3 u = v/dmax(dd, 1e-6);
                vn.add(u);
                half.add(degrees(atan2(RAYON, dmax(dd, 1))));
                err.add(angledeg(d[j], u));
                az.add(atan2(-u.x, u.y));
            }
            dlist azu;
            loopj(n) azu.add(0);
            unwraprad(az.getbuf(), n, azu.getbuf());   // np.unwrap(arctan2(...)) / RAD
            dlist w_t;
            deriv(azu, pas/1000.0, corps->pas < 20 ? 3 : 5, w_t);
            vector<char> on;
            loopj(n) on.add(ok[j] && err[j] <= dmax(max_half*half[j], 1.5) && dist[j] < 900);
            int j = 0;
            while(j < n)
            {
                if(!on[j]) { j++; continue; }
                int a0 = j;
                while(j < n && on[j]) j++;
                int b0 = j;
                if((b0 - a0)*pas < min_ms) continue;
                dlist vt;
                for(int q = a0; q < b0; q++) vt.add(fabs(w_t[q]));
                double vtm = npmedian(vt);
                if(vtm < min_vang) continue;
                dlist erh, e2;
                for(int q = a0; q < b0; q++) { erh.add(err[q]/dmax(half[q], 0.05)); e2.add(err[q]*err[q]); }
                double rms = sqrt(npmean(e2.getbuf(), e2.length()));
                suiviwin &w = out.win.add();
                w.t = g[a0]; w.cible = o; w.duree = (b0 - a0)*pas;
                w.err_half_med = npmedian(erh);
                w.vang_med = vtm;
                w.retard_eq = rms/dmax(vtm, 1e-6)*1000.0;
            }
            int np_ = 0, ns_ = 0;
            loopj(n)
            {
                bool proche = ok[j] && err[j] <= 12.0 && dist[j] < 900 && fabs(w_t[j]) >= min_vang;
                if(proche) { np_++; if(on[j]) ns_++; }
            }
            out.proche_s += np_*pas/1000.0;
            out.sur_s += ns_*pas/1000.0;
        }
    }

    // one player and one aim track: the line of regles.py
    struct g2line
    {
        int key, var;            // var: 0 dmo, 1 hd
        double tau, tau_ecart;
        int D, nh;
        bool hasD;
        double Dx;
        vector<snaprow> rows;
        suiviout S;
        // line
        int snap_n, snap_k, sil_k, douteux, fenetres;
        double precision, J1_q50, J1_q90, part_sur, err_med, retard_med;
        bool alert_snap, alert_sil, alert_suivi;
        double z_snap, z_sil, z_suivi;
    };

    struct g2params
    {
        // signalled snap shot: jump in one interval >= J1 deg, isolated, ending on the shot line
        double hd_J1, hd_iso, hd_pose, hd_dtpas, dmo_J1, dmo_iso, dmo_pose, dmo_dtmax, sil_hd, sil_dmo, oeil_hd, oeil_dmo;
        // alert of a game: repeats and distance to the players
        double Z, Ztrack;
        int rep_snap, rep_sil, rep_win;
        double proche_min;
        // the players of the studies (mean, deviation with its floor)
        double pop_dmo_snap[2], pop_dmo_sil[2], pop_dmo_sur[2], pop_hd_snap[2], pop_hd_sil[2], pop_hd_sur[2];
        g2params() : hd_J1(5), hd_iso(4), hd_pose(2.5), hd_dtpas(2), dmo_J1(8), dmo_iso(5), dmo_pose(1.5), dmo_dtmax(50),
                     sil_hd(5), sil_dmo(10), oeil_hd(6), oeil_dmo(12), Z(4), Ztrack(4), rep_snap(3), rep_sil(3), rep_win(10), proche_min(20)
        {
            pop_dmo_snap[0] = 0.00066525346944025878; pop_dmo_snap[1] = 0.01;
            pop_dmo_sil[0] = 0.00011168192986374804; pop_dmo_sil[1] = 0.01;
            pop_dmo_sur[0] = 0.36534891199217584; pop_dmo_sur[1] = 0.085017958892449202;
            pop_hd_snap[0] = 0; pop_hd_snap[1] = 0.01;
            pop_hd_sil[0] = 8.5550385112220033e-05; pop_hd_sil[1] = 0.01;
            pop_hd_sur[0] = 0.38678677179591647; pop_hd_sur[1] = 0.052005669655147468;
        }
    };

    static bool fiable(const snaprow &r, int var, const g2params &Q)
    {
        return r.hasoeil && pyround(r.ecart_oeil, 1) <= (var ? Q.oeil_hd : Q.oeil_dmo);
    }

    static bool signale_snap(const snaprow &r, int var, const g2params &Q)
    {
        if(!fiable(r, var, Q)) return false;
        double J1 = pyround(r.J1, 2), iso = pyround(r.iso, 2), pose = pyround(r.pose, 2), dts = pyround(r.dt_saut, 1), pas = pyround(r.pas, 1);
        if(var) return J1 >= Q.hd_J1 && iso >= Q.hd_iso && pose <= Q.hd_pose && dts <= Q.hd_dtpas*pas;
        return J1 >= Q.dmo_J1 && iso >= Q.dmo_iso && pose <= Q.dmo_pose && dts <= Q.dmo_dtmax;
    }

    static void g2finish(g2line &L, const g2params &Q)
    {
        int var = L.var;
        L.snap_n = L.snap_k = L.sil_k = 0;
        L.douteux = 0;
        dlist J1;
        int touches = 0;
        loopv(L.rows)
        {
            const snaprow &r = L.rows[i];
            if(!fiable(r, var, Q)) { L.douteux++; continue; }
            L.snap_n++;
            touches += r.touche;
            J1.add(pyround(r.J1, 2));
            if(signale_snap(r, var, Q)) L.snap_k++;
            if(pyround(r.piste_rayon_seg, 2) > (var ? Q.sil_hd : Q.sil_dmo)) L.sil_k++;
        }
        L.precision = L.snap_n ? double(touches)/L.snap_n : DNAN;
        L.J1_q50 = J1.length() ? npmedian(J1) : DNAN;
        L.J1_q90 = J1.length() ? nppercentile(J1, 90) : DNAN;
        L.part_sur = L.S.proche_s ? L.S.sur_s/L.S.proche_s : DNAN;
        L.fenetres = L.S.win.length();
        dlist em, rm;
        loopv(L.S.win) { em.add(pyround(L.S.win[i].err_half_med, 3)); rm.add(pyround(L.S.win[i].retard_eq, 1)); }
        L.err_med = em.length() ? pyround(npmedian(em), 3) : DNAN;
        L.retard_med = rm.length() ? pyround(npmedian(rm), 1) : DNAN;
        const double *ps = var ? Q.pop_hd_snap : Q.pop_dmo_snap, *pl = var ? Q.pop_hd_sil : Q.pop_dmo_sil, *pu = var ? Q.pop_hd_sur : Q.pop_dmo_sur;
        L.alert_snap = L.alert_sil = L.alert_suivi = false;
        L.z_snap = L.z_sil = L.z_suivi = DNAN;
        if(L.snap_n >= 10)
        {
            L.z_snap = (double(L.snap_k)/L.snap_n - ps[0])/ps[1];
            L.z_sil = (double(L.sil_k)/L.snap_n - pl[0])/pl[1];
            if(L.snap_k >= Q.rep_snap && L.z_snap >= Q.Z) L.alert_snap = true;
            if(L.sil_k >= Q.rep_sil && L.z_sil >= Q.Z) L.alert_sil = true;
        }
        if(L.S.proche_s && L.S.proche_s >= Q.proche_min)
        {
            L.z_suivi = (L.part_sur - pu[0])/pu[1];
            if(L.fenetres >= Q.rep_win && L.z_suivi >= Q.Ztrack) L.alert_suivi = true;
        }
    }

    struct g2player
    {
        int key, tirs_vises;
        double present_s, vivant_s, ping;
        vector<g2line> lines;      // dmo, then hd when there is an exact track
    };

    static void g2analyse(const partie &P, const g2params &Q, vector<g2player> &out, const volatile bool *cancel)
    {
        touchmap T;
        T.build(P);
        loopv(P.joueurs)
        {
            if(cancel && *cancel) return;
            int c = P.joueurs[i];
            const piste *pd = P.dmo(c);
            if(!pd) continue;
            const vector<p2shot> *all = P.shots(c);
            vector<int> L;
            if(all) loopvj(*all) if(vise((*all)[j].gun)) L.add(j);
            g2player &gp = out.add();
            gp.key = c;
            gp.tirs_vises = L.length();
            gp.ping = c != P.rec ? P.ping(c) : DNAN;
            gp.present_s = (pd->t[pd->n()-1] - pd->t[0])/1000.0;
            {
                dlist gg;
                nparange(pd->t[0], pd->t[pd->n()-1], 100.0, gg);
                int nv = 0;
                loopvj(gg) if(P.vivant(c, gg[j])) nv++;
                gp.vivant_s = nv/10.0;
            }
            const piste *variantes[2] = { pd, P.hd(c) };
            for(int var = 0; var < 2; var++)
            {
                const piste *pi = variantes[var];
                if(!pi) continue;
                g2line &ln = gp.lines.add();
                ln.key = c; ln.var = var;
                int nused = 0;
                ln.tau = L.length() >= 5 ? decalage_piste(P, *pi, c, ln.tau_ecart, nused) : 0.0;
                if(L.length() < 5) ln.tau_ecart = DNAN;
                int D = 0;
                ln.hasD = choisir_D(P, c, T, D, ln.nh);
                ln.D = D;
                ln.Dx = ln.hasD ? D : (c == P.rec || var == 1 ? 0 : double(int(P.ping(c))) + 20);
                loopvj(L)
                {
                    const p2shot &s = (*all)[L[j]];
                    dv3 u;
                    if(!rayon(s.fr, s.to, u)) continue;
                    snaprow x;
                    memset(&x, 0, sizeof(x));
                    if(!snap_tir(*pi, s.t + ln.tau, u, P.vies.find(c), s.fr, x)) continue;
                    int touche = T.get(c, L[j]);
                    int o = cible_du_tir(P, c, s.t, s.fr, u, ln.Dx, touche);
                    x.haseh = false;
                    if(o >= 0)
                    {
                        const piste *corps = P.corps(o, c);
                        double tq = s.t - ln.Dx;
                        if(corps && corps->t[0] <= tq && tq <= corps->t[corps->n()-1] && corps->trou(tq) <= 150)
                        {
                            dv3 e = corps->eye_at(tq);
                            double half = degrees(atan2(RAYON, dmax((e - s.fr).norm(), 1)));
                            dv3 rc;
                            if(rayon(s.fr, centre(e), rc)) { x.eh = angledeg(rc, u)/dmax(half, 0.05); x.haseh = true; }
                        }
                    }
                    x.t = s.t; x.gun = s.gun; x.touche = touche >= 0 ? 1 : 0; x.cible = o;
                    ln.rows.add(x);
                }
                suivi(P, c, *pi, ln.Dx + ln.tau, ln.S, cancel);
                g2finish(ln, Q);
            }
        }
    }

    // ------------------------------------------------------------ line of sight
    // The game's own test (preactlook in fps.cpp): from an eye to the points of
    // a body, done by the caller with the map of the demo loaded. Mode 0: clear
    // if one point is clear (tested in order). Mode 1: clear if at least 'need'
    // of the points in 'mask' are clear.
    struct losquery
    {
        float eye[3];
        float pts[7][3];
        uchar npts, mode, need, mask;
    };

    struct losapi
    {
        virtual ~losapi() {}
        // res: 1 clear, 0 blocked, one per query; false when the map of the demo is not loaded (any more)
        virtual bool run(const vector<losquery> &q, vector<uchar> &res) = 0;
    };

    static inline void setf3(float *d, const dv3 &v) { d[0] = float(v.x); d[1] = float(v.y); d[2] = float(v.z); }

    // ------------------------------------------------------------ trigger
    // measure.py of the trigger study (demo-cheat-check): flick shots, how far
    // the target's centre still was ahead of the crosshair, where the exact shot
    // line crossed the body (entry edge)
    static const double TR_STEP = 5, TR_GAP = 150, EYEH = 14.0, ABOVE = 1.0, TR_RADIUS = 4.1;

    struct trslot
    {
        int sid, cn, nshots;
        double t0, t1;
        dlist t, yu, pitch;
        vector<dv3> pos;
        dlist lst, len;
    };

    struct trk { bool alive; dv3 eye, dir; double yaw, pitch; };

    struct trshot { double t; int slot, gun; dv3 fr, to; };

    struct trline
    {
        int sid, cn, D, nflicks, npos, nbefore, nband;
        bool bot, alert;
        double before, band;
        vector<double> flicktimes, bandtimes, alltimes;
    };

    struct trgame
    {
        int end, n, mode;
        bool teammode, insta;
        const rawdemo *rd;
        vector<trslot> slots;          // tracked slots (>= 2 positions), sorted by sid
        imap sidx;                     // sid -> index in slots
        vector<int> cns;               // active slots (sid)
        vector<trshot> shots;          // shots of the active slots, t <= end
        imap hit;                      // shot index -> target sid
        imap D;                        // sid -> D (ms)
        // team events by raw cn
        imap teamidx;
        vector<dlist> teamt;
        vector<vector<int> > teamn;
        vector<int> slotsofcn_k;       // all slot bounds: cn, sid, t0, t1
        struct bound { int cn, sid; double t0, t1; bool named; };
        vector<bound> bounds;
        imap aicn;                     // raw cn of bots

        int idx(double t) const { int k = int(floor((t - 0.0)/TR_STEP)); return max(0, min(k, n-1)); }
        int slotof(int cn, double t) const
        {
            loopv(bounds) if(bounds[i].cn == cn && bounds[i].t0 <= t && t < bounds[i].t1) return bounds[i].named ? bounds[i].sid : -1;
            return -1;
        }
        int team_at(int sid, double t) const
        {
            int si = sidx.get(sid, -1);
            int cn = si >= 0 ? slots[si].cn : sid%1000;
            int ti = teamidx.get(cn, -1);
            if(ti < 0) return -1;
            int j = ssright(teamt[ti], t) - 1;
            return j >= 0 ? teamn[ti][j] : -1;
        }
        bool same_team(int a, int b, double t) const
        {
            if(!teammode) return false;
            int ta = team_at(a, t);
            if(ta < 0) return false;
            int tb = team_at(b, t);
            return tb >= 0 && !strcmp(rd->str(ta), rd->str(tb));
        }
        // team code on the grid (team name, or unique when none yet)
        int teamgrid(int sid, int k) const
        {
            int ta = team_at(sid, 5.0*k);
            return ta;
        }
        bool teamdiff(int a, int b, int k) const
        {
            int ta = teamgrid(a, k), tb = teamgrid(b, k);
            if(ta < 0 || tb < 0) return true;
            return strcmp(rd->str(ta), rd->str(tb)) != 0;
        }
    };

    static int trlife(const trslot &s, double x)
    {
        int j = ssright(s.lst, x) - 1;
        bool ok = j >= 0 && x <= s.len[max(j, 0)];
        return ok ? j : -1;
    }

    static void trackat(const trslot &s, int k, trk &r)
    {
        double gg = TR_STEP*k;
        int n = s.t.length();
        int i = max(1, min(ssright(s.t, gg), n-1));
        double t0 = s.t[i-1], t1 = s.t[i];
        double kk = dclamp((gg - t0)/dmax(t1 - t0, 1), 0, 1);
        int v0 = trlife(s, t0), v1 = trlife(s, t1), vg = trlife(s, gg);
        dv3 P = s.pos[i-1]*(1-kk) + s.pos[i]*kk;
        bool jump = (s.pos[i] - s.pos[i-1]).norm() > 40;
        r.alive = (t1 - t0 <= TR_GAP) && gg >= s.t[0] && gg <= s.t[n-1] && v0 >= 0 && v0 == v1 && vg == v0 && !jump;
        r.eye = dv3(P.x, P.y, P.z + EYEH);
        r.yaw = s.yu[i-1]*(1-kk) + s.yu[i]*kk;
        r.pitch = s.pitch[i-1]*(1-kk) + s.pitch[i]*kk;
        r.dir = dirvec(r.yaw, r.pitch);
    }

    static double aim_error(const dv3 &eye, const dv3 &d, const dv3 &te)
    {
        static const double dz[5] = { ABOVE, 0.0, -EYEH*0.45, -EYEH*0.7, -EYEH*0.95 };
        dv3 q0 = te - eye;
        double best = 999.0;
        loopi(5)
        {
            dv3 q = q0;
            q.z += dz[i];
            double n = q.norm();
            best = dmin(best, angledeg(q/(n > 1e-6 ? n : 1e-6), d));
        }
        return best;
    }

    // one game for the trigger (a game of the file; the study read whole files)
    static bool buildtrgame(const rawdemo &rd, int part, trgame &G)
    {
        G.rd = &rd;
        seggame d;
        buildseg(rd, part, d);
        int mapi = -1;
        G.mode = -1;
        loopv(d.ev) if(rd.ev[d.ev[i]].type == E_MAPCHANGE) { mapi = rd.ev[d.ev[i]].s1; G.mode = rd.ev[d.ev[i]].a; break; }
        if(mapi < 0) return false;
        G.teammode = isteammode(G.mode);
        G.insta = isinsta(G.mode);
        G.end = d.last_t;
        loopv(d.ev) { const devent &e = rd.ev[d.ev[i]]; if(e.type == E_TIMEUP && e.a == 0) { G.end = e.t; break; } }
        // slots: a cn between two disconnections (all N_CDIS of the game)
        imap cdisi;
        vector<dlist> cdist;
        loopv(d.ev)
        {
            const devent &e = rd.ev[d.ev[i]];
            if(e.type != E_CDIS) continue;
            int ci = cdisi.get(e.cn, -1);
            if(ci < 0) { ci = cdist.length(); cdist.add(); cdisi.set(e.cn, ci); }
            cdist[ci].add(e.t);
        }
        loopv(cdist) cdist[i].sort(dless);
        imap allcn;
        loopv(d.names.k) allcn.set(d.names.k[i], 1);
        loopv(d.pos) allcn.set(rd.pos[d.pos[i]].cn, 1);
        loopv(d.ev) { const devent &e = rd.ev[d.ev[i]]; if(e.type == E_INITAI) G.aicn.set(e.cn, 1); }
        // names in the slot range (initclient, initai, switchname)
        loopv(allcn.k)
        {
            int c = allcn.k[i];
            int ci = cdisi.get(c, -1);
            dlist cuts;
            if(ci >= 0) cuts = cdist[ci];
            cuts.add(1e12);
            double t0 = -1;
            loopvj(cuts)
            {
                double t1 = cuts[j];
                int sid = c + 1000*j;
                bool haspos = false, hasname = false;
                loopvk(d.pos) { const dposx &p = rd.pos[d.pos[k]]; if(p.cn == c && t0 <= p.t && p.t < t1) { haspos = true; break; } }
                loopvk(d.ev)
                {
                    const devent &e = rd.ev[d.ev[k]];
                    if((e.type == E_INITCLIENT || e.type == E_INITAI || e.type == E_SWITCHNAME) && e.cn == c && t0 <= e.t && e.t < t1) { hasname = true; break; }
                }
                trgame::bound &b = G.bounds.add();
                b.cn = c; b.sid = sid; b.t0 = t0; b.t1 = t1;
                b.named = !(!haspos && !hasname && sid != c);
                t0 = t1;
            }
        }
        // team events by raw cn (initclient, initai, setteam)
        loopv(d.ev)
        {
            const devent &e = rd.ev[d.ev[i]];
            if(e.type != E_INITCLIENT && e.type != E_INITAI && e.type != E_SETTEAM) continue;
            int ti = G.teamidx.get(e.cn, -1);
            if(ti < 0) { ti = G.teamt.length(); G.teamt.add(); G.teamn.add(); G.teamidx.set(e.cn, ti); }
            G.teamt[ti].add(e.t);
            G.teamn[ti].add(e.s2);
        }
        // positions per slot
        imap posl;
        vector<vector<int> > posv;
        loopv(G.bounds)
        {
            const trgame::bound &b = G.bounds[i];
            if(!b.named) continue;
            posv.add();
            posl.set(b.sid, posv.length()-1);
        }
        loopv(d.pos)
        {
            const dposx &p = rd.pos[d.pos[i]];
            loopvj(G.bounds)
            {
                const trgame::bound &b = G.bounds[j];
                if(b.cn == p.cn && b.t0 <= p.t && p.t < b.t1) { if(b.named) posv[posl.get(b.sid, 0)].add(d.pos[i]); break; }
            }
        }
        // shots (t <= end for counting)
        struct rawshot { double t; int slot, gun; dv3 fr, to; };
        vector<rawshot> allshots;
        loopv(d.ev)
        {
            const devent &e = rd.ev[d.ev[i]];
            if(e.type != E_SHOT) continue;
            int s = G.slotof(e.cn, e.t);
            if(s < 0) continue;
            rawshot &r = allshots.add();
            r.t = e.t; r.slot = s; r.gun = e.a;
            r.fr = dv3(e.fr[0], e.fr[1], e.fr[2]); r.to = dv3(e.to[0], e.to[1], e.to[2]);
        }
        imap ns;
        loopv(allshots) if(allshots[i].t <= G.end) ns.set(allshots[i].slot, ns.get(allshots[i].slot, 0) + 1);
        // deaths and damage (t <= end)
        struct dd_t { double t; int v, a; };
        vector<dd_t> deaths, damage;
        loopv(d.ev)
        {
            const devent &e = rd.ev[d.ev[i]];
            if(e.t > G.end) continue;
            if(e.type == E_DIED) { dd_t &x = deaths.add(); x.t = e.t; x.v = G.slotof(e.cn, e.t); x.a = G.slotof(e.a, e.t); }
            else if(e.type == E_DAMAGE) { dd_t &x = damage.add(); x.t = e.t; x.v = G.slotof(e.cn, e.t); x.a = G.slotof(e.a, e.t); }
        }
        // tracked slots
        loopv(posl.k)
        {
            const vector<int> &L = posv[posl.v[i]];
            if(L.length() < 2) continue;
            int sid = posl.k[i];
            trslot &s = G.slots.add();
            s.sid = sid;
            s.cn = 0;
            loopvj(G.bounds) if(G.bounds[j].sid == sid) { s.cn = G.bounds[j].cn; s.t0 = G.bounds[j].t0; s.t1 = G.bounds[j].t1; break; }
            s.nshots = ns.get(sid, 0);
            dlist t;
            loopvj(L) t.add(rd.pos[L[j]].t);
            vector<int> o;
            stableargsort(t.getbuf(), t.length(), o);
            dlist yaw;
            loopvj(o)
            {
                const dposx &p = rd.pos[L[o[j]]];
                s.t.add(p.t);
                s.pos.add(dv3(p.o[0], p.o[1], p.o[2]));
                yaw.add(p.yaw);
                s.pitch.add(p.pitch);
            }
            loopvj(yaw) s.yu.add(0);
            unwrapdeg(yaw.getbuf(), yaw.length(), s.yu.getbuf());
            // lives: spawns of the slot (or first position), each ending at the next death of the slot or the end
            dlist spw;
            loopvj(d.ev)
            {
                const devent &e = rd.ev[d.ev[j]];
                if((e.type == E_SPAWN || e.type == E_SPAWNSTATE) && e.cn == s.cn && s.t0 <= e.t && e.t < s.t1) spw.add(e.t);
            }
            spw.sort(dless);
            double p0 = rd.pos[L[0]].t;
            loopvj(L) p0 = dmin(p0, rd.pos[L[j]].t);
            if(spw.empty() || p0 < spw[0]) spw.insert(0, p0);
            dlist dm;
            loopvj(deaths) if(deaths[j].v == sid) dm.add(deaths[j].t);
            dm.sort(dless);
            double send = dmin(G.end, s.t1);
            loopvj(spw)
            {
                double sp = spw[j];
                if(s.lst.length() && sp <= s.len[s.len.length()-1]) continue;
                double nx = send;
                loopvk(dm) if(dm[k] >= sp) { nx = dm[k]; break; }
                s.lst.add(sp);
                s.len.add(nx);
            }
            G.sidx.set(sid, G.slots.length()-1);
        }
        // active slots: >= 50 positions and >= 5 shots
        loopv(G.slots) if(G.slots[i].t.length() >= 50 && G.slots[i].nshots >= 5) G.cns.add(G.slots[i].sid);
        G.cns.sort();
        if(G.cns.empty()) return false;
        imap iscn;
        loopv(G.cns) iscn.set(G.cns[i], 1);
        loopv(allshots) if(iscn.has(allshots[i].slot) && allshots[i].t <= G.end)
        {
            trshot &s = G.shots.add();
            s.t = allshots[i].t; s.slot = allshots[i].slot; s.gun = allshots[i].gun; s.fr = allshots[i].fr; s.to = allshots[i].to;
        }
        G.n = int(ceil(G.end/TR_STEP));
        if(G.n < 1) G.n = 1;
        // hits
        vector<dd_t> &hl = G.insta ? deaths : damage;
        loopv(hl)
        {
            const dd_t &x = hl[i];
            if(!iscn.has(x.a) || !G.sidx.has(x.v) || x.a == x.v || G.same_team(x.a, x.v, x.t)) continue;
            int cand = -1;
            loopvj(G.shots)
            {
                const trshot &s = G.shots[j];
                if(s.slot == x.a && 0 <= x.t - s.t && x.t - s.t <= 450 && (G.insta || s.gun == 1 || s.gun == 2 || s.gun == 4 || s.gun == 6)) cand = j;
            }
            if(cand >= 0) G.hit.set(cand, x.v);
        }
        return true;
    }

    static bool tr_error_to(const trgame &G, const trshot &s, double D, int o, double &e, double &dist)
    {
        int k = G.idx(s.t - D);
        trk T;
        trackat(G.slots[G.sidx.get(o, 0)], k, T);
        if(!T.alive) return false;
        dv3 u = s.to - s.fr;
        u = u/dmax(u.norm(), 1e-6);
        e = aim_error(s.fr, u, T.eye);
        dist = (T.eye - s.fr).norm();
        return true;
    }

    static void tr_choose_D(trgame &G)
    {
        loopv(G.cns)
        {
            int s = G.cns[i];
            vector<int> H;
            loopv(G.hit.k)
            {
                int j = G.hit.k[i];
                const trshot &sh = G.shots[j];
                if(sh.slot == s && (sh.gun == 1 || sh.gun == 4 || sh.gun == 6)) H.add(j);
            }
            double bv = 0;
            int bD = -60;
            for(int D = -60; D <= 400; D += 10)
            {
                dlist e;
                loopvj(H)
                {
                    double ee, dd;
                    if(tr_error_to(G, G.shots[H[j]], D, G.hit.get(H[j], -1), ee, dd)) e.add(ee);
                }
                double v = e.length() && e.length() >= 0.95*H.length() ? npmedian(e) : 99;
                if(D == -60 || v < bv) { bv = v; bD = D; }
            }
            G.D.set(s, bD);
        }
    }

    // the bot visibility test of the study (7 points, 100 deg view at 16:9, smallest visible size): geometry of one query
    static bool lookatquery(const trk &V, const dv3 &o, losquery &q, bool &trivial)
    {
        const double fov = 100.0, minpx = 5.0, pecc = 30.0, vlines = 1440.0;
        double yaw = V.yaw*DRAD, pitch = V.pitch*DRAD;
        dv3 f(-sin(yaw)*cos(pitch), cos(yaw)*cos(pitch), sin(pitch));
        dv3 rt(-cos(yaw), -sin(yaw), 0);
        dv3 up(f.y*rt.z - f.z*rt.y, f.z*rt.x - f.x*rt.z, f.x*rt.y - f.y*rt.x);
        dv3 tc = (o + dv3(0, 0, -EYEH*0.3)) - V.eye;
        double dist = tc.norm();
        double zc = tc.dot(f);
        double ecc = acos(dclamp(zc/dmax(dist, 1e-6), -1, 1))/DRAD;
        double th = tan(fov*0.5*DRAD), tv = th*9/16.0;
        double zcp = dmax(zc, 1e-3);
        double bodyang = (EYEH + ABOVE)/zcp;
        double minsize = minpx*2*tv/vlines*(1 + ecc/pecc);
        bool ok = zc >= 1 && sqrt(0.42)*bodyang >= minsize;
        int need = 99;
        if(ok) { need = int(dmax(ceil(7*minsize*minsize/(0.42*bodyang*bodyang)), 1)); ok = need <= 7; }
        trivial = true;
        if(!ok) return false;
        static const double bandz[5] = { 0.5, -EYEH*0.3, -EYEH*0.55, -EYEH*0.8, -EYEH + 1 };
        static const int band[7] = { 1, 0, 1, 1, 2, 3, 4 };
        dv3 side(tc.y, -tc.x, 0);
        double sl = side.norm();
        side = sl > 1e-3 ? side*(TR_RADIUS*0.85)/dmax(sl, 1e-6) : dv3(0, 0, 0);
        uchar mask = 0;
        setf3(q.eye, V.eye);
        loopi(7)
        {
            dv3 p = o;
            p.z = o.z + bandz[band[i]];
            if(i == 2) p = p - side;
            else if(i == 3) p = p + side;
            setf3(q.pts[i], p);
            dv3 v = p - V.eye;
            double z = v.dot(f);
            bool inv = z >= 1 && fabs(v.dot(rt)) <= z*th && fabs(v.dot(up)) <= z*tv;
            if(inv) mask |= 1<<i;
        }
        int ninv = 0;
        loopi(7) if(mask&(1<<i)) ninv++;
        if(ninv < need) return false;
        q.npts = 7; q.mode = 1; q.need = uchar(need); q.mask = mask;
        trivial = false;
        return true;
    }

    // flick shots of the active players; los: the map's line of sight (the
    // target of a missed shot is a seen enemy first, the closest otherwise)
    static bool tranalyse(trgame &G, losapi *los, vector<trline> &out, const volatile bool *cancel)
    {
        tr_choose_D(G);
        const int n = G.n;
        // the target of s seen D ms late, at grid point j
        auto shifted = [&](int s, int oo, int j, trk &T) -> bool
        {
            int sh = int(nearbyint(G.D.get(s, -60)/TR_STEP));
            int q = max(0, min(j - sh, n-1));
            trackat(G.slots[G.sidx.get(oo, 0)], q, T);
            return T.alive && j - sh >= 0;
        };
        auto both = [&](int s, int oo, int j) -> bool
        {
            trk A, T;
            trackat(G.slots[G.sidx.get(s, 0)], j, A);
            if(!A.alive) return false;
            if(!shifted(s, oo, j, T)) return false;
            if(G.teammode && !G.teamdiff(s, oo, j)) return false;
            return true;
        };
        auto aimed = [&](int gun) -> bool { return G.insta ? gun == 4 : (gun == 1 || gun == 4); };
        // ---- pass 1: the enemies a missed shot could be aimed at, and the visibility tests they need
        struct miss_t { int shot, s; vector<int> cand; };
        vector<miss_t> misses;
        imap missof;                  // shot index -> misses index
        vector<long long> keys;       // lookat keys (slot index of s, slot index of the enemy, grid point)
        auto lkkey = [](int si, int oi, int b) -> long long { return ((long long)si*4096 + oi)*(1LL<<24) + b; };
        loopv(G.cns)
        {
            int s = G.cns[i];
            int si = G.sidx.get(s, 0);
            loopvj(G.shots)
            {
                const trshot &sh = G.shots[j];
                if(sh.slot != s || !aimed(sh.gun) || G.hit.has(j)) continue;
                int kg = G.idx(sh.t), kD = G.idx(sh.t - G.D.get(s, -60));
                miss_t &m = misses.add();
                m.shot = j; m.s = s;
                missof.set(j, misses.length()-1);
                loopvk(G.slots)
                {
                    int oo = G.slots[k].sid;
                    if(oo == s || G.same_team(s, oo, sh.t)) continue;
                    trk T;
                    trackat(G.slots[k], kD, T);
                    if(!T.alive) continue;
                    m.cand.add(oo);
                    for(int jj = max(0, kg - 8); jj <= kg; jj++)
                    {
                        int b = (jj/4)*4;
                        if(both(s, oo, jj) && both(s, oo, b)) keys.add(lkkey(si, k, b));
                    }
                }
            }
        }
        {
            vector<long long> u = keys;
            u.sort();
            keys.setsize(0);
            loopv(u) if(!i || u[i] != u[i-1]) keys.add(u[i]);
        }
        vector<losquery> qs;
        vector<int> qof;
        loopv(keys)
        {
            long long key = keys[i];
            int b = int(key%(1LL<<24)), oi = int((key/(1LL<<24))%4096), si = int(key/(1LL<<24)/4096);
            trk V, T;
            trackat(G.slots[si], b, V);
            shifted(G.slots[si].sid, G.slots[oi].sid, b, T);
            losquery q;
            bool trivial;
            if(lookatquery(V, T.eye, q, trivial)) { qof.add(qs.length()); qs.add(q); }
            else qof.add(-1);
        }
        vector<uchar> res;
        if(los && qs.length() && !los->run(qs, res)) return false;
        if(cancel && *cancel) return false;
        auto lookat = [&](int s, int oo, int b) -> bool
        {
            if(!los) return false;
            long long key = lkkey(G.sidx.get(s, 0), G.sidx.get(oo, 0), b);
            int lo = 0, hi = keys.length();
            while(lo < hi) { int m = (lo+hi)>>1; if(keys[m] < key) lo = m+1; else hi = m; }
            if(lo >= keys.length() || keys[lo] != key) return false;
            int qi = qof[lo];
            return qi >= 0 && res[qi] != 0;
        };
        auto seen = [&](int s, int oo, int kg) -> bool
        {
            for(int jj = max(0, kg - 8); jj <= kg; jj++)
            {
                int b = (jj/4)*4;
                if(both(s, oo, jj) && both(s, oo, b) && lookat(s, oo, b)) return true;
            }
            return false;
        };
        // ---- pass 2: each shot of each active slot
        loopv(G.cns)
        {
            if(cancel && *cancel) return false;
            int s = G.cns[i];
            const trslot &S = G.slots[G.sidx.get(s, 0)];
            trline &L = out.add();
            L.sid = s; L.cn = S.cn; L.D = G.D.get(s, -60);
            L.bot = G.aicn.has(S.cn);
            L.nflicks = L.npos = L.nbefore = L.nband = 0;
            loopvj(G.shots)
            {
                const trshot &sh = G.shots[j];
                if(sh.slot != s || !aimed(sh.gun)) continue;
                double t = sh.t;
                int kg = G.idx(t), kD = G.idx(t - L.D);
                dv3 u = sh.to - sh.fr;
                u = u/dmax(u.norm(), 1e-6);
                int o = -1;
                double e = 0, dist = 0;
                if(G.hit.has(j))
                {
                    o = G.hit.get(j, -1);
                    if(!tr_error_to(G, sh, L.D, o, e, dist)) continue;
                }
                else
                {
                    int mi = missof.get(j, -1);
                    if(mi < 0) continue;
                    const miss_t &m = misses[mi];
                    bool found = false, bnotseen = false;
                    double bratio = 0;
                    loopvk(m.cand)
                    {
                        int oo = m.cand[k];
                        double ee, dd;
                        if(!tr_error_to(G, sh, L.D, oo, ee, dd)) continue;
                        double hh = degrees(atan2(TR_RADIUS, dmax(dd, 1)));
                        bool notseen = !seen(s, oo, kg);
                        double ratio = ee/dmax(2*hh, 2.0);
                        if(!found || notseen < bnotseen || (notseen == bnotseen && ratio < bratio))
                        {
                            found = true; bnotseen = notseen; bratio = ratio; o = oo; e = ee; dist = dd;
                        }
                    }
                    if(!found) continue;
                }
                trk J, Jk8, Ja, Jb, TO, TO0;
                trackat(S, kg, J);
                if(!J.alive) continue;
                double half = degrees(atan2(TR_RADIUS, dmax(dist, 1)));
                bool on_him = e < dmax(2*half, 2.0);
                int a = max(1, kg - 8), b = max(a + 1, kg);
                trackat(S, a, Ja);
                trackat(S, b, Jb);
                double speed = angledeg(Ja.dir, Jb.dir)/((b - a)*TR_STEP/1000.0);
                const trslot &O = G.slots[G.sidx.get(o, 0)];
                trackat(O, kD, TO);
                dv3 tc = TO.eye + dv3(0, 0, -EYEH*0.3) - sh.fr;
                tc = tc/dmax(tc.norm(), 1e-6);
                trackat(S, max(0, kg - 8), Jk8);
                dv3 m = J.dir - Jk8.dir;
                m = m - u*(m.dot(u));
                if(!(m.norm() > 1e-6 && speed >= 60)) continue;
                m = m/m.norm();
                double lead_half = degrees((tc - u).dot(m))/dmax(half, 0.05);
                trackat(O, max(0, kD - 8), TO0);
                dv3 tp = TO0.eye + dv3(0, 0, -EYEH*0.3) - Jk8.eye;
                tp = tp/dmax(tp.norm(), 1e-6);
                dv3 mc = tc - tp;
                mc = mc - u*(mc.dot(u));
                if(!(mc.norm() > 1e-6)) continue;
                mc = mc/mc.norm();
                if(!on_him || m.dot(mc) > 0.3) continue;
                // a flick shot
                L.nflicks++;
                L.alltimes.add(t);
                if(lead_half > 0) { L.nbefore++; L.flicktimes.add(t); }
                // where the exact line crosses the body (target_xy: its grid, linear between two grid points)
                auto target_xy = [&](double tq, double &X, double &Y) -> bool
                {
                    double f = (tq - 0.0)/TR_STEP;
                    int q = int(floor(f));
                    double w = f - q;
                    if(q < 0 || q + 1 >= n) return false;
                    trk A0, A1;
                    trackat(O, q, A0);
                    trackat(O, q + 1, A1);
                    if(!(A0.alive && A1.alive)) return false;
                    X = A0.eye.x*(1 - w) + A1.eye.x*w;
                    Y = A0.eye.y*(1 - w) + A1.eye.y*w;
                    return true;
                };
                double AX, AY, BX, BY;
                if(!target_xy(t - L.D, AX, AY) || !target_xy(t - L.D - 40, BX, BY)) continue;
                double uhx = u.x, uhy = u.y, nu = sqrt(uhx*uhx + uhy*uhy);
                if(nu < 0.3) continue;
                uhx /= nu; uhy /= nu;
                double sdx = -uhy, sdy = uhx;
                double mh = m.x*sdx + m.y*sdy;
                if(fabs(mh) < 0.5) continue;
                double sg = mh > 0 ? 1.0 : -1.0;
                double relx = AX - sh.fr.x, rely = AY - sh.fr.y;
                if(relx*uhx + rely*uhy < 8) continue;
                double pos = sg*(relx*sdx + rely*sdy)/TR_RADIUS;
                L.npos++;
                if(pos >= 0.5 && pos <= 2.0) { L.nband++; L.bandtimes.add(t); }
            }
            L.before = L.nflicks ? double(L.nbefore)/L.nflicks : DNAN;
            L.band = L.npos ? double(L.nband)/L.npos : DNAN;
            L.alert = false;
        }
        return true;
    }

    // ------------------------------------------------------------ group 3: hidden enemies
    // vue.py, groupe3.py and agrege3.py of the third study, and its rules
    // frozen before the demo of the known wallhacker was opened
    static inline double pymod(double a, double b)
    {
        double m = fmod(a, b);
        if(m != 0) { if((b < 0) != (m < 0)) m += b; }
        else m = copysign(0.0, b);
        return m;
    }

    struct g3params
    {
        double pas, froid, r_tir, r_son, r_deg, fov_yaw, fov_pitch, dmin, dmax, decal, losmax;
        double expo_min, z_ep, z_st, rapide, part_rapide, z_c, ac_zc;
        // F: following a hidden enemy who moves (range, view, his movement in 500 ms, how much nearer the view is to
        // him than to where he was last known, shortest follow, full points under e0 degrees and d0 units, length
        // bonus up to lmax s, alert level in points and against chance)
        double f_dmax, f_theta, f_move, f_marg, f_minms, f_e0, f_d0, f_lmax, f_alert, f_chance;
        // C: an enemy just seen or heard (ms) excuses a shot; a shot during a follow: window (ms) and its points
        double c_known, f_shotwin, f_shot;
        int rep_a, nb, nb_rapide, tirs_min_c, rep_c, ac_cmin;
        bool noduel;
        g3params() : pas(100), froid(1500), r_tir(1000), r_son(340), r_deg(500), fov_yaw(55), fov_pitch(40), dmin(50), dmax(1500),
                     decal(20000), losmax(250), expo_min(120), z_ep(5.565583658258552), z_st(5.050032407331453), rapide(150),
                     part_rapide(0.3580822207522539), z_c(1.7070096168943982), ac_zc(1.5), rep_a(6), nb(5), nb_rapide(3),
                     tirs_min_c(20), rep_c(5), ac_cmin(2), noduel(true)
        {
            f_dmax = 1000; f_theta = 2.0; f_move = 25; f_marg = 0.5; f_minms = 1000; f_e0 = 0.5; f_d0 = 400; f_lmax = 4;
            f_alert = 35; f_chance = 3.0; c_known = 500; f_shotwin = 3000; f_shot = 15;
        }
    };

    struct g3episode { double t, ms, dist, err; int E; bool suit; };
    struct g3follow { double t, ms, dist, err, pts; int E; };
    struct g3shotc { double t, err, dist; int E; bool profond, suit; };
    struct g3app { double t, err, half, dist, mouv, rt; int E; bool prete, hasrt, haserr; };

    struct g3line
    {
        int key, nenemies;
        bool measured, duel;
        double expo_s, vivant_s, D;
        // A: real and by chance (enemy shifted by +-20 s)
        int ep_r, stp_r, suit_r, nlong;
        double ep_c, stp_c, suit_c, z_ep, z_st, nouv_r, nouv_c, rank;
        vector<g3episode> episodes;
        // F: points, real and by chance, follows of at least the shortest length, their seconds
        double f_pts_r, f_pts_c, f_n_c, f_s_r;
        int f_n_r, f_shots;
        vector<g3follow> follows;
        // C
        int c_tirs, c_r, cp_r;
        double c_c, z_c;
        vector<g3shotc> cshots;
        // B
        vector<g3app> apps;
        vector<double> rt_tous;
        bool alertA, alertB, alertC, alertAC, alertF;
    };

    struct g3info { double t, r; };

    struct jeu
    {
        const partie *P;
        const g3params *Q;
        double t0, t1, rtt_rec;
        dlist T;
        imap infoidx;
        vector<vector<g3info> > info;
        imap porteidx;
        vector<vector<g3info> > porte;     // (start, end) as (t, r)
        vector<int> order;                 // observers: tracks of the demo in the order the study read them
        touchmap touches;

        double D(int O, int E) const
        {
            if(O == P->rec) return 0.0;
            double d = P->ping(O);
            if(E == P->rec && P->rec >= 0) d += rtt_rec;
            return dmin(dmax(d, 0.0), 400.0);
        }
        const piste *piste_E(int O, int E) const
        {
            if(O == P->rec && P->aff(E)) return P->aff(E);
            const piste *h = P->hd(E);
            if(h && h->nature == NAT_HD) return h;
            return P->dmo(E);
        }
        double mod(double t) const { return t0 + 1000.0 + pymod(t - (t0 + 1000.0), (t1 - 200.0) - (t0 + 1000.0)); }
    };

    static bool infoless(const g3info &a, const g3info &b) { return a.t < b.t || (a.t == b.t && a.r < b.r); }

    // round trip of the recorder of a local demo: shot -> frag / damage of its hits (60 ms if unknown)
    static double rtt_enregistreuse(const partie &P, const touchmap &T)
    {
        if(P.rec < 0) return 0.0;
        int pi = P.pingidx.get(P.rec, -1);
        if(pi >= 0 && P.pings[pi].length()) { dlist all; loopv(P.pings[pi]) all.add(P.pings[pi][i].p); return npmedian(all); }
        dlist d;
        const vector<p2shot> *L = P.shots(P.rec);
        loopv(T.idx.k)
        {
            int key = T.idx.k[i], c = T.idx.v[i];
            int a = key/65536, si = key%65536;
            if(a != P.rec || !L) continue;
            double ts = (*L)[si].t, best = DINF;
            bool any = false;
            if(P.insta) { loopvj(P.morts_ev) { const mortev &m = P.morts_ev[j]; if(m.actor == a && m.victim == c && 0 <= m.t - ts && m.t - ts <= 450) { best = any ? dmin(best, m.t) : m.t; any = true; } } }
            else { loopvj(P.degats) { const degev &g = P.degats[j]; if(g.actor == a && g.target == c && 0 <= g.t - ts && g.t - ts <= 450) { best = any ? dmin(best, g.t) : g.t; any = true; } } }
            if(any) d.add(best - ts);
        }
        return d.length() >= 5 ? npmedian(d) : 60.0;
    }

    static void buildjeu(const rawdemo &rd, const partie &P, const g3params &Q, jeu &J)
    {
        J.P = &P;
        J.Q = &Q;
        J.touches.build(P);
        double t0 = 0;
        bool first = true;
        loopv(P.pdmo.k) { const piste &p = P.pistes[P.pdmo.v[i]]; if(first || p.t[0] < t0) t0 = p.t[0]; first = false; }
        if(first) t0 = 0.0;
        loopv(P.g.ev) { const devent &e = rd.ev[P.g.ev[i]]; if(e.type == E_MAPCHANGE) { t0 = dmax(t0, e.t); break; } }
        J.t0 = t0;
        J.t1 = P.t_fin;
        nparange(t0 + 1000.0, P.t_fin - 200.0, Q.pas, J.T);
        J.rtt_rec = rtt_enregistreuse(P, J.touches);
        // what each player let the others know of him (time, range)
        auto addinfo = [&](int c, double t, double r)
        {
            int ii = J.infoidx.get(c, -1);
            if(ii < 0) { ii = J.info.length(); J.info.add(); J.infoidx.set(c, ii); }
            g3info &x = J.info[ii].add(); x.t = t; x.r = r;
        };
        loopv(P.g.ev)
        {
            const devent &e = rd.ev[P.g.ev[i]];
            int c = evkey(P.g, e);
            switch(e.type)
            {
                case E_SHOT: addinfo(c, e.t, Q.r_tir); break;
                case E_SOUND: case E_GUNSELECT: case E_ITEMACC: addinfo(c, e.t, Q.r_son); break;
                case E_TELEPORT: case E_JUMPPAD: addinfo(c, e.t, Q.r_deg); break;
                case E_DAMAGE:
                {
                    addinfo(c, e.t, Q.r_deg);
                    int a = evkeya(P.g, e);
                    if(a >= 0) addinfo(a, e.t, Q.r_deg);
                    break;
                }
            }
        }
        loopv(P.sautidx.k) { const dlist &s = P.sauts[P.sautidx.v[i]]; loopvj(s) addinfo(P.sautidx.k[i], s[j], Q.r_son); }
        loopv(J.info) J.info[i].sort(infoless);
        // flag carriers (always known: radar)
        imap debut;
        auto addporte = [&](int c, double a, double b)
        {
            int ii = J.porteidx.get(c, -1);
            if(ii < 0) { ii = J.porte.length(); J.porte.add(); J.porteidx.set(c, ii); }
            g3info &x = J.porte[ii].add(); x.t = a; x.r = b;
        };
        vector<int> debutorder;
        dlist debutt;
        loopv(P.g.ev)
        {
            const devent &e = rd.ev[P.g.ev[i]];
            int c = evkey(P.g, e);
            if(e.type == E_TAKEFLAG)
            {
                int di = debut.get(c, -1);
                if(di < 0) { debut.set(c, debutt.length()); debutt.add(e.t); debutorder.add(c); }
                else debutt[di] = e.t;
            }
            else if(e.type == E_DROPFLAG || e.type == E_SCOREFLAG || e.type == E_DIED)
            {
                int di = debut.get(c, -1);
                if(di >= 0 && !isnand(debutt[di])) { addporte(c, debutt[di], e.t); debutt[di] = DNAN; }
            }
        }
        loopv(debutorder) { int c = debutorder[i]; double t = debutt[debut.get(c, 0)]; if(!isnand(t)) addporte(c, t, P.t_fin); }
        // observers in the order of the demo's tracks (dict order of the study: raw cn, then its keys)
        vector<int> rawcn;
        imap seenraw;
        loopv(P.g.d.pos) { int c = rd.pos[P.g.d.pos[i]].cn; if(!seenraw.has(c)) { seenraw.set(c, 1); rawcn.add(c); } }
        imap seenkey;
        loopv(rawcn)
        {
            loopvj(P.g.d.pos)
            {
                const dposx &p = rd.pos[P.g.d.pos[j]];
                if(p.cn != rawcn[i] || p.t > P.t_fin) continue;
                int key = P.g.cle(p.cn, p.t);
                if(seenkey.has(key)) continue;
                seenkey.set(key, 1);
                if(P.dmo(key)) J.order.add(key);
            }
        }
    }

    // geometry of a pair (O, E) on the grid, for one variant (0 real, then the enemy shifted by +decal, -decal)
    struct g3geo
    {
        int E;
        double D;
        const piste *pe;
        vector<char> ok, los, vu, froid;
        dlist tE, best, dy, dp, dist, half, yawE, age;
        vector<dv3> eyeE;
        vector<int> idx;
    };

    static const double POINTS7[7][2] = { {-4.2, 0}, {0.5, 0}, {-4.2, -1}, {-4.2, 1}, {-7.7, 0}, {-11.2, 0}, {-13.0, 0} };

    static void bodyquery(const dv3 &A, const dv3 &E0, losquery &q)
    {
        dv3 tc = E0 - A;
        dv3 side(tc.y, -tc.x, 0);
        double sl = side.norm();
        side = sl > 1e-3 ? side*(RAYON*0.85/dmax(sl, 1e-3)) : dv3(0, 0, 0);
        setf3(q.eye, A);
        loopi(7)
        {
            dv3 b = E0;
            b.z += POINTS7[i][0];
            b = b + side*POINTS7[i][1];
            setf3(q.pts[i], b);
        }
        q.npts = 7; q.mode = 0; q.need = 1; q.mask = 0x7F;
    }

    static void angles_vue(double yaw, double pitch, const dv3 &eyeO, const dv3 &eyeE, double &best, double &dy, double &dp, double &dist, double &half, double &yawE)
    {
        dv3 d = dirvec(yaw, pitch);
        static const double dzs[4] = { 1.0, -4.0, -9.0, -14.0 };
        best = 999.0;
        loopi(4)
        {
            dv3 q = eyeE - eyeO;
            q.z += dzs[i];
            double n = q.norm();
            best = dmin(best, angledeg(q/dmax(n, 1e-6), d));
        }
        dv3 q = eyeE - eyeO;
        q.z += -5.0;
        dist = q.norm();
        yawE = degrees(atan2(-q.x, q.y));
        double pitE = degrees(atan2(q.z, hypot(q.x, q.y)));
        dy = pymod(yawE - yaw + 180.0, 360.0) - 180.0;
        dp = pitE - pitch;
        half = degrees(atan2(RAYON, dmax(dist, 1.0)));
    }

    static void etat_info(const jeu &J, int O, g3geo &G, const vector<dv3> &eyeO, double decal)
    {
        const g3params &Q = *J.Q;
        const dlist &T = J.T;
        int n = T.length();
        G.vu.setsize(0);
        vector<char> connu;
        loopi(n)
        {
            bool v = G.ok[i] && G.los[i] && fabs(G.dy[i]) <= Q.fov_yaw && fabs(G.dp[i]) <= Q.fov_pitch;
            G.vu.add(v);
            connu.add(v);
        }
        int ii = J.infoidx.get(G.E, -1);
        double D = J.D(O, G.E);
        if(ii >= 0)
        {
            const vector<g3info> &ev = J.info[ii];
            loopv(ev)
            {
                double te = ev[i].t + D - decal;
                if(decal) te = J.mod(te);
                double kf = nearbyint((te - T[0])/Q.pas);
                if(!(kf >= 0 && kf < n)) continue;
                int k = int(kf);
                double dd = (eyeO[k] - G.eyeE[k]).norm();
                if(dd <= ev[i].r && G.ok[k]) connu[k] = true;
            }
        }
        int pi = J.porteidx.get(G.E, -1);
        if(pi >= 0)
        {
            const vector<g3info> &pr = J.porte[pi];
            loopv(pr)
            {
                double a = pr[i].t, b = pr[i].r;
                if(decal)
                {
                    loopj(n)
                    {
                        double m = pymod(T[j] - D + decal - J.t0 - 1000.0, J.t1 - J.t0 - 1200.0) + J.t0 + 1000.0;
                        if(m >= a && m <= b) connu[j] = true;
                    }
                }
                else
                {
                    double ta = a + D - decal, tb = b + D - decal;
                    loopj(n) if(T[j] >= ta && T[j] <= tb) connu[j] = true;
                }
            }
        }
        G.idx.setsize(0);
        G.age.setsize(0);
        int last = -1, seg = 0;
        loopi(n)
        {
            if(connu[i]) last = i;
            if(G.ok[i] && (i == 0 || !G.ok[i-1])) seg = i;
            G.idx.add(last >= seg ? last : -1);
            G.age.add((i - seg)*Q.pas);
        }
        G.froid.setsize(0);
        loopi(n)
        {
            double depuis = G.idx[i] >= 0 ? (i - G.idx[i])*Q.pas : 1e9;
            G.froid.add(G.ok[i] && depuis >= Q.froid);
        }
    }

    struct g3A
    {
        double expo_s, sur_s, nouveau_s, strict_s;
        int episodes, suivis, strict_ep;
        vector<g3episode> liste;
    };

    static void mesure_A(const jeu &J, const vector<dv3> &eyeO, const dlist &y, const dlist &p, const vector<char> &okO, vector<g3geo> &geos, g3A &r, bool keeplist)
    {
        const g3params &Q = *J.Q;
        const dlist &T = J.T;
        int n = T.length();
        vector<char> expo, sur, nouv, nst;
        loopi(n) { expo.add(0); sur.add(0); nouv.add(0); nst.add(0); }
        r.episodes = r.suivis = r.strict_ep = 0;
        vector<dv3> d;
        loopi(n) d.add(dirvec(y[i], p[i]));
        loopv(geos)
        {
            g3geo &G = geos[i];
            vector<char> nv, ns;
            loopj(n)
            {
                double theta = dmax(4.0, 3.0*G.half[j]);
                bool cache = G.ok[j] && !G.los[j] && G.froid[j] && G.dist[j] >= Q.dmin && G.dist[j] <= Q.dmax && okO[j];
                if(cache) expo[j] = 1;
                bool s = cache && G.best[j] <= theta;
                int idx = G.idx[j];
                bool lk_ok = idx >= 0;
                int i0 = max(idx, 0), i1 = max(i0 - 2, 0);
                dv3 lk = G.eyeE[i0];
                dv3 q = lk - eyeO[j]; q.z -= 5.0;
                double e_lk = angledeg(q/dmax(q.norm(), 1e-6), d[j]);
                dv3 vel = (G.eyeE[i0] - G.eyeE[i1])/dmax((i0 - i1)*Q.pas/1000.0, 1e-3);
                if(i0 - i1 == 0) vel = dv3(0, 0, 0);
                double dt_ = dmin((j - i0)*Q.pas/1000.0, 1.5);
                dv3 lk2 = lk + vel*dt_;
                dv3 q2 = lk2 - eyeO[j]; q2.z -= 5.0;
                double e_lk2 = angledeg(q2/dmax(q2.norm(), 1e-6), d[j]);
                double lim = dmax(theta, 8.0);
                bool v = s && (!lk_ok || (e_lk > lim && e_lk2 > lim));
                if(s) sur[j] = 1;
                if(v) nouv[j] = 1;
                double th2 = dmax(2.5, 2.0*G.half[j]);
                bool st = v && G.best[j] <= th2 && G.dist[j] <= 1000.0;
                if(st) nst[j] = 1;
                nv.add(v); ns.add(st);
            }
            // strict episodes: >= 300 ms on the same hidden enemy
            for(int j = 0; j < n;)
            {
                if(!ns[j]) { j++; continue; }
                int a = j; while(j < n && ns[j]) j++;
                if(j - a >= 3) r.strict_ep++;
            }
            for(int j = 0; j < n;)
            {
                if(!nv[j]) { j++; continue; }
                int a = j; while(j < n && nv[j]) j++;
                int b = j;
                if(b - a < 3) continue;
                g3episode ep;
                ep.E = G.E; ep.t = T[a]; ep.ms = (b - a)*Q.pas;
                dlist ds, es;
                for(int q = a; q < b; q++) { ds.add(G.dist[q]); es.add(G.best[q]); }
                ep.dist = npmedian(ds); ep.err = npmedian(es);
                ep.suit = false;
                if(b - a >= 4)
                {
                    double dyE = pymod((G.yawE[b-1] - G.yawE[a]) + 180, 360) - 180;
                    double dyO = y[b-1] - y[a];
                    double sO = dyO > 0 ? 1 : (dyO < 0 ? -1 : 0), sE = dyE > 0 ? 1 : (dyE < 0 ? -1 : 0);
                    if(fabs(dyE) >= 10.0 && sO == sE && fabs(dyO) >= 0.5*fabs(dyE)) { ep.suit = true; r.suivis++; }
                }
                r.episodes++;
                if(keeplist) r.liste.add(ep);
            }
        }
        int ne = 0, ns = 0, nn = 0, nt = 0;
        loopi(n) { ne += expo[i]; ns += sur[i]; nn += nouv[i]; nt += nst[i]; }
        r.expo_s = ne*Q.pas/1000.0; r.sur_s = ns*Q.pas/1000.0; r.nouveau_s = nn*Q.pas/1000.0; r.strict_s = nt*Q.pas/1000.0;
    }

    // where the game of O showed E at time t (O's clock), for a variant
    static dv3 pos_E(const jeu &J, const g3geo &G, double t, double dec)
    {
        double tE = t - G.D + dec;
        if(dec) tE = J.mod(tE);
        return G.pe->eye_at(tE);
    }

    static inline void uyawpitch(const dv3 &u, double &yaw, double &pitch)
    {
        yaw = degrees(atan2(-u.x, u.y));
        pitch = degrees(asin(dmax(-1.0, dmin(1.0, u.z))));
    }

    // B: an enemy comes out (line of sight opens, he was cold), view already on him, first shot at him
    static bool mesure_B(const jeu &J, int O, const vector<dv3> &eyeO, const dlist &y, const dlist &p, const vector<char> &okO,
                         vector<g3geo> &geos, losapi *los, vector<g3app> &app)
    {
        const g3params &Q = *J.Q;
        const partie &P = *J.P;
        const dlist &T = J.T;
        int n = T.length();
        const piste *pv = P.visee(O);
        const piste *pe = P.hd(O) ? P.hd(O) : P.dmo(O);
        const vector<p2shot> *tirs = P.shots(O);
        struct cand_t { int g, kk; double ta; };
        vector<cand_t> cand;
        loopv(geos)
        {
            const g3geo &G = geos[i];
            for(int k = 1; k < n; k++)
            {
                if(G.ok[k] && G.ok[k-1] && okO[k] && okO[k-1] && !G.los[k-1] && G.los[k] && G.froid[k-1] &&
                   fabs(G.dy[k]) <= Q.fov_yaw && fabs(G.dp[k]) <= Q.fov_pitch && G.dist[k] >= Q.dmin)
                { cand_t &c = cand.add(); c.g = i; c.kk = k; c.ta = T[k]; }
            }
        }
        if(cand.empty()) return true;
        // the instant refined to 25 ms: two line of sight tests (-50 ms, then -25 ms)
        static const double steps[2] = { 50.0, 25.0 };
        loopk(2)
        {
            vector<losquery> qs;
            loopv(cand)
            {
                double tq = cand[i].ta - steps[k];
                losquery &q = qs.add();
                bodyquery(pe->eye_at(tq), pos_E(J, geos[cand[i].g], tq, 0), q);
            }
            vector<uchar> res;
            if(!los || !los->run(qs, res)) return false;
            loopv(cand) if(res[i]) cand[i].ta -= steps[k];
        }
        dlist ts_all;
        if(tirs) loopv(*tirs) ts_all.add((*tirs)[i].t);
        loopv(cand)
        {
            const g3geo &G = geos[cand[i].g];
            double t_a = cand[i].ta;
            int E = G.E;
            dv3 eE = pos_E(J, G, t_a, 0), eO = pe->eye_at(t_a);
            double yy, pp;
            pv->ypr_at(t_a, yy, pp);
            dv3 dO = dirvec(yy, pp);
            double err, dy_, dp_, dist, half, ye;
            angles_vue(yy, pp, eO, eE, err, dy_, dp_, dist, half, ye);
            double y0, p0;
            pv->ypr_at(t_a - 400.0, y0, p0);
            double mouv = angledeg(dirvec(y0, p0), dO);
            int j = ssleft(ts_all, t_a) - 1;
            bool prete = true;
            if(j >= 0)
            {
                int g = (*tirs)[j].gun;
                prete = (t_a - (*tirs)[j].t) >= (g >= 0 && g < 7 ? DELAI[g] : 500);
            }
            bool hasrt = false;
            double rt = 0;
            if(tirs) for(int s = j + 1; s < tirs->length(); s++)
            {
                const p2shot &sh = (*tirs)[s];
                if(sh.t > t_a + 1000.0) break;
                if(sh.t < t_a) continue;
                dv3 u = sh.to - sh.fr;
                double nn = u.norm();
                if(nn < 1e-6) continue;
                u = u/nn;
                dv3 eS = pos_E(J, G, sh.t, 0);
                double uy, up, bb, a1, a2, dd, hh, a3;
                uyawpitch(u, uy, up);
                angles_vue(uy, up, sh.fr, eS, bb, a1, a2, dd, hh, a3);
                if(J.touches.get(O, s) == E || bb <= dmax(2.0, 3.0*hh)) { rt = sh.t - t_a; hasrt = true; }
                break;
            }
            g3app &a = app.add();
            a.E = E; a.t = t_a; a.err = pyround(err, 2); a.half = pyround(half, 3); a.dist = pyround(dist, 0);
            a.mouv = pyround(mouv, 2); a.prete = prete; a.hasrt = hasrt; a.rt = hasrt ? pyround(rt, 0) : DNAN; a.haserr = true;
        }
        return true;
    }

    struct g3C { int tirs, caches, profonds, suivis; vector<g3shotc> liste; };

    // C: shots at a hidden cold enemy (exact shot line, the line of sight from the shot blocked)
    static bool mesure_C(const jeu &J, int O, const vector<char> &okO, vector<g3geo> &geos, double dec, losapi *los, g3C &r, bool keeplist,
                         const vector<g3follow> &follows)
    {
        const partie &P = *J.P;
        const g3params &Q = *J.Q;
        const dlist &T = J.T;
        int n = T.length();
        r.tirs = r.caches = r.profonds = r.suivis = 0;
        const vector<p2shot> *tirs = P.shots(O);
        if(!tirs) return true;
        struct cand_t { double t, b, dist; int E; bool prof, suit; dv3 fr, eE; };
        vector<cand_t> cand;
        loopv(*tirs)
        {
            const p2shot &sh = (*tirs)[i];
            if(sh.gun == 0) continue;
            double kf = nearbyint((sh.t - T[0])/Q.pas);
            if(kf < 0 || kf >= n) continue;
            int k = int(kf);
            if(!okO[k]) continue;
            r.tirs++;
            if(J.touches.has(O, i)) continue;
            dv3 u = sh.to - sh.fr;
            double nn = u.norm();
            if(nn < 1e-6) continue;
            u = u/nn;
            bool vis_pres = false, any = false;
            cand_t best;
            loopvj(geos)
            {
                const g3geo &G = geos[j];
                if(!G.ok[k]) continue;
                dv3 eE = pos_E(J, G, sh.t, dec);
                dv3 q = eE - sh.fr;
                double dist = q.norm();
                if(dist < Q.dmin || dist > Q.dmax) continue;
                double half = degrees(atan2(RAYON, dist));
                double th = dmax(2.0, 2.0*half);
                double uy, up, b, a1, a2, a3, a4, a5;
                uyawpitch(u, uy, up);
                angles_vue(uy, up, sh.fr, eE, b, a1, a2, a3, a4, a5);
                if(b > dmax(th, 8.0) + 2.0) continue;
                if(G.los[k] && b <= th) { vis_pres = true; continue; }
                if(b > th) continue;
                // the first moments after the shooter or the enemy (re)appears: where players appear is known
                if(G.age[k] < Q.froid) continue;
                // a shot during a follow of this enemy (Follows hidden) always counts
                bool suit = false;
                for(int s_ = 0; s_ < follows.length(); s_++)
                {
                    const g3follow &w = follows[s_];
                    if(w.E == G.E && sh.t >= w.t - Q.f_shotwin && sh.t <= w.t + w.ms - Q.pas + Q.f_shotwin) { suit = true; break; }
                }
                int idx = G.idx[k];
                if(idx >= 0 && !suit)
                {
                    // just seen or heard: his place is known
                    if((k - idx)*Q.pas < Q.c_known) continue;
                    dv3 lk = G.eyeE[idx];
                    int i1 = max(idx - 2, 0);
                    dv3 vel = idx > i1 ? (G.eyeE[idx] - G.eyeE[i1])/dmax((idx - i1)*Q.pas/1000.0, 1e-3) : dv3(0, 0, 0);
                    dv3 lk2 = lk + vel*dmin((k - idx)*Q.pas/1000.0, 1.5);
                    // the shot nearer to where he was known (or that place carried along his way) than to him: guessed
                    bool skip = false;
                    const dv3 *ls[2] = { &lk, &lk2 };
                    loopl(2)
                    {
                        dv3 qq = *ls[l] - sh.fr;
                        qq.z -= 5.0;
                        double a_ = degrees(acos(dmax(-1.0, dmin(1.0, (qq/dmax(qq.norm(), 1e-6)).dot(u)))));
                        if(a_ < b + Q.f_marg) skip = true;
                    }
                    if(skip) continue;
                }
                bool prof = true;
                for(int q2 = k; q2 < min(n, k + 6); q2++) if(G.los[q2] && G.ok[q2]) { prof = false; break; }
                if(!any || b < best.b) { best.t = sh.t; best.b = b; best.dist = dist; best.E = G.E; best.prof = prof; best.suit = suit; best.fr = sh.fr; best.eE = eE; any = true; }
            }
            if(vis_pres || !any) continue;
            cand.add(best);
        }
        if(cand.empty()) return true;
        vector<losquery> qs;
        loopv(cand) bodyquery(cand[i].fr, cand[i].eE, qs.add());
        vector<uchar> res;
        if(!los || !los->run(qs, res)) return false;
        loopv(cand) if(!res[i])
        {
            r.caches++;
            if(cand[i].prof) r.profonds++;
            if(cand[i].suit) r.suivis++;
            if(keeplist) { g3shotc &c = r.liste.add(); c.t = cand[i].t; c.err = pyround(cand[i].b, 2); c.dist = pyround(cand[i].dist, 0); c.E = cand[i].E; c.profond = cand[i].prof; c.suit = cand[i].suit; }
        }
        return true;
    }

    // F: the view follows an enemy behind a wall who moves, at least f_minms, where neither his last known place nor
    // that place carried along his way explains it (the view is nearer to him than to both by f_marg degrees, or
    // nothing was known of him in this life), never in the first cold ms after one of them (re)appears. Points of a
    // follow: 1 for each 100 ms, x 1 under f_e0 degrees down to 0.25 at the edge of the view, x 1 under f_d0 units
    // down to 0.25 at f_dmax, then x (1 + its length up to f_lmax s / f_lmax): exact, near and long counts most.
    struct g3F { double pts, secs; int n; vector<g3follow> liste; };
    static void mesure_F(const jeu &J, const vector<dv3> &eyeO, const dlist &y, const dlist &p, const vector<char> &okO, const vector<g3geo> &geos, g3F &r, bool keeplist)
    {
        const g3params &Q = *J.Q;
        const dlist &T = J.T;
        int n = T.length();
        r.pts = r.secs = 0; r.n = 0;
        loopv(geos)
        {
            const g3geo &G = geos[i];
            vector<char> on;
            loopj(n)
            {
                bool s = G.ok[j] && okO[j] && !G.los[j] && G.dist[j] >= Q.dmin && G.dist[j] <= Q.f_dmax && G.age[j] >= Q.froid &&
                         G.best[j] <= dmax(Q.f_theta, 3.0*G.half[j]);
                if(s)
                {
                    int j5 = max(j - 5, j - int(G.age[j]/Q.pas + 0.5));
                    s = (G.eyeE[j] - G.eyeE[j5]).norm() >= Q.f_move;
                }
                if(s && G.idx[j] >= 0)
                {
                    dv3 d = dirvec(y[j], p[j]);
                    int i0 = G.idx[j], i1 = max(i0 - 2, 0);
                    dv3 lk = G.eyeE[i0];
                    dv3 q = lk - eyeO[j]; q.z -= 5.0;
                    double e_lk = angledeg(q/dmax(q.norm(), 1e-6), d);
                    dv3 vel = i0 > i1 ? (G.eyeE[i0] - G.eyeE[i1])/dmax((i0 - i1)*Q.pas/1000.0, 1e-3) : dv3(0, 0, 0);
                    dv3 q2 = lk + vel*dmin((j - i0)*Q.pas/1000.0, 1.5) - eyeO[j]; q2.z -= 5.0;
                    double e_lk2 = angledeg(q2/dmax(q2.norm(), 1e-6), d);
                    s = dmin(e_lk, e_lk2) >= G.best[j] + Q.f_marg;
                }
                on.add(s);
            }
            // follows: samples on the same enemy, one missing sample allowed
            for(int j = 0; j < n;)
            {
                if(!on[j]) { j++; continue; }
                int a = j, b = j;
                while(j < n && (on[j] || (j + 1 < n && on[j+1]))) { if(on[j]) b = j; j++; }
                double ms = (b - a + 1)*Q.pas;
                if(ms < Q.f_minms) continue;
                double pts = 0;
                dlist ds, es;
                for(int q = a; q <= b; q++)
                {
                    if(!on[q]) continue;
                    double th = dmax(Q.f_theta, 3.0*G.half[q]);
                    double fe = G.best[q] <= Q.f_e0 ? 1.0 : dmax(0.25, 1.0 - 0.75*(G.best[q] - Q.f_e0)/dmax(th - Q.f_e0, 1e-6));
                    double fd = G.dist[q] <= Q.f_d0 ? 1.0 : dmax(0.25, 1.0 - 0.75*(G.dist[q] - Q.f_d0)/dmax(Q.f_dmax - Q.f_d0, 1e-6));
                    pts += fe*fd;
                    ds.add(G.dist[q]); es.add(G.best[q]);
                }
                pts *= 1.0 + dmin(ms/1000.0, Q.f_lmax)/Q.f_lmax;
                r.pts += pts; r.secs += ms/1000.0; r.n++;
                if(keeplist)
                {
                    g3follow &f = r.liste.add();
                    f.t = T[a]; f.ms = ms; f.E = G.E; f.pts = pts; f.dist = npmedian(ds); f.err = npmedian(es);
                }
            }
        }
    }

    // all of group 3 for one observer (key O); false when the line of sight became unavailable
    static bool g3observer(const jeu &J, int O, losapi *los, g3line &L, const volatile bool *cancel)
    {
        const partie &P = *J.P;
        const g3params &Q = *J.Q;
        const dlist &T = J.T;
        int n = T.length();
        L.key = O; L.measured = false; L.duel = false; L.nenemies = 0;
        L.alertA = L.alertB = L.alertC = L.alertAC = L.alertF = false;
        L.f_pts_r = L.f_pts_c = L.f_n_c = L.f_s_r = 0; L.f_n_r = L.f_shots = 0;
        const piste *pv = P.visee(O);
        const piste *pe = P.hd(O) ? P.hd(O) : P.dmo(O);
        if(!pv || !pe || n <= 0) return true;
        vector<dv3> eyeO;
        dlist y, p;
        vector<char> okO;
        int nok = 0;
        loopi(n)
        {
            double t = T[i];
            bool ok = t >= pv->t[0] && t <= pv->t[pv->n()-1] && pv->trou(t) <= Q.losmax && t >= pe->t[0] && t <= pe->t[pe->n()-1] && pe->trou(t) <= Q.losmax;
            ok = ok && P.vivant(O, t);
            okO.add(ok);
            if(ok) nok++;
            double yy, pp;
            pv->ypr_at(t, yy, pp);
            y.add(yy); p.add(pp);
            eyeO.add(pe->eye_at(t));
        }
        L.vivant_s = nok*Q.pas/1000.0;
        if(nok < 50) return true;
        L.measured = true;
        double tmed = npmedian(T);
        const double decals[3] = { 0.0, Q.decal, -Q.decal };
        vector<g3geo> geos[3];
        dlist Ds;
        imap enemynames;
        loopv(J.order)
        {
            int E = J.order[i];
            if(E == O) continue;
            if(P.meme_equipe(O, E, tmed)) continue;
            const piste *pE = J.piste_E(O, E);
            if(!pE) continue;
            double D = J.D(O, E);
            Ds.add(D);
            int nE = 0;
            loopk(3)
            {
                g3geo &G = geos[k].add();
                G.E = E; G.D = D; G.pe = pE;
                loopj(n)
                {
                    double tE = T[j] - D + decals[k];
                    if(decals[k]) tE = J.mod(tE);
                    bool ok = okO[j] && tE >= pE->t[0] && tE <= pE->t[pE->n()-1] && pE->trou(tE) <= Q.losmax && P.vivant(E, tE);
                    G.ok.add(ok);
                    G.tE.add(tE);
                    G.eyeE.add(pE->eye_at(tE));
                    if(k == 0 && ok) nE++;
                }
            }
            if(nE >= 50) enemynames.set(P.g.names.get(E, -1), 1);
        }
        L.D = Ds.length() ? npmedian(Ds) : DNAN;
        L.nenemies = enemynames.length();
        L.duel = L.nenemies <= 1;
        // line of sight of every pair and variant (7 points of the body, one clear is enough)
        loopk(3)
        {
            if(cancel && *cancel) return false;
            vector<losquery> qs;
            loopv(geos[k]) { const g3geo &G = geos[k][i]; loopj(n) if(G.ok[j]) bodyquery(eyeO[j], G.eyeE[j], qs.add()); }
            vector<uchar> res;
            if(qs.length() && (!los || !los->run(qs, res))) return false;
            int qi = 0;
            loopv(geos[k])
            {
                g3geo &G = geos[k][i];
                loopj(n) G.los.add(G.ok[j] ? res[qi++] != 0 : false);
            }
        }
        loopk(3) loopv(geos[k])
        {
            g3geo &G = geos[k][i];
            loopj(n)
            {
                double best, dy, dp, dist, half, ye;
                angles_vue(y[j], p[j], eyeO[j], G.eyeE[j], best, dy, dp, dist, half, ye);
                G.best.add(best); G.dy.add(dy); G.dp.add(dp); G.dist.add(dist); G.half.add(half); G.yawE.add(ye);
            }
            etat_info(J, O, G, eyeO, decals[k]);
        }
        // A
        g3A A[3];
        loopk(3) mesure_A(J, eyeO, y, p, okO, geos[k], A[k], k == 0);
        L.expo_s = A[0].expo_s;
        L.ep_r = A[0].episodes; L.stp_r = A[0].strict_ep; L.suit_r = A[0].suivis;
        L.ep_c = (A[1].episodes + A[2].episodes)/2.0;
        L.stp_c = (A[1].strict_ep + A[2].strict_ep)/2.0;
        L.suit_c = (A[1].suivis + A[2].suivis)/2.0;
        L.nouv_r = A[0].nouveau_s; L.nouv_c = (A[1].nouveau_s + A[2].nouveau_s)/2.0;
        L.z_ep = (L.ep_r - L.ep_c)/sqrt(L.ep_c + 1.0);
        L.z_st = (L.stp_r - L.stp_c)/sqrt(L.stp_c + 1.0);
        L.episodes = A[0].liste;
        L.nlong = 0;
        loopv(L.episodes) if(L.episodes[i].ms >= 1500 && L.episodes[i].err <= 1.0) L.nlong++;
        // F (before C: a shot during a follow counts and weighs)
        g3F F[3];
        loopk(3) mesure_F(J, eyeO, y, p, okO, geos[k], F[k], true);
        // C
        g3C C[3];
        loopk(3) if(!mesure_C(J, O, okO, geos[k], decals[k], los, C[k], k == 0, F[k].liste)) return false;
        loopk(3) F[k].pts += Q.f_shot*C[k].suivis;
        L.f_shots = C[0].suivis;
        L.c_tirs = C[0].tirs; L.c_r = C[0].caches; L.cp_r = C[0].profonds;
        L.c_c = (C[1].caches + C[2].caches)/2.0;
        L.z_c = (L.c_r - L.c_c)/sqrt(L.c_c + 1.0);
        L.cshots = C[0].liste;
        L.f_pts_r = F[0].pts; L.f_pts_c = (F[1].pts + F[2].pts)/2.0;
        L.f_n_r = F[0].n; L.f_n_c = (F[1].n + F[2].n)/2.0; L.f_s_r = F[0].secs;
        L.follows = F[0].liste;
        // B
        if(!mesure_B(J, O, eyeO, y, p, okO, geos[0], los, L.apps)) return false;
        L.rt_tous.setsize(0);
        loopv(L.apps) { const g3app &a = L.apps[i]; if(a.hasrt && a.err <= dmax(3.0, 2.0*a.half)) L.rt_tous.add(a.rt); }
        // rules (frozen on the players of the studies), never against a single enemy
        bool expo = L.expo_s >= Q.expo_min;
        L.alertA = expo && L.stp_r >= Q.rep_a && L.z_ep >= Q.z_ep && L.z_st >= Q.z_st;
        int nr = 0;
        loopv(L.rt_tous) if(L.rt_tous[i] < Q.rapide) nr++;
        L.alertB = L.rt_tous.length() >= Q.nb && nr >= Q.nb_rapide && double(nr)/L.rt_tous.length() >= Q.part_rapide;
        L.alertC = L.c_tirs >= Q.tirs_min_c && L.c_r >= Q.rep_c && L.z_c >= Q.z_c;
        L.alertAC = L.alertC || L.alertA || (expo && L.stp_r >= Q.rep_a && L.z_st >= Q.z_st && L.c_r >= Q.ac_cmin && L.z_c >= Q.ac_zc);
        L.alertF = L.f_pts_r >= Q.f_alert && L.f_pts_r >= Q.f_chance*L.f_pts_c;
        if(Q.noduel && L.duel) L.alertA = L.alertC = L.alertAC = L.alertF = false;
        // ranking: the smaller of the two distances to chance, players exposed long enough first
        L.rank = expo ? dmin(L.z_ep/1.3136286751096817, L.z_st/1.1924526655551748) : -99;
        return true;
    }

    // ------------------------------------------------------------ group 4: walls, lag, flying, jumps, noclip
    // groupe4.py and vol4.py of the fourth study, from the demo's own data:
    //  W  hits whose shot line crosses at least 1 unit of wall (a stock game shortens its shot at the first
    //     wall) on an enemy its shooter could not see (7 points of the body, the network shifted by
    //     -100 to +100 ms);
    //  L  frags right after the player's own gaps of 300 ms or more in his positions (a lag switch), against
    //     chance (the frags spread over his time alive); gaps of every player at once are the recorder's;
    //  V  in the air with a fall that never grows (the game always pulls down) for 1 s or more, far from any
    //     floor; also, from the positions only, high above any floor without coming down (information);
    //  F  jumps higher or longer than the game's gravity allows, climbing again in the air, floating;
    //  N  the body inside a wall, or passing through one between two positions.
    // The geometry comes from the game, with the map of the demo loaded: the shot's own test (raycube) and the
    // collision of a player (collide), asked to the main thread like the line of sight.

    // what group 4 asks the main thread, one answer (0 / 1) per query
    enum
    {
        GQ_SHOT = 0,   // segment a -> b: 1 clear for a shot (raycube as weapon.cpp shortens a shot)
        GQ_SEG,        // segment a -> b: 1 clear for a body (walls, glass, solid map models)
        GQ_GEO,        // point a: 1 inside the map's geometry (what shots and sight hit)
        GQ_BODY,       // feet a: bits 0, 1, 2 = solid for a player's collision at 2, 7, 13 units above
        GQ_RING,       // point a: 1 when the 4 points 2.5 units around it are all solid for a player
        GQ_PASS,       // point a: 1 when the points 3, 7 and 12 units above it are all solid for a player
        GQ_FLOOR,      // feet a: 1 when something solid for a player lies from h units under them to 0.5 above
        GQ_WATER       // feet a: 1 when water or lava is at 1 or 11.25 units above them
    };
    struct geoquery
    {
        uchar kind;
        float h;
        double a[3], b[3];
    };
    struct geoapi
    {
        virtual ~geoapi() {}
        // false when the map of the demo is not loaded (any more)
        virtual bool rungeo(const vector<geoquery> &q, vector<uchar> &res) = 0;
    };

    struct gqbatch
    {
        vector<geoquery> q;
        vector<uchar> r;
        int point(int kind, const dv3 &p, double h = 0)
        {
            geoquery &x = q.add();
            x.kind = uchar(kind); x.h = float(h);
            loopk(3) { x.a[k] = p[k]; x.b[k] = 0; }
            return q.length()-1;
        }
        int seg(int kind, const dv3 &a, const dv3 &b)
        {
            geoquery &x = q.add();
            x.kind = uchar(kind); x.h = 0;
            loopk(3) { x.a[k] = a[k]; x.b[k] = b[k]; }
            return q.length()-1;
        }
        bool run(geoapi *geo)
        {
            r.setsize(0);
            if(q.empty()) return true;
            return geo && geo->rungeo(q, r) && r.length() == q.length();
        }
        int get(int i) const { return r[i]; }
        void clear() { q.setsize(0); r.setsize(0); }
    };

    static const double G4_GAP = 300, G4_FEN = 500, G4_RETARD = 250, G4_RISE = 30, G4_MONTE_FORT = 45, G4_PLANE_FORT = 1.5;
    static const double G4_CONTACT = 8, G4_VH = 24, G4_VDROP = 20, G4_VFEN = 1000;
    static const double G4_DELTAS[5] = { -100.0, -50.0, 0.0, 50.0, 100.0 };
    static inline bool g4sol(int ph) { return ph >= 2 && ph <= 6; }

    // the study rounds some numbers as numpy does (rint(x*10^n)/10^n) and one to 4 places as Python does
    static inline double npround(double x, int n)
    {
        static const double p10[4] = { 1, 10, 100, 1000 };
        return nearbyint(x*p10[n])/p10[n];
    }
    static double pyround4(double x)
    {
        if(!finited(x)) return x;
        long double v = ((long double)x*16.0L)*625.0L;   // x*10^4, exact
        return double(nearbyintl(v))/10000.0;
    }

    // [a, b] holds a time of the sorted list
    static bool entrei(const dlist *x, double a, double b)
    {
        if(!x || x->empty()) return false;
        int i = ssleft(*x, a);
        return i < x->length() && (*x)[i] <= b;
    }

    // the positions of a player (key), sorted by time
    struct g4pos
    {
        int key;
        dlist t, fm;            // time, length of the falling vector
        vector<dv3> o;          // feet
        vector<uchar> ph, sq;   // physical state, life bit
    };

    struct g4ctx
    {
        const rawdemo *rd;
        const partie *P;
        const jeu *J;
        const mapents *ents;
        double t0, t_inter;
        bool coop;
        imap edit;
        timelists pouss, tele;
        imap specidx;
        vector<dlist> speca, specb;
        imap posidx;
        vector<g4pos> pos;
        imap gridx;                     // floor grid (cells of 8 units): where each player stood on a floor
        vector<dlist> gridz;
        vector<vector<int> > gridk;
        double zz[6001];                // height of a flat jump every ms
        int i0;
        const g4pos *posof(int k) const { int i = posidx.get(k, -1); return i >= 0 ? &pos[i] : NULL; }
    };

    static inline int g4cell(int x, int y) { return (x + 16384)*32768 + (y + 16384); }

    static bool g4local(const partie &P, int k) { return P.rec >= 0 && (k == P.rec || (P.g.ai.has(k) && P.g.aiowner.get(k, -999) == P.rec)); }

    // in the game: alive, not a spectator, after the first second and before the intermission
    static bool g4actif(const g4ctx &C, int k, double t)
    {
        if(!(t >= C.t0 + 1000 && t <= C.t_inter && C.P->vivant(k, t))) return false;
        int si = C.specidx.get(k, -1);
        if(si >= 0) loopv(C.speca[si]) if(t >= C.speca[si][i] - 500 && t <= C.specb[si][i] + 1000) return false;
        return true;
    }

    static bool g4memevie(const g4ctx &C, int k, double a, double b)
    {
        return !(entrei(C.P->morts.find(k), a - 1, b) || entrei(C.P->vies.find(k), a, b));
    }

    // somewhere a player stood on a floor, near p (another player's floor, or his own too)
    static bool g4gridfloor(const g4ctx &C, int k, const dv3 &p, double marge, bool soi)
    {
        int ix = int(floor(p.x/8)), iy = int(floor(p.y/8));
        for(int dx = -1; dx <= 1; dx++) for(int dy = -1; dy <= 1; dy++)
        {
            int gi = C.gridx.get(g4cell(ix + dx, iy + dy), -1);
            if(gi < 0) continue;
            const dlist &zs = C.gridz[gi];
            const vector<int> &ks = C.gridk[gi];
            loopv(zs) if(zs[i] >= p.z - marge - 0.5 && zs[i] <= p.z + 1.0 && (soi || ks[i] != k)) return true;
        }
        return false;
    }

    // longest time in the air of a flat jump for a net drop (ms)
    static double g4dureemax(const g4ctx &C, double chute)
    {
        for(int i = C.i0; i <= 6000; i++) if(C.zz[i] <= -chute) return (i == 6000 ? 6.0 : double(i)*(6.0/6000.0))*1000.0;
        return 6000.0;
    }

    static void g4context(const rawdemo &rd, const partie &P, const jeu &J, const mapents &ents, g4ctx &C)
    {
        C.rd = &rd; C.P = &P; C.J = &J; C.ents = &ents;
        C.t0 = J.t0;
        C.coop = P.g.mode == 1;
        double t_inter = P.t_fin + 1.0;
        imap debi;
        dlist debt;
        loopv(P.g.ev)
        {
            const devent &e = rd.ev[P.g.ev[i]];
            double t = e.t;
            int c = evkey(P.g, e);
            switch(e.type)
            {
                case E_TIMEUP: if(e.a <= 0 && t > C.t0 + 1000) t_inter = dmin(t_inter, t); break;
                case E_EDITMODE: if(e.a) C.edit.set(c, 1); break;
                case E_TELEPORT: case E_JUMPPAD: C.pouss.get(c).add(t); C.tele.get(c).add(t); break;
                case E_SPAWN: case E_SPAWNSTATE: C.pouss.get(c).add(t); break;
                case E_DAMAGE: C.pouss.get(c).add(t); break;
                case E_SHOT:
                {
                    // the recoil of his own weapon pushes him up only when he shoots down
                    dv3 u(e.to[0] - e.fr[0], e.to[1] - e.fr[1], e.to[2] - e.fr[2]);
                    double nn = u.norm(), uz = nn > 1e-6 ? u.z/nn : 0.0;
                    if(e.a >= 0 && e.a < 7 && RECUL[e.a]*dmax(0.0, -uz) > 10.0) C.pouss.get(c).add(t);
                    break;
                }
                case E_EXPLODE: C.pouss.get(c).add(t); break;
                case E_SPECTATOR:
                {
                    int di = debi.get(c, -1);
                    bool open = di >= 0 && !isnand(debt[di]);
                    if(e.a && !open)
                    {
                        if(di < 0) { di = debt.length(); debt.add(t); debi.set(c, di); }
                        else debt[di] = t;
                    }
                    else if(!e.a && open)
                    {
                        int si = C.specidx.get(c, -1);
                        if(si < 0) { si = C.speca.length(); C.speca.add(); C.specb.add(); C.specidx.set(c, si); }
                        C.speca[si].add(debt[di]); C.specb[si].add(t);
                        debt[di] = DNAN;
                    }
                    break;
                }
            }
        }
        loopv(debi.k)
        {
            double a = debt[debi.v[i]];
            if(isnand(a)) continue;
            int c = debi.k[i], si = C.specidx.get(c, -1);
            if(si < 0) { si = C.speca.length(); C.speca.add(); C.specb.add(); C.specidx.set(c, si); }
            C.speca[si].add(a); C.specb[si].add(P.t_fin + 1.0);
        }
        C.t_inter = t_inter;
        C.pouss.sortall(); C.tele.sortall();
        // positions of every key (t <= end of the game), sorted by time
        const gamebase &g = P.g;
        imap li;
        vector<vector<int> > lists;
        loopv(g.d.pos)
        {
            const dposx &p = rd.pos[g.d.pos[i]];
            if(p.t > g.t_fin) continue;
            int key = g.cle(p.cn, p.t);
            int j = li.get(key, -1);
            if(j < 0) { j = lists.length(); lists.add(); li.set(key, j); }
            lists[j].add(g.d.pos[i]);
        }
        loopv(li.k)
        {
            const vector<int> &L = lists[li.v[i]];
            dlist t;
            loopvj(L) t.add(rd.pos[L[j]].t);
            vector<int> o;
            stableargsort(t.getbuf(), t.length(), o);
            g4pos &q = C.pos.add();
            q.key = li.k[i];
            loopvj(o)
            {
                const dposx &p = rd.pos[L[o[j]]];
                q.t.add(p.t);
                q.o.add(dv3(p.o[0], p.o[1], p.o[2]));
                q.ph.add(p.phys);
                q.sq.add(p.seq);
                q.fm.add(sqrt((p.fvec[0]*p.fvec[0] + p.fvec[1]*p.fvec[1]) + p.fvec[2]*p.fvec[2]));
            }
            C.posidx.set(q.key, C.pos.length()-1);
            // floors where this player stood (real floors, map models included)
            loopvj(q.t) if(g4sol(q.ph[j]))
            {
                int cell = g4cell(int(floor(q.o[j].x/8)), int(floor(q.o[j].y/8)));
                int gi = C.gridx.get(cell, -1);
                if(gi < 0) { gi = C.gridz.length(); C.gridz.add(); C.gridk.add(); C.gridx.set(cell, gi); }
                double zr = nearbyint(q.o[j].z*2)/2.0;
                dlist &zs = C.gridz[gi];
                vector<int> &ks = C.gridk[gi];
                bool dup = false;
                for(int m = zs.length()-1; m >= 0 && m >= zs.length()-8; m--) if(zs[m] == zr && ks[m] == q.key) { dup = true; break; }
                if(!dup) { zs.add(zr); ks.add(q.key); }
            }
        }
        // flat jump: vel.z = 125 damped by the air (friction 30 every 20 ms), gravity 200 (physics.cpp)
        const double K = log(30.0/29.0)/0.020;
        C.i0 = 0;
        loopi(6001)
        {
            double tt = i == 6000 ? 6.0 : double(i)*(6.0/6000.0);
            C.zz[i] = 125.0/K*(1.0 - exp(-K*tt)) - 0.5*200.0*tt*tt;
            if(C.zz[i] > C.zz[C.i0]) C.i0 = i;
        }
    }

    // ---- W
    struct g4wallhit { double t, dist; int victim, gun; bool seg, hidden, wall; };
    struct g4W
    {
        int tirs, touches, rates, segt, segr, cachet, murs;
        vector<g4wallhit> list;
    };

    // ---- L
    struct g4gap { double t, ms, jump; int frags; };
    struct g4L
    {
        int note;   // 0 measured, 1 the recorder of a local demo or his bots, 2 too few positions
        double vivant_s, trou_s, trou_max, saut_max, attendu, frac, z, rt_max;
        int trous, communs, propres, frags, frags_trous, dec[2], rt_tirs, rt_retenus, rt_rtouches, rt_touches;
        bool rt;
        vector<g4gap> list;
    };

    // ---- F
    enum { G4F_RISE = 0, G4F_CLIMB, G4F_LONG, G4F_FLOAT };
    struct g4fev { int kind; double t, v1, v2, v3; };
    struct g4F
    {
        int note;   // 0 measured, 1 edit, 2 too few positions
        int monte_fort, plane_fort, float_ep, float_local, echantillons, float_n, segments, mesures, appuis;
        int ex_appui, ex_poussee, ex_eau, monte, remonte, plane;
        double float_s, haut_max;
        dlist rises, ratios;
        vector<g4fev> ev;
    };

    // ---- N
    struct g4nev { double t, v1, v2; dv3 o; bool shared; };
    struct g4N
    {
        int note;
        int echantillons, dedans_n, episodes, ep_max, traversees, ep_propres, tr_propres;
        double dedans_s;
        vector<g4nev> eps, trav;
    };

    // ---- V
    struct g4vep { double t, s; };
    struct g4V
    {
        int note;
        double v1_s, v2_s, air_haut_s;
        int v1_n, v2_n;
        vector<g4vep> v1, v2;
    };

    struct g4line
    {
        int key, npos, ntirs;
        bool bot, local, geo;     // geo: walls, flying, jumps and noclip measured (the map was loaded)
        g4W W;
        g4L L;
        g4F F;
        g4N N;
        g4V V;
    };

    // what one player needs between the rounds of questions to the main thread
    struct g4work
    {
        g4line *out;
        const g4pos *p;
        int n;
        vector<char> act, actN, eau, dedans;
        // W
        vector<int> widx;
        vector<dv3> wA, wB;
        vector<char> whit, wseg, wcache;
        vector<int> wq, wsq, wsn;
        vector<int> hj, hq0, hn;   // hits measured: shot j, first sight query, number of deltas measured
        // N
        vector<int> nbody, nring0;
        vector<uchar> nbits;
        vector<int> ncand, nsegq, nsamp0, nsampn;
        // F
        struct seg_t { int i, j; vector<int> mins, resets; vector<int> qmin, qres; vector<char> gmin, gres; };
        vector<seg_t> fsegs;
        // V
        vector<char> vsol, vexc, vair;
        vector<int> v1w, v1q, v2w, v2q;
        vector<char> v1g, v2g;
        int qwater0;
    };

    static void g4runs(const vector<char> &m, vector<int> &a, vector<int> &b)
    {
        a.setsize(0); b.setsize(0);
        int n = m.length();
        for(int i = 0; i < n;)
        {
            if(!m[i]) { i++; continue; }
            int j = i;
            while(j < n && m[j]) j++;
            a.add(i); b.add(j);
            i = j;
        }
    }

    // runs of true samples close in time: [first, last]
    static void g4runst(const dlist &t, const vector<char> &m, double gap, vector<int> &a, vector<int> &b)
    {
        a.setsize(0); b.setsize(0);
        int n = m.length();
        for(int i = 0; i < n;)
        {
            if(!m[i]) { i++; continue; }
            int j = i;
            while(j + 1 < n && m[j+1] && t[j+1] - t[j] <= gap) j++;
            a.add(i); b.add(j);
            i = j + 1;
        }
    }

    // ---- L: gaps in his positions while the others' arrive, frags right after
    static void g4lag(const g4ctx &C, int k, g4L &r)
    {
        const partie &P = *C.P;
        memset(r.dec, 0, sizeof(r.dec));
        r.note = 0; r.trous = r.communs = r.propres = r.frags = r.frags_trous = r.rt_tirs = r.rt_retenus = r.rt_rtouches = r.rt_touches = 0;
        r.vivant_s = r.trou_s = r.trou_max = r.saut_max = r.attendu = r.frac = r.z = r.rt_max = 0;
        r.rt = false;
        if(g4local(P, k)) { r.note = 1; return; }
        const g4pos *p = C.posof(k);
        if(!p || p->t.length() < 20) { r.note = 2; return; }
        const dlist &t = p->t;
        int n = t.length();
        dlist tq;
        nparange(C.t0 + 2000.0, C.t_inter, 50.0, tq);
        int nviv = 0;
        loopv(tq) if(P.vivant(k, tq[i]) && tq[i] >= t[0] && tq[i] <= t[n-1]) nviv++;
        double vivant_s = double(nviv)*0.05;
        // gaps: the others (distant players alive) sent positions meanwhile, else the gap is everybody's
        struct gap_t { double a, b, s; bool commun; };
        vector<gap_t> tr;
        for(int i = 0; i+1 < n; i++)
        {
            if(!(t[i+1] - t[i] >= G4_GAP)) continue;
            double a = t[i], b = t[i+1];
            if(a < C.t0 + 2000 || b > C.t_inter) continue;
            if(!g4memevie(C, k, a, b)) continue;
            if(p->sq[i] != p->sq[i+1]) continue;
            if(!(g4actif(C, k, a) && g4actif(C, k, 0.5*(a + b)) && g4actif(C, k, b))) continue;
            int n_ref = 0, n_ok = 0;
            loopvj(C.pos)
            {
                const g4pos &q = C.pos[j];
                if(q.key == k || q.t.length() < 20 || g4local(P, q.key)) continue;
                if(!P.vivant(q.key, 0.5*(a + b))) continue;
                n_ref++;
                int m = ssleft(q.t, a + 50);
                if(m < q.t.length() && q.t[m] <= b - 50) n_ok++;
            }
            gap_t &x = tr.add();
            x.a = a; x.b = b; x.s = (p->o[i+1] - p->o[i]).norm(); x.commun = n_ref == 0 || n_ok < 0.5*n_ref;
        }
        vector<gap_t> propres;
        loopv(tr) if(!tr[i].commun) propres.add(tr[i]);
        dlist fr_t;
        loopv(P.morts_ev)
        {
            const mortev &m = P.morts_ev[i];
            if(m.actor == k && m.victim != k && !P.meme_equipe(k, m.victim, m.t) && C.t0 + 2000 <= m.t && m.t <= C.t_inter) fr_t.add(m.t);
        }
        fr_t.sort(dless);
        dlist fa, fb;
        loopv(propres) { fa.add(propres[i].a); fb.add(propres[i].b + G4_FEN); }
        auto dans = [&](const dlist &A, const dlist &B) -> int
        {
            int c = 0;
            loopv(A) loopvj(fr_t) if(fr_t[j] >= A[i] && fr_t[j] <= B[i]) c++;
            return c;
        };
        int obs = dans(fa, fb);
        double duree = 0;
        loopv(fa) duree += dmin(fb[i], C.t_inter) - fa[i];
        double frac = dmin(1.0, duree/1000.0/dmax(vivant_s, 1.0));
        double att = fr_t.length()*frac;
        double t_a = C.t0 + 2000.0, t_b = C.t_inter;
        const double ss[2] = { 20000.0, -20000.0 };
        loopk(2)
        {
            dlist A2, B2;
            loopv(fa) { double a2 = t_a + pymod(fa[i] - t_a + ss[k], t_b - t_a); A2.add(a2); B2.add(a2 + (fb[i] - fa[i])); }
            r.dec[k] = dans(A2, B2);
        }
        // shots held back (arrival late against the shooter's own clock)
        const vector<p2shot> *tirs = P.shots(k);
        if(tirs && tirs->length() >= 10)
        {
            int m = tirs->length();
            dlist ts, o;
            loopi(m) { ts.add((*tirs)[i].t); o.add((*tirs)[i].t - (*tirs)[i].id); }
            dlist retard;
            loopi(m)
            {
                dlist w;
                loopj(m) if(ts[j] >= ts[i] - 20000 && ts[j] <= ts[i] + 20000) w.add(o[j]);
                retard.add(o[i] - npmedian(w));
            }
            bool any = false;
            double mx = 0;
            loopi(m)
            {
                if(!(ts[i] >= C.t0 + 2000 && ts[i] <= C.t_inter)) continue;
                bool hit = C.J->touches.has(k, i);
                r.rt_tirs++;
                if(hit) r.rt_touches++;
                if(retard[i] >= G4_RETARD) { r.rt_retenus++; if(hit) r.rt_rtouches++; }
                mx = any ? dmax(mx, retard[i]) : retard[i];
                any = true;
            }
            r.rt_max = pyround(any ? mx : 0.0, 0);
            r.rt = true;
        }
        loopv(propres)
        {
            int nf = 0;
            loopvj(fr_t) if(fr_t[j] >= propres[i].a && fr_t[j] <= propres[i].b + G4_FEN) nf++;
            if(nf) { g4gap &g = r.list.add(); g.t = propres[i].a; g.ms = pyround(propres[i].b - propres[i].a, 0); g.jump = pyround(propres[i].s, 0); g.frags = nf; }
        }
        r.vivant_s = pyround(vivant_s, 1);
        r.trous = tr.length(); r.propres = propres.length(); r.communs = tr.length() - propres.length();
        double ts_ = 0, tm = 0, sm = 0;
        loopv(propres) { ts_ += propres[i].b - propres[i].a; tm = dmax(tm, propres[i].b - propres[i].a); sm = dmax(sm, propres[i].s); }
        r.trou_s = pyround(ts_/1000.0, 2); r.trou_max = pyround(tm, 0); r.saut_max = pyround(sm, 0);
        r.frags = fr_t.length(); r.frags_trous = obs;
        r.attendu = pyround(att, 3); r.frac = pyround4(frac);
        r.z = pyround((obs - att)/sqrt(att + 1.0), 3);
    }

    // ---- W, first round: the shot line of every shot of the game (hits: up to 1 unit before the body,
    // misses: up to 2 units before the wall)
    static void g4walls1(const g4ctx &C, int k, g4work &w, gqbatch &b)
    {
        const partie &P = *C.P;
        g4W &r = w.out->W;
        r.tirs = r.touches = r.rates = r.segt = r.segr = r.cachet = r.murs = 0;
        const vector<p2shot> *tirs = P.shots(k);
        if(!tirs) return;
        loopv(*tirs)
        {
            const p2shot &s = (*tirs)[i];
            if(s.t < C.t0 + 1000 || s.t > C.t_inter) continue;
            dv3 u = s.to - s.fr;
            double L = u.norm();
            if(L < 12.0) continue;
            u = u/L;
            bool hit = C.J->touches.has(k, i);
            w.widx.add(i);
            w.wA.add(s.fr);
            w.wB.add(s.to - u*(hit ? 1.0 : 2.0));
            w.whit.add(hit);
            w.wq.add(b.seg(GQ_SHOT, s.fr, w.wB.last()));
        }
    }

    // second round: the walls crossed (1 unit at least, a sample every 0.25 unit); the sight of the victims
    static void g4walls2(const g4ctx &C, int k, g4work &w, const gqbatch &b1, gqbatch &b2, vector<losquery> &lq)
    {
        const partie &P = *C.P;
        int m = w.widx.length();
        loopi(m)
        {
            w.wsq.add(-1); w.wsn.add(0);
            if(b1.get(w.wq[i])) continue;
            dv3 a = w.wA[i], u = w.wB[i] - a;
            double L = u.norm();
            if(L < 2.0) continue;
            dv3 ul = u/L;
            dlist s;
            nparange(1.0, L, 0.25, s);
            w.wsq[i] = b2.q.length(); w.wsn[i] = s.length();
            loopvj(s) b2.point(GQ_GEO, dv3(a.x + ul.x*s[j], a.y + ul.y*s[j], a.z + ul.z*s[j]));
        }
        // the victim as the shooter saw it: hidden at the 7 points for every shift of the network
        if(!m) return;
        const vector<p2shot> &tirs = *P.shots(k);
        loopi(m)
        {
            if(!w.whit[i]) continue;
            int si = w.widx[i], v = C.J->touches.get(k, si);
            const p2shot &s = tirs[si];
            const piste *pe = C.J->piste_E(k, v);
            if(!pe) continue;
            double D = C.J->D(k, v);
            int q0 = lq.length(), nq = 0;
            loopj(5)
            {
                double tE = s.t - D + G4_DELTAS[j];
                if(!(tE >= pe->t[0] && tE <= pe->t[pe->n()-1] && pe->trou(tE) <= 250)) continue;
                bodyquery(s.fr, pe->eye_at(tE), lq.add());
                nq++;
            }
            if(!nq) continue;
            w.hj.add(i); w.hq0.add(q0); w.hn.add(nq);
        }
    }

    static void g4walls3(const g4ctx &C, int k, g4work &w, const gqbatch &b2, const vector<uchar> &lr)
    {
        const partie &P = *C.P;
        g4W &r = w.out->W;
        int m = w.widx.length();
        w.wseg.setsize(0); w.wcache.setsize(0);
        loopi(m)
        {
            bool seg = false;
            if(w.wsq[i] >= 0)
            {
                int best = 0, cur = 0;
                loopj(w.wsn[i]) { if(b2.get(w.wsq[i] + j)) cur++; else cur = 0; if(cur > best) best = cur; }
                seg = best*0.25 >= 1.0;
            }
            w.wseg.add(seg);
            w.wcache.add(0);
        }
        loopv(w.hj)
        {
            int vis = 0;
            loopj(w.hn[i]) if(lr[w.hq0[i] + j]) vis++;
            w.wcache[w.hj[i]] = w.hn[i] >= 3 && !vis;
        }
        r.tirs = m;
        const vector<p2shot> *tirs = P.shots(k);
        loopi(m)
        {
            bool hit = w.whit[i] != 0, seg = w.wseg[i] != 0, cache = w.wcache[i] != 0;
            if(hit) r.touches++; else r.rates++;
            if(seg && hit) r.segt++;
            if(seg && !hit) r.segr++;
            if(cache && hit) r.cachet++;
            bool mur = seg && cache && hit;
            if(mur) r.murs++;
            if(mur || (seg && hit))
            {
                const p2shot &s = (*tirs)[w.widx[i]];
                g4wallhit &h = r.list.add();
                h.t = s.t; h.victim = C.J->touches.get(k, w.widx[i]); h.gun = s.gun;
                h.dist = pyround((s.to - s.fr).norm(), 0);
                h.seg = seg; h.hidden = cache; h.wall = mur;
            }
        }
    }

    // ---- shared first round: water or lava at the feet (jumps, flying); the body at 3 heights (noclip)
    static void g4body1(const g4ctx &C, int k, g4work &w, gqbatch &b)
    {
        const g4pos &p = *w.p;
        int n = w.n;
        w.act.setsize(0);
        loopi(n) w.act.add(g4actif(C, k, p.t[i]));
        w.qwater0 = b.q.length();
        loopi(n) b.point(GQ_WATER, p.o[i]);
        // noclip: positions in the game (the recorder's own edit mode, FLOAT, is not in the demo: kept out)
        w.actN = w.act;
        if(g4local(*C.P, k))
        {
            dlist tf;
            loopi(n) if(w.act[i] && p.ph[i] == 0) tf.add(p.t[i]);
            if(tf.length())
            {
                loopi(n)
                {
                    int j = ssleft(tf, p.t[i]);
                    double a = fabs(p.t[i] - tf[clamp(j-1, 0, tf.length()-1)]), c = fabs(p.t[i] - tf[clamp(j, 0, tf.length()-1)]);
                    if(!(dmin(a, c) > 1500)) w.actN[i] = 0;
                }
            }
        }
        w.nbody.setsize(0);
        loopi(n) w.nbody.add(w.actN[i] ? b.point(GQ_BODY, p.o[i]) : -1);
        w.eau.setsize(0);
    }

    static const double G4_HAUTS[3] = { 2.0, 7.0, 13.0 };

    // ---- F, second round: the jumps measured, the floors where a landing may have been missed
    static void g4jumps2(const g4ctx &C, int k, g4work &w, gqbatch &b)
    {
        const partie &P = *C.P;
        const g4pos &p = *w.p;
        g4F &r = w.out->F;
        int n = w.n;
        r.monte_fort = r.plane_fort = r.float_ep = r.float_local = r.echantillons = r.float_n = r.segments = r.mesures = r.appuis = 0;
        r.ex_appui = r.ex_poussee = r.ex_eau = r.monte = r.remonte = r.plane = 0;
        r.float_s = 0; r.haut_max = DNAN;
        r.note = 0;
        if(C.coop || C.edit.has(k)) { r.note = 1; return; }
        vector<char> act = w.act;
        if(g4local(P, k))
        {
            // the recorder of a local demo: FLOAT is his edit mode, not written in the demo (kept out)
            dlist tf;
            loopi(n) if(act[i] && p.ph[i] == 0) tf.add(p.t[i]);
            r.float_local = tf.length();
            if(tf.length())
            {
                loopi(n)
                {
                    int j = ssleft(tf, p.t[i]);
                    double a = fabs(p.t[i] - tf[clamp(j-1, 0, tf.length()-1)]), c = fabs(p.t[i] - tf[clamp(j, 0, tf.length()-1)]);
                    if(!(dmin(a, c) > 1500)) act[i] = 0;
                }
            }
        }
        dlist t_eau;
        loopi(n) if(w.eau[i]) t_eau.add(p.t[i]);
        dlist pe;
        const dlist *pl = C.pouss.find(k);
        if(pl) pe = *pl;
        if(C.ents->ok)
        {
            loopi(n) if(pres(p.o[i], C.ents->pad, 40.0) || pres(p.o[i], C.ents->tele, 40.0)) pe.add(p.t[i]);
            pe.sort(dless);
        }
        w.fsegs.setsize(0);
        vector<char> air;
        loopi(n) air.add(p.ph[i] == 1 && act[i]);
        for(int i = 0; i < n;)
        {
            if(!air[i]) { i++; continue; }
            int j = i;
            while(j + 1 < n && air[j+1] && p.t[j+1] - p.t[j] <= 250 && p.sq[j+1] == p.sq[i]) j++;
            r.segments++;
            int a = i, z = j;
            i = j + 1;
            if(z - a < 2) continue;
            double ta = p.t[a], tb = p.t[z];
            if(a == 0 || !g4sol(p.ph[a-1]) || p.t[a] - p.t[a-1] > 250 || p.sq[a-1] != p.sq[a]) { r.ex_appui++; continue; }
            if(entrei(&pe, ta - 500, tb + 50)) { r.ex_poussee++; continue; }
            if(t_eau.length() && entrei(&t_eau, ta - 1000, tb + 1000)) { r.ex_eau++; continue; }
            r.mesures++;
            g4work::seg_t &s = w.fsegs.add();
            s.i = a; s.j = z;
            int len = z - a + 1;
            for(int q = 1; q < len - 1; q++) if(p.o[a+q].z <= p.o[a+q-1].z && p.o[a+q].z <= p.o[a+q+1].z) s.mins.add(q);
            for(int q = 1; q < len; q++) if(p.fm[a+q-1] >= 15.0 && p.fm[a+q] <= 10.0) s.resets.add(q);
            // a floor under the lowest points (8 units) or where the fall stopped growing (16 units): the floor
            // where a player stood first (his own included), else the map
            loopvj(s.mins)
            {
                bool g = g4gridfloor(C, k, p.o[a + s.mins[j]], G4_CONTACT, true);
                s.gmin.add(g);
                s.qmin.add(g ? -1 : b.point(GQ_FLOOR, p.o[a + s.mins[j]], G4_CONTACT));
            }
            loopvj(s.resets)
            {
                bool g = g4gridfloor(C, k, p.o[a + s.resets[j]], 16.0, true);
                s.gres.add(g);
                s.qres.add(g ? -1 : b.point(GQ_FLOOR, p.o[a + s.resets[j]], 16.0));
            }
        }
        // floating (FLOAT is only for edit mode and spectators)
        vector<char> fl;
        loopi(n) { fl.add(act[i] && p.ph[i] == 0); if(act[i]) r.echantillons++; if(fl[i]) r.float_n++; }
        r.float_s = pyround(r.float_n*0.033, 2);
        vector<int> ra, rb;
        g4runs(fl, ra, rb);
        loopv(ra) if(rb[i] - ra[i] >= 3)
        {
            r.float_ep++;
            if(!w.out->local) { g4fev &e = r.ev.add(); e.kind = G4F_FLOAT; e.t = p.t[ra[i]]; e.v1 = (p.t[rb[i]-1] - p.t[ra[i]])/1000.0; e.v2 = e.v3 = 0; }
        }
    }

    static void g4jumps3(const g4ctx &C, int k, g4work &w, const gqbatch &b)
    {
        const g4pos &p = *w.p;
        g4F &r = w.out->F;
        if(r.note) return;
        int n = w.n;
        double hmax = -1e300;
        bool anyrise = false;
        loopv(w.fsegs)
        {
            const g4work::seg_t &s = w.fsegs[i];
            int i0 = s.i, j0 = s.j, len = j0 - i0 + 1;
            vector<int> cont;
            loopvj(s.mins) if(s.gmin[j] || b.get(s.qmin[j])) cont.add(s.mins[j]);
            loopvj(s.resets) if(s.gres[j] || b.get(s.qres[j])) cont.add(s.resets[j]);
            cont.sort();
            vector<int> cu;
            loopvj(cont) if(!cu.length() || cu.last() != cont[j]) cu.add(cont[j]);
            r.appuis += cu.length();
            vector<int> coup;
            coup.add(-1);
            loopvj(cu) coup.add(cu[j]);
            coup.add(len);
            for(int c = 0; c+1 < coup.length(); c++)
            {
                int a_ = coup[c], b_ = coup[c+1];
                int g0 = a_ < 0 ? i0 - 1 : i0 + a_;
                int q0 = a_ >= 0 ? a_ + 1 : 0, q1 = b_ < len ? b_ : len;
                if(q1 <= q0) continue;
                double z0 = p.o[g0].z, zmax = -1e300;
                for(int q = q0; q < q1; q++) zmax = dmax(zmax, p.o[i0+q].z);
                double rise = zmax - z0, rr = pyround(rise, 1);
                r.rises.add(rr);
                if(!anyrise || rr > hmax) hmax = rr;
                anyrise = true;
                if(rise > G4_RISE)
                {
                    r.monte++;
                    g4fev &e = r.ev.add(); e.kind = G4F_RISE; e.t = p.t[i0+q0]; e.v1 = pyround(rise, 1); e.v2 = pyround(p.t[i0+q1-1] - p.t[i0+q0], 0); e.v3 = 0;
                    if(e.v1 > G4_MONTE_FORT) r.monte_fort++;
                }
                // climbs again: goes down 4 units then up 8 with no floor between
                double zs = -1e300, zm = 1e300;
                int found = -1;
                for(int q = q0; q < q1; q++)
                {
                    double zq = p.o[i0+q].z;
                    if(zq > zs) { zs = zq; zm = 1e300; }
                    if(zs - zq >= 4.0 && zq < zm) zm = zq;
                    if(zm < 1e300 && zq - zm >= 8.0) { found = q; break; }
                }
                if(found >= 0)
                {
                    r.remonte++;
                    g4fev &e = r.ev.add(); e.kind = G4F_CLIMB; e.t = p.t[i0+found]; e.v1 = pyround(p.o[i0+found].z - zm, 1); e.v2 = e.v3 = 0;
                }
                // time between two floors against the net drop
                int g1;
                bool land_ok;
                if(b_ < len) { g1 = i0 + b_; land_ok = true; }
                else { g1 = j0 + 1; land_ok = j0 + 1 < n && g4sol(p.ph[j0+1]) && p.t[j0+1] - p.t[j0] <= 250; }
                if(land_ok)
                {
                    double chute = z0 - p.o[g1].z;
                    double dmx = g4dureemax(C, chute) + 250.0;
                    double dur = p.t[g1] - p.t[g0];
                    r.ratios.add(npround(dur/dmax(dmx - 250.0, 1.0), 3));
                    if(dur > dmx)
                    {
                        r.plane++;
                        double ms = pyround(dur, 0), mx = pyround(dmx, 0);
                        bool fort = ms > G4_PLANE_FORT*(mx - 250.0);
                        if(fort)
                        {
                            r.plane_fort++;
                            g4fev &e = r.ev.add(); e.kind = G4F_LONG; e.t = p.t[g0]; e.v1 = ms; e.v2 = mx; e.v3 = pyround(chute, 1);
                        }
                    }
                }
            }
        }
        r.haut_max = anyrise ? hmax : DNAN;
    }

    // ---- N, second round: around the body heights found inside (2 of 3 needed)
    static void g4noclip2(const g4ctx &C, int k, g4work &w, const gqbatch &b1, gqbatch &b2)
    {
        const g4pos &p = *w.p;
        int n = w.n;
        w.nbits.setsize(0); w.nring0.setsize(0);
        loopi(n)
        {
            uchar bits = w.nbody[i] >= 0 ? uchar(b1.get(w.nbody[i])) : 0;
            w.nbits.add(bits);
            int c = (bits&1) + ((bits>>1)&1) + ((bits>>2)&1);
            if(c < 2) { w.nring0.add(-1); continue; }
            w.nring0.add(b2.q.length());
            loopj(3) if(bits&(1<<j)) b2.point(GQ_RING, p.o[i] + dv3(0, 0, G4_HAUTS[j]));
        }
    }

    // third round: positions in a row on both sides of a wall
    static void g4noclip3(const g4ctx &C, int k, g4work &w, const gqbatch &b2, gqbatch &b3)
    {
        const g4pos &p = *w.p;
        int n = w.n;
        w.dedans.setsize(0);
        loopi(n)
        {
            int c = 0;
            if(w.nring0[i] >= 0)
            {
                int q = w.nring0[i];
                loopj(3) if(w.nbits[i]&(1<<j)) { if(b2.get(q)) c++; q++; }
            }
            w.dedans.add(c >= 2);
        }
        const dlist *tp = C.tele.find(k);
        vector<char> pres_tp;
        loopi(n) pres_tp.add(C.ents->ok && (pres(p.o[i], C.ents->tele, 40.0) || pres(p.o[i], C.ents->dest, 40.0)));
        w.ncand.setsize(0); w.nsegq.setsize(0);
        for(int c = 0; c+1 < n; c++)
        {
            if(!(w.actN[c] && w.actN[c+1] && p.t[c+1] - p.t[c] <= 150 && p.sq[c] == p.sq[c+1] && !w.dedans[c] && !w.dedans[c+1] && !pres_tp[c] && !pres_tp[c+1])) continue;
            if(entrei(tp, p.t[c] - 300, p.t[c+1] + 300) || !g4memevie(C, k, p.t[c], p.t[c+1])) continue;
            double d = (p.o[c+1] - p.o[c]).norm();
            if(!(d >= 1.0 && d <= 48.0)) continue;
            w.ncand.add(c);
            w.nsegq.add(b3.seg(GQ_SEG, p.o[c] + dv3(0, 0, 7.0), p.o[c+1] + dv3(0, 0, 7.0)));
        }
    }

    // fourth round: the samples along the blocked ones (every 0.5 unit, at 3, 7 and 12 units above)
    static void g4noclip4(const g4ctx &C, int k, g4work &w, const gqbatch &b3, gqbatch &b4)
    {
        const g4pos &p = *w.p;
        w.nsamp0.setsize(0); w.nsampn.setsize(0);
        loopv(w.ncand)
        {
            if(b3.get(w.nsegq[i])) { w.nsamp0.add(-1); w.nsampn.add(0); continue; }
            int c = w.ncand[i];
            dv3 a = p.o[c], d = p.o[c+1] - a;
            double L = d.norm();
            dv3 ul = d/dmax(L, 1e-6);
            dlist s;
            nparange(0.0, L + 1e-6, 0.5, s);
            w.nsamp0.add(b4.q.length()); w.nsampn.add(s.length());
            loopvj(s) b4.point(GQ_PASS, dv3(a.x + ul.x*s[j], a.y + ul.y*s[j], a.z + ul.z*s[j]));
        }
    }

    static void g4noclip5(const g4ctx &C, int k, g4work &w, const gqbatch &b4)
    {
        const g4pos &p = *w.p;
        g4N &r = w.out->N;
        int n = w.n;
        r.echantillons = r.dedans_n = 0;
        loopi(n) { if(w.actN[i]) r.echantillons++; if(w.dedans[i]) r.dedans_n++; }
        double ds = 0;
        loopi(n) if(w.dedans[i]) ds += dmin(i+1 < n ? p.t[i+1] - p.t[i] : 33.0, 100.0);
        r.dedans_s = pyround(ds/1000.0, 2);
        vector<int> ra, rb;
        g4runs(w.dedans, ra, rb);
        r.ep_max = 0;
        loopv(ra) if(rb[i] - ra[i] >= 2)
        {
            g4nev &e = r.eps.add();
            e.t = p.t[ra[i]]; e.v1 = rb[i] - ra[i]; e.v2 = pyround(p.t[rb[i]-1] - p.t[ra[i]], 0);
            e.o = dv3(pyround(p.o[ra[i]].x, 1), pyround(p.o[ra[i]].y, 1), pyround(p.o[ra[i]].z, 1));
            e.shared = false;
            r.ep_max = max(r.ep_max, rb[i] - ra[i]);
        }
        loopv(w.ncand)
        {
            if(w.nsamp0[i] < 0) continue;
            int best = 0, cur = 0;
            loopj(w.nsampn[i]) { if(b4.get(w.nsamp0[i] + j)) cur++; else cur = 0; best = max(best, cur); }
            if(best*0.5 < 1.0) continue;
            int c = w.ncand[i];
            g4nev &e = r.trav.add();
            e.t = p.t[c]; e.v1 = pyround((p.o[c+1] - p.o[c]).norm(), 1); e.v2 = pyround(best*0.5, 1);
            e.o = dv3(pyround(p.o[c].x, 1), pyround(p.o[c].y, 1), pyround(p.o[c].z, 1));
            e.shared = false;
        }
        r.episodes = r.eps.length(); r.traversees = r.trav.length();
    }

    // ---- V, second round: in the air with no fall far from a floor (V1); high above any floor (V2)
    static void g4fly2(const g4ctx &C, int k, g4work &w, gqbatch &b)
    {
        const partie &P = *C.P;
        const g4pos &p = *w.p;
        g4V &r = w.out->V;
        int n = w.n;
        r.note = 0; r.v1_s = r.v2_s = r.air_haut_s = 0; r.v1_n = r.v2_n = 0;
        w.v1w.setsize(0); w.v1q.setsize(0); w.v1g.setsize(0); w.v2w.setsize(0); w.v2q.setsize(0); w.v2g.setsize(0);
        if(C.coop || C.edit.has(k)) { r.note = 1; return; }
        vector<char> act = w.act;
        if(g4local(P, k)) loopi(n) if(p.ph[i] == 0) act[i] = 0;
        dlist te;
        loopi(n) if(w.eau[i]) te.add(p.t[i]);
        if(te.length())
        {
            loopi(n)
            {
                int j = ssleft(te, p.t[i]);
                double a = fabs(p.t[i] - te[clamp(j-1, 0, te.length()-1)]), c = fabs(p.t[i] - te[clamp(j, 0, te.length()-1)]);
                if(!(dmin(a, c) > 1000)) act[i] = 0;
            }
        }
        w.vsol.setsize(0);
        loopi(n) w.vsol.add(g4sol(p.ph[i]));
        // V1 candidates: in the air, alive, out of water, falling slower than 5 units/s; far (8 units) from any
        // floor (another player's, or the map's)
        loopi(n) if(act[i] && p.ph[i] == 1 && p.fm[i] < 5.0)
        {
            bool g = g4gridfloor(C, k, p.o[i], G4_CONTACT, false);
            w.v1w.add(i); w.v1g.add(g);
            w.v1q.add(g ? -1 : b.point(GQ_FLOOR, p.o[i], G4_CONTACT));
        }
        // V2: excused after a push until the floor, a fall of 20 units from the highest point or 3 s
        dlist pe;
        const dlist *pl = C.pouss.find(k);
        if(pl) pe = *pl;
        if(C.ents->ok)
        {
            loopi(n) if(pres(p.o[i], C.ents->pad, 40.0) || pres(p.o[i], C.ents->tele, 40.0)) pe.add(p.t[i]);
            pe.sort(dless);
        }
        w.vexc.setsize(0);
        bool ex = false;
        double zmax = -1e9, tpu = -1e18;
        int ip = 0;
        loopi(n)
        {
            while(ip < pe.length() && pe[ip] <= p.t[i]) { ex = true; zmax = p.o[i].z; tpu = pe[ip]; ip++; }
            if(ex)
            {
                zmax = dmax(zmax, p.o[i].z);
                if(w.vsol[i] || p.o[i].z < zmax - G4_VDROP || p.t[i] - tpu > 3000.0) ex = false;
            }
            w.vexc.add(ex);
        }
        w.vair.setsize(0);
        loopi(n) w.vair.add(act[i] && !w.vsol[i] && !w.vexc[i]);
        loopi(n) if(w.vair[i])
        {
            bool g = g4gridfloor(C, k, p.o[i], G4_VH, false);
            w.v2w.add(i); w.v2g.add(g);
            w.v2q.add(g ? -1 : b.point(GQ_FLOOR, p.o[i], G4_VH));
        }
    }

    static void g4fly3(const g4ctx &C, int k, g4work &w, const gqbatch &b)
    {
        const g4pos &p = *w.p;
        g4V &r = w.out->V;
        if(r.note) return;
        int n = w.n;
        vector<char> m1;
        loopi(n) m1.add(0);
        loopv(w.v1w) m1[w.v1w[i]] = !(w.v1g[i] || b.get(w.v1q[i]));
        vector<int> ra, rb;
        g4runst(p.t, m1, 250.0, ra, rb);
        double s1 = 0;
        loopv(ra) if(p.t[rb[i]] - p.t[ra[i]] >= G4_VFEN)
        {
            g4vep &e = r.v1.add();
            e.t = p.t[ra[i]]; e.s = npround((p.t[rb[i]] - p.t[ra[i]])/1000.0, 2);
            s1 += e.s;
        }
        r.v1_n = r.v1.length(); r.v1_s = npround(s1, 2);
        vector<char> haut;
        loopi(n) haut.add(0);
        loopv(w.v2w) haut[w.v2w[i]] = !(w.v2g[i] || b.get(w.v2q[i]));
        vector<char> m2;
        loopi(n) m2.add(0);
        loopi(n) if(haut[i])
        {
            int j = ssleft(p.t, p.t[i] + G4_VFEN);
            if(j >= n) continue;
            bool ok = true;
            for(int q = i; q <= j && ok; q++) if(!w.vair[q]) ok = false;
            for(int q = i+1; q <= j && ok; q++) if(p.t[q] - p.t[q-1] > 250) ok = false;
            if(!ok) continue;
            if(p.o[j].z - p.o[i].z > -G4_VDROP) m2[i] = 1;
        }
        g4runst(p.t, m2, 300.0, ra, rb);
        double s2 = 0;
        loopv(ra)
        {
            g4vep &e = r.v2.add();
            e.t = p.t[ra[i]]; e.s = npround((p.t[rb[i]] - p.t[ra[i]])/1000.0 + G4_VFEN/1000.0, 2);
            s2 += e.s;
        }
        r.v2_n = r.v2.length(); r.v2_s = npround(s2, 2);
        double ah = 0;
        loopi(n) if(haut[i]) ah += dmin(i+1 < n ? p.t[i+1] - p.t[i] : 33.0, 100.0);
        r.air_haut_s = pyround(ah/1000.0, 2);
    }

    // places (cells of 32 units) where another player of the game is also inside a wall: a feature of the map
    // (a passage the map file shows solid, another version of the map), not a cheat
    static void g4lieux(vector<g4line> &L)
    {
        auto casecode = [](const dv3 &o, int dx, int dy, int dz) -> long long
        {
            long long x = (long long)floor(o.x/32) + dx + 4096, y = (long long)floor(o.y/32) + dy + 4096, z = (long long)floor(o.z/32) + dz + 4096;
            return (x*8192 + y)*8192 + z;
        };
        struct occ { long long c; int key; };
        vector<occ> qui;
        loopv(L)
        {
            if(!L[i].geo || L[i].N.note) continue;
            const g4N &N = L[i].N;
            const vector<g4nev> *lists[2] = { &N.eps, &N.trav };
            loopk(2) loopvj(*lists[k])
            {
                const dv3 &o = (*lists[k])[j].o;
                for(int dx = -1; dx <= 1; dx++) for(int dy = -1; dy <= 1; dy++) for(int dz = -1; dz <= 1; dz++)
                {
                    occ &x = qui.add(); x.c = casecode(o, dx, dy, dz); x.key = L[i].key;
                }
            }
        }
        loopv(L)
        {
            if(!L[i].geo || L[i].N.note) continue;
            g4N &N = L[i].N;
            vector<g4nev> *lists[2] = { &N.eps, &N.trav };
            int props[2] = { 0, 0 };
            loopk(2) loopvj(*lists[k])
            {
                g4nev &e = (*lists[k])[j];
                long long c = casecode(e.o, 0, 0, 0);
                e.shared = false;
                for(int m = 0; m < qui.length(); m++) if(qui[m].c == c && qui[m].key != L[i].key) { e.shared = true; break; }
                if(!e.shared) props[k]++;
            }
            N.ep_propres = props[0]; N.tr_propres = props[1];
        }
    }

    // a group of players, at most this many positions (the queries of a round stay small)
    #define G4_GROUP 40000

    // all of group 4 for one game; false when the geometry became unavailable (L is kept)
    static bool g4analyse(const rawdemo &rd, const partie &P, const jeu &J, const mapents &ents, losapi *los, geoapi *geo,
                          vector<g4line> &out, const volatile bool *cancel)
    {
        g4ctx *Cp = new g4ctx;
        g4ctx &C = *Cp;
        g4context(rd, P, J, ents, C);
        // the players: positions (20 at least) or shots (5 at least)
        vector<int> keys;
        loopv(C.pos) keys.add(C.pos[i].key);
        loopv(P.tiridx.k) if(!C.posidx.has(P.tiridx.k[i])) keys.add(P.tiridx.k[i]);
        keys.sort();
        loopv(keys)
        {
            int k = keys[i];
            const g4pos *p = C.posof(k);
            int nl = p ? p->t.length() : 0;
            const vector<p2shot> *s = P.shots(k);
            int ns = s ? s->length() : 0;
            if(nl < 20 && ns < 5) continue;
            g4line &l = out.add();
            l.key = k; l.npos = nl; l.ntirs = ns;
            l.bot = P.g.ai.has(k); l.local = g4local(P, k); l.geo = false;
            l.W.tirs = l.W.touches = l.W.rates = l.W.segt = l.W.segr = l.W.cachet = l.W.murs = 0;
            l.F.note = l.N.note = l.V.note = (C.coop || C.edit.has(k)) ? 1 : (nl < 20 ? 2 : 0);
            l.N.echantillons = l.N.dedans_n = l.N.episodes = l.N.ep_max = l.N.traversees = l.N.ep_propres = l.N.tr_propres = 0;
            l.N.dedans_s = 0;
            l.F.monte_fort = l.F.plane_fort = l.F.float_ep = l.F.float_local = l.F.echantillons = l.F.float_n = l.F.segments = l.F.mesures = l.F.appuis = 0;
            l.F.ex_appui = l.F.ex_poussee = l.F.ex_eau = l.F.monte = l.F.remonte = l.F.plane = 0;
            l.F.float_s = 0; l.F.haut_max = DNAN;
            l.V.v1_s = l.V.v2_s = l.V.air_haut_s = 0; l.V.v1_n = l.V.v2_n = 0;
            g4lag(C, k, l.L);
        }
        bool ok = true;
        if(geo && los)
        {
            // groups of players, four rounds of questions each (and the sight of the victims)
            for(int g0 = 0; g0 < out.length() && ok;)
            {
                if(cancel && *cancel) { ok = false; break; }
                int g1 = g0, total = 0;
                while(g1 < out.length() && (g1 == g0 || total + out[g1].npos <= G4_GROUP)) { total += out[g1].npos; g1++; }
                vector<g4work *> ws;
                for(int i = g0; i < g1; i++)
                {
                    g4work *w = new g4work;
                    w->out = &out[i];
                    w->p = C.posof(out[i].key);
                    w->n = w->p ? w->p->t.length() : 0;
                    ws.add(w);
                }
                gqbatch b1, b2, b3, b4;
                loopv(ws)
                {
                    g4work &w = *ws[i];
                    int k = w.out->key;
                    g4walls1(C, k, w, b1);
                    if(w.n >= 20 && !w.out->F.note) g4body1(C, k, w, b1);
                }
                ok = b1.run(geo);
                vector<losquery> lq;
                vector<uchar> lr;
                if(ok)
                {
                    loopv(ws)
                    {
                        g4work &w = *ws[i];
                        int k = w.out->key;
                        g4walls2(C, k, w, b1, b2, lq);
                        if(w.n >= 20 && !w.out->F.note)
                        {
                            w.eau.setsize(0);
                            loopj(w.n) w.eau.add(b1.get(w.qwater0 + j) != 0);
                            g4jumps2(C, k, w, b2);
                            g4noclip2(C, k, w, b1, b2);
                            g4fly2(C, k, w, b2);
                        }
                    }
                    ok = b2.run(geo) && (lq.empty() || los->run(lq, lr));
                }
                if(ok)
                {
                    loopv(ws)
                    {
                        g4work &w = *ws[i];
                        int k = w.out->key;
                        g4walls3(C, k, w, b2, lr);
                        if(w.n >= 20 && !w.out->F.note)
                        {
                            g4jumps3(C, k, w, b2);
                            g4fly3(C, k, w, b2);
                            g4noclip3(C, k, w, b2, b3);
                        }
                    }
                    ok = b3.run(geo);
                }
                if(ok)
                {
                    loopv(ws) { g4work &w = *ws[i]; if(w.n >= 20 && !w.out->F.note) g4noclip4(C, w.out->key, w, b3, b4); }
                    ok = b4.run(geo);
                }
                if(ok)
                {
                    loopv(ws)
                    {
                        g4work &w = *ws[i];
                        if(w.n >= 20 && !w.out->F.note) g4noclip5(C, w.out->key, w, b4);
                        w.out->geo = true;
                    }
                }
                ws.deletecontents();
                g0 = g1;
            }
            if(ok) g4lieux(out);
        }
        if(!ok) loopv(out) out[i].geo = false;
        delete Cp;
        return ok && geo && los;
    }

    // ------------------------------------------------------------ player stats
    // What each player did in a game, for the page Player stats: counted from the demo's own messages and
    // positions, nothing judged. Frags on enemies, deaths, suicides, frags on a teammate, best streak; shots,
    // hits and damage by weapon (the weapon of a hit: the push sent with it, else the explosion or the shot it
    // follows; instagib: a rifle hit counts 100 and a saw hit 50, dealt or taken); flags taken, scored and
    // returned, frags on a carrier; ping; the time in his half, in the other half and near his flag (modes
    // where each team has a flag or a base); staying still (a few seconds in a small circle, strafes included;
    // the dead, the wait to spawn and the intermission left out); frags and deaths with the killer out of the
    // victim's field of view; the reactions to an enemy coming out (group 3, with the map).
    #define ST_WEAPONS 7
    struct stparams
    {
        double fov, fovrec;          // horizontal fields of view (degrees): the others (unknown in a demo), the recorder
        double baser;                // near his flag: radius (units)
        double stillms, stillr;      // staying still: at least stillms (ms) in a circle of radius stillr (units)
        stparams() : fov(100), fovrec(100), baser(250), stillms(3000), stillr(20) {}
    };

    struct stweapon { int shots, hits, frags, deaths; double dealt, taken; };

    // the numbers of one player in one game (plain data)
    struct stcounts
    {
        bool recgame, camp;
        int frags, deaths, suicides, teamkills, streak, pingn;
        int backfrags, backfragsn, backdeaths, backdeathsn, killedrec, killedbyrec, backonrec, backbyrec;
        int flagtaken, flagscored, flagreturned, carrierfrags, still_n, still_frags;
        double time_ms, pingsum, dealt, taken, ownhalf_ms, enemyhalf_ms, nearbase_ms, still_ms;
        stweapon w[ST_WEAPONS];
    };

    struct stplayer
    {
        int key;
        bool bot, recorder;
        stcounts c;
        vector<int> rt;              // reactions to an enemy coming out (ms), from group 3
    };

    // modes where each team has its flag or its base at home (ctf, protect, collect)
    static inline bool campmode(int m) { return m == 11 || m == 12 || m == 13 || m == 14 || m == 17 || m == 18 || m == 20 || m == 21 || m == 22; }

    // the team of a player at time t as the flags know it: 1 good, 2 evil, 0 another team
    static int teamflag(const partie &P, int key, double t)
    {
        int ti = P.team_at(key, t);
        if(ti < 0) return 0;
        const char *s = P.g.rd->str(ti);
        return !strcmp(s, "good") ? 1 : (!strcmp(s, "evil") ? 2 : 0);
    }

    // where a track was and looked at time t: between two close samples (500 ms at most) interpolated, else held from
    // the last sample before t (a victim's last position before his death: then comes the gap to his next life);
    // false when that sample is older than 500 ms
    static bool ststate(const piste *p, double t, dv3 &eye, double &yaw, double &pitch)
    {
        if(!p || p->n() < 2) return false;
        int i = ssright(p->t, t);
        if(i <= 0 || t - p->t[i-1] > 500) return false;
        if(i < p->n() && p->t[i] - p->t[i-1] <= 500) { eye = p->eye_at(t); p->ypr_at(t, yaw, pitch); }
        else { eye = p->eye[i-1]; yaw = p->yaw[i-1]; pitch = p->pitch[i-1]; }
        return true;
    }

    // the eye q out of the view (yaw, pitch) from the eye o: horizontal field fov, the vertical one of a 16:9 screen
    static bool outofview(double yaw, double pitch, const dv3 &o, const dv3 &q, double fov)
    {
        double best, dy, dp, dist, half, ye;
        angles_vue(yaw, pitch, o, q, best, dy, dp, dist, half, ye);
        double vfov = 2.0*degrees(atan(tan(fov*0.5*DRAD)*9.0/16.0));
        return fabs(dy) > fov*0.5 || fabs(dp) > vfov*0.5;
    }

    struct stshot { double t; int gun, id; bool hit; };

    static void stanalyse(const rawdemo &rd, const partie &P, const vector<g3line> &g3, const mapents &ents, const stparams &S, vector<stplayer> &out)
    {
        out.shrink(0);
        const gamebase &g = P.g;
        if(g.mode == 1) return;      // coop edit: not a game
        double t_fin = P.t_fin, t_inter = t_fin + 1;
        loopv(g.ev) { const devent &e = rd.ev[g.ev[i]]; if(e.type == E_TIMEUP && e.a <= 0 && e.t > 1000) t_inter = dmin(t_inter, e.t); }
        // positions (feet) of each player, sorted, before the intermission
        imap pidx;
        vector<dlist> pt;
        vector<vector<dv3> > po;
        {
            vector<vector<int> > lists;
            loopv(g.d.pos)
            {
                const dposx &p = rd.pos[g.d.pos[i]];
                if(p.t > t_fin || p.t >= t_inter) continue;
                int key = g.cle(p.cn, p.t);
                int li = pidx.get(key, -1);
                if(li < 0) { li = lists.length(); lists.add(); pidx.set(key, li); }
                lists[li].add(g.d.pos[i]);
            }
            loopv(lists)
            {
                dlist t;
                loopvj(lists[i]) t.add(rd.pos[lists[i][j]].t);
                vector<int> o;
                stableargsort(t.getbuf(), t.length(), o);
                dlist &T = pt.add();
                vector<dv3> &O = po.add();
                loopvj(o) { const dposx &p = rd.pos[lists[i][o[j]]]; T.add(p.t); O.add(dv3(p.o[0], p.o[1], p.o[2])); }
            }
        }
        // the players: two positions at least
        vector<int> keys;
        loopv(pidx.k) if(pt[pidx.v[i]].length() >= 2) keys.add(pidx.k[i]);
        keys.sort();
        imap sidx;
        loopv(keys)
        {
            stplayer &s = out.add();
            s.key = keys[i];
            s.bot = g.ai.has(keys[i]);
            s.recorder = P.rec >= 0 && keys[i] == P.rec;
            memset(&s.c, 0, sizeof(s.c));
            s.c.recgame = P.rec >= 0 && !s.recorder;
            const dlist &T = pt[pidx.get(keys[i], 0)];
            s.c.time_ms = T[T.length()-1] - T[0];
            sidx.set(keys[i], i);
        }
        auto st = [&](int key) -> stcounts * { int i = sidx.get(key, -1); return i >= 0 ? &out[i].c : NULL; };
        // the messages of the game, in order
        imap shotsidx, expidx, dmgidx, carrier, streak;
        vector<vector<stshot> > shots;
        struct expl_t { double t; int gun, id; };
        vector<expl_t> exps;
        struct dmg_t { double t; int actor, gun; };
        vector<dmg_t> lastdmg;
        timelists kills;             // frags on enemies, by killer
        const vector<int> &ev = g.ev;
        loopv(ev)
        {
            const devent &e = rd.ev[ev[i]];
            double t = e.t;
            int c = evkey(g, e);
            switch(e.type)
            {
                case E_SHOT:
                {
                    int si = shotsidx.get(c, -1);
                    if(si < 0) { si = shots.length(); shots.add(); shotsidx.set(c, si); }
                    stshot &s = shots[si].add();
                    s.t = t; s.gun = e.a; s.id = e.b; s.hit = false;
                    stcounts *p = st(c);
                    if(p && e.a >= 0 && e.a < ST_WEAPONS) p->w[e.a].shots++;
                    break;
                }
                case E_EXPLODE:
                {
                    int xi = expidx.get(c, -1);
                    if(xi < 0) { xi = exps.length(); exps.add(); expidx.set(c, xi); }
                    exps[xi].t = t; exps[xi].gun = e.a; exps[xi].id = e.b;
                    break;
                }
                case E_DAMAGE:
                {
                    int a = evkeya(g, e);
                    if(a < 0 || a == c || P.meme_equipe(a, c, t)) break;
                    // the weapon: the push sent with the hit, else the explosion of a projectile at that time, else
                    // the shooter's last shot (450 ms for a hitscan weapon or the saw, 5 s for a rocket or a grenade)
                    int gun = -1;
                    for(int j = i+1; j < ev.length() && j <= i+4; j++)
                    {
                        const devent &f = rd.ev[ev[j]];
                        if(f.t != e.t || (f.type == E_DAMAGE && evkey(g, f) == c)) break;
                        if(f.type == E_HITPUSH && evkey(g, f) == c) { gun = f.a; break; }
                    }
                    int xi = expidx.get(a, -1);
                    const expl_t *x = xi >= 0 && exps[xi].t == t ? &exps[xi] : NULL;
                    if(gun < 0 && x) gun = x->gun;
                    stshot *hs = NULL;
                    int si = shotsidx.get(a, -1);
                    if(si >= 0)
                    {
                        vector<stshot> &L = shots[si];
                        if(x && gun == x->gun) for(int j = L.length()-1; j >= 0 && !hs; j--) if(L[j].id == x->id && L[j].gun == x->gun) hs = &L[j];
                        for(int pass = 0; pass < 2 && !hs; pass++)
                        {
                            for(int j = L.length()-1; j >= 0; j--)
                            {
                                const stshot &s = L[j];
                                double dt = t - s.t;
                                if(dt < 0) continue;
                                if(dt > 5000) break;
                                bool proj = s.gun == 3 || s.gun == 5;
                                if(proj != (pass == 1) || (gun >= 0 && s.gun != gun) || dt > (proj ? 5000 : 450)) continue;
                                hs = &L[j];
                                break;
                            }
                        }
                    }
                    if(gun < 0 && hs) gun = hs->gun;
                    if(hs) hs->hit = true;
                    double dmg = e.b;
                    if(P.insta) dmg = gun == 0 ? 50 : 100;
                    bool known = gun >= 0 && gun < ST_WEAPONS;
                    stcounts *pa = st(a), *pc = st(c);
                    if(pa) { pa->dealt += dmg; if(known) pa->w[gun].dealt += dmg; }
                    if(pc) { pc->taken += dmg; if(known) pc->w[gun].taken += dmg; }
                    int di = dmgidx.get(c, -1);
                    if(di < 0) { di = lastdmg.length(); lastdmg.add(); dmgidx.set(c, di); }
                    lastdmg[di].t = t; lastdmg[di].actor = a; lastdmg[di].gun = gun;
                    break;
                }
                case E_DIED:
                {
                    int a = evkeya(g, e);
                    stcounts *pv = st(c), *pa = a >= 0 ? st(a) : NULL;
                    if(pv) pv->deaths++;
                    streak.set(c, 0);
                    bool carried = carrier.get(c, 0) != 0;
                    carrier.set(c, 0);
                    if(a < 0 || a == c) { if(pv) pv->suicides++; break; }
                    if(P.meme_equipe(a, c, t)) { if(pa) pa->teamkills++; break; }
                    int gun = -1, di = dmgidx.get(c, -1);
                    if(di >= 0 && lastdmg[di].t == t && lastdmg[di].actor == a) gun = lastdmg[di].gun;
                    bool known = gun >= 0 && gun < ST_WEAPONS;
                    kills.get(a).add(t);
                    if(pa)
                    {
                        pa->frags++;
                        int sk = streak.get(a, 0) + 1;
                        streak.set(a, sk);
                        pa->streak = max(pa->streak, sk);
                        if(known) pa->w[gun].frags++;
                        if(carried) pa->carrierfrags++;
                        if(c == P.rec) pa->killedrec++;
                    }
                    if(pv)
                    {
                        if(known) pv->w[gun].deaths++;
                        if(a == P.rec) pv->killedbyrec++;
                    }
                    // the killer out of the victim's field of view (the recorder's own field for the recorder)
                    dv3 veye, keye;
                    double vy, vp, ky, kp;
                    if(ststate(P.visee(c), t, veye, vy, vp) && ststate(P.visee(a), t, keye, ky, kp))
                    {
                        bool hid = outofview(vy, vp, veye, keye, c == P.rec ? S.fovrec : S.fov);
                        if(pa) { pa->backfragsn++; if(hid) { pa->backfrags++; if(c == P.rec) pa->backonrec++; } }
                        if(pv) { pv->backdeathsn++; if(hid) { pv->backdeaths++; if(a == P.rec) pv->backbyrec++; } }
                    }
                    break;
                }
                case E_TAKEFLAG: { stcounts *p = st(c); if(p) p->flagtaken++; carrier.set(c, 1); break; }
                case E_SCOREFLAG: { stcounts *p = st(c); if(p) p->flagscored++; carrier.set(c, 0); break; }
                case E_RETURNFLAG: { stcounts *p = st(c); if(p) p->flagreturned++; break; }
                case E_DROPFLAG: carrier.set(c, 0); break;
                case E_PING: { stcounts *p = st(c); if(p) { p->pingsum += e.a; p->pingn++; } break; }
            }
        }
        kills.sortall();
        // hits: the shots that touched an enemy
        loopv(shotsidx.k)
        {
            stcounts *p = st(shotsidx.k[i]);
            if(!p) continue;
            const vector<stshot> &L = shots[shotsidx.v[i]];
            loopvj(L) if(L[j].hit && L[j].gun >= 0 && L[j].gun < ST_WEAPONS) p->w[L[j].gun].hits++;
        }
        // his half, the other half and near his flag: the nearest flag at home, while he is alive
        bool camp = campmode(g.mode) && ents.ok;
        if(camp)
        {
            int n1 = 0, n2 = 0;
            loopv(ents.flagteam) { if(ents.flagteam[i] == 1) n1++; else if(ents.flagteam[i] == 2) n2++; }
            camp = n1 > 0 && n2 > 0;
        }
        loopv(out)
        {
            stplayer &s = out[i];
            int pi = pidx.get(s.key, 0);
            const dlist &T = pt[pi];
            const vector<dv3> &O = po[pi];
            int n = T.length();
            if(camp)
            {
                for(int j = 0; j+1 < n; j++)
                {
                    double dt = T[j+1] - T[j];
                    if(dt <= 0 || dt > 250) continue;
                    int team = teamflag(P, s.key, T[j]);
                    if(!team) continue;
                    double dmine = DINF, dother = DINF;
                    loopvk(ents.flag)
                    {
                        double d = (O[j] - ents.flag[k]).norm();
                        if(ents.flagteam[k] == team) dmine = dmin(dmine, d); else dother = dmin(dother, d);
                    }
                    s.c.camp = true;
                    if(dmine <= dother) s.c.ownhalf_ms += dt; else s.c.enemyhalf_ms += dt;
                    if(dmine <= S.baser) s.c.nearbase_ms += dt;
                }
            }
            // staying still: positions of one life (no gap of 500 ms) inside a square of side 2 r (x, y)
            double side = 2.0*S.stillr;
            const dlist *kl = kills.find(s.key);
            for(int j = 0; j < n;)
            {
                double x0 = O[j].x, x1 = x0, y0 = O[j].y, y1 = y0;
                int e = j;
                while(e+1 < n && T[e+1] - T[e] <= 500)
                {
                    double ax = dmin(x0, O[e+1].x), bx = dmax(x1, O[e+1].x), ay = dmin(y0, O[e+1].y), by = dmax(y1, O[e+1].y);
                    if(bx - ax > side || by - ay > side) break;
                    x0 = ax; x1 = bx; y0 = ay; y1 = by;
                    e++;
                }
                double dur = T[e] - T[j];
                if(dur >= S.stillms)
                {
                    s.c.still_ms += dur;
                    s.c.still_n++;
                    if(kl) for(int q = ssleft(*kl, T[j]); q < kl->length() && (*kl)[q] <= T[e]; q++) s.c.still_frags++;
                    j = e + 1;
                }
                else j++;
            }
            loopvj(g3) if(g3[j].key == s.key && g3[j].measured) loopvk(g3[j].rt_tous) s.rt.add(int(g3[j].rt_tous[k]));
        }
    }

    // ------------------------------------------------------------ one demo, every check
    // the thresholds of the alerts (defaults: the studies), copied when an analysis starts
    struct params
    {
        g1params p1;
        g2params q2;
        g3params q3;
        // group 1 alerts
        int hb_hits, early, sp_time, mv_speed, mv_time, mv_ping, ratio_win, jumps;
        double hb_share, hb_dist, clock, ratio;
        // trigger
        int tr_flicks;
        double tr_z;
        // shortest game read (ms)
        int min_ms;
        // group 4: asked analyses and the automatic analysis (the history alone keeps groups 1 to 3); its alert levels
        bool g4;
        int wall_hits, wall_share, lag_frags, lag_gaps, jump_events, noclip_events;
        double lag_z, air_s;
        // player stats: counted only when the page is on
        bool stats;
        stparams st;
        params() : hb_hits(10), early(1), sp_time(1000), mv_speed(220), mv_time(3000), mv_ping(250), ratio_win(5), jumps(3),
                   hb_share(0.40), hb_dist(6.0), clock(1000), ratio(1.5), tr_flicks(15), tr_z(3.0), min_ms(30000),
                   g4(true), wall_hits(3), wall_share(10), lag_frags(3), lag_gaps(2), jump_events(8), noclip_events(10), lag_z(2.0), air_s(2.0),
                   stats(false) {}
        // the players of the trigger study (mean, deviation)
        double trbefore() const { return 0.40809902721774494 + tr_z*0.12830132712116679; }
        double tredge() const { return 0.2154442096831897 + tr_z*0.11301461100662767; }
    };

    // results of one game of the file
    struct gameres
    {
        int part, map, mode, t_fin, rec, source, nplayers;
        bool hdused, losok, trdone, g3done;
        g1result g1;
        vector<g2player> g2;
        vector<trline> tr;
        vector<g3line> g3;
        vector<g4line> g4;
        bool g4geo;        // group 4 measured with the map (walls, flying, jumps, noclip)
        imap names;        // key -> pool string
        imap botkeys;      // key -> 1
        mapents ents;
        vector<stplayer> st;   // player stats (when the page is on)
        bool stdone;
        gameres() : part(0), map(-1), mode(-1), t_fin(0), rec(-1), source(0), nplayers(0), hdused(false), losok(false), trdone(false), g3done(false), g4geo(false),
                    stdone(false) {}
    };

    // what the analysis needs from the main thread (the map, files of the game): a small service
    struct mainservice
    {
        virtual ~mainservice() {}
        virtual losapi *los() = 0;
        virtual geoapi *geo() = 0;
        virtual bool mapents_of(const char *map, mapents &me) = 0;   // entities of the map file
        virtual void progress(int pct) = 0;
    };

    static void analysegames(const rawdemo &rd, const hddata *hd, const params &PA, mainservice &svc, vector<gameres *> &out, const volatile bool *cancel)
    {
        int nseg = rd.nsegs;
        loopk(nseg)
        {
            if(cancel && *cancel) return;
            gamebase g;
            buildgame(rd, k, g);
            // a game worth reading: long enough (30 s by default) and two players with positions
            imap pk;
            loopv(g.d.pos) pk.set(rd.pos[g.d.pos[i]].cn, 1);
            if(g.t_fin < PA.min_ms || pk.length() < 2) continue;
            gameres *G = new gameres;
            out.add(G);
            G->part = k; G->map = g.map; G->mode = g.mode; G->t_fin = g.t_fin;
            G->names = g.names;
            loopv(g.ai.k) G->botkeys.set(g.ai.k[i], 1);
            const char *mapname = g.map >= 0 ? rd.str(g.map) : "";
            svc.mapents_of(mapname, G->ents);
            int base = 100*k/nseg, span = 100/nseg;
            svc.progress(base + span/20);
            partie P;
            buildpartie(rd, hd, k, P);
            G->rec = P.rec; G->source = P.source; G->hdused = P.phd.length() > 0;
            if(cancel && *cancel) return;
            // the checks that need the map first (it may be unloaded soon: end of a recorded game)
            losapi *los = svc.los();
            G->losok = los != NULL;
            jeu J;
            if(los || PA.g4) buildjeu(rd, P, PA.q3, J);
            if(los)
            {
                trgame T;
                if(buildtrgame(rd, k, T)) G->trdone = tranalyse(T, los, G->tr, cancel);
                else G->trdone = true;
                if(cancel && *cancel) return;
                svc.progress(base + span/5);
                bool ok = true;
                loopv(J.order)
                {
                    if(cancel && *cancel) return;
                    g3line &x = G->g3.add();
                    if(!g3observer(J, J.order[i], los, x, cancel)) { ok = false; break; }
                    svc.progress(base + span/5 + (PA.g4 ? span*2/5 : span*3/5)*(i+1)/max(J.order.length(), 1));
                }
                G->g3done = ok;
                if(!ok) G->g3.shrink(0);
            }
            // group 4: the lag check needs no map, the others its walls and floors
            if(PA.g4)
            {
                if(cancel && *cancel) return;
                G->g4geo = g4analyse(rd, P, J, G->ents, los, svc.geo(), G->g4, cancel);
                if(cancel && *cancel) return;
            }
            svc.progress(base + span*4/5);
            g1analyse(rd, hd, k, G->ents, PA.p1, G->g1);
            if(cancel && *cancel) return;
            g2analyse(P, PA.q2, G->g2, cancel);
            if(PA.stats && !(cancel && *cancel)) { stanalyse(rd, P, G->g3, G->ents, PA.st, G->st); G->stdone = true; }
            G->nplayers = G->g1.players.length();
            svc.progress(base + span);
        }
    }

}

#ifndef DEMOANALYSE_STANDALONE
// ================================================================ in the game
// Settings: the defaults are the values of the studies. A threshold changed
// in the menu redraws the table at once; the measuring settings (range of a
// shot sound, cold time, fast movement, shot clock) count from the next
// analysis.
VARP(demoanalysis, 0, 0, 1);
VARP(demoanalysishistory, 0, 1, 1);
VARP(demoanalysisms, 1, 5, 20);
VARP(demoanalysisallmoments, 0, 0, 1);
VARP(demoanalysishitboxhits, 1, 10, 1000);
VARP(demoanalysishitboxshare, 1, 40, 100);
FVARP(demoanalysishitboxdist, 4.1f, 6.0f, 50);
VARP(demoanalysisclock, 100, 1000, 600000);
VARP(demoanalysisearly, 1, 1, 1000);
VARP(demoanalysisspeedtime, 100, 1000, 600000);
VARP(demoanalysismovespeed, 150, 220, 2000);
VARP(demoanalysismovetime, 100, 3000, 600000);
VARP(demoanalysismoveping, 0, 250, 1000);
FVARP(demoanalysisratio, 1.05f, 1.5f, 20);
VARP(demoanalysisratiowindows, 1, 5, 1000);
VARP(demoanalysisjumps, 1, 3, 1000);
FVARP(demoanalysissnapz, 1, 4, 20);
VARP(demoanalysissnaprepeat, 1, 3, 1000);
FVARP(demoanalysistrackz, 1, 4, 20);
VARP(demoanalysistrackwindows, 1, 10, 1000);
VARP(demoanalysistrackseconds, 1, 20, 600);
VARP(demoanalysistriggerflicks, 1, 15, 1000);
FVARP(demoanalysistriggerz, 0.5f, 3, 10);
VARP(demoanalysissound, 100, 1000, 5000);
VARP(demoanalysiscold, 100, 1500, 10000);
VARP(demoanalysisexposure, 0, 120, 3600);
VARP(demoanalysishiddenrepeat, 1, 6, 1000);
FVARP(demoanalysishiddenz, 1, 4, 20);
VARP(demoanalysishiddenshots, 1, 5, 1000);
VARP(demoanalysisfollow, 1, 35, 10000);
VARP(demoanalysiscombinedshots, 1, 2, 1000);
FVARP(demoanalysiscombinedz, 0, 1.5f, 20);
VARP(demoanalysiswaitfast, 10, 150, 1000);
VARP(demoanalysiswaitcount, 1, 5, 1000);
VARP(demoanalysiswaitfastcount, 1, 3, 1000);
VARP(demoanalysisduel, 0, 1, 1);
VARP(demoanalysisminsecs, 0, 30, 600);
VARP(demoanalysisrupturegames, 2, 3, 100);
VARP(demoanalysisrupturemeasures, 1, 2, 10);
FVARP(demoanalysisrupturez, 1, 3.5f, 20);
VARP(demoanalysiswalls, 1, 3, 1000);
VARP(demoanalysiswallshare, 1, 10, 100);
VARP(demoanalysislagfrags, 1, 3, 1000);
FVARP(demoanalysislagz, 0, 2, 20);
VARP(demoanalysislaggaps, 1, 2, 1000);
FVARP(demoanalysisair, 0.5f, 2, 600);
VARP(demoanalysisjumplook, 1, 8, 1000);
VARP(demoanalysisnocliplook, 1, 10, 1000);
VARP(demoanalysisstats, 0, 0, 1);
VARP(demoanalysisauto, 0, 0, 1);
VARP(demoanalysisfov, 10, 100, 150);
VARP(demoanalysisbaseradius, 10, 250, 4000);
FVARP(demoanalysisstill, 0.5f, 3, 60);
FVARP(demoanalysisstillradius, 1, 20, 500);
extern int fov;

namespace demoanalyse
{
    bool mirroring = false;

    static void currentparams(params &PA)
    {
        PA.hb_hits = demoanalysishitboxhits; PA.hb_share = demoanalysishitboxshare/100.0; PA.hb_dist = demoanalysishitboxdist;
        PA.clock = demoanalysisclock; PA.early = demoanalysisearly; PA.sp_time = demoanalysisspeedtime;
        PA.mv_speed = demoanalysismovespeed; PA.mv_time = demoanalysismovetime; PA.mv_ping = demoanalysismoveping;
        PA.ratio = demoanalysisratio; PA.ratio_win = demoanalysisratiowindows; PA.jumps = demoanalysisjumps;
        PA.p1.v_dep_max = demoanalysismovespeed;
        PA.p1.avance_ms = demoanalysisclock;
        PA.q2.Z = demoanalysissnapz; PA.q2.Ztrack = demoanalysistrackz;
        PA.q2.rep_snap = PA.q2.rep_sil = demoanalysissnaprepeat;
        PA.q2.rep_win = demoanalysistrackwindows; PA.q2.proche_min = demoanalysistrackseconds;
        PA.tr_flicks = demoanalysistriggerflicks; PA.tr_z = demoanalysistriggerz;
        g3params &Q = PA.q3;
        Q.r_tir = demoanalysissound; Q.froid = demoanalysiscold; Q.expo_min = demoanalysisexposure; Q.rep_a = demoanalysishiddenrepeat;
        double z = demoanalysishiddenz;
        // the players of the study (mean + z deviations): 5.57, 5.05, 1.71 and 0.36 with z = 4
        Q.z_ep = 0.3110689578198247 + z*1.3136286751096817;
        Q.z_st = 0.2802217451107533 + z*1.1924526655551748;
        Q.z_c = -0.05896285041082495 + z*0.4414931168263058;
        Q.part_rapide = 0.043170805353291986 + z*0.07872785384974047;
        Q.rep_c = demoanalysishiddenshots; Q.ac_cmin = demoanalysiscombinedshots; Q.ac_zc = demoanalysiscombinedz;
        Q.f_alert = demoanalysisfollow;
        Q.rapide = demoanalysiswaitfast; Q.nb = demoanalysiswaitcount; Q.nb_rapide = demoanalysiswaitfastcount;
        Q.noduel = demoanalysisduel != 0;
        PA.min_ms = demoanalysisminsecs*1000;
        PA.wall_hits = demoanalysiswalls; PA.wall_share = demoanalysiswallshare;
        PA.lag_frags = demoanalysislagfrags; PA.lag_z = demoanalysislagz; PA.lag_gaps = demoanalysislaggaps;
        PA.air_s = demoanalysisair; PA.jump_events = demoanalysisjumplook; PA.noclip_events = demoanalysisnocliplook;
        PA.stats = demoanalysisstats != 0;
        PA.st.fov = demoanalysisfov; PA.st.fovrec = ::fov; PA.st.baser = demoanalysisbaseradius;
        PA.st.stillms = demoanalysisstill*1000.0; PA.st.stillr = demoanalysisstillradius;
    }

    // ------------------------------------------------------------ files read by the background thread
    // paths found by the game (findfile) on the main thread; plain stdio and zlib here
    static bool readgzfile(const char *p, vector<uchar> &out)
    {
        gzFile f = gzopen(p, "rb");
        if(!f) return false;
        uchar *buf = new uchar[1<<16];
        for(;;)
        {
            int n = gzread(f, buf, 1<<16);
            if(n <= 0) break;
            out.put(buf, n);
        }
        delete[] buf;
        gzclose(f);
        return out.length() > 0;
    }

    static bool readrawfile(const char *p, vector<uchar> &out)
    {
        FILE *f = fopen(p, "rb");
        if(!f) return false;
        uchar *buf = new uchar[1<<16];
        for(;;)
        {
            size_t n = fread(buf, 1, 1<<16, f);
            if(!n) break;
            out.put(buf, int(n));
        }
        delete[] buf;
        fclose(f);
        return out.length() > 0;
    }

    // ------------------------------------------------------------ an analysis in the background
    enum { REQ_LOS = 1, REQ_ENTS, REQ_GEO };
    enum { RS_IDLE = 0, RS_PENDING, RS_DONE, RS_FAILED };

    // a row's distance to the name's usual games (history), for the page
    struct usualnote { int key; double z; const char *label; int games; };

    struct job : mainservice, losapi, geoapi
    {
        bool automatic;
        string dmopath, hdpath, stem, label, mapname, reqmap;
        vector<uchar> data;       // a recording kept in memory (automatic history), else read from dmopath
        params PA;
        SDL_Thread *thread;
        SDL_mutex *lock;
        SDL_cond *cond;
        volatile bool cancel, finished;
        volatile int pct;
        int reqtype, reqdone;
        volatile int reqstate;
        const vector<losquery> *reqq;
        vector<uchar> *reqres;
        const vector<geoquery> *reqgq;
        vector<uchar> *reqgres;
        mapents *reqents;
        bool maploaded, readok;
        rawdemo rd;
        vector<gameres *> games;
        ullong demoid;
        Uint64 t0;
        double secs, lossecs;
        long long nrays;
        int confirm;             // automatic: 0 the recording goes on, 1 kept, -1 dropped
        vector<usualnote> usual;

        job() : automatic(false), thread(NULL), lock(NULL), cond(NULL), cancel(false), finished(false), pct(0), reqtype(0), reqdone(0),
                reqstate(RS_IDLE), reqq(NULL), reqres(NULL), reqgq(NULL), reqgres(NULL), reqents(NULL), maploaded(false), readok(false), demoid(0), t0(0), secs(0),
                lossecs(0), nrays(0), confirm(0)
        {
            dmopath[0] = hdpath[0] = stem[0] = label[0] = mapname[0] = reqmap[0] = 0;
            lock = SDL_CreateMutex();
            cond = SDL_CreateCond();
        }
        ~job()
        {
            games.deletecontents();
            if(cond) SDL_DestroyCond(cond);
            if(lock) SDL_DestroyMutex(lock);
        }
        // a request to the main thread, answered in a later frame
        bool request(int type)
        {
            SDL_LockMutex(lock);
            reqtype = type;
            reqdone = 0;
            reqstate = RS_PENDING;
            while(reqstate == RS_PENDING && !cancel) SDL_CondWaitTimeout(cond, lock, 50);
            bool ok = reqstate == RS_DONE && !cancel;
            reqstate = RS_IDLE;
            SDL_UnlockMutex(lock);
            return ok;
        }
        bool run(const vector<losquery> &q, vector<uchar> &res)
        {
            res.setsize(0);
            loopv(q) res.add(0);
            if(q.empty()) return true;
            reqq = &q;
            reqres = &res;
            return request(REQ_LOS);
        }
        losapi *los() { return maploaded ? this : NULL; }
        bool rungeo(const vector<geoquery> &q, vector<uchar> &res)
        {
            res.setsize(0);
            loopv(q) res.add(0);
            if(q.empty()) return true;
            reqgq = &q;
            reqgres = &res;
            return request(REQ_GEO);
        }
        geoapi *geo() { return maploaded ? this : NULL; }
        bool mapents_of(const char *map, mapents &me)
        {
            copystring(reqmap, map);
            reqents = &me;
            return request(REQ_ENTS);
        }
        void progress(int p) { pct = clamp(p, 0, 100); }
    };

    // Python's round (pyround) works on long double: the x87 unit must run at 64 bits, as in the
    // main thread of the game (Windows starts every new thread at 53 bits)
    static void fullprecision()
    {
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
        unsigned short cw;
        __asm__ __volatile__("fnstcw %0" : "=m"(cw));
        cw |= 0x300;
        __asm__ __volatile__("fldcw %0" : : "m"(cw));
#endif
    }

    static int workermain(void *data)
    {
        job *j = (job *)data;
        SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
        fullprecision();
        {
            vector<uchar> d;
            if(j->data.length()) d.move(j->data);
            else readgzfile(j->dmopath, d);
            demoparser dp(j->rd);
            j->readok = d.length() > 0 && dp.parse(d.getbuf(), d.length());
            // the demo's id: its first 64 KB (the same for a recording and the file it becomes)
            ullong h = 14695981039346656037ULL;
            int n = min(d.length(), 65536);
            loopi(n) { h ^= d[i]; h *= 1099511628211ULL; }
            j->demoid = h;
        }
        hddata hd;
        if(j->readok && j->hdpath[0] && !j->cancel)
        {
            vector<uchar> h;
            if(readrawfile(j->hdpath, h)) parsehd(h.getbuf(), h.length(), hd);
        }
        if(j->readok && !j->cancel) analysegames(j->rd, hd.ok ? &hd : NULL, j->PA, *j, j->games, &j->cancel);
        j->finished = true;
        return 0;
    }

    static job *cur = NULL;        // running
    static job *queued = NULL;     // asked while another runs
    static job *waiting = NULL;    // automatic, finished before the end of its recording
    static job *last = NULL;       // the last analysis asked (shown in the page)

    // the game's own line of sight test (preactlook in fps.cpp)
    static inline bool rayclear(const float *a, const float *b)
    {
        vec A(a[0], a[1], a[2]), d(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
        float mag = d.magnitude();
        if(mag < 0.5f) return true;
        d.mul(1.0f/mag);
        float dist = raycube(A, d, mag, RAY_ALPHAPOLY|RAY_SKIPFIRST);
        return !(dist >= 0 && dist < mag - 0.35f);
    }

    // group 4's questions, with the game's own tests: a shot's (weapon.cpp), and the collision of a player
    // (collide, as physics.cpp moves him: cubes with their clip and noclip, solid map models)
    static physent gqprobe;
    static bool gqcollide(const vec &o, float eyeheight, float aboveeye)
    {
        physent &e = gqprobe;
        e.o = o;
        e.radius = e.xradius = e.yradius = 0.01f;
        e.eyeheight = eyeheight;
        e.aboveeye = aboveeye;
        e.type = ENT_PLAYER;
        e.collidetype = COLLIDE_ELLIPSE;
        e.state = CS_ALIVE;
        return collide(&e, vec(0, 0, 0), 0, false) || collideinside;
    }
    static inline bool gqsolid(float x, float y, float z) { return gqcollide(vec(x, y, z), 0.01f, 0.01f); }
    static bool gqclear(const vec &a, const vec &b, int mode)
    {
        vec d = vec(b).sub(a);
        float mag = d.magnitude();
        if(mag < 0.5f) return true;
        d.mul(1.0f/mag);
        float dist = raycube(a, d, mag, mode);
        return !(dist >= 0 && dist < mag - 0.35f);
    }
    static inline bool gqwater(float x, float y, float z) { int m = lookupmaterial(vec(x, y, z))&MATF_VOLUME; return m == MAT_WATER || m == MAT_LAVA; }

    static uchar geoanswer(const geoquery &q)
    {
        float x = float(q.a[0]), y = float(q.a[1]), z = float(q.a[2]);
        switch(q.kind)
        {
            case GQ_SHOT: return gqclear(vec(x, y, z), vec(float(q.b[0]), float(q.b[1]), float(q.b[2])), RAY_CLIPMAT|RAY_ALPHAPOLY) ? 1 : 0;
            case GQ_SEG: return gqclear(vec(x, y, z), vec(float(q.b[0]), float(q.b[1]), float(q.b[2])), RAY_CLIPMAT|RAY_POLY) ? 1 : 0;
            case GQ_GEO: return raycube(vec(x, y, z), vec(0, 0, 1), 0.01f, 0) <= 0 ? 1 : 0;
            case GQ_BODY: return (gqsolid(x, y, z + 2) ? 1 : 0) | (gqsolid(x, y, z + 7) ? 2 : 0) | (gqsolid(x, y, z + 13) ? 4 : 0);
            case GQ_RING: return gqsolid(x + 2.5f, y, z) && gqsolid(x - 2.5f, y, z) && gqsolid(x, y + 2.5f, z) && gqsolid(x, y - 2.5f, z) ? 1 : 0;
            case GQ_PASS: return gqsolid(x, y, z + 3) && gqsolid(x, y, z + 7) && gqsolid(x, y, z + 12) ? 1 : 0;
            case GQ_FLOOR: return gqcollide(vec(x, y, z + 0.5f), q.h + 0.5f, 0) ? 1 : 0;
            case GQ_WATER: return gqwater(x, y, z + 11.25f) || gqwater(x, y, z + 1) ? 1 : 0;
        }
        return 0;
    }

    static void serve(job *j)
    {
        SDL_LockMutex(j->lock);
        if(j->reqstate == RS_PENDING)
        {
            if(j->reqtype == REQ_ENTS)
            {
                const char *map = j->reqmap;
                j->maploaded = map[0] && !strcmp(game::getclientmap(), map);
                copystring(j->mapname, map);
                mapents &me = *j->reqents;
                vector<entity> ents;
                me.ok = map[0] && loadents(map, ents);
                me.tele.setsize(0); me.dest.setsize(0); me.pad.setsize(0); me.flag.setsize(0); me.flagteam.setsize(0);
                loopv(ents)
                {
                    const entity &e = ents[i];
                    dv3 p(e.o.x, e.o.y, e.o.z);
                    if(e.type == TELEPORT) me.tele.add(p);
                    else if(e.type == TELEDEST) me.dest.add(p);
                    else if(e.type == JUMPPAD) me.pad.add(p);
                    else if(e.type == FLAG && (e.attr2 == 1 || e.attr2 == 2)) { me.flag.add(p); me.flagteam.add(e.attr2); }
                }
                j->reqstate = RS_DONE;
                SDL_CondSignal(j->cond);
            }
            else if(j->reqtype == REQ_LOS)
            {
                if(j->cancel || !j->maploaded || strcmp(game::getclientmap(), j->mapname))
                {
                    j->reqstate = RS_FAILED;
                    SDL_CondSignal(j->cond);
                }
                else
                {
                    Uint64 freq = SDL_GetPerformanceFrequency(), start = SDL_GetPerformanceCounter();
                    Uint64 budget = Uint64(double(freq)*demoanalysisms/1000.0);
                    const vector<losquery> &q = *j->reqq;
                    vector<uchar> &res = *j->reqres;
                    int n = q.length();
                    while(j->reqdone < n)
                    {
                        const losquery &x = q[j->reqdone];
                        bool clear = false;
                        if(x.mode == 0)
                        {
                            for(int k = 0; k < x.npts && !clear; k++) { j->nrays++; if(rayclear(x.eye, x.pts[k])) clear = true; }
                        }
                        else
                        {
                            int c = 0, left = 0;
                            loopk(x.npts) if(x.mask&(1<<k)) left++;
                            for(int k = 0; k < x.npts; k++)
                            {
                                if(!(x.mask&(1<<k))) continue;
                                if(c >= x.need || c + left < x.need) break;
                                j->nrays++;
                                if(rayclear(x.eye, x.pts[k])) c++;
                                left--;
                            }
                            clear = c >= x.need;
                        }
                        res[j->reqdone] = clear ? 1 : 0;
                        j->reqdone++;
                        if(!(j->reqdone&15) && SDL_GetPerformanceCounter() - start >= budget) break;
                    }
                    j->lossecs += double(SDL_GetPerformanceCounter() - start)/double(freq);
                    if(j->reqdone >= n) { j->reqstate = RS_DONE; SDL_CondSignal(j->cond); }
                }
            }
            else if(j->reqtype == REQ_GEO)
            {
                if(j->cancel || !j->maploaded || strcmp(game::getclientmap(), j->mapname))
                {
                    j->reqstate = RS_FAILED;
                    SDL_CondSignal(j->cond);
                }
                else
                {
                    Uint64 freq = SDL_GetPerformanceFrequency(), start = SDL_GetPerformanceCounter();
                    Uint64 budget = Uint64(double(freq)*demoanalysisms/1000.0);
                    const vector<geoquery> &q = *j->reqgq;
                    vector<uchar> &res = *j->reqgres;
                    int n = q.length();
                    while(j->reqdone < n)
                    {
                        res[j->reqdone] = geoanswer(q[j->reqdone]);
                        j->reqdone++;
                        if(!(j->reqdone&15) && SDL_GetPerformanceCounter() - start >= budget) break;
                    }
                    j->lossecs += double(SDL_GetPerformanceCounter() - start)/double(freq);
                    if(j->reqdone >= n) { j->reqstate = RS_DONE; SDL_CondSignal(j->cond); }
                }
            }
        }
        SDL_UnlockMutex(j->lock);
    }

    static void finishjob(job *j);

    static void startjob(job *j)
    {
        currentparams(j->PA);
        j->PA.g4 = !j->automatic || demoanalysisauto;
        j->t0 = SDL_GetPerformanceCounter();
        j->thread = SDL_CreateThread(workermain, "demo analysis", j);
        if(!j->thread) j->finished = true;
        cur = j;
    }

    static void askjob(job *j)
    {
        if(!cur) { startjob(j); return; }
        if(queued) DELETEP(queued);
        queued = j;
        if(!cur->automatic) cur->cancel = true;   // a new analysis replaces the one asked before
    }

    void frame()
    {
        if(!cur) return;
        serve(cur);
        if(!cur->finished) return;
        if(cur->thread) { SDL_WaitThread(cur->thread, NULL); cur->thread = NULL; }
        cur->secs = double(SDL_GetPerformanceCounter() - cur->t0)/double(SDL_GetPerformanceFrequency());
        job *j = cur;
        cur = NULL;
        finishjob(j);
        if(queued) { job *q = queued; queued = NULL; startjob(q); }
    }

    // ------------------------------------------------------------ the demo being played, the recording
    static string playpath = "";

    static void checkdemo(const char *stem);

    // a local demo named as the demo player opens it (the server's "demo/<stem>.dmo", found as findfile does)
    static void playbackpath(const char *stem, char *out, int len)
    {
        defformatstring(rel, "demo/%s.dmo", stem);
        const char *found = findfile(rel, "rb");
        copystring(out, found ? found : rel, len);
    }

    void onplayback(const char *dmofile)
    {
        const char *found = dmofile && dmofile[0] ? findfile(dmofile, "rb") : NULL;
        copystring(playpath, found ? found : (dmofile ? dmofile : ""));
        // a demo worth a look is checked once it is opened
        const char *b = strrchr(playpath, '\\'), *b2 = strrchr(playpath, '/');
        if(!b || (b2 && b2 > b)) b = b2;
        string stem;
        copystring(stem, b ? b+1 : playpath);
        int n = strlen(stem);
        if(n >= 4 && !strcasecmp(stem + n - 4, ".dmo")) stem[n-4] = 0;
        checkdemo(stem);
    }
    void onplaybackend() { playpath[0] = 0; }

    static vector<uchar> mirror;
    static string mirrorstem = "";
    #define MIRROR_MAX (96<<20)

    void onrecordstart(const void *header, int len, const char *stem)
    {
        mirror.setsize(0);
        mirroring = demoanalysis && (demoanalysishistory || demoanalysisstats || demoanalysisauto) && !game::demoplayback;
        if(!mirroring) return;
        mirror.put((const uchar *)header, len);
        copystring(mirrorstem, stem);
    }

    void onrecordpacket(const void *stamp, const uchar *data, int len)
    {
        if(!mirroring) return;
        if(mirror.length() + 12 + len > MIRROR_MAX) { mirroring = false; mirror.setsize(0); return; }
        mirror.put((const uchar *)stamp, 12);
        mirror.put(data, len);
    }

    static void sidecarof(const char *dmo, char *hd, int len)
    {
        copystring(hd, dmo, len);
        int n = strlen(hd);
        if(n >= 4 && !strcasecmp(hd + n - 4, ".dmo")) copystring(hd + n - 4, ".dmohd", len - (n - 4));
        else concatstring(hd, ".dmohd", len);
    }

    // the game ended (intermission): its map is still loaded, the recording is analysed now (history, player
    // stats, the automatic analysis)
    void onrecordintermission(const char *stem, const char *hdfile)
    {
        if(!mirroring || game::demoplayback) return;
        mirroring = false;
        if(!demoanalysis || !(demoanalysishistory || demoanalysisstats || demoanalysisauto) || !mirror.length()) { mirror.setsize(0); return; }
        job *j = new job;
        j->automatic = true;
        j->data.move(mirror);
        copystring(j->stem, stem);
        // the file being written, named as the demo player will open it (the page's moments)
        playbackpath(stem, j->dmopath, sizeof(j->dmopath));
        formatstring(j->label, "%s.dmo", stem);
        const char *found = hdfile && hdfile[0] ? findfile(hdfile, "rb") : NULL;
        if(found) copystring(j->hdpath, found);
        if(cur) { if(queued) { delete j; return; } queued = j; }
        else startjob(j);
    }

    static void historyupdate(job *j);

    // the recording ended (next map, disconnection): kept or dropped
    void onrecordend(const char *stem, const char *dmofile, bool saved)
    {
        mirroring = false;
        mirror.setsize(0);
        if(!stem || !stem[0]) return;
        if(cur && cur->automatic && !strcmp(cur->stem, stem)) { cur->confirm = saved ? 1 : -1; return; }
        if(queued && queued->automatic && !strcmp(queued->stem, stem)) { queued->confirm = saved ? 1 : -1; return; }
        if(waiting && !strcmp(waiting->stem, stem))
        {
            job *w = waiting;
            waiting = NULL;
            if(saved) historyupdate(w);
            if(w != last) delete w;
            return;
        }
        // no intermission (left during the game, map changed by a vote): from the file kept, without
        // line of sight when its map is no longer loaded
        if(!saved || !demoanalysis || !(demoanalysishistory || demoanalysisstats || demoanalysisauto) || !dmofile || !dmofile[0]) return;
        const char *found = findfile(dmofile, "rb");
        if(!found) return;
        job *j = new job;
        j->automatic = true;
        j->confirm = 1;
        copystring(j->stem, stem);
        copystring(j->dmopath, found);
        // named as the demo player will open it (the page's moments), when that file is there
        string pp;
        playbackpath(stem, pp, sizeof(pp));
        if(fileexists(pp, "r")) copystring(j->dmopath, pp);
        formatstring(j->label, "%s.dmo", stem);
        string hd;
        sidecarof(j->dmopath, hd, sizeof(hd));
        if(fileexists(hd, "r")) copystring(j->hdpath, hd);
        if(cur) { if(queued) { delete j; return; } queued = j; }
        else startjob(j);
    }

    // ------------------------------------------------------------ verdicts (thresholds of the menu)
    static bool hitboxalert(const g1player &p, const params &PA)
    {
        return p.hb_touches >= PA.hb_hits && pyround(p.hb_part, 3) >= PA.hb_share && pyround(p.hb_med, 2) >= PA.hb_dist;
    }
    static bool clockalert(const g1player &p, const params &PA)
    {
        return (!isnand(p.ca_avance) && p.ca_avance >= PA.clock) || p.ca_early >= PA.early;
    }
    static bool speedalert(const g1player &p, const params &PA)
    {
        if(!p.vi_ok) return false;
        double ping = isnand(p.ping) ? 0 : p.ping;
        return p.vi_trop_ms >= PA.sp_time || (p.vi_deprapide_ms >= PA.mv_time && ping <= PA.mv_ping) ||
               (!isnand(p.vi_rapmed) && p.vi_rapmed >= PA.ratio && p.vi_fen_rap >= PA.ratio_win) || p.vi_sauts >= PA.jumps;
    }
    static const g2line *g2best(const vector<g2player> &G, int key)
    {
        loopv(G) if(G[i].key == key)
        {
            const g2line *best = NULL;
            loopvj(G[i].lines) if(!best || G[i].lines[j].var == 1) best = &G[i].lines[j];
            return best;
        }
        return NULL;
    }
    static void g2verdict(const g2line &L, const params &PA, bool &snap, bool &silent, bool &track)
    {
        const g2params &Q = PA.q2;
        const double *ps = L.var ? Q.pop_hd_snap : Q.pop_dmo_snap, *pl = L.var ? Q.pop_hd_sil : Q.pop_dmo_sil, *pu = L.var ? Q.pop_hd_sur : Q.pop_dmo_sur;
        snap = silent = track = false;
        if(L.snap_n >= 10)
        {
            snap = L.snap_k >= Q.rep_snap && (double(L.snap_k)/L.snap_n - ps[0])/ps[1] >= Q.Z;
            silent = L.sil_k >= Q.rep_sil && (double(L.sil_k)/L.snap_n - pl[0])/pl[1] >= Q.Z;
        }
        if(L.S.proche_s && L.S.proche_s >= Q.proche_min) track = L.fenetres >= Q.rep_win && (L.part_sur - pu[0])/pu[1] >= Q.Ztrack;
    }
    static const trline *trof(const vector<trline> &T, int key) { loopv(T) if(T[i].sid == key) return &T[i]; return NULL; }
    static bool tralert(const trline &t, const params &PA)
    {
        return t.nflicks >= PA.tr_flicks && t.npos >= PA.tr_flicks && t.before >= PA.trbefore() && t.band >= PA.tredge();
    }
    static const g3line *g3of(const vector<g3line> &T, int key) { loopv(T) if(T[i].key == key && T[i].measured) return &T[i]; return NULL; }
    static void g3verdict(const g3line &L, const params &PA, bool &A, bool &C, bool &AC, bool &B)
    {
        const g3params &Q = PA.q3;
        bool expo = L.expo_s >= Q.expo_min;
        A = expo && L.stp_r >= Q.rep_a && L.z_ep >= Q.z_ep && L.z_st >= Q.z_st;
        C = L.c_tirs >= Q.tirs_min_c && L.c_r >= Q.rep_c && L.z_c >= Q.z_c;
        AC = C || A || (expo && L.stp_r >= Q.rep_a && L.z_st >= Q.z_st && L.c_r >= Q.ac_cmin && L.z_c >= Q.ac_zc);
        if(Q.noduel && L.duel) A = C = AC = false;
        int nr = 0;
        loopv(L.rt_tous) if(L.rt_tous[i] < Q.rapide) nr++;
        B = L.rt_tous.length() >= Q.nb && nr >= Q.nb_rapide && double(nr)/L.rt_tous.length() >= Q.part_rapide;
    }

    // F with the levels of the menu: enough points and far above chance, never against a single enemy
    static bool g3fverdict(const g3line &L, const params &PA)
    {
        const g3params &Q = PA.q3;
        if(Q.noduel && L.duel) return false;
        return L.f_pts_r >= Q.f_alert && L.f_pts_r >= Q.f_chance*L.f_pts_c;
    }

    static const g4line *g4of(const vector<g4line> &L, int key) { loopv(L) if(L[i].key == key) return &L[i]; return NULL; }
    static bool wallalert(const g4line &l, const params &PA)
    {
        return l.geo && l.W.touches && l.W.murs >= PA.wall_hits && l.W.murs >= PA.wall_share/100.0*l.W.touches;
    }
    static bool lagalert(const g4line &l, const params &PA)
    {
        return !l.L.note && l.L.frags_trous >= PA.lag_frags && l.L.z >= PA.lag_z && l.L.propres >= PA.lag_gaps;
    }
    static bool airalert(const g4line &l, const params &PA) { return l.geo && !l.V.note && l.V.v1_s >= PA.air_s; }
    // jumps and noclip: never an alert, only "to look at" (the studies could not measure them as well)
    static int jumpevents(const g4line &l) { return l.F.monte + l.F.remonte + l.F.plane_fort + (l.local ? 0 : l.F.float_ep); }
    static int noclipevents(const g4line &l) { return l.N.ep_propres + l.N.tr_propres; }
    static bool jumplook(const g4line &l, const params &PA) { return l.geo && !l.F.note && jumpevents(l) >= PA.jump_events; }
    static bool nocliplook(const g4line &l, const params &PA) { return l.geo && !l.N.note && noclipevents(l) >= PA.noclip_events; }

    // ------------------------------------------------------------ the page: table and moments
    // One line per player. Each check shows a short value: plain (measured, nothing found), red (over its alert
    // level), yellow (over its "look" level: jumps, noclip; never an alert) or grey (it could not judge: too little
    // data for its own rule, not measured, a single enemy). The numbers, what normal players do and the levels show
    // when the mouse rests on a cell (guitip). The verdict of a player follows the checks: Alert (one in red), Worth
    // a look (one in yellow), else OK. The checks are shown by kind of cheat, one question per tab: aim, wallhack,
    // movement. Nothing here changes a check or its result.
    enum { C_PLAYER = 0, C_PING, C_HITBOX, C_CLOCK, C_SPEED, C_SNAP, C_TRACK, C_TRIGGER, C_HIDDEN, C_WAIT, C_USUAL,
           C_WALLS, C_LAG, C_AIR, C_JUMPS, C_NOCLIP, C_HSHOTS, C_VERDICT, C_FOUND, C_FOLLOW, NUMCOLS };
    enum { CS_VALUE = 0, CS_ALERT, CS_LOOK, CS_FEW, CS_NODATA, CS_ONEENEMY };
    enum { V_OK = 0, V_LOOK, V_ALERT };
    // the checks that judge a player, by kind of cheat in the order of the tabs (Reaction and Usual are information)
    static const int judgecols[] = { C_SNAP, C_TRACK, C_TRIGGER, C_HITBOX, C_HIDDEN, C_HSHOTS, C_FOLLOW, C_WALLS, C_SPEED, C_AIR, C_JUMPS, C_NOCLIP, C_CLOCK, C_LAG };
    #define NUMJUDGE int(sizeof(judgecols)/sizeof(judgecols[0]))
    enum { F_AIM = 0, F_WALLHACK, F_MOVEMENT, NUMFAMILIES };
    static const char * const familynames[NUMFAMILIES] = { "Aim", "Wallhack", "Movement" };
    static int familyof(int col)
    {
        switch(col)
        {
            case C_SNAP: case C_TRACK: case C_TRIGGER: case C_WAIT: case C_HITBOX: return F_AIM;
            case C_HIDDEN: case C_HSHOTS: case C_FOLLOW: case C_WALLS: return F_WALLHACK;
            default: return F_MOVEMENT;
        }
    }
    static const char * const colnames[NUMCOLS] =
    {
        "Player", "Ping", "Hitbox", "Clock", "Speed", "Snap", "Tracking", "Trigger", "View on hidden", "Reaction", "Usual",
        "Walls", "Lag", "Air", "Jumps", "Noclip", "Shots at hidden", "Verdict", "What was found", "Follows hidden"
    };
    // what normal players do (the studies): shown next to the values, never used by a check
    static const double NORMHBSHARE = 0.112;      // hits off the body: 99 % of the players at most
    static const double NORMSNAPSHARE = 0.023;    // snap shots on an estimated aim: 99 % of the players at most
    static const int NORMFOLLOW = 25;             // points of following hidden enemies: the most of the players
    static int normmax(double share, int n) { return int(share*n + 0.5); }
    #define TIPLEN 600

    struct reprow
    {
        int key, verdict, enough, flagged;
        bool bot, recorder, any;
        double rank;
        string cell[NUMCOLS], found[NUMCOLS];
        bool alert[NUMCOLS], look[NUMCOLS];
        uchar state[NUMCOLS];
        char tip[NUMCOLS][TIPLEN];
    };
    struct repmoment { double t; int key, cn, type; bool alert; string text; };
    struct repnote { string text; char tip[TIPLEN]; };
    struct repgame
    {
        int part, duration;
        bool hd, los, trdone, g3done, g4done, local;
        string map, mode;
        vector<reprow> rows;
        vector<repmoment> moments;
    };
    struct report
    {
        bool valid;
        string label, path;
        int nparties;
        double secs;
        uint phash;
        vector<repgame> games;
        report() : valid(false), nparties(0), secs(0), phash(0) { label[0] = path[0] = 0; }
    };
    static report rep;
    static vector<repnote> repnotes;     // the names of the last analysis (all its games), kept when the table is rebuilt
    static int repgamesel = 0, repmomentpage = 0, reprowpage = 0;
    // what one page shows (the menu window is about 26 lines high in a 1600x900 window)
    #define ROWS_PAGE 16
    #define NOTES_PAGE 5
    static int repnotepage = 0;

    static const char *keyname(const job &j, const gameres &G, int key, char *buf, int len)
    {
        int s = G.names.get(key, -1);
        filtertext(buf, s >= 0 ? j.rd.str(s) : "?", false, false, len-1);
        if(!buf[0]) copystring(buf, "?", len);
        return buf;
    }

    // exact (for demogoto) or short (shown: minutes and seconds)
    static void fmttime(double ms, char *buf, int len, bool exact = true)
    {
        int m = int(ms + 0.5);
        if(m < 0) m = 0;
        if(exact) nformatstring(buf, len, "%d:%02d.%03d", m/60000, (m/1000)%60, m%1000);
        else nformatstring(buf, len, "%d:%02d", m/60000, (m/1000)%60);
    }

    static const char * const gunnames[7] = { "saw", "shotgun", "chaingun", "rocket", "rifle", "grenade", "pistol" };

    static void addmoment(repgame &rg, double t, int key, int type, bool alert, const char *fmt, ...) PRINTFARGS(6, 7);
    static void addmoment(repgame &rg, double t, int key, int type, bool alert, const char *fmt, ...)
    {
        repmoment &m = rg.moments.add();
        m.t = t; m.key = key; m.cn = key%1000; m.type = type; m.alert = alert;
        string tt;
        fmttime(t, tt, sizeof(tt), false);
        string body;
        va_list args;
        va_start(args, fmt);
        vformatstring(body, fmt, args);
        va_end(args);
        formatstring(m.text, "%s  %s", tt, body);
    }

    static bool momentless(const repmoment &a, const repmoment &b) { return a.t < b.t; }

    // a cell: its short text and what it shows (value, red, yellow, grey)
    static void cellset(reprow &r, int col, int state, const char *fmt, ...) PRINTFARGS(4, 5);
    static void cellset(reprow &r, int col, int state, const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vformatstring(r.cell[col], fmt, args);
        va_end(args);
        r.state[col] = uchar(state);
    }
    // its explanation when the mouse rests on it
    static void tipset(reprow &r, int col, const char *fmt, ...) PRINTFARGS(3, 4);
    static void tipset(reprow &r, int col, const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vformatstring(r.tip[col], fmt, args);
        va_end(args);
    }
    // a check that could not judge and has no value to show: grey "no data" (too few positions, not measured), "1 enemy"
    // (a check with a value under its own minimum shows that value in grey instead)
    static void cellgrey(reprow &r, int col, int state, const char *why)
    {
        copystring(r.cell[col], state == CS_ONEENEMY ? "1 enemy" : "no data");
        r.state[col] = uchar(state);
        copystring(r.tip[col], why, TIPLEN);
    }
    static const char *ordinal(int n, char *buf, int len)
    {
        int t = n%100, u = n%10;
        nformatstring(buf, len, "%d%s", n, t >= 11 && t <= 13 ? "th" : (u == 1 ? "st" : (u == 2 ? "nd" : (u == 3 ? "rd" : "th"))));
        return buf;
    }
    static const char * const NEEDMAP = "Needs the demo's map loaded: open this demo, then Analyze.";

    static void buildrepgame(const job &jb, const gameres &G, const params &PA, repgame &rg)
    {
        const rawdemo &rd = jb.rd;
        rg.part = G.part; rg.duration = G.t_fin;
        rg.hd = G.hdused; rg.los = G.losok; rg.trdone = G.trdone; rg.g3done = G.g3done; rg.g4done = G.g4geo;
        rg.local = G.rec >= 0;
        copystring(rg.map, G.map >= 0 ? rd.str(G.map) : "?");
        copystring(rg.mode, modename(G.mode));
        // ranking of the hidden enemies check (players exposed long enough)
        vector<const g3line *> ranked;
        loopv(G.g3) if(G.g3[i].measured && G.g3[i].expo_s >= PA.q3.expo_min) ranked.add(&G.g3[i]);
        loopv(G.g1.players)
        {
            const g1player &p = G.g1.players[i];
            if(!p.npos) continue;
            reprow &r = rg.rows.add();
            memset(r.alert, 0, sizeof(r.alert));
            memset(r.look, 0, sizeof(r.look));
            loopk(NUMCOLS) { r.cell[k][0] = r.found[k][0] = r.tip[k][0] = 0; r.state[k] = CS_VALUE; }
            r.key = p.key; r.bot = p.bot; r.recorder = p.key == G.rec; r.rank = -99;
            string nm;
            keyname(jb, G, p.key, nm, sizeof(nm));
            int same = 0;
            loopvj(rg.rows) if(j < rg.rows.length()-1 && !strcmp(rg.rows[j].cell[C_PLAYER], nm)) same++;
            if(same) concformatstring(nm, " #%d", same+1);
            if(r.bot) concatstring(nm, " (bot)");
            copystring(r.cell[C_PLAYER], nm);
            if(!isnand(p.ping)) formatstring(r.cell[C_PING], "%d", int(p.ping));
            else copystring(r.cell[C_PING], "-");
            {
                string pg;
                if(!isnand(p.ping)) formatstring(pg, "%d ms", int(p.ping));
                else copystring(pg, "not known");
                tipset(r, C_PLAYER, "Ping %s; %d shots, %d frags, %d deaths.%s A name alone proves nothing: names can be copied.",
                       pg, p.nshots, p.frags, p.deaths, r.recorder ? " He recorded this demo." : (r.bot ? " A bot of the game." : ""));
            }
            // hitbox: where the shooter's hits stopped, as he saw his victim
            r.alert[C_HITBOX] = hitboxalert(p, PA);
            if(!p.hb_touches) cellgrey(r, C_HITBOX, CS_NODATA, "No hit to check (rifle, chaingun and pistol hits only).");
            else
            {
                bool few = !r.alert[C_HITBOX] && p.hb_touches < PA.hb_hits;
                cellset(r, C_HITBOX, r.alert[C_HITBOX] ? CS_ALERT : (few ? CS_FEW : CS_VALUE), "%d of %d", p.hb_hors, p.hb_touches);
                tipset(r, C_HITBOX, "%d of %d hits stopped off the body as the shooter saw it, %.1f units from its axis in the middle. Normal players: up to 1 hit in 9, 4.1 units. Alert: %d hits, %d %% off and %.1f units.%s",
                       p.hb_hors, p.hb_touches, p.hb_med, PA.hb_hits, int(PA.hb_share*100 + 0.5), PA.hb_dist, few ? " Too few hits here to judge." : "");
                if(r.alert[C_HITBOX]) formatstring(r.found[C_HITBOX], "%d of %d hits off the body (normal 0-%d)", p.hb_hors, p.hb_touches, normmax(NORMHBSHARE, p.hb_touches));
            }
            // clock: the shooter's own clock against the demo's
            r.alert[C_CLOCK] = clockalert(p, PA);
            {
                bool ahead = !isnand(p.ca_avance);
                if(!r.alert[C_CLOCK] && !p.nshots) cellgrey(r, C_CLOCK, CS_NODATA, "No shot.");
                else if(!r.alert[C_CLOCK] && !ahead && !p.ca_early)
                {
                    cellset(r, C_CLOCK, CS_FEW, "%d shot%s", p.nshots, p.nshots == 1 ? "" : "s");
                    tipset(r, C_CLOCK, "%d shot%s: the shot clock needs 3 to be followed.", p.nshots, p.nshots == 1 ? "" : "s");
                }
                else
                {
                    int st = r.alert[C_CLOCK] ? CS_ALERT : CS_VALUE;
                    if(p.ca_early) cellset(r, C_CLOCK, st, "%d early", p.ca_early);
                    else if(ahead && p.ca_avance >= 100) cellset(r, C_CLOCK, st, "+%.1f s", p.ca_avance/1000.0);
                    else cellset(r, C_CLOCK, st, "on time");
                    string a;
                    if(ahead) formatstring(a, "His shot clock got at most %.1f s ahead of the demo (ahead: a game running too fast)", p.ca_avance/1000.0);
                    else copystring(a, "His shot clock could not be followed (fewer than 3 shots)");
                    tipset(r, C_CLOCK, "%s; %d shot%s came sooner than the weapon allows (%d checked). Normal players: under 0.1 s, never too early. Alert: %.1f s ahead or %d shot%s too early.",
                           a, p.ca_early, p.ca_early == 1 ? "" : "s", p.ca_paires, PA.clock/1000.0, PA.early, PA.early == 1 ? "" : "s");
                    if(r.alert[C_CLOCK])
                    {
                        if(ahead && p.ca_avance >= PA.clock) formatstring(r.found[C_CLOCK], "Shot clock %.1f s ahead (normal 0 s)", p.ca_avance/1000.0);
                        else formatstring(r.found[C_CLOCK], "%d shot%s fired too early (normal 0)", p.ca_early, p.ca_early == 1 ? "" : "s");
                    }
                }
            }
            // movement: speed announced, real speed, distance against the speed announced, warps
            r.alert[C_SPEED] = speedalert(p, PA);
            if(!p.vi_ok) cellgrey(r, C_SPEED, CS_NODATA, "No position to measure (edit mode).");
            else
            {
                double ping = isnand(p.ping) ? 0 : p.ping;
                bool fa = p.vi_trop_ms >= PA.sp_time, fb = p.vi_deprapide_ms >= PA.mv_time && ping <= PA.mv_ping,
                     fc = !isnand(p.vi_rapmed) && p.vi_rapmed >= PA.ratio && p.vi_fen_rap >= PA.ratio_win, fd = p.vi_sauts >= PA.jumps;
                int st = r.alert[C_SPEED] ? CS_ALERT : CS_VALUE;
                if(fb && !isnand(p.vi_depmax)) cellset(r, C_SPEED, st, "%.0f u/s", p.vi_depmax);
                else if(fa) cellset(r, C_SPEED, st, "%.1f s over", p.vi_trop_ms/1000.0);
                else if(fc) cellset(r, C_SPEED, st, "x%.1f speed", p.vi_rapmed);
                else if(fd) cellset(r, C_SPEED, st, "%d warps", p.vi_sauts);
                else if(!isnand(p.vi_depmax)) cellset(r, C_SPEED, st, "%.0f u/s", p.vi_depmax);
                else cellset(r, C_SPEED, CS_FEW, "no data");
                string top, rat;
                if(!isnand(p.vi_depmax)) formatstring(top, "Top speed %.0f units/s (500 ms with no push)", p.vi_depmax);
                else copystring(top, "Top speed not measured (never 500 ms with no push)");
                if(!isnand(p.vi_rapmed)) formatstring(rat, "%.2f times his announced speed (%d windows)", p.vi_rapmed, p.vi_fen_rap);
                else copystring(rat, "distance against his announced speed not measured");
                tipset(r, C_SPEED, "%s, over %d units/s for %.1f s; over the game's limit for %.1f s; %s; %d warp%s with no teleporter. Normal players: up to 212 units/s, under 0.1 s over the limit, 1.0 times, 0-2 warps. Alert: %.1f s over %d with a ping up to %d, %.1f s over the limit, %.1f times on %d windows, or %d warps.",
                       top, PA.mv_speed, p.vi_deprapide_ms/1000.0, p.vi_trop_ms/1000.0, rat, p.vi_sauts, p.vi_sauts == 1 ? "" : "s",
                       PA.mv_time/1000.0, PA.mv_speed, PA.mv_ping, PA.sp_time/1000.0, PA.ratio, PA.ratio_win, PA.jumps);
                if(r.alert[C_SPEED])
                {
                    if(fb && !isnand(p.vi_depmax)) formatstring(r.found[C_SPEED], "Moves at %.0f units/s (normal up to 212)", p.vi_depmax);
                    else if(fa) formatstring(r.found[C_SPEED], "Speed over the limit %.1f s (normal 0)", p.vi_trop_ms/1000.0);
                    else if(fc) formatstring(r.found[C_SPEED], "Moves %.1f times his speed (normal 1.0)", p.vi_rapmed);
                    else formatstring(r.found[C_SPEED], "%d warps with no teleporter (normal 0-2)", p.vi_sauts);
                }
            }
            // aim: snap and silent aim (shots), tracking (time on a moving enemy)
            const g2line *L = g2best(G.g2, p.key);
            if(!L || !L->snap_n) cellgrey(r, C_SNAP, CS_NODATA, "No shot measurable on his aim track.");
            if(!L || L->S.proche_s <= 0) cellgrey(r, C_TRACK, CS_NODATA, "Never near a moving enemy with his view.");
            if(L)
            {
                bool snap, silent, track;
                g2verdict(*L, PA, snap, silent, track);
                r.alert[C_SNAP] = snap || silent;
                r.alert[C_TRACK] = track;
                const char *aim = L->var ? "the exact aim of this demo" : "his aim estimated from the server's positions";
                if(L->snap_n)
                {
                    bool few = !r.alert[C_SNAP] && L->snap_n < 10;
                    if(silent && !snap) cellset(r, C_SNAP, CS_ALERT, "%d silent", L->sil_k);
                    else cellset(r, C_SNAP, r.alert[C_SNAP] ? CS_ALERT : (few ? CS_FEW : CS_VALUE), "%d of %d", L->snap_k, L->snap_n);
                    string nrm;
                    if(L->var || !normmax(NORMSNAPSHARE, L->snap_n)) copystring(nrm, "0");
                    else formatstring(nrm, "0-%d", normmax(NORMSNAPSHARE, L->snap_n));
                    tipset(r, C_SNAP, "His view jumped onto the target in one step for %d of %d shots; %d shot%s left far from the view (%s). Normal players: %s. Alert: %d shots and %.1f deviations above the players.%s",
                           L->snap_k, L->snap_n, L->sil_k, L->sil_k == 1 ? "" : "s", aim, nrm, PA.q2.rep_snap, PA.q2.Z, few ? " Fewer than 10 shots measured here: too few to judge." : "");
                    if(snap) formatstring(r.found[C_SNAP], "Snapped onto %d of %d shots (normal %s)", L->snap_k, L->snap_n, nrm);
                    else if(silent) formatstring(r.found[C_SNAP], "%d shots far from the view (normal 0)", L->sil_k);
                }
                if(L->S.proche_s > 0)
                {
                    bool few = !track && (L->S.proche_s < PA.q2.proche_min || L->fenetres < PA.q2.rep_win);
                    int share = isnand(L->part_sur) ? 0 : int(L->part_sur*100 + 0.5);
                    cellset(r, C_TRACK, track ? CS_ALERT : (few ? CS_FEW : CS_VALUE), "%d %%", share);
                    const double *pu = L->var ? PA.q2.pop_hd_sur : PA.q2.pop_dmo_sur;
                    tipset(r, C_TRACK, "His view stayed on a moving enemy near it %d %% of the time (%.0f s near, held %d times; %s). Normal players: about %d %%, up to %d %%. Alert: %.0f s near, %d times held and %.1f deviations above the players (about %d %%).%s",
                           share, L->S.proche_s, L->fenetres, aim, L->var ? 38 : 37, L->var ? 50 : 52, PA.q2.proche_min, PA.q2.rep_win, PA.q2.Ztrack,
                           int((pu[0] + PA.q2.Ztrack*pu[1])*100 + 0.5),
                           !few ? "" : (L->S.proche_s < PA.q2.proche_min ? " Too little time near moving enemies here to judge." : " Held too few times here to judge."));
                    if(track) formatstring(r.found[C_TRACK], "On a moving target %d %% (normal %d %%)", share, L->var ? 38 : 37);
                }
            }
            // trigger: flick shots fired as the view reaches the body
            const trline *T = trof(G.tr, p.key);
            if(T && T->nflicks)
            {
                r.alert[C_TRIGGER] = tralert(*T, PA);
                bool few = !r.alert[C_TRIGGER] && (T->nflicks < PA.tr_flicks || T->npos < PA.tr_flicks);
                int b = isnand(T->before) ? 0 : int(T->before*100 + 0.5), e = T->npos && !isnand(T->band) ? int(T->band*100 + 0.5) : 0;
                int st = r.alert[C_TRIGGER] ? CS_ALERT : (few ? CS_FEW : CS_VALUE);
                if(T->npos) cellset(r, C_TRIGGER, st, "%d%%, %d%%", b, e);
                else cellset(r, C_TRIGGER, st, "%d%%", b);
                tipset(r, C_TRIGGER, "Of his %d flick shots, %d %% left before the view reached the body's centre; of the %d measured on the body, %d %% went in at its entry edge (a trigger fires on first contact). Normal players: about 41 %% and 22 %%. Alert: %d flick shots and both over %d %% and %d %%.%s",
                       T->nflicks, b, T->npos, e, PA.tr_flicks, int(PA.trbefore()*100 + 0.5), int(PA.tredge()*100 + 0.5), few ? " Too few flick shots here to judge." : "");
                if(r.alert[C_TRIGGER]) formatstring(r.found[C_TRIGGER], "Fires on first contact %d %% (normal 22 %%)", e);
            }
            else cellgrey(r, C_TRIGGER, CS_NODATA, G.trdone ? "No flick shot." : NEEDMAP);
            // hidden enemies: the view held on an enemy behind a wall (A), shots at hidden enemies (C), waiting (B,
            // information); an alert of A and C together turns both cells red
            const g3line *H = g3of(G.g3, p.key);
            if(H)
            {
                bool A, C, AC, B;
                g3verdict(*H, PA, A, C, AC, B);
                (void)B;
                int rank = 0;
                if(H->expo_s >= PA.q3.expo_min) { rank = 1; loopvj(ranked) if(ranked[j]->rank > H->rank) rank++; }
                r.alert[C_HIDDEN] = AC;
                r.rank = H->rank;
                bool both = AC && !A && !C, duel = PA.q3.noduel && H->duel;
                const char *duelwhy = " Only one enemy here: knowing where he is is normal, never an alert (setting).";
                {
                    bool few = !(A || both) && H->expo_s < PA.q3.expo_min;
                    if(duel) cellset(r, C_HIDDEN, CS_ONEENEMY, "1 enemy");
                    else cellset(r, C_HIDDEN, A || both ? CS_ALERT : (few ? CS_FEW : CS_VALUE), "%d (chance %.1f)", H->stp_r, H->stp_c);
                    string rk = "";
                    if(rank) { string o; formatstring(rk, ", %s of %d players near them", ordinal(rank, o, sizeof(o)), ranked.length()); }
                    tipset(r, C_HIDDEN, "His view held on an enemy behind a wall that he could not know about %d times, %.1f by chance (the same enemies 20 s earlier or later); %.0f s near hidden enemies%s. Normal players: about as often as chance. Alert: %.0f s near, %d times and far from chance, or with the shots at hidden enemies.%s",
                           H->stp_r, H->stp_c, H->expo_s, rk, PA.q3.expo_min, PA.q3.rep_a, duel ? duelwhy : (few ? " Too little time near hidden enemies here to judge." : ""));
                    if(A || both) formatstring(r.found[C_HIDDEN], "Views on hidden enemies %d (chance %.1f)", H->stp_r, H->stp_c);
                }
                {
                    bool few = !(C || both) && H->c_tirs < PA.q3.tirs_min_c;
                    if(duel) cellset(r, C_HSHOTS, CS_ONEENEMY, "1 enemy");
                    else cellset(r, C_HSHOTS, C || both ? CS_ALERT : (few ? CS_FEW : CS_VALUE), "%d (chance %.1f)", H->c_r, H->c_c);
                    tipset(r, C_HSHOTS, "%d shot%s aimed at an enemy behind a wall that he could not know about (not just seen or heard, nearer to him than to where he was known), %.1f by chance, out of %d shots. Normal players: 0-2 in a game, fewer than chance. Alert: %d shots and far from chance, or %d with the views far from chance.%s",
                           H->c_r, H->c_r == 1 ? "" : "s", H->c_c, H->c_tirs, PA.q3.rep_c, PA.q3.ac_cmin, duel ? duelwhy : (few ? " Too few shots here to judge." : ""));
                    if(C || both) formatstring(r.found[C_HSHOTS], "%d shots at hidden enemies (normal 0-2)", H->c_r);
                }
                {
                    // F: the view following a hidden enemy who moves, in points (exact, near and long count most)
                    bool F = g3fverdict(*H, PA);
                    r.alert[C_FOLLOW] = F;
                    if(duel) cellset(r, C_FOLLOW, CS_ONEENEMY, "1 enemy");
                    else cellset(r, C_FOLLOW, F ? CS_ALERT : CS_VALUE, "%.0f (chance %.0f)", H->f_pts_r, H->f_pts_c);
                    string best = "";
                    const g3follow *lg = NULL;
                    loopvj(H->follows) if(!lg || H->follows[j].pts > lg->pts) lg = &H->follows[j];
                    if(lg)
                    {
                        string tt, nb;
                        fmttime(lg->t, tt, sizeof(tt), false);
                        keyname(jb, G, lg->E, nb, sizeof(nb));
                        formatstring(best, " Best: %s at %s, %.1f s, %.0f units away, %.1f degrees off.", nb, tt, lg->ms/1000.0, lg->dist, lg->err);
                    }
                    tipset(r, C_FOLLOW, "His view followed an enemy who moved behind a wall, where nothing he saw or heard could tell him (not where he last knew him, nor further along his way): %d time%s of 1 s or more, %.1f s in all, %d shot%s at him meanwhile; %.0f points, %.1f by chance (the same enemies 20 s earlier or later). A point a tenth of a second, more when exact, near and long, %.0f a shot.%s Normal players: 0-%d. Alert: %.0f points and %.0f times chance.%s",
                           H->f_n_r, H->f_n_r == 1 ? "" : "s", H->f_s_r, H->f_shots, H->f_shots == 1 ? "" : "s", H->f_pts_r, H->f_pts_c, PA.q3.f_shot, best, NORMFOLLOW, PA.q3.f_alert, PA.q3.f_chance, duel ? duelwhy : "");
                    if(F) formatstring(r.found[C_FOLLOW], "Follows hidden enemies %.0f points (normal 0-%d)", H->f_pts_r, NORMFOLLOW);
                }
                {
                    int nr = 0, n = H->rt_tous.length();
                    loopvj(H->rt_tous) if(H->rt_tous[j] < PA.q3.rapide) nr++;
                    if(!n) cellgrey(r, C_WAIT, CS_NODATA, "No reaction measured: an enemy coming out from behind a wall where his crosshair was already placed, then his shot.");
                    else
                    {
                        cellset(r, C_WAIT, n < PA.q3.nb ? CS_FEW : CS_VALUE, "%d of %d fast", nr, n);
                        tipset(r, C_WAIT, "His crosshair was already placed where the enemy came out from behind a wall (he was waiting there): %d reaction%s, %d faster than %.0f ms, median %.0f ms. Normal players: median about 490 ms, about 1 in 20 under 150 ms. Information only, never an alert: waiting for an enemy is normal play.",
                               n, n == 1 ? "" : "s", nr, PA.q3.rapide, npmedian(H->rt_tous));
                    }
                }
            }
            else
            {
                const char *why = G.g3done ? "Not measured for this player." : NEEDMAP;
                cellgrey(r, C_HIDDEN, CS_NODATA, why);
                cellgrey(r, C_HSHOTS, CS_NODATA, why);
                cellgrey(r, C_FOLLOW, CS_NODATA, why);
                cellgrey(r, C_WAIT, CS_NODATA, why);
            }
            // walls, lag, air (alerts); jumps and noclip (yellow "look": never an alert)
            const g4line *W4 = g4of(G.g4, p.key);
            if(!W4)
            {
                const char *why = "Not measured in this analysis.";
                cellgrey(r, C_WALLS, CS_NODATA, why); cellgrey(r, C_LAG, CS_NODATA, why); cellgrey(r, C_AIR, CS_NODATA, why);
                cellgrey(r, C_JUMPS, CS_NODATA, why); cellgrey(r, C_NOCLIP, CS_NODATA, why);
            }
            else
            {
                r.alert[C_LAG] = lagalert(*W4, PA);
                if(W4->L.note == 1) cellgrey(r, C_LAG, CS_NODATA, "Not measurable: the player who recorded this local demo and his bots, or the other player of a duel recorded locally.");
                else if(W4->L.note) cellgrey(r, C_LAG, CS_FEW, "Too few positions to measure.");
                else
                {
                    int st = r.alert[C_LAG] ? CS_ALERT : CS_VALUE;
                    if(!W4->L.propres) cellset(r, C_LAG, st, "no gaps");
                    else cellset(r, C_LAG, st, "%d frag%s", W4->L.frags_trous, W4->L.frags_trous == 1 ? "" : "s");
                    tipset(r, C_LAG, "%d gap%s of 300 ms or more in his positions, %d frag%s right after them, %.1f expected by chance. Normal players: 0-2 frags after gaps (a bad connection gives gaps with no frags). Alert: %d frags, %.1f deviations from chance and %d gaps.",
                           W4->L.propres, W4->L.propres == 1 ? "" : "s", W4->L.frags_trous, W4->L.frags_trous == 1 ? "" : "s", W4->L.attendu, PA.lag_frags, PA.lag_z, PA.lag_gaps);
                    if(r.alert[C_LAG]) formatstring(r.found[C_LAG], "%d frags right after lag (chance %.1f)", W4->L.frags_trous, W4->L.attendu);
                }
                if(!W4->geo)
                {
                    cellgrey(r, C_WALLS, CS_NODATA, NEEDMAP); cellgrey(r, C_AIR, CS_NODATA, NEEDMAP);
                    cellgrey(r, C_JUMPS, CS_NODATA, NEEDMAP); cellgrey(r, C_NOCLIP, CS_NODATA, NEEDMAP);
                }
                else
                {
                    r.alert[C_WALLS] = wallalert(*W4, PA);
                    if(!W4->W.touches) cellgrey(r, C_WALLS, CS_NODATA, "No hit.");
                    else
                    {
                        bool few = !r.alert[C_WALLS] && W4->W.touches < PA.wall_hits;
                        cellset(r, C_WALLS, r.alert[C_WALLS] ? CS_ALERT : (few ? CS_FEW : CS_VALUE), "%d of %d", W4->W.murs, W4->W.touches);
                        tipset(r, C_WALLS, "%d of his %d hits crossed at least 1 unit of wall on an enemy he could not see. Normal players: 0-1 in a game. Alert: %d hits and %d %% of his hits.%s",
                               W4->W.murs, W4->W.touches, PA.wall_hits, PA.wall_share, few ? " Too few hits here to judge." : "");
                        if(r.alert[C_WALLS]) formatstring(r.found[C_WALLS], "%d of %d hits through walls (normal 0-1)", W4->W.murs, W4->W.touches);
                    }
                    if(W4->V.note) cellgrey(r, C_AIR, W4->V.note == 1 ? CS_NODATA : CS_FEW, W4->V.note == 1 ? "Edit mode: not measured." : "Too few positions to measure.");
                    else
                    {
                        r.alert[C_AIR] = airalert(*W4, PA);
                        if(W4->V.v1_s > 0) cellset(r, C_AIR, r.alert[C_AIR] ? CS_ALERT : CS_VALUE, "%.1f s", W4->V.v1_s);
                        else cellset(r, C_AIR, CS_VALUE, "0 s");
                        tipset(r, C_AIR, "%.1f s in the air with a fall that never grows, far from any floor (the game always pulls down). High above any floor for %.0f s from the positions only (information: jumping on and on over a gap gives some). Normal players: 0 s. Alert: %.1f s.",
                               W4->V.v1_s, W4->V.v2_s, PA.air_s);
                        if(r.alert[C_AIR]) formatstring(r.found[C_AIR], "%.1f s in the air, no fall (normal 0)", W4->V.v1_s);
                    }
                    if(W4->F.note) cellgrey(r, C_JUMPS, W4->F.note == 1 ? CS_NODATA : CS_FEW, W4->F.note == 1 ? "Edit mode: not measured." : "Too few positions to measure.");
                    else
                    {
                        int e = jumpevents(*W4);
                        r.look[C_JUMPS] = jumplook(*W4, PA);
                        cellset(r, C_JUMPS, r.look[C_JUMPS] ? CS_LOOK : CS_VALUE, "%d", e);
                        string fl = "";
                        if(!W4->local) formatstring(fl, ", %d floating", W4->F.float_ep);
                        tipset(r, C_JUMPS, "%d jump%s the game's gravity cannot make: %d higher than a jump, %d climbing again in the air, %d too long%s. Normal players: 0-5. Yellow from %d: worth a look, never an alert.",
                               e, e == 1 ? "" : "s", W4->F.monte, W4->F.remonte, W4->F.plane_fort, fl, PA.jump_events);
                        if(r.look[C_JUMPS]) formatstring(r.found[C_JUMPS], "%d jumps gravity can't make (normal 0-5)", e);
                    }
                    if(W4->N.note) cellgrey(r, C_NOCLIP, W4->N.note == 1 ? CS_NODATA : CS_FEW, W4->N.note == 1 ? "Edit mode: not measured." : "Too few positions to measure.");
                    else
                    {
                        int e = noclipevents(*W4);
                        r.look[C_NOCLIP] = nocliplook(*W4, PA);
                        cellset(r, C_NOCLIP, r.look[C_NOCLIP] ? CS_LOOK : CS_VALUE, "%d", e);
                        tipset(r, C_NOCLIP, "The body was inside a wall %d time%s and passed through one %d time%s, except where other players also go. Normal players: 0. Yellow from %d: worth a look, never an alert.",
                               W4->N.ep_propres, W4->N.ep_propres == 1 ? "" : "s", W4->N.tr_propres, W4->N.tr_propres == 1 ? "" : "s", PA.noclip_events);
                        if(r.look[C_NOCLIP]) formatstring(r.found[C_NOCLIP], "%d times inside walls (normal 0)", e);
                    }
                }
            }
            // information only (never red): how far this game stands from the name's usual games
            {
                const usualnote *u = NULL;
                loopvj(jb.usual) if(jb.usual[j].key == p.key) u = &jb.usual[j];
                if(!u)
                {
                    copystring(r.cell[C_USUAL], "no history");
                    r.state[C_USUAL] = CS_NODATA;
                    tipset(r, C_USUAL, "No history for this name: fewer than %d games known on this computer, or a bot, or a default name. Nothing to compare. Information only.", demoanalysisrupturegames);
                }
                else
                {
                    bool distant = fabs(u->z) >= demoanalysisrupturez;
                    cellset(r, C_USUAL, CS_VALUE, "%s", distant ? "unusual" : "usual");
                    tipset(r, C_USUAL, "Compared with his %d usual games on this computer: %s %+.1f deviations, the measure that moved most (unusual from %.1f). Information only, never an alert.",
                           u->games, u->label, u->z, demoanalysisrupturez);
                }
                // no kind of cheat: shown with the mouse on his name, like his ping
                string us;
                if(!u) formatstring(us, "\nHis usual games: none known (fewer than %d games on this computer, or a bot, or a default name).", demoanalysisrupturegames);
                else formatstring(us, "\nThis game against his %d usual games on this computer: %s (%s %+.1f deviations, the measure that moved most; unusual from %.1f). Information only.",
                                  u->games, r.cell[C_USUAL], u->label, u->z, demoanalysisrupturez);
                concatstring(r.tip[C_PLAYER], us, TIPLEN);
            }
            // the verdict: a check in red, else in yellow, else OK
            r.any = false;
            bool anylook = false;
            loopk(NUMCOLS) if(k != C_USUAL && r.alert[k]) r.any = true;
            loopk(NUMCOLS) if(r.look[k]) anylook = true;
            // the name in red when the player has an alert in any column (of either view)
            r.alert[C_PLAYER] = r.any;
            r.verdict = r.any ? V_ALERT : (anylook ? V_LOOK : V_OK);
            r.enough = r.flagged = 0;
            int first = -1;
            loopk(NUMJUDGE)
            {
                int c = judgecols[k], s = r.state[c];
                if(s == CS_VALUE || s == CS_ALERT || s == CS_LOOK) r.enough++;
                if(s == CS_ALERT || s == CS_LOOK) r.flagged++;
                if(s == CS_ALERT && first < 0) first = c;
            }
            if(first < 0) loopk(NUMJUDGE) if(r.state[judgecols[k]] == CS_LOOK && first < 0) first = judgecols[k];
            r.state[C_PLAYER] = r.state[C_VERDICT] = uchar(r.verdict == V_ALERT ? CS_ALERT : (r.verdict == V_LOOK ? CS_LOOK : CS_VALUE));
            copystring(r.cell[C_VERDICT], r.verdict == V_ALERT ? "Alert" : (r.verdict == V_LOOK ? "Worth a look" : "OK"));
            if(first >= 0)
            {
                if(r.flagged > 1) formatstring(r.cell[C_FOUND], "%s +%d", r.found[first], r.flagged - 1);
                else copystring(r.cell[C_FOUND], r.found[first]);
            }
            else if(r.enough) formatstring(r.cell[C_FOUND], "Nothing found (%d of %d checks)", r.enough, NUMJUDGE);
            else copystring(r.cell[C_FOUND], "Too little data to judge");
        }
        // Alert first, then Worth a look, then OK; each in the order the players connected
        {
            vector<reprow> sorted;
            for(int v = V_ALERT; v >= V_OK; v--) loopv(rg.rows) if(rg.rows[i].verdict == v) sorted.add(rg.rows[i]);
            rg.rows.shrink(0);
            loopv(sorted) rg.rows.add(sorted[i]);
        }
        // moments of each check (by default only for the players with an alert or a "look")
        auto rowof = [&](int key) -> const reprow * { loopv(rg.rows) if(rg.rows[i].key == key) return &rg.rows[i]; return NULL; };
        string a, b;
        loopv(G.g1.moments)
        {
            const moment &m = G.g1.moments[i];
            const reprow *r = rowof(m.key);
            if(!r) continue;
            keyname(jb, G, m.key, a, sizeof(a));
            switch(m.type)
            {
                case M_HITBOX:
                    keyname(jb, G, m.other, b, sizeof(b));
                    addmoment(rg, m.t, m.key, m.type, r->alert[C_HITBOX], "%s hit %s %.1f units off the axis (normal 4.1)", a, b, m.v1);
                    break;
                case M_CLOCK: addmoment(rg, m.t, m.key, m.type, r->alert[C_CLOCK], "%s: shot clock %.1f s ahead (up to %.1f s; normal 0)", a, m.v1/1000.0, m.v2/1000.0); break;
                case M_EARLY: addmoment(rg, m.t, m.key, m.type, r->alert[C_CLOCK], "%s: %s shot %d ms after the last (it needs %d)", a, m.v2 >= 0 && m.v2 < 7 ? gunnames[int(m.v2)] : "?", int(m.v1), int(m.v3)); break;
                case M_SPEED: addmoment(rg, m.t, m.key, m.type, r->alert[C_SPEED], "%s: announces %.0f units/s (the game allows %.0f)", a, m.v1, m.v2); break;
                case M_MOVE: addmoment(rg, m.t, m.key, m.type, r->alert[C_SPEED], "%s: moves at %.0f units/s for %.1f s (normal up to 212)", a, m.v1, m.v2/1000.0); break;
                case M_JUMP: addmoment(rg, m.t, m.key, m.type, r->alert[C_SPEED], "%s: warps %.0f units in %d ms, no teleporter", a, m.v1, int(m.v2)); break;
            }
        }
        loopv(G.g2)
        {
            const g2line *L = g2best(G.g2, G.g2[i].key);
            const reprow *r = rowof(G.g2[i].key);
            if(!L || !r) continue;
            keyname(jb, G, L->key, a, sizeof(a));
            loopvj(L->rows)
            {
                const snaprow &x = L->rows[j];
                if(signale_snap(x, L->var, PA.q2)) addmoment(rg, x.t, L->key, M_SNAP, r->alert[C_SNAP], "%s: view jumps %.0f degrees onto the shot", a, x.J1);
                else if(fiable(x, L->var, PA.q2) && pyround(x.piste_rayon_seg, 2) > (L->var ? PA.q2.sil_hd : PA.q2.sil_dmo))
                    addmoment(rg, x.t, L->key, M_SNAP, r->alert[C_SNAP], "%s: shot %.0f degrees away from the view", a, x.piste_rayon_seg);
            }
        }
        loopv(G.tr)
        {
            const trline &T = G.tr[i];
            const reprow *r = rowof(T.sid);
            if(!r) continue;
            keyname(jb, G, T.sid, a, sizeof(a));
            loopvj(T.bandtimes) addmoment(rg, T.bandtimes[j], T.sid, M_TRIGGER, r->alert[C_TRIGGER], "%s: flick shot on the body's entry edge", a);
        }
        loopv(G.g3)
        {
            const g3line &H = G.g3[i];
            const reprow *r = rowof(H.key);
            if(!H.measured || !r) continue;
            keyname(jb, G, H.key, a, sizeof(a));
            loopvj(H.episodes)
            {
                const g3episode &e = H.episodes[j];
                keyname(jb, G, e.E, b, sizeof(b));
                addmoment(rg, e.t, H.key, M_HIDDENAIM, r->alert[C_HIDDEN], "%s: view on hidden %s for %.1f s%s", a, b, e.ms/1000.0, e.suit ? ", follows him" : "");
            }
            loopvj(H.cshots)
            {
                const g3shotc &c = H.cshots[j];
                keyname(jb, G, c.E, b, sizeof(b));
                addmoment(rg, c.t, H.key, M_HIDDENSHOT, r->alert[C_HIDDEN] || (c.suit && r->alert[C_FOLLOW]), "%s: shot at hidden %s (%.1f degrees off)%s", a, b, c.err, c.suit ? ", following him" : "");
            }
            loopvj(H.follows)
            {
                const g3follow &w = H.follows[j];
                keyname(jb, G, w.E, b, sizeof(b));
                addmoment(rg, w.t, H.key, M_FOLLOW, r->alert[C_FOLLOW], "%s follows hidden %s %.1f s, %.0f u, %.1f deg", a, b, w.ms/1000.0, w.dist, w.err);
            }
            loopvj(H.apps)
            {
                const g3app &x = H.apps[j];
                if(!x.hasrt || x.rt >= PA.q3.rapide || x.err > dmax(3.0, 2.0*x.half)) continue;
                keyname(jb, G, x.E, b, sizeof(b));
                addmoment(rg, x.t, H.key, M_WAIT, false, "%s: shot %s %d ms after he came out", a, b, int(x.rt));
            }
        }
        loopv(G.g4)
        {
            const g4line &l = G.g4[i];
            const reprow *r = rowof(l.key);
            if(!r) continue;
            keyname(jb, G, l.key, a, sizeof(a));
            loopvj(l.W.list)
            {
                const g4wallhit &h = l.W.list[j];
                if(!h.wall) continue;
                keyname(jb, G, h.victim, b, sizeof(b));
                addmoment(rg, h.t, l.key, M_WALL, r->alert[C_WALLS], "%s hit %s through a wall (%s, %.0f units)", a, b, h.gun >= 0 && h.gun < 7 ? gunnames[h.gun] : "?", h.dist);
            }
            loopvj(l.L.list)
            {
                const g4gap &g = l.L.list[j];
                addmoment(rg, g.t, l.key, M_LAG, r->alert[C_LAG], "%s: no position for %d ms, then %d frag%s", a, int(g.ms), g.frags, g.frags > 1 ? "s" : "");
            }
            loopvj(l.V.v1) addmoment(rg, l.V.v1[j].t, l.key, M_AIR, r->alert[C_AIR], "%s: %.1f s in the air, no fall (normal 0)", a, l.V.v1[j].s);
            loopvj(l.F.ev)
            {
                const g4fev &e = l.F.ev[j];
                bool sh = r->look[C_JUMPS];
                switch(e.kind)
                {
                    case G4F_RISE: addmoment(rg, e.t, l.key, M_JUMPF, sh, "%s: rises %.0f units above his last floor (a jump: about 21)", a, e.v1); break;
                    case G4F_CLIMB: addmoment(rg, e.t, l.key, M_JUMPF, sh, "%s: climbs %.0f units again in the air", a, e.v1); break;
                    case G4F_LONG:
                        if(e.v3 < 0) addmoment(rg, e.t, l.key, M_JUMPF, sh, "%s: %.1f s in the air, %.0f units up (a jump: %.1f s at most)", a, e.v1/1000.0, -e.v3, (e.v2 - 250)/1000.0);
                        else addmoment(rg, e.t, l.key, M_JUMPF, sh, "%s: %.1f s in the air, %.0f units down (a jump: %.1f s at most)", a, e.v1/1000.0, e.v3, (e.v2 - 250)/1000.0);
                        break;
                    case G4F_FLOAT: addmoment(rg, e.t, l.key, M_JUMPF, sh, "%s: floats in the air for %.1f s", a, e.v1); break;
                }
            }
            loopvj(l.N.eps) if(!l.N.eps[j].shared) addmoment(rg, l.N.eps[j].t, l.key, M_NOCLIP, r->look[C_NOCLIP], "%s: inside a wall (%d positions)", a, int(l.N.eps[j].v1));
            loopvj(l.N.trav) if(!l.N.trav[j].shared) addmoment(rg, l.N.trav[j].t, l.key, M_NOCLIP, r->look[C_NOCLIP], "%s: passes through %.1f units of wall", a, l.N.trav[j].v2);
        }
        rg.moments.sort(momentless);
    }

    // the settings themselves (the bytes of params also hold padding that nothing sets)
    static uint paramhash()
    {
        const double v[] =
        {
            double(demoanalysishitboxhits), double(demoanalysishitboxshare), demoanalysishitboxdist, double(demoanalysisclock),
            double(demoanalysisearly), double(demoanalysisspeedtime), double(demoanalysismovespeed), double(demoanalysismovetime),
            double(demoanalysismoveping), demoanalysisratio, double(demoanalysisratiowindows), double(demoanalysisjumps),
            demoanalysissnapz, double(demoanalysissnaprepeat), demoanalysistrackz, double(demoanalysistrackwindows),
            double(demoanalysistrackseconds), double(demoanalysistriggerflicks), demoanalysistriggerz, double(demoanalysissound),
            double(demoanalysiscold), double(demoanalysisexposure), double(demoanalysishiddenrepeat), demoanalysishiddenz,
            double(demoanalysishiddenshots), double(demoanalysiscombinedshots), demoanalysiscombinedz, double(demoanalysiswaitfast),
            double(demoanalysiswaitcount), double(demoanalysiswaitfastcount), double(demoanalysisduel), double(demoanalysisallmoments),
            double(demoanalysiswalls), double(demoanalysiswallshare), double(demoanalysislagfrags), demoanalysislagz,
            double(demoanalysislaggaps), demoanalysisair, double(demoanalysisjumplook), double(demoanalysisnocliplook),
            double(demoanalysisfollow)
        };
        uint h = 2166136261U;
        const uchar *p = (const uchar *)v;
        loopi(int(sizeof(v))) { h ^= p[i]; h *= 16777619U; }
        return h;
    }

    // the page's report, rebuilt when a threshold changes
    static void buildreport()
    {
        if(!last) { rep.valid = false; return; }
        params PA;
        currentparams(PA);
        rep.valid = true;
        copystring(rep.label, last->label);
        copystring(rep.path, last->dmopath);
        rep.nparties = last->rd.nsegs;
        rep.secs = last->secs;
        rep.phash = paramhash();
        rep.games.shrink(0);
        loopv(last->games) buildrepgame(*last, *last->games[i], PA, rep.games.add());
        if(repgamesel >= rep.games.length()) repgamesel = 0;
    }

    static report &currentreport()
    {
        if(last && (!rep.valid || rep.phash != paramhash())) buildreport();
        return rep;
    }

    // ------------------------------------------------------------ history (this computer only)
    // "moving average": per name and measure only n, mean and spread (Welford) and
    // the 5 last values; nothing else (no positions, no chat, no files). A demo is
    // counted once (its id is kept 30 days).
    struct hmeas { string key; int n; double mean, m2; double lastv[5]; int nlast; };
    struct hplayer { string name, base; int games, alerts, first, lastday; vector<hmeas> m; };
    static vector<hplayer *> hist;
    static vector<ullong> hdemos;
    static vector<int> hdays;
    static bool histloaded = false;
    #define HISTFILE "demoanalysis-history.txt"

    // player stats, kept in the same file (its version 2): "g <mode> <map> <games>", the games read by mode and
    // map; then for each name "q <name>" and its records "s <mode> <map> <n> <n numbers> [<bin>:<count> ...]", one
    // per mode and map (sums, the last day, the best streak; the reactions by 10 ms). A file of version 1 reads as
    // before; an older game reading this one keeps its own lines and drops these. Names and maps are written in hex.
    enum
    {
        SV_GAMES = 0, SV_TIME, SV_LASTDAY, SV_FRAGS, SV_DEATHS, SV_SUICIDES, SV_TEAMKILLS, SV_STREAK, SV_PINGSUM, SV_PINGN,
        SV_BACKFRAGS, SV_BACKFRAGSN, SV_BACKDEATHS, SV_BACKDEATHSN, SV_RECGAMES, SV_KILLEDREC, SV_KILLEDBYREC, SV_BACKONREC,
        SV_BACKBYREC, SV_FLAGTAKEN, SV_FLAGSCORED, SV_FLAGRETURNED, SV_CARRIERFRAGS, SV_CAMPGAMES, SV_OWNHALF, SV_ENEMYHALF,
        SV_NEARBASE, SV_STILL, SV_STILLN, SV_STILLFRAGS, SV_DEALT, SV_TAKEN, SV_ALERTGAMES, SV_ALHITBOX, SV_ALCLOCK, SV_ALMOVE,
        SV_ALAIM, SV_ALHIDDEN, SV_ALWALLS, SV_G3GAMES, SV_G4GAMES, SV_WEAPON0,
        SV_NVALS = SV_WEAPON0 + 6*ST_WEAPONS
    };
    enum { SW_SHOTS = 0, SW_HITS, SW_DEALT, SW_TAKEN, SW_FRAGS, SW_DEATHS };
    #define SV_W(w, k) (SV_WEAPON0 + 6*(w) + (k))
    #define ST_BINS 151
    struct strecord { int mode; string map; double v[SV_NVALS]; int bins[ST_BINS]; };
    struct stname { string name; vector<strecord> recs; };
    struct stmodemap { int mode; string map; int games; };
    static vector<stname *> stnames;
    static vector<stmodemap> stgames;
    static bool stchanged = true;        // the table of the page is built again

    static const char *basename_(const char *n, char *out, int len)
    {
        // without clan tags ([..] |..| (..) {..} <..>), letters and digits, lower case
        string s = "";
        int k = 0;
        for(const char *p = n; *p; p++)
        {
            char c = *p, e = c == '[' ? ']' : (c == '|' ? '|' : (c == '(' ? ')' : (c == '{' ? '}' : (c == '<' ? '>' : 0))));
            if(e)
            {
                const char *q = strchr(p+1, e);
                if(q) { p = q; continue; }
            }
            if(k < MAXSTRLEN-1) s[k++] = c;
        }
        s[k] = 0;
        int o = 0;
        for(const char *p = s; *p && o < len-1; p++) if(isalnum(uchar(*p))) out[o++] = char(tolower(uchar(*p)));
        out[o] = 0;
        if(!o) { o = 0; for(const char *p = n; *p && o < len-1; p++) out[o++] = char(tolower(uchar(*p))); out[o] = 0; }
        return out;
    }

    static bool defaultname(const char *n)
    {
        string s;
        int o = 0;
        for(const char *p = n; *p && o < MAXSTRLEN-1; p++) if(!isspace(uchar(*p))) s[o++] = char(tolower(uchar(*p)));
        s[o] = 0;
        return !s[0] || !strcmp(s, "unnamed") || !strcmp(s, "bot");
    }

    static int today() { return int(time(NULL)/86400); }

    static int stemday(const char *stem)
    {
        int y, mo, d;
        if(stem && sscanf(stem, "%4d-%2d-%2d", &y, &mo, &d) == 3 && y > 2000 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31)
        {
            // days since 1970-01-01 (civil calendar)
            y -= mo <= 2;
            int era = (y >= 0 ? y : y - 399)/400, yoe = y - era*400, doy = (153*(mo + (mo > 2 ? -3 : 9)) + 2)/5 + d - 1;
            int doe = yoe*365 + yoe/4 - yoe/100 + doy;
            return era*146097 + doe - 719468;
        }
        return today();
    }

    static void hexname(const char *n, char *out, int len)
    {
        int o = 0;
        for(const uchar *p = (const uchar *)n; *p && o + 2 < len; p++) o += snprintf(out + o, len - o, "%02x", *p);
        out[o] = 0;
    }
    static void unhexname(const char *h, char *out, int len)
    {
        int o = 0;
        for(const char *p = h; p[0] && p[1] && o < len-1; p += 2) { uint c; if(sscanf(p, "%2x", &c) != 1) break; out[o++] = char(c); }
        out[o] = 0;
    }

    static hplayer *histfind(const char *name)
    {
        loopv(hist) if(!strcmp(hist[i]->name, name)) return hist[i];
        return NULL;
    }
    static hmeas *measfind(hplayer *p, const char *key)
    {
        loopv(p->m) if(!strcmp(p->m[i].key, key)) return &p->m[i];
        return NULL;
    }

    static void histload()
    {
        if(histloaded) return;
        histloaded = true;
        stream *f = openutf8file(path(HISTFILE, true), "r");
        if(!f) return;
        char line[4096];
        hplayer *p = NULL;
        stname *sn = NULL;
        while(f->getline(line, sizeof(line)))
        {
            if(line[0] == 'd')
            {
                uint hi, lo; int day;
                if(sscanf(line, "d %8x%8x %d", &hi, &lo, &day) == 3) { hdemos.add((ullong(hi)<<32) | lo); hdays.add(day); }
            }
            else if(line[0] == 'p')
            {
                int g, a, fi, la;
                char hx[600];
                if(sscanf(line, "p %d %d %d %d %599s", &g, &a, &fi, &la, hx) != 5) { p = NULL; continue; }
                p = new hplayer;
                unhexname(hx, p->name, sizeof(p->name));
                basename_(p->name, p->base, sizeof(p->base));
                p->games = g; p->alerts = a; p->first = fi; p->lastday = la;
                hist.add(p);
            }
            else if(line[0] == 'm' && p)
            {
                hmeas m;
                int k = 0;
                char key[64];
                int used = 0;
                if(sscanf(line, "m %63s %d %lf %lf %d%n", key, &m.n, &m.mean, &m.m2, &k, &used) < 5) continue;
                copystring(m.key, key);
                m.nlast = 0;
                const char *q = line + used;
                loopi(min(k, 5)) { double v; int u = 0; if(sscanf(q, " %lf%n", &v, &u) != 1) break; m.lastv[m.nlast++] = v; q += u; }
                p->m.add(m);
            }
            else if(line[0] == 'g')
            {
                int mo, gm;
                char mh[600];
                if(sscanf(line, "g %d %599s %d", &mo, mh, &gm) != 3) continue;
                stmodemap &c = stgames.add();
                c.mode = mo;
                unhexname(mh, c.map, sizeof(c.map));
                c.games = gm;
            }
            else if(line[0] == 'q')
            {
                char hx[600];
                if(sscanf(line, "q %599s", hx) != 1) { sn = NULL; continue; }
                sn = new stname;
                unhexname(hx, sn->name, sizeof(sn->name));
                stnames.add(sn);
            }
            else if(line[0] == 's' && sn)
            {
                int mo, nv, used = 0;
                char mh[600];
                if(sscanf(line, "s %d %599s %d%n", &mo, mh, &nv, &used) < 3) continue;
                strecord &r = sn->recs.add();
                r.mode = mo;
                unhexname(mh, r.map, sizeof(r.map));
                memset(r.v, 0, sizeof(r.v));
                memset(r.bins, 0, sizeof(r.bins));
                const char *q = line + used;
                loopi(nv) { double x; int u = 0; if(sscanf(q, " %lf%n", &x, &u) != 1) break; if(i < SV_NVALS) r.v[i] = x; q += u; }
                for(;;) { int b, c, u = 0; if(sscanf(q, " %d:%d%n", &b, &c, &u) != 2) break; if(b >= 0 && b < ST_BINS) r.bins[b] += c; q += u; }
            }
        }
        delete f;
    }

    static void histsave()
    {
        stream *f = openutf8file(path(HISTFILE, true), "w");
        if(!f) return;
        f->printf("v %d\n", stnames.length() || stgames.length() ? 2 : 1);
        int now = today();
        loopv(hdemos) if(now - hdays[i] <= 30) f->printf("d %08x%08x %d\n", uint(hdemos[i]>>32), uint(hdemos[i]&0xFFFFFFFFU), hdays[i]);
        loopv(hist)
        {
            hplayer &p = *hist[i];
            char hx[600];
            hexname(p.name, hx, sizeof(hx));
            f->printf("p %d %d %d %d %s\n", p.games, p.alerts, p.first, p.lastday, hx);
            loopvj(p.m)
            {
                const hmeas &m = p.m[j];
                f->printf("m %s %d %.17g %.17g %d", m.key, m.n, m.mean, m.m2, m.nlast);
                loopk(m.nlast) f->printf(" %.17g", m.lastv[k]);
                f->printf("\n");
            }
        }
        loopv(stgames)
        {
            char mh[600];
            hexname(stgames[i].map, mh, sizeof(mh));
            f->printf("g %d %s %d\n", stgames[i].mode, mh[0] ? mh : "-", stgames[i].games);
        }
        loopv(stnames)
        {
            const stname &s = *stnames[i];
            char hx[600];
            hexname(s.name, hx, sizeof(hx));
            f->printf("q %s\n", hx);
            loopvj(s.recs)
            {
                const strecord &r = s.recs[j];
                char mh[600];
                hexname(r.map, mh, sizeof(mh));
                f->printf("s %d %s %d", r.mode, mh[0] ? mh : "-", int(SV_NVALS));
                loopk(SV_NVALS) f->printf(" %.0f", r.v[k]);
                loopk(ST_BINS) if(r.bins[k]) f->printf(" %d:%d", k, r.bins[k]);
                f->printf("\n");
            }
        }
        delete f;
    }

    // measures kept for one row of a game (the studies' history: groups 1, 2 and 3)
    struct hval { const char *key; double v; };
    static void rowmeasures(const job &j, const gameres &G, const g1player &p, vector<hval> &out)
    {
        auto add = [&](const char *k, double v) { if(finited(v)) { hval &h = out.add(); h.key = k; h.v = v; } };
        if(p.hb_touches >= 5) { add("hbpart", pyround(p.hb_part, 3)); add("hbmed", pyround(p.hb_med, 2)); }
        if(!isnand(p.ca_avance)) add("clock", p.ca_avance);
        if(p.vi_ok)
        {
            if(p.vi_fen_rap >= 5) add("ratio", pyround(p.vi_rapmed, 3));
            if(!isnand(p.vi_depmax)) add("depmax", pyround(p.vi_depmax, 1));
            add("fastms", p.vi_trop_ms);
            add("jumps", p.vi_sauts);
        }
        const g2line *L = g2best(G.g2, p.key);
        const g2player *gp = NULL;
        loopv(G.g2) if(G.g2[i].key == p.key) gp = &G.g2[i];
        if(gp && p.key != G.rec && !isnand(gp->ping)) add("ping", pyround(gp->ping, 0));
        if(L && L->snap_n >= 20)
        {
            add("acc", L->precision); add("j1med", L->J1_q50); add("j1big", L->J1_q90);
            add("snap", double(L->snap_k)/L->snap_n); add("silent", double(L->sil_k)/L->snap_n);
        }
        if(gp && gp->vivant_s >= 60) { add("shotsmin", gp->tirs_vises/(gp->vivant_s/60.0)); add("fragsmin", p.frags/(gp->vivant_s/60.0)); }
        if(L && L->S.proche_s >= 20) add("ontarget", L->part_sur);
        const trline *T = trof(G.tr, p.key);
        if(T && T->nflicks >= 15 && T->npos >= 15) { add("trbefore", T->before); add("tredge", T->band); }
        const g3line *H = g3of(G.g3, p.key);
        if(H)
        {
            if(H->expo_s >= 120) { add("zep", H->z_ep); add("zst", H->z_st); }
            if(H->c_tirs >= 20) add("zc", H->z_c);
        }
        (void)j;
    }

    struct hfloor { const char *key, *label; double floor; bool print, cheat; };
    // floors: how much a measure moves from one game to the next for a same player (studies 1 and 2)
    static const hfloor hfloors[] =
    {
        { "ping", "ping", 15, true, false }, { "acc", "accuracy", 0.0612, true, false }, { "shotsmin", "shots a minute", 3.57, true, false },
        { "fragsmin", "frags a minute", 1.16, true, false }, { "j1med", "aim moves", 0.848, true, false }, { "j1big", "big aim moves", 4.72, true, false },
        { "ontarget", "tracking", 0.058, true, true }, { "clock", "shot clock", 20, true, true }, { "ratio", "movement", 0.005, true, true },
        { "hbpart", "hitbox", 0.086, true, true }, { "hbmed", "hit distance", 0.67, false, true }, { "depmax", "speed", 22.9, false, true },
        { "fastms", "speed", 50, false, true }, { "jumps", "jumps", 1, false, true }, { "snap", "snap", 0.01, false, true },
        { "silent", "silent aim", 0.01, false, true }, { "trbefore", "trigger", 0.128, false, true }, { "tredge", "trigger", 0.113, false, true },
        { "zep", "hidden aim", 1.31, false, true }, { "zst", "hidden aim", 1.19, false, true }, { "zc", "hidden shots", 0.44, false, true },
    };
    static const hfloor *floorof(const char *k) { loopi(int(sizeof(hfloors)/sizeof(hfloors[0]))) if(!strcmp(hfloors[i].key, k)) return &hfloors[i]; return NULL; }

    // distance of a value to a name's usual games (3 by default), in deviations with a floor
    static bool zof(const hplayer *p, const char *k, double v, double &z)
    {
        if(!p) return false;
        const hmeas *m = NULL;
        loopv(p->m) if(!strcmp(p->m[i].key, k)) m = &p->m[i];
        const hfloor *fl = floorof(k);
        if(!m || m->n < demoanalysisrupturegames || !fl) return false;
        double sd = m->n > 1 ? sqrt(m->m2/(m->n - 1)) : 0;
        z = (v - m->mean)/dmax(dmax(sd, fl->floor), 1e-6);
        return true;
    }

    // the checks in red for one player of a game, with the levels of the menu (as the page colours them): hitbox,
    // clock, movement, aim (snap, silent aim, tracking, trigger), hidden enemies, walls / lag / air; false when none
    enum { AF_HITBOX = 0, AF_CLOCK, AF_MOVE, AF_AIM, AF_HIDDEN, AF_WALLS, NUMAF };
    static bool alertfamilies(const gameres &G, int key, const params &PA, bool *af)
    {
        loopi(NUMAF) af[i] = false;
        loopv(G.g1.players) if(G.g1.players[i].key == key)
        {
            const g1player &q = G.g1.players[i];
            af[AF_HITBOX] = hitboxalert(q, PA); af[AF_CLOCK] = clockalert(q, PA); af[AF_MOVE] = speedalert(q, PA);
        }
        const g2line *L = g2best(G.g2, key);
        if(L) { bool a1, a2, a3; g2verdict(*L, PA, a1, a2, a3); af[AF_AIM] = a1 || a2 || a3; }
        const trline *T = trof(G.tr, key);
        if(T && tralert(*T, PA)) af[AF_AIM] = true;
        const g3line *H = g3of(G.g3, key);
        if(H) { bool A, C, AC, B; g3verdict(*H, PA, A, C, AC, B); af[AF_HIDDEN] = AC || g3fverdict(*H, PA); }
        const g4line *W = g4of(G.g4, key);
        if(W) af[AF_WALLS] = wallalert(*W, PA) || lagalert(*W, PA) || airalert(*W, PA);
        loopi(NUMAF) if(af[i]) return true;
        return false;
    }

    // the players worth a look in an analysis: a check in red, neither a bot nor the recorder of the demo
    static int worthalook(const job &jb, const params &PA)
    {
        int n = 0;
        loopv(jb.games)
        {
            const gameres &G = *jb.games[i];
            loopvj(G.g1.players)
            {
                const g1player &p = G.g1.players[j];
                bool af[NUMAF];
                if(p.npos && !p.bot && p.key != G.rec && alertfamilies(G, p.key, PA, af)) n++;
            }
        }
        return n;
    }

    // the player stats of one game, added to the records of its names (once per demo, as the history)
    static strecord &strecordof(stname *s, int mode, const char *map)
    {
        loopv(s->recs) if(s->recs[i].mode == mode && !strcmp(s->recs[i].map, map)) return s->recs[i];
        strecord &r = s->recs.add();
        r.mode = mode;
        copystring(r.map, map);
        memset(r.v, 0, sizeof(r.v));
        memset(r.bins, 0, sizeof(r.bins));
        return r;
    }

    static void statsadd(const job &jb, const gameres &G, const params &PA, int day)
    {
        if(!G.stdone) return;
        const char *map = G.map >= 0 ? jb.rd.str(G.map) : "";
        int added = 0;
        loopv(G.st)
        {
            const stplayer &p = G.st[i];
            if(p.bot) continue;
            string nm;
            keyname(jb, G, p.key, nm, sizeof(nm));
            if(defaultname(nm)) continue;
            stname *s = NULL;
            loopvj(stnames) if(!strcmp(stnames[j]->name, nm)) { s = stnames[j]; break; }
            if(!s) { s = new stname; copystring(s->name, nm); stnames.add(s); }
            strecord &r = strecordof(s, G.mode, map);
            const stcounts &c = p.c;
            double *v = r.v;
            v[SV_GAMES] += 1;
            v[SV_TIME] += c.time_ms;
            v[SV_LASTDAY] = dmax(v[SV_LASTDAY], day);
            v[SV_FRAGS] += c.frags; v[SV_DEATHS] += c.deaths; v[SV_SUICIDES] += c.suicides; v[SV_TEAMKILLS] += c.teamkills;
            v[SV_STREAK] = dmax(v[SV_STREAK], c.streak);
            v[SV_PINGSUM] += c.pingsum; v[SV_PINGN] += c.pingn;
            v[SV_BACKFRAGS] += c.backfrags; v[SV_BACKFRAGSN] += c.backfragsn; v[SV_BACKDEATHS] += c.backdeaths; v[SV_BACKDEATHSN] += c.backdeathsn;
            if(c.recgame)
            {
                v[SV_RECGAMES] += 1;
                v[SV_KILLEDREC] += c.killedrec; v[SV_KILLEDBYREC] += c.killedbyrec; v[SV_BACKONREC] += c.backonrec; v[SV_BACKBYREC] += c.backbyrec;
            }
            v[SV_FLAGTAKEN] += c.flagtaken; v[SV_FLAGSCORED] += c.flagscored; v[SV_FLAGRETURNED] += c.flagreturned; v[SV_CARRIERFRAGS] += c.carrierfrags;
            if(c.camp) { v[SV_CAMPGAMES] += 1; v[SV_OWNHALF] += c.ownhalf_ms; v[SV_ENEMYHALF] += c.enemyhalf_ms; v[SV_NEARBASE] += c.nearbase_ms; }
            v[SV_STILL] += c.still_ms; v[SV_STILLN] += c.still_n; v[SV_STILLFRAGS] += c.still_frags;
            v[SV_DEALT] += c.dealt; v[SV_TAKEN] += c.taken;
            loopk(ST_WEAPONS)
            {
                v[SV_W(k, SW_SHOTS)] += c.w[k].shots; v[SV_W(k, SW_HITS)] += c.w[k].hits;
                v[SV_W(k, SW_DEALT)] += c.w[k].dealt; v[SV_W(k, SW_TAKEN)] += c.w[k].taken;
                v[SV_W(k, SW_FRAGS)] += c.w[k].frags; v[SV_W(k, SW_DEATHS)] += c.w[k].deaths;
            }
            // the checks: the games where he was in red (the levels of the menu at that time)
            bool af[NUMAF];
            if(alertfamilies(G, p.key, PA, af)) v[SV_ALERTGAMES] += 1;
            if(g3of(G.g3, p.key)) v[SV_G3GAMES] += 1;
            if(g4of(G.g4, p.key)) v[SV_G4GAMES] += 1;
            if(af[AF_HITBOX]) v[SV_ALHITBOX] += 1;
            if(af[AF_CLOCK]) v[SV_ALCLOCK] += 1;
            if(af[AF_MOVE]) v[SV_ALMOVE] += 1;
            if(af[AF_AIM]) v[SV_ALAIM] += 1;
            if(af[AF_HIDDEN]) v[SV_ALHIDDEN] += 1;
            if(af[AF_WALLS]) v[SV_ALWALLS] += 1;
            loopvj(p.rt) r.bins[min(max(p.rt[j], 0)/10, ST_BINS-1)]++;
            added++;
        }
        // the games of each mode and map (the filters of the page): games with at least one name
        if(!added) return;
        bool found = false;
        loopv(stgames) if(stgames[i].mode == G.mode && !strcmp(stgames[i].map, map)) { stgames[i].games++; found = true; break; }
        if(!found) { stmodemap &c = stgames.add(); c.mode = G.mode; copystring(c.map, map); c.games = 1; }
        stchanged = true;
    }

    // a measure of the history as the notes show it (its key says its unit), and "label value, usual value"
    static void histvalue(const char *key, double v, char *buf, int len)
    {
        if(!strcmp(key, "ping")) nformatstring(buf, len, "%.0f ms", v);
        else if(!strcmp(key, "clock") || !strcmp(key, "fastms")) nformatstring(buf, len, "%.1f s", v/1000.0);
        else if(!strcmp(key, "ratio")) nformatstring(buf, len, "%.2f", v);
        else if(!strcmp(key, "depmax")) nformatstring(buf, len, "%.0f units/s", v);
        else if(!strcmp(key, "hbmed")) nformatstring(buf, len, "%.1f units", v);
        else if(!strcmp(key, "j1med") || !strcmp(key, "j1big")) nformatstring(buf, len, "%.1f degrees", v);
        else if(!strcmp(key, "shotsmin") || !strcmp(key, "fragsmin") || !strcmp(key, "zep") || !strcmp(key, "zst") || !strcmp(key, "zc")) nformatstring(buf, len, "%.1f", v);
        else if(!strcmp(key, "jumps")) nformatstring(buf, len, "%.0f", v);
        else nformatstring(buf, len, "%.0f %%", v*100);
    }
    static void histdetail(char *out, int len, const char *label, const char *key, double v, const hmeas *usual)
    {
        string a, b;
        histvalue(key, v, a, sizeof(a));
        if(usual) histvalue(key, usual->mean, b, sizeof(b));
        else copystring(b, "?");
        int n = strlen(out);
        if(n < len - 1) nformatstring(out + n, len - n, "%s%s %s, usual %s", n ? "; " : "", label, a, b);
    }

    static void historyrows(job *jb, bool update, vector<repnote> *notes)
    {
        histload();
        bool counted = false;
        loopv(hdemos) if(hdemos[i] == jb->demoid) counted = true;
        params PA;
        currentparams(PA);
        jb->usual.setsize(0);
        bool updhist = update && demoanalysishistory, updstats = update && demoanalysisstats;
        loopv(jb->games)
        {
            gameres &G = *jb->games[i];
            int day = stemday(jb->stem[0] ? jb->stem : jb->label);
            // names in this game, for the clan tag check
            vector<int> keys;
            loopvj(G.g1.players) if(G.g1.players[j].npos && !G.g1.players[j].bot) keys.add(G.g1.players[j].key);
            loopvj(keys)
            {
                string na, ba;
                keyname(*jb, G, keys[j], na, sizeof(na));
                if(defaultname(na)) continue;
                basename_(na, ba, sizeof(ba));
                loopk(j)
                {
                    string nb, bb;
                    keyname(*jb, G, keys[k], nb, sizeof(nb));
                    if(defaultname(nb) || !strcmp(na, nb)) continue;
                    basename_(nb, bb, sizeof(bb));
                    if(!strcmp(ba, bb) && notes)
                    {
                        repnote &n = notes->add();
                        formatstring(n.text, "Two players, one name: %s and %s (the same name without clan tags, both in this game).", nb, na);
                        copystring(n.tip, "Both names were in this game at the same time: two different players, one copies the other's name or clan tag. A name alone proves nothing.", TIPLEN);
                    }
                }
            }
            loopvj(G.g1.players)
            {
                const g1player &p = G.g1.players[j];
                if(!p.npos || p.bot) continue;
                string nm;
                keyname(*jb, G, p.key, nm, sizeof(nm));
                if(defaultname(nm)) continue;
                vector<hval> m;
                rowmeasures(*jb, G, p, m);
                if(m.empty()) continue;
                hplayer *h = histfind(nm);
                // distance to his usual games (the largest of the cheat measures)
                usualnote un; un.key = p.key; un.z = 0; un.label = ""; un.games = h ? h->games : 0;
                loopvk(m)
                {
                    const hfloor *fl = floorof(m[k].key);
                    double z;
                    if(fl && fl->cheat && zof(h, m[k].key, m[k].v, z) && fabs(z) > fabs(un.z)) { un.z = z; un.label = fl->label; }
                }
                if(un.z) jb->usual.add(un);
                // the name's usual way to play (fingerprint of study 2): 2 measures at 3.5 deviations after 3 games (settings)
                if(notes)
                {
                    if(h && h->games >= demoanalysisrupturegames)
                    {
                        string what = "", detail = "";
                        int strong = 0;
                        loopvk(m)
                        {
                            const hfloor *fl = floorof(m[k].key);
                            double z;
                            if(fl && fl->print && zof(h, m[k].key, m[k].v, z) && fabs(z) >= demoanalysisrupturez)
                            {
                                strong++;
                                concformatstring(what, "%s%s", what[0] ? ", " : "", fl->label);
                                histdetail(detail, sizeof(detail), fl->label, m[k].key, m[k].v, measfind(h, m[k].key));
                            }
                        }
                        if(strong >= demoanalysisrupturemeasures)
                        {
                            repnote &n = notes->add();
                            formatstring(n.text, "%s: unlike his %d usual games (%s).", nm, h->games, what);
                            nformatstring(n.tip, TIPLEN, "%s. Another player under this name, another game, or a cheat: indications, not proof.", detail);
                        }
                    }
                    else if(!h)
                    {
                        string ba;
                        basename_(nm, ba, sizeof(ba));
                        loopvk(hist)
                        {
                            hplayer *o = hist[k];
                            if(o->games < demoanalysisrupturegames || strcmp(o->base, ba) || !strcmp(o->name, nm)) continue;
                            string what = "", detail = "";
                            int strong = 0, compared = 0;
                            loopl(m.length())
                            {
                                const hfloor *fl = floorof(m[l].key);
                                double z;
                                if(!fl || !fl->print || !zof(o, m[l].key, m[l].v, z)) continue;
                                compared++;
                                if(fabs(z) >= demoanalysisrupturez)
                                {
                                    strong++;
                                    concformatstring(what, "%s%s", what[0] ? ", " : "", fl->label);
                                    histdetail(detail, sizeof(detail), fl->label, m[l].key, m[l].v, measfind(o, m[l].key));
                                }
                            }
                            if(!compared) continue;
                            repnote &n = notes->add();
                            if(strong >= demoanalysisrupturemeasures)
                            {
                                formatstring(n.text, "%s: same name as %s without the clan tag, plays differently (%s).", nm, o->name, what);
                                nformatstring(n.tip, TIPLEN, "Compared with the %d usual games of %s: %s. Another player, or the same one playing differently: indications, not proof.", o->games, o->name, detail);
                            }
                            else
                            {
                                // grey: too few measures compared for the comparison to tell anything
                                bool few = compared < demoanalysisrupturemeasures;
                                formatstring(n.text, "%s%s: same name as %s without the clan tag, plays alike (%d measure%s compared).", few ? "\f4" : "", nm, o->name, compared, compared == 1 ? "" : "s");
                                nformatstring(n.tip, TIPLEN, "Compared with the %d usual games of %s on %d measure%s: %d far from them (%.1f deviations or more; %d needed to say he plays differently).%s",
                                              o->games, o->name, compared, compared == 1 ? "" : "s", strong, demoanalysisrupturez, demoanalysisrupturemeasures, few ? " Too few measures compared to tell: grey." : "");
                            }
                        }
                    }
                }
                if(!updhist || counted) continue;
                if(!h)
                {
                    h = new hplayer;
                    copystring(h->name, nm);
                    basename_(nm, h->base, sizeof(h->base));
                    h->games = h->alerts = 0;
                    h->first = day;
                    hist.add(h);
                }
                h->games++;
                h->lastday = day;
                bool alert = hitboxalert(p, PA) || clockalert(p, PA) || speedalert(p, PA);
                const g2line *L = g2best(G.g2, p.key);
                if(L) { bool a1, a2, a3; g2verdict(*L, PA, a1, a2, a3); alert = alert || a1 || a2 || a3; }
                const trline *T = trof(G.tr, p.key);
                if(T && tralert(*T, PA)) alert = true;
                const g3line *H3 = g3of(G.g3, p.key);
                if(H3) { bool A, C, AC, B; g3verdict(*H3, PA, A, C, AC, B); alert = alert || AC || g3fverdict(*H3, PA); }
                if(alert) h->alerts++;
                loopvk(m)
                {
                    hmeas *e = measfind(h, m[k].key);
                    if(!e) { hmeas &ne = h->m.add(); copystring(ne.key, m[k].key); ne.n = 0; ne.mean = ne.m2 = 0; ne.nlast = 0; e = &ne; }
                    int n = e->n + 1;
                    double x = m[k].v, d = x - e->mean, mean = e->mean + d/n;
                    e->m2 += d*(x - mean);
                    e->mean = mean;
                    e->n = n;
                    if(e->nlast == 5) { memmove(e->lastv, e->lastv+1, 4*sizeof(double)); e->nlast = 4; }
                    e->lastv[e->nlast++] = x;
                }
            }
            if(updstats && !counted) statsadd(*jb, G, PA, day);
        }
        if((updhist || updstats) && !counted)
        {
            hdemos.add(jb->demoid);
            hdays.add(today());
            histsave();
        }
    }

    static void historyupdate(job *j)
    {
        if(!demoanalysis || !(demoanalysishistory || demoanalysisstats) || !j->readok || j->games.empty()) return;
        historyrows(j, true, NULL);
    }

    // ------------------------------------------------------------ demos worth a look (the list of the demos)
    // demoanalysis-demos.txt in the profile: the local demos where an analysis (asked or automatic) found a player
    // worth a look. "s <stem>": not checked yet, another colour in the list and the automatic clean-up keeps it;
    // "c <stem>": checked (the button Mark as checked, or the demo opened), an ordinary demo again. Lines of demos
    // gone from the demo folder are dropped. Nothing is written until an analysis finds something. With the demo
    // analysis off, the list and the clean-up are as before and the file is neither read nor written (the marks are
    // there again when the analysis is turned back on).
    struct suspectdemo { string stem; bool checked; };
    static vector<suspectdemo> suspects;
    static bool suspectsloaded = false;
    #define SUSPECTFILE "demoanalysis-demos.txt"

    static bool localdemo(const char *stem)
    {
        if(!stem || !stem[0]) return false;
        defformatstring(rel, "demo/%s.dmo", stem);
        path(rel);
        const char *found = findfile(rel, "rb");
        return found && fileexists(found, "r");
    }

    static void suspectsload()
    {
        if(suspectsloaded) return;
        suspectsloaded = true;
        stream *f = openutf8file(path(SUSPECTFILE, true), "r");
        if(!f) return;
        char line[1024];
        while(f->getline(line, sizeof(line)))
        {
            char st[8], hx[600];
            if(sscanf(line, "%7s %599s", st, hx) != 2 || (strcmp(st, "s") && strcmp(st, "c"))) continue;
            suspectdemo &d = suspects.add();
            unhexname(hx, d.stem, sizeof(d.stem));
            d.checked = st[0] == 'c';
        }
        delete f;
    }

    static void suspectssave()
    {
        stream *f = openutf8file(path(SUSPECTFILE, true), "w");
        if(!f) return;
        loopv(suspects)
        {
            if(!localdemo(suspects[i].stem)) continue;
            char hx[600];
            hexname(suspects[i].stem, hx, sizeof(hx));
            f->printf("%s %s\n", suspects[i].checked ? "c" : "s", hx);
        }
        delete f;
    }

    static suspectdemo *suspectof(const char *stem)
    {
        if(!stem || !stem[0]) return NULL;
        suspectsload();
        loopv(suspects) if(!strcasecmp(suspects[i].stem, stem)) return &suspects[i];
        return NULL;
    }

    // a local demo where this analysis found a player worth a look; the demo being watched is checked already
    static void marksuspect(const job *j)
    {
        if(!demoanalysis || !localdemo(j->stem)) return;
        bool playing = game::demoplayback && playpath[0] && j->dmopath[0] && !strcmp(playpath, j->dmopath);
        suspectdemo *d = suspectof(j->stem);
        if(d) { if(playing && !d->checked) { d->checked = true; suspectssave(); } return; }
        suspectdemo &n = suspects.add();
        copystring(n.stem, j->stem);
        n.checked = playing;
        suspectssave();
    }

    // the automatic clean-up of the demos (demohd prune) keeps a demo worth a look not checked yet, while the demo
    // analysis is on; with the analysis off, the clean-up is the usual one
    bool keepdemo(const char *stem)
    {
        if(!demoanalysis) return false;
        suspectdemo *d = suspectof(stem);
        return d && !d->checked;
    }

    static void checkdemo(const char *stem)
    {
        if(!demoanalysis) return;
        suspectdemo *d = suspectof(stem);
        if(d && !d->checked) { d->checked = true; suspectssave(); }
    }
    ICOMMAND(demoanalysesuspect, "s", (char *stem), intret(keepdemo(stem) ? 1 : 0));
    ICOMMAND(demoanalysecheck, "s", (char *stem), checkdemo(stem));

    // ------------------------------------------------------------ the page shows an analysis
    static int alertunseen = 0;      // players worth a look in the automatic analysis, until its page is opened

    // asked, or automatic at the end of a recorded game (then a line in the console when a player is worth a look)
    static void showanalysis(job *j, bool asked)
    {
        // the players worth a look: neither the bots nor the recorder
        params PA;
        currentparams(PA);
        int n = j->readok ? worthalook(*j, PA) : 0;
        // an automatic analysis with nobody worth a look does not replace one whose line was not seen yet
        if(!asked && !n && alertunseen && last) return;
        if(last && last != waiting) delete last;
        last = j;
        vector<repnote> notes;
        if(j->readok && j->games.length()) historyrows(j, asked, &notes);
        repnotes.shrink(0);
        loopv(notes)
        {
            bool dup = false;
            loopvj(repnotes) if(!strcmp(repnotes[j].text, notes[i].text)) dup = true;
            if(!dup) repnotes.add(notes[i]);
        }
        buildreport();
        repmomentpage = reprowpage = repnotepage = 0;
        if(asked) alertunseen = 0;
        if(!n) return;
        marksuspect(j);
        if(!asked)
        {
            alertunseen = n;
            conoutf("Demo analysis: %d player%s with an alert", n, n > 1 ? "s" : "");
        }
    }
    ICOMMAND(demoanalysealert, "", (),
    {
        if(!alertunseen || !last) { result(""); return; }
        defformatstring(s, "Demo analysis: %d player%s with an alert", alertunseen, alertunseen > 1 ? "s" : "");
        result(s);
    });
    ICOMMAND(demoanalyseseen, "", (), alertunseen = 0);

    static void finishjob(job *j)
    {
        if(j->cancel) { delete j; return; }
        if(j->automatic)
        {
            // the automatic analysis of a recorded game: its page (and its line) now, during the intermission;
            // the history and the player stats once the recording is kept
            if(demoanalysis && demoanalysisauto && j->readok && j->games.length()) showanalysis(j, false);
            if(j->confirm > 0) historyupdate(j);
            if(j->confirm == 0) { if(waiting && waiting != last) delete waiting; waiting = j; return; }
            if(j != last) delete j;
            return;
        }
        // an analysis asked: the page, and the history (the comparison is made before the game is added)
        showanalysis(j, true);
    }

    // ------------------------------------------------------------ commands
    static void analysecmd(const char *file)
    {
        if(!demoanalysis) { conoutf("\f2Demo analysis is off (Recording, Demo analysis)"); return; }
        string dmo = "";
        if(!file || !file[0])
        {
            if(!game::demoplayback || !playpath[0]) { conoutf("\f2Open a demo on this computer first, or type /demoanalyse followed by a demo file"); return; }
            copystring(dmo, playpath);
        }
        else
        {
            string tries[4];
            copystring(tries[0], file);
            formatstring(tries[1], "%s.dmo", file);
            formatstring(tries[2], "demo/%s", file);
            formatstring(tries[3], "demo/%s.dmo", file);
            loopi(4)
            {
                path(tries[i]);
                const char *found = findfile(tries[i], "rb");
                if(found && fileexists(found, "r")) { copystring(dmo, found); break; }
            }
            if(!dmo[0]) { conoutf("\f2Demo not found: %s", file); return; }
        }
        job *j = new job;
        copystring(j->dmopath, dmo);
        string hd;
        sidecarof(dmo, hd, sizeof(hd));
        if(fileexists(hd, "r")) copystring(j->hdpath, hd);
        // the file name alone (a path found by the game can mix both separators)
        const char *b = strrchr(dmo, '\\'), *b2 = strrchr(dmo, '/');
        if(!b || (b2 && b2 > b)) b = b2;
        copystring(j->label, b ? b+1 : dmo);
        copystring(j->stem, j->label);
        int n = strlen(j->stem);
        if(n >= 4 && !strcasecmp(j->stem + n - 4, ".dmo")) j->stem[n-4] = 0;
        askjob(j);
    }
    ICOMMAND(demoanalyse, "s", (char *file), analysecmd(file));

    static repgame *selgame()
    {
        report &r = currentreport();
        if(!r.valid || r.games.empty()) return NULL;
        if(repgamesel < 0 || repgamesel >= r.games.length()) repgamesel = 0;
        return &r.games[repgamesel];
    }

    // shown moments: of the players with an alert, or all of them
    static void shownmoments(repgame &g, vector<int> &out)
    {
        out.setsize(0);
        loopv(g.moments) if(demoanalysisallmoments || g.moments[i].alert) out.add(i);
    }
    #define MOMENTS_PAGE 14

    // the analysis asked (running, or waiting for the end of the history of a recorded game)
    static job *askedjob()
    {
        if(cur && !cur->automatic) return cur;
        if(queued && !queued->automatic) return queued;
        return NULL;
    }
    ICOMMAND(demoanalysebusy, "", (), intret(askedjob() ? 1 : 0));
    ICOMMAND(demoanalyseplaying, "", (), intret(game::demoplayback && playpath[0] ? 1 : 0));
    ICOMMAND(demoanalysestatus, "", (),
    {
        job *a = askedjob();
        if(a) { defformatstring(s, "Analyzing %s... %d %%", a->label, a == cur ? a->pct : 0); result(s); return; }
        if(!last) { result(""); return; }
        if(!last->readok) { result("This demo could not be read."); return; }
        if(!selgame()) { defformatstring(s, "Nothing to show in this demo (no game of %d s with two players).", demoanalysisminsecs); result(s); return; }
        result(rep.label);
    });
    // the game shown: map, mode, length, kind of demo
    ICOMMAND(demoanalysegameinfo, "", (),
    {
        repgame *g = askedjob() ? NULL : selgame();
        if(!g) { result(""); return; }
        string dur;
        string s;
        fmttime(g->duration, dur, sizeof(dur), false);
        formatstring(s, "%s, %s, %s - %s demo, HD aim %s", g->map, g->mode, dur, g->local ? "local" : "server", g->hd ? "used" : "not available");
        result(s);
    });
    ICOMMAND(demoanalysesightnote, "", (),
    {
        repgame *g = selgame();
        if(!g || (g->g3done && g->g4done)) { result(""); return; }
        result(g->los ? "Line of sight stopped (the map changed): the checks that need the map are missing." :
                        "The checks that need the demo's map were not run: open this demo, then Analyze.");
    });
    ICOMMAND(demoanalysegames, "", (), { report &r = currentreport(); intret(r.valid ? r.games.length() : 0); });
    ICOMMAND(demoanalysenextgame, "", (), { report &r = currentreport(); if(r.valid && r.games.length()) { repgamesel = (repgamesel + 1)%r.games.length(); repmomentpage = reprowpage = 0; } });
    ICOMMAND(demoanalysegamesel, "", (), intret(repgamesel + 1));
    // the table by pages of ROWS_PAGE rows: rows of the page shown, row r of that page
    static int rowpages(const repgame *g) { return g ? max((g->rows.length() + ROWS_PAGE - 1)/ROWS_PAGE, 1) : 1; }
    static int rowfirst(const repgame *g)
    {
        reprowpage = clamp(reprowpage, 0, rowpages(g) - 1);
        return reprowpage*ROWS_PAGE;
    }
    ICOMMAND(demoanalyserows, "", (), { repgame *g = selgame(); intret(g ? clamp(g->rows.length() - rowfirst(g), 0, ROWS_PAGE) : 0); });
    ICOMMAND(demoanalyserowpages, "", (), intret(rowpages(selgame())));
    ICOMMAND(demoanalyserowpage, "i", (int *d),
    {
        repgame *g = selgame();
        reprowpage = clamp(reprowpage + *d, 0, rowpages(g) - 1);
        defformatstring(s, "%d / %d", reprowpage + 1, rowpages(g));
        result(s);
    });
    // a cell of the table in the colour of what it shows: red alert, yellow look, grey when its check could not judge,
    // white a value measured (the text of the menus is already greyish: white keeps the grey cells apart)
    static const char *statecolour(int s) { return s == CS_ALERT ? "\f3" : (s == CS_LOOK ? "\f2" : (s == CS_FEW || s == CS_NODATA || s == CS_ONEENEMY ? "\f4" : "\f7")); }
    static reprow *pagerow(int prow, int col)
    {
        repgame *g = selgame();
        int row = g ? rowfirst(g) + prow : -1;
        if(!g || prow < 0 || prow >= ROWS_PAGE || !g->rows.inrange(row) || col < 0 || col >= NUMCOLS) return NULL;
        return &g->rows[row];
    }
    ICOMMAND(demoanalysecell, "ii", (int *prow, int *col),
    {
        reprow *r = pagerow(*prow, *col);
        if(!r) { result(""); return; }
        defformatstring(s, "%s%s", statecolour(r->state[*col]), r->cell[*col]);
        result(s);
    });
    // what a cell explains when the mouse rests on it (guitip): its numbers, what normal players do and the levels;
    // the verdict and "What was found": what each kind of cheat found (aim, wallhack, movement), then which checks
    // could judge, by kind of cheat too
    static void celltip(int prow, int col)
    {
        reprow *r = pagerow(prow, col);
        if(!r) { result(""); return; }
        if(col != C_VERDICT && col != C_FOUND) { result(r->tip[col]); return; }
        static char t[4096];
        copystring(t, r->verdict == V_ALERT ? "Alert: a check is over its alert level." : (r->verdict == V_LOOK ? "Worth a look: a check is over its look level (never an alert)." : "OK: nothing found by the checks that had enough data."), sizeof(t));
        for(int f = 0; f < NUMFAMILIES; f++)
        {
            bool listed = false;
            loopk(NUMJUDGE)
            {
                int c = judgecols[k];
                if(familyof(c) != f || !r->found[c][0]) continue;
                if(listed) concatstring(t, "; ", sizeof(t));
                else { concatstring(t, "\n", sizeof(t)); concatstring(t, familynames[f], sizeof(t)); concatstring(t, ": ", sizeof(t)); }
                concatstring(t, r->found[c], sizeof(t));
                listed = true;
            }
            if(listed) concatstring(t, ".", sizeof(t));
        }
        static const char * const heads[3] = { "\nEnough data: ", "\nToo little data: ", "\nNo data: " };
        loopi(3)
        {
            bool started = false;
            for(int f = 0; f < NUMFAMILIES; f++)
            {
                bool listed = false;
                loopk(NUMJUDGE)
                {
                    int c = judgecols[k], s = r->state[c], grp = s == CS_FEW ? 1 : (s == CS_NODATA || s == CS_ONEENEMY ? 2 : 0);
                    if(grp != i || familyof(c) != f) continue;
                    if(listed) concatstring(t, ", ", sizeof(t));
                    else
                    {
                        concatstring(t, started ? "; " : heads[i], sizeof(t));
                        concatstring(t, familynames[f], sizeof(t));
                        concatstring(t, " (", sizeof(t));
                        started = true;
                    }
                    concatstring(t, colnames[c], sizeof(t));
                    listed = true;
                }
                if(listed) concatstring(t, ")", sizeof(t));
            }
            if(started) concatstring(t, ".", sizeof(t));
        }
        concatstring(t, "\nIndications, not proof: look at the moments before deciding anything.", sizeof(t));
        result(t);
    }
    ICOMMAND(demoanalysetip, "ii", (int *prow, int *col), celltip(*prow, *col));
    // the notes about the names, by pages of NOTES_PAGE
    static int notepages() { return max((repnotes.length() + NOTES_PAGE - 1)/NOTES_PAGE, 1); }
    ICOMMAND(demoanalysenotes, "", (),
    {
        if(!last) { intret(0); return; }
        repnotepage = clamp(repnotepage, 0, notepages() - 1);
        intret(clamp(repnotes.length() - repnotepage*NOTES_PAGE, 0, NOTES_PAGE));
    });
    ICOMMAND(demoanalysenote, "i", (int *i),
    {
        int k = clamp(repnotepage, 0, notepages() - 1)*NOTES_PAGE + *i;
        result(last && *i >= 0 && *i < NOTES_PAGE && repnotes.inrange(k) ? repnotes[k].text : "");
    });
    ICOMMAND(demoanalysenotetip, "i", (int *i),
    {
        int k = clamp(repnotepage, 0, notepages() - 1)*NOTES_PAGE + *i;
        result(last && *i >= 0 && *i < NOTES_PAGE && repnotes.inrange(k) ? repnotes[k].tip : "");
    });
    ICOMMAND(demoanalysenotepages, "", (), intret(notepages()));
    ICOMMAND(demoanalysenotepage, "i", (int *d),
    {
        repnotepage = clamp(repnotepage + *d, 0, notepages() - 1);
        defformatstring(s, "%d / %d", repnotepage + 1, notepages());
        result(s);
    });
    ICOMMAND(demoanalysemoments, "", (), { repgame *g = selgame(); if(!g) { intret(0); return; } vector<int> m; shownmoments(*g, m); intret(m.length()); });
    ICOMMAND(demoanalysemomentpage, "i", (int *d),
    {
        repgame *g = selgame();
        if(!g) return;
        vector<int> m;
        shownmoments(*g, m);
        int pages = max((m.length() + MOMENTS_PAGE - 1)/MOMENTS_PAGE, 1);
        repmomentpage = clamp(repmomentpage + *d, 0, pages - 1);
        defformatstring(s, "%d / %d", repmomentpage + 1, pages);
        result(s);
    });
    ICOMMAND(demoanalysemoment, "i", (int *i),
    {
        repgame *g = selgame();
        if(!g) { result(""); return; }
        vector<int> m;
        shownmoments(*g, m);
        int k = repmomentpage*MOMENTS_PAGE + *i;
        if(*i < 0 || *i >= MOMENTS_PAGE || !m.inrange(k)) { result(""); return; }
        const repmoment &mo = g->moments[m[k]];
        if(!mo.alert) { result(mo.text); return; }
        // red: a moment of a check in red; yellow: of jumps or noclip ("look")
        defformatstring(s, "%s%s", mo.type == M_JUMPF || mo.type == M_NOCLIP ? "\f2" : "\f3", mo.text);
        result(s);
    });
    // go to the moment (2 s before) and follow the player, in the demo being played
    ICOMMAND(demoanalysegoto, "i", (int *i),
    {
        repgame *g = selgame();
        if(!g) return;
        vector<int> m;
        shownmoments(*g, m);
        int k = repmomentpage*MOMENTS_PAGE + *i;
        if(!m.inrange(k)) return;
        const repmoment &mo = g->moments[m[k]];
        if(!game::demoplayback || !playpath[0] || strcmp(playpath, rep.path)) { conoutf("\f2Open this demo to go to its moments"); return; }
        if(g->part > 0) { conoutf("\f2Moments of a second game in a file cannot be reached by time"); return; }
        string t;
        string cmd;
        fmttime(dmax(mo.t - 2000, 0), t, sizeof(t));
        formatstring(cmd, "demogoto %s; follow %d", t, mo.cn);
        execute(cmd);
    });

    // ------------------------------------------------------------ the page Player stats
    // One row per name: the sums of its records that pass the filters (a mode or all, a map or all), sorted by
    // the title clicked, by pages of ST_ROWS (the menu window holds about 26 lines in a 1600x900 window).
    struct strow { string name; int maps; double v[SV_NVALS]; int bins[ST_BINS]; };
    static vector<strow> strows;
    static int stmodesel = -1, stweaponsel = -1, stpagesel = 0, stbuiltview = -1;
    static string stmapsel = "";
    #define ST_VIEWS 8
    #define ST_COLS 9
    #define ST_ROWS 16
    static int stsortcol[ST_VIEWS] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    static bool stsortasc[ST_VIEWS] = { false, false, false, false, false, false, false, false };
    static const char * const sthead[ST_VIEWS][ST_COLS] =
    {
        { "Name", "Games", "Time", "Last seen", "Maps", "Ping", "Reaction", NULL, NULL },
        { "Name", "Games", "Frags", "Deaths", "Ratio", "Suicides", "Teamkills", "Streak", NULL },
        { "Name", "Games", "Shots", "Hits", "Hit %", "Damage", "Taken", "Frags", "Deaths" },
        { "Name", "Games", "With you", "Killed you", "You killed", NULL, NULL, NULL, NULL },
        { "Name", "Games", "Frags", "Behind", "Deaths", "Blind", NULL, NULL, NULL },
        { "Name", "Games", "Taken", "Scored", "Returned", "On carrier", NULL, NULL, NULL },
        { "Name", "Games", "Own half", "Other", "Near flag", "Still", "Times", "Frags", NULL },
        { "Name", "Games", "Alerts", "Hitbox", "Clock", "Move", "Aim", "Hidden", "Walls" }
    };
    static const int stwidth[ST_VIEWS][ST_COLS] =
    {
        { 15, 6, 7, 11, 5, 5, 9, 0, 0 },
        { 15, 6, 6, 7, 6, 9, 10, 7, 0 },
        { 15, 6, 6, 5, 6, 7, 6, 6, 7 },
        { 15, 6, 9, 13, 13, 0, 0, 0, 0 },
        { 15, 6, 6, 11, 7, 11, 0, 0, 0 },
        { 15, 6, 6, 7, 9, 10, 0, 0, 0 },
        { 15, 6, 9, 6, 10, 6, 6, 6, 0 },
        { 15, 6, 7, 7, 6, 5, 4, 7, 6 }
    };
    static const char * const stweapons[ST_WEAPONS] = { "Chainsaw", "Shotgun", "Chaingun", "Rocket launcher", "Rifle", "Grenade launcher", "Pistol" };

    // days since 1970-01-01 -> YYYY-MM-DD (civil calendar)
    static void stdaytext(int day, char *buf, int len)
    {
        int z = day + 719468, era = (z >= 0 ? z : z - 146096)/146097, doe = z - era*146097;
        int yoe = (doe - doe/1460 + doe/36524 - doe/146096)/365, y = yoe + era*400, doy = doe - (365*yoe + yoe/4 - yoe/100);
        int mp = (5*doy + 2)/153, d = doy - (153*mp + 2)/5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
        if(m <= 2) y++;
        nformatstring(buf, len, "%04d-%02d-%02d", y, m, d);
    }

    // the reactions counted (the bins hold 10 ms each)
    static int streactions(const int *bins)
    {
        int n = 0;
        loopi(ST_BINS) n += bins[i];
        return n;
    }

    // the reaction in the middle: none under demoanalysiswaitcount reactions (one or two shots that left as the enemy
    // came out gave 5 or 25 ms), and for an even number the mean of the two in the middle
    static int stmedian(const int *bins)
    {
        int n = streactions(bins);
        if(n <= 0 || n < demoanalysiswaitcount) return -1;
        int lo = -1, c = 0;
        loopi(ST_BINS)
        {
            c += bins[i];
            if(lo < 0 && c >= (n + 1)/2) lo = i;
            if(c >= n/2 + 1) return (lo + i)*5 + 5;
        }
        return -1;
    }

    // the cell of a row: its text, and the number it is sorted by (missing: "-")
    static double stcell(const strow &r, int view, int col, char *buf, int len, bool &missing)
    {
        const double *v = r.v;
        missing = false;
        buf[0] = 0;
        auto num = [&](double n) -> double { nformatstring(buf, len, "%d", int(n + 0.5)); return n; };
        auto none = [&]() -> double { copystring(buf, "-", len); missing = true; return 0; };
        auto pct = [&](double a, double b) -> double { if(b <= 0) return none(); double p = 100.0*a/b; nformatstring(buf, len, "%d %%", int(p + 0.5)); return p; };
        auto withpct = [&](double a, double b) -> double { if(b <= 0) return none(); nformatstring(buf, len, "%d (%d %%)", int(a + 0.5), int(100.0*a/b + 0.5)); return a; };
        auto mins = [&](double ms) -> double { int m = int(ms/60000.0 + 0.5); nformatstring(buf, len, "%d:%02d", m/60, m%60); return ms; };
        if(col == 0) { copystring(buf, r.name, len); return 0; }
        if(col == 1) return num(v[SV_GAMES]);
        switch(view)
        {
            case 0:
                switch(col)
                {
                    case 2: return mins(v[SV_TIME]);
                    case 3: if(v[SV_LASTDAY] <= 0) return none(); stdaytext(int(v[SV_LASTDAY]), buf, len); return v[SV_LASTDAY];
                    case 4: return num(r.maps);
                    case 5: if(v[SV_PINGN] <= 0) return none(); return num(v[SV_PINGSUM]/v[SV_PINGN]);
                    case 6: { int m = stmedian(r.bins); if(m < 0) return none(); nformatstring(buf, len, "%d ms", m); return m; }
                }
                break;
            case 1:
                switch(col)
                {
                    case 2: return num(v[SV_FRAGS]);
                    case 3: return num(v[SV_DEATHS]);
                    case 4: { double x = v[SV_FRAGS]/dmax(v[SV_DEATHS], 1.0); nformatstring(buf, len, "%.2f", x); return x; }
                    case 5: return num(v[SV_SUICIDES]);
                    case 6: return num(v[SV_TEAMKILLS]);
                    case 7: return num(v[SV_STREAK]);
                }
                break;
            case 2:
            {
                int w = stweaponsel;
                double shots = 0, hits = 0, dealt, taken, frags, deaths;
                if(w < 0)
                {
                    loopi(ST_WEAPONS) { shots += v[SV_W(i, SW_SHOTS)]; hits += v[SV_W(i, SW_HITS)]; }
                    dealt = v[SV_DEALT]; taken = v[SV_TAKEN]; frags = v[SV_FRAGS]; deaths = v[SV_DEATHS];
                }
                else
                {
                    shots = v[SV_W(w, SW_SHOTS)]; hits = v[SV_W(w, SW_HITS)]; dealt = v[SV_W(w, SW_DEALT)]; taken = v[SV_W(w, SW_TAKEN)];
                    frags = v[SV_W(w, SW_FRAGS)]; deaths = v[SV_W(w, SW_DEATHS)];
                }
                switch(col)
                {
                    case 2: return num(shots);
                    case 3: return num(hits);
                    case 4: return pct(hits, shots);
                    case 5: return num(dealt);
                    case 6: return num(taken);
                    case 7: return num(frags);
                    case 8: return num(deaths);
                }
                break;
            }
            case 3:
                if(v[SV_RECGAMES] <= 0) return none();
                switch(col)
                {
                    case 2: return num(v[SV_RECGAMES]);
                    case 3: nformatstring(buf, len, "%d, %d behind", int(v[SV_KILLEDREC] + 0.5), int(v[SV_BACKONREC] + 0.5)); return v[SV_KILLEDREC];
                    case 4: nformatstring(buf, len, "%d, %d behind", int(v[SV_KILLEDBYREC] + 0.5), int(v[SV_BACKBYREC] + 0.5)); return v[SV_KILLEDBYREC];
                }
                break;
            case 4:
                switch(col)
                {
                    case 2: return num(v[SV_BACKFRAGSN]);
                    case 3: return withpct(v[SV_BACKFRAGS], v[SV_BACKFRAGSN]);
                    case 4: return num(v[SV_BACKDEATHSN]);
                    case 5: return withpct(v[SV_BACKDEATHS], v[SV_BACKDEATHSN]);
                }
                break;
            case 5:
                switch(col)
                {
                    case 2: return num(v[SV_FLAGTAKEN]);
                    case 3: return num(v[SV_FLAGSCORED]);
                    case 4: return num(v[SV_FLAGRETURNED]);
                    case 5: return num(v[SV_CARRIERFRAGS]);
                }
                break;
            case 6:
            {
                double camp = v[SV_OWNHALF] + v[SV_ENEMYHALF];
                switch(col)
                {
                    case 2: return pct(v[SV_OWNHALF], camp);
                    case 3: return pct(v[SV_ENEMYHALF], camp);
                    case 4: return pct(v[SV_NEARBASE], camp);
                    case 5: { double mean = v[SV_STILL]/dmax(v[SV_GAMES], 1.0); int s = int(mean/1000.0 + 0.5); nformatstring(buf, len, "%d:%02d", s/60, s%60); return mean; }
                    case 6: return num(v[SV_STILLN]);
                    case 7: return num(v[SV_STILLFRAGS]);
                }
                break;
            }
            case 7:
                switch(col)
                {
                    case 2: return num(v[SV_ALERTGAMES]);
                    case 3: return num(v[SV_ALHITBOX]);
                    case 4: return num(v[SV_ALCLOCK]);
                    case 5: return num(v[SV_ALMOVE]);
                    case 6: return num(v[SV_ALAIM]);
                    case 7: if(v[SV_G3GAMES] <= 0) return none(); return num(v[SV_ALHIDDEN]);
                    case 8: if(v[SV_G4GAMES] <= 0) return none(); return num(v[SV_ALWALLS]);
                }
                break;
        }
        return none();
    }

    // My list: the names ticked on the page (kept from one launch to the next: playerstatsmylist); shown alone
    // instead of every name when asked. The name searched (Find a name): only the names that hold it, without case nor colours.
    static vector<char *> stpicked;
    static bool stlistonly = false;
    static string stfind = "";

    // a name in lower case, without its colour codes (to search it)
    static void stplain(const char *s, char *d, int len)
    {
        int n = 0;
        for(; *s && n < len-1; s++)
        {
            if(*s == '\f') { if(s[1]) s++; continue; }
            d[n++] = tolower((uchar)*s);
        }
        d[n] = 0;
    }

    static int stpickedat(const char *name)
    {
        loopv(stpicked) if(!strcmp(stpicked[i], name)) return i;
        return -1;
    }

    static bool stshown(const char *name)
    {
        if(stlistonly) return stpickedat(name) >= 0;
        if(!stfind[0]) return true;
        string p;
        stplain(name, p, sizeof(p));
        return strstr(p, stfind) != NULL;
    }

    static bool stmatch(const strecord &r) { return (stmodesel < 0 || r.mode == stmodesel) && (!stmapsel[0] || !strcmp(r.map, stmapsel)); }
    static bool strowless(const strow &a, const strow &b) { int c = strcasecmp(a.name, b.name); return c < 0 || (!c && strcmp(a.name, b.name) < 0); }

    static void stbuild(int view)
    {
        histload();
        strows.shrink(0);
        loopv(stnames)
        {
            const stname &s = *stnames[i];
            if(!stshown(s.name)) continue;
            int ri = -1;
            vector<const char *> maps;
            loopvj(s.recs)
            {
                const strecord &r = s.recs[j];
                if(r.v[SV_GAMES] <= 0 || !stmatch(r)) continue;
                if(ri < 0)
                {
                    ri = strows.length();
                    strow &w = strows.add();
                    copystring(w.name, s.name);
                    w.maps = 0;
                    memset(w.v, 0, sizeof(w.v));
                    memset(w.bins, 0, sizeof(w.bins));
                }
                strow &w = strows[ri];
                loopk(SV_NVALS) w.v[k] = k == SV_LASTDAY || k == SV_STREAK ? dmax(w.v[k], r.v[k]) : w.v[k] + r.v[k];
                loopk(ST_BINS) w.bins[k] += r.bins[k];
                bool seen = false;
                loopvk(maps) if(!strcmp(maps[k], r.map)) seen = true;
                if(!seen) { maps.add(r.map); w.maps++; }
            }
        }
        // by name first (the order of equal values), then by the column chosen; missing values last
        strows.sort(strowless);
        int col = clamp(stsortcol[view], 0, ST_COLS-1);
        if(col > 0 || !stsortasc[view])
        {
            dlist keys;
            char buf[MAXSTRLEN];
            loopv(strows)
            {
                bool missing = false;
                double k = col > 0 ? stcell(strows[i], view, col, buf, sizeof(buf), missing) : double(i);
                keys.add(missing ? DINF : (stsortasc[view] ? k : -k));
            }
            vector<int> o;
            stableargsort(keys.getbuf(), keys.length(), o);
            vector<strow> sorted;
            loopv(o) sorted.add(strows[o[i]]);
            strows.shrink(0);
            loopv(sorted) strows.add(sorted[i]);
        }
        stbuiltview = view;
        stchanged = false;
    }

    static int stview(int v) { return clamp(v, 0, ST_VIEWS-1); }
    static void stensure(int view) { if(stchanged || stbuiltview != view) stbuild(view); }
    static int stpages() { return max((strows.length() + ST_ROWS - 1)/ST_ROWS, 1); }

    // the games of a mode and a map (-1, "": all)
    static int stcountgames(int mode, const char *map)
    {
        int n = 0;
        loopv(stgames) if((mode < 0 || stgames[i].mode == mode) && (!map[0] || !strcmp(stgames[i].map, map))) n += stgames[i].games;
        return n;
    }

    // the modes (maps) with games under the other filter and their games; order: the most played first
    static void stchoices(bool mode, vector<int> &modes, vector<const char *> &maps, vector<int> &counts, vector<int> &order)
    {
        loopv(stgames)
        {
            const stmodemap &g = stgames[i];
            if(mode ? (stmapsel[0] && strcmp(g.map, stmapsel)) : (stmodesel >= 0 && g.mode != stmodesel)) continue;
            int k = -1;
            if(mode) { loopvj(modes) if(modes[j] == g.mode) k = j; }
            else { loopvj(maps) if(!strcmp(maps[j], g.map)) k = j; }
            if(k < 0) { k = counts.length(); counts.add(0); if(mode) modes.add(g.mode); else maps.add(g.map); }
            counts[k] += g.games;
        }
        // most played first (selection by count, then by mode number or name)
        int n = counts.length();
        loopi(n) order.add(i);
        loopi(n) for(int j = i+1; j < n; j++)
        {
            int a = order[i], b = order[j];
            bool before = counts[b] > counts[a] || (counts[b] == counts[a] && (mode ? modes[b] < modes[a] : strcmp(maps[b], maps[a]) < 0));
            if(before) swap(order[i], order[j]);
        }
    }

    // the next mode (map) with games under the other filter, the most played first; "all" comes first
    static void stcycle(bool mode, int d)
    {
        vector<int> modes, counts, order;
        vector<const char *> maps;
        stchoices(mode, modes, maps, counts, order);
        int n = counts.length();
        int cur = 0;
        loopi(n) if(mode ? modes[order[i]] == stmodesel : !strcmp(maps[order[i]], stmapsel)) cur = i + 1;
        cur = ((cur + d)%(n + 1) + n + 1)%(n + 1);
        if(mode) stmodesel = cur ? modes[order[cur-1]] : -1;
        else copystring(stmapsel, cur ? maps[order[cur-1]] : "");
        stchanged = true;
        stpagesel = 0;
    }

    ICOMMAND(playerstatsinfo, "", (),
    {
        histload();
        if(stnames.empty()) { result("No player stats yet: they are added at the end of each recorded game and with each analysis."); return; }
        stensure(max(stbuiltview, 0));
        int g = stcountgames(stmodesel, stmapsel);
        defformatstring(s, "%d game%s, %d name%s (your recorded games and analyzed demos, on this computer)", g, g == 1 ? "" : "s", strows.length(), strows.length() == 1 ? "" : "s");
        result(s);
    });
    ICOMMAND(playerstatsmode, "i", (int *d),
    {
        histload();
        if(*d) stcycle(true, *d);
        defformatstring(s, "Mode: %s", stmodesel < 0 ? "All" : modename(stmodesel));
        result(s);
    });
    ICOMMAND(playerstatsmap, "i", (int *d),
    {
        histload();
        if(*d) stcycle(false, *d);
        defformatstring(s, "Map: %s", stmapsel[0] ? stmapsel : "All");
        result(s);
    });
    ICOMMAND(playerstatsweapon, "i", (int *d),
    {
        if(*d) { stweaponsel = ((stweaponsel + 1 + *d)%(ST_WEAPONS + 1) + ST_WEAPONS + 1)%(ST_WEAPONS + 1) - 1; stchanged = true; }
        defformatstring(s, "Weapon: %s", stweaponsel < 0 ? "All weapons" : stweapons[stweaponsel]);
        result(s);
    });
    static int stcols(int view) { int v = stview(view), n = 0; while(n < ST_COLS && sthead[v][n]) n++; return n; }
    ICOMMAND(playerstatscols, "i", (int *view), intret(stcols(*view)));
    ICOMMAND(playerstatswidth, "ii", (int *view, int *col), intret(*col >= 0 && *col < ST_COLS ? stwidth[stview(*view)][*col] : 0));
    ICOMMAND(playerstatshead, "ii", (int *view, int *col),
    {
        int v = stview(*view);
        if(*col < 0 || *col >= ST_COLS || !sthead[v][*col]) { result(""); return; }
        if(stsortcol[v] == *col) { defformatstring(s, "\f2%s", sthead[v][*col]); result(s); }
        else result(sthead[v][*col]);
    });
    ICOMMAND(playerstatssort, "ii", (int *view, int *col),
    {
        int v = stview(*view);
        if(*col < 0 || *col >= ST_COLS || !sthead[v][*col]) return;
        if(stsortcol[v] == *col) stsortasc[v] = !stsortasc[v];
        else { stsortcol[v] = *col; stsortasc[v] = *col == 0; }
        stchanged = true;
        stpagesel = 0;
    });
    ICOMMAND(playerstatssorted, "i", (int *view),
    {
        int v = stview(*view);
        int c = clamp(stsortcol[v], 0, ST_COLS-1);
        defformatstring(s, "By %s, %s", c ? sthead[v][c] : "name", c ? (stsortasc[v] ? "lowest first" : "highest first") : (stsortasc[v] ? "A to Z" : "Z to A"));
        result(s);
    });
    ICOMMAND(playerstatsrows, "i", (int *view),
    {
        int v = stview(*view);
        stensure(v);
        stpagesel = clamp(stpagesel, 0, stpages() - 1);
        intret(clamp(strows.length() - stpagesel*ST_ROWS, 0, ST_ROWS));
    });
    ICOMMAND(playerstatscell, "iii", (int *view, int *row, int *col),
    {
        int v = stview(*view);
        stensure(v);
        int r = stpagesel*ST_ROWS + *row;
        if(*row < 0 || *row >= ST_ROWS || !strows.inrange(r) || *col < 0 || *col >= ST_COLS || !sthead[v][*col]) { result(""); return; }
        char buf[MAXSTRLEN];
        bool missing;
        double x = stcell(strows[r], v, *col, buf, sizeof(buf), missing);
        // grey: not measured, or a name seen in fewer games than the history needs to tell what is usual; red: games
        // where a check was in red; white otherwise (the text of the menus is already greyish)
        const char *c = "\f7";
        if(*col > 0)
        {
            if(missing) c = "\f4";
            else if(v == 7 && *col >= 2 && x > 0) c = "\f3";
            else if(strows[r].v[SV_GAMES] < demoanalysisrupturegames) c = "\f4";
        }
        defformatstring(s, "%s%s", c, buf);
        result(s);
    });
    // what a cell of the page Player stats explains when the mouse rests on it (guitip): the exact numbers behind it,
    // the games and filters it covers, what normal players do for the checks
    static void stattip(int view, int row, int col)
    {
        int v = stview(view);
        stensure(v);
        int ri = stpagesel*ST_ROWS + row;
        if(row < 0 || row >= ST_ROWS || !strows.inrange(ri) || col < 0 || col >= ST_COLS || !sthead[v][col]) { result(""); return; }
        const strow &w = strows[ri];
        const double *x = w.v;
        int games = int(x[SV_GAMES] + 0.5);
        string filt;
        string extra = "";
        formatstring(filt, "%d game%s; mode %s, map %s", games, games == 1 ? "" : "s", stmodesel < 0 ? "all" : modename(stmodesel), stmapsel[0] ? stmapsel : "all");
        char buf[MAXSTRLEN];
        bool missing;
        stcell(w, v, col, buf, sizeof(buf), missing);
        if(v == 0 && col == 5 && !missing) concatstring(buf, " ms", sizeof(buf));
        static char t[1024];
        if(col == 0) { nformatstring(t, sizeof(t), "%s: %s, %d map%s. A name can be copied: its stats can mix players.", w.name, filt, w.maps, w.maps == 1 ? "" : "s"); result(t); return; }
        if(missing && v == 0 && col == 6 && streactions(w.bins) > 0)
        {
            int n = streactions(w.bins);
            nformatstring(t, sizeof(t), "%s: %d reaction%s in these games (%s), too few to give a value: at least %d (Demo analysis settings, Waiting: reactions, at least).",
                          sthead[v][col], n, n == 1 ? "" : "s", filt, demoanalysiswaitcount);
            result(t);
            return;
        }
        if(missing) { nformatstring(t, sizeof(t), "%s: not measured in these games (%s).", sthead[v][col], filt); result(t); return; }
        switch(v)
        {
            case 0:
                if(col == 6) { int n = streactions(w.bins); formatstring(extra, " The middle of %d reaction%s (the view already on an enemy coming out, then a shot). Normal players: about 300 to 700 ms.", n, n == 1 ? "" : "s"); }
                else if(col == 5) copystring(extra, " The mean of the pings he sent.");
                else if(col == 2) copystring(extra, " Hours:minutes, from his first to his last position in each game.");
                break;
            case 1:
                if(col == 4) formatstring(extra, " %d frags, %d deaths.", int(x[SV_FRAGS] + 0.5), int(x[SV_DEATHS] + 0.5));
                break;
            case 2:
                if(col == 4)
                {
                    double shots = 0;
                    double hits = 0;
                    if(stweaponsel < 0) loopi(ST_WEAPONS) { shots += x[SV_W(i, SW_SHOTS)]; hits += x[SV_W(i, SW_HITS)]; }
                    else { shots = x[SV_W(stweaponsel, SW_SHOTS)]; hits = x[SV_W(stweaponsel, SW_HITS)]; }
                    formatstring(extra, " %d hits of %d shots (%s).", int(hits + 0.5), int(shots + 0.5), stweaponsel < 0 ? "all weapons" : stweapons[stweaponsel]);
                }
                else formatstring(extra, " %s.", stweaponsel < 0 ? "All weapons" : stweapons[stweaponsel]);
                break;
            case 3:
                formatstring(extra, " In %d of your recorded games together; behind: out of the other's field of view.", int(x[SV_RECGAMES] + 0.5));
                break;
            case 4:
                if(col == 3) formatstring(extra, " %d of %d frags where both could be placed.", int(x[SV_BACKFRAGS] + 0.5), int(x[SV_BACKFRAGSN] + 0.5));
                else if(col == 5) formatstring(extra, " %d of %d deaths where both could be placed.", int(x[SV_BACKDEATHS] + 0.5), int(x[SV_BACKDEATHSN] + 0.5));
                break;
            case 6:
                if(col <= 4) { int n = int(x[SV_CAMPGAMES] + 0.5); formatstring(extra, " Of his time alive in %d game%s with a flag or a base at home.", n, n == 1 ? "" : "s"); }
                else if(col == 5)
                {
                    int tot = int(x[SV_STILL]/1000.0 + 0.5);
                    formatstring(extra, " The mean per game, minutes:seconds; in all %d:%02d in %d game%s. Still: at least %.1f s inside %.0f units (Player stats settings).", tot/60, tot%60, games, games == 1 ? "" : "s", demoanalysisstill, demoanalysisstillradius);
                }
                break;
            case 7:
                copystring(extra, " Games where this check was in red (with the levels of the menu then). Normal players: none.");
                if(col == 7) { int n = int(x[SV_G3GAMES] + 0.5); concformatstring(extra, " Measured with the map in %d game%s.", n, n == 1 ? "" : "s"); }
                else if(col == 8) { int n = int(x[SV_G4GAMES] + 0.5); concformatstring(extra, " Full analyses: %d game%s.", n, n == 1 ? "" : "s"); }
                break;
        }
        nformatstring(t, sizeof(t), "%s: %s (%s).%s%s", sthead[v][col], buf, filt, extra, games < demoanalysisrupturegames ? " Fewer games than the history needs to tell what is usual: grey." : "");
        result(t);
    }
    ICOMMAND(playerstatstip, "iii", (int *view, int *row, int *col), stattip(*view, *row, *col));
    ICOMMAND(playerstatspages, "i", (int *view), { stensure(stview(*view)); intret(stpages()); });
    ICOMMAND(playerstatspage, "ii", (int *view, int *d),
    {
        stensure(stview(*view));
        stpagesel = clamp(stpagesel + *d, 0, stpages() - 1);
        defformatstring(s, "%d / %d", stpagesel + 1, stpages());
        result(s);
    });

    // the lists the buttons Mode, Map and Weapon open (kind 0, 1, 2): All first, then the modes and maps with games
    // under the other filter (the most played first, with their games), the weapons in their order
    struct stchoicelist { vector<int> modes, counts, order; vector<const char *> maps; };
    static int stchoicecount(int kind, stchoicelist &l)
    {
        histload();
        if(kind == 2) return ST_WEAPONS + 1;
        stchoices(kind == 0, l.modes, l.maps, l.counts, l.order);
        return l.counts.length() + 1;
    }
    static void stchoicetext(int kind, int i)
    {
        stchoicelist l;
        int n = stchoicecount(kind, l);
        if(i < 0 || i >= n) { result(""); return; }
        bool cur;
        string s;
        if(kind == 2) { cur = i - 1 == stweaponsel; copystring(s, i ? stweapons[i-1] : "All weapons"); }
        else if(!i)
        {
            cur = kind ? !stmapsel[0] : stmodesel < 0;
            formatstring(s, "All (%d)", kind ? stcountgames(stmodesel, "") : stcountgames(-1, stmapsel));
        }
        else
        {
            int k = l.order[i-1];
            cur = kind ? !strcmp(l.maps[k], stmapsel) : l.modes[k] == stmodesel;
            formatstring(s, "%s (%d)", kind ? l.maps[k] : modename(l.modes[k]), l.counts[k]);
        }
        defformatstring(t, "%s%s", cur ? "\f2" : "", s);
        result(t);
    }
    static void stchoose(int kind, int i)
    {
        stchoicelist l;
        int n = stchoicecount(kind, l);
        if(i < 0 || i >= n) return;
        if(kind == 2) stweaponsel = i - 1;
        else if(kind == 1) copystring(stmapsel, i ? l.maps[l.order[i-1]] : "");
        else stmodesel = i ? l.modes[l.order[i-1]] : -1;
        stchanged = true;
        stpagesel = 0;
    }
    ICOMMAND(playerstatschoices, "i", (int *kind), { stchoicelist l; intret(stchoicecount(*kind, l)); });
    ICOMMAND(playerstatschoice, "ii", (int *kind, int *i), stchoicetext(*kind, *i));
    ICOMMAND(playerstatschoose, "ii", (int *kind, int *i), stchoose(*kind, *i));

    // My list: tick a name of the table (a row of the page shown), show the list alone or every name again, empty it
    static const char *strowname(int view, int row)
    {
        stensure(stview(view));
        int r = stpagesel*ST_ROWS + row;
        return row >= 0 && row < ST_ROWS && strows.inrange(r) ? strows[r].name : NULL;
    }
    // My list is kept from one launch to the next in the setting playerstatsmylist (saved in the config like the
    // other settings): each name in hex (any character of a name is safe in the config), one space between them, at
    // most ST_PICKMAX names. Written again each time a name is ticked or the list emptied; read when the config sets it.
    #define ST_PICKMAX 1000
    static void stpicksave()
    {
        vector<char> buf;
        loopv(stpicked)
        {
            char h[2*MAXSTRLEN+1];
            hexname(stpicked[i], h, sizeof(h));
            if(!h[0]) continue;
            if(buf.length()) buf.add(' ');
            buf.put(h, strlen(h));
        }
        buf.add('\0');
        setsvar("playerstatsmylist", buf.getbuf(), false);
    }
    static void stpick(const char *name, bool toggle)
    {
        if(!name) return;
        int k = stpickedat(name);
        if(k >= 0) { if(toggle) delete[] stpicked.remove(k); }
        else if(stpicked.length() < ST_PICKMAX) stpicked.add(newstring(name));
        stpicksave();
        if(stlistonly) stchanged = true;
    }
    static void stshowlist(bool on)
    {
        if(on && stpicked.empty()) on = false;
        if(on != stlistonly) { stlistonly = on; stchanged = true; stpagesel = 0; }
    }
    // the saved list: a word that is not a whole hex name is skipped, a name twice is kept once; a name the history
    // does not know (any more) stays in the list without a row
    static void stpickload(const char *s)
    {
        stpicked.deletearrays();
        for(const char *p = s; p && *p;)
        {
            while(*p == ' ') p++;
            const char *e = p;
            while(*e && *e != ' ') e++;
            int len = int(e - p);
            bool ok = len > 0 && !(len&1) && len <= 2*(MAXSTRLEN-1);
            for(const char *q = p; ok && q < e; q++) ok = isxdigit(uchar(*q)) != 0;
            if(ok && stpicked.length() < ST_PICKMAX)
            {
                char h[2*MAXSTRLEN];
                memcpy(h, p, len);
                h[len] = 0;
                string n;
                unhexname(h, n, sizeof(n));
                if(n[0] && stpickedat(n) < 0) stpicked.add(newstring(n));
            }
            p = e;
        }
        if(stlistonly && stpicked.empty()) { stlistonly = false; stpagesel = 0; }
        stchanged = true;
        stpicksave();
    }
    SVARFP(playerstatsmylist, "", stpickload(playerstatsmylist));
    ICOMMAND(playerstatsrowpick, "ii", (int *view, int *row), stpick(strowname(*view, *row), true));
    ICOMMAND(playerstatsrowpicked, "ii", (int *view, int *row), { const char *n = strowname(*view, *row); intret(n && stpickedat(n) >= 0 ? 1 : 0); });
    ICOMMAND(playerstatspicks, "", (), intret(stpicked.length()));
    ICOMMAND(playerstatsshowlist, "i", (int *on), stshowlist(*on != 0));
    ICOMMAND(playerstatslistshown, "", (), intret(stlistonly ? 1 : 0));
    ICOMMAND(playerstatsclearlist, "", (), { stpicked.deletearrays(); stpicksave(); stshowlist(false); });

    // Find a name: the text typed so far filters the table (not My list); the names that hold it, under the filters
    // Mode and Map, are suggested: those that start with it first, then the most games, then by name
    #define ST_FOUND 4
    static vector<int> stfound;
    static void stsearch(const char *text)
    {
        string p;
        stplain(text, p, sizeof(p));
        if(strcmp(p, stfind)) { copystring(stfind, p); if(!stlistonly) { stchanged = true; stpagesel = 0; } }
        stfound.shrink(0);
        if(!stfind[0]) return;
        histload();
        vector<int> games;
        vector<bool> starts;
        loopv(stnames)
        {
            const stname &s = *stnames[i];
            double g = 0;
            loopvj(s.recs) if(stmatch(s.recs[j])) g += s.recs[j].v[SV_GAMES];
            if(g <= 0) continue;
            string n;
            stplain(s.name, n, sizeof(n));
            const char *at = strstr(n, stfind);
            if(!at) continue;
            stfound.add(i);
            games.add(int(g + 0.5));
            starts.add(at == n);
        }
        loopi(min(stfound.length(), ST_FOUND)) for(int j = i+1; j < stfound.length(); j++)
        {
            int c = int(starts[j]) - int(starts[i]);
            if(!c) c = games[j] - games[i];
            if(!c) c = strcasecmp(stnames[stfound[i]]->name, stnames[stfound[j]]->name);
            if(c > 0) { swap(stfound[i], stfound[j]); swap(games[i], games[j]); bool t = starts[i]; starts[i] = starts[j]; starts[j] = t; }
        }
        if(stfound.length() > ST_FOUND) stfound.shrink(ST_FOUND);
    }
    // green: already in My list
    static void stfoundtext(int i)
    {
        if(!stfound.inrange(i) || !stnames.inrange(stfound[i])) { result(""); return; }
        const char *n = stnames[stfound[i]]->name;
        defformatstring(s, "%s%s", stpickedat(n) >= 0 ? "\f0" : "", n);
        result(s);
    }
    ICOMMAND(playerstatsfind, "s", (char *text), { stsearch(text); intret(stfound.length()); });
    ICOMMAND(playerstatsfound, "i", (int *i), stfoundtext(*i));
    ICOMMAND(playerstatsfoundpick, "i", (int *i), { if(stfound.inrange(*i) && stnames.inrange(stfound[*i])) stpick(stnames[stfound[*i]]->name, false); });
}
#endif
