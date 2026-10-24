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
// enemy visibility from your eyes in the HD track (off: the file is unchanged)
VARP(demohdvis, 0, 0, 1);
// matches kept by the clean-up, typed in the menu (a 3 digit field): an empty
// or wrong entry reads as 0 and the previous number stays, above 200 reads as
// 200 (no range message). Nothing is deleted here, so neither at start nor
// while typing: the clean-up runs when a recorded match ends (finish()).
static int demokeepprev = 5;
VARFP(demokeep, 0, 5, 999, { if(demokeep < 1) demokeep = demokeepprev; else demokeepprev = demokeep = min(demokeep, 200); });

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
    static bool dbdrag = false;
    static int dbhidden = 0; // 1 hidden by its button (SHOW stays), 2 by the key (nothing left)
    static float dbdragk = 0;
    static float dbbox[13][4]; // DB_N
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

    // Enemy visibility in the HD track (demohdvis 1), from game::reactframe.
    // 32-byte records among the aim samples, marked by cn HD_EXT_CN, which no
    // player has: older clients load them as the aim track of a player who
    // never exists and play the demo as before. When demohdvis is 0 nothing
    // is written and the file is exactly the usual one.
    //   t, HD_EXT_CN, type << HD_ZOOM_SHIFT, a, b, then 6 floats:
    //   HD_EXT_VIEW (first record, again if it changes): a = recorder cn,
    //     b = HD_EXT_VERSION; fov, zoom fov, vertical fov (no zoom), window
    //     width, height, smallest visible size that counts as seen (px)
    //   HD_EXT_VIS (only when the state of an enemy changes): a = his cn,
    //     b = 1 seen | 2 in the view | 4 line of sight | 8 under the
    //     crosshair | visible points (0-7) << 4; visible size (px, square
    //     root of the visible area), whole body height (px), angle from the
    //     crosshair (deg), distance, screen
    //     x, y of his chest (-1..1 on screen, 2 = behind)
    static uchar visbits[256];
    static int visseen[256], visframe = 0;
    static bool visviewdone = false;
    static float visviewlast[6];

    bool visrecording() { return demohdvis && active && hd && !game::demoplayback; }

    static void visreset()
    {
        memset(visbits, 0, sizeof(visbits));
        memset(visseen, 0, sizeof(visseen));
        visframe = 0;
        visviewdone = false;
    }

    static void writeext(int type, int a, int b, const float *v)
    {
        hdsample s;
        s.t = clockms();
        s.cn = HD_EXT_CN;
        s.flags = uchar(type << HD_ZOOM_SHIFT);
        s.gun = uchar(a);
        s.state = uchar(b);
        s.pad = 0;
        s.yaw = v[0];
        s.pitch = v[1];
        s.roll = v[2];
        s.o = vec(v[3], v[4], v[5]);
        writehdsample(s);
    }

    void visview(float fov, float zoomfov, float vfov, int w, int h, int minpx)
    {
        if(!visrecording()) return;
        float v[6] = { fov, zoomfov, vfov, float(w), float(h), float(minpx) };
        if(visviewdone && !memcmp(v, visviewlast, sizeof(v))) return;
        memcpy(visviewlast, v, sizeof(v));
        visviewdone = true;
        writeext(HD_EXT_VIEW, rec_cn, HD_EXT_VERSION, v);
    }

    void visbegin() { visframe++; }

    void notevis(int cn, int bits, float vispx, float bodypx, float xhang, float dist, float sx, float sy)
    {
        if(!visrecording() || cn < 0 || cn > 255) return;
        visseen[cn] = visframe;
        if((visbits[cn]&15) == (bits&15)) return;
        visbits[cn] = uchar(bits);
        float v[6] = { vispx, bodypx, xhang, dist, sx, sy };
        writeext(HD_EXT_VIS, cn, bits, v);
    }

    // enemies not looked at this frame (dead, gone, now on your team) are no longer seen
    void visend()
    {
        if(!visrecording()) return;
        loopi(256) if(visbits[i]&15 && visseen[i] != visframe)
        {
            visbits[i] = 0;
            float v[6] = { 0, 0, 0, 0, 2, 2 };
            writeext(HD_EXT_VIS, i, 0, v);
        }
    }

    void visnone()
    {
        if(!visrecording()) return;
        visbegin();
        visend();
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
        visreset();
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

    // Automatic clean-up (demokeep). Only this recorder's matches count and
    // go: stems named YYYY-MM-DD_HH-MM-SS_<map> by buildnames(), in the game's
    // own demo folder. A match is a whole set: demo/<stem>.dmo, its .dmohd and
    // the local original moved to demo/replaced when the server's copy took
    // its place. The demokeep newest matches in demo/ stay; every set older
    // than the oldest kept one goes, oldest first. Demos received with getdemo,
    // copied in or renamed by the player are never counted nor removed.
    // Runs only when a recorded match has just been saved (finish(): next map,
    // disconnect, quit, or the server's copy arriving), never at start.
    static bool ownstem(const char *s)
    {
        static const char pat[] = "dddd-dd-dd_dd-dd-dd_";
        loopi(int(sizeof(pat))-1)
        {
            uchar c = (uchar)s[i];
            if(pat[i]=='d' ? !isdigit(c) : c != (uchar)pat[i]) return false;
        }
        const char *m = s + sizeof(pat)-1;
        if(!*m) return false;
        for(; *m; m++) if(!isalnum((uchar)*m) && *m!='-' && *m!='_') return false;
        return true;
    }

    // the file in the game's own folder (never a package or install folder)
    static void ownpath(char (&dst)[MAXSTRLEN], const char *dir, const char *stem, const char *ext)
    {
        defformatstring(rel, "%s/%s.%s", dir, stem, ext);
        if(homedir[0]) formatstring(dst, "%s%s", homedir, rel);
        else copystring(dst, rel, MAXSTRLEN);
        path(dst);
    }

    static bool ownexists(const char *dir, const char *stem, const char *ext)
    {
        string p;
        ownpath(p, dir, stem, ext);
        struct stat st;
        return stat(p, &st)==0;
    }

    static bool stemnewer(const char *a, const char *b) { return strcmp(a, b) > 0; }

    void prune()
    {
        static const char * const dirs[2] = { "demo", "demo/replaced" };
        static const char * const exts[2] = { "dmo", "dmohd" };
        vector<char *> stems;
        loopj(2) loopk(2)
        {
            vector<char *> names;
            listfiles(dirs[j], exts[k], names);
            loopv(names)
            {
                char *n = names[i];
                bool skip = !ownstem(n) || (active && dmostem[0] && !strcmp(n, dmostem)) || !ownexists(dirs[j], n, exts[k]);
                for(int s = 0; s < stems.length() && !skip; s++) if(!strcmp(stems[s], n)) skip = true;
                if(skip) delete[] n;
                else stems.add(n);
            }
        }
        stems.sort(stemnewer);
        int kept = 0, cut = stems.length();
        loopv(stems) if(ownexists("demo", stems[i], "dmo") && ++kept >= demokeep) { cut = i+1; break; }
        for(int i = stems.length()-1; i >= cut; i--) loopj(2) loopk(2)
        {
            string p;
            ownpath(p, dirs[j], stems[i], exts[k]);
            remove(p);
        }
        stems.deletearrays();
        scanfiles(true);
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
        int nvis = 0;
        while(readsample(f, s))
        {
            if(s.cn == HD_EXT_CN) { nvis++; continue; } // visibility records, not aim
            track.add(s);
        }
        if(nvis) logoutf("HD visibility records: %d", nvis);
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
        if(game::demoplayback && a >= 0 && b >= 0 && src[b].t - src[a].t > 250 && t > src[a].t && t < src[b].t) return false; // samples lost here: the demo's network position, not a slide across the hole
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
        povclose();
    }

    void onplayback(const char *dmofilepath)
    {
        dmolenms = scandmolen(dmofilepath);
        dbdrag = false;
        dbhidden = 0;
        loadtrack(dmofilepath);
        if(track.length()) dmolenms = max(dmolenms, track.last().t);
        play_idx = 0;
        autofollowdone = false;
        game::demoxrayreset(); // a demo opens: X-ray at its start value; going back in it (onplaybackreset) keeps it
        povopen(dmofilepath); // its network positions, for the players' views (demopov)
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
        dbhidden = 0;
        autofollowdone = false;
        povclose();
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

    // a player's view (demopov) and the VIEW line of the demo controls, end of the file
    static fpsent *povviewer(fpsent *target);
    static bool povplace(fpsent *d, fpsent *viewer, vec &o, float &yaw, float &pitch, float &roll);
    static void viewlabel(char *buf, int len);
    static const char *viewtip();
    static void viewclick();
    static bool viewhole(char *buf, int len);
    static void dbviewtop(int w, int h, float s);
    static void gotoprompt();
    bool timeediting();
    static void timeupdate();
    static bool timekey(int code, bool isdown);
    static void dbtimefield(const char *tnow, const char *tdur, float right, float y, float th, float s, int w, int h, float cx, float cy);

    void applyothers(fpsent *skip)
    {
        if(!game::demoplayback || !demohdplay) return;
        fpsent *pv = povviewer(skip);
        if(!play_loaded && !pv) return; // no HD aim and no player's view (estimated): the demo's own positions
        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(!d || d==skip || d==game::player1) continue;
            if(d->state==CS_SPECTATOR) continue;
            if(d->state==CS_DEAD && d->ragdoll) continue;
            vec po;
            float pyaw, ppitch, proll;
            if(pv && d->state!=CS_DEAD && povplace(d, pv, po, pyaw, ppitch, proll))
            {
                d->yaw = pyaw;
                d->pitch = ppitch;
                d->roll = proll;
                d->o = po;
                d->smoothmillis = 0;
                d->resetinterp();
                continue;
            }
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
    // The local files move to demo/replaced while the server's take their
    // name, then are removed: the server's copy is the match from then on.
    // Anything unexpected keeps the local ones (a set left in demo/replaced
    // goes with its match in the demokeep clean-up).
    #define AUTODEMO_DIR "demo/replaced"
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
        if(ok)
        {
            remove(bdmo);
            if(hadhd) remove(bhd);
        }
        scanmillis = 0;
        return ok;
    }

    void stoprecording() { finish(true); }

    // Demo controls, drawn like the client menus (near black panels, thin orange
    // edge, white text and icons, orange for hover and for what is on, a line of
    // help over the control under the mouse). A compact block at the bottom
    // centre: state and exact time, the view shown (whose game: VIEW, click to
    // change it), timeline (click or drag), back / play-pause / ahead (10 s,
    // 0.5 s while Alt is held), speed, go to a typed time, X-ray, hitboxes; the players you can
    // follow at the bottom right (click a name to follow); HIDE / SHOW top right.
    // Ids: 0 block, 1 back, 2 play / pause, 3 ahead, 4 timeline, 5 speed,
    // 6 X-ray, 7 hide / show, 8 player list (rows in dbrows), 9 hitboxes,
    // 10 the view shown, 11 go to a time, 12 the time now (click: type one).
    enum { DB_N = 13, DB_ROWS = 24, DB_ORANGE = 0xFF7A18 };
    static float dbrowbox[DB_ROWS][4];
    static int dbrowcn[DB_ROWS], dbnrows = 0;

    static void dbset(int id, float x, float y, float w, float h)
    {
        if(id < 0 || id >= DB_N) return;
        dbbox[id][0] = x; dbbox[id][1] = y; dbbox[id][2] = w; dbbox[id][3] = h;
    }

    static bool dbin(const float *b, float px, float py)
    {
        return b[2] > 0 && px >= b[0] && py >= b[1] && px < b[0]+b[2] && py < b[1]+b[3];
    }

    static int dbhit(float px, float py)
    {
        // the controls first, then the panels behind them (8, 0)
        for(int i = 1; i < DB_N; i++) if(i != 8 && dbin(dbbox[i], px, py)) return i;
        if(dbin(dbbox[8], px, py)) return 8;
        if(dbin(dbbox[0], px, py)) return 0;
        return -1;
    }

    static int dbhitrow(float px, float py)
    {
        loopi(dbnrows) if(dbin(dbrowbox[i], px, py)) return i;
        return -1;
    }

    static void dbtime(int ms, char *dst, int len)
    {
        ms = max(ms, 0);
        nformatstring(dst, len, "%d:%02d", ms/60000, (ms/1000)%60);
    }

    static void dbtimems(int ms, char *dst, int len)
    {
        ms = max(ms, 0);
        nformatstring(dst, len, "%d:%02d.%03d", ms/60000, (ms/1000)%60, ms%1000);
    }

    static bool dbalt() { return (SDL_GetModState() & KMOD_ALT) != 0; }
    static int dbstep() { return dbalt() ? 500 : 10000; }

    static const int dbspeeds[4] = { 25, 50, 100, 200 };
    static void dbnextspeed(int dir)
    {
        int cur = 2;
        loopi(4) if(game::gamespeed >= dbspeeds[i]) cur = i;
        if(game::gamespeed != dbspeeds[cur]) cur = dir > 0 ? cur : min(cur+1, 3);
        game::changegamespeed(dbspeeds[(cur + (dir > 0 ? 1 : 3)) % 4]);
    }

    static void dbedge(float x, float y, float w, float h, float t, int alpha)
    {
        hudrect(x, y, w, t, DB_ORANGE, alpha);
        hudrect(x, y + h - t, w, t, DB_ORANGE, alpha);
        hudrect(x, y, t, h, DB_ORANGE, alpha);
        hudrect(x + w - t, y, t, h, DB_ORANGE, alpha);
    }

    static void dbpanel(float x, float y, float w, float h, float t)
    {
        hudrect(x, y, w, h, 0x080A0E, 175);
        dbedge(x, y, w, h, t, 150);
    }

    // text th pixels high, top left at x, y
    static float dbtextk(float th)
    {
        int tw = 0, fh = 0;
        text_bounds("0", tw, fh);
        return th / max(fh, 1);
    }

    static void dbtext(const char *s, float x, float y, float th, int color, int alpha = 255)
    {
        float k = dbtextk(th);
        pushhudmatrix();
        hudmatrix.translate(x, y, 0);
        hudmatrix.scale(k, k, 1);
        flushhudmatrix();
        draw_text(s, 0, 0, (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, alpha);
        pophudmatrix();
    }

    static float dbtextw(const char *s, float th)
    {
        int sw = 0, sh = 0;
        text_bounds(s, sw, sh);
        return sw * dbtextk(th);
    }

    static void dbicon(const char *path, float x, float y, float s, int color, int alpha = 255)
    {
        if(!settexture(path, 3)) return;
        gle::colorub((color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, alpha);
        gle::defvertex(2);
        gle::deftexcoord0();
        gle::begin(GL_TRIANGLE_STRIP);
        gle::attribf(x,   y);   gle::attribf(0, 0);
        gle::attribf(x+s, y);   gle::attribf(1, 0);
        gle::attribf(x,   y+s); gle::attribf(0, 1);
        gle::attribf(x+s, y+s); gle::attribf(1, 1);
        gle::end();
        gle::colorf(1, 1, 1, 1);
    }

    // one flat control: optional icon then optional text, centred; orange
    // underline and orange text when on, orange tint under the mouse
    static void dbdrawbtn(int id, int w, int h, int hover, const char *icon, const char *label, bool on, float th)
    {
        float bx = dbbox[id][0]*w, by = dbbox[id][1]*h, bw = dbbox[id][2]*w, bh = dbbox[id][3]*h;
        if(bw <= 0) return;
        bool hit = hover==id;
        hudrect(bx, by, bw, bh, on ? 0x2E1D0E : 0x1C2026, 205);
        if(hit) hudrect(bx, by, bw, bh, DB_ORANGE, 45);
        float ul = max(2.0f, bh*0.08f);
        if(on || hit) hudrect(bx, by + bh - ul, bw, ul, DB_ORANGE, on ? 235 : 150);
        int col = on || hit ? DB_ORANGE : 0xFFFFFF;
        float is = icon ? bh*0.5f : 0, gap = icon && label ? bh*0.14f : 0, lw = label ? dbtextw(label, th) : 0;
        float x = bx + (bw - (is + gap + lw))/2;
        if(icon)
        {
            defformatstring(path, "data/demobar_%s.png", icon);
            dbicon(path, x, by + (bh - is)/2, is, col);
        }
        if(label) dbtext(label, x + is + gap, by + (bh - th)/2, th, col);
    }

    // the line of help over (or under, for the top right button) a control
    static void dbtip(const char *tip, const float *b, int w, int h, float th, bool below)
    {
        if(!tip) return;
        float tw = dbtextw(tip, th), pad = th*0.5f;
        float bw = tw + 2*pad, bh = th + pad*1.2f;
        float cx = (b[0] + b[2]/2)*w;
        float x = clamp(cx - bw/2, 4.0f, w - bw - 4.0f), y = below ? (b[1] + b[3])*h + 6 : b[1]*h - bh - 6;
        hudrect(x, y, bw, bh, 0x080A0E, 235);
        dbedge(x, y, bw, bh, 1, 210);
        dbtext(tip, x + pad, y + (bh - th)/2, th, 0xFFFFFF);
    }

    // Spectating in a game (not a demo): the same player list, only what a
    // spectator already has (names, teams, who you follow; no position). While
    // the scoreboard key is held, a cursor appears and a click on a name follows
    // that player. Nothing while you play.
    VARP(spectatorlist, 0, 1, 1);

    bool specshown()
    {
        return spectatorlist && !game::demoplayback && game::connected && game::player1 && game::player1->state == CS_SPECTATOR;
    }

    bool speccursor()
    {
        return specshown() && game::showscoreboard;
    }

    // blue side: in a demo the recorder's team; spectating, the same side as the
    // scoreboard and the names over the heads (isteam with your own team)
    static bool dbally(fpsent *d)
    {
        if(game::demoplayback) return visteam(d) == 1;
        return m_check(game::gamemode, M_TEAM) && !strcmp(game::player1->team, d->team);
    }

    static int dbcolor(fpsent *d)
    {
        int col;
        if(game::demoplayback)
        {
            col = namecolor(d, 0xFFFFDD);
            if(d->state == CS_DEAD && !visteam(d)) col = (col>>1)&0x7F7F7F;
        }
        else
        {
            col = m_check(game::gamemode, M_TEAM) ? (dbally(d) ? 0x6496FF : 0xFF4B19) : 0xFFFFDD;
            if(d->state == CS_DEAD) col = (col>>1)&0x7F7F7F;
        }
        return game::friendnamecolor(d, col);
    }

    // players you can follow, bottom right, above the SPECTATOR label
    static void dbplayers(int w, int h, float s, float cx, float cy)
    {
        dbnrows = 0;
        dbbox[8][2] = 0;
        vector<fpsent *> list;
        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(d && d != game::player1 && d->state != CS_SPECTATOR && d->lifesequence >= 0) list.add(d);
        }
        if(list.empty()) return;
        // your side first, then the others, each in the order of the game
        vector<fpsent *> sorted;
        loopv(list) if(dbally(list[i])) sorted.add(list[i]);
        loopv(list) if(!dbally(list[i])) sorted.add(list[i]);
        float th = 21*s, rowh = 27*s, pad = 9*s, eye = 20*s;
        int maxrows = clamp(int((h*0.44f) / rowh), 4, int(DB_ROWS));
        int n = min(sorted.length(), maxrows);
        float namew = dbtextw("PLAYERS", th*0.85f);
        loopi(n) namew = max(namew, dbtextw(game::colorname(sorted[i]), th));
        const char *foot = game::demoplayback || speccursor() ? NULL : "Hold Tab to pick a player";
        if(foot) namew = max(namew, dbtextw(foot, th*0.8f) - eye - pad);
        float pw = namew + eye + 3*pad, ph = n*rowh + rowh + pad + (foot ? rowh : 0);
        float px = w - pw - 18*s, py = (game::demoplayback ? h*0.745f : h*0.82f) - ph;
        dbpanel(px, py, pw, ph, max(1.0f, s));
        dbset(8, px/w, py/h, pw/w, ph/h);
        dbtext("PLAYERS", px + pad, py + pad*0.6f, th*0.85f, 0x8A9199);
        fpsent *f = game::followingplayer();
        int hoverrow = dbhitrow(cx, cy);
        loopi(n)
        {
            fpsent *d = sorted[i];
            float ry = py + rowh + i*rowh;
            float rb[4] = { px/w, ry/h, pw/w, rowh/h };
            memcpy(dbrowbox[i], rb, sizeof(rb));
            dbrowcn[i] = d->clientnum;
            bool followed = d == f, hit = hoverrow == i;
            if(followed) hudrect(px + 1, ry, pw - 2, rowh, DB_ORANGE, 40);
            if(hit) hudrect(px + 1, ry, pw - 2, rowh, DB_ORANGE, 45);
            if(followed) dbicon("data/following.png", px + pad, ry + (rowh - eye)/2, eye, 0xFFFFFF);
            int col = dbcolor(d);
            dbtext(game::colorname(d), px + 2*pad + eye, ry + (rowh - th)/2, th, hit ? DB_ORANGE : col);
        }
        if(foot) dbtext(foot, px + pad, py + rowh + n*rowh + (rowh - th*0.8f)/2, th*0.8f, 0x8A9199);
        dbnrows = n;
    }

    void drawhud(int w, int h)
    {
        loopi(DB_N) dbbox[i][2] = 0;
        dbnrows = 0;
        timeupdate(); // the time typed in place in the demo controls
        if(!game::demoplayback || !w || !h) { return; } drawhitboxes(w, h); // under the controls, even hidden
        if(dbhidden == 2) return; // hidden by the key: nothing at all, for watching or filming

        float cx, cy;
        g3d_cursorpos(cx, cy);

        float s = h/1080.0f, th = 23*s, small = 16*s;
        float hidew = max(72.0f, h*0.08f), hideh = max(28.0f, h*0.035f);
        dbset(7, (w - hidew - 12)/w, 12.0f/h, hidew/w, hideh/h);
        int hover = dbhit(cx, cy);
        if(dbhidden)
        {
            dbdrawbtn(7, w, h, hover, "show", "SHOW", false, th*0.85f);
            if(hover==7) dbtip("Show the demo controls", dbbox[7], w, h, small, true);
            if(hitboxactive()) dbviewtop(w, h, s); // hitboxes drawn: whose view they are in, even without the controls
            return;
        }

        int dur = max(dmolenms, 1);
        int now = clamp(server::demotime(), 0, dur);
        float k = dbdrag ? dbdragk : clamp(float(now)/float(dur), 0.0f, 1.0f);
        bool paused = game::ispaused(), alt = dbalt();

        // the block: state + time, the view shown, timeline, help line, buttons
        float pad = 12*s, btnh = 40*s, gap = 6*s, vh = th + 8*s;
        bool local = game::demoxrayallowed();
        float bw[7] = { 100*s, 120*s, 100*s, 96*s, local ? 110*s : 0, local ? 128*s : 0, local ? 140*s : 0 };
        float roww = 3*gap;
        loopi(7) if(bw[i] > 0) roww += bw[i] + (i >= 4 ? gap : 0);
        float blockw = max(roww, 580*s) + 2*pad;
        float blockh = pad + th + 8*s + vh + 6*s + 18*s + 4*s + small + 8*s + btnh + pad;
        float bx = (w - blockw)/2, by = h*0.915f - blockh;
        dbpanel(bx, by, blockw, blockh, max(1.0f, s));
        dbset(0, bx/w, by/h, blockw/w, blockh/h);

        float y = by + pad;
        const char *state = paused ? "PAUSED" : "PLAYING";
        dbtext(state, bx + pad, y, th, paused ? DB_ORANGE : 0xFFFFFF);
        if(game::gamespeed != 100)
        {
            defformatstring(sp, "%.2fx", game::gamespeed/100.0f);
            dbtext(sp, bx + pad + dbtextw(state, th) + 10*s, y, th, 0x8A9199);
        }
        string tnow, tdur;
        dbtimems(int(k*dur + 0.5f), tnow, sizeof(tnow));
        dbtimems(dur, tdur, sizeof(tdur));
        dbtimefield(tnow, tdur, bx + blockw - pad, y, th, s, w, h, cx, cy); // "now / length": click the time now to type one
        y += th + 8*s;

        // the view shown: whose game, or the real positions (click: change it)
        string vlabel;
        viewlabel(vlabel, sizeof(vlabel));
        float vth = th*0.92f, vw = min(dbtextw(vlabel, vth) + 3*pad, blockw - 2*pad), vx = bx + (blockw - vw)/2;
        dbset(10, vx/w, y/h, vw/w, vh/h);
        bool vhit = dbhit(cx, cy) == 10;
        hudrect(vx, y, vw, vh, 0x1C2026, 215);
        if(vhit) hudrect(vx, y, vw, vh, DB_ORANGE, 45);
        dbedge(vx, y, vw, vh, max(1.0f, s), vhit ? 220 : 120);
        dbtext(vlabel, vx + 1.5f*pad, y + (vh - vth)/2, vth, 0xFFFFFF);
        y += vh + 6*s;

        float tlx = bx + pad, tlw = blockw - 2*pad, tlh = 18*s;
        dbset(4, tlx/w, y/h, tlw/w, tlh/h);
        if(dbdrag && dbbox[4][2] > 0)
        {
            dbdragk = clamp((cx - dbbox[4][0]) / dbbox[4][2], 0.0f, 1.0f);
            k = dbdragk;
        }
        hover = dbhit(cx, cy);
        bool tlhit = hover==4 || dbdrag;
        float track = max(3.0f, 5*s), ty = y + (tlh - track)/2;
        hudrect(tlx, ty, tlw, track, 0x3A3F48, 230);
        hudrect(tlx, ty, tlw*k, track, tlhit ? DB_ORANGE : 0xFFFFFF, 240);
        float knob = 14*s;
        hudrect(tlx + tlw*k - knob/4, y + (tlh - knob)/2, knob/2, knob, tlhit ? DB_ORANGE : 0xFFFFFF, 255);
        y += tlh + 4*s;

        string hole;
        bool inhole = viewhole(hole, sizeof(hole)), typing = timeediting(); // the player whose view is shown lost their HD aim here
        const char *help = typing ? "TYPE A TIME (9:00.842)   ENTER: GO THERE   ESC: CANCEL" : inhole ? hole : (alt ? "ALT: STEPS OF 0.5 S" : "HOLD ALT FOR 0.5 S STEPS");
        dbtext(help, bx + (blockw - dbtextw(help, small))/2, y, small, typing || inhole || alt ? DB_ORANGE : 0x8A9199);
        y += small + 8*s;

        float x = bx + (blockw - roww)/2;
        int ids[7] = { 1, 2, 3, 5, 11, 6, 9 };
        loopi(7) if(bw[i] > 0) { dbset(ids[i], x/w, y/h, bw[i]/w, btnh/h); x += bw[i] + gap; }
        hover = dbhit(cx, cy);

        dbdrawbtn(1, w, h, hover, "back", alt ? "0.5s" : "10s", false, th*0.95f);
        dbdrawbtn(2, w, h, hover, paused ? "play" : "pause", paused ? "PLAY" : "PAUSE", false, th*0.95f);
        dbdrawbtn(3, w, h, hover, "forward", alt ? "0.5s" : "10s", false, th*0.95f);
        defformatstring(spd, "%.2fx", game::gamespeed/100.0f);
        dbdrawbtn(5, w, h, hover, NULL, spd, game::gamespeed != 100, th*0.95f);
        if(bw[4] > 0) dbdrawbtn(11, w, h, hover, NULL, "GO TO", false, th*0.95f);
        if(bw[5] > 0) dbdrawbtn(6, w, h, hover, "xray", "X-RAY", game::demoxrayactive(), th*0.95f);
        if(bw[6] > 0) dbdrawbtn(9, w, h, hover, "hitbox", "HITBOX", hitboxactive(), th*0.95f);
        dbdrawbtn(7, w, h, hover, "hide", "HIDE", false, th*0.85f);

        dbplayers(w, h, s, cx, cy);

        // help over the control under the mouse
        const char *tip = NULL;
        string tipbuf;
        int row = dbhitrow(cx, cy);
        switch(dbdrag ? -1 : hover)
        {
            case 1: tip = alt ? "Back 0.5 s" : "Back 10 s (Alt: 0.5 s)"; break;
            case 2: tip = paused ? "Play" : "Pause"; break;
            case 3: tip = alt ? "Ahead 0.5 s" : "Ahead 10 s (Alt: 0.5 s)"; break;
            case 5: tip = "Speed: click for faster, Alt+click for slower"; break;
            case 6: tip = game::demoxrayactive() ? "X-ray on: players seen through walls" : "X-ray: see players through walls"; break;
            case 7: tip = "Hide the demo controls"; break;
            case 9: tip = hitboxactive() ? "Hitboxes on: white touched, yellow missed, pink uncertain, red disagree" : "Hitboxes: the real shape a shot must touch"; break;
            case 10: tip = viewtip(); break;
            case 11: tip = "Go to a time you type, to the millisecond (paused there)"; break;
            case 12: if(!timeediting()) tip = "Click and type a time, to the millisecond (Enter: go there)"; break;
            case 4:
            {
                float hk = dbbox[4][2] > 0 ? clamp((cx - dbbox[4][0]) / dbbox[4][2], 0.0f, 1.0f) : 0;
                string t;
                dbtime(int(hk*dur + 0.5f), t, sizeof(t));
                formatstring(tipbuf, "Go to %s", t);
                tip = tipbuf;
                break;
            }
            case 8:
                if(row >= 0)
                {
                    fpsent *d = game::getclient(dbrowcn[row]);
                    if(d) { formatstring(tipbuf, "Follow %s", game::colorname(d)); tip = tipbuf; }
                    dbtip(tip, dbrowbox[row], w, h, small, false);
                    tip = NULL;
                }
                break;
        }
        if(tip) dbtip(tip, dbbox[hover], w, h, small, hover==7);
    }

    // player list while spectating in a game (see specshown)
    void drawspec(int w, int h)
    {
        dbnrows = 0;
        if(!specshown() || !w || !h) return;
        float cx = 0, cy = 0;
        bool cur = speccursor();
        if(cur) g3d_cursorpos(cx, cy);
        else cx = cy = -1;
        float s = h/1080.0f;
        dbplayers(w, h, s, cx, cy);
        int row = cur ? dbhitrow(cx, cy) : -1;
        if(row >= 0)
        {
            fpsent *d = game::getclient(dbrowcn[row]);
            if(d)
            {
                defformatstring(tip, "Follow %s", game::colorname(d));
                dbtip(tip, dbrowbox[row], w, h, 16*s, false);
            }
        }
    }

    static bool speckey(int code, bool isdown)
    {
        static bool pressed = false;
        if(code != -1) return false;
        if(!isdown) { bool was = pressed; pressed = false; return was; }
        if(!speccursor()) return false;
        float cx, cy;
        g3d_cursorpos(cx, cy);
        int row = dbhitrow(cx, cy);
        if(row < 0) return false; // elsewhere: the click is for the game (next player)
        pressed = true;
        defformatstring(cmd, "follow %d", dbrowcn[row]);
        execute(cmd);
        return true;
    }

    void togglebar()
    {
        dbhidden = dbhidden ? 0 : 2;
        dbdrag = false;
    }

    bool barkey(int code, bool isdown)
    {
        if(!game::demoplayback) return speckey(code, isdown);
        if(timekey(code, isdown)) return true; // the time typed in place: keys that would leave it
        if(lookkey(code, isdown)) return true; // held: the mouse turns the view (demolookkey)
        if(code != -1) return false;
        float cx, cy;
        g3d_cursorpos(cx, cy);
        int hit = dbhit(cx, cy), row = hit==8 ? dbhitrow(cx, cy) : -1;
        // a click away from the controls goes to the game (left click = next
        // player to watch, like the right click); its release too
        static bool barpressed = false;
        if(isdown && (hit < 0 || (dbhidden && hit != 7))) { barpressed = false; return false; }
        if(!isdown && !barpressed && !dbdrag) return false;
        barpressed = isdown;
        if(isdown)
        {
            if(hit==7)
            {
                dbhidden = dbhidden ? 0 : 1;
                dbdrag = false;
            }
            else if(dbhidden) {}
            else if(hit==1) server::seekdemoms(max(server::demotime() - dbstep(), 0));
            else if(hit==2) server::forcepaused(!server::ispaused());
            else if(hit==3) server::seekdemoms(server::demotime() + dbstep());
            else if(hit==4)
            {
                dbdrag = true;
                if(dbbox[4][2] > 0) dbdragk = clamp((cx - dbbox[4][0]) / dbbox[4][2], 0.0f, 1.0f);
            }
            else if(hit==5) dbnextspeed(dbalt() ? -1 : 1);
            else if(hit==6) game::toggledemoxray();
            else if(hit==9) togglehitbox();
            else if(hit==10) viewclick();
            else if(hit==11 || hit==12) gotoprompt();
            else if(hit==8 && row >= 0)
            {
                defformatstring(cmd, "follow %d", dbrowcn[row]);
                execute(cmd);
            }
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

ICOMMAND(toggledemobar, "", (),
{
    if(!game::demoplayback) { conoutf("only while watching a demo"); return; }
    demohd::togglebar();
});

void game::demohdsample() { game::reactframe(); demohd::captureframe(); }
void game::demohdapplyzoom() { demohd::applyzoom(); }
void game::demohdafterpacket(int chan, const uchar *data, int len) { if(!game::specpacketqueued()) demohd::afterpacket(chan, data, len); }
void game::demohdwritepos(const uchar *data, int len) { demohd::writepos(data, len); }

extern int hidehud;
bool game::demobarcursor() { return ((demoplayback && !demohd::looking()) || demohd::speccursor()) && !mainmenu && !hidehud; }

// Demo hitboxes (demohitbox, Off by default; HITBOX button of the demo
// controls, key toggledemohitbox): the shape the game really tests when a shot
// is fired (weapon.cpp intersect: an upright cylinder of the player's radius,
// from eyeheight under the eyes to aboveeye over them, at the player's
// position), a thin wireframe around every player still in the game, in the
// colour of their X-ray outline. Only while a demo played on this computer is
// watched (the X-ray check, every frame): never in a game, while spectating,
// in edit mode or in a demo played by a server.
// With demohitboxshots (On) each shot shows for 1.5 s of demo time: its line
// from the shooter's eyes (where the view shows them) to where it stopped, a
// cross there with their name, and the hitbox of each player near the line at
// that moment: white when the line crosses it, yellow when it passes beside it
// (within 12 units). In a player's view (demopov, or the view chosen for the
// free camera; in a local demo the view of its recorder too) only that
// player's own shots get hitboxes, checked against what their game decided
// (the hits and frags the server replayed with the shot): white = touched in
// their game and crossed here, yellow = missed in their game and beside here;
// when the two disagree, PINK marked "uncertain" while the line passes within
// demohitboxuncertain units of the edge of the hitbox (the rebuilt view is not
// more exact than that; in an estimated view, also what the target moves in
// 15 ms), RED beyond, with what their game counted and how far the line is;
// the victims of the others' shots are light blue (demohitboxothercolour:
// decided by the others' own game). The shot that killed and
// the hitbox of its victim stay demohitboxdeath ms where the victim was hit
// (the body falls, the hitbox does not). After a jump in the demo (seek,
// -10 s, go to a time) the shots and frags of the last moments before the new
// time are shown.
VARP(demohitbox, 0, 0, 1);
VARP(demohitboxshots, 0, 1, 1);
VARP(demohitboxdeath, 0, 1500, 5000);
FVARP(demohitboxuncertain, 0, 0.5f, 4);
HVARP(demohitboxothercolour, 0, 0x80C8FF, 0xFFFFFF);

// A player's view (demopov, On): in a server demo that has their exact aim,
// while you follow a player the others are shown where THEIR game showed them,
// rebuilt the way their game did it: from the positions the server sent them
// (the demo's N_POS, about 30 a second), their ping (N_CLIENTPING) plus how
// late the demo dates their own eyes (povcalage), the smoothing of client.cpp
// parsepositions (smoothmove, smoothdist), the push aside of updatepos and the
// prediction of fps.cpp otherplayers (moveplayer from the last position
// received, 5 ms steps). The followed player stays at their real place. The
// reaction times of the kill feed are measured on what the killer's game
// showed. A player with no exact aim (a client without it in a server demo,
// anybody but the recorder of a local demo) gets an ESTIMATED view, said so:
// the same rebuilding from their ping and the positions the demo has, their
// shots placed with their own clock of them (N_SHOTFX id). The recorder of a
// local demo already has their own view: the demo itself. 0 = the demo's
// positions in every view that follows a player, and the kill feed on them.
VARP(demopov, 0, 1, 1);

// The free camera of a demo: whose game it shows (a client number), -1 the
// real positions (a server demo) or the recorder's game (a local demo). Not
// saved; -1 when a demo opens. The VIEW control of the demo controls (or
// demonextview) changes it.
VAR(demofreeview, -1, -1, 255);

// Hold this key in a demo (MOUSE3, the middle button, by default; several key
// names can be given) to look around with the mouse; released, the cursor of
// the demo controls comes back where it was.
SVARP(demolookkey, "MOUSE3");

extern const char *getkeyname(int code);
extern int commandmillis, commandpos; // console.cpp: the command line, for the time typed in place (demo controls)
extern string commandbuf;
namespace game { extern int smoothmove, smoothdist; }

namespace demohd
{
    #define HB_SHOTMS 1500
    #define HB_MAXSHOTS 16
    #define HB_MAXHIT 4
    #define HB_MAXCAND 16
    #define HB_SIDES 24
    #define HB_NEAR 12
    #define POV_STEP 5
    #define POV_WARM 8
    #define POV_LAG 1000
    #define POV_MAXDELAY 400
    #define POV_PAIRS 32 // (viewer, player) ghosts kept: the kill feed of a view uses two per player

    // ---- a player's view (demopov) ----

    // a network position of the demo (N_POS), kept as the game reads it
    struct povnet
    {
        int t, o[3];                            // demo time; feet, in 1/DMF units
        ushort dir, mag, veldir, fmag, falldir;
        uchar roll, physstate, fflags;          // fflags 1: falling sent, 2: with its direction
    };
    static vector<povnet> *povnets[256];
    static bool povloaded = false, povtried = false;
    static string povpath; // the .dmo watched, while its positions are not read yet

    // one player (target) as a viewer's game showed them: a body moved by the
    // game's physics (CS_LAGGED: no player collision, never in the dynent cache)
    struct povghost
    {
        fpsent *e;
        int idx, physt, smoothms, viewer, target, delay;
        bool valid, smooth;
        vec deltapos;
        float deltayaw, deltapitch, deltaroll;
    };
    static povghost povghosts[256], povscratch, povpairs[POV_PAIRS];
    static int povpairnext = 0;
    static int povextra[256]; // ms over the ping: how late the demo dates the player's own HD track (povcalage)
    bool povmoving = false; // fps.cpp physicstrigger: a rebuilt body makes no sound

    // the clock of each player's shots (N_SHOTFX id) on the demo's time: the
    // smallest gap (demo time of the shot - id) on their last shots, when they
    // sent it at once (their send wait, 0 to 33 ms, is about 0 then); and the
    // ping of the recorder of a local demo (not in the file): the smallest wait
    // between their own shot and the hit or frag the server sent back
    #define POV_CLOCKS 9
    static int povclk[256][POV_CLOCKS], povclkn[256], povrecping = 0;
    // going back in a demo restarts it: the player followed, kept (seekkeep)
    static int seekcn = -1;
    static string seekname;

    static bool lookon = false, lookback = false;
    static int lookx = 0, looky = 0, lookbackms = 0;

    // client.cpp parsepositions, the same reading of the bytes
    static int povparse(int t, uchar *data, int len)
    {
        ucharbuf p(data, len);
        int n = 0;
        while(p.remaining())
        {
            int type = getint(p);
            if(type == N_TELEPORT) { getint(p); getint(p); getint(p); continue; }
            if(type == N_JUMPPAD) { getint(p); getint(p); continue; }
            if(type != N_POS) break;
            int cn = getuint(p), physstate = p.get(), flags = getuint(p);
            povnet q;
            q.t = t;
            loopk(3)
            {
                int v = p.get(); v |= p.get()<<8; if(flags&(1<<k)) { v |= p.get()<<16; if(v&0x800000) v |= ~0U<<24; }
                q.o[k] = v;
            }
            int dir = p.get(); dir |= p.get()<<8;
            q.dir = ushort(dir);
            q.roll = uchar(p.get());
            int mag = p.get(); if(flags&(1<<3)) mag |= p.get()<<8;
            q.mag = ushort(mag);
            dir = p.get(); dir |= p.get()<<8;
            q.veldir = ushort(dir);
            q.fflags = 0;
            q.fmag = q.falldir = 0;
            if(flags&(1<<4))
            {
                mag = p.get(); if(flags&(1<<5)) mag |= p.get()<<8;
                q.fmag = ushort(mag);
                q.fflags |= 1;
                if(flags&(1<<6)) { dir = p.get(); dir |= p.get()<<8; q.falldir = ushort(dir); q.fflags |= 2; }
            }
            q.physstate = uchar(physstate);
            if(p.overread()) break;
            if(cn < 0 || cn > 255) continue;
            if(!povnets[cn]) povnets[cn] = new vector<povnet>;
            vector<povnet> &l = *povnets[cn];
            if(l.length() && l.last().t > t) continue;
            l.add(q);
            n++;
        }
        return n;
    }

    static void povclearghost(povghost &g)
    {
        DELETEP(g.e);
        g.valid = g.smooth = false;
        g.idx = -1;
    }

    void povclose()
    {
        loopi(256)
        {
            DELETEP(povnets[i]);
            povclearghost(povghosts[i]);
        }
        povclearghost(povscratch);
        loopi(POV_PAIRS) povclearghost(povpairs[i]);
        memset(povextra, 0, sizeof(povextra));
        memset(povclkn, 0, sizeof(povclkn));
        povrecping = 0;
        seekcn = -1;
        povloaded = povtried = false;
        povpath[0] = '\0';
        demofreeview = -1;
        lookon = lookback = false;
    }

    // How late a player's own HD track is dated, measured in the demo itself:
    // while they move, each network position of theirs (relayed by the server
    // at demo time w) is found on their HD track at time u. Dated "their clock +
    // smallest delay", w - u would only be the wait for the server's next
    // update (0 to 33 ms, 16 on average): the median shortfall is how much
    // later their track is dated, so their game's view lags their HD eyes by
    // their ping plus that much.
    #define POV_RELAYWAIT 16
    static void povcalage(int cn)
    {
        povextra[cn] = 0;
        if(!povnets[cn] || !playexact[cn] || track.empty()) return;
        vector<povnet> &l = *povnets[cn];
        vector<int> lags;
        for(int i = 0; i < l.length(); i += 4)
        {
            const povnet &q = l[i];
            if(q.mag < 60) continue; // they must move, for a position to mean one moment
            vec o(q.o[0]/DMF, q.o[1]/DMF, q.o[2]/DMF + 14);
            int lo = 0, hi = track.length();
            while(lo < hi) { int mid = (lo + hi)/2; if(track[mid].t < q.t - 150) lo = mid + 1; else hi = mid; }
            const hdsample *prev = NULL;
            float bestd = 1e9f, bestt = 0;
            for(int j = lo; j < track.length() && track[j].t <= q.t + 50; j++)
            {
                const hdsample &b = track[j];
                if(b.cn != cn) continue;
                if(prev && b.t > prev->t && b.t - prev->t <= 50)
                {
                    vec ab = vec(b.o).sub(prev->o);
                    float n2 = ab.squaredlen(), k = n2 > 1e-6f ? clamp(vec(o).sub(prev->o).dot(ab)/n2, 0.0f, 1.0f) : 0;
                    float dist = vec(ab).mul(k).add(prev->o).dist(o);
                    if(dist < bestd) { bestd = dist; bestt = prev->t + k*(b.t - prev->t); }
                }
                prev = &b;
            }
            if(bestd < 1.0f) lags.add(q.t - int(bestt + 0.5f));
        }
        if(lags.length() < 50) return;
        lags.sort();
        povextra[cn] = clamp(POV_RELAYWAIT - lags[lags.length()/2], 0, 40);
    }

    // a local demo: its recorder's own rifle shots (written as they fired) and
    // the hits and frags the server sent back for them, a ping later
    static void povrecshot(int t, uchar *data, int len, int &lastshot, vector<int> &waits)
    {
        ucharbuf p(data, len);
        int type = getint(p);
        if(type == N_SHOTFX)
        {
            int cn = getint(p), gun = getint(p);
            if(!p.overread() && cn == play_cn) lastshot = gun == GUN_RIFLE ? t : -1;
        }
        else if(type == N_DAMAGE || type == N_DIED)
        {
            int target = getint(p), actor = getint(p);
            if(p.overread() || actor != play_cn || target == play_cn || lastshot < 0) return;
            int w = t - lastshot;
            if(w >= 0 && w <= POV_MAXDELAY) waits.add(w);
        }
    }

    // the network positions of the demo, read once, when a player's view is
    // first needed (demopov On: as the demo opens)
    static void povload()
    {
        povtried = true;
        stream *f = opengzfile(povpath, "rb");
        if(!f) return;
        demoheader hdr;
        int total = 0, lastshot = -1;
        vector<int> waits;
        if(f->read(&hdr, sizeof(hdr)) == sizeof(hdr))
        {
            vector<uchar> buf;
            int stamp[3];
            while(f->read(stamp, sizeof(stamp)) == sizeof(stamp))
            {
                lilswap(stamp, 3);
                int len = stamp[2];
                if(len < 0 || len > (1<<20)) break;
                buf.setsize(0);
                databuf<uchar> b = buf.reserve(len+1);
                if(len && f->read(b.buf, len) != size_t(len)) break;
                if(stamp[1] == 0) total += povparse(stamp[0], b.buf, len);
                else if(stamp[1] == 1 && play_cn >= 0 && len) povrecshot(stamp[0], b.buf, len, lastshot, waits);
            }
        }
        delete f;
        povloaded = total > 0;
        if(povloaded && play_cn < 0) loopi(256) povcalage(i); // a server demo: how late each exact aim is dated
        if(waits.length()) { waits.sort(); povrecping = waits[waits.length()/10]; } // the 10th percentile: their send wait near 0
    }

    // a demo opens; with demopov Off nothing is read until a player's view is
    // asked for
    void povopen(const char *dmopath)
    {
        povclose();
        if(!dmopath || !dmopath[0]) return;
        copystring(povpath, dmopath);
        if(demopov) povload();
    }

    static bool povready()
    {
        if(povloaded) return true;
        if(povtried || !povpath[0]) return false;
        povload(); // asked for during the demo
        return povloaded;
    }

    static bool povsamelife(const povnet &a, const povnet &b)
    {
        return !((a.physstate^b.physstate)&8) && b.t - a.t <= POV_LAG;
    }

    // what client.cpp parsepositions does with a position it receives
    static void povset(fpsent *e, const povnet &q)
    {
        vec vel, falling;
        loopk(3) e->o[k] = q.o[k]/DMF;
        e->o.z += e->eyeheight;
        e->yaw = q.dir%360;
        e->pitch = clamp(q.dir/360, 0, 180)-90;
        e->roll = clamp(int(q.roll), 0, 180)-90;
        vecfromyawpitch(q.veldir%360, clamp(q.veldir/360, 0, 180)-90, 1, 0, vel);
        vel.mul(q.mag/DVELF);
        if(q.fflags&1)
        {
            if(q.fflags&2) vecfromyawpitch(q.falldir%360, clamp(q.falldir/360, 0, 180)-90, 1, 0, falling);
            else falling = vec(0, 0, -1);
            falling.mul(q.fmag/DVELF);
        }
        else falling = vec(0, 0, 0);
        e->move = (q.physstate>>4)&2 ? -1 : (q.physstate>>4)&1;
        e->strafe = (q.physstate>>6)&2 ? -1 : (q.physstate>>6)&1;
        e->vel = vel;
        e->falling = falling;
        e->physstate = q.physstate&7;
        updatephysstate(e);
    }

    // client.cpp updatepos: a player overlapping you is pushed aside; "you" is
    // the viewer, at their real place (their HD track) when it happened
    static void povpush(fpsent *e, int viewer, int t)
    {
        hdsample s;
        int idx = play_idx;
        if(!play_loaded || viewer < 0 || !lerpsample(track, idx, t, viewer, s) || s.state != CS_ALIVE) return;
        const float r = e->radius*2, rz = e->aboveeye + e->eyeheight;
        const float dx = s.o.x - e->o.x, dy = s.o.y - e->o.y, dz = s.o.z - e->o.z;
        const float fx = fabs(dx), fy = fabs(dy), fz = fabs(dz);
        if(fx < r && fy < r && fz < rz)
        {
            if(fx < fy) e->o.y += dy < 0 ? r-fy : -(r-fy);
            else        e->o.x += dx < 0 ? r-fx : -(r-fx);
        }
    }

    // fps.cpp otherplayers: moveplayer from the last position received, in
    // the 5 ms steps of the game's physics, up to demo time t
    static void povstep(povghost &g, int t)
    {
        int n = t/POV_STEP - g.physt/POV_STEP;
        if(n <= 0) return;
        g.physt = (t/POV_STEP)*POV_STEP;
        n = min(n, POV_LAG/POV_STEP);
        povmoving = true;
        loopi(n) moveplayer(g.e, 1, false, POV_STEP);
        povmoving = false;
    }

    // fps.cpp predictplayer: what is drawn, the rest of the smoothing included
    static void povshown(const povghost &g, int t, vec &o, float &yaw, float &pitch, float &roll)
    {
        o = g.e->o;
        yaw = g.e->yaw;
        pitch = g.e->pitch;
        roll = g.e->roll;
        if(!g.smooth || game::smoothmove <= 0) return;
        float k = 1.0f - float(t - g.smoothms)/game::smoothmove;
        if(k <= 0) return;
        o.add(vec(g.deltapos).mul(k));
        yaw += g.deltayaw*k;
        if(yaw < 0) yaw += 360;
        else if(yaw >= 360) yaw -= 360;
        pitch += g.deltapitch*k;
        roll += g.deltaroll*k;
    }

    // player cn at time t (the time base of the demo's positions) as the
    // viewer's game showed them; false when that game had nothing to show
    static bool povat(povghost &g, int cn, int t, int viewer, int delay, vec &o, float &yaw, float &pitch, float &roll)
    {
        if(cn < 0 || cn > 255 || !povnets[cn]) return false;
        vector<povnet> &l = *povnets[cn];
        if(l.empty() || t < l[0].t) return false;
        int lo = 0, hi = l.length()-1;
        while(lo < hi)
        {
            int mid = (lo + hi + 1)/2;
            if(l[mid].t <= t) lo = mid;
            else hi = mid - 1;
        }
        int k = lo;
        if(t - l[k].t > POV_LAG) return false;
        if(!g.e) { g.e = new fpsent; g.e->state = CS_LAGGED; }
        // rebuilt from POV_WARM positions back when the time jumps, goes back more than a physics step (a ping that
        // grows by a few ms does not) or the viewer or the player changes
        if(!g.valid || g.viewer != viewer || g.target != cn || k < g.idx || t < g.physt - POV_STEP || k - g.idx > POV_WARM)
        {
            int k0 = k;
            while(k0 > 0 && k - k0 < POV_WARM && povsamelife(l[k0-1], l[k0])) k0--;
            g.e->reset();
            povset(g.e, l[k0]);
            int mat = lookupmaterial(vec(g.e->o.x, g.e->o.y, g.e->o.z + (3*g.e->aboveeye - g.e->eyeheight)/4)); // as moveplayer
            g.e->inwater = isliquid(mat&MATF_VOLUME) ? mat&MATF_VOLUME : MAT_AIR;
            povpush(g.e, viewer, l[k0].t + delay);
            g.idx = k0;
            g.physt = l[k0].t;
            g.smooth = false;
            g.valid = true;
            g.viewer = viewer;
            g.target = cn;
            g.delay = delay;
        }
        while(g.idx < k)
        {
            const povnet &q = l[g.idx+1];
            povstep(g, q.t);
            vec oldpos;
            float oldyaw, oldpitch, oldroll;
            povshown(g, q.t, oldpos, oldyaw, oldpitch, oldroll);
            bool same = povsamelife(l[g.idx], q);
            povset(g.e, q);
            povpush(g.e, viewer, q.t + delay);
            if(same && game::smoothmove && oldpos.dist(g.e->o) < game::smoothdist)
            {
                g.deltapos = vec(oldpos).sub(g.e->o);
                g.deltayaw = oldyaw - g.e->yaw;
                if(g.deltayaw > 180) g.deltayaw -= 360;
                else if(g.deltayaw < -180) g.deltayaw += 360;
                g.deltapitch = oldpitch - g.e->pitch;
                g.deltaroll = oldroll - g.e->roll;
                g.smoothms = q.t;
                g.smooth = true;
            }
            else g.smooth = false;
            g.idx++;
        }
        povstep(g, t);
        povshown(g, t, o, yaw, pitch, roll);
        return true;
    }

    static bool povexact(fpsent *d)
    {
        return d && d != game::player1 && d->clientnum >= 0 && d->clientnum <= 255 && playexact[d->clientnum] && hastrack(d->clientnum);
    }

    // whose game can be shown: 1 exact (their aim in a server demo), 2
    // estimated (no exact aim: their network positions and ping), 0 none (the
    // recorder of a local demo: the demo itself is their view; a bot: its
    // owner's game; nothing in the file)
    static int povkindof(fpsent *v, bool known)
    {
        if(!v || v == game::player1 || v->aitype != AI_NONE || v->clientnum < 0 || v->clientnum > 255 || v->state == CS_SPECTATOR) return 0;
        if(play_cn >= 0 && v->clientnum == play_cn) return 0;
        if(play_cn < 0 && povexact(v)) return 1;
        if(!povloaded) return known || povtried || !povpath[0] ? 0 : 2; // the demo's positions not read yet: it may be
        return povnets[v->clientnum] ? 2 : 0;
    }
    static int povkind(fpsent *v) { return povkindof(v, true); }

    // how far in the past the viewer's game showed the others: their ping (the
    // demo's N_CLIENTPING) plus how late the demo dates their own eyes
    // (povcalage); estimated: plus the server's wait to relay them (16 ms on
    // average: the demo shows the viewer that much after they were there)
    static int povdelay(fpsent *v)
    {
        int cn = v->clientnum, extra = cn >= 0 && cn <= 255 ? povextra[cn] : 0;
        if(povkind(v) != 1) extra = POV_RELAYWAIT;
        return clamp(v->ping + extra, 0, POV_MAXDELAY);
    }

    // the recorder of a local demo in another player's view: their own
    // positions (sent by them) reached that player through the server, so
    // their ping and one more relay wait later
    static int povdelayfor(fpsent *v, fpsent *d)
    {
        int delay = povdelay(v);
        if(play_cn >= 0 && d && d->clientnum == play_cn) delay = clamp(delay + povrecping + POV_RELAYWAIT, 0, POV_MAXDELAY);
        return delay;
    }

    // whose game is shown: the followed player (demopov), or with the free
    // camera the player chosen for it (demofreeview); NULL = the real
    // positions (a local demo: its recorder's game, the demo itself)
    static fpsent *povviewer(fpsent *target)
    {
        if(!game::demoxrayallowed()) return NULL;
        fpsent *v = target;
        if(!v)
        {
            if(demofreeview < 0 || !game::player1 || game::player1->state != CS_SPECTATOR) return NULL;
            v = game::getclient(demofreeview);
        }
        else if(!demopov) return NULL;
        if(!v || !povready() || !povkind(v)) return NULL;
        return v;
    }

    // a ghost for one (viewer, player) pair, kept from frame to frame
    static povghost &povpair(int viewer, int target)
    {
        loopi(POV_PAIRS) if(povpairs[i].valid && povpairs[i].viewer == viewer && povpairs[i].target == target) return povpairs[i];
        loopi(POV_PAIRS) if(!povpairs[i].valid) return povpairs[i];
        povghost &g = povpairs[povpairnext];
        povpairnext = (povpairnext + 1) % POV_PAIRS;
        povclearghost(g);
        return g;
    }

    // fps.cpp trackonefragreact: the times of a killer are measured from THEIR
    // real eyes and aim (their HD track; with no exact aim, their place in the
    // demo, not where the followed player's game showed them); false when
    // nothing is rebuilt or they cannot be placed
    bool truepos(fpsent *d, vec &o, float &yaw, float &pitch)
    {
        if(!d || !povviewer(game::followingplayer()) || d->clientnum < 0 || d->clientnum > 255) return false;
        if(playexact[d->clientnum] && hastrack(d->clientnum))
        {
            hdsample s;
            int idx = play_idx;
            if(!lerpsample(track, idx, server::demotime(), d->clientnum, s)) return false;
            o = s.o;
            yaw = s.yaw;
            pitch = s.pitch;
            return true;
        }
        if(!povkind(d)) return false;
        float roll;
        return povat(povpair(-1, d->clientnum), d->clientnum, server::demotime(), -1, 0, o, yaw, pitch, roll);
    }

    // fps.cpp trackonefragreact: where the killer's game showed the followed
    // player (target), so the killer's times are those of their own game
    // (demopov On; estimated without their exact aim); false when that view
    // cannot be rebuilt
    bool clientview(fpsent *target, fpsent *viewer, vec &o)
    {
        if(!demopov || !target || !viewer || target == viewer || !game::demoxrayallowed() || !povready() || !povkind(viewer)) return false;
        int tc = target->clientnum;
        if(tc < 0 || tc > 255) return false;
        int delay = povdelayfor(viewer, target);
        float yaw, pitch, roll;
        return povat(povpair(viewer->clientnum, tc), tc, server::demotime() - delay, viewer->clientnum, delay, o, yaw, pitch, roll);
    }

    // the other players of a player's view, every frame (applyothers)
    static bool povplace(fpsent *d, fpsent *viewer, vec &o, float &yaw, float &pitch, float &roll)
    {
        if(!viewer || !d || d == viewer || d->clientnum < 0 || d->clientnum > 255) return false;
        int delay = povdelayfor(viewer, d);
        return povat(povghosts[d->clientnum], d->clientnum, server::demotime() - delay, viewer->clientnum, delay, o, yaw, pitch, roll);
    }

    // where player d is shown at demo time t in this view: as the viewer's
    // game showed them, else the demo's position (HD track, else their network
    // positions, else the one now)
    static void povplaceat(fpsent *d, int t, fpsent *viewer, vec &o)
    {
        float yaw, pitch, roll;
        if(d->clientnum >= 0 && d->clientnum <= 255)
        {
            if(viewer && d != viewer)
            {
                int delay = povdelayfor(viewer, d);
                povscratch.valid = false; // a one-off moment: rebuilt exactly from the positions before it
                if(povat(povscratch, d->clientnum, t - delay, viewer->clientnum, delay, o, yaw, pitch, roll)) return;
            }
            hdsample hs;
            int idx = play_idx;
            if(play_loaded && hastrack(d->clientnum) && lerpsample(track, idx, t, d->clientnum, hs)) { o = hs.o; return; }
            povscratch.valid = false;
            if(povloaded && povat(povscratch, d->clientnum, t, -1, 0, o, yaw, pitch, roll)) return;
        }
        o = d->o;
    }

    // where the shooter's game showed player d when the shot was fired (tt:
    // the time of the demo's positions they were seeing), and how fast d went
    static bool povplacefor(fpsent *d, int tt, fpsent *shooter, vec &o, float &speed)
    {
        if(!d || !shooter || d == shooter || d->clientnum < 0 || d->clientnum > 255) return false;
        int t = tt;
        if(play_cn >= 0 && d->clientnum == play_cn) t -= povrecping + POV_RELAYWAIT; // the recorder: their own positions, sent earlier
        float yaw, pitch, roll;
        povscratch.valid = false;
        if(!povat(povscratch, d->clientnum, t, shooter->clientnum, max(tt - t, 0) + povdelay(shooter), o, yaw, pitch, roll)) return false;
        speed = vec(povscratch.e->vel).add(povscratch.e->falling).magnitude();
        return true;
    }

    bool povshownnow()
    {
        return povviewer(game::followingplayer()) != NULL;
    }

    // ---- the view shown, in words, for the demo controls ----

    // 1 the game of who (a player's view), 4 the game of who, estimated (no
    // exact aim), 2 the game of who, the recorder of a local demo (the demo
    // itself), 0 the real positions (HD tracks), 3 the demo's own positions
    static int viewkind(fpsent *&who)
    {
        who = NULL;
        if(fpsent *pv = povviewer(game::followingplayer())) { who = pv; return povkind(pv) == 1 ? 1 : 4; }
        if(play_cn >= 0) { who = game::getclient(play_cn); return who ? 2 : 3; } // a local demo: what its recorder's game showed
        return play_loaded && nexactcns > 0 ? 0 : 3;
    }

    static void viewlabel(char *buf, int len)
    {
        fpsent *who;
        int k = viewkind(who);
        bool freecam = !game::followingplayer();
        if(k == 1) nformatstring(buf, len, "\f4VIEW\f7  %s\f7's game%s", game::colorname(who), freecam ? " (free camera)" : "");
        else if(k == 4) nformatstring(buf, len, "\f4VIEW\f7  %s\f7's game, \f6estimated\f7%s", game::colorname(who), freecam ? " (free camera)" : "");
        else if(k == 2) nformatstring(buf, len, "\f4VIEW\f7  %s\f7's game (they recorded this demo)", game::colorname(who));
        else if(k == 0) copystring(buf, "\f4VIEW\f7  real positions (server)", len);
        else copystring(buf, "\f4VIEW\f7  positions of the demo", len);
    }

    // a player whose view can be shown (known: the demo's positions read, a
    // click; else what may be, a tip)
    static bool viewother(bool known)
    {
        if(known && !povready()) return false;
        loopv(game::players) if(povkindof(game::players[i], known)) return true;
        return false;
    }

    static const char *viewtip()
    {
        static string tip;
        fpsent *who, *f = game::followingplayer();
        int k = viewkind(who);
        if(f)
        {
            if(k == 1 || k == 4)
            {
                if(play_cn < 0) return "Click: the real positions (server)";
                fpsent *r = game::getclient(play_cn);
                if(!r) return "Click: the positions of the demo";
                formatstring(tip, "Click: %s\f7's game (they recorded this demo)", game::colorname(r));
                return tip;
            }
            int fk = povkindof(f, false);
            if(!fk) return play_cn >= 0 ? "This demo shows what its recorder's game showed" : "This player's view cannot be rebuilt: real positions";
            formatstring(tip, "Click: %s\f7's game%s", game::colorname(f), fk == 1 ? "" : ", estimated");
            return tip;
        }
        if(!viewother(false)) return play_cn >= 0 ? "This demo shows what its recorder's game showed" : "No player view in this demo: its own positions";
        return play_cn >= 0 ? "Click: next view (the recorder's game, then each player's, estimated)" :
                              "Click: next view (real positions, then each player's game)";
    }

    // the free camera: the next view, the real positions (a local demo: its
    // recorder's game) then the game of each player whose view can be shown,
    // by client number
    static void nextfreeview()
    {
        if(!game::demoplayback) { conoutf("Only while watching a demo"); return; }
        if(!game::demoxrayallowed()) { conoutf("Only for demos played on this computer"); return; }
        if(!viewother(true)) { conoutf("No player view in this demo"); return; }
        int next = -1;
        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(!povkind(d)) continue;
            int c = d->clientnum;
            if(c > demofreeview && (next < 0 || c < next)) next = c;
        }
        demofreeview = next;
        if(game::followingplayer()) execute("follow"); // the free camera, where the followed player was
        fpsent *v = povviewer(NULL);
        if(v) conoutf("Free camera: %s\f7's game%s", game::colorname(v), povkind(v) == 1 ? "" : ", estimated");
        else if(fpsent *r = play_cn >= 0 ? game::getclient(play_cn) : NULL) conoutf("Free camera: %s\f7's game (they recorded this demo)", game::colorname(r));
        else conoutf("Free camera: real positions");
    }

    static void viewclick()
    {
        fpsent *f = game::followingplayer();
        if(f) { if(povkindof(f, false) || povviewer(f)) execute("toggledemopov"); return; }
        if(viewother(false)) nextfreeview();
    }

    // the HD track of the player whose view is shown lost samples here (more
    // than 250 ms without one, lerpsample): their network position is shown,
    // not a frozen or sliding player
    static bool viewhole(char *buf, int len)
    {
        if(play_cn >= 0 || !play_loaded || !game::demoxrayallowed()) return false;
        fpsent *f = game::followingplayer(), *v = f ? f : povviewer(NULL);
        if(!povexact(v) || v->state != CS_ALIVE) return false;
        hdsample s;
        int idx = play_idx;
        if(lerpsample(track, idx, server::demotime(), v->clientnum, s)) return false;
        nformatstring(buf, len, "%s\f6: HD AIM LOST HERE, NETWORK POSITION SHOWN", game::colorname(v));
        return true;
    }

    // the demo controls hidden by their button, hitboxes drawn: the view they
    // are in, at the top
    static void dbviewtop(int w, int h, float s)
    {
        string vl, hole;
        viewlabel(vl, sizeof(vl));
        bool hl = viewhole(hole, sizeof(hole));
        float th = 20*s, pad = 9*s;
        float tw = max(dbtextw(vl, th), hl ? dbtextw(hole, th*0.8f) : 0.0f);
        float bw = tw + 2*pad, bh = th + 1.2f*pad + (hl ? th*0.8f + 4*s : 0);
        float x = (w - bw)/2, y = 12*s;
        dbpanel(x, y, bw, bh, max(1.0f, s));
        dbtext(vl, x + pad, y + 0.6f*pad, th, 0xFFFFFF);
        if(hl) dbtext(hole, x + pad, y + 0.6f*pad + th + 4*s, th*0.8f, DB_ORANGE);
    }

    // ---- going back keeps the player followed ----

    // server.cpp seekdemoms: going back restarts the demo, which forgot the
    // player followed (-10 s, PgUp, a click on the timeline, a time typed); they
    // are kept, quietly, and followed again (by name: a client number can be
    // somebody else earlier in the demo)
    void seekkeep()
    {
        seekcn = game::demoplayback && game::player1 && game::player1->state == CS_SPECTATOR ? game::following : -1;
        if(seekcn < 0) return;
        fpsent *d = game::getclient(seekcn);
        if(!d) { seekcn = -1; return; }
        copystring(seekname, d->name);
        game::following = -1; // no "follow off" while the demo restarts
    }

    void seekrefollow()
    {
        if(seekcn < 0) return;
        int cn = seekcn;
        seekcn = -1;
        fpsent *d = game::getclient(cn);
        if(!d || strcmp(d->name, seekname))
        {
            d = NULL;
            loopv(game::players) if(game::players[i] && !strcmp(game::players[i]->name, seekname)) { d = game::players[i]; break; }
        }
        if(d && d != game::player1 && d->state != CS_SPECTATOR && game::demoplayback && game::player1->state == CS_SPECTATOR) game::following = d->clientnum;
        else conoutf("follow off");
    }

    // ---- going to a time typed to the millisecond ----

    // "9:00.842", "9.00.842", "9:00,842", "9:00", "540.842" (seconds) -> ms; -1 if unreadable
    static int parsedemotime(const char *s)
    {
        if(!s) return -1;
        while(*s == ' ') s++;
        int part[3] = { 0, 0, 0 }, digits[3] = { 0, 0, 0 }, n = 0;
        char sep[3] = { 0, 0, 0 };
        while(*s && n < 3)
        {
            if(*s < '0' || *s > '9') return -1;
            int v = 0, l = 0;
            while(*s >= '0' && *s <= '9') { if(l < 9) v = v*10 + (*s - '0'); l++; s++; }
            part[n] = v;
            digits[n] = min(l, 9);
            n++;
            if(!*s || *s == ' ') break;
            if(*s != ':' && *s != '.' && *s != ',') return -1;
            sep[n-1] = *s++;
        }
        while(*s == ' ') s++;
        if(*s || !n) return -1;
        int mins = 0, secs = 0, frac = -1, fdigits = 0;
        if(n == 1) secs = part[0];
        else if(n == 2 && sep[0] == ':') { mins = part[0]; secs = part[1]; }
        else if(n == 2) { secs = part[0]; frac = part[1]; fdigits = digits[1]; }
        else { mins = part[0]; secs = part[1]; frac = part[2]; fdigits = digits[2]; }
        if(n >= 2 && (n == 3 || sep[0] == ':') && secs >= 60) return -1;
        int ms = 0;
        if(frac >= 0)
        {
            ms = frac;
            for(int d = fdigits; d < 3; d++) ms *= 10;
            for(int d = fdigits; d > 3; d--) ms /= 10;
        }
        return (mins*60 + secs)*1000 + ms;
    }

    // The time of the demo controls, typed in place: a click on it (or GO TO,
    // or demogotoprompt) pauses the demo and turns it into a field holding the
    // time now, selected (typing replaces it); Enter goes to the time typed, to
    // the millisecond, paused there; Esc puts the demo back as it was. The keys
    // go to the command line (console.cpp), which is not drawn meanwhile; its
    // text is "/demogoto <time>" so it runs as a command, never as a message.
    #define DB_GOTOCMD "/demogoto "
    static bool dbtimeedit = false, dbtimefresh = false, dbtimeplay = false, dbtimewent = false;
    static string dbtimeinit;

    bool timeediting()
    {
        return dbtimeedit && commandmillis >= 0 && game::demoplayback && !dbhidden && game::demoxrayallowed() &&
               !strncmp(commandbuf, DB_GOTOCMD, strlen(DB_GOTOCMD));
    }

    // the demo at that time exactly, paused there
    static void gototime(const char *s)
    {
        if(!game::demoplayback) { conoutf("Only while watching a demo"); return; }
        if(!game::demoxrayallowed()) { conoutf("Only for demos played on this computer"); return; }
        int ms = parsedemotime(s);
        if(ms < 0) { conoutf("Type a time like 9:00.842"); return; }
        if(dmolenms > 0) ms = min(ms, dmolenms - 1);
        server::seekdemoms(ms); // the player followed is kept (seekkeep)
        if(!server::ispaused()) server::forcepaused(true);
        dbtimewent = true;
    }

    // GO TO, demogotoprompt, a click on the time: the field (the command line
    // when the demo controls are hidden)
    static void gotoprompt()
    {
        if(!game::demoplayback || !game::demoxrayallowed() || dbtimeedit) return;
        if(dbhidden) { execute("inputcommand [/demogoto ] [] [Go to a time, paused (e.g. 9:00.842):] x"); return; }
        dbtimeplay = !server::ispaused();
        if(dbtimeplay) server::forcepaused(true);
        string now;
        dbtimems(clamp(server::demotime(), 0, max(dmolenms, 1)), now, sizeof(now));
        formatstring(dbtimeinit, "%s%s", DB_GOTOCMD, now);
        defformatstring(cmd, "inputcommand [%s] [] [] x", dbtimeinit);
        execute(cmd);
        dbtimeedit = commandmillis >= 0;
        dbtimefresh = true;
        dbtimewent = false;
    }

    // every frame: the field closed (Enter went to the time; Esc or a time that
    // could not be read: the demo plays again if it was playing), typing over
    // the selected time, the caret kept after "/demogoto "
    static void timeupdate()
    {
        if(!dbtimeedit) return;
        size_t pl = strlen(DB_GOTOCMD);
        if(commandmillis < 0 || !game::demoplayback || strncmp(commandbuf, DB_GOTOCMD, pl))
        {
            dbtimeedit = false;
            if(!dbtimewent && dbtimeplay && game::demoplayback && server::ispaused()) server::forcepaused(false);
            return;
        }
        if(dbtimefresh && strcmp(commandbuf, dbtimeinit))
        {
            size_t n = strlen(dbtimeinit);
            if(!strncmp(commandbuf, dbtimeinit, n) && commandpos < 0) memmove(&commandbuf[pl], &commandbuf[n], strlen(&commandbuf[n]) + 1);
            dbtimefresh = false;
        }
        if(commandpos >= 0 && commandpos < int(pl)) commandpos = int(strlen(commandbuf)) > int(pl) ? int(pl) : -1;
    }

    // the keys the command line would use to leave the field (history, the
    // start of the line); on the selected time, Backspace or Delete clears it
    static bool timekey(int code, bool isdown)
    {
        if(!timeediting()) return false;
        int pl = int(strlen(DB_GOTOCMD)), len = int(strlen(commandbuf));
        bool atstart = commandpos == pl || (commandpos < 0 && len <= pl);
        switch(code)
        {
            case SDLK_UP: case SDLK_DOWN: case SDLK_TAB: case SDLK_PAGEUP: case SDLK_PAGEDOWN:
                return true;
            case SDLK_HOME:
                if(isdown) { dbtimefresh = false; commandpos = len > pl ? pl : -1; }
                return true;
            case SDLK_LEFT:
                if(isdown) dbtimefresh = false;
                return atstart;
            case SDLK_BACKSPACE: case SDLK_DELETE:
                if(isdown && dbtimefresh) { commandbuf[pl] = '\0'; commandpos = -1; dbtimefresh = false; return true; }
                return code == SDLK_BACKSPACE && atstart;
            case SDLK_RIGHT: case SDLK_END:
                if(isdown) dbtimefresh = false;
                return false;
        }
        return false;
    }

    // the time "now / length" at the top right of the demo controls; the time
    // now can be clicked (id 12) and becomes the field
    static void dbtimefield(const char *tnow, const char *tdur, float right, float y, float th, float s, int w, int h, float cx, float cy)
    {
        defformatstring(rest, " / %s", tdur);
        float rw = dbtextw(rest, th), padx = 5*s;
        bool edit = timeediting(), local = game::demoxrayallowed();
        const char *shown = edit ? commandbuf + strlen(DB_GOTOCMD) : tnow;
        float fw = max(dbtextw(shown, th), dbtextw("0:00.000", th)) + 2*padx;
        float fx = right - rw - fw, fy = y - 2*s, fh = th + 4*s;
        if(local) dbset(12, fx/w, fy/h, fw/w, fh/h);
        bool hover = local && !edit && dbhit(cx, cy) == 12;
        if(edit)
        {
            hudrect(fx, fy, fw, fh, 0x101318, 240);
            dbedge(fx, fy, fw, fh, max(1.0f, 2*s), 255);
            if(dbtimefresh && shown[0]) hudrect(fx + padx, fy + 2*s, dbtextw(shown, th), th, DB_ORANGE, 110); // selected: typing replaces it
            int caret = commandpos >= 0 ? commandpos - int(strlen(DB_GOTOCMD)) : int(strlen(shown));
            float k = dbtextk(th);
            pushhudmatrix();
            hudmatrix.translate(fx + padx, y, 0);
            hudmatrix.scale(k, k, 1);
            flushhudmatrix();
            draw_text(shown, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, dbtimefresh ? -1 : caret);
            pophudmatrix();
        }
        else
        {
            if(hover) hudrect(fx, fy, fw, fh, DB_ORANGE, 45);
            dbtext(tnow, fx + padx, y, th, hover ? DB_ORANGE : 0xE6E6E6);
        }
        dbtext(rest, right - rw, y, th, 0xE6E6E6);
    }

    // ---- the free camera, looking around ----

    // a local demo shows its recorder until you follow somebody or leave them
    bool recorderview() { return game::demoplayback && play_cn >= 0 && !autofollowdone; }

    // the free camera of a demo flies with the real time, at the normal speed
    // whatever the speed of the demo (0.25x, 2x) and while it is paused: the
    // demo, its players and its time are not touched (fps.cpp updateworld)
    static int flylast = -1;
    bool freefly()
    {
        fpsent *p = game::player1;
        if(!game::demoplayback || !game::demoxrayallowed() || !p || p->state != CS_SPECTATOR || game::followingplayer() || recorderview())
        {
            flylast = -1;
            return false;
        }
        if(flylast < 0 || totalmillis - flylast > 250) { flylast = totalmillis; p->newpos = p->o; p->deltapos = vec(0, 0, 0); }
        int diff = totalmillis - flylast;
        if(diff > 0)
        {
            int steps = (diff + POV_STEP - 1)/POV_STEP;
            p->o = p->newpos;
            loopi(steps - 1) moveplayer(p, 10, true, POV_STEP);
            p->deltapos = p->o;
            moveplayer(p, 10, true, POV_STEP);
            p->newpos = p->o;
            p->deltapos.sub(p->newpos);
            flylast += steps*POV_STEP;
        }
        // as engine/physics.cpp interppos: drawn between the last two steps
        p->o = p->newpos;
        int ahead = flylast - totalmillis;
        if(ahead > 0) p->o.add(vec(p->deltapos).mul(min(ahead, POV_STEP)/float(POV_STEP)));
        return true;
    }

    static bool islookkey(int code)
    {
        const char *name = getkeyname(code);
        if(!name || !demolookkey[0]) return false;
        size_t n = strlen(name);
        for(const char *s = demolookkey; *s;)
        {
            while(*s == ' ') s++;
            const char *e = s;
            while(*e && *e != ' ') e++;
            if(size_t(e - s) == n && !strncasecmp(s, name, n)) return true;
            s = e;
        }
        return false;
    }

    // the look key while a demo plays: held, the mouse turns the view
    bool lookkey(int code, bool isdown)
    {
        if(!islookkey(code)) return false;
        if(isdown)
        {
            if(!game::demoxrayallowed()) return false;
            if(!lookon) { SDL_GetMouseState(&lookx, &looky); lookon = true; lookback = false; }
        }
        else if(lookon) { lookon = false; lookback = true; lookbackms = totalmillis; }
        else return false;
        return true;
    }

    bool looking() { return lookon && game::demoplayback && game::demoxrayallowed(); }

    // main.cpp: the mouse is freed again, the cursor goes back where it was
    bool lookcursor(int &x, int &y)
    {
        if(!lookback) return false;
        lookback = false;
        if(totalmillis - lookbackms > 500) return false;
        x = lookx;
        y = looky;
        return true;
    }

    // ---- hitboxes and shots ----

    // how a hitbox of a shot is drawn: crossed (white), beside (yellow), the
    // rebuilt view and the shooter's game disagree near the edge of the hitbox
    // (pink, "uncertain") or further (red, with what their game counted), the
    // victim of another player's shot in a player's view (light blue)
    enum { HB_CROSSED = 0, HB_BESIDE, HB_UNSURE, HB_OTHER, HB_DISAGREE };
    struct hbcyl { vec o; float eyeheight, aboveeye, radius, margin, slack; int look; bool victim, gamehit; }; // HB_DISAGREE: pink or red when drawn
    struct hbshot
    {
        // t: played by the server; ts: fired, on the demo's time (the shooter's HD track, else when the demo got it);
        // tt: the time of the demo's positions the shooter's game was showing then (their own view), -1 unknown
        int t, ts, tt, cn, gun, color, nhit, view, ends, victim, ncand, ngamehits;
        bool line;
        vec from, to, eye;
        string name;
        int cand[HB_MAXCAND], gamehits[HB_MAXHIT]; // gamehits: the players the shooter's game touched (N_DAMAGE)
        hbcyl hit[HB_MAXHIT];
    };
    static vector<hbshot> hbshots;
    static vector<vec> hblines;

    bool hitboxactive() { return demohitbox && game::demoxrayallowed(); }

    void togglehitbox()
    {
        if(!game::demoplayback) { conoutf("Hitboxes are only shown while watching a demo"); return; }
        if(!game::demoxrayallowed()) { conoutf("Hitboxes are only for demos played on this computer"); return; }
        demohitbox = demohitbox ? 0 : 1;
        conoutf("Hitboxes %s", demohitbox ? "on" : "off");
    }

    static hbshot &hbadd(fpsent *s, int now)
    {
        while(hbshots.length() >= HB_MAXSHOTS) hbshots.remove(0);
        hbshot &h = hbshots.add();
        h.t = h.ts = now;
        h.tt = -1;
        h.cn = s->clientnum;
        h.gun = -1;
        h.color = game::demohitboxcolor(s);
        copystring(h.name, game::colorname(s));
        h.nhit = h.ncand = h.ngamehits = 0;
        h.view = -2;
        h.ends = now + HB_SHOTMS;
        h.victim = -1;
        h.line = true;
        return h;
    }

    // the clock of a player's shots: each shot's gap (demo time - their id of
    // it), the smallest of their last ones (povclock)
    static void povlearnclock(int cn, int t, int id)
    {
        if(cn < 0 || cn > 255 || id < 0) return;
        int v = t - id, n = povclkn[cn];
        if(n)
        {
            int lo = povclk[cn][0];
            loopi(min(n, POV_CLOCKS)) lo = min(lo, povclk[cn][i]);
            if(v < lo - 100 || v > lo + 300) n = 0; // their clock jumped (a pause, a new map): learnt again
        }
        povclk[cn][n%POV_CLOCKS] = v;
        povclkn[cn] = n + 1;
    }

    static bool povclock(int cn, int &c)
    {
        int n = cn >= 0 && cn <= 255 ? min(povclkn[cn], POV_CLOCKS) : 0;
        if(n <= 0) return false;
        c = povclk[cn][0];
        loopi(n) c = min(c, povclk[cn][i]);
        return true;
    }

    // With the shooter's exact aim (HD track): the moment their eyes were where
    // the line starts ("from") AND looked along it (their yaw and pitch against
    // from -> to), in the 160 ms before the demo got the shot; -1 when that
    // moment is not clear (eyes and aim still: the middle of a 100 ms stillness
    // was 30-60 ms off, the target drawn where it was too early)
    #define HB_FIREWIN 160
    #define HB_FIRECLEAR 8
    static int hbhdinstant(int cn, int t, const vec &from, const vec &to)
    {
        vec sdir = vec(to).sub(from);
        float slen = sdir.magnitude();
        if(slen < 0.5f) return -1;
        sdir.div(slen);
        static float cost[HB_FIREWIN+1];
        hdsample s;
        int lo = 0, hi = track.length(), best = -1;
        while(lo < hi) { int mid = (lo + hi)/2; if(track[mid].t < t) lo = mid + 1; else hi = mid; } // also for a shot read at once by a jump
        int idx = lo;
        loopk(HB_FIREWIN+1)
        {
            cost[k] = 1e9f;
            if(!lerpsample(track, idx, t - k, cn, s)) continue;
            float dp = s.o.dist(from);
            if(dp > 2.0f) continue; // the eyes elsewhere
            vec aim;
            vecfromyawpitch(s.yaw, s.pitch, 1, 0, aim);
            cost[k] = dp/0.1f + acosf(clamp(aim.dot(sdir), -1.0f, 1.0f))/RAD/0.05f; // 0.1 unit of the eyes = 0.05 degree of aim
            if(best < 0 || cost[k] < cost[best]) best = k;
        }
        if(best < 0) return -1;
        lo = hi = best;
        while(lo > 0 && cost[lo-1] <= cost[best] + 1) lo--;
        while(hi < HB_FIREWIN && cost[hi+1] <= cost[best] + 1) hi++;
        return hi - lo <= HB_FIRECLEAR && hi < HB_FIREWIN - 4 ? t - (lo + hi)/2 : -1;
    }

    // When the shot was fired (ts, on the demo's time: the shooter's HD track,
    // else when the demo got it) and the time of the demo's positions their
    // game was showing then (tt, for their own view). Their own clock of the
    // shot (N_SHOTFX id) plus its smallest gap to the demo (povclock: a shot
    // sent at once) is when the demo would have got it: their game showed the
    // others their ping and 15 ms before that (measured on demos with exact
    // aim: within 5-7 ms for 9 shots in 10). With their exact aim: ts from the
    // HD track and tt = ts - the delay of their view, unless it is not clear or
    // disagrees with their clock by more than 30 ms (an HD track dated
    // otherwise at the start of a demo): their clock. The recorder of a local
    // demo: their view is the demo itself, at ts.
    #define HB_CLOCKLATE 15
    #define HB_CLOCKAGREE 30
    static void hbinstants(fpsent *s, int t, int id, const vec &from, const vec &to, int &ts, int &tt)
    {
        int cn = s->clientnum;
        ts = t;
        tt = -1;
        if(cn < 0 || cn > 255) return;
        int c, ttclock = id >= 0 && povclock(cn, c) ? id + c - clamp(s->ping, 0, POV_MAXDELAY) - HB_CLOCKLATE : -1;
        bool exact = play_loaded && playexact[cn];
        int hd = exact ? hbhdinstant(cn, t, from, to) : -1;
        if(play_cn >= 0 && cn == play_cn) { if(hd >= 0) ts = hd; return; } // the recorder of a local demo
        if(!exact) { tt = ttclock; return; }
        int delay = povdelay(s);
        if(hd >= 0 && (ttclock < 0 || abs(hd - delay - ttclock) <= HB_CLOCKAGREE)) { ts = hd; tt = hd - delay; return; }
        if(ttclock >= 0) { tt = ttclock; ts = clamp(ttclock + delay, t - POV_LAG, t); return; }
        if(hd >= 0) ts = hd;
        tt = ts - delay;
    }

    // a shot replayed from the demo (client.cpp N_SHOTFX, id: the shooter's
    // clock of it); its hitboxes are worked out for the view they are drawn in
    // (hbcompute)
    void noteshot(fpsent *s, int gun, const vec &from, const vec &to, int id)
    {
        if(!s || !game::demoxrayallowed()) return;
        int now = server::demotime(), pt = min(server::demopackettime(), now); // the exact time of the shot in the demo
        if(demohitbox || demopov) povlearnclock(s->clientnum, pt, id); // the shooter's clock, also before the hitboxes are on and for a shot read at once by a jump
        if(!hitboxactive()) return;
        if(gun < GUN_SG || gun > GUN_PISTOL || gun == GUN_RL || gun == GUN_GL) return; // projectiles: no line
        if(vec(to).sub(from).magnitude() < 0.5f) return;
        int ts, tt;
        hbinstants(s, pt, id, from, to, ts, tt);
        if(now - pt > HB_SHOTMS) return; // read at once by a jump in the demo, already over
        hbshot &h = hbadd(s, pt);
        h.ts = ts;
        h.tt = tt;
        h.gun = gun;
        h.from = h.eye = from;
        h.to = to;
        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(d && d != s && d->state == CS_ALIVE && d->clientnum >= 0 && h.ncand < HB_MAXCAND) h.cand[h.ncand++] = d->clientnum;
        }
    }

    // the shot of this player played at the same moment of the demo: the
    // server sends a shot (N_SHOTFX), then the hits it made (N_DAMAGE) and the
    // frag (N_DIED), all at once; a frag with no shot then (another weapon, a
    // fall) has no line. The recorder of a local demo: their own last shot,
    // written as they fired, the server's answer a ping later
    static hbshot *hbshotat(int cn, int pt)
    {
        bool rec = play_cn >= 0 && cn == play_cn;
        loopvrev(hbshots)
        {
            hbshot &h = hbshots[i];
            if(h.cn != cn || !h.line) continue;
            if(rec ? h.t <= pt && pt - h.t <= POV_MAXDELAY : abs(pt - h.t) <= 2) return &h;
        }
        return NULL;
    }

    static void hbgamehit(hbshot &h, int cn)
    {
        loopi(h.ngamehits) if(h.gamehits[i] == cn) return;
        if(h.ngamehits < HB_MAXHIT) h.gamehits[h.ngamehits++] = cn;
        h.view = -2;
    }

    // a hit replayed from the demo (client.cpp N_DAMAGE): the shooter's game
    // touched this player with that shot
    void notedamage(fpsent *target, fpsent *actor)
    {
        if(!target || !actor || target == actor || !hitboxactive()) return;
        int now = server::demotime(), pt = min(server::demopackettime(), now);
        if(hbshot *h = hbshotat(actor->clientnum, pt)) hbgamehit(*h, target->clientnum);
    }

    // a frag replayed from the demo (client.cpp N_DIED): the shot that killed
    // and the hitbox of its victim stay demohitboxdeath ms where they were hit
    void notedeath(fpsent *victim, fpsent *actor)
    {
        if(!victim || !actor || victim == actor || demohitboxdeath <= 0 || !hitboxactive()) return;
        int now = server::demotime(), pt = min(server::demopackettime(), now);
        if(now - pt > demohitboxdeath) return; // read at once by a jump in the demo, already over
        if(hbshot *s = hbshotat(actor->clientnum, pt))
        {
            hbshot &h = *s;
            hbgamehit(h, victim->clientnum);
            if(h.victim < 0)
            {
                h.victim = victim->clientnum;
                h.ends = max(h.ends, pt + demohitboxdeath);
                h.view = -2;
                return;
            }
        }
        // no shot line (rocket, grenade...): the victim's hitbox only
        hbshot &h = hbadd(actor, pt);
        h.from = h.eye = h.to = victim->o;
        h.line = false;
        h.victim = victim->clientnum;
        h.ends = pt + demohitboxdeath;
    }

    // along the line: how deep a point is in the hitbox (> 0 inside), and how
    // far it is from it outside
    static float hbdepth(const vec &p, const vec &o, float bottom, float top, float radius)
    {
        return min(min(radius - vec(p.x - o.x, p.y - o.y, 0).magnitude(), p.z - bottom), top - p.z);
    }
    static float hbdist(const vec &p, const vec &o, float bottom, float top, float radius)
    {
        float dxy = max(vec(p.x - o.x, p.y - o.y, 0).magnitude() - radius, 0.0f), dz = max(max(bottom - p.z, p.z - top), 0.0f);
        return sqrtf(dxy*dxy + dz*dz);
    }

    // how far the line is from the edge of the hitbox (units): crossing, how
    // deep it passes in; beside, how far from it it passes (the depth is
    // concave along the line and the distance convex: one extremum, by thirds)
    static float hbmargin(const vec &o, float eyeheight, float aboveeye, float radius, const vec &from, const vec &to, bool crossing)
    {
        vec dir = vec(to).sub(from);
        float len = dir.magnitude();
        if(len < 1e-3f) return 0;
        dir.div(len);
        float bottom = o.z - eyeheight, top = o.z + aboveeye, lo = 0, hi = len;
        loopi(40)
        {
            float a = lo + (hi - lo)/3, b = hi - (hi - lo)/3;
            vec pa = vec(dir).mul(a).add(from), pb = vec(dir).mul(b).add(from);
            if(crossing ? hbdepth(pa, o, bottom, top, radius) < hbdepth(pb, o, bottom, top, radius) : hbdist(pa, o, bottom, top, radius) > hbdist(pb, o, bottom, top, radius)) lo = a;
            else hi = b;
        }
        vec p = vec(dir).mul((lo + hi)/2).add(from);
        return max(crossing ? hbdepth(p, o, bottom, top, radius) : hbdist(p, o, bottom, top, radius), 0.0f);
    }

    // the hitboxes of a shot in the view drawn now (viewer, or NULL = the
    // demo's positions): only the viewer's own shots in a player's view (in a
    // local demo, the recorder's in theirs, the demo itself), the victim of a
    // frag always
    #define HB_ESTSPREAD 15
    static void hbcompute(hbshot &h, fpsent *pv)
    {
        h.view = pv ? pv->clientnum : -1;
        h.nhit = 0;
        fpsent *s = game::getclient(h.cn);
        h.eye = h.from;
        if(h.line && pv && s && s != pv) povplaceat(s, h.ts, pv, h.eye); // the shooter where this view shows them
        int judge = pv ? pv->clientnum : play_cn; // whose game decided here
        bool own = judge < 0 || h.cn == judge, mine = judge >= 0 && h.cn == judge, estimated = pv && povkind(pv) == 2;
        vec dir = vec(h.to).sub(h.eye);
        float len = dir.magnitude();
        loopi(h.ncand + 1)
        {
            int cn = i < h.ncand ? h.cand[i] : h.victim;
            if(cn < 0 || h.nhit >= HB_MAXHIT) continue;
            bool victim = cn == h.victim, gamehit = victim;
            if(victim && i < h.ncand) continue; // the victim once, at the end
            loopj(h.ngamehits) if(h.gamehits[j] == cn) gamehit = true;
            if(!own && !victim) continue;
            fpsent *d = game::getclient(cn);
            if(!d || (d == pv && d == game::followingplayer() && !isthirdperson())) continue; // your own eyes
            vec o;
            float speed = 0;
            // where this view showed them when the shot was fired; the shooter's own view: what their game was showing
            if(!(mine && pv && h.tt >= 0 && povplacefor(d, h.tt, pv, o, speed))) povplaceat(d, h.ts, pv, o);
            bool hit = false, nearby = victim || (mine && gamehit);
            if(h.line && len >= 0.5f)
            {
                vec saved = d->o;
                float radius = d->radius;
                d->o = o;
                hit = game::intersect(d, h.eye, vec(dir).mul((len + 1.0f)/len).add(h.eye)); // "to" stops on the hitbox it hit
                d->radius = radius + HB_NEAR;
                if(!nearby) nearby = hit || game::intersect(d, h.eye, vec(dir).mul((len + HB_NEAR)/len).add(h.eye));
                d->radius = radius;
                d->o = saved;
            }
            if(!nearby) continue;
            hbcyl &c = h.hit[h.nhit++];
            c.o = o;
            c.eyeheight = d->eyeheight;
            c.aboveeye = d->aboveeye;
            c.radius = d->radius;
            c.margin = c.slack = 0;
            c.gamehit = gamehit;
            // the shooter's own view: checked against what their game decided (shotgun: pellets at random, only
            // the centre line is drawn, so a pellet that touched counts): when they disagree, uncertain near the
            // edge (demohitboxuncertain; an estimated view: also what the target moves in 15 ms), else disagree;
            // the real positions: the line alone; another player's shot in a player's view: decided by that game
            if(!h.line) c.look = judge >= 0 && !mine ? HB_OTHER : HB_CROSSED; // a frag with no line (rocket, grenade): its victim
            else if(mine && h.gun != GUN_SG && len >= 0.5f && hit != gamehit)
            {
                c.margin = hbmargin(o, d->eyeheight, d->aboveeye, d->radius, h.eye, vec(dir).mul((len + 1.0f)/len).add(h.eye), hit);
                c.slack = estimated ? speed*HB_ESTSPREAD/1000.0f : 0.0f;
                c.look = HB_DISAGREE; // pink or red by demohitboxuncertain, when drawn
            }
            else if(mine) c.look = (h.gun == GUN_SG ? gamehit || hit : hit) ? HB_CROSSED : HB_BESIDE;
            else if(judge >= 0) c.look = HB_OTHER;
            else c.look = hit ? HB_CROSSED : HB_BESIDE;
            c.victim = victim;
        }
    }

    static void hbcylinder(const vec &o, float eyeheight, float aboveeye, float radius)
    {
        float bottom = o.z - eyeheight, top = o.z + aboveeye;
        loopi(HB_SIDES)
        {
            float a0 = 2*PI*i/HB_SIDES, a1 = 2*PI*(i+1)/HB_SIDES;
            vec p0(o.x + cosf(a0)*radius, o.y + sinf(a0)*radius, 0), p1(o.x + cosf(a1)*radius, o.y + sinf(a1)*radius, 0);
            hblines.add(vec(p0.x, p0.y, bottom)); hblines.add(vec(p1.x, p1.y, bottom));
            hblines.add(vec(p0.x, p0.y, top)); hblines.add(vec(p1.x, p1.y, top));
        }
        // the two side edges as seen from the camera
        vec side(o.y - camera1->o.y, camera1->o.x - o.x, 0);
        float sl = side.magnitude();
        if(sl < 1e-3f) return;
        side.mul(radius/sl);
        loopk(2)
        {
            vec e = k ? vec(o).add(side) : vec(o).sub(side);
            hblines.add(vec(e.x, e.y, bottom)); hblines.add(vec(e.x, e.y, top));
        }
    }

    void drawhitboxes(int w, int h)
    {
        if(!hitboxactive() || !camera1) { hbshots.setsize(0); return; }
        int now = server::demotime();
        loopvrev(hbshots) if(hbshots[i].t > now || now > hbshots[i].ends) hbshots.remove(i);
        float thick = max(1.0f, h/720.0f);
        fpsent *f = game::followingplayer(), *exclude = isthirdperson() ? NULL : f, *pv = povviewer(f);
        loopv(game::players)
        {
            fpsent *d = game::players[i];
            if(!d || d == game::player1 || d == exclude || d->lifesequence < 0) continue;
            if(d->state != CS_ALIVE && d->state != CS_LAGGED) continue;
            hblines.setsize(0);
            hbcylinder(d->o, d->eyeheight, d->aboveeye, d->radius);
            hudworldlines(hblines.getbuf(), hblines.length(), w, h, game::demohitboxcolor(d), 200, thick);
        }
        int view = pv ? pv->clientnum : -1;
        float th = max(12.0f, h*18/1080.0f);
        loopv(hbshots)
        {
            hbshot &s = hbshots[i];
            if(s.view != view) hbcompute(s, pv);
            if(s.victim < 0 && !demohitboxshots) continue;
            int alpha = 255 - (now - s.t)*175/max(s.ends - s.t, 1);
            if(s.line && demohitboxshots)
            {
                hblines.setsize(0);
                hblines.add(s.eye); hblines.add(s.to);
                loopk(3)
                {
                    vec d(0, 0, 0);
                    d[k] = 1.5f;
                    hblines.add(vec(s.to).sub(d)); hblines.add(vec(s.to).add(d));
                }
                hudworldlines(hblines.getbuf(), hblines.length(), w, h, s.color, alpha, thick);
                float x, y;
                if(hudworldpoint(s.to, w, h, x, y)) dbtext(s.name, x + th*0.5f, y - th*0.5f, th, s.color, alpha);
            }
            loopj(s.nhit)
            {
                hbcyl &c = s.hit[j];
                const int looks[5] = { 0xFFFFFF, 0xFFD040, 0xFF5AE6, demohitboxothercolour, 0xFF3C3C }; // white, yellow, pink, light blue, red
                int look = c.look == HB_DISAGREE && c.margin <= demohitboxuncertain + c.slack ? HB_UNSURE : c.look, col = looks[clamp(look, 0, 4)];
                hblines.setsize(0);
                hbcylinder(c.o, c.eyeheight, c.aboveeye, c.radius);
                hudworldlines(hblines.getbuf(), hblines.length(), w, h, col, alpha, c.victim ? thick*1.75f : thick);
                float x, y;
                if((look == HB_UNSURE || look == HB_DISAGREE) && hudworldpoint(vec(c.o).addz(c.aboveeye + 3), w, h, x, y))
                {
                    string msg;
                    if(look == HB_UNSURE) copystring(msg, "uncertain");
                    else formatstring(msg, c.gamehit ? "their game: hit, line %.2f units outside" : "their game: missed, line %.2f units inside", c.margin);
                    dbtext(msg, x - dbtextw(msg, th)/2, y - 1.6f*th, th, col, alpha); // above the impact cross
                }
            }
        }
    }
}

ICOMMAND(toggledemohitbox, "", (), demohd::togglehitbox());
ICOMMAND(demohitboxshown, "", (), intret(demohd::hitboxactive() ? 1 : 0));
ICOMMAND(toggledemopov, "", (),
{
    if(!game::demoplayback) { conoutf("Only while watching a demo"); return; }
    if(!game::demoxrayallowed()) { conoutf("Only for demos played on this computer"); return; }
    demopov = demopov ? 0 : 1;
    conoutf("%s", demopov ? "Following a player: the others as their game showed them" : "Following a player: the real positions");
});
ICOMMAND(demopovshown, "", (), intret(demohd::povshownnow() ? 1 : 0));
ICOMMAND(demonextview, "", (), demohd::nextfreeview());
ICOMMAND(demogoto, "s", (char *t), demohd::gototime(t));
ICOMMAND(demogotoprompt, "", (), demohd::gotoprompt());
ICOMMAND(demoviewshown, "", (),
{
    fpsent *who;
    int k = demohd::viewkind(who);
    intret((k == 1 || k == 4) && who ? who->clientnum : -1);
});
ICOMMAND(demoholeshown, "", (),
{
    string hole;
    intret(demohd::viewhole(hole, sizeof(hole)) ? 1 : 0);
});
bool game::demolookcursor(int &x, int &y) { return demohd::lookcursor(x, y); }
bool game::demotimeedit() { return demohd::timeediting(); }
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

