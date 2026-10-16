// Human perception for bots (botperception 1): what a bot may know about the other players.
// Not a standalone header: ai.cpp includes it once, inside namespace ai, right after getaimpos().
// All the Human perception code lives here and in aihumanproc.h; ai.cpp only calls it from one-line
// branches, and fps.cpp / entities.cpp tell it which sounds they play (ai::noise). With botperception 0
// (the default) none of it runs and the bots behave exactly as before.

    // ---- Human perception (botperception 1) ----
    // A bot only knows what a player in its place could know: what is on its
    // screen (botfov across, 16:9 high, one of 7 points of the body in a clear
    // line of sight, big enough to be noticed, less so in the corner of the eye),
    // noticed after a human reaction time; the sounds the game really plays
    // around it (jumps, landings after a long fall, shots, weapon switches,
    // teleports, jump pads, pickups, pain, splashes) as a rough direction and
    // distance, never an exact position; roughly where a hit came from. Once it
    // loses an enemy from sight it guesses where he will come back into view from
    // what it saw or heard (never his real position), and forgets him after
    // botmemory seconds, unless he is the only enemy alive. The flag radar tells
    // where a flag carrier is: where to go, never where to aim. Aim, moves and
    // goals are unchanged. Online, the same for the bots this game plays. botperception 0
    // (the default) runs none of this. botperception 2 adds, on top of it, the aim
    // and a few decisions of a measured reference player (aistyle.h).
    VARP(botperception, 0, 0, 2);
    VARP(botfov, 60, 100, 150);      // degrees across, 16:9 view
    VARP(botreact, 150, 290, 1000);  // ms, median reaction of a skill 100 bot to an unexpected enemy
    VARP(botreflex, 100, 150, 500);  // ms, median reaction when the bot was already looking there for him
    VARP(botmemory, 1, 4, 30);       // seconds before an enemy not seen nor heard is forgotten (never if he is the only one alive)
    VARP(bothearing, 0, 1, 1);
    VARP(botminpx, 1, 5, 40);        // smallest enemy noticed, pixels across on a 1440-line screen with the bot's view

    // a bot the server asks this game to play as Tiny Apa (l4zy/botplay/v1), whatever botperception says
    static bool styleforced[MAXBOTS];
    static bool styleanyforced = false;
    static inline int botmode(fpsent *d) // how this game plays bot d: botperception, or 2 when the server asks for Tiny Apa
    {
        if(styleanyforced)
        {
            int i = d->clientnum - MAXCLIENTS;
            if(i >= 0 && i < MAXBOTS && styleforced[i]) return 2;
        }
        return botperception;
    }

    static const int PSIGHTMS = 16, PGAPMS = 300, PNEARMS = 400, PHOLDMS = 1500, PEXITNODES = 96;
    static const float PECC = 30, PANTICIP = 8, PSPEED = 120, PEXITLEN = 1024;

    // The entry points of human perception are kept out of line, so that the Classic code
    // that calls them compiles exactly as before (same inlining, same float rounding, hence
    // the very same decisions as always with botperception 0).
    #ifdef __GNUC__
    #define AI_HUMAN static __attribute__((noinline))
    #else
    #define AI_HUMAN static
    #endif

    // d->ai: the bots this game plays (all of a local game; online, those the server gives to this game)
    AI_HUMAN bool humanbotcheck(fpsent *d) { return d->ai != NULL; }
    static inline bool humanbot(fpsent *d) { return botmode(d) && humanbotcheck(d); }
    static void pollsounds();

    enum { AI_P_NONE = 0, AI_P_SEEN, AI_P_HEARD, AI_P_HIT };
    static const int AI_P_EXITS = 8;

    struct aiperceived // what one bot knows about one other player
    {
        int cn, lifesequence;
        bool visible, tracking, aware, reachedlost;
        int visstart, vislast, reactat;     // current sighting, last time seen, when it gets noticed (0: nothing pending)
        int cueat, cuesrc, cuemillis;       // a sound or a hit not reacted to yet
        vec cuepos;
        int knownmillis, knownsrc;          // where the bot believes he is, and since when
        vec knownpos, knownvel;
        int radarmillis;                    // flag carrier shown by the radar: where to go, never where to aim
        vec radarpos;
        int numexits, curexit, exitstamp, exitsince, exitarrive; // where he may come back into view
        int exitnode[AI_P_EXITS];
        float exitlen[AI_P_EXITS];
        bool exitchecked[AI_P_EXITS];
        float dose;                         // style bots: attention left for him (aistyle.h)
        int dosemillis;

        aiperceived() { reset(-1, 0); }

        void reset(int n, int life)
        {
            cn = n;
            lifesequence = life;
            visible = tracking = aware = reachedlost = false;
            visstart = vislast = reactat = cueat = cuesrc = cuemillis = knownmillis = knownsrc = radarmillis = 0;
            cuepos = knownpos = knownvel = radarpos = vec(0, 0, 0);
            numexits = exitstamp = exitsince = exitarrive = 0;
            curexit = -1;
            dose = 0;
            dosemillis = 0;
        }
    };

    // what each of our bots perceives, kept outside aiinfo (Classic bots stay laid out in memory
    // exactly as before): one list per bot slot, dropped when the bot changes or respawns
    struct aiperception
    {
        vector<aiperceived> seen;
        aiinfo *owner;
        int life, nextsight;
    };
    static aiperception aiperceptions[MAXBOTS];
    static bool aiperceiving = false;

    // style bots (botperception 2): defined in aistyle.h
    static float stylereactshift(fpsent *d);
    static void styleperceive(fpsent *d);
    static void styleevent(fpsent *d, aiperceived &p, int kind);
    static bool styleforget(fpsent *d, aiperceived &p);
    static bool stylethreat(fpsent *d, fpsent *e);
    static void stylenearmiss(fpsent *e);
    static void stylerespawned(fpsent *e);
    static void styledrop(int slot);
    AI_HUMAN void humankilled(fpsent *d, fpsent *e);
    AI_HUMAN bool stylejump(fpsent *d, aistate &b, bool sequenced, float offz);
    AI_HUMAN int stylepursue(fpsent *d, aistate &b, fpsent *e, float guard, float wander);
    AI_HUMAN bool stylewait(fpsent *d, aistate &b);
    AI_HUMAN int styleinterest(fpsent *d, aistate &b);
    static void stylepursuestate(fpsent *d, aistate &b, fpsent *e);

    static aiperception *perception(fpsent *d)
    {
        int i = d->clientnum - MAXCLIENTS;
        return d->ai && i >= 0 && i < MAXBOTS ? &aiperceptions[i] : NULL;
    }

    static aiperception &claimperception(fpsent *d) // the list of this bot, emptied for a new bot or a new life
    {
        aiperception &s = *perception(d);
        if(s.owner != d->ai || s.life != d->lifesequence)
        {
            s.seen.setsize(0);
            s.owner = d->ai;
            s.life = d->lifesequence;
            s.nextsight = lastmillis + (d->clientnum - MAXCLIENTS)%PSIGHTMS;
        }
        aiperceiving = true;
        return s;
    }

    static float rndgauss()
    {
        float u = (rnd(0x10000) + 1)/65537.0f, v = rnd(0x10000)/65536.0f;
        return sqrtf(-2*logf(u))*cosf(2*PI*v);
    }

    static aiperceived *findperceived(fpsent *d, fpsent *e)
    {
        aiperception *s = e ? perception(d) : NULL;
        if(!s || s->owner != d->ai || s->life != d->lifesequence) return NULL;
        loopv(s->seen) if(s->seen[i].cn == e->clientnum) return &s->seen[i];
        return NULL;
    }

    static aiperceived &getperceived(fpsent *d, fpsent *e)
    {
        aiperception &s = claimperception(d);
        aiperceived *p = findperceived(d, e);
        if(!p) { p = &s.seen.add(); p->reset(e->clientnum, e->lifesequence); }
        else if(p->lifesequence != e->lifesequence) p->reset(e->clientnum, e->lifesequence);
        return *p;
    }

    static int aliveenemies(fpsent *d)
    {
        int n = 0;
        loopv(players) if(players[i] != d && players[i]->state == CS_ALIVE && !isteam(d->team, players[i]->team)) n++;
        return n;
    }

    static bool trackedother(fpsent *d, int cn) // is the bot already following another enemy with its eyes?
    {
        aiperception *s = perception(d);
        if(s) loopv(s->seen)
        {
            aiperceived &p = s->seen[i];
            if(p.cn != cn && p.visible && p.tracking) return true;
        }
        return false;
    }

    // reaction time in ms: shifted log-normal fitted to measured human reactions in game
    // (median ~290 ms for a skill 100 bot, 10 % under ~185, 10 % over ~560), slower in the
    // corner of the eye, for a small or half hidden enemy and when busy with another one;
    // a simple reflex (median botreflex) when the bot was already looking there for him
    static int reactiontime(fpsent *d, bool anticipated, float ecc, float sizeratio, bool busy)
    {
        float skill = clamp(d->skill, 1, 100), base, median, sigma, extra = 0;
        if(anticipated) { base = 100; median = botreflex + (100 - skill)*1.0f; sigma = 0.35f; }
        else
        {
            base = 120; median = botreact + (100 - skill)*2.2f; sigma = 0.75f;
            if(botmode(d) == 2) median += stylereactshift(d);
            extra += 40*clamp(ecc/(botfov*0.5f), 0.0f, 1.0f);
            extra += 80*clamp((3 - sizeratio)*0.5f, 0.0f, 1.0f);
            if(busy) extra += 150;
        }
        float t = base + max(median - base, 10.0f)*expf(sigma*rndgauss()) + extra;
        return int(clamp(t, base, 3000.0f));
    }

    static bool clearview(const vec &from, const vec &to) // glass and the holes of leaves do not hide
    {
        vec dir = vec(to).sub(from);
        float mag = dir.magnitude();
        if(mag < 0.5f) return true;
        float hit = raycube(from, dir.mul(1/mag), mag, RAY_ALPHAPOLY|RAY_SKIPFIRST);
        return hit < 0 || hit >= mag - 0.35f;
    }

    // how much of e bot d really sees: 0 nothing, else how many of 7 points of his body
    // (head, chest and its two sides, hips, knees, feet) are in its view with a clear line
    // of sight, when the visible part is big enough to be noticed: botminpx pixels of a
    // 1440-line screen with this view (same size estimate as the kill feed reaction times),
    // more in the corner of the eye where sight is not sharp
    static int lookat(fpsent *d, fpsent *e, float &ecc, float &sizeratio)
    {
        ecc = 180;
        sizeratio = 0;
        const vec &eye = d->o;
        vec tc = vec(e->o.x, e->o.y, e->o.z - e->eyeheight*0.3f).sub(eye);
        float dist = tc.magnitude();
        if(dist < 1) { ecc = 0; sizeratio = 10; return 7; }
        if(dist > fog) return 0;
        vec f, rt(-cosf(d->yaw*RAD), -sinf(d->yaw*RAD), 0), up;
        vecfromyawpitch(d->yaw, d->pitch, 1, 0, f);
        up.cross(f, rt);
        float zc = tc.dot(f);
        ecc = acosf(clamp(zc/dist, -1.0f, 1.0f))/RAD;
        if(zc < 1) return 0;
        float th = tanf(botfov*0.5f*RAD), tv = th*9/16.0f,
              bodyang = (e->eyeheight + e->aboveeye)/zc,
              minsize = botminpx*2*tv/1440*(1 + ecc/PECC);
        if(sqrtf(0.42f)*bodyang < minsize) return 0;
        int need = max(int(ceilf(7*minsize*minsize/(0.42f*bodyang*bodyang))), 1);
        if(need > 7) return 0;
        static const int band[7] = { 1, 0, 1, 1, 2, 3, 4 };
        float bandz[5] = { e->o.z + 0.5f, e->o.z - e->eyeheight*0.3f, e->o.z - e->eyeheight*0.55f, e->o.z - e->eyeheight*0.8f, e->o.z - e->eyeheight + 1 };
        vec side(tc.y, -tc.x, 0);
        float sl = side.magnitude();
        if(sl > 1e-3f) side.mul(e->radius*0.85f/sl);
        else side = vec(0, 0, 0);
        int seen = 0;
        loopi(7)
        {
            if(seen + 7 - i < need) return 0;
            vec p(e->o.x, e->o.y, bandz[band[i]]);
            if(i == 2) p.sub(side);
            else if(i == 3) p.add(side);
            vec v = vec(p).sub(eye);
            float z = v.dot(f);
            if(z < 1 || fabs(v.dot(rt)) > z*th || fabs(v.dot(up)) > z*tv) continue;
            if(clearview(eye, p)) seen++;
        }
        if(seen < need) return 0;
        sizeratio = sqrtf(0.42f*seen/7)*bodyang/minsize;
        return seen;
    }

    static void sight(fpsent *d, fpsent *e, aiperceived &p, bool busy)
    {
        float ecc, sizeratio;
        if(lookat(d, e, ecc, sizeratio))
        {
            if(!p.visible && (!p.vislast || lastmillis - p.vislast > PGAPMS))
            { // a new sighting, noticed after a reaction time (a reflex if it was looking there for him)
                p.visstart = lastmillis;
                p.tracking = false;
                p.reactat = lastmillis + reactiontime(d, p.aware && ecc <= PANTICIP, ecc, sizeratio, busy);
            }
            p.visible = true;
            p.vislast = lastmillis;
        }
        else
        {
            p.visible = false;
            if(lastmillis - p.vislast > PGAPMS) { p.tracking = false; p.reactat = 0; } // gone: noticed or not, a new sighting next time
        }
    }

    AI_HUMAN void perceive(fpsent *d)
    {
        if(!perception(d)) return;
        pollsounds();
        aiperception &s = claimperception(d);
        if(d->state != CS_ALIVE) return;
        if(botmode(d) == 2) styleperceive(d);
        bool dosight = lastmillis >= s.nextsight;
        if(dosight) s.nextsight = lastmillis + PSIGHTMS;
        loopvrev(s.seen) if(!getclient(s.seen[i].cn)) s.seen.remove(i);
        int alive = aliveenemies(d);
        loopv(players)
        {
            fpsent *e = players[i];
            if(e == d) continue;
            if(!targetable(d, e))
            {
                aiperceived *p = findperceived(d, e);
                if(p && (p->aware || p->visible || p->reactat || p->cueat || p->radarmillis)) p->reset(e->clientnum, e->lifesequence);
                continue;
            }
            aiperceived &p = getperceived(d, e);
            if(dosight) sight(d, e, p, trackedother(d, e->clientnum));
            if(p.visible)
            {
                if(!p.tracking && p.reactat && lastmillis >= p.reactat)
                { // noticed: from now on it follows him with its eyes and knows where he is
                    p.tracking = p.aware = true;
                    p.reactat = p.cueat = 0;
                    if(botmode(d) == 2) styleevent(d, p, 0);
                }
                if(p.tracking)
                {
                    p.knownpos = e->o;
                    p.knownvel = vec(e->vel).add(e->falling);
                    p.knownmillis = lastmillis;
                    p.knownsrc = AI_P_SEEN;
                    p.numexits = 0;
                    p.curexit = -1;
                    p.reachedlost = false;
                }
            }
            if(p.cueat && lastmillis >= p.cueat)
            { // a sound or a hit, reacted to: he is somewhere around there
                p.cueat = 0;
                if(!(p.visible && p.tracking) && p.cuemillis > p.knownmillis)
                {
                    p.aware = true;
                    p.knownpos = p.cuepos;
                    p.knownvel = vec(0, 0, 0);
                    p.knownmillis = p.cuemillis;
                    p.knownsrc = p.cuesrc;
                    p.numexits = 0;
                    p.curexit = -1;
                    p.reachedlost = false;
                    if(botmode(d) == 2) styleevent(d, p, p.cuesrc == AI_P_HIT ? 2 : 1);
                }
            }
            if(p.aware && !p.visible && alive > 1 && (botmode(d) == 2 ? styleforget(d, p) : lastmillis - p.knownmillis > botmemory*1000))
            { // attention shared between several enemies: one not seen nor heard for a while is forgotten
                p.aware = p.tracking = false;
                p.numexits = 0;
                p.curexit = -1;
            }
        }
    }

    static void cue(fpsent *d, fpsent *e, const vec &pos, int src)
    {
        aiperceived &p = getperceived(d, e);
        if(p.visible && p.tracking) return;
        if(!p.cueat) p.cueat = lastmillis + reactiontime(d, false, 0, 3, trackedother(d, e->clientnum));
        p.cuepos = pos;
        p.cuesrc = src;
        p.cuemillis = lastmillis;
    }

    // a sound player src makes at pos, as the game plays it: heard up to the distance where
    // the game's own sound fades out (a landing is faint), roughly located: best in front,
    // worse to the side, behind and far away; never the exact position
    void noise(fpsent *src, int n, const vec &pos)
    {
        if((!botperception && !styleanyforced) || !bothearing || !src || src->state != CS_ALIVE) return;
        float reach = maxsoundradius(n);
        if(n == S_LAND) reach *= 0.6f;
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == src || !d->ai || !botmode(d) || d->state != CS_ALIVE || !targetable(d, src)) continue;
            float dist = d->o.dist(pos);
            if(dist >= reach || dist < 1) continue;
            float farness = dist/reach, yaw, pitch;
            vectoyawpitch(vec(pos).sub(d->o), yaw, pitch);
            float side = fmodf(fabs(yaw - d->yaw), 360);
            if(side > 180) side = 360 - side;
            float spread = (6 + 14*min(side/120, 1.0f))*(1 + farness);
            yaw += spread*rndgauss();
            pitch = clamp(pitch + 0.5f*spread*rndgauss(), -80.0f, 80.0f);
            vec at;
            vecfromyawpitch(yaw, pitch, 1, 0, at);
            at.mul(dist*expf((0.15f + 0.35f*farness)*rndgauss())).add(d->o);
            cue(d, src, at, AI_P_HEARD);
        }
    }

    // Shots, weapon switches and pain are read from the players once per frame (with human
    // perception only), so that the weapon code stays exactly as it is: same sounds as the
    // game plays (guns[].sound, S_WEAPLOAD, S_PAIN1), at the player's position.
    static int polledframe = -1, polledlife[MAXCLIENTS+MAXBOTS], polledfired[MAXCLIENTS+MAXBOTS], polledgun[MAXCLIENTS+MAXBOTS], polledpain[MAXCLIENTS+MAXBOTS];
    static void pollsounds()
    {
        if(polledframe == lastmillis) return;
        polledframe = lastmillis;
        loopv(players)
        {
            fpsent *e = players[i];
            int cn = e->clientnum;
            if(cn < 0 || cn >= MAXCLIENTS+MAXBOTS) continue;
            if(polledlife[cn] != e->lifesequence + 1) // first look at him or a new life: nothing heard yet
            {
                if(polledlife[cn] && (botperception == 2 || styleanyforced) && e->state == CS_ALIVE) stylerespawned(e);
                polledlife[cn] = e->lifesequence + 1;
                polledfired[cn] = e->lastfiredmillis;
                polledgun[cn] = e->gunselect;
                polledpain[cn] = e->lastpain;
                continue;
            }
            bool alive = e->state == CS_ALIVE;
            if(e->lastfiredmillis != polledfired[cn])
            {
                polledfired[cn] = e->lastfiredmillis;
                if(alive && e->lastfiredgun >= 0 && e->lastfiredgun < NUMGUNS) noise(e, guns[e->lastfiredgun].sound, e->o);
                if(alive && (botperception == 2 || styleanyforced)) stylenearmiss(e);
            }
            if(e->gunselect != polledgun[cn])
            {
                polledgun[cn] = e->gunselect;
                if(alive) noise(e, S_WEAPLOAD, e->o);
            }
            if(e->lastpain != polledpain[cn])
            {
                polledpain[cn] = e->lastpain;
                if(alive && e->health > 0) noise(e, S_PAIN1, e->o);
            }
        }
    }

    AI_HUMAN void feelhit(fpsent *d, fpsent *e) // a hit tells roughly where it came from (damage compass, trail)
    {
        vec dir = vec(e->o).sub(d->o);
        float dist = dir.magnitude(), yaw, pitch;
        if(dist < 1) return;
        vectoyawpitch(dir, yaw, pitch);
        yaw += 12*rndgauss();
        pitch = clamp(pitch + 6*rndgauss(), -80.0f, 80.0f);
        vec at;
        vecfromyawpitch(yaw, pitch, 1, 0, at);
        at.mul(dist*expf(0.4f*rndgauss())).add(d->o);
        cue(d, e, at, AI_P_HIT);
    }

    static bool nearleave(const vector<vec> &leave, const vec &o) // a teleporter or a jump pad: a way out of sight
    {
        loopv(leave) if(leave[i].squaredist(o) <= 24*24) return true;
        return false;
    }

    // where e, lost from sight, can come back into view: the first waypoints the bot sees
    // along the paths that leave where it last saw or heard him through waypoints it cannot
    // see, and the teleporters and jump pads on those paths
    static void findexits(fpsent *d, fpsent *e, aiperceived &p)
    {
        p.numexits = 0;
        p.curexit = -1;
        p.exitstamp = p.knownmillis;
        p.exitsince = lastmillis;
        int start = closestwaypoint(vec(p.knownpos).sub(vec(0, 0, e->eyeheight)), SIGHTMIN, true);
        if(!iswaypoint(start)) return;
        static vector<vec> leave;
        leave.setsize(0);
        loopv(entities::ents)
        {
            extentity &ent = *entities::ents[i];
            if((ent.type == TELEPORT || ent.type == JUMPPAD) && ent.o.dist(waypoints[start].o) <= PEXITLEN + 64) leave.add(ent.o);
        }
        static int node[PEXITNODES];
        static float len[PEXITNODES];
        static bool done[PEXITNODES];
        int num = 1;
        node[0] = start;
        len[0] = 0;
        done[0] = false;
        for(;;)
        {
            int cur = -1;
            loopi(num) if(!done[i] && (cur < 0 || len[i] < len[cur])) cur = i;
            if(cur < 0) break;
            done[cur] = true;
            waypoint &w = waypoints[node[cur]];
            if(cur && (clearview(d->o, vec(w.o).add(vec(0, 0, e->eyeheight*0.7f))) || nearleave(leave, w.o)))
            {
                if(p.numexits < AI_P_EXITS)
                {
                    p.exitnode[p.numexits] = node[cur];
                    p.exitlen[p.numexits] = len[cur];
                    p.exitchecked[p.numexits] = false;
                    p.numexits++;
                }
                continue;
            }
            loopj(MAXWAYPOINTLINKS)
            {
                int link = w.links[j];
                if(!link) break;
                if(!iswaypoint(link)) continue;
                float l = len[cur] + w.o.dist(waypoints[link].o);
                if(l > PEXITLEN) continue;
                int found = -1;
                for(int m = 0; m < num; m++) if(node[m] == link) { found = m; break; }
                if(found < 0)
                {
                    if(num >= PEXITNODES) continue;
                    found = num++;
                    node[found] = link;
                    len[found] = l;
                    done[found] = false;
                }
                else if(!done[found] && l < len[found]) len[found] = l;
            }
        }
    }

    // the exit the bot watches: picked at random, more likely a near one, one on his way and
    // one he can already have reached; another one when nobody came out, again and again
    static void guessexit(fpsent *d, fpsent *e, aiperceived &p)
    {
        if(p.exitstamp != p.knownmillis || (!p.numexits && lastmillis - p.exitsince > 1000)) findexits(d, e, p);
        if(!p.numexits) return;
        if(p.curexit >= 0)
        {
            if(lastmillis < max(p.exitarrive, p.exitsince) + PHOLDMS) return;
            p.exitchecked[p.curexit] = true;
            p.curexit = -1;
        }
        int left = 0;
        loopi(p.numexits) if(!p.exitchecked[i]) left++;
        if(!left)
        { // all watched: look for them again from where the bot is now
            findexits(d, e, p);
            if(!p.numexits) return;
        }
        vec v(p.knownvel.x, p.knownvel.y, 0);
        float speed = v.magnitude(), since = (lastmillis - p.knownmillis)/1000.0f, w[AI_P_EXITS], total = 0;
        loopi(p.numexits)
        {
            w[i] = 0;
            if(p.exitchecked[i]) continue;
            vec to = vec(waypoints[p.exitnode[i]].o).sub(p.knownpos);
            to.z = 0;
            float tl = to.magnitude(),
                  dirw = speed > 20 && tl > 1 ? 0.25f + 0.75f*max(v.dot(to)/(speed*tl), 0.0f) : 1.0f,
                  arrive = p.exitlen[i]/max(speed, PSPEED);
            w[i] = dirw*(arrive <= since + 0.5f ? 1.0f : 0.4f)/(1 + p.exitlen[i]/256);
            total += w[i];
        }
        float r = rndscale(total);
        int pick = -1;
        loopi(p.numexits) if(w[i] > 0)
        {
            pick = i;
            if((r -= w[i]) <= 0) break;
        }
        if(pick < 0) return;
        p.curexit = pick;
        p.exitsince = lastmillis;
        p.exitarrive = p.knownmillis + int(1000*p.exitlen[pick]/max(speed, PSPEED));
    }

    // where the bot believes e is now: just after losing him, where it saw or heard him (a
    // little further along his way); later the exit it guesses he will come out from
    static vec guesspos(fpsent *d, fpsent *e, aiperceived &p)
    {
        int since = lastmillis - p.knownmillis;
        if(since < PNEARMS || waypoints.empty())
        {
            vec v(p.knownvel.x, p.knownvel.y, 0);
            float speed = v.magnitude();
            if(speed > 200) v.mul(200/speed);
            return v.mul(min(since, 250)/1000.0f).add(p.knownpos);
        }
        guessexit(d, e, p);
        if(p.curexit >= 0) return vec(waypoints[p.exitnode[p.curexit]].o).add(vec(0, 0, e->eyeheight));
        return p.knownpos;
    }

    static vec guessaimpos(fpsent *d, fpsent *e, const vec &at) // like getaimpos, on the guessed place
    {
        vec o = at;
        if(d->gunselect == GUN_RL) o.z += (e->aboveeye*0.2f)-(0.8f*d->eyeheight);
        else if(d->gunselect != GUN_GL) o.z += (e->aboveeye-e->eyeheight)*0.5f;
        return o;
    }

    // where a bot chasing e goes: where it saw or heard him, then the exit it guesses; for a
    // flag carrier the radar shows, there; false when it knows nothing (forgotten)
    AI_HUMAN bool humanchase(fpsent *d, fpsent *e, vec &pos)
    {
        aiperceived *p = findperceived(d, e);
        if(!p) return false;
        if(p->visible && p->tracking) { pos = e->feetpos(); return true; }
        if(p->radarmillis && lastmillis - p->radarmillis < 2000 && (!p->aware || p->radarmillis > p->knownmillis))
        {
            pos = vec(p->radarpos).sub(vec(0, 0, e->eyeheight));
            return true;
        }
        if(!p->aware) return false;
        vec last = vec(p->knownpos).sub(vec(0, 0, e->eyeheight));
        if(!p->reachedlost && d->feetpos().squaredist(last) > CLOSEDIST*CLOSEDIST) { pos = last; return true; }
        p->reachedlost = true;
        guessexit(d, e, *p);
        pos = p->curexit >= 0 ? waypoints[p->exitnode[p->curexit]].o : last;
        return true;
    }

    AI_HUMAN bool humanviolence(fpsent *d, aistate &b, fpsent *e, int pursue)
    {
        aiperceived &p = getperceived(d, e);
        if(!p.aware)
        { // never seen nor heard: only the flag code asks this, about a flag carrier the radar shows
            p.radarmillis = lastmillis;
            p.radarpos = e->o;
            if(pursue)
            {
                if((b.targtype != AI_T_AFFINITY || !(pursue%2)) && makeroute(d, b, vec(p.radarpos).sub(vec(0, 0, e->eyeheight))))
                    d->ai->switchstate(b, AI_S_PURSUE, AI_T_PLAYER, e->clientnum);
                else if(pursue >= 3) return false;
            }
            return true;
        }
        if(pursue)
        {
            vec at;
            if(!humanchase(d, e, at)) return false;
            if((b.targtype != AI_T_AFFINITY || !(pursue%2)) && makeroute(d, b, at))
                d->ai->switchstate(b, AI_S_PURSUE, AI_T_PLAYER, e->clientnum);
            else if(pursue >= 3) return false;
            if(botmode(d) == 2) stylepursuestate(d, b, e);
        }
        if(d->ai->enemy != e->clientnum)
        {
            d->ai->enemyseen = d->ai->enemymillis = lastmillis;
            d->ai->enemy = e->clientnum;
        }
        return true;
    }

    // the enemy the bot goes for: the nearest one it sees, else the nearest one it remembers or heard
    AI_HUMAN bool targethuman(fpsent *d, aistate &b, int pursue, float mindist)
    {
        static vector<fpsent *> tried;
        tried.setsize(0);
        vec dp = d->headpos();
        for(;;)
        {
            fpsent *t = NULL;
            float best = 1e30f;
            aiperception *s = perception(d);
            if(s) loopv(s->seen)
            {
                aiperceived &p = s->seen[i];
                if(!p.aware) continue;
                fpsent *e = getclient(p.cn);
                if(!e || !targetable(d, e) || tried.find(e) >= 0) continue;
                bool insight = p.visible && p.tracking;
                float v = (insight ? e->o : p.knownpos).squaredist(dp);
                if(mindist > 0 && v > mindist) continue;
                if(!insight) v += 1e12f;
                else if(botmode(d) == 2 && stylethreat(d, e)) v *= 0.25f;
                if(v < best) { best = v; t = e; }
            }
            if(!t) return false;
            if(violence(d, b, t, pursue)) return true;
            tried.add(t);
        }
    }

    AI_HUMAN int humanpursue(fpsent *d, aistate &b, fpsent *e, float guard, float wander) // chase what it knows
    {
        if(botmode(d) == 2) return stylepursue(d, b, e, guard, wander);
        vec pos;
        return humanchase(d, e, pos) && patrol(d, b, pos, guard, wander) ? 1 : 0;
    }

    AI_HUMAN bool humandamaged(fpsent *d, fpsent *e) // a hit tells roughly where it came from, and nothing to team mates
    {
        if(d->ai && e && canmove(d) && targetable(d, e)) feelhit(d, e);
        return true;
    }

    AI_HUMAN void forgetall() // back to Classic: what the bots perceived is dropped
    {
        loopi(MAXBOTS)
        {
            aiperceptions[i].seen.setsize(0);
            aiperceptions[i].owner = NULL;
        }
        aiperceiving = false;
    }

    AI_HUMAN vec humanrangepos(fpsent *d, fpsent *e, const vec &ep) // weapon choice: from what it knows
    {
        aiperceived *p = findperceived(d, e);
        return p && p->aware && !(p->visible && p->tracking) ? p->knownpos : ep;
    }

    // ---- Tiny Apa names and servers (botperception 2) ----
    // A bot this game plays as Tiny Apa (every bot of a local game; online, the bots the server gives to this game) is
    // shown as "Tiny Apa" ("Tiny Apa 2", "Tiny Apa 3"... when there are several) in the scoreboard, the kill feed and the
    // messages. A server of the clan (handshake done) names it so for everyone: this game tells it how it plays each of
    // its bots (l4zy/botstyle/v1) and shows the server's name; elsewhere the name is shown on this screen only, and the
    // server's own name ("bot") comes back as soon as the bot plays Classic or goes to another player's game.
    static string stylesrvname[MAXBOTS];
    static int stylenum[MAXBOTS], stylesent[MAXBOTS];
    static bool stylenaming = false;

    static int styleslot(fpsent *d)
    {
        int i = d->clientnum - MAXCLIENTS;
        return d->aitype != AI_NONE && i >= 0 && i < MAXBOTS ? i : -1;
    }

    static bool stylename(const char *s) { return !strncmp(s, "Tiny Apa", 8) && (!s[8] || s[8] == ' '); }

    // a name given by a clan server: "Tiny Apa 2" arrives as "TinyApa2" (every client drops the spaces of names);
    // shown again as "Tiny Apa 2", with the server's number, the same on every L4ZY screen
    static bool stylesrvta(const char *s)
    {
        if(strncmp(s, "TinyApa", 7) && strncmp(s, "Tiny Apa", 8)) return false;
        for(s += s[4] == ' ' ? 8 : 7; *s == ' '; s++);
        for(; *s; s++) if(*s < '0' || *s > '9') return false;
        return true;
    }

    static void styleshown(char *dst, const char *srv) // the name to show for a server name
    {
        if(!stylesrvta(srv)) { copystring(dst, srv, MAXNAMELEN+1); return; }
        const char *num = srv + (srv[4] == ' ' ? 8 : 7);
        while(*num == ' ') num++;
        if(*num && strcmp(num, "1")) nformatstring(dst, MAXNAMELEN+1, "Tiny Apa %s", num);
        else copystring(dst, "Tiny Apa", MAXNAMELEN+1);
    }

    static void stylenumname(char *dst, int num)
    {
        if(num > 1) nformatstring(dst, MAXNAMELEN+1, "Tiny Apa %d", num);
        else copystring(dst, "Tiny Apa", MAXNAMELEN+1);
    }

    static int stylepick(fpsent *d) // the smallest number no other bot shows
    {
        for(int num = 1;; num++)
        {
            string cand;
            stylenumname(cand, num);
            bool used = false;
            loopv(players) if(players[i] != d && players[i]->aitype != AI_NONE && !strcmp(players[i]->name, cand)) { used = true; break; }
            if(!used) return num;
        }
    }

    static void styleforcedcheck()
    {
        styleanyforced = false;
        loopi(MAXBOTS) if(styleforced[i]) { styleanyforced = true; break; }
    }

    AI_HUMAN const char *stylebotname(fpsent *d, const char *name, int ocn) // from ai::init (N_INITAI): the name to show
    {
        int slot = styleslot(d);
        if(slot < 0) return name;
        if(!d->name[0] || d->ownernum != ocn)
        { // a new bot, or it changes hands: what it perceived and its style start again
            stylenum[slot] = 0;
            aiperceptions[slot].seen.setsize(0);
            aiperceptions[slot].owner = NULL;
            styledrop(slot);
        }
        copystring(stylesrvname[slot], name, MAXNAMELEN+1);
        stylesent[slot] = 0;          // a new record from the server: tell it again how this game plays the bot
        styleforced[slot] = false;    // the server repeats l4zy/botplay/v1 after each N_INITAI of its Tiny Apa bots
        styleforcedcheck();
        static string shown;
        if(stylesrvta(name) || botperception != 2 || ocn < 0 || ocn != player1->clientnum)
        {
            stylenum[slot] = 0;
            styleshown(shown, name);
            return shown;
        }
        if(!stylenum[slot]) stylenum[slot] = stylepick(d);
        stylenaming = true;
        stylenumname(shown, stylenum[slot]);
        return shown;
    }

    AI_HUMAN void stylenames() // each frame while Tiny Apa is in use, a bot still carries the name, or on a clan server
    {
        bool any = false, srv = game::tinyapaserver(), present[MAXBOTS];
        loopi(MAXBOTS) present[i] = false;
        loopv(players)
        {
            fpsent *d = players[i];
            int slot = styleslot(d);
            if(slot < 0) continue;
            present[slot] = true;
            bool want = d->ai && botmode(d) == 2;
            if(want && !stylesrvta(stylesrvname[slot]))
            {
                if(!stylenum[slot]) { stylenum[slot] = stylepick(d); stylenumname(d->name, stylenum[slot]); }
            }
            else if(stylenum[slot]) { stylenum[slot] = 0; styleshown(d->name, stylesrvname[slot]); }
            if(stylenum[slot]) any = true;
            if(srv && d->ai && !styleforced[slot])
            { // tell the server how this game plays its bot, once per record: it names it for everyone
                int st = want ? 2 : 0;
                if(stylesrvta(stylesrvname[slot]) != (st == 2) && stylesent[slot] != st + 1)
                {
                    defformatstring(msg, "%s %d %d", BOTSTYLE_CMD, d->clientnum, st);
                    addmsg(N_SERVCMD, "rs", msg);
                    stylesent[slot] = st + 1;
                }
            }
        }
        loopi(MAXBOTS) if(styleforced[i] && !present[i]) styleforced[i] = false;
        styleforcedcheck();
        stylenaming = any;
    }

    void stylebotplay(int cn, int st) // l4zy/botplay/v1 from the server: play this bot as Tiny Apa, whatever the setting
    {
        fpsent *d = getclient(cn);
        int slot = d ? styleslot(d) : -1;
        if(slot < 0) return;
        styleforced[slot] = st == 2;
        styleforcedcheck();
    }

    bool tinyapanamed(fpsent *d, const char *name) // colorname: a Tiny Apa name is unique, it needs no [cn]
    {
        return styleslot(d) >= 0 && stylename(name);
    }
