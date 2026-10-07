// Local vanilla .dmo + HD sidecar (.dmohd).
//
// The .dmo is a normal Sauerbraten demo (gzip, DEMO_MAGIC) so any client can
// play it. The sidecar stores float yaw/pitch/eye at high rate. Do not feed
// HD through sendposition() — that re-quantizes to 1°.
//
// Future custom server / other clients of this fork:
//   Keep the vanilla .dmo. Send the .dmohd bytes with the same basename
//   (channel 2 after N_SENDDEMO, or a file drop). Call
//   demohd::installsidecar(dmoname, data, len). Playback already looks for
//   <name>.dmohd next to the .dmo. Samples are tagged per cn so a later mux
//   of several players is the same format with ntracks > 1. Do not bump
//   PROTOCOL_VERSION for this.

#include "game.h"

#include <sys/stat.h>
#ifdef WIN32
#include <objbase.h>
#include <shlobj.h>
#endif

#define DEMOHD_MINMS 8000
#define DEMOHD_MAXBYTES (96<<20)
#define HD_LIVE_KEEP 2000
#define HD_SEND_MAX 12

extern int mainmenu;

// recording the local demo also asks clan servers for their copy (AUTODEMO_CMD)
VARFP(demohdrecord, 0, 1, 1, game::announceautodemo());
VARP(demohdplay, 0, 1, 1);
static void hdprune();
VARFP(demokeep, 1, 5, 30, { hdprune(); });

namespace demohd
{
    struct hdsample
    {
        int t, cn;
        uchar flags, gun, state, pad;
        float yaw, pitch, roll;
        vec o;
    };

    struct fileitem
    {
        string path, hdpath, label, stem;
        time_t mtime;
        long size;
    };

    static stream *dmo = NULL, *hd = NULL;
    static string dmofile, hdfile, dmostem, curmap, lastmap;
    static bool active = false, committed = false, sessionmap = false, welcomed = false;
    static bool needplayersnap = false;
    static int skippackets = 0, recstart = 0, lastsamplet = -1, rec_cn = -1, recmode = 0;
    // the server's copy of this match was asked for: the next map closes the
    // file even when the same map is played again
    static bool splitnext = false;
    static uint matchid = 0;
    static int waitstart = 0;

    static vector<hdsample> track, live, pending;
    static int play_cn = -1, play_idx = 0, live_idx = 0, nplaycns = 0;
    static uchar playhas[256];
    // exact aim: samples the player sent himself (HD_OWN): the recorder's own
    // track, or every modded client in a server demo. Other tracks of a local
    // recording are only the network aim seen by the recorder.
    static uchar playexact[256];
    // first and last sample time of each track: a cut (truncated) file stops
    // early, after it the normal network aim is used again
    static int playfirst[256], playlast[256];
    #define HD_TRACK_SLACK 500
    static int nexactcns = 0;
    static bool play_loaded = false, autofollowdone = false, hdzoomframe = false;
    static float hdzoomval = 0;
    static int scanmillis = 0, dmolenms = 0;
    static bool dbdrag = false, dbhidden = false;
    static float dbdragk = 0;
    static float dbbox[8][4];
    static vector<fileitem> files;

    static int clockms()
    {
        if(!game::maptime) return 0;
        return max(lastmillis - game::maptime, 0);
    }

    static int peektype(const uchar *data, int len)
    {
        if(!data || len <= 0) return -1;
        ucharbuf p(const_cast<uchar *>(data), len);
        return getint(p);
    }

    static bool skipincoming(const uchar *data, int len)
    {
        int t = peektype(data, len);
        return t==N_SERVINFO || t==N_PONG || t==N_SERVCMD || t==N_CLIENTPING || t==N_DEMOPACKET
            || t==N_SENDDEMO || t==N_SENDDEMOLIST || t==N_SENDMAP
            || t==N_HDPOS || t==N_SENDDEMOHD;
    }

    static void sanitize(char *dst, const char *src, int maxlen)
    {
        int n = 0;
        for(const char *s = src; *s && n < maxlen-1; s++)
        {
            uchar c = (uchar)*s;
            if(isalnum(c) || c=='-' || c=='_') dst[n++] = char(c);
            else if(c==' ' || c=='.' || c=='/' || c=='\\') dst[n++] = '-';
        }
        dst[n] = 0;
        if(!dst[0]) copystring(dst, "map", maxlen);
    }

    static void sidecarname(char *dst, const char *dmo, int len)
    {
        copystring(dst, dmo, len);
        int n = strlen(dst);
        if(n >= 4 && !strcasecmp(dst+n-4, ".dmo")) copystring(dst+n-4, ".dmohd", len-(n-4));
        else concatstring(dst, ".dmohd", len);
    }

    static void writedemopacket(int chan, const uchar *data, int len)
    {
        if(!dmo || !data || len <= 0) return;
        int stamp[3] = { clockms(), chan, len };
        lilswap(stamp, 3);
        dmo->write(stamp, sizeof(stamp));
        dmo->write(data, len);
        if(dmo->rawtell() >= DEMOHD_MAXBYTES)
        {
            conoutf(CON_WARN, "local demo hit size limit");
            committed = true;
            // finish() is called from onstartgame/ondisconnect
        }
    }

    static void flushsnap(packetbuf &p)
    {
        if(p.length() > 0) writedemopacket(1, p.buf, p.length());
        p.reset();
    }

    static void writeplayers()
    {
        packetbuf p(MAXTRANS);
        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(!d || !d->name[0] || d->clientnum < 0) continue;
            if(p.length() > MAXTRANS-128) flushsnap(p);
            if(d->aitype != AI_NONE)
            {
                putint(p, N_INITAI);
                putint(p, d->clientnum);
                putint(p, d->ownernum);
                putint(p, d->aitype);
                putint(p, d->skill);
                putint(p, d->playermodel);
                sendstring(d->name, p);
                sendstring(d->team, p);
            }
            else
            {
                putint(p, N_INITCLIENT);
                putint(p, d->clientnum);
                sendstring(d->name, p);
                sendstring(d->team, p);
                putint(p, d->playermodel);
            }
        }
        flushsnap(p);

