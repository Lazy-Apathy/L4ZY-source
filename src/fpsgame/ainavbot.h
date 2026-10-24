// ainavbot.h: the Human bots (and so Tiny Apa) find their way on the walk grid of ainav.cpp instead of the waypoints.
// Not a standalone header: ainav.cpp includes it at its end, inside namespace ai (it uses the grid itself). ai.cpp calls
// it from one-line branches taken only by the bots this game plays as Human or Tiny Apa (botperception 1 or 2, or a bot
// the server asks to play as Tiny Apa): Classic bots never come here.
//
// What the bots want does not change (items, an enemy, a flag, a place to wander to: ai.cpp, aihuman.h, aistyle.h);
// only the way there does: a path on the grid (hierarchical A*) to the node nearest that place, followed in straight
// lines wherever the floor allows (it heads for the furthest node of its path it can walk to straight), each link taken
// the way the grid found it with the game's own physics (steps and slopes walked, never jumped; drops; jumps onto a
// ledge, over a gap or a low obstacle, with a run-up when a gap needs speed; jump pads and their air control;
// teleporters; swimming up out of the water). In the air its keys are corrected so that it lands where the grid said.
// What it goes for can change at any moment (an enemy seen or heard, damage, an item taken, a flag moving): the base
// game's new choice is a new path at once, from where it stands; only a flight already launched (a jump, a drop, a jump
// pad) ends first, its view staying its own while it fights (aim), the keys in the air then chosen for that view.
// Players it perceives are walked round; stuck, it finds a new path (never a suicide). While the grid is not ready
// (being made or loaded, a coop edit being joined), the bots keep the waypoints as before; a path already found is
// still followed meanwhile, and a new one is made once the grid has changed. Nothing is sent online.
// Deadly falls (a death pit, lava, out of the world: botnavsafe 1, the default): no step, no run, no jump of its own off
// such an edge (its course on the floor is foreseen a moment ahead; it slows down, steps aside or brakes in time); a link
// of its path across such a fall is taken only at the speed and in the line the grid simulated, looking along it, and
// it flies for a spot a little inside the floor it lands on, not for the very edge.

    struct navstep
    {
        vec o;                      // feet on that node (a jump pad or a teleporter: its floor)
        int node;
        uchar type, yaw, t0, t1;    // the link that reaches it from the step before (the first step: a walk)
        uchar approach;             // a step behind the node a drop or a jump starts from: it comes running the way the grid simulated it
        uchar risky;                // that link flies over a deadly fall (botnavsafe): taken only as the grid simulated it
    };
    enum { NBP_WALK = 0, NBP_TAKEOFF, NBP_AIR };
    static const int NAVB_LOOK = 100;        // ms between two looks for the furthest node it can walk to straight
    static const float NAVB_SMOOTH = 256;    // u, furthest node it heads for straight
    static const int NAVB_STEPS = 48;        // steps of its path looked at for that
    static const int NAVB_STUCK = 1200;      // ms with keys held and less than NAVB_HEADWAY u gone: stuck
    static const float NAVB_HEADWAY = 12;
    static const float NAVB_RUNUP = 28;      // u of run-up before a gap when it comes too slow

    struct navbot
    {
        aiinfo *owner;
        int life;
        int goal, gen;              // goal node (the bot's route holds -2 - goal while this path is its way), grid generation
        vec goalpos;                // where it was asked to go (the path is made again from there when needed)
        vector<navstep> path;
        int aim, aimat;             // the step it heads for, when it last looked for a further one
        vec aimfrom;                // where it stood when it chose to head for it
        int phase, phaseat, launchat, runups; // a link other than a walk: path[aim] is reached from path[aim-1] by it
        vec takeoff, runupat, launchvel; // launchvel: its speed when a jump pad threw it
        vec tkpos; int tkat;        // where it stood lately while taking off, and since when (blocked on its way to the edge)
        bool runup, jumped, ready, flying; // flying: a drop that crosses a void to a floor about as high (simulated at full speed)
        bool leap, commit;          // over a deadly fall (botnavsafe): a drop across taken with a jump off the edge; on its way to the edge (decided 10 u before)
        bool pending;               // a new goal came in the middle of a link: the new path is made once it is down
        bool wandering; int wanderat; // its path is a short walk of its own (nothing else to do), since then
        int badfrom[4], badto[4], badat[4];   // links that did not take it where they should, lately (navbotbadlink)
        bool badfam[4];             // ... refused at their start (no room, not fast enough): the links like it too (navlinklike)
        int progat, stuck, replanat, wanderfail, fails;
        vec progpos;
        int node;                   // node it stood on lately
        uint seed;                  // its own small differences between ways of about the same length (navroutenodes)
        int routes;
        void reset(aiinfo *ai, int l)
        {
            owner = ai; life = l;
            goal = gen = -1;
            goalpos = vec(0, 0, 0);
            path.setsize(0);
            aim = aimat = 0;
            aimfrom = vec(0, 0, 0);
            phase = NBP_WALK; phaseat = launchat = runups = 0;
            takeoff = runupat = launchvel = tkpos = vec(0, 0, 0); tkat = 0;
            runup = jumped = ready = pending = flying = wandering = leap = commit = false; wanderat = 0;
            loopi(4) { badfrom[i] = badto[i] = -1; badat[i] = 0; badfam[i] = false; }
            progat = stuck = replanat = wanderfail = fails = 0;
            progpos = vec(0, 0, 0);
            node = -1;
            routes = 0;
        }
    };
    static navbot navbots[MAXBOTS];
    static uint navbotseeds = 0x9E3779B9u;

    static navbot *navbotof(fpsent *d) // this bot's way (a new bot or a new life starts afresh)
    {
        if(!d || !d->ai) return NULL;
        int i = d->clientnum - MAXCLIENTS;
        if(i < 0 || i >= MAXBOTS) return NULL;
        navbot &nb = navbots[i];
        if(nb.owner != d->ai || nb.life != d->lifesequence)
        {
            nb.reset(d->ai, d->lifesequence);
            navbotseeds = navbotseeds*1664525u + 1013904223u;
            nb.seed = navbotseeds ^ uint(d->clientnum)*2654435761u;
        }
        return &nb;
    }
    // a flight already launched (a jump, a drop, a jump pad) goes on to its landing, even if the base game cleared the
    // bot's route meanwhile (its waiting state does, each time it thinks)
    static inline bool navbotinflight(const navbot &nb) { return nb.phase == NBP_AIR && nb.goal >= 0 && nb.path.inrange(nb.aim) && nb.aim >= 1; }
    static inline bool navbothaspath(fpsent *d, const navbot &nb)
    {
        if(navbotinflight(nb)) return true;
        return nb.path.length() && nb.goal >= 0 && d->ai->route.length() == 1 && d->ai->route[0] == -2 - nb.goal;
    }
    static void navbotdrop(fpsent *d, navbot &nb) // its grid way is given up
    {
        nb.path.setsize(0);
        nb.phase = NBP_WALK;
        nb.goal = -1;
        if(d->ai->route.length() == 1 && d->ai->route[0] < -1) d->ai->route.setsize(0);
    }

    // how the Human and Tiny Apa bots find their way: 1 the walk grid (default), 0 the waypoints as before (the grid is
    // then not made); a change takes effect at once (their ways are made again)
    static void navbotswitched()
    {
        loopv(players)
        {
            fpsent *d = players[i];
            if(!d->ai) continue;
            int s = d->clientnum - MAXCLIENTS;
            if(s >= 0 && s < MAXBOTS && navbots[s].owner == d->ai) { navbots[s].path.setsize(0); navbots[s].goal = -1; navbots[s].phase = NBP_WALK; navbots[s].pending = false; }
            if(d->ai->route.length() == 1 && d->ai->route[0] < -1) d->ai->route.setsize(0);
        }
    }
    VARFP(botnavgrid, 0, 1, 1, navbotswitched());
    // the bots on the grid keep clear of deadly falls they did not plan (a death pit, lava, the void): 1 (default), 0 as
    // they did before (Nav-2)
    VARP(botnavsafe, 0, 1, 1);
    bool navbotready() { return botnavgrid && navready(); }

    static inline bool navbotonfloor(const fpsent *d) { return d->physstate >= PHYS_SLOPE && !d->timeinair; }
    // down on a floor for good, as the grid's simulations tell it (not while the physics lifts the feet onto a ledge)
    static inline bool navbotground(const fpsent *d) { return (d->physstate == PHYS_FLOOR || d->physstate == PHYS_SLOPE || d->physstate == PHYS_STEP_DOWN) && !d->timeinair; }
    static inline float navbotyawof(float dx, float dy) { return -atan2f(dx, dy)/RAD; }
    static inline float navbotwrap(float a) { while(a > 180) a -= 360; while(a < -180) a += 360; return a; }
    static inline float navbotdist2(const vec &a, const vec &b) { float dx = a.x - b.x, dy = a.y - b.y; return dx*dx + dy*dy; }
    static inline bool navbotlinktype(int t) { return t != NAV_WALK; }
    static inline vec navbotdir(const navstep &st) { vec dir; vecfromyawpitch(navbyteyaw(st.yaw), 0, 1, 0, dir); return dir; }

    // ---- nodes near a place
    // the node at feet: in its column or the next ones, rather a floor under them than over them
    static int navbotnodeat(const vec &feet, float maxd = 10, float up = 6, float down = 12)
    {
        int col = navcolof(feet.x, feet.y);
        if(col < 0) return -1;
        int cx = col%navcolsw, cy = col/navcolsw, r = int(maxd/navcellsize) + 1, best = -1;
        float bd = 1e16f;
        for(int y = max(cy - r, 0); y <= min(cy + r, navcolsw - 1); y++)
            for(int x = max(cx - r, 0); x <= min(cx + r, navcolsw - 1); x++)
                for(int k = navcolhead(y*navcolsw + x); k >= 0; k = navnodes[k].next)
                {
                    const navnode &n = navnodes[k];
                    float dz = n.o.z - feet.z;
                    if(dz > up || dz < -down) continue;
                    float d2 = navbotdist2(n.o, feet);
                    if(d2 > maxd*maxd) continue;
                    d2 += dz*dz*(dz > 0 ? 4 : 1);
                    if(d2 < bd) { bd = d2; best = k; }
                }
        return best;
    }
    // the node for a place a bot wants to go to (an item floats over its floor, a player stands on it): the nearest
    // one, rather under it, and rather in the biggest part of the grid (where it can go and come back)
    static int navbotgoalnode(const vec &pos)
    {
        int col = navcolof(pos.x, pos.y);
        if(col < 0) return -1;
        int cx = col%navcolsw, cy = col/navcolsw, r = int(40/navcellsize) + 1, best = -1;
        float bd = 1e16f;
        for(int y = max(cy - r, 0); y <= min(cy + r, navcolsw - 1); y++)
            for(int x = max(cx - r, 0); x <= min(cx + r, navcolsw - 1); x++)
                for(int k = navcolhead(y*navcolsw + x); k >= 0; k = navnodes[k].next)
                {
                    const navnode &n = navnodes[k];
                    if(n.flags&NAVF_DEAD) continue;
                    float dz = n.o.z - pos.z;
                    if(dz > 24 || dz < -80) continue;
                    float d2 = navbotdist2(n.o, pos);
                    if(d2 > 40*40) continue;
                    d2 += dz*dz*(dz > 0 ? 4 : 0.25f);
                    if(!(navcomp.inrange(k) && navcomp[k] == navbigcomp)) d2 = d2*2 + 64;
                    if(d2 < bd) { bd = d2; best = k; }
                }
        return best;
    }
    bool navnodepos(int n, vec &pos)
    {
        if(!navready() || !navnodes.inrange(n) || navnodes[n].flags&NAVF_DEAD) return false;
        pos = navnodes[n].o;
        return true;
    }
    // a node of the grid from..maxd u away from `from`, in the part of the grid where `from` stands (-1: none found)
    int navrandomnode(const vec &from, float mind, float maxd)
    {
        if(!navready() || navnodes.length() <= 1) return -1;
        int s = navnearest(from, 16, 24), comp = s >= 0 && navcomp.inrange(s) ? navcomp[s] : navbigcomp, n = navnodes.length();
        loopi(48)
        {
            int g = rnd(n);
            const navnode &nn = navnodes[g];
            if(nn.flags&(NAVF_DEAD|NAVF_VIRTUAL) || !navcomp.inrange(g) || navcomp[g] != comp) continue;
            float d2 = nn.o.squaredist(from);
            if(d2 < mind*mind || d2 > maxd*maxd) continue;
            return g;
        }
        return -1;
    }

    // ---- the path: hierarchical A* from where it stands to the goal node, with the links it takes
    static const navlink *navbotlinkof(int from, int to)
    {
        const navnode &n = navnodes[from];
        const navlink *best = NULL;
        loopi(n.nlinks)
        {
            const navlink &l = navlinks[n.link0 + i];
            if(l.to != to || !navlinkvalid(l)) continue;
            if(!best || l.cost < best->cost) best = &l;
        }
        return best;
    }
    static int navbotstart(fpsent *d, navbot &nb)
    {
        vec feet = d->feetpos();
        int s = navbotnodeat(feet);
        if(s < 0 && navnodes.inrange(nb.node) && !(navnodes[nb.node].flags&NAVF_DEAD) && navbotdist2(navnodes[nb.node].o, feet) < 24*24 && fabs(navnodes[nb.node].o.z - feet.z) < 24) s = nb.node;
        if(s < 0 && !navbotonfloor(d)) s = navbotnodeat(feet, 16, 4, 96); // in the air: the floor it falls to
        return s;
    }
    static void navbotwalkfrom(fpsent *d, navbot &nb) // a new way, or a link done: it walks on from where it stands
    {
        nb.phase = NBP_WALK;
        nb.aimat = 0;
        nb.aimfrom = d->feetpos();
        nb.progat = lastmillis; nb.progpos = d->feetpos();
    }
    static bool navbotbadway(const navbot &nb, const vector<navstep> &path);
    static bool navbotavoid(const navbot &nb);
    static bool navbotpitunder(const vec &a, const vec &b);
    static void navbotbadpair(navbot &nb, int from, int to, bool fam);
    static bool navbotline(const vec &from, int cur, int to);
    // a drop, a jump onto a ledge or over a gap was simulated running at full speed along the link from its node: on the way there the
    // bot passes a step behind that node (12 to 24 u back along the link, walked straight), unless it already comes that way
    static void navbotapproaches(navbot &nb)
    {
        for(int k = 2; k < nb.path.length(); k++)
        {
            int type = nb.path[k].type;
            if(type != NAV_DROP && type != NAV_JUMP && type != NAV_GAP) continue;
            const navstep &a = nb.path[k-1], &prev = nb.path[k-2];
            vec dir = navbotdir(nb.path[k]);
            float pa = (a.o.x - prev.o.x)*dir.x + (a.o.y - prev.o.y)*dir.y, ps = fabs((a.o.x - prev.o.x)*dir.y - (a.o.y - prev.o.y)*dir.x);
            if(pa >= 10 && ps < 4) continue; // it already comes running along the link
            for(float back = 24; back >= 12; back -= 6)
            {
                vec p = vec(dir).mul(-back).add(a.o);
                int n = navbotnodeat(p, 4, 4, 6);
                if(n < 0 || n == a.node || n == prev.node || navnodes[n].flags&(NAVF_DEAD|NAVF_WATER)) continue;
                if(!navbotline(navnodes[n].o, n, a.node) || !navbotline(prev.o, prev.node, n)) continue;
                navstep st;
                st.o = navnodes[n].o; st.node = n;
                st.type = NAV_WALK; st.yaw = st.t0 = st.t1 = 0;
                st.approach = 1; st.risky = 0;
                nb.path.insert(k-1, st);
                k++;
                break;
            }
        }
    }
    // parts of the grid with no way from one to the other (strongly connected parts: from anywhere in one, the same
    // places can be reached), for the grid as it is now: a search known to fail is not made again
    static navset64 navbotnoway;
    static int navbotnowaygen = -1;
    static inline ullong navbotnowaykey(int s, int g) { return navcomp.inrange(s) && navcomp.inrange(g) ? (ullong(uint(navcomp[s])) << 32) | uint(navcomp[g]) : ~0ULL; }
    // a jump over a gap or a drop across a void needs its full speed along the link: room behind its node for a run-up
    // (walked straight, as navbotbegin looks for it), or a way that comes to that node running along it already (or, the
    // first link of a path, its own speed that way); else it would only refuse it at the edge
    static bool navbotcanrun(const fpsent *d, const navstep *prev, const navstep &a, const navstep &to)
    {
        if(to.type != NAV_GAP && !(to.type == NAV_DROP && navbotdist2(to.o, a.o) >= 32*32 && to.o.z >= a.o.z - 24)) return true;
        vec dir = navbotdir(to);
        if(prev)
        {
            float pa = (a.o.x - prev->o.x)*dir.x + (a.o.y - prev->o.y)*dir.y, ps = fabs((a.o.x - prev->o.x)*dir.y - (a.o.y - prev->o.y)*dir.x);
            if(pa >= (to.risky ? 20 : 10) && ps < 4) return true;
        }
        else if(d->vel.x*dir.x + d->vel.y*dir.y >= (to.risky ? 100 : 60)) return true;
        for(float back = NAVB_RUNUP; back >= (to.risky ? 20 : 12); back -= 8) // over a deadly fall: a longer run-up (100 u/s 10 u before the node)
        {
            vec r = vec(dir).mul(-back).add(a.o);
            int n = navbotnodeat(r, 5, 4, 6);
            if(n >= 0 && navbotline(a.o, a.node, n)) return true;
        }
        return false;
    }
    static bool navbotmake(fpsent *d, navbot &nb, int s, int g)
    {
        static vector<int> nodes;
        nodes.setsize(0);
        float cost = 0;
        bool ok;
        if(navbotnowaygen != navgeneration) { navbotnoway.clear(); navbotnowaygen = navgeneration; }
        ullong noway = navbotnowaykey(s, g);
        // already on that node: no way to make, as with the waypoints (the base game's route() fails on the waypoint it
        // stands on, and the base game then walks round its spot: a guard or a watch does not stand still there)
        if(s == g) return false;
        if(noway != ~0ULL && navcomp[s] != navcomp[g] && navbotnoway.has(noway)) ok = false;
        else
        {
            navrouteseed = navvariety ? (nb.seed ^ uint(nb.routes)*0x632BE5ABu) | 1 : 0;
            bool avoiding = navbotavoid(nb); // the links that just failed it: the search goes round them (another way)
            ok = navroutenodes(s, g, &nodes, cost);
            navavoidn = 0;
            navrouteseed = 0;
            if(!ok && !avoiding && noway != ~0ULL && navcomp[s] != navcomp[g]) navbotnoway.add(noway);
        }
        // no way there: the way it has goes on (the base game is told there is none, and picks what next)
        if(!ok || nodes.empty()) return false;
        nb.routes++;
        static vector<navstep> steps;
        steps.setsize(0);
        loopvrev(nodes)
        {
            navstep &st = steps.add();
            st.node = nodes[i];
            st.o = navnodes[nodes[i]].o;
            st.type = NAV_WALK; st.yaw = st.t0 = st.t1 = 0;
            st.approach = 0; st.risky = 0;
            if(steps.length() > 1)
            {
                const navlink *l = navbotlinkof(steps[steps.length() - 2].node, st.node);
                if(!l) return false;
                st.type = l->type; st.yaw = l->yaw; st.t0 = l->t0; st.t1 = l->t1;
            }
        }
        if(botnavsafe) for(int k = 1; k < steps.length(); k++)
        {
            int t = steps[k].type;
            if(t == NAV_DROP || t == NAV_JUMP || t == NAV_GAP) steps[k].risky = navbotpitunder(steps[k-1].o, steps[k].o) ? 1 : 0;
        }
        if(botnavsafe) for(int k = 1; k < steps.length(); k++) if(!navbotcanrun(d, k >= 2 ? &steps[k-2] : NULL, steps[k-1], steps[k]))
        { // a jump or a drop across it would only refuse at the edge (no room for its run-up): another search that goes round it
            static int depth = 0;
            navbotbadpair(nb, steps[k-1].node, steps[k].node, true);
            if(depth >= 2) return false;
            depth++;
            bool again = navbotmake(d, nb, s, g);
            depth--;
            return again;
        }
        if(navbotbadway(nb, steps)) return false; // through a link that just failed it: not now
        nb.path.setsize(0);
        nb.path.put(steps.getbuf(), steps.length());
        navbotapproaches(nb);
        nb.pending = false;
        nb.wandering = false;
        bool same = nb.goal == g; // the base game asks for the same goal again often (each time it thinks): its progress and its stuck count go on
        nb.goal = g;
        nb.gen = navgeneration;
        nb.aim = 0;
        nb.phase = NBP_WALK;
        nb.aimat = 0;
        nb.aimfrom = d->feetpos();
        if(!same)
        {
            nb.runups = 0; nb.runup = false;
            nb.stuck = 0;
            nb.progat = lastmillis; nb.progpos = d->feetpos();
        }
        d->ai->route.setsize(0);
        d->ai->route.add(-2 - g);
        return true;
    }
    static bool navbotto(fpsent *d, aistate &b, navbot &nb, const vec &pos, int g, bool changed)
    {
        if(g < 0) return false;
        if(changed && nb.goal == g && nb.gen == navgeneration && navbothaspath(d, nb)) { nb.goalpos = pos; b.override = false; return true; }
        if(navbotinflight(nb) || (botnavsafe && nb.phase == NBP_TAKEOFF && nb.goal >= 0 && nb.path.inrange(nb.aim) && nb.aim >= 1 && !navbotonfloor(d) && !d->inwater))
        { // in the air (a flight already launched, or off the floor at the start of a link): it lands where the link takes it and makes the new path once down
          // (not towards a goal known out of reach from there); before the takeoff a new goal is a new path at once
            int l = nb.path[nb.aim].node;
            ullong noway = navbotnowaykey(l, g);
            if(noway != ~0ULL && navcomp[l] != navcomp[g] && navbotnowaygen == navgeneration && navbotnoway.has(noway)) return false;
            nb.goal = g; nb.goalpos = pos; nb.pending = true;
            d->ai->route.setsize(0);
            d->ai->route.add(-2 - g);
            b.override = false;
            return true;
        }
        int s = navbotstart(d, nb);
        if(s < 0 || !navbotmake(d, nb, s, g)) return false;
        nb.goalpos = pos;
        b.override = false;
        return true;
    }

    bool navbotroute(fpsent *d, aistate &b, const vec &pos, bool changed)
    {
        navbot *nb = navbotof(d);
        if(!nb || !navbotready()) return false;
        return navbotto(d, b, *nb, pos, navbotgoalnode(pos), changed);
    }
    // makeroute() towards a waypoint: its place on the grid; a node of the grid itself is -2 - node (navbotrandom)
    bool navbotroutenode(fpsent *d, aistate &b, int wp, bool changed)
    {
        navbot *nb = navbotof(d);
        if(!nb || !navbotready()) return false;
        if(wp < -1)
        {
            int g = -2 - wp;
            if(!navnodes.inrange(g) || navnodes[g].flags&(NAVF_DEAD|NAVF_VIRTUAL)) return false;
            return navbotto(d, b, *nb, navnodes[g].o, g, changed);
        }
        if(!iswaypoint(wp)) return false;
        return navbotto(d, b, *nb, waypoints[wp].o, navbotgoalnode(waypoints[wp].o), changed);
    }
    // an interest of the base game (an item, a team mate, a flag): its place, even where no waypoint is near it
    bool navbotinterest(fpsent *d, aistate &b, const interest &n)
    {
        vec pos;
        switch(n.targtype)
        {
            case AI_T_ENTITY:
                if(!entities::ents.inrange(n.target)) return false;
                pos = entities::ents[n.target]->o;
                break;
            case AI_T_PLAYER:
            {
                fpsent *e = getclient(n.target);
                if(!e || e->state != CS_ALIVE) return false;
                pos = e->feetpos();
                break;
            }
            default:
                return navbotroutenode(d, b, n.node, true);
        }
        return navbotroute(d, b, pos, true);
    }

    // a place to wander to (the base game's random waypoint): the same waypoints when the map has them (the same
    // wandering, only the way is the grid's); on a map without waypoints, nodes of the grid in the same range
    bool navbotrandom(fpsent *d, aistate &b, const vec &pos, float guard, float wander)
    {
        navbot *nb = navbotof(d);
        if(!nb || !navbotready()) return false;
        vec feet = d->feetpos();
        static vector<int> candidates;
        candidates.setsize(0);
        findwaypointswithin(pos, guard, wander, candidates);
        int tries = 0;
        while(!candidates.empty() && tries < 8)
        {
            int w = rnd(candidates.length()), n = candidates.removeunordered(w);
            if(!iswaypoint(n) || !waypoints[n].links[0] || n == d->lastnode || d->ai->hasprevnode(n)) continue;
            int g = navbotgoalnode(waypoints[n].o);
            if(g < 0 || navbotdist2(navnodes[g].o, feet) < 24*24) continue;
            tries++;
            if(navbotto(d, b, *nb, waypoints[n].o, g, false)) return true;
        }
        if(tries) return false;
        int s = navbotstart(d, *nb), comp = s >= 0 && navcomp.inrange(s) ? navcomp[s] : navbigcomp, n = navnodes.length();
        if(n <= 1) return false;
        loopi(64)
        {
            int g = rnd(n);
            const navnode &nn = navnodes[g];
            if(nn.flags&(NAVF_DEAD|NAVF_VIRTUAL) || !navcomp.inrange(g) || navcomp[g] != comp) continue;
            float d2 = nn.o.squaredist(pos);
            if(d2 <= guard*guard || d2 >= wander*wander || navbotdist2(nn.o, feet) < 32*32) continue;
            if(navbotto(d, b, *nb, nn.o, g, false)) return true;
            if(++tries >= 4) break;
        }
        return false;
    }
    // the base game's state "go to that node" for a node of the grid (-2 - node): there, or done
    int navbotnodegoal(fpsent *d, aistate &b)
    {
        int g = -2 - b.target;
        if(!navbotready() || !navnodes.inrange(g) || navnodes[g].flags&NAVF_DEAD) return 0;
        if(navnodes[g].o.dist(d->feetpos()) <= CLOSEDIST) return 0;
        return navbotroutenode(d, b, b.target, true) ? 1 : 0;
    }

    // ---- straight lines: can a player walk straight from `from` (feet, on node cur) to node `to`? Every column the
    // line crosses has a node joined to the one before by a walk; the line keeps off the walls and the edges of those
    // columns (the box of a player would scrape a wall, or step off) and off jump pads and teleporters
    static inline bool navbotclear(const navnode &n, const vec &p)
    {
        float ox = p.x - navcolx(n.col), oy = p.y - navcoly(n.col), unit = navcellsize/8.0f;
        // a node off the middle of its column: something stands in that column, keep close to where the node is
        float nx = n.o.x - navcolx(n.col), ny = n.o.y - navcoly(n.col);
        if(nx*nx + ny*ny > 1 && (p.x - n.o.x)*(p.x - n.o.x) + (p.y - n.o.y)*(p.y - n.o.y) > 2.5f*2.5f*unit*unit) return false;
        uchar sides = n.blocked|n.edges;
        if(!sides) return true;
        loopk(8) if(sides&(1<<k))
        {
            float ux = navdirs[k][0], uy = navdirs[k][1];
            bool diag = ux && uy;
            float along = diag ? (ox*ux + oy*uy)*0.7071f : ox*ux + oy*uy;
            if(along > (diag ? 1.6f : 1.1f)*unit) return false;
        }
        return true;
    }
    static bool navbottriggered(const vec &p)
    {
        if(navtriggers.empty() || p.x < navtrigmin.x || p.y < navtrigmin.y || p.x > navtrigmax.x || p.y > navtrigmax.y) return false;
        loopv(navtriggers) if(p.dist(navtriggers[i].o) < navtriggers[i].r + 1.5f) return true;
        return false;
    }
    // a jump pad or a teleporter there (feet): Tiny Apa's own steps (a fight, a search) keep off them on the grid
    bool navbotontrigger(const vec &p) { return navbotready() && navbottriggered(p); }
    static bool navbotline(const vec &from, int cur, int to)
    {
        if(!navnodes.inrange(cur) || !navnodes.inrange(to)) return false;
        const vec &b = navnodes[to].o;
        float dx = b.x - from.x, dy = b.y - from.y, len = sqrtf(dx*dx + dy*dy);
        if(len < 1) return cur == to || fabs(b.z - from.z) < 6;
        int steps = int(ceilf(len/(navcellsize*0.4f)));
        int prev = cur;
        for(int i = 1; i <= steps; i++)
        {
            float t = float(i)/steps;
            vec p(from.x + dx*t, from.y + dy*t, from.z + (b.z - from.z)*t);
            int col = navcolof(p.x, p.y);
            if(col < 0) return false;
            int n = -1;
            if(navnodes[prev].col == col) n = prev;
            else
            {
                const navnode &np = navnodes[prev];
                float best = 1e9f;
                loopj(np.nlinks)
                {
                    const navlink &l = navlinks[np.link0 + j];
                    if(l.type != NAV_WALK || !navlinkvalid(l) || navnodes[l.to].col != col) continue;
                    float dz = fabs(navnodes[l.to].o.z - p.z);
                    if(dz < best) { best = dz; n = l.to; }
                }
                if(n < 0 || best > 24) return false;
            }
            const navnode &nn = navnodes[n];
            if(nn.flags&(NAVF_DEAD|NAVF_WATER) && n != to) return false;
            if(!navbotclear(nn, p) || navbottriggered(vec(p.x, p.y, nn.o.z))) return false;
            prev = n;
        }
        return prev == to;
    }

    // the furthest step of its path, on from the one it heads for, it can walk to straight (no link other than a walk on
    // the way: such a link is taken from its own node)
    static void navbotsmooth(fpsent *d, navbot &nb, const vec &feet)
    {
        nb.aimat = lastmillis;
        int cur = navbotnodeat(feet, 8, 6, 10);
        if(cur < 0) return;
        int last = nb.aim;
        while(last + 1 < nb.path.length() && !nb.path[last].approach && nb.path[last + 1].type == NAV_WALK && last - nb.aim < NAVB_STEPS) last++;
        for(int k = last; k > nb.aim; k--)
        {
            if(navbotdist2(nb.path[k].o, feet) > NAVB_SMOOTH*NAVB_SMOOTH) continue;
            if(navbotline(feet, cur, nb.path[k].node)) { nb.aim = k; nb.aimfrom = feet; return; }
        }
    }

    // ---- players it perceives in its way are walked round (it sees them, or it bumps into them)
    static vec navbotavoid(fpsent *d, const vec &feet, const vec &spot)
    {
        vec to = vec(spot).sub(feet);
        to.z = 0;
        float len = to.magnitude();
        if(len < 1) return spot;
        to.mul(1/len);
        loopv(players)
        {
            fpsent *e = players[i];
            if(e == d || e->state != CS_ALIVE) continue;
            vec ef = e->feetpos(), rel = vec(ef).sub(feet);
            if(fabs(rel.z) > 20) continue;
            rel.z = 0;
            float along = rel.dot(to);
            if(along < 0 || along > min(len, 48.0f)) continue;
            float side = rel.x*to.y - rel.y*to.x, room = d->radius + e->radius + 3;
            if(fabs(side) >= room) continue;
            bool touch = rel.magnitude() < d->radius + e->radius + 2;
            if(!touch && !navbotsees(d, e)) continue;
            // round him on the side it already leans to, if there is floor there
            vec perp(to.y, -to.x, 0);
            float s = side > 0 ? -1 : 1;
            int cur = navbotnodeat(feet, 8, 6, 10);
            if(cur < 0) return spot;
            loopk(2)
            {
                vec p = vec(ef).add(vec(perp).mul(s*(room + 4)));
                int n = navbotnodeat(vec(p.x, p.y, feet.z), 6, 6, 10);
                if(n >= 0 && navbotline(feet, cur, n)) return vec(p.x, p.y, navnodes[n].o.z);
                s = -s;
            }
            return spot;
        }
        return spot;
    }

    // engaged with an enemy (pursuing one, or one in sight): its view is its own (aim), never turned towards a link
    static bool navbotengaged(fpsent *d, const aistate &b)
    {
        if(b.type == AI_S_PURSUE) return true;
        fpsent *e = getclient(d->ai->enemy);
        return e && e != d && e->state == CS_ALIVE && navbotsees(d, e);
    }
    // ---- deadly falls (botnavsafe): does a fall from feet at (x, y, z) end in death (death or lava below, or nothing below
    // at all: out of the world)? Kept per 2 u and 4 u of height for the grid as it is now (an edit makes it again)
    static navset64 navbotpitknown, navbotpitdeadly;
    static int navbotpitgen = -1;
    // from p (a little above the feet): nothing below (out of the world), death or lava, or a slope too steep to stand on
    // that slides it down into one of those
    static bool navbotpitdown(const vec &p)
    {
        vec n(0, 0, 1);
        float down = rayfloor(p, n, RAY_CLIPMAT, p.z + 8);
        if(down < 0 || down >= p.z - 0.5f) return true;
        vec f(p.x, p.y, p.z - down);
        if(navdeadly(f)) return true;
        if(n.z >= NAV_SLOPEZ) return false;
        vec downhill(n.x, n.y, 0);
        if(downhill.squaredlen() < 1e-6f) return false;
        downhill.normalize();
        for(float s = 4; s <= 64; s += 4)
        { // down the slope: onto a floor it stands on, or into the fall
            vec q = vec(downhill).mul(s).add(f);
            q.z = f.z + 4;
            float d2 = rayfloor(q, n, RAY_CLIPMAT, q.z + 8);
            if(d2 < 0 || d2 >= q.z - 0.5f || navdeadly(vec(q.x, q.y, q.z - d2))) return true;
            if(n.z >= NAV_SLOPEZ) return false;
        }
        return false;
    }
    static bool navbotpit(float x, float y, float z)
    {
        if(navbotpitgen != navgeneration || navbotpitknown.count > (1<<18)) { navbotpitknown.clear(); navbotpitdeadly.clear(); navbotpitgen = navgeneration; }
        int ix = int(floorf(x*0.5f)), iy = int(floorf(y*0.5f)), iz = int(floorf(z*0.25f));
        ullong key = (ullong(uint(ix)&0xFFFFF) << 40) | (ullong(uint(iy)&0xFFFFF) << 20) | ullong(uint(iz)&0xFFFFF);
        if(navbotpitknown.has(key)) return navbotpitdeadly.has(key);
        bool deadly = navbotpitdown(vec((ix + 0.5f)*2, (iy + 0.5f)*2, iz*4 + 6.0f));
        navbotpitknown.add(key);
        if(deadly) navbotpitdeadly.add(key);
        return deadly;
    }
    // a link from a to b flies over such a fall (on its way, at the lower height of the two)
    static bool navbotpitunder(const vec &a, const vec &b)
    {
        float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx*dx + dy*dy), z = min(a.z, b.z);
        int n = max(int(len/4), 1);
        for(int i = 1; i < n; i++) if(navbotpit(a.x + dx*i/n, a.y + dy*i/n, z)) return true;
        return false;
    }
    // feet on a jump pad whose flights (the grid's) cross a deadly fall: stepped onto by mistake (a fight, a slide), it can
    // throw a bot into it (known per pad for the grid as it is now)
    static vector<schar> navbotpadrisk;
    static int navbotpadriskgen = -1;
    static bool navbotriskypad(const vec &p)
    {
        if(navtriggers.empty() || p.x < navtrigmin.x || p.y < navtrigmin.y || p.x > navtrigmax.x || p.y > navtrigmax.y) return false;
        if(navbotpadriskgen != navgeneration || navbotpadrisk.length() != navpads.length())
        {
            navbotpadrisk.setsize(0);
            loopv(navpads) navbotpadrisk.add(-1);
            navbotpadriskgen = navgeneration;
        }
        loopv(navtriggers)
        {
            const navtrigger &tr = navtriggers[i];
            if(tr.kind || !navpads.inrange(tr.index) || p.dist(tr.o) >= tr.r + 1.5f) continue;
            schar &r = navbotpadrisk[tr.index];
            if(r < 0)
            {
                r = 0;
                int v = navpads[tr.index].vnode;
                if(navnodes.inrange(v))
                {
                    const navnode &n = navnodes[v];
                    loopj(n.nlinks)
                    {
                        const navlink &l = navlinks[n.link0 + j];
                        if(l.type == NAV_PAD && navlinkvalid(l) && navbotpitunder(n.o, navnodes[l.to].o)) { r = 1; break; }
                    }
                }
            }
            if(r) return true;
        }
        return false;
    }
    // its course on the floor (the game's physics without the walls, 10 ms steps): keys (move, strafe) held for `hold` ms,
    // then keys against its course until it stops: false, with the first spot, if its feet come over a deadly fall (or onto
    // a jump pad that throws over one, unless its way takes that pad: padok)
    static bool navbotsafecourse(const fpsent *d, int move, int strafe, int hold, vec &where, bool notrigger = false, bool padok = true)
    {
        vec p = d->feetpos(), v(d->vel.x, d->vel.y, 0), dir(0, 0, 0);
        if(move || strafe) { vecfromyawpitch(d->yaw, 0, move, strafe, dir); dir.z = 0; dir.normalize(); }
        vec target = vec(dir).mul(d->maxspeed*(move && !strafe ? 1.3f : 1.0f)), braking(0, 0, 0);
        static const float k = powf(1 - 1/6.0f, 10/20.0f);
        float moved = 0;
        bool safe = !navbotpit(p.x, p.y, p.z); // already over such a fall (a lip, the top of a slope): a way off it is let through, if it stays off it
        if(!safe && (move || strafe)) hold = max(hold, 300); // (walking out of there: its keys held a while)
        for(int t = 0; t < 1000; t += 10)
        {
            if(t >= hold)
            {
                if(braking.iszero())
                {
                    float vl = sqrtf(v.x*v.x + v.y*v.y);
                    if(vl < 4) break;
                    braking = vec(v).mul(-1/vl);
                }
                if(v.dot(braking) >= 0) break; // stopped
                target = vec(braking).mul(d->maxspeed);
            }
            v.sub(target).mul(k).add(target);
            vec dp = vec(v).mul(0.01f);
            p.add(dp);
            moved += sqrtf(dp.x*dp.x + dp.y*dp.y);
            if(moved >= 2)
            {
                moved = 0;
                if(notrigger && navbottriggered(p)) { where = p; return false; } // (a way round an edge: not onto a jump pad or a teleporter)
                if(!padok && navbotriskypad(p)) { where = p; return false; }
                if(!navbotpit(p.x, p.y, p.z)) safe = true;
                else if(safe) { where = p; return false; }
            }
        }
        if(navbotpit(p.x, p.y, p.z)) { where = p; return false; }
        return true;
    }
    // where a jump from where it stands comes down at height z (its own height by default), holding keys (move, strafe)
    static bool navbotjumpland(const fpsent *d, int move, int strafe, vec &land, float z = -1e9f)
    {
        vec p = d->feetpos(), v(d->vel.x, d->vel.y, max(d->vel.z, JUMPVEL)), dir(0, 0, 0);
        if(z < -1e8f) z = p.z;
        if(move || strafe) { vecfromyawpitch(d->yaw, 0, move, strafe, dir); dir.z = 0; dir.normalize(); }
        vec target = vec(dir).mul(d->maxspeed*(move && !strafe ? 1.3f : 1.0f)*1.3f);
        static const float k = powf(1 - 1/30.0f, 5/20.0f);
        float fz = 0;
        for(int t = 0; t < 3000; t += 5)
        {
            fz -= GRAVITY*0.005f;
            v.sub(target).mul(k).add(target);
            vec dp = vec(v).add(vec(0, 0, fz)).mul(0.005f);
            p.add(dp);
            if(dp.z < 0 && p.z <= z) { land = p; return true; }
        }
        land = p;
        return false;
    }
    // ---- keys towards a direction (world yaw), as process() picks them: by 45 degrees from where it looks
    static const int navbotkeys8[8][2] = { { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
    static void navbotkeysto(fpsent *d, float yaw)
    {
        float rel = yaw - d->yaw;
        while(rel < 0) rel += 360;
        while(rel >= 360) rel -= 360;
        int r = int(floorf((rel + 22.5f)/45.0f))&7;
        d->move = navbotkeys8[r][0];
        d->strafe = navbotkeys8[r][1];
    }
    // its view turns towards yaw (a player looks where he jumps): at most `rate` degrees a second
    static void navbotturn(fpsent *d, float yaw, float rate)
    {
        float diff = navbotwrap(yaw - d->yaw), step = rate*clamp(curtime, 1, 100)/1000.0f;
        d->yaw += clamp(diff, -step, step);
        while(d->yaw < 0) d->yaw += 360;
        while(d->yaw >= 360) d->yaw -= 360;
        d->ai->targyaw = yaw;
        navbotstyleview(d);
    }

    // ---- in the air: where it lands at height z holding keys (move, strafe) from now on: the game's physics without the
    // walls (5 ms steps: gravity, air control towards the keys), as moveplayer() does
    static bool navbotpredict(const fpsent *d, int move, int strafe, float z, vec &land)
    {
        vec p = d->feetpos(), v = d->vel, f = d->falling, dir(0, 0, 0);
        if(move || strafe) { vecfromyawpitch(d->yaw, 0, move, strafe, dir); dir.z = 0; dir.normalize(); }
        vec target = vec(dir).mul(d->maxspeed*(move && !strafe ? 1.3f : 1.0f)*1.3f);
        static const float k = powf(1 - 1/30.0f, 5/20.0f);
        for(int t = 0; t < 4000; t += 5)
        {
            f.z -= GRAVITY*0.005f;
            v.sub(target).mul(k).add(target);
            vec dp = vec(v).add(f).mul(0.005f), np = vec(p).add(dp);
            if(dp.z < 0 && np.z <= z)
            {
                float u = p.z - np.z > 1e-4f ? (p.z - z)/(p.z - np.z) : 1;
                land = vec(p).add(dp.mul(u));
                return true;
            }
            p = np;
        }
        land = p;
        return false;
    }
    // the keys that land it nearest the node it flies to (those of its plan while they do well enough)
    static void navbotairkeys(fpsent *d, const vec &goal)
    {
        vec land;
        float best = 1e16f;
        int bm = d->move, bs = d->strafe;
        if(navbotpredict(d, bm, bs, goal.z, land)) best = navbotdist2(land, goal);
        if(best <= 4*4) return;
        int pm = bm, ps = bs;
        loopi(9)
        {
            int m = i < 8 ? navbotkeys8[i][0] : 0, s = i < 8 ? navbotkeys8[i][1] : 0;
            if(m == pm && s == ps) continue;
            if(!navbotpredict(d, m, s, goal.z, land)) continue;
            float e = navbotdist2(land, goal);
            if(e < best - 2*2) { best = e; bm = m; bs = s; }
        }
        d->move = bm; d->strafe = bs;
    }

    // ---- taking a link: from path[aim-1] (where it takes off) to path[aim]
    // down again after a link: where the grid said, or a little further on its way (it walks on from there); -1 elsewhere
    static bool navbotline(const vec &from, int cur, int to);
    static int navbotlandstep(const navbot &nb, const vec &feet)
    {
        const navstep &to = nb.path[nb.aim];
        float tol = to.type == NAV_TELE || to.type == NAV_PAD ? 24 : 16;
        if(navbotdist2(feet, to.o) < tol*tol && fabs(feet.z - to.o.z) < 12) return nb.aim;
        for(int k = nb.aim + 1; k < nb.path.length() && k <= nb.aim + 8; k++)
        {
            const navstep &s = nb.path[k];
            if(navbotlinktype(s.type)) break;
            if(navbotdist2(feet, s.o) < 12*12 && fabs(feet.z - s.o.z) < 8) return k;
        }
        // on the same floor as the node, a little further: it walks back to it straight
        if(fabs(feet.z - to.o.z) < 6 && navbotdist2(feet, to.o) < 40*40)
        {
            int cur = navbotnodeat(feet, 8, 6, 10);
            if(cur >= 0 && navbotline(feet, cur, to.node)) return nb.aim;
        }
        return -1;
    }
    static bool navbotlandnear(const navbot &nb, const vec &feet)
    {
        const navstep &to = nb.path[nb.aim];
        if(fabs(feet.z - to.o.z) >= 10 || navbotdist2(feet, to.o) >= 96*96) return false; // at the height it jumped for, in the same part of the grid
        int cur = navbotnodeat(feet, 12, 6, 10);
        return cur >= 0 && navcomp.inrange(cur) && navcomp.inrange(to.node) && navcomp[cur] == navcomp[to.node];
    }
    // a link that did not take it where it should is kept in mind a while: a way through it is not taken again meanwhile
    static const int NAVB_BADMS = 20000;
    static void navbotbadpair(navbot &nb, int from, int to, bool fam)
    {
        int k = 0;
        loopi(4) if(nb.badat[i] < nb.badat[k]) k = i;
        nb.badfrom[k] = from; nb.badto[k] = to; nb.badat[k] = lastmillis; nb.badfam[k] = fam;
    }
    static void navbotbadlink(navbot &nb, bool fam = false) { navbotbadpair(nb, nb.path[nb.aim - 1].node, nb.path[nb.aim].node, fam); }
    static bool navbotbadway(const navbot &nb, const vector<navstep> &path)
    {
        loopi(4) if(nb.badat[i] && lastmillis - nb.badat[i] < NAVB_BADMS)
            for(int k = 1; k < path.length(); k++) if((path[k].node == nb.badto[i] && path[k-1].node == nb.badfrom[i]) || (nb.badfam[i] && navlinklike(nb.badfrom[i], nb.badto[i], path[k-1].node, path[k].node))) return true;
        return false;
    }
    static bool navbotavoid(const navbot &nb) // its links that failed it a moment ago: its next search goes round them
    {
        navavoidn = 0;
        loopi(4) if(nb.badat[i] && lastmillis - nb.badat[i] < NAVB_BADMS && navnodes.inrange(nb.badfrom[i]) && navnodes.inrange(nb.badto[i]))
        {
            int u = nb.badfrom[i], v = nb.badto[i];
            navavoidfrom[navavoidn] = u; navavoidto[navavoidn] = v;
            navavoidcl[navavoidn] = navnodecluster.inrange(u) && navnodecluster.inrange(v) && navnodecluster[u] == navnodecluster[v] ? navnodecluster[u] : -1;
            navavoidfam[navavoidn] = nb.badfam[i];
            loopj(4)
            { // the clusters the links like it can lie in
                vec c = vec(j&1 ? 16.0f : -16.0f, j&2 ? 16.0f : -16.0f, 0).add(navnodes[u].o);
                int n = nb.badfam[i] ? navbotnodeat(c, 12, 6, 6) : -1;
                navavoidcl2[navavoidn][j] = n >= 0 && navnodecluster.inrange(n) ? navnodecluster[n] : -1;
            }
            navavoidn++;
        }
        return navavoidn > 0;
    }
    static bool navbotreplan(fpsent *d, aistate &b, navbot &nb)
    {
        nb.replanat = lastmillis;
        vec pos = nb.goalpos;
        int g = nb.goal >= 0 && navnodes.inrange(nb.goal) && !(navnodes[nb.goal].flags&NAVF_DEAD) ? nb.goal : navbotgoalnode(pos);
        nb.path.setsize(0);
        nb.phase = NBP_WALK;
        if(!navbotready() || g < 0 || !navbotto(d, b, nb, pos, g, false)) { navbotdrop(d, nb); return false; }
        return true;
    }
    static void navbotfailed(fpsent *d, aistate &b, navbot &nb) // the link did not take it where it should
    {
        if(nb.aim >= 1 && nb.path.inrange(nb.aim)) navbotbadlink(nb, botnavsafe && nb.phase == NBP_TAKEOFF && !nb.jumped && (nb.path[nb.aim].type == NAV_GAP || nb.flying)); // refused at its start: the links like it too
        if(++nb.fails >= 3) { nb.fails = 0; navbotdrop(d, nb); d->ai->clear(false); return; } // that way does not work for it: the base game picks another goal
        navbotreplan(d, b, nb);
    }
    // a drop that crosses a void to a floor about as high (simulated at full speed): taken like a jump over a gap
    static inline bool navbotflyingdrop(const navstep &a, const navstep &to) { return to.type == NAV_DROP && navbotdist2(to.o, a.o) >= 32*32 && to.o.z >= a.o.z - 24; }
    static void navbotbegin(fpsent *d, navbot &nb) // at the node it takes off from
    {
        nb.phase = NBP_TAKEOFF;
        nb.phaseat = lastmillis;
        nb.jumped = nb.runup = nb.ready = nb.leap = nb.commit = false;
        nb.takeoff = nb.tkpos = d->feetpos();
        nb.tkat = lastmillis;
        const navstep &to = nb.path[nb.aim], &a = nb.path[nb.aim - 1];
        nb.flying = navbotflyingdrop(a, to);
        if(to.type == NAV_GAP || nb.flying)
        { // where it takes a run-up from if it comes too slow: behind its node, along the jump, where it can walk straight
            const navstep &from = nb.path[nb.aim - 1];
            vec dir = navbotdir(nb.path[nb.aim]);
            nb.runupat = from.o;
            for(float back = NAVB_RUNUP; back >= 12; back -= 8)
            {
                vec r = vec(dir).mul(-back).add(from.o);
                int n = navbotnodeat(r, 5, 4, 6);
                if(n >= 0 && navbotline(from.o, from.node, n)) { nb.runupat = navnodes[n].o; break; }
            }
            // already running straight at it: no run-up; else one first (with no room for it, the jump is not tried)
            float speed = d->vel.x*dir.x + d->vel.y*dir.y;
            nb.runup = speed < (to.type == NAV_GAP ? 110 : 60); // a drop across: its keys in the air make up for a slower run
            if(botnavsafe && to.risky) nb.runup = speed < (to.type == NAV_GAP ? 118 : 110); // over a deadly fall: at the speed the grid ran
        }
    }
    static inline bool navbotswift(const navbot &nb) { return nb.path[nb.aim].type == NAV_GAP || nb.flying; } // needs its full speed at the edge
    // the jump of a link: asked again at every frame until the physics takes it (a frame can go by without any step of the
    // physics, and the base game clears the key at the end of each frame)
    static void navbotjumpnow(fpsent *d, navbot &nb, const vec &feet)
    {
        if(!nb.jumped) { nb.jumped = true; nb.launchat = lastmillis; nb.takeoff = feet; }
        if(navbotonfloor(d) || d->inwater) d->jumping = true;
    }
    static inline bool navbotrising(const fpsent *d, const navstep &from, const vec &feet) { return d->vel.z + d->falling.z > 40 || feet.z > from.o.z + 2; }
    static void navbotair(navbot &nb, int at, const vec &feet) { nb.phase = NBP_AIR; nb.launchat = at; nb.takeoff = feet; }

    // where it heads for while it takes a link (TAKEOFF) or flies (AIR); false when the link is given up (a new way, or none)
    static bool navbotlinkspot(fpsent *d, aistate &b, navbot &nb, const vec &feet, vec &spot)
    {
        const navstep &to = nb.path[nb.aim], &from = nb.path[nb.aim - 1];
        vec dir = navbotdir(to);
        bool air = !navbotonfloor(d) && !d->inwater;
        if(nb.phase == NBP_AIR)
        {
            spot = to.o;
            if(((navbotground(d) && d->vel.z + d->falling.z < 20) || (d->inwater && to.type != NAV_SWIM && !(navnodes.inrange(to.node) && navnodes[to.node].flags&NAVF_WATER))) && lastmillis - nb.launchat > 60)
            { // down again: where the grid said (or a little further on its way), or elsewhere
                int k = navbotlandstep(nb, feet);
                if(k >= 0) { nb.fails = 0; nb.aim = k; navbotwalkfrom(d, nb); navbotstyleview(d); return true; }
                if(navbotlandnear(nb, feet))
                { // at the height the link was for, a little off its node: the link did its part, a new path from there
                    nb.fails = 0;
                    navbotstyleview(d);
                    navbotreplan(d, b, nb);
                    return true;
                }
                navbotfailed(d, b, nb);
                return false;
            }
            if(lastmillis - nb.launchat > 6000) { navbotfailed(d, b, nb); return false; }
            return true;
        }
        if(lastmillis - nb.phaseat > (navbotswift(nb) ? 4000 : 2500)) { navbotfailed(d, b, nb); return false; }
        if(to.type == NAV_DROP || to.type == NAV_JUMP || to.type == NAV_GAP)
        { // blocked on its way to take off (a wall the simulated player went past), or on a slope too steep to jump from:
          // another way, at once rather than at the end of its time
            if(navbotdist2(feet, nb.tkpos) > 2*2 || nb.runup || !(d->move || d->strafe)) { nb.tkpos = feet; nb.tkat = lastmillis; }
            else if(navbotonfloor(d) && lastmillis - nb.tkat > (to.type == NAV_JUMP ? 1300 : 700)) { navbotfailed(d, b, nb); return false; }
            // (on its way off the edge over a deadly fall, botnavsafe: the lip of the edge, not a slope too steep to jump from)
            if(d->physstate == PHYS_SLIDE && lastmillis - nb.phaseat > 400 && !(botnavsafe && to.risky && (nb.commit || nb.jumped))) { navbotfailed(d, b, nb); return false; }
        }
        switch(to.type)
        {
            case NAV_PADIN: case NAV_TELEIN:
            { // onto the jump pad or into the teleporter; once it took it, the flight (or the fall at the other end) is the next link
                spot = to.o;
                int want = to.type == NAV_PADIN ? JUMPPAD : TELEPORT;
                if(d->lastpickup == want && d->lastpickupmillis >= nb.phaseat && nb.aim + 1 < nb.path.length())
                {
                    nb.aim++;
                    navbotair(nb, d->lastpickupmillis, feet);
                    nb.launchvel = d->vel;
                    spot = nb.path[nb.aim].o;
                }
                return true;
            }
            case NAV_DROP:
                if(nb.flying && nb.runup)
                { // back to where it takes its run-up from (it came too slow for that drop across)
                    spot = nb.runupat;
                    if(navbotdist2(feet, nb.runupat) < 5*5 || lastmillis - nb.phaseat > 1500) { nb.runup = false; nb.phaseat = lastmillis; }
                    return true;
                }
                spot = vec(dir).mul(32).add(from.o);
                if(air && feet.z < from.o.z - 3) navbotair(nb, lastmillis, nb.takeoff);
                else if(air && botnavsafe && !nb.runup && (feet.x - from.o.x)*dir.x + (feet.y - from.o.y)*dir.y > 2) navbotair(nb, lastmillis, nb.takeoff); // off the edge: in the air at once
                return true;
            case NAV_GAP:
            {
                if(nb.runup)
                { // back to where it takes its run-up from
                    spot = nb.runupat;
                    if(navbotdist2(feet, nb.runupat) < 5*5 || lastmillis - nb.phaseat > 1500) { nb.runup = false; nb.phaseat = lastmillis; }
                    return true;
                }
                spot = vec(dir).mul(48).add(from.o);
                if(nb.jumped && air && navbotrising(d, from, feet)) { navbotair(nb, nb.launchat, nb.takeoff); return true; }
                vec rel = vec(feet).sub(from.o);
                if(!nb.jumped && rel.x*dir.x + rel.y*dir.y > 6) { navbotfailed(d, b, nb); return false; } // past its node without a jump
                if(nb.jumped && air && !navbotrising(d, from, feet) && feet.z < from.o.z - 3) { navbotfailed(d, b, nb); return false; } // fell in
                return true;
            }
            case NAV_JUMP:
                spot = nb.ready ? vec(dir).mul(16).add(from.o) : from.o; // onto its node first (where the grid jumped from), then the jump
                if(nb.jumped && air && navbotrising(d, from, feet)) navbotair(nb, nb.launchat, nb.takeoff);
                return true;
            case NAV_SWIM:
                spot = vec(dir).mul(32).add(from.o);
                if(!d->inwater && air) navbotair(nb, lastmillis, feet);
                return true;
            default: // a flight or a teleport not entered from its own node: walked on as it can
                navbotwalkfrom(d, nb);
                return true;
        }
    }

    // ---- once per frame (hunt): the spot it heads for; 1 it goes, 0 it has no way (it waits), -1 not on the grid (the
    // waypoints, as before)
    static bool navbotwander(fpsent *d, aistate &b, navbot &nb) // no way to go: a few steps to a node nearby (as the base game walks to a random waypoint)
    {
        vec feet = d->feetpos();
        int s = navbotstart(d, nb);
        if(s < 0) return false;
        int comp = navcomp.inrange(s) ? navcomp[s] : -1, n = navnodes.length();
        loopi(24)
        {
            int g = rnd(n);
            const navnode &nn = navnodes[g];
            if(nn.flags&(NAVF_DEAD|NAVF_VIRTUAL) || !navcomp.inrange(g) || navcomp[g] != comp) continue;
            float d2 = navbotdist2(nn.o, feet);
            if(d2 < 48*48 || d2 > 240*240) continue;
            if(navbotto(d, b, nb, nn.o, g, false)) { nb.wandering = true; nb.wanderat = lastmillis; return true; }
            break;
        }
        return false;
    }
    int navbothunt(fpsent *d, aistate &b)
    {
        navbot *pnb = navbotof(d);
        if(!pnb) return -1;
        navbot &nb = *pnb;
        vec feet = d->feetpos();
        if(navbotonfloor(d) && navready()) { int n = navbotnodeat(feet, 8, 6, 10); if(n >= 0) nb.node = n; }
        if(!navbothaspath(d, nb) && nb.wandering && lastmillis - nb.wanderat < 600 && nb.path.length() && nb.goal >= 0 && d->ai->route.empty() && navbotready() && nb.gen == navgeneration)
            d->ai->route.add(-2 - nb.goal); // the base game cleared its route a moment after it set off to wander: the same walk goes on (not a new one every frame)
        if(!navbothaspath(d, nb))
        {
            nb.path.setsize(0);
            nb.phase = NBP_WALK;
            if(!navbotready()) return -1;
            if(lastmillis - nb.wanderfail < 250) return d->ai->route.length() && iswaypoint(d->ai->route[0]) ? -1 : 0;
            // a way the waypoints gave it (before the grid was ready, or off the grid): the same goal, on the grid
            bool ok = d->ai->route.length() && iswaypoint(d->ai->route[0]) && navbotroutenode(d, b, d->ai->route[0], false);
            if(!ok && d->ai->route.length() && iswaypoint(d->ai->route[0])) { nb.wanderfail = lastmillis; return -1; } // not on the grid from here: those waypoints a moment more
            if(!ok && !navbotwander(d, b, nb))
            { // nowhere to go from here: off the grid (a spot it does not know), it walks to the nearest node
                nb.wanderfail = lastmillis;
                int n = navbotnodeat(feet, 48, 24, 64);
                if(n < 0) return waypoints.length() > 1 ? -1 : 0;
                d->ai->spot = navnodes[n].o;
                d->ai->targnode = -1;
                return 1;
            }
        }
        // the grid changed under its path (made again, a coop edit joined): a new path from here
        if(nb.gen != navgeneration && nb.phase == NBP_WALK && navbotready() && lastmillis - nb.replanat > 200 && !navbotreplan(d, b, nb)) return 0;
        if(!navbothaspath(d, nb)) return 0;
        vec spot;
        if(nb.phase != NBP_WALK)
        {
            bool ok = navbotlinkspot(d, b, nb, feet, spot);
            if(!navbothaspath(d, nb)) return 0;
            if(ok && nb.phase != NBP_WALK) { d->ai->spot = spot; d->ai->targnode = -1; return 1; }
        }
        if(nb.pending && nb.phase == NBP_WALK)
        { // a new goal came during the link: its path, from where it came down
            nb.pending = false;
            if(!navbotreplan(d, b, nb)) return 0;
        }
        // walking: there yet?
        const navstep &a = nb.path[nb.aim];
        bool takeoff = nb.aim + 1 < nb.path.length() && navbotlinktype(nb.path[nb.aim + 1].type);
        float reach = takeoff ? (nb.path[nb.aim + 1].type == NAV_GAP ? 3.0f : 5.0f) : 6.0f;
        if(navbotdist2(feet, a.o) < reach*reach && fabs(feet.z - a.o.z) < 10)
        {
            if(nb.aim + 1 >= nb.path.length())
            { // the goal: the way is done (the base game picks what next; meanwhile it walks on a little, as it did)
                d->ai->spot = a.o;
                d->ai->targnode = -1;
                navbotdrop(d, nb);
                return 1;
            }
            nb.aim++;
            navbotwalkfrom(d, nb);
            if(navbotlinktype(nb.path[nb.aim].type))
            {
                navbotbegin(d, nb);
                bool ok = navbotlinkspot(d, b, nb, feet, spot);
                if(!navbothaspath(d, nb)) return 0;
                if(ok && nb.phase != NBP_WALK) { d->ai->spot = spot; d->ai->targnode = -1; return 1; }
            }
        }
        if(!nb.aimat || lastmillis - nb.aimat >= NAVB_LOOK) navbotsmooth(d, nb, feet);
        const navstep &t = nb.path[nb.aim];
        // off its way (pushed, fell, a wrong step): a new path from where it is
        float sx = t.o.x - nb.aimfrom.x, sy = t.o.y - nb.aimfrom.y, sl = sx*sx + sy*sy,
              u = sl > 1 ? clamp(((feet.x - nb.aimfrom.x)*sx + (feet.y - nb.aimfrom.y)*sy)/sl, 0.0f, 1.0f) : 1,
              ox = nb.aimfrom.x + sx*u - feet.x, oy = nb.aimfrom.y + sy*u - feet.y;
        if((ox*ox + oy*oy > 40*40 || feet.z < min(nb.aimfrom.z, t.o.z) - 28) && navbotonfloor(d) && navbotready() && lastmillis - nb.replanat > 300)
        {
            if(!navbotreplan(d, b, nb)) return 0;
            navbotsmooth(d, nb, feet);
        }
        const navstep &t2 = nb.path[nb.aim];
        spot = navbotavoid(d, feet, t2.o);
        d->ai->spot = spot;
        d->ai->targnode = -1;
        return 1;
    }

    // walking (its path, or the moves of Tiny Apa in a fight): never off an edge it did not plan (its speed in a turn, a
    // push): a big drop just ahead where the grid has no floor, and no link of its path there: it brakes
    static void navbotedgeguard(fpsent *d, navbot &nb, const vec &feet)
    {
        if(!navbotonfloor(d) || d->inwater) return;
        float vx = d->vel.x, vy = d->vel.y, v = sqrtf(vx*vx + vy*vy);
        if(v < 60) return;
        for(int k = nb.aim; k <= nb.aim + 2 && k + 1 < nb.path.length(); k++)
            if(navbotlinktype(nb.path[k + 1].type) && navbotdist2(feet, nb.path[k].o) < 64*64) return; // a link of its path starts there (it runs to it)
        vec ahead(feet.x + vx*0.15f, feet.y + vy*0.15f, feet.z + 4);
        if(raycube(ahead, vec(0, 0, -1), 128) < 32) return;
        if(navbotnodeat(vec(ahead.x, ahead.y, feet.z), 8, 6, 24) >= 0) return; // the grid has a floor there after all
        navbotkeysto(d, navbotyawof(-vx, -vy));
    }
    // ---- the keys while it takes a link (after process() and the moves of Tiny Apa): the takeoff, the plan of the flight
    // as the grid found it, and its correction so as to land where it should
    // never off an edge into a deadly fall (a death pit, the void), whatever moves it (its path, the fight moves of Tiny Apa,
    // the base game), unless it runs at a link of its path that way: keys that would walk it off such an edge are let go
    static bool navbotdeadlyahead(const vec &feet, const vec &dir)
    {
        for(float r = 8; r <= 16; r += 8)
        {
            vec p = vec(dir).mul(r).add(feet);
            p.z += 4;
            float down = raycube(p, vec(0, 0, -1), 256);
            if(down < 36) return false; // a floor there (or a small step down)
            if(down >= 250 || navdeadly(vec(p.x, p.y, p.z - down))) return true;
        }
        return false;
    }
    static void navbotdeathguard(fpsent *d, navbot *nb, const vec &feet)
    {
        if(!navbotonfloor(d) || d->inwater || (!d->move && !d->strafe)) return;
        vec m;
        vecfromyawpitch(d->yaw, 0, d->move, d->strafe, m);
        m.z = 0;
        if(m.squaredlen() < 1e-4f) return;
        m.normalize();
        if(nb && navbothaspath(d, *nb))
        {
            if(nb->phase != NBP_WALK) return; // taking a link: its own takeoff
            for(int k = nb->aim; k <= nb->aim + 2 && k + 1 < nb->path.length(); k++)
                if(navbotlinktype(nb->path[k + 1].type) && navbotdist2(feet, nb->path[k].o) < 64*64)
                {
                    vec ld = navbotdir(nb->path[k + 1]);
                    if(ld.x*m.x + ld.y*m.y > 0.7f) return; // it runs at that link (its way goes there)
                }
        }
        if(!navbotdeadlyahead(feet, m)) return;
        d->move = d->strafe = 0;
        float vx = d->vel.x, vy = d->vel.y;
        if(vx*m.x + vy*m.y > 40) navbotkeysto(d, navbotyawof(-vx, -vy)); // and it brakes when it runs at that edge
    }
    // a jump from where it stands (keys as they are) comes down on the floor of node `to` (at its height, not over a
    // deadly fall)
    static bool navbotleaplands(const fpsent *d, const navstep &to)
    {
        static const int keys[4][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 0 } };
        loopk(4)
        { // its keys in the air let go, forward, back (its correction in the air then aims inside that floor), or as they are
            vec land;
            int m = k < 3 ? keys[k][0] : d->move, s = k < 3 ? keys[k][1] : d->strafe;
            if(!navbotjumpland(d, m, s, land, to.o.z) || navbotpit(land.x, land.y, to.o.z)) continue;
            float down = raycube(vec(land.x, land.y, to.o.z + 4), vec(0, 0, -1), 16);
            if(down >= 2 && down <= 8) return true;
        }
        return false;
    }
    // the spot it flies for over a deadly fall: a little further on than the node the grid landed on (that node can lie on
    // the very edge of the floor), as far as the same floor goes on
    static vec navbotlandgoal(const navbot &nb)
    {
        const navstep &to = nb.path[nb.aim], &from = nb.path[nb.aim - 1];
        vec dir(to.o.x - from.o.x, to.o.y - from.o.y, 0);
        float len = dir.magnitude();
        if(len < 1) return to.o;
        dir.mul(1/len);
        for(float s = 8; s >= 4; s -= 2)
        {
            vec p = vec(dir).mul(s).add(to.o), q = vec(dir).mul(s + 4).add(to.o);
            if(navbotpit(p.x, p.y, to.o.z) || navbotpit(q.x, q.y, to.o.z)) continue;
            float down = raycube(vec(p.x, p.y, to.o.z + 4), vec(0, 0, -1), 16);
            if(down < 3 || down > 6) continue; // the same floor there (not a wall, not a step down)
            return vec(p.x, p.y, to.o.z);
        }
        return to.o;
    }
    // ---- never off an edge into a deadly fall it did not plan (botnavsafe), whatever moves it (its path, the fight moves of
    // Tiny Apa, the base game, its speed after a landing): its course is foreseen with its keys held a moment, then braking;
    // if that ends over such a fall, the keys nearest to its own that do not (along the edge, or none), else it brakes; and
    // no jump of its own that lands over it. A link of its path that crosses that fall from just ahead is its own way.
    static bool navbotlinkway(fpsent *d, navbot *nb, const vec &feet, const vec &where)
    {
        if(!nb || !navbothaspath(d, *nb) || nb->phase != NBP_WALK) return false;
        for(int k = nb->aim; k <= nb->aim + 2 && k + 1 < nb->path.length(); k++)
        {
            if(!navbotlinktype(nb->path[k + 1].type) || navbotdist2(feet, nb->path[k].o) >= 16*16) continue; // (at its takeoff only)
            vec ld = navbotdir(nb->path[k + 1]), rel = vec(where).sub(nb->path[k].o);
            if(rel.x*ld.x + rel.y*ld.y >= -4 && fabs(rel.x*ld.y - rel.y*ld.x) <= 10) return true; // the fall that link flies over
        }
        return false;
    }
    static void navbotfallguard(fpsent *d, navbot *nb)
    {
        if((!navbotonfloor(d) && !(d->physstate == PHYS_SLIDE && !d->timeinair)) || d->inwater) return; // (on the very edge the physics says it slides: still on it)
        if(nb && navbothaspath(d, *nb) && nb->phase != NBP_WALK)
        { // taking a link: over a deadly fall, its own takeoff (decided before the edge); else kept off such a fall too
            if(nb->phase == NBP_AIR || !nb->path.inrange(nb->aim)) return;
            const navstep &to = nb->path[nb->aim];
            if(to.risky || nb->jumped || to.type == NAV_PADIN || to.type == NAV_TELEIN || to.type == NAV_SWIM) return;
        }
        vec feet = d->feetpos(), where;
        int hold = clamp(2*curtime, 20, 50);
        bool padok = false; // its way takes a jump pad just ahead: onto it, as planned
        if(nb && navbothaspath(d, *nb)) for(int k = nb->aim; k <= nb->aim + 3 && k < nb->path.length(); k++) if(nb->path[k].type == NAV_PADIN) padok = true;
        if(!navbotsafecourse(d, d->move, d->strafe, hold, where, false, padok) && !navbotlinkway(d, nb, feet, where))
        {
            vec want(0, 0, 0);
            if(d->move || d->strafe) { vecfromyawpitch(d->yaw, 0, d->move, d->strafe, want); want.z = 0; want.normalize(); }
            int bm = 0, bs = 0;
            float best = -2;
            if(!want.iszero()) loopi(9)
            {
                int m = i < 8 ? navbotkeys8[i][0] : 0, s = i < 8 ? navbotkeys8[i][1] : 0;
                if(m == d->move && s == d->strafe) continue;
                vec dir(0, 0, 0);
                if(m || s) { vecfromyawpitch(d->yaw, 0, m, s, dir); dir.z = 0; dir.normalize(); }
                float score = m || s ? dir.dot(want) : -0.25f; // the nearest way to its own, rather than standing still
                if(score <= best || !navbotsafecourse(d, m, s, hold, where, true)) continue;
                best = score; bm = m; bs = s;
            }
            if(best > -2) { d->move = bm; d->strafe = bs; }
            else
            { // no keys of its own (it slides on), or no way is safe any more: it brakes against its course
                float vx = d->vel.x, vy = d->vel.y;
                if(vx*vx + vy*vy > 4*4) navbotkeysto(d, navbotyawof(-vx, -vy)); else d->move = d->strafe = 0;
            }
        }
        if(d->jumping && !(nb && navbothaspath(d, *nb) && nb->phase != NBP_WALK))
        { // a jump of its own (not one of its links): not one that comes down over a deadly fall
            vec land;
            navbotjumpland(d, d->move, d->strafe, land);
            if(navbotpit(land.x, land.y, feet.z)) d->jumping = false;
        }
    }
    static void navbotlinkkeys(fpsent *d, aistate &b, navbot *pnb);
    void navbotkeys(fpsent *d, aistate &b, int result)
    {
        navbot *pnb = navbotof(d);
        if(navbotready() && !botnavsafe) navbotdeathguard(d, pnb, d->feetpos());
        navbotlinkkeys(d, b, pnb);
        if(navbotready() && botnavsafe) navbotfallguard(d, pnb);
    }
    static void navbotlinkkeys(fpsent *d, aistate &b, navbot *pnb)
    {
        if(!pnb || !navbothaspath(d, *pnb)) return;
        navbot &nb = *pnb;
        if(!nb.path.inrange(nb.aim)) return;
        int style = navbotstyle(d);
        vec feet = d->feetpos();
        bool engaged = navbotengaged(d, b); // fighting: its view stays on its aim, the keys follow its view
        if(nb.phase == NBP_WALK)
        { // nearing a gap: its view turns along the jump (it needs to run straight at it)
            if(nb.aim + 1 < nb.path.length() && (nb.path[nb.aim + 1].type == NAV_GAP || navbotflyingdrop(nb.path[nb.aim], nb.path[nb.aim + 1])) && !(style&1) && !b.idle && navbotdist2(feet, nb.path[nb.aim].o) < 40*40)
                navbotturn(d, navbyteyaw(nb.path[nb.aim + 1].yaw), 540); // (even while it fights: it needs its full speed, forward key)
            navbotedgeguard(d, nb, feet);
            return;
        }
        const navstep &to = nb.path[nb.aim];
        float yaw = navbyteyaw(to.yaw);
        if(nb.phase == NBP_TAKEOFF)
        {
            if((style&1 || b.idle == 1) && to.type != NAV_PADIN && to.type != NAV_TELEIN && !(botnavsafe && to.risky && (nb.commit || nb.jumped))) // (on its way off the edge over a deadly fall: too late to stop)
            { // it holds its place (a fight): the link waits, it is taken again from its node afterwards
                nb.aim--;
                navbotwalkfrom(d, nb);
                return;
            }
            d->ai->dontmove = false;
            switch(to.type)
            {
                case NAV_DROP: // it looks where it goes: the grid simulated a player running straight ahead (forward key only)
                    if(!engaged || nb.flying) navbotturn(d, yaw, 720);
                    if(nb.flying && nb.runup) { navbotkeysto(d, navbotyawof(nb.runupat.x - feet.x, nb.runupat.y - feet.y)); break; }
                    navbotkeysto(d, yaw);
                    if(nb.leap && navbotonfloor(d) && lastmillis - nb.launchat < 300) d->jumping = true; // its jump off the edge: asked until it leaves the floor
                    if(nb.flying && !nb.jumped && navbotonfloor(d))
                    { // a drop across a void: at its node, running straight at full speed, or a run-up first (else another way)
                        const vec &a = nb.path[nb.aim - 1].o;
                        vec dir = navbotdir(to), rel = vec(feet).sub(a);
                        float along = rel.x*dir.x + rel.y*dir.y, side = fabs(rel.x*dir.y - rel.y*dir.x);
                        bool risky = botnavsafe && to.risky; // over a deadly fall: it makes up its mind 10 u before its node, while it can still stop before the edge
                        if(along > (risky && !nb.commit ? -10 : -3) && side < 6)
                        {
                            float speed = d->vel.x*dir.x + d->vel.y*dir.y, across = fabs(d->vel.x*dir.y - d->vel.y*dir.x);
                            if(risky && !nb.commit && along <= -3 && speed >= 100 && across < 25) nb.commit = true; // fast and straight enough on its way: on to the edge
                            else if((risky && nb.commit) || (!(risky && along <= -3) && speed >= (risky ? 110 : 60) && across < (risky ? 25 : 40)))
                            { // on its way: it runs off the edge (its keys in the air carry it further than the simulated player, who let go at the edge)
                                nb.jumped = true;
                                // over a deadly fall the grid's player only just reached that floor: a jump off the edge (as a
                                // player does), if it comes down on that floor
                                if(risky && navbotleaplands(d, to)) { nb.leap = true; nb.launchat = lastmillis; d->jumping = true; }
                            }
                            else if(nb.runups < 2 && nb.runupat.dist(a) >= 1) { nb.runup = true; nb.runups++; nb.phaseat = lastmillis; d->move = d->strafe = 0; }
                            else { d->move = d->strafe = 0; navbotfailed(d, b, nb); return; }
                        }
                    }
                    break;
                case NAV_JUMP:
                {
                    const vec &a = nb.path[nb.aim - 1].o;
                    navbotturn(d, yaw, 720); // a jump onto a ledge as simulated (forward key): its view along it for the takeoff, even while it fights (free again in the air)
                    if(!nb.ready)
                    { // onto its node first: the grid jumped from there
                        navbotkeysto(d, navbotyawof(a.x - feet.x, a.y - feet.y));
                        if(navbotdist2(feet, a) < 2.5f*2.5f || (lastmillis - nb.phaseat > 1000 && navbotdist2(feet, a) < 6*6)) nb.ready = true;
                        else if(lastmillis - nb.phaseat > 1000) { d->move = d->strafe = 0; navbotfailed(d, b, nb); return; } // it could not get there: not a jump from elsewhere, another way
                        else break;
                    }
                    navbotkeysto(d, yaw);
                    if(!nb.jumped && navbotonfloor(d))
                    { // its speed along the ledge first (it may come at it sideways), and its view along the jump (forward
                      // key only): the grid jumped running straight at it
                        vec dir = navbotdir(to);
                        if((fabs(d->vel.x*dir.y - d->vel.y*dir.x) > 40 || fabs(navbotwrap(d->yaw - yaw)) > 25) && lastmillis - nb.phaseat < 1200) break;
                    }
                    if(nb.jumped || navbotonfloor(d)) navbotjumpnow(d, nb, feet);
                    if(nb.jumped && navbotonfloor(d) && lastmillis - nb.launchat > 400) nb.jumped = false; // it did not leave the floor: again
                    break;
                }
                case NAV_GAP:
                {
                    navbotturn(d, yaw, 720); // its full speed needs the forward key: its view along the jump, even while it fights
                    if(nb.runup) { navbotkeysto(d, navbotyawof(nb.runupat.x - feet.x, nb.runupat.y - feet.y)); break; }
                    navbotkeysto(d, yaw);
                    const vec &a = nb.path[nb.aim - 1].o;
                    vec dir = navbotdir(to), rel = vec(feet).sub(a);
                    float along = rel.x*dir.x + rel.y*dir.y, side = fabs(rel.x*dir.y - rel.y*dir.x), speed = d->vel.x*dir.x + d->vel.y*dir.y;
                    if(nb.jumped) navbotjumpnow(d, nb, feet);
                    else if(navbotonfloor(d) && botnavsafe && to.risky && !nb.commit && along > -10 && along <= -3 && side < 6)
                    { // over a deadly fall: it makes up its mind 10 u before its node, while it can still stop before the edge
                        if(speed >= 100 && fabs(d->vel.x*dir.y - d->vel.y*dir.x) < 25 && fabs(navbotwrap(d->yaw - yaw)) < 20) nb.commit = true;
                        else if(nb.runups < 2 && nb.runupat.dist(a) >= 1) { nb.runup = true; nb.runups++; nb.phaseat = lastmillis; d->move = d->strafe = 0; } // too slow for that gap: a run-up first
                        else { d->move = d->strafe = 0; navbotfailed(d, b, nb); return; } // no room for a run-up: another way, before the edge
                    }
                    else if(navbotonfloor(d) && along > -3 && side < 6)
                    {
                        float across = fabs(d->vel.x*dir.y - d->vel.y*dir.x);
                        bool risky = botnavsafe && to.risky; // over a deadly fall: as fast and as straight as the grid ran (or on its way since 10 u before)
                        if((risky && nb.commit) || (speed >= (risky ? 115 : 105) && across < (risky ? 25 : 35) && fabs(navbotwrap(d->yaw - yaw)) < 20)) navbotjumpnow(d, nb, feet);
                        else if(nb.runups < 2 && nb.runupat.dist(a) >= 1) { nb.runup = true; nb.runups++; nb.phaseat = lastmillis; d->move = d->strafe = 0; } // too slow for that gap: a run-up first
                        else { d->move = d->strafe = 0; navbotfailed(d, b, nb); return; } // no room for a run-up, or still too slow: it would fall in, another way
                    }
                    break;
                }
                case NAV_SWIM:
                    if(!engaged) navbotturn(d, yaw, 540);
                    d->move = 1; d->strafe = 0;
                    if(d->inwater && (lookupmaterial(vec(d->o.x, d->o.y, d->o.z + 2))&MATF_VOLUME) != MAT_WATER) { navbotjumpnow(d, nb, feet); d->pitch = 0; }
                    else d->pitch = 60;
                    break;
                case NAV_PADIN: case NAV_TELEIN:
                    navbotkeysto(d, navbotyawof(to.o.x - feet.x, to.o.y - feet.y));
                    break;
            }
            return;
        }
        // NBP_AIR: the keys of its plan (as the grid simulated them), then a correction to land on the node
        if(d->inwater && to.type != NAV_SWIM) return;
        d->ai->dontmove = false;
        d->ai->becareful = false;
        int t = lastmillis - nb.launchat;
        bool keys = false;
        switch(to.t1)
        {
            case NAVT_LANDING: keys = t >= to.t0*10; break;
            case NAVT_TAKEOFF: keys = false; break;
            case NAVT_OVER: keys = navbotdist2(feet, nb.takeoff) < float(to.t0)*to.t0; break;
            default: keys = t >= to.t0*10 && t < to.t1*10; break;
        }
        if(to.type == NAV_TELE) keys = false;
        if(to.type != NAV_TELE && to.type != NAV_SWIM && (!engaged || (botnavsafe && to.risky))) navbotturn(d, yaw, 720); // it looks where it flies (forward key only, as simulated), unless it fights (over a deadly fall: always)
        if(keys) navbotkeysto(d, yaw); else d->move = d->strafe = 0;
        if(!keys && to.type == NAV_PAD && t < to.t0*10 && nb.launchvel.x*nb.launchvel.x + nb.launchvel.y*nb.launchvel.y < 20*20 && nb.aim >= 1)
        { // a pad that throws straight up, the keys of the plan later: the grid flew from the middle of the pad, it gets
          // back over it meanwhile (the pad threw it from wherever it touched it)
            const vec &c = nb.path[nb.aim - 1].o;
            vec stop = vec(d->vel.x, d->vel.y, 0).mul(0.59f).add(feet); // where it would drift to with no keys (air friction)
            if(navbotdist2(stop, c) > 2*2) navbotkeysto(d, navbotyawof(c.x - stop.x, c.y - stop.y));
        }
        if(to.type == NAV_SWIM) { d->pitch = 0; if(d->inwater) { d->move = 1; d->strafe = 0; } }
        float vz = d->vel.z + d->falling.z;
        if(!d->inwater && to.type != NAV_TELE && ((vz < 0 && feet.z > to.o.z + 1) || to.type == NAV_DROP || to.type == NAV_GAP || engaged || (botnavsafe && to.risky))) navbotairkeys(d, botnavsafe && to.risky ? navbotlandgoal(nb) : to.o); // across a void, or its view on an enemy: corrected from the start
    }

    // ---- jumps on the grid: only those its links ask for (navbotkeys), none at a step; Tiny Apa keeps its own (a few at
    // random, as measured on its reference player), never next to an edge, a link, a jump pad or a teleporter
    bool navbotjump(fpsent *d, aistate &b)
    {
        navbot *pnb = navbotof(d);
        if(!pnb || !navbothaspath(d, *pnb)) return false;
        navbot &nb = *pnb;
        if(nb.phase != NBP_WALK || !navbotonfloor(d) || d->inwater) return true;
        if(nb.stuck >= 2 && lastmillis >= d->ai->jumpseed) { d->jumping = true; d->ai->jumpseed = lastmillis + 800 + rnd(400); return true; } // stuck: a jump to get free
        if(navbotmode(d) < 2) return true;
        if(navnodes.inrange(nb.node) && navnodes[nb.node].flags&NAVF_EDGE) return true;
        loopk(2) if(nb.aim + 1 + k < nb.path.length() && navbotlinktype(nb.path[nb.aim + 1 + k].type) && navbotdist2(d->feetpos(), nb.path[nb.aim + k].o) < 64*64) return true;
        if(lastmillis < d->ai->jumpseed || lastmillis < d->ai->jumprand) return true;
        loopv(navtriggers) if(navtriggers[i].o.squaredist(d->feetpos()) < 32*32) return true;
        if(!navbotstylejump(d, b)) return true;
        d->jumping = true;
        int seed = (111 - d->skill)*5;
        d->ai->jumpseed = lastmillis + seed + rnd(seed);
        seed *= b.idle ? 50 : 25;
        d->ai->jumprand = lastmillis + seed + rnd(seed);
        return true;
    }

    // ---- stuck on the grid (keys held, going nowhere): a new path, then a jump and a new path, then another goal; never
    // a suicide (the waypoints' timers are not used on the grid)
    bool navbottimeouts(fpsent *d, aistate &b)
    {
        navbot *pnb = navbotof(d);
        if(!pnb) return false;
        navbot &nb = *pnb;
        if(!navbothaspath(d, nb) && !navbotready() && !(navlocal && navgeneration > 0)) return false; // the waypoints, as before
        d->ai->blocktime = d->ai->blockseq = d->ai->targtime = d->ai->targseq = 0;
        d->ai->lasthunt = lastmillis;
        vec feet = d->feetpos();
        if(!navbothaspath(d, nb) || nb.phase != NBP_WALK || navbotstyle(d)&3 || (!(d->move || d->strafe) && !d->blocked) || navbotdist2(feet, nb.progpos) > NAVB_HEADWAY*NAVB_HEADWAY || fabs(feet.z - nb.progpos.z) > 16)
        {
            if(navbotdist2(feet, nb.progpos) > NAVB_HEADWAY*NAVB_HEADWAY && nb.stuck > 0 && lastmillis - nb.progat > 400) nb.stuck--;
            nb.progat = lastmillis; nb.progpos = feet;
            return true;
        }
        if(lastmillis - nb.progat < NAVB_STUCK) return true;
        nb.stuck++;
        nb.progat = lastmillis; nb.progpos = feet;
        if(nb.stuck == 1 || nb.stuck == 3) navbotreplan(d, b, nb);
        else if(nb.stuck >= 4) { nb.stuck = 0; navbotdrop(d, nb); d->ai->reset(true); }
        return true;
    }


    // ---- what the walk grid tells the moves network of Tiny Apa (ainet.h, botnet 1) of a bot on it: 2 a link of its way is
    // being taken, or one that needs its full speed (a gap, a drop across a void) starts within 48 u (its run straight at it:
    // the keys stay the grid's), 4 a jump of its own is fine where it stands (on the floor, not on an edge, no link within
    // 64 u, no jump pad or teleporter within 32 u), 8 it is stuck (the grid's jump to get free goes through)
    int navbotnetinfo(fpsent *d)
    {
        navbot *pnb = navbotof(d);
        if(!pnb || !navbotready()) return 0;
        navbot &nb = *pnb;
        int r = 0;
        vec feet = d->feetpos();
        bool nearlink = false;
        if(navbothaspath(d, nb) && nb.path.inrange(nb.aim))
        {
            if(nb.phase != NBP_WALK) r |= 2;
            else if(nb.aim + 1 < nb.path.length() && (nb.path[nb.aim + 1].type == NAV_GAP || navbotflyingdrop(nb.path[nb.aim], nb.path[nb.aim + 1])) &&
                    navbotdist2(feet, nb.path[nb.aim].o) < 48*48) r |= 2;
            loopk(2) if(nb.aim + 1 + k < nb.path.length() && navbotlinktype(nb.path[nb.aim + 1 + k].type) && navbotdist2(feet, nb.path[nb.aim + k].o) < 64*64) nearlink = true;
            if(nb.stuck >= 2) r |= 8;
        }
        bool jump = !(r&2) && !nearlink && navbotonfloor(d) && !d->inwater && !(navnodes.inrange(nb.node) && navnodes[nb.node].flags&NAVF_EDGE);
        if(jump) loopv(navtriggers) if(navtriggers[i].o.squaredist(feet) < 32*32) { jump = false; break; }
        if(jump) r |= 4;
        return r;
    }
    // ... and the keys the network would hold in the air: do they bring the bot down over a deadly fall (botnavsafe), at the
    // height it took off from? (its course in the air as moveplayer() steers it: navbotpredict)
    bool navbotnetairok(fpsent *d, int move, int strafe, float z)
    {
        if(!navbotready() || !botnavsafe) return true;
        vec land;
        navbotpredict(d, move, strafe, z, land);
        return !navbotpit(land.x, land.y, z);
    }
    // ... and on a slope (a floor steeper than the physics' FLOORZ: a bot that stands still there slides down it, too slowly
    // for the guard of its steps to see): does that slope take it over a deadly fall within 96 u (botnavsafe)? downhill: the
    // way down the slope, flat and unit
    bool navbotnetslope(fpsent *d, vec &downhill)
    {
        if(!navbotready() || !botnavsafe || d->physstate != PHYS_SLOPE || d->inwater) return false;
        downhill = vec(d->floor.x, d->floor.y, 0);
        if(downhill.squaredlen() < 1e-6f) return false;
        downhill.normalize();
        vec feet = d->feetpos();
        for(float s = 4; s <= 96; s += 4)
        {
            vec q = vec(downhill).mul(s).add(feet);
            if(navbotpit(q.x, q.y, feet.z)) return true;
        }
        return false;
    }
