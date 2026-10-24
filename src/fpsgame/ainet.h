// Tiny Apa's moves from a small neural network (botnet 1). Prototype, off by default.
// Not a standalone header: aihumanproc.h includes it at its end, inside namespace ai (so no line of ai.cpp moves).
//
// The keys a Tiny Apa bot holds (forwards / backwards x sideways) and its jumps are drawn from a small network learned
// offline on the reference player's own games (imitation), every 33 ms (the rate of the player's position updates in the
// demos), wherever it is: under fire, and also out of a fight (on its way somewhere, holding a place, searching), so
// that it always moves the same way. It only gets what the bot perceives (human perception: what it sees of the enemy,
// where it last saw him and how long ago, whether he aims at it, his reload, the last sound it heard of him), the walls,
// steps down and deadly places around it, which steps would hide it from him, the keys it held lately, how long it has
// been alive, and out of a fight where it is going: WHERE to go stays the choice of the rules and of the walk grid (a
// spot of its way, the place it holds); the network only decides HOW to get there. Its aim and its shots stay with the
// rules of aistyle.h.
// On its way, when it makes no headway towards where it goes for a while, the rules take the keys back for a moment, then
// hand them over again softly. A step towards a wall, a ledge or something deadly gives way to the likeliest safe one
// (aistyle.h stylestepok), and in the air no keys bring it down over a deadly fall; out of a fight the walk grid keeps its
// links (a run-up, a takeoff, a flight, a jump pad: its own keys), and its guard against deadly falls runs after this
// (navbotkeys). With the enemy in sight and his weapon (nearly) ready, it stands still less often than the network says,
// and right after its own shot not for long (measured on the reference player). The strong position "cover and jump" and
// swimming stay with the rules.
// The network is a plain two-layer perceptron (91 inputs, 64 + 64 ReLU units, 9 + 1 outputs) read from a text file
// (botnetfile), computed here by hand: no library.
// botnet 0 (the default) runs none of this: the bots move exactly as before; Classic and Human bots never reach it.

    static bool netload();
    SVAR(botnetfile, "data/botnet/tinyapa-moves.txt"); // the network's file (not saved: later, one file per player model)
    VARFP(botnet, 0, 0, 1, { if(botnet) netload(); }); // read when switched on (or with the config at start), not in a fight

    extern int navbotnetinfo(fpsent *d); // ainavbot.h: what the walk grid tells the network of this bot
    extern bool navbotnetairok(fpsent *d, int move, int strafe, float z); // ainavbot.h: keys in the air that land it safely
    extern bool navbotnetslope(fpsent *d, vec &downhill); // ainavbot.h: on a slope that slides it into a deadly fall

    static const int NETNF = 91, NETNK = 9, NETTICK = 33, NETMEM = 10000, NETSON = 5000;
    static const int NETCOMBAT = 1500;   // ms: the enemy it has in mind seen that recently, it is in a fight (no way to follow)
    static const float NETNEAR = 32;     // u: closer than that to where it goes, it is there (it moves about freely)
    static const float NETPLACE = 96;    // u: as much around the place it holds (the rules keep it within SDODGER of it)
    static const int NETPROGMS = 2000;   // ms: headway checked that often on its way...
    static const float NETPROGMIN = 10;  // u: ...at least that much towards where it goes, else the rules take the keys back
    static const int NETBACK = 500, NETRAMP = 500; // ms the rules keep them, then ms to hand them over again
    // right after its own shot, the enemy in sight with his weapon (nearly) ready again: a stop lasts no longer than that
    // (measured on the reference player in duels: still in 4 % of such moments, such a stop over within 100 ms one time in
    // two, and 300 ms or more after 0.7 % of the shots; the network alone kept the bot still there 3 to 4 times as often:
    // it shoots more often standing still than the player, who shoots on the move, and then stays still)
    static const int NETSTOPSHOT = 150;
    // in a fight, the enemy in sight with his weapon (nearly) ready: a stop is drawn NETDANGER times as often as the network
    // says (measured: the reference player is still in 9.7 % of such moments; the bot the network moved, playing against
    // itself, in 2 to 3 times as many: it creates moments the player does not, then stays still as the player would elsewhere)
    static const float NETDANGER = 0.5f;
    static const char * const netnames[NETNF] =
    {
        "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8", "k_duree",
        "kp0", "kp1", "kp2", "kp3", "kp4", "kp5", "kp6", "kp7", "kp8", "kp_duree", "strafe_chg", "nchg_1s",
        "vit", "vit_av", "vit_cote", "vit_z", "sol", "en_air", "depuis_saut", "mon_rechargement", "tangage",
        "cible", "cible_vue", "cible_pts", "vue_depuis", "perdue_depuis", "dist", "cap_sin", "cap_cos", "haut", "cib_vlat",
        "cib_vrad", "cib_vise", "cib_rechargement", "cib_en_air", "n_vus", "n_vivants",
        "mur0", "mur1", "mur2", "mur3", "mur4", "mur5", "mur6", "mur7",
        "chute0", "chute1", "chute2", "chute3", "chute4", "chute5", "chute6", "chute7",
        "mortel0", "mortel1", "mortel2", "mortel3", "mortel4", "mortel5", "mortel6", "mortel7",
        "ldv", "ldv_s1", "ldv_s0", "ldv_ar", "ldv_av", "duel", "ffa", "equipe", "insta",
        "but", "but_av", "but_cote", "but_dist", "but_haut", "vie_depuis", "son", "son_sin", "son_cos", "son_dist", "son_depuis"
    };
    // the 8 key directions, as aistyle.h stylekeys (forwards, sideways); a key class is 3*(move+1) + (strafe+1), 4 = still
    static const int netkeys8[8][2] = { { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };

    struct netmodel
    {
        bool loaded, failed;
        string file;
        int nin, h1, h2, nk;
        vector<float> mu, sd, w1, b1, w2, b2, wk, bk, wj, bj;
        netmodel() : loaded(false), failed(false), nin(0), h1(0), h2(0), nk(0) { file[0] = 0; }
    };
    static netmodel netm;

    static bool netreadfloats(char *&p, vector<float> &v, int n)
    {
        v.setsize(0);
        loopi(n)
        {
            while(*p && isspace(uchar(*p))) p++;
            if(!*p) return false;
            char *end = NULL;
            v.add(float(strtod(p, &end)));
            if(end == p) return false;
            p = end;
        }
        return true;
    }

    static bool netword(char *&p, const char *w)
    {
        while(*p && isspace(uchar(*p))) p++;
        int n = strlen(w);
        if(strncmp(p, w, n) || (p[n] && !isspace(uchar(p[n])))) return false;
        p += n;
        return true;
    }

    static int netint(char *&p)
    {
        while(*p && isspace(uchar(*p))) p++;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if(end == p) return -1;
        p = end;
        return int(v);
    }

    // the text file written by the offline learning tool (MLP.exporter): header, input names (they must be
    // exactly netnames, in that order), input normalisation, then each layer row by row
    static bool netload()
    {
        if(netm.loaded && !strcmp(netm.file, botnetfile)) return true;
        if(netm.failed && !strcmp(netm.file, botnetfile)) return false;
        netm.loaded = false; netm.failed = true;
        copystring(netm.file, botnetfile);
        char *buf = loadfile(path(botnetfile, true), NULL);
        if(!buf) { conoutf(CON_WARN, "bot network: cannot read %s", botnetfile); return false; }
        char *p = buf;
        bool ok = netword(p, "botnet") && netint(p) == 1;
        if(ok) { netm.nin = netint(p); netm.h1 = netint(p); netm.h2 = netint(p); netm.nk = netint(p); }
        ok = ok && netm.nin == NETNF && netm.nk == NETNK && netm.h1 > 0 && netm.h1 <= 256 && netm.h2 > 0 && netm.h2 <= 256;
        if(ok && netword(p, "noms"))
        {
            loopi(NETNF)
            {
                while(*p && isspace(uchar(*p))) p++;
                int n = strlen(netnames[i]);
                if(strncmp(p, netnames[i], n) || (p[n] && !isspace(uchar(p[n])))) { ok = false; break; }
                p += n;
            }
        }
        else ok = false;
        ok = ok && netword(p, "mu") && netreadfloats(p, netm.mu, NETNF) && netword(p, "sd") && netreadfloats(p, netm.sd, NETNF);
        struct { const char *name; vector<float> *v; int a, b; } layers[8] =
        {
            { "W1", &netm.w1, netm.nin, netm.h1 }, { "b1", &netm.b1, netm.h1, 0 }, { "W2", &netm.w2, netm.h1, netm.h2 }, { "b2", &netm.b2, netm.h2, 0 },
            { "Wk", &netm.wk, netm.h2, netm.nk }, { "bk", &netm.bk, netm.nk, 0 }, { "Wj", &netm.wj, netm.h2, 1 }, { "bj", &netm.bj, 1, 0 }
        };
        loopi(8)
        {
            if(!ok) break;
            ok = netword(p, layers[i].name) && netint(p) == layers[i].a && (!layers[i].b || netint(p) == layers[i].b) &&
                 netreadfloats(p, *layers[i].v, layers[i].a*max(layers[i].b, 1));
        }
        delete[] buf;
        if(!ok) { conoutf(CON_WARN, "bot network: %s is not a network for these inputs", botnetfile); return false; }
        loopi(NETNF) if(netm.sd[i] < 1e-6f) netm.sd[i] = 1;
        netm.loaded = true; netm.failed = false;
        logoutf("bot network: %s (%d inputs, %d + %d units)", botnetfile, netm.nin, netm.h1, netm.h2);
        return true;
    }

    // what one bot keeps for the network: its keys lately, its jumps, where it last saw each enemy, since when it is alive,
    // and the hand-over to the rules on its way (headway, when they took the keys back)
    struct netseen { int cn, life, last, start, nhist; float pos[3], vel[2]; int histt[4]; float histp[4][3]; };
    struct netstate
    {
        aiinfo *owner;
        int life, next, lastt, combo, combostart, prevcombo, prevdur, strafesign, strafet, nchg, chgt[64], airstart, jumpt, lifestart;
        float airz;
        bool wasground, active, rulejump, wasway;
        int netmove, netstrafe;
        int progat, backat;
        float progpos[2], progdir[2];
        vector<netseen> seen;
        void reset(aiinfo *ai, int lf)
        {
            owner = ai; life = lf; next = lastt = 0; combo = 4; combostart = 0; prevcombo = 4; prevdur = 0; strafesign = 0; strafet = 0;
            nchg = 0; airstart = -1; jumpt = -1000000; lifestart = 0; airz = 0; wasground = true; active = rulejump = wasway = false; netmove = netstrafe = 0;
            progat = 0; backat = -1000000; progpos[0] = progpos[1] = progdir[0] = progdir[1] = 0;
            seen.setsize(0);
        }
    };
    static netstate netstates[MAXBOTS];

    static inline float netlf(float ms, float cap) { return logf(1 + clamp(ms, 0.0f, cap)/100.0f); }

    static netseen *netfindseen(netstate &n, int cn)
    {
        loopv(n.seen) if(n.seen[i].cn == cn) return &n.seen[i];
        return NULL;
    }

    static float netreload(fpsent *e)
    {
        if(!e || !e->lastfiredmillis) return 1.5f;
        int gun = e->lastfiredgun >= 0 && e->lastfiredgun < NUMGUNS ? e->lastfiredgun : GUN_RIFLE;
        return min(float(lastmillis - e->lastfiredmillis)/max(guns[gun].attackdelay, 1), 1.5f);
    }

    static bool netclear(const vec &from, const vec &to) // as the offline line of sight: level geometry only, glass does not hide
    {
        vec dir = vec(to).sub(from);
        float mag = dir.magnitude();
        if(mag < 0.5f) return true;
        float hit = raycube(from, dir.mul(1/mag), mag, RAY_SKIPFIRST);
        return hit < 0 || hit >= mag - 0.35f;
    }

    // every tick (network in charge or not): who it sees now (human perception), where it last saw each enemy, and the
    // target: the one it sees nearest to its crosshair, else the one it saw last (forgotten after 10 s or at his death)
    static int netmemory(fpsent *d, netstate &n, int &nvis, int &nalive)
    {
        int t = lastmillis;
        vec f;
        vecfromyawpitch(d->yaw, d->pitch, 1, 0, f);
        int best = -1, bestlast = -1;
        float bestecc = 1e9f;
        nvis = nalive = 0;
        loopvrev(n.seen)
        { // forgotten: he died (the kill feed tells) or left, or 10 s without seeing him
            fpsent *e = getclient(n.seen[i].cn);
            if(!e || e->state != CS_ALIVE || e->lifesequence != n.seen[i].life || t - n.seen[i].last > NETMEM) n.seen.remove(i);
        }
        loopv(players)
        {
            fpsent *e = players[i];
            if(e == d || !targetable(d, e)) continue;
            nalive++;
            aiperceived *p = findperceived(d, e);
            netseen *s = netfindseen(n, e->clientnum);
            bool vis = p && p->visible;
            if(vis)
            {
                if(!s) { s = &n.seen.add(); s->cn = e->clientnum; s->life = e->lifesequence; s->start = t; s->nhist = 0; }
                else if(t - s->last > NETTICK + 20) { s->start = t; s->nhist = 0; } // seen again: a new sighting
                s->last = t;
                // his speed as the player saw it: the move of his eye over the last ~100 ms (3 ticks)
                if(s->nhist == 4) { memmove(s->histt, s->histt + 1, 3*sizeof(int)); memmove(s->histp, s->histp + 1, 3*sizeof(s->histp[0])); s->nhist = 3; }
                s->histt[s->nhist] = t; s->histp[s->nhist][0] = e->o.x; s->histp[s->nhist][1] = e->o.y; s->histp[s->nhist][2] = e->o.z; s->nhist++;
                s->vel[0] = s->vel[1] = 0;
                if(s->nhist >= 2 && s->histt[s->nhist-1] > s->histt[0])
                {
                    float dt = (s->histt[s->nhist-1] - s->histt[0])/1000.0f;
                    float vx = (s->histp[s->nhist-1][0] - s->histp[0][0])/dt, vy = (s->histp[s->nhist-1][1] - s->histp[0][1])/dt;
                    if(vx*vx + vy*vy <= 600*600) { s->vel[0] = vx; s->vel[1] = vy; }
                }
                s->pos[0] = e->o.x; s->pos[1] = e->o.y; s->pos[2] = e->o.z;
                nvis++;
                vec tc = vec(e->o.x, e->o.y, e->o.z - 14*0.3f).sub(d->o);
                float dist = tc.magnitude(), ecc = dist > 1e-3f ? acosf(clamp(tc.dot(f)/dist, -1.0f, 1.0f)) : 0;
                if(ecc < bestecc) { bestecc = ecc; best = e->clientnum; }
            }
            if(s && !vis) s->nhist = 0;
        }
        if(best < 0) loopv(n.seen) if(bestlast < 0 || n.seen[i].last > netfindseen(n, bestlast)->last) bestlast = n.seen[i].cn;
        return best >= 0 ? best : bestlast;
    }

    // the last sound it heard of an enemy it does not see (human perception, aihuman.h noise(): roughly located, as heard),
    // not seen again since, at most NETSON ms ago
    static bool netheard(fpsent *d, netstate &n, vec &at, int &when)
    {
        bool found = false;
        loopv(players)
        {
            fpsent *e = players[i];
            if(e == d || !targetable(d, e)) continue;
            aiperceived *p = findperceived(d, e);
            if(!p || p->lifesequence != e->lifesequence || !p->cuemillis || p->cuesrc == AI_P_SEEN || lastmillis - p->cuemillis > NETSON) continue;
            netseen *s = netfindseen(n, e->clientnum);
            if(s && s->last >= p->cuemillis) continue; // seen again since
            if(!found || p->cuemillis > when) { found = true; when = p->cuemillis; at = p->cuepos; }
        }
        return found;
    }

    // where it is going, out of a fight: the place it holds (holding a place, guarding one), the spot of its way it heads
    // for (the walk grid's, or the waypoints'), or where it stands (nothing to do); that choice stays the rules' and the grid's
    static vec netgoal(fpsent *d, aistyle &s)
    {
        if(s.mvmode == STYLE_MV_HOLD || s.mvmode == STYLE_MV_GUARD) return vec(s.dodgeat[0], s.dodgeat[1], s.dodgeat[2]);
        if(!d->ai->dontmove && !d->ai->spot.iszero()) return d->ai->spot;
        return d->feetpos();
    }

    // the 91 inputs, exactly as the offline extraction computes them from the player's demos
    static void netinputs(fpsent *d, netstate &n, float *x, float *mur, int target, int nvis, int nalive, bool goalok, const vec &goal)
    {
        memset(x, 0, NETNF*sizeof(float));
        int t = lastmillis;
        x[n.combo] = 1;
        x[9] = netlf(t - n.combostart, 3000);
        x[10 + n.prevcombo] = 1;
        x[19] = netlf(n.prevdur, 3000);
        x[20] = netlf(t - n.strafet, 5000);
        int recent = 0;
        loopi(n.nchg) if(t - n.chgt[i] <= 1000) recent++;
        x[21] = recent/10.0f;
        float yr = d->yaw*RAD;
        vec2 fwd(-sinf(yr), cosf(yr)), lat(cosf(yr), sinf(yr)), vh(d->vel.x, d->vel.y);
        x[22] = min(vh.magnitude(), 400.0f)/100.0f;
        x[23] = vh.dot(fwd)/100.0f;
        x[24] = vh.dot(lat)/100.0f;
        x[25] = clamp(d->vel.z + d->falling.z, -400.0f, 400.0f)/100.0f;
        bool ground = d->physstate >= PHYS_SLOPE;
        x[26] = ground ? 1 : 0;
        x[27] = !ground && n.airstart >= 0 ? min((t - n.airstart)/1000.0f, 2.0f) : 0;
        x[28] = netlf(t - n.jumpt, 5000);
        x[29] = netreload(d);
        x[30] = d->pitch/90.0f;
        x[45] = min(nvis, 3)/3.0f;
        x[46] = min(nalive, 5)/5.0f;
        x[42] = 2; x[43] = 1.5f;
        netseen *s = target >= 0 ? netfindseen(n, target) : NULL;
        fpsent *e = target >= 0 ? getclient(target) : NULL;
        vec feet = d->feetpos();
        // walls, steps down and deadly places in the 8 key directions (as aistyle.h stylestepok looks)
        loopk(8)
        {
            vec dir;
            vecfromyawpitch(d->yaw, 0, netkeys8[k][0], netkeys8[k][1], dir);
            dir.z = 0; dir.normalize();
            float w = min(raycube(vec(feet).add(vec(0, 0, 5)), dir, 48), raycube(vec(feet).add(vec(0, 0, 11)), dir, 48));
            w = clamp(w, 0.0f, 48.0f);
            mur[k] = w;
            float reach = clamp(min(24.0f, w - 2), 0.0f, 24.0f);
            vec a = vec(dir).mul(reach).add(feet).add(vec(0, 0, 4));
            float dd = clamp(raycube(a, vec(0, 0, -1), 512), 0.0f, 512.0f);
            bool open = w >= 24;
            int mat = lookupmaterial(vec(a.x, a.y, a.z - dd + 1));
            x[47 + k] = w/48.0f;
            x[55 + k] = open ? min(dd, 64.0f)/64.0f : 0;
            x[63 + k] = open && (isdeadly(mat&MATF_VOLUME) || (mat&MAT_DEATH) || dd >= 512) ? 1 : 0;
        }
        if(s)
        {
            bool vis = e && s->last == t;
            x[31] = 1;
            x[32] = vis ? 1 : 0;
            aiperceived *p = e ? findperceived(d, e) : NULL;
            int pts = 0;
            if(vis && p) loopi(7) if(p->vismask&(1<<i)) pts++;
            x[33] = vis ? pts/7.0f : 0;
            x[34] = vis ? netlf(t - s->start, 3000) : 0;
            x[35] = vis ? 0 : netlf(t - s->last, NETMEM);
            vec pe(s->pos[0], s->pos[1], s->pos[2]), dv = vec(pe).sub(d->o);
            float distc = dv.magnitude();
            x[36] = min(distc, 2000.0f)/500.0f;
            float cap = (-atan2f(dv.x, dv.y)/RAD - d->yaw)*RAD;
            x[37] = sinf(cap); x[38] = cosf(cap);
            x[39] = clamp(dv.z, -300.0f, 300.0f)/100.0f;
            vec2 u(dv.x, dv.y);
            float ul = u.magnitude();
            if(ul > 1e-6f) u.div(ul); else u = vec2(0, 0);
            vec2 perp(-u.y, u.x), v2(s->vel[0], s->vel[1]);
            x[40] = v2.dot(perp)/100.0f;
            x[41] = v2.dot(u)/100.0f;
            if(vis)
            {
                vec ef;
                vecfromyawpitch(e->yaw, e->pitch, 1, 0, ef);
                vec to = vec(d->o.x, d->o.y, d->o.z - 14*0.3f).sub(e->o);
                float tl = to.magnitude();
                float ang = tl > 1e-6f ? acosf(clamp(ef.dot(to)/tl, -1.0f, 1.0f))/RAD : 0;
                x[42] = min(ang/45.0f, 2.0f);
                if(s->nhist >= 2 && s->histt[s->nhist-1] > s->histt[0])
                    x[44] = fabs(s->histp[s->nhist-1][2] - s->histp[0][2])/((s->histt[s->nhist-1] - s->histt[0])/1000.0f) > 40 ? 1 : 0;
            }
            x[43] = e ? netreload(e) : 1.5f;
            // lines of sight to his chest: from its eye, and from its eye 24 u aside, back, forwards (walls allowing)
            vec tg(pe.x, pe.y, pe.z - 14*0.3f);
            x[71] = netclear(d->o, tg) ? 1 : 0;
            static const int shifts[4] = { 6, 2, 4, 0 }; // (0, 1) (0, -1) (-1, 0) (1, 0) in netkeys8
            loopj(4)
            {
                int k = shifts[j];
                vec dir;
                vecfromyawpitch(d->yaw, 0, netkeys8[k][0], netkeys8[k][1], dir);
                dir.z = 0; dir.normalize();
                float sh = clamp(min(24.0f, mur[k] - 2), 0.0f, 24.0f);
                x[72 + j] = netclear(vec(dir).mul(sh).add(d->o), tg) ? 1 : 0;
            }
        }
        int others = 0;
        loopv(players) if(players[i] != d && players[i]->state != CS_SPECTATOR && !isteam(d->team, players[i]->team)) others++;
        bool duel = !m_teammode && others == 1;
        x[76] = duel ? 1 : 0;
        x[77] = !m_teammode && !duel ? 1 : 0;
        x[78] = m_teammode ? 1 : 0;
        x[79] = m_insta ? 1 : 0;
        // where it is going (out of a fight), since when it is alive, the last sound it heard
        if(goalok)
        {
            vec g = vec(goal).sub(feet);
            float gl = sqrtf(g.x*g.x + g.y*g.y);
            x[80] = 1;
            if(gl > 1e-3f) { x[81] = (g.x*fwd.x + g.y*fwd.y)/gl; x[82] = (g.x*lat.x + g.y*lat.y)/gl; }
            x[83] = min(gl, 128.0f)/128.0f;
            x[84] = clamp(g.z, -64.0f, 64.0f)/64.0f;
        }
        x[85] = netlf(t - n.lifestart, 5000);
        vec heard;
        int when = 0;
        if(netheard(d, n, heard, when))
        {
            vec dv = vec(heard).sub(d->o);
            x[86] = 1;
            float cap = (-atan2f(dv.x, dv.y)/RAD - d->yaw)*RAD;
            x[87] = sinf(cap); x[88] = cosf(cap);
            x[89] = min(dv.magnitude(), 1000.0f)/500.0f;
            x[90] = netlf(t - when, NETSON);
        }
    }

    // forward pass: 9 key probabilities and the probability of a jump
    static void netforward(const float *x, float *pk, float &pj)
    {
        static float z[NETNF], a1[256], a2[256];
        loopi(NETNF) z[i] = (x[i] - netm.mu[i])/netm.sd[i];
        const int h1 = netm.h1, h2 = netm.h2;
        loopj(h1) a1[j] = netm.b1[j];
        loopi(NETNF) { float zi = z[i]; if(zi == 0) continue; const float *w = &netm.w1[i*h1]; loopj(h1) a1[j] += zi*w[j]; }
        loopj(h1) if(a1[j] < 0) a1[j] = 0;
        loopj(h2) a2[j] = netm.b2[j];
        loopi(h1) { float ai = a1[i]; if(ai == 0) continue; const float *w = &netm.w2[i*h2]; loopj(h2) a2[j] += ai*w[j]; }
        loopj(h2) if(a2[j] < 0) a2[j] = 0;
        float mx = -1e30f;
        loopk(NETNK)
        {
            float s = netm.bk[k];
            loopi(h2) s += a2[i]*netm.wk[i*NETNK + k];
            pk[k] = s;
            mx = max(mx, s);
        }
        float tot = 0;
        loopk(NETNK) { pk[k] = expf(pk[k] - mx); tot += pk[k]; }
        loopk(NETNK) pk[k] /= tot;
        float sj = netm.bj[0];
        loopi(h2) sj += a2[i]*netm.wj[i];
        pj = 1/(1 + expf(-sj));
    }

    // the rules' "keys held but going nowhere" (a few steps another way: aistyle.h stylesteps) watches their own keys: while
    // the network moves the bot, its stops and its steps about are not a corner it is stuck in (the walk grid still sees a bot
    // that is really stuck, and the headway guard gives the keys back to the rules)
    static inline void netnoescape(fpsent *d, aistyle &s)
    {
        vec feet = d->feetpos();
        s.escat = lastmillis; s.escpos[0] = feet.x; s.escpos[1] = feet.y;
    }

    static bool netstepok(fpsent *d, int pick)
    {
        int mv = pick/3 - 1, st = pick%3 - 1;
        if(!mv && !st) return true;
        vec dir;
        vecfromyawpitch(d->yaw, 0, mv, st, dir);
        dir.z = 0; dir.normalize();
        return stylestepok(d, dir);
    }

    // a stop, or keys whose way does not climb the slope (down it, or along it: it still slides)
    static bool netdownhill(fpsent *d, int pick, const vec &down)
    {
        int mv = pick/3 - 1, st = pick%3 - 1;
        if(!mv && !st) return true;
        vec dir;
        vecfromyawpitch(d->yaw, 0, mv, st, dir);
        dir.z = 0; dir.normalize();
        return dir.dot(down) > -0.2f;
    }

    // its keys lately (every tick, network or rules), its jumps, and its keys and jumps when the network moves it. Kept out
    // of line (AI_HUMAN, as the other Human entry points) so that process() compiles as before around this one call.
    AI_HUMAN void netsteps(fpsent *d)
    {
        if(!botnet || !stylebot(d) || d->state != CS_ALIVE) return;
        int slot = d->clientnum - MAXCLIENTS;
        if(slot < 0 || slot >= MAXBOTS) return;
        aistyle *ps = style(d);
        if(!ps || ps->owner != d->ai || ps->life != d->lifesequence) return;
        netstate &n = netstates[slot];
        bool ground = d->physstate >= PHYS_SLOPE;
        int t = lastmillis;
        if(n.owner != d->ai || n.life != d->lifesequence)
        { // a new life: no keys before, no jump, nothing seen
            n.reset(d->ai, d->lifesequence);
            n.combo = 3*(d->move + 1) + (d->strafe + 1);
            n.combostart = n.strafet = n.lastt = n.lifestart = t;
            n.strafesign = d->strafe;
            n.airstart = ground ? -1 : t;
            n.wasground = ground;
        }
        aistyle &s = *ps;
        // the rules' keys and jump this frame (process() and stylesteps ran first): what a hand-over goes back to
        int rmove = d->move, rstrafe = d->strafe;
        bool rjump = d->jumping;
        // the rules keep the strong position "cover and jump", a few steps out of a corner it was stuck in, a walk off an
        // isolated waypoint, and swimming (and, out of a fight, the links of its way: below)
        bool drive = !s.covermode && lastmillis >= s.escuntil && !iswaypoint(s.walkto) && !d->inwater && netload();
        if(n.active && !drive) n.active = false;
        if(n.active)
        { // between two decisions the keys it chose stay held
            d->move = n.netmove; d->strafe = n.netstrafe;
            d->jumping = n.rulejump && rjump;
            netnoescape(d, s);
        }
        if(n.active && !n.netmove && !n.netstrafe && styleshotmove(d)) n.next = min(n.next, t); // still as its shot is near: it decides now
        if(t < n.next) return;
        n.next = t + NETTICK;
        // the keys it held since the last tick
        int combo = 3*(d->move + 1) + (d->strafe + 1);
        if(combo != n.combo)
        {
            n.prevcombo = n.combo; n.prevdur = t - n.combostart; n.combostart = t; n.combo = combo;
            if(n.nchg == 64) { memmove(n.chgt, n.chgt + 1, 63*sizeof(int)); n.nchg = 63; }
            n.chgt[n.nchg++] = t;
            if(d->strafe && d->strafe != n.strafesign) { n.strafesign = d->strafe; n.strafet = t; }
        }
        if(ground) { n.airstart = -1; n.airz = d->feetpos().z; }
        else if(n.airstart < 0) n.airstart = t;
        if(n.wasground && d->physstate == PHYS_FALL && d->vel.z > 60 && d->vel.z < 200) n.jumpt = t; // a jump (not a jump pad)
        n.wasground = ground;
        n.lastt = t;
        int nvis, nalive, target = netmemory(d, n, nvis, nalive);
        if(!drive) { n.active = false; return; }
        int nav = navbotready() ? navbotnetinfo(d) : 0;
        // in a fight (the enemy it has in mind seen lately): its moves under fire, no way to follow; else where it goes
        netseen *ts = target >= 0 ? netfindseen(n, target) : NULL;
        bool fight = ts && t - ts->last < NETCOMBAT;
        // out of a fight, a link of its way being taken or close ahead (a run-up, a takeoff, a flight, a jump pad): the grid's
        // keys (in a fight its links wait, and a flight already launched keeps the grid's keys in the air: navbotkeys)
        if(!fight && nav&2) { n.active = false; return; }
        vec feet = d->feetpos(), goal = fight ? feet : netgoal(d, s);
        float x[NETNF], mur[8], pk[NETNK], pj;
        netinputs(d, n, x, mur, target, nvis, nalive, !fight, goal);
        netforward(x, pk, pj);
        if(fight && x[32] > 0.5f && x[43] >= 0.7f && pk[4] < 0.999f)
        { // he is in sight and his weapon is (nearly) ready: less still (NETDANGER), and right after its own shot no stop
          // longer than NETSTOPSHOT
            float keep = x[29] < 1 && n.combo == 4 && t - n.combostart >= NETSTOPSHOT ? 0.0f : pk[4]*NETDANGER, scale = (1 - keep)/(1 - pk[4]);
            loopk(NETNK) pk[k] *= scale;
            pk[4] = keep;
        }
        if(fight && pk[4] < 0.999f && styleshotmove(d))
        { // its own shot is near or just gone: it keeps moving (aistyle.h stylesoon: as often as the reference player does)
            float scale = 1/(1 - pk[4]);
            loopk(NETNK) pk[k] *= scale;
            pk[4] = 0;
        }
        // on its way (out of a fight, away from where it goes): its keys as the network draws them; no headway for a while
        // (less than NETPROGMIN u in NETPROGMS ms towards where it goes: the reference player makes less than that in about
        // 2 % of such moments) and the rules take the keys back (NETBACK ms), then hand them over again little by little
        // (NETRAMP ms)
        vec g = vec(goal).sub(feet);
        g.z = 0;
        float gl = g.magnitude();
        bool way = !fight && gl > (s.mvmode == STYLE_MV_HOLD || s.mvmode == STYLE_MV_GUARD ? NETPLACE : NETNEAR);
        if(!n.active) n.wasway = false;
        if(way)
        {
            g.mul(1/gl);
            if(!n.wasway) { n.progat = t; n.progpos[0] = feet.x; n.progpos[1] = feet.y; n.progdir[0] = g.x; n.progdir[1] = g.y; }
            else if(t - n.progat >= NETPROGMS)
            {
                float gone = (feet.x - n.progpos[0])*n.progdir[0] + (feet.y - n.progpos[1])*n.progdir[1];
                if(gone < NETPROGMIN && t - n.backat >= NETBACK + NETRAMP) n.backat = t;
                n.progat = t; n.progpos[0] = feet.x; n.progpos[1] = feet.y; n.progdir[0] = g.x; n.progdir[1] = g.y;
            }
        }
        n.wasway = way;
        int since = t - n.backat;
        float w = !way || since >= NETBACK + NETRAMP ? 1.0f : (since < NETBACK ? 0.0f : float(since - NETBACK)/NETRAMP);
        int pick = 3*(rmove + 1) + (rstrafe + 1);
        bool rules = w < 1 && rndscale(1) >= w;
        if(!rules)
        { // draw the keys; a step towards a wall, a ledge or something deadly gives way to the likeliest safe one (on its
          // way: to the rules' keys, the grid knows the way)
            float r = rndscale(1), acc = 0;
            int draw = NETNK - 1;
            loopk(NETNK) { acc += pk[k]; if(r < acc) { draw = k; break; } }
            if(ground && !netstepok(d, draw))
            {
                if(way && (rmove || rstrafe)) rules = true;
                else
                {
                    bool tried[NETNK];
                    loopk(NETNK) tried[k] = false;
                    loopk(NETNK)
                    {
                        if(netstepok(d, draw)) break;
                        tried[draw] = true;
                        int next = 4;
                        float bestp = -1;
                        loopj(NETNK) if(!tried[j] && pk[j] > bestp) { bestp = pk[j]; next = j; }
                        draw = next;
                    }
                }
            }
            if(!rules) pick = draw;
        }
        // on a slope that slides it into a deadly fall when it stands still (the walk grid's guard does not see that slow
        // slide): no stop and no keys that do not climb it; the likeliest keys that do
        vec down;
        if(!rules && ground && navbotnetslope(d, down) && netdownhill(d, pick, down))
        {
            int best = -1;
            float bestp = -1;
            loopk(NETNK) if(pk[k] > bestp && !netdownhill(d, k, down)) { bestp = pk[k]; best = k; }
            if(best >= 0) pick = best;
        }
        // in the air: no keys that would bring it down over a deadly fall (the walk grid's guard watches its steps on the
        // floor and its jumps, not its keys in the air): then the keys it holds, or none (if none of those lands it safely
        // either, its draw stays)
        if(!ground && !d->inwater && pick != n.combo && !navbotnetairok(d, pick/3 - 1, pick%3 - 1, n.airz))
        {
            if(navbotnetairok(d, n.combo/3 - 1, n.combo%3 - 1, n.airz)) pick = n.combo;
            else if(pick != 4 && navbotnetairok(d, 0, 0, n.airz)) pick = 4;
        }
        n.netmove = pick/3 - 1; n.netstrafe = pick%3 - 1;
        d->move = n.netmove; d->strafe = n.netstrafe;
        // its jumps: the network's own (in a fight, and on its way where the grid lets it jump: no edge, link, jump pad or
        // teleporter near), the rules' when they have the keys or when the grid needs a jump to get it free
        n.rulejump = rules || (nav&8) || (!fight && !navbotready());
        if(n.rulejump) d->jumping = rjump;
        else d->jumping = ground && !d->timeinair && (fight || (nav&4)) && rndscale(1) < pj;
        n.active = true;
        netnoescape(d, s);
    }