        putint(p, N_RESUME);
        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(!d || d->clientnum < 0) continue;
            if(p.length() > MAXTRANS-80) { putint(p, -1); flushsnap(p); putint(p, N_RESUME); }
            putint(p, d->clientnum);
            putint(p, d->state);
            putint(p, d->frags);
            putint(p, d->flags);
            putint(p, d->deaths);
            putint(p, d->quadmillis);
            putint(p, d->lifesequence);
            putint(p, d->health);
            putint(p, d->maxhealth);
            putint(p, d->armour);
            putint(p, d->armourtype);
            putint(p, d->gunselect);
            loopi(GUN_PISTOL-GUN_SG+1) putint(p, d->ammo[GUN_SG+i]);
        }
        putint(p, -1);
        flushsnap(p);

        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(!d || d->clientnum < 0 || d->state != CS_SPECTATOR) continue;
            if(p.length() > MAXTRANS-32) flushsnap(p);
            putint(p, N_SPECTATOR);
            putint(p, d->clientnum);
            putint(p, 1);
        }
        flushsnap(p);
    }

    static void writesnapshot()
    {
        packetbuf p(MAXTRANS);
        putint(p, N_WELCOME);
        putint(p, N_MAPCHANGE);
        sendstring(game::getclientmap(), p);
        putint(p, game::gamemode);
        putint(p, 1);
        int remain = 7200;
        if(game::maplimit >= 0) remain = max((game::maplimit - lastmillis + 999)/1000, 1);
        putint(p, N_TIMEUP);
        putint(p, remain);
        putint(p, N_ITEMLIST);
        loopv(entities::getents())
        {
            extentity *e = entities::getents()[i];
            if(!e || !e->spawned() || e->type < I_SHELLS || e->type > I_QUAD) continue;
            if(p.length() > MAXTRANS-64) flushsnap(p);
            putint(p, i);
            putint(p, e->type);
        }
        putint(p, -1);
        flushsnap(p);
        writeplayers();
    }

    static void writehdheader()
    {
        if(!hd) return;
        char magic[8];
        memset(magic, 0, 8);
        copystring(magic, DEMOHD_MAGIC, 8);
        hd->write(magic, 8);
        hd->putlil<ushort>(DEMOHD_VERSION);
        hd->putlil<ushort>(1);
        hd->putlil<int>(PROTOCOL_VERSION);
        hd->putlil<int>(game::gamemode);
        hd->putlil<int>(rec_cn);
        hd->putlil<uint>(matchid);
        char map[64], dmo[64], reserved[64];
        memset(map, 0, sizeof(map));
        memset(dmo, 0, sizeof(dmo));
        memset(reserved, 0, sizeof(reserved));
        copystring(map, game::getclientmap(), 64);
        copystring(dmo, dmostem, 64);
        hd->write(map, 64);
        hd->write(dmo, 64);
        hd->write(reserved, 64);
    }

    static void writehdsample(const hdsample &s)
    {
        if(!hd) return;
        if(hd->rawtell() >= DEMOHD_MAXBYTES)
        {
            DELETEP(hd);
            conoutf(CON_WARN, "HD demo hit size limit");
            return;
        }
        hd->putlil<int>(s.t);
        hd->putchar(s.cn);
        hd->putchar(s.flags);
        hd->putchar(s.gun);
        hd->putchar(s.state);
        hd->putlil<float>(s.yaw);
        hd->putlil<float>(s.pitch);
        hd->putlil<float>(s.roll);
        hd->putlil<float>(s.o.x);
        hd->putlil<float>(s.o.y);
        hd->putlil<float>(s.o.z);
    }

    static void putsamplemsg(packetbuf &p, const hdsample &s)
    {
        putint(p, s.cn);
        putint(p, s.t);
        putint(p, s.flags);
        putint(p, s.gun);
        putint(p, s.state);
        putfloat(p, s.yaw);
        putfloat(p, s.pitch);
        putfloat(p, s.roll);
        putfloat(p, s.o.x);
        putfloat(p, s.o.y);
        putfloat(p, s.o.z);
    }

    static bool getsamplemsg(ucharbuf &p, hdsample &s)
    {
        s.cn = getint(p);
        s.t = getint(p);
        s.flags = uchar(getint(p));
        s.gun = uchar(getint(p));
        s.state = uchar(getint(p));
        s.yaw = getfloat(p);
        s.pitch = getfloat(p);
        s.roll = getfloat(p);
        s.o.x = getfloat(p);
        s.o.y = getfloat(p);
        s.o.z = getfloat(p);
        s.pad = uchar(clamp(int(((s.flags >> HD_ZOOM_SHIFT) & 31) * 255 / 31.0f + 0.5f), 0, 255));
        return !p.overread();
    }

    static void prunelive()
    {
        int cutoff = lastmillis - HD_LIVE_KEEP;
        while(live.length() && live[0].t < cutoff) live.remove(0);
        if(live_idx >= live.length()) live_idx = max(live.length()-1, 0);
    }

    static bool haslive(int cn)
    {
        loopvrev(live) if(live[i].cn==cn) return true;
        return false;
    }

    static bool readsample(stream *f, hdsample &s)
    {
        if(!f || f->end()) return false;
        stream::offset sz = f->size(), pos = f->tell();
        if(sz >= 0 && pos >= 0 && pos + 32 > sz) return false;
        s.t = f->getlil<int>();
        int cn = f->getchar(), flags = f->getchar(), gun = f->getchar(), state = f->getchar();
        if(cn < 0 || flags < 0 || gun < 0 || state < 0) return false;
        s.cn = cn;
        s.flags = uchar(flags);
        s.gun = uchar(gun);
        s.state = uchar(state);
        s.pad = uchar(clamp(int(((s.flags >> HD_ZOOM_SHIFT) & 31) * 255 / 31.0f + 0.5f), 0, 255));
        s.yaw = f->getlil<float>();
        s.pitch = f->getlil<float>();
        s.roll = f->getlil<float>();
        s.o.x = f->getlil<float>();
        s.o.y = f->getlil<float>();
        s.o.z = f->getlil<float>();
        return true;
    }

    static void hdremove(const char *rel)
    {
        if(!rel || !rel[0]) return;
        const char *found = findfile(rel, "r");
        if(found && found[0]) remove(found);
        if(found != rel) remove(rel);
    }

    static void resolvepath(char *dst, const char *rel, const char *mode)
    {
        const char *found = findfile(rel, mode);
        copystring(dst, found ? found : rel, MAXSTRLEN);
        path(dst);
    }

    static void buildnames()
    {
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        string stamp, mapname;
        if(lt) strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", lt);
        else copystring(stamp, "now");
        sanitize(mapname, game::getclientmap(), 64);
        formatstring(dmostem, "%s_%s", stamp, mapname);
        if(homedir[0])
        {
            defformatstring(absdir, "%sdemo", homedir);
            createdir(absdir);
        }
        else createdir("demo");
        formatstring(dmofile, "demo/%s.dmo", dmostem);
        formatstring(hdfile, "demo/%s.dmohd", dmostem);
        path(dmofile);
        path(hdfile);
    }

    static void closestreams()
    {
        DELETEP(dmo);
        DELETEP(hd);
    }

    static void startfiles(bool snapshot)
    {
        if(active) return;
        buildnames();
        dmo = opengzfile(dmofile, "wb");
        hd = openrawfile(hdfile, "wb");
        if(!dmo || !hd)
        {
            conoutf(CON_ERROR, "could not write local demo");
            closestreams();
            hdremove(dmofile);
            hdremove(hdfile);
            return;
        }
        demoheader hdr;
        memcpy(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic));
        hdr.version = DEMO_VERSION;
        hdr.protocol = PROTOCOL_VERSION;
        lilswap(&hdr.version, 2);
        dmo->write(&hdr, sizeof(hdr));
        rec_cn = game::player1 ? game::player1->clientnum : -1;
        recmode = game::gamemode;
        splitnext = false;
        matchid = uint(time(NULL));
        writehdheader();
        recstart = totalmillis ? totalmillis : 1;
        lastsamplet = -1;
        skippackets = 0;
        committed = false;
        waitstart = totalmillis;
        active = true;
        needplayersnap = false;
        if(snapshot)
        {
            writesnapshot();
            skippackets = 0;
        }
        else needplayersnap = true;
        if(!welcomed)
        {
            welcomed = true;
            conoutf("recording a local demo + HD aim track. options \f0record\f7 to extract / keep count");
        }
    }

    static void finish(bool keep)
    {
        if(!active)
        {
            skippackets = 0;
            return;
        }
        string dkeep, hkeep;
        copystring(dkeep, dmofile);
        copystring(hkeep, hdfile);
        int duration = recstart ? max(totalmillis - recstart, 0) : 0;
        closestreams();
        active = false;
        skippackets = 0;
        needplayersnap = false;
        lastsamplet = -1;
        bool drop = !keep || !committed || duration < DEMOHD_MINMS;
        if(drop)
        {
            hdremove(dkeep);
            hdremove(hkeep);
        }
        else
        {
            conoutf("saved local demo + HD \"%s.dmo\"", dmostem);
            prune();
        }
        dmofile[0] = hdfile[0] = dmostem[0] = 0;
        committed = false;
        splitnext = false;
    }

    static bool fileless(const fileitem &a, const fileitem &b) { return a.mtime < b.mtime; }

    static bool already(const char *stem)
    {
        loopv(files) if(!strcasecmp(files[i].stem, stem)) return true;
        return false;
    }

    static bool readable(const char *p)
    {
        struct stat st;
        return p && p[0] && stat(p, &st)==0;
    }

    static void addstem(const char *stem)
    {
        if(!stem || !stem[0] || already(stem)) return;
        fileitem &it = files.add();
        copystring(it.stem, stem);
        defformatstring(drel, "demo/%s.dmo", stem);
        defformatstring(hrel, "demo/%s.dmohd", stem);
        resolvepath(it.path, drel, "r");
        resolvepath(it.hdpath, hrel, "r");
        formatstring(it.label, "%s.dmo", stem);
        it.mtime = 0;
        it.size = 0;
        struct stat st;
        if(!readable(it.path))
        {
            defformatstring(local, "demo%c%s.dmo", PATHDIV, stem);
            if(stat(local, &st)!=0) { files.pop(); return; }
            copystring(it.path, local);
        }
        else if(stat(it.path, &st)!=0) { files.pop(); return; }
        it.mtime = st.st_mtime;
        it.size = (long)st.st_size;
        if(!readable(it.hdpath))
        {
            defformatstring(localhd, "demo%c%s.dmohd", PATHDIV, stem);
            if(stat(localhd, &st)==0)
            {
                copystring(it.hdpath, localhd);
                it.size += (long)st.st_size;
            }
        }
        else if(stat(it.hdpath, &st)==0) it.size += (long)st.st_size;
        if(active && dmofile[0] && !strcasecmp(it.path, dmofile)) files.pop();
    }

    static void scanfiles(bool force = false)
    {
        if(!force && scanmillis && totalmillis - scanmillis < 400) return;
        scanmillis = totalmillis ? totalmillis : 1;
        files.setsize(0);
        vector<char *> names;
        listfiles("demo", "dmo", names);
        listfiles("demo", "dmohd", names);
        loopv(names)
        {
            addstem(names[i]);
            DELETEA(names[i]);
        }
        files.sort(fileless);
    }

    void prune()
    {
        scanfiles(true);
        while(files.length() > demokeep)
        {
            fileitem it = files.remove(0);
            remove(it.path);
            remove(it.hdpath);
        }
    }

    static float lerpyaw(float a, float b, float k)
    {
        float d = b - a;
        while(d > 180) d -= 360;
        while(d < -180) d += 360;
        float r = a + d*k;
        while(r < 0) r += 360;
        while(r >= 360) r -= 360;
        return r;
    }

    static int scandmolen(const char *path)
    {
        if(!path || !path[0]) return 0;
        stream *f = opengzfile(path, "rb");
        if(!f) return 0;
        demoheader hdr;
        if(f->read(&hdr, sizeof(hdr)) != sizeof(hdr)) { delete f; return 0; }
        int lastt = 0, stamp[3];
        uchar skip[512];
        while(f->read(stamp, sizeof(stamp)) == sizeof(stamp))
        {
            lilswap(stamp, 3);
            lastt = max(lastt, stamp[0]);
            int len = stamp[2];
            if(len < 0 || len > (1<<20)) break;
            bool ok = true;
            while(len > 0)
            {
                int n = min(len, (int)sizeof(skip));
                if(f->read(skip, n) != n) { ok = false; break; }
                len -= n;
            }
            if(!ok) break;
        }
        delete f;
        return lastt;
    }

    static bool hdsampleless(const hdsample &a, const hdsample &b) { return a.t < b.t; }

    static bool loadtrack(const char *dmopath)
    {
        track.setsize(0);
        play_loaded = false;
        play_cn = -1;
        play_idx = 0;
        nplaycns = nexactcns = 0;
        memset(playhas, 0, sizeof(playhas));
        memset(playexact, 0, sizeof(playexact));
        autofollowdone = false;
        string hdpath;
        sidecarname(hdpath, dmopath, sizeof(hdpath));
        stream *f = openrawfile(hdpath, "rb");
        if(!f)
        {
            string alt;
            copystring(alt, hdpath);
            f = openrawfile(path(alt), "rb");
        }
        if(!f)
        {
            logoutf("no HD aim file next to this demo");
            return false;
        }
        char magic[8];
        if(f->read(magic, 8) != 8 || memcmp(magic, DEMOHD_MAGIC, 7))
        {
            logoutf("HD aim file is unreadable");
            delete f;
            return false;
        }
        int version = f->getlil<ushort>();
        f->getlil<ushort>();
        int protocol = f->getlil<int>();
        f->getlil<int>();
        play_cn = f->getlil<int>();
        f->getlil<uint>();
        char skip[64];
        f->read(skip, 64);
        f->read(skip, 64);
        f->read(skip, 64);
        if(version != DEMOHD_VERSION)
        {
            logoutf("HD aim file is from another version");
            delete f;
            return false;
        }
        (void)protocol;
        hdsample s;
        while(readsample(f, s)) track.add(s);
        delete f;
        // server tracks stamped with each player's own (spaced) sample times
        // interleave slightly; playback walks one time-ordered list
        insertionsort(track.getbuf(), track.length(), hdsampleless);
        play_loaded = track.length() > 0;
        play_idx = 0;
        nplaycns = nexactcns = 0;
        memset(playhas, 0, sizeof(playhas));
        memset(playexact, 0, sizeof(playexact));
        loopv(track)
        {
            int c = track[i].cn;
            if(c < 0 || c > 255) continue;
            if(!playhas[c]) { playhas[c] = 1; nplaycns++; playfirst[c] = track[i].t; }
            playlast[c] = track[i].t;
            if((track[i].flags & HD_OWN) && !playexact[c]) { playexact[c] = 1; nexactcns++; }
        }
        if(play_loaded) logoutf("HD aim tracks loaded (%d players)", nplaycns);
        else logoutf("HD aim file is empty");
        return play_loaded;
    }

    // Curved playback between samples (live and demo): cubic Hermite with
    // monotone tangents (Fritsch-Carlson: mean of the neighbouring slopes,
    // at most 3x the smaller one, zero when they change sign), so a sharp
    // stop never overshoots. 0 = straight lines.
    VAR(hdcurve, 0, 1, 1);

    static float hdslope(float v0, float v1, int t0, int t1) { return t1 > t0 ? (v1 - v0)/float(t1 - t0) : 0.0f; }

    static float hdtangent(float d0, float d1)
    {
        if(d0*d1 <= 0) return 0;
        float m = (d0 + d1)/2, lim = 3*min(fabs(d0), fabs(d1));
        return clamp(m, -lim, lim);
    }

    // value between v1 (time t1) and v2 (t2); v0/t0 and v3/t3 are the outer neighbours
    // (t0 == t1 or t3 == t2 when there is none)
    static float hdhermite(float v0, float v1, float v2, float v3, int t0, int t1, int t2, int t3, float k)
    {
        float h = float(t2 - t1), m = hdslope(v1, v2, t1, t2);
        float m1 = t0 < t1 ? hdtangent(hdslope(v0, v1, t0, t1), m) : m;
        float m2 = t3 > t2 ? hdtangent(m, hdslope(v2, v3, t2, t3)) : m;
        float k2 = k*k, k3 = k2*k;
        return (2*k3 - 3*k2 + 1)*v1 + (k3 - 2*k2 + k)*h*m1 + (-2*k3 + 3*k2)*v2 + (k3 - k2)*h*m2;
    }

    static float hdangle(float from, float to)
    {
        float d = to - from;
        while(d > 180) d -= 360;
        while(d < -180) d += 360;
        return d;
    }

    static bool lerpsample(vector<hdsample> &src, int &idx, int t, int cn, hdsample &out)
    {
        if(src.empty()) return false;
        int i = idx;
        if(i < 0) i = 0;
        if(i >= src.length()) i = src.length()-1;
        while(i+1 < src.length() && src[i+1].t <= t) i++;
        while(i > 0 && src[i].t > t) i--;
        idx = i;
        int a = -1, b = -1;
        for(int j = i; j < src.length(); j++) if(src[j].cn==cn || cn<0) { if(src[j].t >= t) { b = j; break; } a = j; }
        if(a < 0)
        {
            for(int j = i; j >= 0; j--) if(src[j].cn==cn || cn<0) { a = j; break; }
        }
        if(a < 0 && b < 0) return false;
        if(b < 0) { out = src[a]; return true; }
        if(a < 0) { out = src[b]; return true; }
        const hdsample &sa = src[a], &sb = src[b];
        float k = sb.t==sa.t ? 1 : clamp(float(t - sa.t)/float(sb.t - sa.t), 0.0f, 1.0f);
        out = sa;
        out.yaw = lerpyaw(sa.yaw, sb.yaw, k);
        out.pitch = sa.pitch + (sb.pitch - sa.pitch)*k;
        out.roll = sa.roll + (sb.roll - sa.roll)*k;
        out.o.x = sa.o.x + (sb.o.x - sa.o.x)*k;
        out.o.y = sa.o.y + (sb.o.y - sa.o.y)*k;
        out.o.z = sa.o.z + (sb.o.z - sa.o.z)*k;
        out.pad = uchar(clamp(int(sa.pad + (sb.pad - sa.pad)*k + 0.5f), 0, 255));
        if(k > 0.5f) out.flags = sb.flags;
        if(hdcurve && sb.t > sa.t)
        {
            int p = a, n = b;
            for(int j = a-1; j >= 0; j--) if(src[j].cn==sa.cn) { p = j; break; }
            for(int j = b+1; j < src.length(); j++) if(src[j].cn==sb.cn) { n = j; break; }
            const hdsample &sp = src[p], &sn = src[n];
            // yaw unwrapped around sa (359 -> 0 is a 1 degree step)
            float y0 = -hdangle(sp.yaw, sa.yaw), y2 = hdangle(sa.yaw, sb.yaw), y3 = y2 + hdangle(sb.yaw, sn.yaw);
            float yaw = sa.yaw + hdhermite(y0, 0, y2, y3, sp.t, sa.t, sb.t, sn.t, k);
            while(yaw < 0) yaw += 360;
            while(yaw >= 360) yaw -= 360;
            out.yaw = yaw;
            #define HDCURVE(f) out.f = hdhermite(sp.f, sa.f, sb.f, sn.f, sp.t, sa.t, sb.t, sn.t, k)
            HDCURVE(pitch); HDCURVE(roll); HDCURVE(o.x); HDCURVE(o.y); HDCURVE(o.z);
            #undef HDCURVE
        }
        return true;
    }

    void writeshoot(int cn, int gun, int id, const vec &from, const vec &to)
    {
        if(!active || game::demoplayback || cn < 0) return;
        uchar buf[64];
        ucharbuf p(buf, sizeof(buf));
        putint(p, N_SHOTFX);
        putint(p, cn);
        putint(p, gun);
        putint(p, id);
        loopk(3) putint(p, int(from[k]*DMF));
        loopk(3) putint(p, int(to[k]*DMF));
        writedemopacket(1, p.buf, p.length());
    }

    void writepos(const uchar *data, int len)
    {
        if(!active || game::demoplayback || !data || len <= 0) return;
        writedemopacket(0, data, len);
    }

    void afterpacket(int chan, const uchar *data, int len)
    {
        if(!active || game::demoplayback) return;
        if(chan > 1) return;
        if(skippackets > 0) { skippackets--; return; }
        if(skipincoming(data, len)) return;
        writedemopacket(chan, data, len);
        if(needplayersnap)
        {
            needplayersnap = false;
            writeplayers();
        }
    }

    void captureframe()
    {
        if(game::demoplayback || !game::player1 || !camera1) return;
        bool neednet = game::clanmodactive() && game::player1->state != CS_SPECTATOR && game::player1->clientnum >= 0;
        if(!active && !neednet) return;
        if(active && committed == false && game::player1->state != CS_SPECTATOR && totalmillis - waitstart > 2000)
            committed = true;
        int t = clockms();
        if(lastsamplet >= 0 && t - lastsamplet < DEMOHD_MINDT) return;
        lastsamplet = t;
        hdsample s;
        s.t = t;
        s.cn = rec_cn >= 0 ? rec_cn : game::player1->clientnum;
        s.flags = HD_FRAME;
        if(game::player1->state==CS_SPECTATOR) s.flags |= HD_SPEC;
        else s.flags |= HD_OWN;
        s.gun = game::player1->gunselect;
        s.state = game::player1->state;
        int zq = clamp(int(getzoomprogress()*31.0f + 0.5f), 0, 31);
        s.flags |= uchar(zq << HD_ZOOM_SHIFT);
        s.pad = uchar(clamp(int(getzoomprogress()*255.0f + 0.5f), 0, 255));
        s.yaw = camera1->yaw;
        s.pitch = camera1->pitch;
        s.roll = camera1->roll;
        s.o = camera1->o;
        if(active)
        {
            writehdsample(s);
            loopv(game::players)
            {
                fpsent *d = game::players[i];
                if(!d || d==game::player1 || d->clientnum < 0 || d->clientnum > 255) continue;
                if(d->state==CS_SPECTATOR || d->state==CS_SPAWNING) continue;
                hdsample w;
                w.t = t;
                w.cn = d->clientnum;
                w.flags = HD_FRAME;
                w.gun = d->gunselect;
                w.state = d->state;
                w.pad = 0;
                w.yaw = d->yaw;
                w.pitch = d->pitch;
                w.roll = d->roll;
                w.o = d->o;
                writehdsample(w);
            }
        }
        if(neednet)
        {
            s.cn = game::player1->clientnum;
            s.flags = (s.flags & ~HD_SPEC) | HD_OWN;
            if(pending.length() >= HD_SEND_MAX*2) pending.remove(0, pending.length() - HD_SEND_MAX);
            pending.add(s);
        }
    }

    void flushtoServer()
    {
        if(!game::clanmodactive() || pending.empty()) { pending.setsize(0); return; }
        int n = min(pending.length(), HD_SEND_MAX);
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_HDPOS);
        putint(p, n);
        loopi(n) putsamplemsg(p, pending[i]);
        pending.remove(0, n);
        sendclientpacket(p.finalize(), 1);
    }

    // Live spectating: a compatible server relays each player's samples with
    // their own server time "t", correctly spaced. Local time of a sample =
    // t + offset (per player); the view is drawn hdlivedelay ms in the past,
    // the same delay for every player, so a packet (about 33 ms of samples)
    // plays at its real pace and changing the followed player keeps the delay.
    // The offset aims at the smallest delay seen (+1 ms per packet for clock
    // drift, restart on a jump) and moves by 1 ms per packet at most, so the
    // timeline never jumps. Packets whose t are all equal (older servers) or
    // out of order keep the fixed 4 ms spacing and no delay, as before.
    VARP(hdlivedelay, 0, 200, 1000);

    struct liveclock { int off, offgoal, lastt, laststamp; bool timed; };
    static liveclock liveclocks[256];

    static void resetliveclock(liveclock &c) { c.off = c.offgoal = c.lastt = c.laststamp = 0; c.timed = false; }

    // local time shown for this player: the spectator's replay delay (see
    // game::updatespecqueue), which moves toward hdlivedelay
    static int livetime(int cn)
    {
        return cn >= 0 && cn <= 255 && liveclocks[cn].timed ? lastmillis - game::specviewdelay() : lastmillis;
    }

    int livedelay() { return hdlivedelay; }

    // the server has the clan extras (handshake done) and is not an older one
    // that relays HD aim with one time per packet (seen and never timed)
    static bool livetimedseen = false, liveoldseen = false;
    bool livecompatible() { return game::clanmodactive() && (livetimedseen || !liveoldseen); }

    static void clearliveclocks() { loopi(256) resetliveclock(liveclocks[i]); }

    // drop samples of this player stamped at or after t (timeline restarts)
    static void cutlive(int cn, int t)
    {
        loopvrev(live) if(live[i].cn==cn && live[i].t >= t) live.remove(i);
    }

    static void insertlive(const hdsample &s)
    {
        int at = live.length();
        while(at > 0 && live[at-1].t > s.t) at--;
        live.insert(at, s);
        if(at <= live_idx && live_idx < live.length()-1) live_idx++;
    }

    void parselive(ucharbuf &p)
    {
        int n = getint(p);
        if(n < 1 || n > 32) return;
        hdsample in[32];
        loopi(n) if(!getsamplemsg(p, in[i])) return;

        int cn = in[0].cn;
        bool onecn = cn >= 0 && cn <= 255, ordered = true, spaced = false;
        loopi(n)
        {
            if(in[i].cn != cn) onecn = false;
            if(i && in[i].t < in[i-1].t) ordered = false;
            if(i && in[i].t - in[i-1].t > 1000) ordered = false;
            if(i && in[i].t > in[i-1].t) spaced = true;
        }
        bool timed = false;
        if(onecn && ordered)
        {
            liveclock &c = liveclocks[cn];
            if(n > 1) timed = spaced;
            else timed = c.timed && in[0].t > c.lastt && in[0].t - c.lastt <= 1000;
        }
        if(!timed && n > 1) liveoldseen = true;
        if(!timed)
        {
            if(onecn && liveclocks[cn].timed)
            {
                cutlive(cn, lastmillis - (n-1)*DEMOHD_MINDT);
                resetliveclock(liveclocks[cn]);
            }
            loopi(n)
            {
                hdsample s = in[i];
                s.t = lastmillis - (n-1-i)*DEMOHD_MINDT;
                live.add(s);
            }
            prunelive();
            return;
        }

        liveclock &c = liveclocks[cn];
        int delay = lastmillis - in[n-1].t;
        bool restart = !c.timed || in[0].t <= c.lastt || in[0].t - c.lastt > 1000;
        if(restart) c.off = c.offgoal = delay;
        else
        {
            c.offgoal = min(c.offgoal + 1, delay);
            c.off += clamp(c.offgoal - c.off, -1, 1);
        }
        int first = in[0].t + c.off;
        if(restart)
        {
            cutlive(cn, first);
            c.laststamp = first - 1;
        }
        loopi(n)
        {
            hdsample s = in[i];
            s.t = max(in[i].t + c.off, c.laststamp + 1);
            c.laststamp = s.t;
            insertlive(s);
        }
        c.lastt = in[n-1].t;
        c.timed = true;
        livetimedseen = true;
        prunelive();
    }

    void clearlive()
    {
        live.setsize(0);
        pending.setsize(0);
        live_idx = 0;
        clearliveclocks();
    }

    void onstartgame(bool skip)
    {
        const char *map = game::getclientmap();
        if(skip)
        {
            clearlive();
            finish(true);
            sessionmap = false;
            return;
        }
        if(active && !splitnext && map[0] && lastmap[0] && !strcasecmp(map, lastmap))
        {
            committed = committed || game::player1->state != CS_SPECTATOR;
            return;
        }
        clearlive();
        finish(true);
        copystring(curmap, map);
        copystring(lastmap, map);
        if(skip || mainmenu || !demohdrecord || !m_mp(game::gamemode) || m_check(game::gamemode, M_EDIT))
        {
            sessionmap = false;
            return;
        }
        bool snap = sessionmap;
        sessionmap = true;
        startfiles(snap);
    }

    void onselfspawn()
    {
        if(active) committed = true;
    }

    void ondisconnect()
    {
        livetimedseen = liveoldseen = false;
        clearlive();
        finish(true);
        sessionmap = false;
        lastmap[0] = 0;
        track.setsize(0);
        play_loaded = false;
        play_cn = -1;
        autofollowdone = false;
    }

    void onplayback(const char *dmofilepath)
    {
        dmolenms = scandmolen(dmofilepath);
        dbdrag = false;
        dbhidden = false;
        loadtrack(dmofilepath);
        if(track.length()) dmolenms = max(dmolenms, track.last().t);
        play_idx = 0;
        autofollowdone = false;
    }

    void onplaybackend()
    {
        track.setsize(0);
        play_loaded = false;
        play_cn = -1;
        nplaycns = nexactcns = 0;
        memset(playhas, 0, sizeof(playhas));
        memset(playexact, 0, sizeof(playexact));
        play_idx = 0;
        dmolenms = 0;
        dbdrag = false;
        dbhidden = false;
        autofollowdone = false;
    }

    void onplaybackreset()
    {
        play_idx = 0;
    }

    int recordercn() { return play_cn; }
    bool isrecorder(fpsent *d)
    {
        return game::demoplayback && d && play_cn >= 0 && d->clientnum == play_cn;
    }

    bool ally(fpsent *d)
    {
        if(!isrecorder(d))
        {
            if(!game::demoplayback || !d || play_cn < 0) return false;
            fpsent *r = game::getclient(play_cn);
            if(!r || !m_check(game::gamemode, M_TEAM) || !d->team[0] || !r->team[0]) return false;
            if(d->state==CS_SPECTATOR || r->state==CS_SPECTATOR) return false;
            return !strcmp(d->team, r->team);
        }
        return true;
    }

    bool allyteam(const char *team)
    {
        if(!game::demoplayback || play_cn < 0 || !team || !team[0] || !m_check(game::gamemode, M_TEAM)) return false;
        fpsent *r = game::getclient(play_cn);
        if(!r || !r->team[0]) return false;
        return !strcmp(team, r->team);
    }

    int visteam(fpsent *d)
    {
        if(!game::demoplayback || play_cn < 0 || !d) return 0;
        return ally(d) ? 1 : 2;
    }

    int namecolor(fpsent *d, int fallback)
    {
        int vt = visteam(d);
        if(!vt || (d && d->state==CS_SPECTATOR)) return fallback;
        int c = vt==1 ? 0x6496FF : 0xFF4B19;
        if(d->state==CS_DEAD) c = (c>>1)&0x7F7F7F;
        return c;
    }
    bool hastrack(int cn)
    {
        if(!play_loaded) return false;
        if(cn < 0) return play_cn >= 0;
        if(cn > 255 || !playhas[cn]) return false;
        if(!game::demoplayback) return true;
        int t = server::demotime();
        return t >= playfirst[cn] - HD_TRACK_SLACK && t <= playlast[cn] + HD_TRACK_SLACK;
    }

    // exact aim known for this player: HD track in the demo, or live HD
    // stream from a custom server while spectating
    bool hdaim(fpsent *d)
    {
        if(!d || d->clientnum < 0) return false;
        if(game::demoplayback) return play_loaded && d->clientnum <= 255 && playexact[d->clientnum] && hastrack(d->clientnum);
        return game::clanmodactive() && game::player1 && game::player1->state==CS_SPECTATOR && haslive(d->clientnum);
    }

    bool applycamera(fpsent *target)
    {
        if(!demohdplay || !target) return false;
        hdsample s;
        bool ok = false;
        if(game::demoplayback && play_loaded && hastrack(target->clientnum))
            ok = lerpsample(track, play_idx, server::demotime(), target->clientnum, s);
        else if(!game::demoplayback && game::clanmodactive() && game::player1 && game::player1->state==CS_SPECTATOR && haslive(target->clientnum))
            ok = lerpsample(live, live_idx, livetime(target->clientnum), target->clientnum, s);
        if(!ok) return false;
        game::player1->yaw = s.yaw;
        game::player1->pitch = s.pitch;
        game::player1->roll = s.roll;
        game::player1->o = s.o;
        game::player1->resetinterp();
        target->yaw = s.yaw;
        target->pitch = s.pitch;
        target->roll = s.roll;
        target->o = s.o;
        target->smoothmillis = 0;
        target->resetinterp();
        hdzoomframe = true;
        hdzoomval = s.pad / 255.0f;
        return true;
    }

    void applyothers(fpsent *skip)
    {
        if(!game::demoplayback || !demohdplay || !play_loaded) return;
        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(!d || d==skip || d==game::player1) continue;
            if(d->state==CS_SPECTATOR) continue;
            if(d->state==CS_DEAD && d->ragdoll) continue;
            if(!hastrack(d->clientnum)) continue;
            hdsample s;
            if(!lerpsample(track, play_idx, server::demotime(), d->clientnum, s)) continue;
            d->yaw = s.yaw;
            d->pitch = s.pitch;
            d->roll = s.roll;
            d->o = s.o;
            d->smoothmillis = 0;
            d->resetinterp();
        }
    }

    void applyzoom()
    {
        if(hdzoomframe) forcezoom(hdzoomval);
        hdzoomframe = false;
    }

    void tryautofollow()
    {
        if(!game::demoplayback || autofollowdone || play_cn < 0) return;
        if(game::following >= 0 && game::following != play_cn)
        {
            autofollowdone = true;
            return;
        }
        fpsent *r = game::getclient(play_cn);
        if(!r || r->state==CS_SPECTATOR) return;
        game::following = play_cn;
        autofollowdone = true;
    }

    // "HD" while the followed player is shown with his exact aim (demo track
    // he sent himself, or live samples), nothing otherwise
    const char *hudstatus()
    {
        fpsent *f = game::followingplayer();
        if(!f || !demohdplay || f->clientnum < 0 || f->clientnum > 255) return "";
        if(game::demoplayback) return play_loaded && hastrack(f->clientnum) && playexact[f->clientnum] ? "HD" : "";
        if(game::player1 && game::player1->state==CS_SPECTATOR && game::clanmodactive() && haslive(f->clientnum)) return "HD";
        return "";
    }

    void installsidecar(const char *dmoname, const uchar *data, int len)
    {
        if(!dmoname || !dmoname[0] || !data || len < 8) return;
        string base, hdname;
        copystring(base, dmoname);
        int n = strlen(base);
        if(n >= 4 && !strcasecmp(base+n-4, ".dmo")) base[n-4] = 0;
        formatstring(hdname, "%s.dmohd", base);
        const char *path = server::getdemofile(hdname, true);
        stream *f = openrawfile(path ? path : hdname, "wb");
        if(!f) return;
        f->write(data, len);
        delete f;
        conoutf("saved HD demo track \"%s\"", hdname);
    }

    // Server copy of the match replacing the local recording (AUTODEMO_CMD).
    // The local files move to demo/replaced (kept AUTODEMO_KEEPDAYS days),
    // the server's take their name. Anything unexpected keeps the local ones.
    #define AUTODEMO_DIR "demo/replaced"
    #define AUTODEMO_KEEPDAYS 7
    // the local recording may start before the server's (map load) but must
    // not cover much more than this match
    #define AUTODEMO_SLACK 20000
    #define AUTODEMO_HDHEADER 220
    #define AUTODEMO_HDSAMPLE 32

    // full path (stat, move, remove); streams take the relative name
    static void autodemopath(char *dst, const char *fmt, const char *stem, char *reldst = NULL)
    {
        defformatstring(rel, fmt, stem);
        path(rel);
        if(reldst) copystring(reldst, rel, MAXSTRLEN);
        const char *found = findfile(rel, "w");
        copystring(dst, found ? found : rel, MAXSTRLEN);
    }

    static bool autodemoexists(const char *p)
    {
        struct stat st;
        return stat(p, &st)==0;
    }

    static bool autodemomove(const char *from, const char *to)
    {
        if(autodemoexists(to)) return false; // never overwrite
#ifdef WIN32
        return MoveFileExA(from, to, MOVEFILE_WRITE_THROUGH) != 0;
#else
        return rename(from, to)==0;
#endif
    }

    static bool autodemowrite(const char *p, const uchar *data, int len)
    {
        stream *f = openrawfile(p, "wb");
        if(!f) return false;
        bool ok = f->write(data, len) == size_t(len);
        delete f;
        return ok;
    }

    static void autodemopurge()
    {
        time_t now = time(NULL);
        static const char * const exts[2] = { "dmo", "dmohd" };
        loopk(2)
        {
            vector<char *> names;
            listfiles(AUTODEMO_DIR, exts[k], names);
            loopv(names)
            {
                defformatstring(rel, "%s/%s.%s", AUTODEMO_DIR, names[i], exts[k]);
                path(rel);
                const char *found = findfile(rel, "r");
                struct stat st;
                if(found && stat(found, &st)==0 && now - st.st_mtime > AUTODEMO_KEEPDAYS*24*3600) remove(found);
                DELETEA(names[i]);
            }
        }
    }

    // the HD aim file: same match (map, mode), whole samples, and the
    // player's own aim in it (else the local file knows more)
    static bool autodemohdok(const uchar *data, int len, const char *map, int mode, int cn)
    {
        if(!data || len < AUTODEMO_HDHEADER + AUTODEMO_HDSAMPLE || (len - AUTODEMO_HDHEADER) % AUTODEMO_HDSAMPLE) return false;
        if(memcmp(data, DEMOHD_MAGIC, sizeof(DEMOHD_MAGIC))) return false;
        ushort version;
        int hdmode;
        memcpy(&version, data + 8, 2);
        memcpy(&hdmode, data + 16, 4);
        lilswap(&version, 1);
        lilswap(&hdmode, 1);
        if(version != DEMOHD_VERSION || hdmode != mode) return false;
        char hdmap[65];
        memcpy(hdmap, data + 28, 64);
        hdmap[64] = 0;
        if(strcasecmp(hdmap, map)) return false;
        for(int off = AUTODEMO_HDHEADER; off < len; off += AUTODEMO_HDSAMPLE)
            if(data[off+4] == cn && (data[off+5] & HD_OWN)) return true;
        return false;
    }

    // the .dmo: readable to its last packet with a matching gzip check, and
    // its first packet is the welcome of the same match; returns its length
    // in ms, -1 when it is not usable
    static int autodemodmolen(const char *p, const uchar *raw, int rawlen, const char *map, int mode)
    {
        if(!raw || rawlen < 18 + 8) return -1;
        stream *f = opengzfile(p, "rb");
        if(!f) return -1;
        uint total = 0;
        int lastt = -1;
        demoheader hdr;
        bool ok = f->read(&hdr, sizeof(hdr)) == sizeof(hdr) && !memcmp(hdr.magic, DEMO_MAGIC, sizeof(hdr.magic));
        if(ok)
        {
            total += sizeof(hdr);
            lilswap(&hdr.version, 2);
            ok = hdr.version == DEMO_VERSION && hdr.protocol == PROTOCOL_VERSION;
        }
        bool first = true;
        uchar skip[4096];
        while(ok)
        {
            int stamp[3];
            size_t n = f->read(stamp, sizeof(stamp));
            if(!n) break;
            if(n != sizeof(stamp)) { ok = false; break; }
            total += sizeof(stamp);
            lilswap(stamp, 3);
            int len = stamp[2];
            if(len < 0 || len > (1<<24)) { ok = false; break; }
            lastt = max(lastt, stamp[0]);
            if(first)
            {
                first = false;
                uchar *buf = new (false) uchar[max(len, 1)];
                if(!buf || f->read(buf, len) != size_t(len)) { DELETEA(buf); ok = false; break; }
                total += len;
                ucharbuf q(buf, len);
                string text;
                ok = getint(q) == N_WELCOME && getint(q) == N_MAPCHANGE;
                if(ok) { getstring(text, q); ok = !q.overread() && !strcasecmp(text, map) && getint(q) == mode && !q.overread(); }
                delete[] buf;
                continue;
            }
            while(len > 0)
            {
                int k = min(len, int(sizeof(skip)));
                if(f->read(skip, k) != size_t(k)) { ok = false; break; }
                total += k;
                len -= k;
            }
        }
        uint crc = f->getcrc();
        delete f;
        if(!ok || first) return -1;
        const uchar *t = raw + rawlen - 8;
        uint rcrc = t[0] | (t[1]<<8) | (t[2]<<16) | (uint(t[3])<<24);
        uint rsize = t[4] | (t[5]<<8) | (t[6]<<16) | (uint(t[7])<<24);
        if(rcrc != crc || rsize != total) return -1;
        return lastt;
    }

    // the local recording of the match that just ended, if any; it will be
    // closed at the next map even when the same map is played again
    bool autodemolocal(const char *map, int mode, char *stem, int stemlen, int &elapsed)
    {
        if(!active || !committed || !dmo || !dmostem[0] || !map || !map[0]) return false;
        if(strcasecmp(curmap, map) || recmode != mode) return false;
        copystring(stem, dmostem, stemlen);
        elapsed = recstart ? max(totalmillis - recstart, 0) : 0;
        splitnext = true;
        autodemopurge();
        return true;
    }

    bool autodemoreplace(const char *stem, const char *map, int mode, int cn, int elapsed, const uchar *dmodata, int dmolen, const uchar *hddata, int hdlen)
    {
        if(!stem || !stem[0] || !autodemohdok(hddata, hdlen, map, mode, cn)) return false;
        string ldmo, lhd, tdmo, thd, bdmo, bhd, rdmo, rhd;
        autodemopath(ldmo, "demo/%s.dmo", stem);
        autodemopath(lhd, "demo/%s.dmohd", stem);
        autodemopath(tdmo, "demo/%s.dmo.srv", stem, rdmo);
        autodemopath(thd, "demo/%s.dmohd.srv", stem, rhd);
        autodemopath(bdmo, AUTODEMO_DIR "/%s.dmo", stem);
        autodemopath(bhd, AUTODEMO_DIR "/%s.dmohd", stem);
        bool ok = autodemowrite(rdmo, dmodata, dmolen) && autodemowrite(rhd, hddata, hdlen);
        if(ok)
        {
            int len = autodemodmolen(rdmo, dmodata, dmolen, map, mode);
            ok = len >= DEMOHD_MINMS && elapsed <= len + AUTODEMO_SLACK;
        }
        if(ok && active && !strcmp(dmostem, stem)) finish(true);
        // the local files must still be there (not dropped or pruned), and
        // an older copy in demo/replaced is never overwritten
        if(ok) ok = autodemoexists(ldmo) && !autodemoexists(bdmo) && !autodemoexists(bhd);
        bool hadhd = ok && autodemoexists(lhd);
        if(ok) ok = autodemomove(ldmo, bdmo);
        if(ok && hadhd && !autodemomove(lhd, bhd))
        {
            autodemomove(bdmo, ldmo);
            ok = false;
        }
        if(ok && !autodemomove(tdmo, ldmo))
        {
            autodemomove(bdmo, ldmo);
            if(hadhd) autodemomove(bhd, lhd);
            ok = false;
        }
        if(ok && !autodemomove(thd, lhd))
        {
            remove(ldmo);
            autodemomove(bdmo, ldmo);
            if(hadhd) autodemomove(bhd, lhd);
            ok = false;
        }
        remove(tdmo);
        remove(thd);
        scanmillis = 0;
        return ok;
    }

    void stoprecording() { finish(true); }

    static void dbset(int id, float x, float y, float w, float h)
    {
        if(id < 0 || id >= 8) return;
        dbbox[id][0] = x; dbbox[id][1] = y; dbbox[id][2] = w; dbbox[id][3] = h;
    }

    static int dbhit(float px, float py)
    {
        // the buttons first, then the bar behind them (0)
        for(int n = 1; n <= 8; n++)
        {
            int i = n%8;
            if(dbbox[i][2] <= 0) continue;
            if(px >= dbbox[i][0] && py >= dbbox[i][1] && px < dbbox[i][0]+dbbox[i][2] && py < dbbox[i][1]+dbbox[i][3])
                return i;
        }
        return -1;
    }

    static void dbtime(int ms, char *dst, int len)
    {
        ms = max(ms, 0);
        nformatstring(dst, len, "%d:%02d", ms/60000, (ms/1000)%60);
    }

    static void dbdrawbtn(int id, int w, int h, int hover, const char *label, bool on)
    {
        float bx = dbbox[id][0]*w, by = dbbox[id][1]*h, bw = dbbox[id][2]*w, bh = dbbox[id][3]*h;
        int col = on ? 0x4A90D9 : (hover==id ? 0x80C0FF : 0x3A3A48);
        hudrect(bx, by, bw, bh, col, 220);
        int lw = 0, lh = 0;
        text_bounds(label, lw, lh);
        draw_text(label, int(bx + (bw-lw)/2), int(by + (bh-lh)/2), 255, 255, 255, 255);
    }

    void drawhud(int w, int h)
    {
        loopi(8) dbbox[i][2] = 0;
        if(!game::demoplayback || !w || !h) return;

        float cx, cy;
        g3d_cursorpos(cx, cy);

        float hidew = max(72.0f, h*0.08f), hideh = max(28.0f, h*0.035f);
        dbset(7, w - hidew - 12, 12, hidew, hideh);
        loopi(8) { dbbox[i][0] /= w; dbbox[i][1] /= h; dbbox[i][2] /= w; dbbox[i][3] /= h; }

        int hover = dbhit(cx, cy);
        dbdrawbtn(7, w, h, hover, dbhidden ? "SHOW" : "HIDE", false);
        if(dbhidden) return;

        int dur = max(dmolenms, 1);
        int now = clamp(server::demotime(), 0, dur);
        float k = dbdrag ? dbdragk : clamp(float(now)/float(dur), 0.0f, 1.0f);

        float margin = w*0.03f;
        float bh = max(40.0f, h*0.06f);
        float y = h - bh - h*0.018f;
        float x = margin;
        float btnw = bh*1.65f, gap = 6;
        float btn = bh - 10;
        float by = y + 5;

        dbset(0, margin, y, w - 2*margin, bh); // the bar itself: a click on it is not for the game
        dbset(1, x, by, btnw, btn);
        x += btnw + gap;
        dbset(2, x, by, btnw, btn);
        x += btnw + gap;
        dbset(3, x, by, btnw, btn);
        x += btnw + gap + 8;

        float right = w - margin;
        float spdw = btnw*1.15f;
        dbset(6, right - spdw, by, spdw, btn);
        dbset(5, right - spdw*2 - gap, by, spdw, btn);

        string tnow, tdur;
        dbtime(int(k*dur + 0.5f), tnow, sizeof(tnow));
        dbtime(dur, tdur, sizeof(tdur));
        defformatstring(times, "%s / %s", tnow, tdur);
        int tw = 0, th = 0;
        text_bounds(times, tw, th);
        float timew = tw + 16;
        float sliderx = x;
        float sliderw = max(80.0f, dbbox[5][0] - gap - 8 - timew - sliderx);
        dbset(4, sliderx, by + btn*0.28f, sliderw, btn*0.44f);
        float timex = sliderx + sliderw + 8;

        for(int i = 0; i <= 6; i++) { dbbox[i][0] /= w; dbbox[i][1] /= h; dbbox[i][2] /= w; dbbox[i][3] /= h; }

        if(dbdrag && dbbox[4][2] > 0)
        {
            dbdragk = clamp((cx - dbbox[4][0]) / dbbox[4][2], 0.0f, 1.0f);
            k = dbdragk;
        }

        hudrect(margin - 8, y - 6, w - 2*margin + 16, bh + 12, 0x101018, 170);

        hover = dbhit(cx, cy);
        dbdrawbtn(7, w, h, hover, "HIDE", false);
        const char *btnlbl[7] = { "", "-10s", game::ispaused() ? "PLAY" : "PAUSE", "+10s", "", "50%", "100%" };
        loopi(3) dbdrawbtn(i+1, w, h, hover, btnlbl[i+1], false);

        float sx = dbbox[4][0]*w, sy = dbbox[4][1]*h, sw = dbbox[4][2]*w, sh = dbbox[4][3]*h;
        hudrect(sx, sy, sw, sh, 0x2A2A33, 220);
        hudrect(sx, sy, sw*k, sh, hover==4 || dbdrag ? 0x70B0FF : 0x4A90D9, 230);
        hudrect(sx + sw*k - 5, sy - 4, 10, sh + 8, 0xE8F0FF, 255);

        draw_text(times, int(timex), int(by + (btn-th)/2), 220, 220, 220, 255);

        loopi(2)
        {
            int id = 5+i;
            bool on = (id==5 && game::gamespeed==50) || (id==6 && game::gamespeed==100);
            dbdrawbtn(id, w, h, hover, btnlbl[id], on);
        }
    }

    bool barkey(int code, bool isdown)
    {
        if(!game::demoplayback) return false;
        if(code != -1) return false;
        float cx, cy;
        g3d_cursorpos(cx, cy);
        int hit = dbhit(cx, cy);
        // a click away from the bar goes to the game (left click = next
        // player to watch, like the right click); its release too
        static bool barpressed = false;
        if(isdown && (hit < 0 || (dbhidden && hit != 7))) { barpressed = false; return false; }
        if(!isdown && !barpressed && !dbdrag) return false;
        barpressed = isdown;
        if(isdown)
        {
            if(hit==7)
            {
                dbhidden = !dbhidden;
                dbdrag = false;
            }
            else if(dbhidden) {}
            else if(hit==1) server::seekdemoms(max(server::demotime() - 10000, 0));
            else if(hit==2) server::forcepaused(!server::ispaused());
            else if(hit==3) server::seekdemoms(server::demotime() + 10000);
            else if(hit==4)
            {
                dbdrag = true;
                if(dbbox[4][2] > 0) dbdragk = clamp((cx - dbbox[4][0]) / dbbox[4][2], 0.0f, 1.0f);
            }
            else if(hit==5) game::changegamespeed(50);
            else if(hit==6) game::changegamespeed(100);
        }
        else if(dbdrag)
        {
            dbdrag = false;
            int dur = max(dmolenms, 1);
            server::seekdemoms(int(dbdragk * dur + 0.5f));
        }
        return true;
    }
}

static void hdprune() { demohd::prune(); }

void game::demohdsample() { demohd::captureframe(); }
void game::demohdapplyzoom() { demohd::applyzoom(); }
void game::demohdafterpacket(int chan, const uchar *data, int len) { if(!game::specpacketqueued()) demohd::afterpacket(chan, data, len); }
void game::demohdwritepos(const uchar *data, int len) { demohd::writepos(data, len); }

extern int hidehud;
bool game::demobarcursor() { return demoplayback && !mainmenu && !hidehud; }
bool game::demobarkey(int code, bool isdown) { return demohd::barkey(code, isdown); }

static bool hdcopyfile(const char *src, const char *dst)
{
    stream *in = openrawfile(src, "rb");
    if(!in) return false;
    stream *out = openrawfile(dst, "wb");
    if(!out) { delete in; return false; }
    uchar buf[8192];
    size_t n;
    while((n = in->read(buf, sizeof(buf))) > 0) out->write(buf, n);
    delete in;
    delete out;
    return true;
}

#ifdef WIN32
static void hdpickfolder(char *dst, int dstlen)
{
    dst[0] = 0;
    BROWSEINFOA bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = NULL;
    bi.lpszTitle = "Copy this demo to...";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if(!pidl) return;
    SHGetPathFromIDListA(pidl, dst);
    CoTaskMemFree(pidl);
    (void)dstlen;
}
#endif

ICOMMAND(localdemofolder, "", (),
{
    if(homedir[0])
    {
        defformatstring(s, "%sdemo", homedir);
        result(s);
    }
    else result("demo");
});
ICOMMAND(numlocaldemos, "", (), { demohd::scanfiles(); intret(demohd::files.length()); });
ICOMMAND(localdemoname, "i", (int *i),
{
    demohd::scanfiles();
    if(!demohd::files.inrange(*i)) { result(""); return; }
    demohd::fileitem &it = demohd::files[*i];
    struct stat hd;
    const char *hdtag = (it.hdpath[0] && stat(it.hdpath, &hd)==0) ? ", HD" : "";
    defformatstring(s, "%s  (%d MB%s)", it.label, max(int((it.size+500000)/1000000), 0), hdtag);
    result(s);
});
ICOMMAND(localdemostem, "i", (int *i),
{
    demohd::scanfiles();
    if(!demohd::files.inrange(*i)) { result(""); return; }
    result(demohd::files[*i].stem);
});
ICOMMAND(localdemoextract, "i", (int *i),
{
    demohd::scanfiles(true);
    if(!demohd::files.inrange(*i)) { conoutf(CON_WARN, "no demo there"); return; }
    demohd::fileitem it = demohd::files[*i];
#ifdef WIN32
    string destfolder;
    hdpickfolder(destfolder, sizeof(destfolder));
    if(!destfolder[0]) return;
    defformatstring(d1, "%s\\%s", destfolder, it.label);
    string hdlabel;
    copystring(hdlabel, it.label);
    int n = strlen(hdlabel);
    if(n >= 4 && !strcasecmp(hdlabel+n-4, ".dmo")) copystring(hdlabel+n-4, ".dmohd", sizeof(hdlabel)-size_t(n-4));
    else concatstring(hdlabel, ".dmohd");
    defformatstring(d2, "%s\\%s", destfolder, hdlabel);
    bool ok = hdcopyfile(it.path, d1);
    hdcopyfile(it.hdpath, d2);
    if(ok) conoutf("copied demo + HD track to %s (originals stay in the game folder)", destfolder);
    else conoutf(CON_ERROR, "could not copy the demo");
#else
    conoutf("demo is in the demo folder: %s", it.path);
#endif
});
ICOMMAND(playlocaldemo, "i", (int *i),
{
    demohd::scanfiles();
    if(!demohd::files.inrange(*i)) { conoutf(CON_WARN, "no demo there"); return; }
    if(game::remote) disconnect();
    if(!isconnected()) localconnect();
    game::changemap(demohd::files[*i].stem, -1);
});
ICOMMAND(toggledemorecord, "", (),
{
    demohdrecord = demohdrecord ? 0 : 1;
    if(!demohdrecord)
    {
        demohd::stoprecording();
        conoutf("local demo + HD recording off");
    }
    else
    {
        conoutf("local demo + HD recording on");
        game::announceautodemo();
    }
});
ICOMMAND(toggledemohd, "", (),
{
    if(!game::demoplayback) { conoutf("HD playback is only while watching a demo"); return; }
    if(!demohd::play_loaded)
    {
        conoutf("HD unavailable — aim 1° 30 Hz (no sidecar for this demo)");
        return;
    }
    demohdplay = demohdplay ? 0 : 1;
    conoutf("HD aim %s", demohdplay ? "on" : "off");
});
ICOMMAND(demohdstatus, "", (), result(demohd::hudstatus()));
ICOMMAND(demohdloaded, "", (), intret(demohd::play_loaded ? 1 : 0));
ICOMMAND(demorewind, "i", (int *ms),
{
    int delta = *ms ? *ms : 10000;
    if(!game::demoplayback) { conoutf("rewind is only while watching a demo"); return; }
    server::seekdemoms(max(server::demotime() - max(delta, 1), 0));
});
ICOMMAND(demoforward, "i", (int *ms),
{
    int delta = *ms ? *ms : 10000;
    if(!game::demoplayback) { conoutf("only while watching a demo"); return; }
    server::seekdemoms(server::demotime() + max(delta, 1));
});
ICOMMAND(installdemohd, "ss", (char *dmo, char *hdpath),
{
    if(!dmo[0] || !hdpath[0]) { conoutf("usage: installdemohd <demo.dmo> <file.dmohd>"); return; }
    stream *f = openrawfile(hdpath, "rb");
    if(!f) { conoutf(CON_ERROR, "could not read \"%s\"", hdpath); return; }
    stream::offset sz = f->size();
    if(sz <= 0 || sz > DEMOHD_MAXBYTES) { delete f; conoutf(CON_ERROR, "bad HD file"); return; }
    uchar *buf = new uchar[(int)sz];
    f->read(buf, (size_t)sz);
    delete f;
    demohd::installsidecar(dmo, buf, (int)sz);
    delete[] buf;
});

