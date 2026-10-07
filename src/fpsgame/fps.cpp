#include "game.h"

extern float curfov, fovy, aspect;
extern int fov;

namespace game
{
    bool intermission = false;
    int maptime = 0, maprealtime = 0, maplimit = -1;
    int respawnent = -1;
    int lasthit = 0, lasthitcn = -1, lastspawnattempt = 0; // lasthitcn: who the hit crosshair is for

    int following = -1, followdir = 0;

    fpsent *player1 = NULL;         // our client
    vector<fpsent *> players;       // other clients
    int savedammo[NUMGUNS];

    bool clientoption(const char *arg) { return false; }

    void taunt()
    {
        if(player1->state!=CS_ALIVE || player1->physstate<PHYS_SLOPE) return;
        if(lastmillis-player1->lasttaunt<1000) return;
        player1->lasttaunt = lastmillis;
        addmsg(N_TAUNT, "rc", player1);
    }
    COMMAND(taunt, "");

    ICOMMAND(getfollow, "", (),
    {
        fpsent *f = followingplayer();
        intret(f ? f->clientnum : -1);
    });

	void follow(char *arg)
    {
        if(arg[0] ? player1->state==CS_SPECTATOR : following>=0)
        {
            following = arg[0] ? parseplayer(arg) : -1;
            if(following==player1->clientnum) following = -1;
            followdir = 0;
            conoutf("follow %s", following>=0 ? "on" : "off");
        }
	}
    COMMAND(follow, "s");

    void nextfollow(int dir)
    {
        if(player1->state!=CS_SPECTATOR || clients.empty())
        {
            stopfollowing();
            return;
        }
        int cur = following >= 0 ? following : (dir < 0 ? clients.length() - 1 : 0);
        loopv(clients)
        {
            cur = (cur + dir + clients.length()) % clients.length();
            if(clients[cur] && clients[cur]->state!=CS_SPECTATOR)
            {
                if(following<0) conoutf("follow on");
                following = cur;
                followdir = dir;
                return;
            }
        }
        stopfollowing();
    }
    ICOMMAND(nextfollow, "i", (int *dir), nextfollow(*dir < 0 ? -1 : 1));


    const char *getclientmap() { return clientmap; }

    void resetgamestate()
    {
        if(m_classicsp)
        {
            clearmovables();
            clearmonsters();                 // all monsters back at their spawns for editing
            entities::resettriggers();
        }
        clearprojectiles();
        clearbouncers();
    }

    fpsent *spawnstate(fpsent *d)              // reset player state not persistent accross spawns
    {
        d->respawn();
        d->spawnstate(gamemode);
        return d;
    }

    void respawnself()
    {
        if(ispaused()) return;
        if(m_mp(gamemode))
        {
            int seq = (player1->lifesequence<<16)|((lastmillis/1000)&0xFFFF);
            if(player1->respawned!=seq) { addmsg(N_TRYSPAWN, "rc", player1); player1->respawned = seq; }
        }
        else
        {
            spawnplayer(player1);
            showscores(false);
            lasthit = 0;
            if(cmode) cmode->respawned(player1);
        }
    }

    fpsent *pointatplayer()
    {
        loopv(players) if(players[i] != player1 && intersect(players[i], player1->o, worldpos)) return players[i];
        return NULL;
    }

    void stopfollowing()
    {
        if(following<0) return;
        following = -1;
        followdir = 0;
        conoutf("follow off");
    }

    fpsent *followingplayer(fpsent *fallback)
    {
        if(player1->state!=CS_SPECTATOR || following<0) return fallback;
        fpsent *target = getclient(following);
        if(target && target->state!=CS_SPECTATOR) return target;
        return fallback;
    }

    fpsent *hudplayer()
    {
        if(thirdperson && allowthirdperson()) return player1;
        return followingplayer(player1);
    }

    void setupcamera()
    {
        demohd::tryautofollow();
        fpsent *target = followingplayer();
        if(!target && demoplayback)
        {
            int cn = demohd::recordercn();
            if(cn >= 0) target = getclient(cn);
            if(!target) target = player1;
        }
        if(target)
        {
            if(demohd::applycamera(target))
            {
                demohd::applyothers(target);
                return;
            }
            if(target != player1)
            {
                player1->yaw = target->yaw;
                player1->pitch = target->state==CS_DEAD ? 0 : target->pitch;
                player1->o = target->o;
                player1->resetinterp();
            }
        }
        demohd::applyothers(target);
    }

    bool allowthirdperson(bool msg)
    {
        return player1->state==CS_SPECTATOR || player1->state==CS_EDITING || m_edit || !multiplayer(msg);
    }
    ICOMMAND(allowthirdperson, "b", (int *msg), intret(allowthirdperson(*msg!=0) ? 1 : 0));

    bool detachcamera()
    {
        fpsent *d = hudplayer();
        return d->state==CS_DEAD;
    }

    bool collidecamera()
    {
        switch(player1->state)
        {
            case CS_EDITING: return false;
            case CS_SPECTATOR: return followingplayer()!=NULL;
        }
        return true;
    }

    VARP(smoothmove, 0, 75, 100);
    VARP(smoothdist, 0, 32, 64);

    void predictplayer(fpsent *d, bool move)
    {
        d->o = d->newpos;
        d->yaw = d->newyaw;
        d->pitch = d->newpitch;
        d->roll = d->newroll;
        if(move)
        {
            moveplayer(d, 1, false);
            d->newpos = d->o;
        }
        float k = 1.0f - float(lastmillis - d->smoothmillis)/smoothmove;
        if(k>0)
        {
            d->o.add(vec(d->deltapos).mul(k));
            d->yaw += d->deltayaw*k;
            if(d->yaw<0) d->yaw += 360;
            else if(d->yaw>=360) d->yaw -= 360;
            d->pitch += d->deltapitch*k;
            d->roll += d->deltaroll*k;
        }
    }

    void otherplayers(int curtime)
    {
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == player1 || d->ai) continue;

            if(d->state==CS_DEAD && d->ragdoll) moveragdoll(d);
            else if(!intermission)
            {
                if(lastmillis - d->lastaction >= d->gunwait) d->gunwait = 0;
                if(d->quadmillis) entities::checkquad(curtime, d);
            }

            const int lagtime = totalmillis-d->lastupdate;
            if(!lagtime || intermission) continue;
            else if(lagtime>1000 && d->state==CS_ALIVE)
            {
                d->state = CS_LAGGED;
                continue;
            }
            if(d->state==CS_ALIVE || d->state==CS_EDITING)
            {
                if(demoplayback && demohd::hastrack(d->clientnum)) d->smoothmillis = 0;
                else if(smoothmove && d->smoothmillis>0) predictplayer(d, true);
                else moveplayer(d, 1, false);
            }
            else if(d->state==CS_DEAD && !d->ragdoll && lastmillis-d->lastpain<2000) moveplayer(d, 1, true);
        }
    }

    VARFP(slowmosp, 0, 0, 1, { if(m_sp && !slowmosp) server::forcegamespeed(100); });

    void checkslowmo()
    {
        static int lastslowmohealth = 0;
        server::forcegamespeed(intermission ? 100 : clamp(player1->health, 25, 200));
        if(player1->health<player1->maxhealth && lastmillis-max(maptime, lastslowmohealth)>player1->health*player1->health/2)
        {
            lastslowmohealth = lastmillis;
            player1->health++;
        }
    }

    static void updatefragreact();
    static void pollfragreact();
    static void clearfragreact();
    void fragreactspawned(fpsent *d);

    void updateworld()        // main game update loop
    {
        if(!maptime) { maptime = lastmillis; maprealtime = totalmillis; return; }
        if(!curtime) { gets2c(); updatespecqueue(); if(player1->clientnum>=0) c2sinfo(); return; }

        physicsframe();
        ai::navigate();
        if(player1->state != CS_DEAD && !intermission)
        {
            if(player1->quadmillis) entities::checkquad(curtime, player1);
        }
        otherplayers(curtime);
        updatefragreact();
        updateweapons(curtime);
        ai::update();
        moveragdolls();
        gets2c();
        updatespecqueue();
        updatemovables(curtime);
        updatemonsters(curtime);
        if(connected)
        {
            if(player1->state == CS_DEAD)
            {
                if(player1->ragdoll) moveragdoll(player1);
                else if(lastmillis-player1->lastpain<2000)
                {
                    player1->move = player1->strafe = 0;
                    moveplayer(player1, 10, true);
                }
            }
            else if(!intermission)
            {
                if(player1->ragdoll) cleanragdoll(player1);
                moveplayer(player1, 10, true);
                swayhudgun(curtime);
                entities::checkitems(player1);
                if(m_sp)
                {
                    if(slowmosp) checkslowmo();
                    if(m_classicsp) entities::checktriggers();
                }
                else if(cmode) cmode->checkitems(player1);
            }
        }
        pollfragreact();
        if(player1->clientnum>=0) c2sinfo();   // do this last, to reduce the effective frame lag
    }

    float proximityscore(float x, float lower, float upper)
    {
        if(x <= lower) return 1.0f;
        if(x >= upper) return 0.0f;
        float a = x - lower, b = x - upper;
        return (b * b) / (a * a + b * b);
    }

    static inline float harmonicmean(float a, float b) { return a + b > 0 ? 2 * a * b / (a + b) : 0.0f; }

    // avoid spawning near other players
    float ratespawn(dynent *d, const extentity &e)
    {
        fpsent *p = (fpsent *)d;
        vec loc = vec(e.o).addz(p->eyeheight);
        float maxrange = !m_noitems ? 400.0f : (cmode ? 300.0f : 110.0f);
        float minplayerdist = maxrange;
        loopv(players)
        {
            const fpsent *o = players[i];
            if(o == p)
            {
                if(m_noitems || (o->state != CS_ALIVE && lastmillis - o->lastpain > 3000)) continue;
            }
            else if(o->state != CS_ALIVE || isteam(o->team, p->team)) continue;

            vec dir = vec(o->o).sub(loc);
            float dist = dir.squaredlen();
            if(dist >= minplayerdist*minplayerdist) continue;
            dist = sqrtf(dist);
            dir.mul(1/dist);

            // scale actual distance if not in line of sight
            if(raycube(loc, dir, dist) < dist) dist *= 1.5f;
            minplayerdist = min(minplayerdist, dist);
        }
        float rating = 1.0f - proximityscore(minplayerdist, 80.0f, maxrange);
        return cmode ? harmonicmean(rating, cmode->ratespawn(p, e)) : rating;
    }

    void pickgamespawn(fpsent *d)
    {
        int ent = m_classicsp && d == player1 && respawnent >= 0 ? respawnent : -1;
        int tag = cmode ? cmode->getspawngroup(d) : 0;
        findplayerspawn(d, ent, tag);
    }

    void spawnplayer(fpsent *d)   // place at random spawn
    {
        pickgamespawn(d);
        spawnstate(d);
        if(d==player1)
        {
            if(editmode) d->state = CS_EDITING;
            else if(d->state != CS_SPECTATOR) d->state = CS_ALIVE;
        }
        else d->state = CS_ALIVE;
        fragreactspawned(d);
    }

    VARP(spawnwait, 0, 0, 1000);

    void respawn()
    {
        if(player1->state==CS_DEAD)
        {
            player1->attacking = false;
            int wait = cmode ? cmode->respawnwait(player1) : 0;
            if(wait>0)
            {
                lastspawnattempt = lastmillis;
                //conoutf(CON_GAMEINFO, "\f2you must wait %d second%s before respawn!", wait, wait!=1 ? "s" : "");
                return;
            }
            if(lastmillis < player1->lastpain + spawnwait) return;
            if(m_dmsp) { changemap(clientmap, gamemode); return; }    // if we die in SP we try the same map again
            respawnself();
            if(m_classicsp)
            {
                conoutf(CON_GAMEINFO, "\f2You wasted another life! The monsters stole your armour and some ammo...");
                loopi(NUMGUNS) if(i!=GUN_PISTOL && (player1->ammo[i] = savedammo[i]) > 5) player1->ammo[i] = max(player1->ammo[i]/3, 5);
            }
        }
    }
    COMMAND(respawn, "");

    // inputs

    VARP(attackspawn, 0, 1, 1);

    void doattack(bool on)
    {
        if(!connected || intermission) return;
        if((player1->attacking = on) && attackspawn) respawn();
    }

    // Cubescript `attack` is type D: a scripted `attack 1` is rewritten to 0
    // unless a key-release action can be queued. Lab sequences need a real
    // projectile without that bind path.
    ICOMMAND(hwrtlabattack, "i", (int *on), { doattack(*on!=0); });

    VARP(jumpspawn, 0, 1, 1);

    bool canjump()
    {
        if(!connected || intermission) return false;
        if(jumpspawn) respawn();
        return player1->state!=CS_DEAD;
    }

    bool allowmove(physent *d)
    {
        if(d->type!=ENT_PLAYER) return true;
        return !((fpsent *)d)->lasttaunt || lastmillis-((fpsent *)d)->lasttaunt>=1000;
    }

    VARP(hitsound, 0, 0, 1);

    void damaged(int damage, fpsent *d, fpsent *actor, bool local)
    {
        if((d->state!=CS_ALIVE && d->state != CS_LAGGED && d->state != CS_SPAWNING) || intermission) return;

        if(local) damage = d->dodamage(damage);
        else if(actor==player1) return;

        if(!local && actor && actor != d && actor->ownernum != player1->clientnum && !isteam(actor->team, d->team))
            actor->totaldamage += damage;

        fpsent *h = hudplayer();
        if(h!=player1 && actor==h && d!=actor)
        {
            if(hitsound && lasthit != lastmillis) playsound(S_HIT);
            lasthit = lastmillis;
            lasthitcn = d->clientnum;
        }
        if(d==h)
        {
            damageblend(damage);
            damagecompass(damage, actor->o);
        }
        damageeffect(damage, d, d!=h);

		ai::damaged(d, actor);

        if(m_sp && slowmosp && d==player1 && d->health < 1) d->health = 1;

        if(d->health<=0) { if(local) killed(d, actor); }
        else if(d==h) playsound(S_PAIN6);
        else playsound(S_PAIN1+rnd(5), &d->o);
    }

    VARP(deathscore, 0, 1, 1);

    void deathstate(fpsent *d, bool restore)
    {
        d->state = CS_DEAD;
        d->lastpain = lastmillis;
        if(!restore)
        {
            gibeffect(max(-d->health, 0), d->vel, d);
            d->deaths++;
        }
        if(d==player1)
        {
            if(deathscore) showscores(true);
            disablezoom();
            if(!restore) loopi(NUMGUNS) savedammo[i] = player1->ammo[i];
            d->attacking = false;
            d->roll = 0;
            playsound(S_DIE1+rnd(2));
            clearfragreact();
        }
        else
        {
            d->move = d->strafe = 0;
            d->resetinterp();
            d->smoothmillis = 0;
            playsound(S_DIE1+rnd(2), &d->o);
        }
    }

    VARP(teamcolorfrags, 0, 1, 1);

    VARP(killfeed, 0, 1, 1);
    VARP(killfeedconsole, 0, 0, 1);
    VARP(killfeedfilter, 0, 0, 2); // 0 = all, 1 = team, 2 = mine
    VARP(killfeedalign, -1, -1, 1);
    FVARP(killfeedx, 0, 0.02f, 1);
    FVARP(killfeedy, 0, 0.40f, 1);
    FVARP(killfeedscale, 0.2f, 0.5f, 1.5f);
    VARP(killfeedfade, 1, 5, 20);
    VARP(killfeedmax, 1, 5, 5);
    VARP(reacttime, 0, 1, 1); // vue / viseur ms on your kills and deaths
    VARP(reacttimeall, 0, 0, 1); // also other players' frags, as sent by their killer (compatible server)

    VARP(killstreak, 0, 1, 1);
    VARP(killstreakothers, 0, 1, 1);
    VARP(killstreaktk, 0, 1, 1); // 1 = teamkill breaks the streak
    VARP(killstreak1, 0, 0, 50); // leftover, ignored (old saves)
    VARP(killstreak2, 0, 0, 50);
    VARP(killstreak3, 0, 0, 50);
    VARP(killstreak4, 0, 0, 50);
    VARP(killstreakstep, 0, 5, 50); // announce every N frags (5, 10, 15…)
    VARP(killstreakfade, 1, 3, 10);
    FVARP(killstreakscale, 0.2f, 0.85f, 2.0f);
    FVARP(killstreakx, 0, 0.50f, 1);
    FVARP(killstreaky, 0, 0.18f, 1);
    VARP(killstreakalign, -1, 0, 1);

    VAR(fullkilllog, 0, 0, 1);
    VARP(fullkilllogsize, 0, 75, 100);

    struct killfeedline
    {
        string killer, victim;
        int gun;
        bool suicide, teamkill, mine, myteam;
        string actorteam;   // the filter follows whoever you watch, so it is decided when drawn
        int vsak, vsvk;     // kills of the actor on the victim and back, at the time of this frag
        int millis;
        int actorcn, victimcn;
        int fovms, xhms, waitmillis;
        bool showreact, approx, pending;
    };
    static vector<killfeedline> killfeedlines;

    struct kdlogline
    {
        string text;
        int actorcn, victimcn;
        int fovms, xhms, waitmillis;
        bool showreact, approx, pending;
    };
    static vector<kdlogline> kdlog, kdprev;
    static string kdlogtitle, kdprevtitle;
    static int killlogskip = 0;

    struct streakpopup
    {
        string text;
        int millis;
        bool mine;
    };
    static vector<streakpopup> streakpopups;

    // frags between two other players, so that one relayed react time fills at most one frag
    struct recentfrag { int actorcn, victimcn, millis; bool done; };
    static vector<recentfrag> recentfrags;

    static void clearkillfeed() { killfeedlines.setsize(0); recentfrags.setsize(0); }

    // per-opponent score for this match: "kills-deaths" against that player
    VARP(killfeedvs, 0, 1, 1);

    // every "a killed b" of the match, by client number, so the counter can be
    // shown from whoever you watch (spectating / demo), not only from you
    struct vspair { int actor, victim, n; };
    static vector<vspair> vspairs;

    static int vscount(int actor, int victim)
    {
        loopv(vspairs) if(vspairs[i].actor == actor && vspairs[i].victim == victim) return vspairs[i].n;
        return 0;
    }

    static void countvs(fpsent *victim, fpsent *actor, bool suicide)
    {
        if(suicide || actor->type!=ENT_PLAYER || victim->type!=ENT_PLAYER) return;
        if(actor->clientnum < 0 || victim->clientnum < 0) return;
        loopv(vspairs) if(vspairs[i].actor == actor->clientnum && vspairs[i].victim == victim->clientnum) { vspairs[i].n++; return; }
        vspair &p = vspairs.add();
        p.actor = actor->clientnum;
        p.victim = victim->clientnum;
        p.n = 1;
    }

    static void forgetvs(int cn) // the cn is reused by the next player who joins
    {
        loopv(vspairs) if(vspairs[i].actor == cn || vspairs[i].victim == cn) vspairs.remove(i--);
    }

    // point of view: the player you follow, else in your demo the recorder, else you
    static fpsent *vsviewer()
    {
        if(fpsent *f = followingplayer()) return f;
        if(demoplayback && demohd::recordercn() >= 0)
        {
            fpsent *r = getclient(demohd::recordercn());
            if(r) return r;
        }
        return player1;
    }

    static const char *vsscorefrom(fpsent *viewer, int cn)
    {
        static string buf;
        buf[0] = 0;
        if(!viewer || viewer->clientnum < 0 || cn < 0 || cn == viewer->clientnum) return buf;
        int k = vscount(viewer->clientnum, cn), d = vscount(cn, viewer->clientnum);
        if(k || d) formatstring(buf, "\f0%d\f7-\f3%d", k, d);
        return buf;
    }

    const char *vsscore(fpsent *d)
    {
        if(!d || d->state==CS_SPECTATOR) return "";
        return vsscorefrom(vsviewer(), d->clientnum);
    }

    // killfeed line: the score of the watched player against the other one
    static void clearstreakpopups() { streakpopups.setsize(0); }

    static bool isstreaktier(int n)
    {
        return killstreakstep > 0 && n >= killstreakstep && (n % killstreakstep) == 0;
    }

    static void addstreakpopup(const char *text, bool mine)
    {
        while(streakpopups.length() >= 2) streakpopups.remove(0);
        streakpopup &p = streakpopups.add();
        copystring(p.text, text);
        p.millis = lastmillis;
        p.mine = mine;
    }

    static const char *streakcolorname(fpsent *d)
    {
        if(d == player1 && selfhascolor())
        {
            static string pre;
            formatstring(pre, "\fs\f%c", selfcolcode());
            return colorname(d, NULL, pre, "\fr", "you");
        }
        if(d && isfriend(d) && friendhascolor(isfriendally(d)))
        {
            static string pre;
            formatstring(pre, "\fs\f%c", friendcolcode(isfriendally(d)));
            return colorname(d, NULL, pre, "\fr", "you");
        }
        if(m_teammode)
            return colorname(d, NULL, isteam(d->team, player1->team) ? "\fs\f1" : "\fs\f3", "\fr", "you");
        return colorname(d, NULL, "", "", "you");
    }

    static void updatekillstreak(fpsent *victim, fpsent *actor, bool suicide, bool teamkill)
    {
        if(!victim) return;
        if(suicide)
        {
            victim->killstreak = 0;
            return;
        }
        if(actor && actor->type != ENT_INANIMATE)
        {
            if(teamkill)
            {
                if(killstreaktk) actor->killstreak = 0;
            }
            else
            {
                actor->killstreak++;
                if(killstreak && isstreaktier(actor->killstreak))
                {
                    bool mine = actor == vsviewer();
                    if(mine || killstreakothers)
                    {
                        string buf;
                        formatstring(buf, "%s  %d STREAK", streakcolorname(actor), actor->killstreak);
                        addstreakpopup(buf, mine);
                    }
                }
            }
        }
        victim->killstreak = 0;
    }

    static const char *gunabbrev(int gun)
    {
        switch(gun)
        {
            case GUN_FIST: return "fist";
            case GUN_SG: return "SG";
            case GUN_CG: return "CG";
            case GUN_RL: return "RL";
            case GUN_RIFLE: return "rifle";
            case GUN_GL: return "GL";
            case GUN_PISTOL: return "pistol";
            default: return NULL;
        }
    }

    static void colorfeedname(fpsent *d, char *dst)
    {
        if(d == player1)
        {
            if(selfhascolor())
                nformatstring(dst, MAXSTRLEN, "\fs\f%c%s\fr", selfcolcode(), colorname(d, NULL, "", "", "you"));
            else copystring(dst, colorname(d, NULL, "", "", "you"), MAXSTRLEN);
            return;
        }
        if(isfriend(d) && friendhascolor(isfriendally(d)))
        {
            string pre;
            formatstring(pre, "\fs\f%c", friendcolcode(isfriendally(d)));
            copystring(dst, colorname(d, NULL, pre, "\fr", "you"), MAXSTRLEN);
            return;
        }
        if(m_teammode && teamcolorfrags) copystring(dst, teamcolorname(d, "you"), MAXSTRLEN);
        else copystring(dst, colorname(d, NULL, "", "", "you"), MAXSTRLEN);
    }

    static const int FRAGRT_CN = 256, FRAGRT_WAIT = 400, FRAGRT_GRACE = 400, FRAGRT_MAXMS = 60000;

    struct reactstreak { int enter, last, ended; };
    static reactstreak react_myfov[FRAGRT_CN], react_myxh[FRAGRT_CN];
    static reactstreak react_theirfov[FRAGRT_CN], react_theirxh[FRAGRT_CN];
    static int react_hitfov[FRAGRT_CN], react_hitxh[FRAGRT_CN], react_hittime[FRAGRT_CN];

    static int react_born = 0;
    static fpsent *react_viewer = NULL; // whose eyes the react times are measured from

    static void reactclear(reactstreak &s) { s.enter = s.last = s.ended = 0; }

    static void clearfragreactcn(int cn)
    {
        if(cn < 0 || cn >= FRAGRT_CN) return;
        reactclear(react_myfov[cn]);
        reactclear(react_myxh[cn]);
        reactclear(react_theirfov[cn]);
        reactclear(react_theirxh[cn]);
        react_hitfov[cn] = react_hitxh[cn] = react_hittime[cn] = 0;
    }

    static void clearfragreact()
    {
        loopi(FRAGRT_CN) clearfragreactcn(i);
        react_born = lastmillis;
    }

    void fragreactspawned(fpsent *d)
    {
        if(!d) return;
        if(d == player1 || d == react_viewer) clearfragreact();
        else clearfragreactcn(d->clientnum);
    }

    static float reactangdiff(float a, float b)
    {
        float d = fmodf(fabsf(a - b), 360.0f);
        if(d > 180.0f) d = 360.0f - d;
        return d;
    }

    static vec reactchest(fpsent *t)
    {
        vec p = t->o;
        p.z -= t->eyeheight * 0.45f;
        return p;
    }

    // Eye often sits inside a wall/ceiling cube; skip that cube like the gun trace.
    static bool reactlos(const vec &from, const vec &to)
    {
        vec dir = vec(to).sub(from);
        float mag = dir.magnitude();
        if(mag < 0.5f) return true;
        dir.mul(1.0f/mag);
        float dist = raycube(from, dir, mag, RAY_CLIPMAT|RAY_ALPHAPOLY|RAY_SKIPFIRST);
        return dist < 0 || dist >= mag - 0.35f;
    }

    static bool reactlosbody(const vec &from, fpsent *t)
    {
        vec chest = reactchest(t), hips = t->o;
        hips.z -= t->eyeheight * 0.85f;
        return reactlos(from, t->o) || reactlos(from, chest) || reactlos(from, hips);
    }

    static bool reactincone(const vec &eye, float yaw, float pitch, float hfov, float vfov, fpsent *t)
    {
        vec delta = reactchest(t).sub(eye);
        float dist = delta.magnitude();
        if(dist < 1.0f) return true;
        float tyaw, tpitch;
        vectoyawpitch(delta, tyaw, tpitch);
        float ypad = atan2f(t->radius + 4.0f, dist)/RAD;
        float ppad = atan2f(t->eyeheight*0.65f + t->aboveeye + 4.0f, dist)/RAD;
        return reactangdiff(tyaw, yaw) <= hfov*0.5f + ypad && reactangdiff(tpitch, pitch) <= vfov*0.5f + ppad;
    }

    static bool reactxhair(const vec &eye, float yaw, float pitch, fpsent *t)
    {
        vec dir;
        vecfromyawpitch(yaw, pitch, 1, 0, dir);
        float range = float(max(getworldsize()*2, 1024));
        vec to = vec(dir).mul(range).add(eye);
        float frac = 1;
        if(!intersect(t, eye, to, frac)) return false;
        float pdist = range * frac;
        if(pdist <= 1.0f) return true;
        float wdist = raycube(eye, dir, pdist, RAY_CLIPMAT|RAY_ALPHAPOLY|RAY_SKIPFIRST);
        return wdist < 0 || wdist >= pdist - 0.35f;
    }

    static bool reactmyxhair(fpsent *t)
    {
        // Same segment as the local shot (eye → last center-screen world hit).
        if(intersect(t, player1->o, worldpos)) return true;
        return reactxhair(player1->o, player1->yaw, player1->pitch, t);
    }

    static void reactset(reactstreak &s, bool inside)
    {
        if(inside)
        {
            if(!s.enter) s.enter = lastmillis;
        }
        else if(s.enter)
        {
            s.last = max(lastmillis - s.enter, 1);
            s.ended = lastmillis;
            s.enter = 0;
        }
    }

    static int reactms(const reactstreak &s, int grace = 0)
    {
        int enter = s.enter;
        if(react_born && enter && enter < react_born) enter = 0;
        if(enter) return clamp(max(lastmillis - enter, 1), 1, FRAGRT_MAXMS);
        if(react_born && s.ended && s.ended < react_born) return 0;
        if(grace > 0 && s.ended && lastmillis - s.ended <= grace && s.last > 0)
            return clamp(s.last, 1, FRAGRT_MAXMS);
        return 0;
    }

    static void trackonefragreact(fpsent *v, fpsent *d, float myhfov, float myvfov, float thfov, float tvfov)
    {
        int cn = d->clientnum;
        bool xh = v == player1 ? reactmyxhair(d) : reactxhair(v->o, v->yaw, v->pitch, d);
        bool seen = xh || (reactincone(v->o, v->yaw, v->pitch, myhfov, myvfov, d) && reactlosbody(v->o, d));
        reactset(react_myfov[cn], seen);
        reactset(react_myxh[cn], xh);

        bool txh = reactxhair(d->o, d->yaw, d->pitch, v);
        bool tseen = txh || (reactincone(d->o, d->yaw, d->pitch, thfov, tvfov, v) && reactlosbody(d->o, v));
        reactset(react_theirfov[cn], tseen);
        reactset(react_theirxh[cn], txh);
    }

    static void fragreactfovs(float &myhfov, float &myvfov, float &thfov, float &tvfov)
    {
        myhfov = curfov > 10 ? curfov : (fov > 10 ? float(fov) : 100.0f);
        myvfov = fovy > 10 ? fovy : myhfov * 0.75f;
        thfov = 100.0f;
        tvfov = 75.0f;
        if(aspect > 0.1f) tvfov = 2*atanf(tanf(thfov*0.5f*RAD)/aspect)/RAD;
    }

    static void samplefragreact(fpsent *other, bool frommyeyes, int &fovms, int &xhms)
    {
        fovms = xhms = 0;
        if(!other || !player1) return;
        int cn = other->clientnum;
        if(cn < 0 || cn >= FRAGRT_CN) return;
        if(frommyeyes)
        {
            fovms = reactms(react_myfov[cn], FRAGRT_GRACE);
            xhms = reactms(react_myxh[cn], FRAGRT_GRACE);
            if(react_hittime[cn] && lastmillis - react_hittime[cn] <= 2000 && (!react_born || react_hittime[cn] >= react_born))
            {
                fovms = max(fovms, react_hitfov[cn]);
                xhms = max(xhms, react_hitxh[cn]);
            }
        }
        else
        {
            fovms = reactms(react_theirfov[cn], FRAGRT_GRACE);
            xhms = reactms(react_theirxh[cn], FRAGRT_GRACE);
        }
    }

    void notefragreacthit(fpsent *target)
    {
        if(!reacttime || !target || !player1 || target == player1) return;
        int cn = target->clientnum;
        if(cn < 0 || cn >= FRAGRT_CN) return;
        if(intersect(target, player1->o, worldpos))
        {
            reactset(react_myxh[cn], true);
            reactset(react_myfov[cn], true);
        }
        react_hitfov[cn] = reactms(react_myfov[cn], FRAGRT_GRACE);
        react_hitxh[cn] = reactms(react_myxh[cn], FRAGRT_GRACE);
        react_hittime[cn] = lastmillis;
    }

    // you while you play; when you spectate or watch a demo, the player you
    // watch, but only if their aim is exact (HD), else nothing is measured
    static fpsent *reactviewerof()
    {
        if(!player1) return NULL;
        if(!demoplayback && player1->state != CS_SPECTATOR) return player1;
        fpsent *v = vsviewer();
        if(!v || v == player1 || !demohd::hdaim(v)) return NULL;
        return v;
    }

    static void updatefragreact()
    {
        fpsent *v = reacttime && !intermission && !m_edit ? reactviewerof() : NULL;
        if(v != react_viewer) { clearfragreact(); react_viewer = v; }
        if(!v || v->state != CS_ALIVE) return;

        float myhfov, myvfov, thfov, tvfov;
        fragreactfovs(myhfov, myvfov, thfov, tvfov);
        if(v != player1 && v != followingplayer()) { myhfov = thfov; myvfov = tvfov; } // not rendered from their eyes

        loopv(players)
        {
            fpsent *d = players[i];
            if(!d || d == v || d == player1) continue;
            int cn = d->clientnum;
            if(cn < 0 || cn >= FRAGRT_CN) continue;
            if(d->state != CS_ALIVE)
            {
                reactclear(react_myfov[cn]);
                reactclear(react_myxh[cn]);
                reactclear(react_theirfov[cn]);
                reactclear(react_theirxh[cn]);
                continue;
            }
            trackonefragreact(v, d, myhfov, myvfov, thfov, tvfov);
        }
    }

    static void formatreact(char *dst, int len, int fovms, int xhms, bool approx)
    {
        if(approx) nformatstring(dst, len, "\f4~%d / ~%d", fovms, xhms);
        else nformatstring(dst, len, "\f4%d / %d", fovms, xhms);
    }

    static void finishreact(int actorcn, int victimcn, int fovms, int xhms, bool approx)
    {
        int best = -1, bestms = -1;
        loopv(killfeedlines)
        {
            killfeedline &e = killfeedlines[i];
            if(!e.showreact || e.actorcn != actorcn || e.victimcn != victimcn) continue;
            if(!e.pending && !e.approx) continue;
            if(e.millis >= bestms) { bestms = e.millis; best = i; }
        }
        if(best >= 0)
        {
            killfeedline &e = killfeedlines[best];
            e.fovms = fovms;
            e.xhms = xhms;
            e.approx = approx;
            e.pending = false;
        }
        int kbest = -1;
        loopv(kdlog)
        {
            kdlogline &e = kdlog[i];
            if(!e.showreact || e.actorcn != actorcn || e.victimcn != victimcn) continue;
            if(!e.pending && !e.approx) continue;
            kbest = i;
        }
        if(kbest >= 0)
        {
            kdlogline &e = kdlog[kbest];
            e.fovms = fovms;
            e.xhms = xhms;
            e.approx = approx;
            e.pending = false;
        }
    }

    static void pollfragreact()
    {
        loopv(killfeedlines)
        {
            killfeedline &e = killfeedlines[i];
            if(!e.pending || lastmillis < e.waitmillis) continue;
            e.pending = false;
            e.approx = true;
        }
        loopv(kdlog)
        {
            kdlogline &e = kdlog[i];
            if(!e.pending || lastmillis < e.waitmillis) continue;
            e.pending = false;
            e.approx = true;
        }
    }

    void applyfragrt(int actor, int fovms, int xhms)
    {
        if(!player1) return;
        finishreact(actor, player1->clientnum, clamp(fovms, 0, FRAGRT_MAXMS), clamp(xhms, 0, FRAGRT_MAXMS), false);
    }

    static const int FRAGRTALL_MAXAGE = 3000;

    // React times of a frag between two other players, measured by the killer
    // himself: fills that frag's kill feed line (and one console line when frags
    // go to the console). Unknown, late or repeated messages are ignored.
    void applyfragrtall(int actor, int victim, int fovms, int xhms)
    {
        if(!reacttimeall || !player1 || demoplayback || !clanmodactive()) return;
        if(actor == victim || actor == player1->clientnum || victim == player1->clientnum) return;
        int best = -1;
        loopvrev(recentfrags)
        {
            recentfrag &r = recentfrags[i];
            if(lastmillis - r.millis > FRAGRTALL_MAXAGE) break;
            if(r.actorcn == actor && r.victimcn == victim && !r.done) { best = i; break; }
        }
        if(best < 0) return;
        recentfrag &r = recentfrags[best];
        r.done = true;
        fovms = clamp(fovms, 0, FRAGRT_MAXMS);
        xhms = clamp(xhms, 0, FRAGRT_MAXMS);
        loopvrev(killfeedlines)
        {
            killfeedline &e = killfeedlines[i];
            if(e.actorcn != actor || e.victimcn != victim || e.millis != r.millis) continue;
            e.fovms = fovms;
            e.xhms = xhms;
            e.showreact = true;
            e.approx = e.pending = false;
            break;
        }
        if(killfeed && !killfeedconsole) return;
        fpsent *a = getclient(actor), *v = getclient(victim);
        if(!a || !v) return;
        fpsent *h = followingplayer(player1);
        int contype = v==h || a==h ? CON_FRAG_SELF : CON_FRAG_OTHER;
        const char *aname, *vname;
        if(m_teammode && teamcolorfrags)
        {
            aname = teamcolorname(a, "you");
            vname = teamcolorname(v, "you");
        }
        else
        {
            aname = colorname(a, NULL, "", "", "you");
            vname = colorname(v, NULL, "", "", "you");
        }
        string rtxt;
        formatreact(rtxt, sizeof(rtxt), fovms, xhms, false);
        conoutf(contype, "\f2%s fragged %s %s", aname, vname, rtxt);
    }

    static void rotatekdlog()
    {
        kdprev.setsize(0);
        loopv(kdlog) kdprev.add() = kdlog[i];
        copystring(kdprevtitle, kdlogtitle);
        kdlog.setsize(0);
        kdlogtitle[0] = 0;
        killlogskip = 0;
    }

    static kdlogline &addkdlog(const char *line)
    {
        while(kdlog.length() >= 400)
        {
            kdlog.remove(0);
            if(killlogskip > 0) killlogskip--;
        }
        kdlogline &e = kdlog.add();
        copystring(e.text, line);
        e.actorcn = e.victimcn = -1;
        e.fovms = e.xhms = e.waitmillis = 0;
        e.showreact = e.approx = e.pending = false;
        return e;
    }

    static int resolvefeedgun(fpsent *actor, fpsent *victim, bool suicide)
    {
        if(victim && victim->lastwoundmillis && lastmillis - victim->lastwoundmillis < 800)
        {
            int gun = victim->lastwoundgun;
            if(gun >= GUN_FIST && gun <= GUN_PISTOL) return gun;
        }
        if(actor && actor->type != ENT_INANIMATE && actor->lastfiredmillis && lastmillis - actor->lastfiredmillis < 12000)
        {
            int gun = actor->lastfiredgun;
            if(gun >= GUN_FIST && gun <= GUN_PISTOL) return gun;
        }
        if(!suicide && actor && actor->lastattackgun >= GUN_FIST && actor->lastattackgun <= GUN_PISTOL)
            return actor->lastattackgun;
        return -1;
    }

    static void addkillfeed(fpsent *victim, fpsent *actor)
    {
        bool suicide = victim==actor || actor->type==ENT_INANIMATE;
        bool teamkill = !suicide && m_teammode && isteam(victim->team, actor->team);
        updatekillstreak(victim, actor, suicide, teamkill);
        int gun = resolvefeedgun(actor, victim, suicide);
        countvs(victim, actor, suicide);
        string vstxt; // for your kill log (F8): your own score at that moment
        vstxt[0] = 0;
        if(!suicide && killfeedvs && player1)
        {
            if(actor==player1) copystring(vstxt, vsscorefrom(player1, victim->clientnum));
            else if(victim==player1) copystring(vstxt, vsscorefrom(player1, actor->clientnum));
        }

        string vname, aname;
        colorfeedname(victim, vname);
        if(suicide) copystring(aname, vname);
        else
        {
            colorfeedname(actor, aname);
            if(killstreak && !teamkill && actor==vsviewer() && actor->killstreak >= 2)
                concformatstring(aname, " \f4x%d", actor->killstreak);
        }

        if(killfeed)
        {
            while(killfeedlines.length() >= 32) killfeedlines.remove(0);
            killfeedline &e = killfeedlines.add();
            copystring(e.victim, vname);
            copystring(e.killer, aname);
            e.suicide = suicide;
            e.teamkill = teamkill;
            e.mine = victim==player1 || actor==player1;
            e.myteam = victim==player1 || (!suicide && (actor==player1 || isteam(actor->team, player1->team)));
            copystring(e.actorteam, actor->team);
            e.vsak = vscount(actor->clientnum, victim->clientnum);
            e.vsvk = vscount(victim->clientnum, actor->clientnum);
            e.gun = gun;
            e.millis = lastmillis;
            e.actorcn = actor->clientnum;
            e.victimcn = victim->clientnum;
            e.fovms = e.xhms = e.waitmillis = 0;
            e.showreact = e.approx = e.pending = false;
        }

        if(!suicide && !teamkill && actor->type!=ENT_INANIMATE && victim!=player1 && actor!=player1)
        {
            while(recentfrags.length() >= 32) recentfrags.remove(0);
            recentfrag &r = recentfrags.add();
            r.actorcn = actor->clientnum;
            r.victimcn = victim->clientnum;
            r.millis = lastmillis;
            r.done = false;
        }

        int fovms = 0, xhms = 0, waitmillis = 0;
        bool showreact = false, approx = false, pending = false;
        fpsent *rv = react_viewer;
        if(rv && rv != player1 && reacttime && !suicide && !teamkill && (victim==rv || actor==rv) && actor->type!=ENT_INANIMATE)
        {
            // watched player (HD): their own view; the killer's view is exact only if the killer is HD too
            showreact = true;
            if(actor==rv) samplefragreact(victim, true, fovms, xhms);
            else
            {
                samplefragreact(actor, false, fovms, xhms);
                approx = !demohd::hdaim(actor);
            }
        }
        else if(reacttime && !suicide && !teamkill && (victim==player1 || actor==player1) && actor->type!=ENT_INANIMATE)
        {
            showreact = true;
            if(actor==player1)
            {
                samplefragreact(victim, true, fovms, xhms);
                if(clanmodactive() && !demoplayback && victim->aitype==AI_NONE)
                {
                    defformatstring(cmd, "%s %d %d %d", FRAGRT_CMD, victim->clientnum, fovms, xhms);
                    addmsg(N_SERVCMD, "rs", cmd);
                }
            }
            else
            {
                samplefragreact(actor, false, fovms, xhms);
                if(clanmodactive() && !demoplayback && actor->aitype==AI_NONE)
                {
                    pending = true;
                    waitmillis = lastmillis + FRAGRT_WAIT;
                }
                else approx = true;
            }
        }
        if(killfeed && killfeedlines.length())
        {
            killfeedline &e = killfeedlines.last();
            if((e.mine || showreact) && e.victimcn==victim->clientnum && e.actorcn==actor->clientnum)
            {
                e.fovms = fovms;
                e.xhms = xhms;
                e.waitmillis = waitmillis;
                e.showreact = showreact;
                e.approx = approx;
                e.pending = pending;
            }
        }

        if(victim!=player1 && actor!=player1) return;

        string line;
        const char *gname = gunabbrev(gun);
        if(suicide) formatstring(line, "%s \f4suicided", vname);
        else if(teamkill && actor==player1)
        {
            if(gname) formatstring(line, "%s \f6teamkilled\f7 %s \f4(%s)", aname, vname, gname);
            else formatstring(line, "%s \f6teamkilled\f7 %s", aname, vname);
        }
        else if(actor==player1)
        {
            if(gname) formatstring(line, "%s \f0fragged\f7 %s \f4(%s)", aname, vname, gname);
            else formatstring(line, "%s \f0fragged\f7 %s", aname, vname);
        }
        else
        {
            if(gname) formatstring(line, "%s \f3fragged\f7 %s \f4(%s)", aname, vname, gname);
            else formatstring(line, "%s \f3fragged\f7 %s", aname, vname);
        }
        if(vstxt[0]) concformatstring(line, " \f7[%s\f7]", vstxt);
        kdlogline &kd = addkdlog(line);
        kd.actorcn = actor->clientnum;
        kd.victimcn = victim->clientnum;
        kd.fovms = fovms;
        kd.xhms = xhms;
        kd.waitmillis = waitmillis;
        kd.showreact = showreact;
        kd.approx = approx;
        kd.pending = pending;
    }

    void killed(fpsent *d, fpsent *actor)
    {
        if(d->state==CS_EDITING)
        {
            d->editstate = CS_DEAD;
            d->deaths++;
            if(d!=player1) d->resetinterp();
            return;
        }
        else if((d->state!=CS_ALIVE && d->state != CS_LAGGED && d->state != CS_SPAWNING) || intermission) return;

        if(cmode) cmode->died(d, actor);

        // before the names: the feed uses colorname too, whose 3 buffers rotate
        addkillfeed(d, actor);
        fpsent *h = followingplayer(player1);
        int contype = d==h || actor==h ? CON_FRAG_SELF : CON_FRAG_OTHER;
        const char *dname = "", *aname = "";
        if(m_teammode && teamcolorfrags)
        {
            dname = teamcolorname(d, "you");
            aname = teamcolorname(actor, "you");
        }
        else
        {
            dname = colorname(d, NULL, "", "", "you");
            aname = colorname(actor, NULL, "", "", "you");
        }
        if(!killfeed || killfeedconsole)
        {
            if(actor->type==ENT_AI)
                conoutf(contype, "\f2%s got killed by %s!", dname, aname);
            else if(d==actor || actor->type==ENT_INANIMATE)
                conoutf(contype, "\f2%s suicided%s", dname, d==player1 ? "!" : "");
            else if(isteam(d->team, actor->team))
            {
                contype |= CON_TEAMKILL;
                if(actor==player1) conoutf(contype, "\f6%s fragged a teammate (%s)", aname, dname);
                else if(d==player1) conoutf(contype, "\f6%s got fragged by a teammate (%s)", dname, aname);
                else conoutf(contype, "\f2%s fragged a teammate (%s)", aname, dname);
            }
            else
            {
                if(d==player1) conoutf(contype, "\f2%s got fragged by %s", dname, aname);
                else conoutf(contype, "\f2%s fragged %s", aname, dname);
            }
        }
        deathstate(d);
		ai::killed(d, actor);
    }

    void timeupdate(int secs)
    {
        server::timeupdate(secs);
        if(secs > 0)
        {
            maplimit = lastmillis + secs*1000;
        }
        else
        {
            intermission = true;
            player1->attacking = false;
            if(cmode) cmode->gameover();
            conoutf(CON_GAMEINFO, "\f2intermission:");
            conoutf(CON_GAMEINFO, "\f2game has ended!");
            if(m_ctf) conoutf(CON_GAMEINFO, "\f2player frags: %d, flags: %d, deaths: %d", player1->frags, player1->flags, player1->deaths);
            else if(m_collect) conoutf(CON_GAMEINFO, "\f2player frags: %d, skulls: %d, deaths: %d", player1->frags, player1->flags, player1->deaths);
            else conoutf(CON_GAMEINFO, "\f2player frags: %d, deaths: %d", player1->frags, player1->deaths);
            int accuracy = (player1->totaldamage*100)/max(player1->totalshots, 1);
            conoutf(CON_GAMEINFO, "\f2player total damage dealt: %d, damage wasted: %d, accuracy(%%): %d", player1->totaldamage, player1->totalshots-player1->totaldamage, accuracy);
            if(m_sp) spsummary(accuracy);

            showscores(true);
            disablezoom();

            execident("intermission");
            matchrec::onintermission();
        }
    }

    ICOMMAND(getfrags, "", (), intret(player1->frags));
    ICOMMAND(getflags, "", (), intret(player1->flags));
    ICOMMAND(getdeaths, "", (), intret(player1->deaths));
    ICOMMAND(getaccuracy, "", (), intret((player1->totaldamage*100)/max(player1->totalshots, 1)));
    ICOMMAND(gettotaldamage, "", (), intret(player1->totaldamage));
    ICOMMAND(gettotalshots, "", (), intret(player1->totalshots));

    vector<fpsent *> clients;

    fpsent *newclient(int cn)   // ensure valid entity
    {
        if(cn < 0 || cn > max(0xFF, MAXCLIENTS + MAXBOTS))
        {
            neterr("clientnum", false);
            return NULL;
        }

        if(cn == player1->clientnum) return player1;

        while(cn >= clients.length()) clients.add(NULL);
        if(!clients[cn])
        {
            fpsent *d = new fpsent;
            d->clientnum = cn;
            clients[cn] = d;
            players.add(d);
        }
        return clients[cn];
    }

    fpsent *getclient(int cn)   // ensure valid entity
    {
        if(cn == player1->clientnum) return player1;
        return clients.inrange(cn) ? clients[cn] : NULL;
    }

    void clientdisconnected(int cn, bool notify)
    {
        if(!clients.inrange(cn)) return;
        if(following==cn)
        {
            if(followdir) nextfollow(followdir);
            else stopfollowing();
        }
        unignore(cn);
        translategone(cn);
        forgetvs(cn);
        fpsent *d = clients[cn];
        if(!d) return;
        if(d == react_viewer) react_viewer = NULL;
        if(cn >= 0 && cn < FRAGRT_CN) clearfragreactcn(cn);
        if(notify && d->name[0]) conoutf("\f4leave:\f7 %s", colorname(d));
        removeweapons(d);
        removetrackedparticles(d);
        removetrackeddynlights(d);
        if(cmode) cmode->removeplayer(d);
        players.removeobj(d);
        DELETEP(clients[cn]);
        cleardynentcache();
    }

    void clearclients(bool notify)
    {
        loopv(clients) if(clients[i]) clientdisconnected(i, notify);
    }

    void initclient()
    {
        player1 = spawnstate(new fpsent);
        filtertext(player1->name, "unnamed", false, false, MAXNAMELEN);
        players.add(player1);
        loadfriends();
    }

    VARP(showmodeinfo, 0, 1, 1);

    void startgame()
    {
        if(kdlog.length()) rotatekdlog();
        formatstring(kdlogtitle, "%s (%s)", clientmap[0] ? clientmap : "map", server::modename(gamemode));
        clearkillfeed();
        vspairs.setsize(0);
        clearstreakpopups();
        clearfragreact();
        clearmovables();
        clearmonsters();

        clearprojectiles();
        clearbouncers();
        clearragdolls();

        clearteaminfo();

        // reset perma-state
        loopv(players)
        {
            fpsent *d = players[i];
            d->frags = d->flags = 0;
            d->killstreak = 0;
            d->deaths = 0;
            d->totaldamage = 0;
            d->totalshots = 0;
            d->maxhealth = 100;
            d->lifesequence = -1;
            d->respawned = d->suicided = -2;
        }

        setclientmode();

        intermission = false;
        maptime = maprealtime = 0;
        maplimit = -1;

        if(cmode)
        {
            cmode->preload();
            cmode->setup();
        }

        conoutf(CON_GAMEINFO, "\f2game mode is %s", server::modename(gamemode));

        if(m_sp)
        {
            defformatstring(scorename, "bestscore_%s", getclientmap());
            const char *best = getalias(scorename);
            if(*best) conoutf(CON_GAMEINFO, "\f2try to beat your best score so far: %s", best);
        }
        else
        {
            const char *info = m_valid(gamemode) ? gamemodes[gamemode - STARTGAMEMODE].info : NULL;
            if(showmodeinfo && info) conoutf(CON_GAMEINFO, "\f0%s", info);
        }

        if(player1->playermodel != playermodel) switchplayermodel(playermodel);

        showscores(false);
        disablezoom();
        lasthit = 0;

        execident("mapstart");
        matchrec::onstartgame(m_demo || m_edit || demoplayback);
        demohd::onstartgame(m_demo || m_edit || demoplayback);
    }

    void loadingmap(const char *name)
    {
        execident("playsong");
    }

    void startmap(const char *name)   // called just after a map load
    {
        ai::savewaypoints();
        ai::clearwaypoints(true);

        respawnent = -1; // so we don't respawn at an old spot
        if(!m_mp(gamemode)) spawnplayer(player1);
        else findplayerspawn(player1, -1);
        entities::resetspawns();
        copystring(clientmap, name ? name : "");

        sendmapinfo();
    }

    const char *getmapinfo()
    {
        return showmodeinfo && m_valid(gamemode) ? gamemodes[gamemode - STARTGAMEMODE].info : NULL;
    }

    const char *getscreenshotinfo()
    {
        return server::modename(gamemode, NULL);
    }

    void physicstrigger(physent *d, bool local, int floorlevel, int waterlevel, int material)
    {
        if(d->type==ENT_INANIMATE) return;
        if     (waterlevel>0) { if(material!=MAT_LAVA) playsound(S_SPLASH1, d==player1 ? NULL : &d->o); }
        else if(waterlevel<0) playsound(material==MAT_LAVA ? S_BURN : S_SPLASH2, d==player1 ? NULL : &d->o);
        if     (floorlevel>0) { if(d==player1 || d->type!=ENT_PLAYER || ((fpsent *)d)->ai) msgsound(S_JUMP, d); }
        else if(floorlevel<0) { if(d==player1 || d->type!=ENT_PLAYER || ((fpsent *)d)->ai) msgsound(S_LAND, d); }
    }

    void dynentcollide(physent *d, physent *o, const vec &dir)
    {
        switch(d->type)
        {
            case ENT_AI: if(dir.z > 0) stackmonster((monster *)d, o); break;
            case ENT_INANIMATE: if(dir.z > 0) stackmovable((movable *)d, o); break;
        }
    }

    void msgsound(int n, physent *d)
    {
        if(!d || d==player1)
        {
            addmsg(N_SOUND, "ci", d, n);
            playsound(n);
        }
        else
        {
            if(d->type==ENT_PLAYER && ((fpsent *)d)->ai)
                addmsg(N_SOUND, "ci", d, n);
            playsound(n, &d->o);
        }
    }

    int numdynents() { return players.length()+monsters.length()+movables.length(); }

    dynent *iterdynents(int i)
    {
        if(i<players.length()) return players[i];
        i -= players.length();
        if(i<monsters.length()) return (dynent *)monsters[i];
        i -= monsters.length();
        if(i<movables.length()) return (dynent *)movables[i];
        return NULL;
    }

    bool duplicatename(fpsent *d, const char *name = NULL, const char *alt = NULL)
    {
        if(!name) name = d->name;
        if(alt && d != player1 && !strcmp(name, alt)) return true;
        loopv(players) if(d!=players[i] && !strcmp(name, players[i]->name)) return true;
        return false;
    }

    static string cname[3];
    static int cidx = 0;

    const char *colorname(fpsent *d, const char *name, const char *prefix, const char *suffix, const char *alt)
    {
        if(!name) name = alt && d == player1 ? alt : d->name;
        bool dup = !name[0] || duplicatename(d, name, alt) || d->aitype != AI_NONE;
        if(dup || prefix[0] || suffix[0])
        {
            cidx = (cidx+1)%3;
            if(dup) formatstring(cname[cidx], d->aitype == AI_NONE ? "%s%s \fs\f5(%d)\fr%s" : "%s%s \fs\f5[%d]\fr%s", prefix, name, d->clientnum, suffix);
            else formatstring(cname[cidx], "%s%s%s", prefix, name, suffix);
            return cname[cidx];
        }
        return name;
    }

    VARP(teamcolortext, 0, 1, 1);

    const char *teamcolorname(fpsent *d, const char *alt)
    {
        if(!teamcolortext || !m_teammode || d->state==CS_SPECTATOR) return colorname(d, NULL, "", "", alt);
        return colorname(d, NULL, isteam(d->team, player1->team) ? "\fs\f1" : "\fs\f3", "\fr", alt);
    }

    const char *teamcolor(const char *name, bool sameteam, const char *alt)
    {
        if(!teamcolortext || !m_teammode) return sameteam || !alt ? name : alt;
        cidx = (cidx+1)%3;
        formatstring(cname[cidx], sameteam ? "\fs\f1%s\fr" : "\fs\f3%s\fr", sameteam || !alt ? name : alt);
        return cname[cidx];
    }

    const char *teamcolor(const char *name, const char *team, const char *alt)
    {
        return teamcolor(name, team && isteam(team, player1->team), alt);
    }

    VARP(teamsounds, 0, 1, 1);

    void teamsound(bool sameteam, int n, const vec *loc)
    {
        playsound(n, loc, NULL, teamsounds ? (m_teammode && sameteam ? SND_USE_ALT : SND_NO_ALT) : 0);
    }

    void teamsound(fpsent *d, int n, const vec *loc)
    {
        teamsound(isteam(d->team, player1->team), n, loc);
    }

    void suicide(physent *d)
    {
        if(d==player1 || (d->type==ENT_PLAYER && ((fpsent *)d)->ai))
        {
            if(d->state!=CS_ALIVE) return;
            fpsent *pl = (fpsent *)d;
            if(!m_mp(gamemode)) killed(pl, pl);
            else
            {
                int seq = (pl->lifesequence<<16)|((lastmillis/1000)&0xFFFF);
                if(pl->suicided!=seq) { addmsg(N_SUICIDE, "rc", pl); pl->suicided = seq; }
            }
        }
        else if(d->type==ENT_AI) suicidemonster((monster *)d);
        else if(d->type==ENT_INANIMATE) suicidemovable((movable *)d);
    }
    ICOMMAND(suicide, "", (), suicide(player1));

    bool needminimap() { return m_ctf || m_protect || m_hold || m_capture || m_collect; }

    void drawicon(int icon, float x, float y, float sz)
    {
        settexture("packages/hud/items.png");
        float tsz = 0.25f, tx = tsz*(icon%4), ty = tsz*(icon/4);
        gle::defvertex(2);
        gle::deftexcoord0();
        gle::begin(GL_TRIANGLE_STRIP);
        gle::attribf(x,    y);    gle::attribf(tx,     ty);
        gle::attribf(x+sz, y);    gle::attribf(tx+tsz, ty);
        gle::attribf(x,    y+sz); gle::attribf(tx,     ty+tsz);
        gle::attribf(x+sz, y+sz); gle::attribf(tx+tsz, ty+tsz);
        gle::end();
    }

    float abovegameplayhud(int w, int h)
    {
        switch(hudplayer()->state)
        {
            case CS_EDITING:
            case CS_SPECTATOR:
                return demoplayback ? 0.88f : 1;
            default:
                return 1650.0f/1800.0f;
        }
    }

    int ammohudup[3] = { GUN_CG, GUN_RL, GUN_GL },
        ammohuddown[3] = { GUN_RIFLE, GUN_SG, GUN_PISTOL },
        ammohudcycle[7] = { -1, -1, -1, -1, -1, -1, -1 };

    ICOMMAND(ammohudup, "V", (tagval *args, int numargs),
    {
        loopi(3) ammohudup[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    ICOMMAND(ammohuddown, "V", (tagval *args, int numargs),
    {
        loopi(3) ammohuddown[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    ICOMMAND(ammohudcycle, "V", (tagval *args, int numargs),
    {
        loopi(7) ammohudcycle[i] = i < numargs ? getweapon(args[i].getstr()) : -1;
    });

    VARP(ammohud, 0, 1, 1);

    void drawammohud(fpsent *d)
    {
        float x = HICON_X + 2*HICON_STEP, y = HICON_Y, sz = HICON_SIZE;
        pushhudmatrix();
        hudmatrix.scale(1/3.2f, 1/3.2f, 1);
        flushhudmatrix();
        float xup = (x+sz)*3.2f, yup = y*3.2f + 0.1f*sz;
        loopi(3)
        {
            int gun = ammohudup[i];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            drawicon(HICON_FIST+gun, xup, yup, sz);
            yup += sz;
        }
        float xdown = x*3.2f - sz, ydown = (y+sz)*3.2f - 0.1f*sz;
        loopi(3)
        {
            int gun = ammohuddown[3-i-1];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            ydown -= sz;
            drawicon(HICON_FIST+gun, xdown, ydown, sz);
        }
        int offset = 0, num = 0;
        loopi(7)
        {
            int gun = ammohudcycle[i];
            if(gun < GUN_FIST || gun > GUN_PISTOL) continue;
            if(gun == d->gunselect) offset = i + 1;
            else if(d->ammo[gun]) num++;
        }
        float xcycle = (x+sz/2)*3.2f + 0.5f*num*sz, ycycle = y*3.2f-sz;
        loopi(7)
        {
            int gun = ammohudcycle[(i + offset)%7];
            if(gun < GUN_FIST || gun > GUN_PISTOL || gun == d->gunselect || !d->ammo[gun]) continue;
            xcycle -= sz;
            drawicon(HICON_FIST+gun, xcycle, ycycle, sz);
        }
        pophudmatrix();
    }

    VARP(healthcolors, 0, 1, 1);

    void drawhudicons(fpsent *d)
    {
        hudscope part("icons", "Health, armour, ammo");
        hudrect(HICON_X, HICON_Y, 3*HICON_STEP, HICON_SIZE);
        pushhudmatrix();
        hudmatrix.scale(2, 2, 1);
        flushhudmatrix();

        defformatstring(health, "%d", d->state==CS_DEAD ? 0 : d->health);
        bvec healthcolor = bvec::hexcolor(healthcolors && !m_insta ? (d->state==CS_DEAD ? 0x808080 : (d->health<=25 ? 0xFF0000 : (d->health<=50 ? 0xFF8000 : (d->health<=100 ? 0xFFFFFF : 0x40C0FF)))) : 0xFFFFFF);
        draw_text(health, (HICON_X + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, healthcolor.r, healthcolor.g, healthcolor.b);
        if(d->state!=CS_DEAD)
        {
            if(d->armour) draw_textf("%d", (HICON_X + HICON_STEP + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->armour);
            draw_textf("%d", (HICON_X + 2*HICON_STEP + HICON_SIZE + HICON_SPACE)/2, HICON_TEXTY/2, d->ammo[d->gunselect]);
        }

        pophudmatrix();

        if(d->state != CS_DEAD && d->maxhealth > 100)
        {
            float scale = 0.66f;
            pushhudmatrix();
            hudmatrix.scale(scale, scale, 1);
            flushhudmatrix();

            float width, height;
            text_boundsf(health, width, height);
            draw_textf("/%d", (HICON_X + HICON_SIZE + HICON_SPACE + width*2)/scale, (HICON_TEXTY + height)/scale, d->maxhealth);

            pophudmatrix();
        }

        drawicon(HICON_HEALTH, HICON_X, HICON_Y);
        if(d->state!=CS_DEAD)
        {
            if(d->armour) drawicon(HICON_BLUE_ARMOUR+d->armourtype, HICON_X + HICON_STEP, HICON_Y);
            drawicon(HICON_FIST+d->gunselect, HICON_X + 2*HICON_STEP, HICON_Y);
            if(d->quadmillis) drawicon(HICON_QUAD, HICON_X + 3*HICON_STEP, HICON_Y);
            if(ammohud) drawammohud(d);
        }
    }

    VARP(gameclock, 0, 0, 1);
    FVARP(gameclockscale, 1e-3f, 0.75f, 1e3f);
    HVARP(gameclockcolour, 0, 0xFFFFFF, 0xFFFFFF);
    VARP(gameclockalpha, 0, 255, 255);
    HVARP(gameclocklowcolour, 0, 0xFFC040, 0xFFFFFF);
    VARP(gameclockalign, -1, 0, 1);
    FVARP(gameclockx, 0, 0.50f, 1);
    FVARP(gameclocky, 0, 0.03f, 1);

    void drawgameclock(int w, int h)
    {
        hudscope part("gameclock", "Match clock");
        int secs = max(maplimit-lastmillis + 999, 0)/1000, mins = secs/60;
        secs %= 60;

        defformatstring(buf, "%d:%02d", mins, secs);
        int tw = 0, th = 0;
        text_bounds(buf, tw, th);

        vec2 offset = vec2(gameclockx, gameclocky).mul(vec2(w, h).div(gameclockscale));
        if(gameclockalign == 1) offset.x -= tw;
        else if(gameclockalign == 0) offset.x -= tw/2.0f;
        offset.y -= th/2.0f;

        pushhudmatrix();
        hudmatrix.scale(gameclockscale, gameclockscale, 1);
        flushhudmatrix();

        int color = mins < 1 ? gameclocklowcolour : gameclockcolour;
        draw_text(buf, int(offset.x), int(offset.y), (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, gameclockalpha);
        hudrect(offset.x, offset.y, tw, th);

        pophudmatrix();
    }

    extern int hudscore;
    extern void drawhudscore(int w, int h);

    VARP(ammobar, 0, 0, 1);
    VARP(ammobaralign, -1, 0, 1);
    VARP(ammobarhorizontal, 0, 0, 1);
    VARP(ammobarflip, 0, 0, 1);
    VARP(ammobarhideempty, 0, 1, 1);
    VARP(ammobarsep, 0, 20, 500);
    VARP(ammobarcountsep, 0, 20, 500);
    FVARP(ammobarcountscale, 0.5, 1.5, 2);
    FVARP(ammobarx, 0, 0.025f, 1.0f);
    FVARP(ammobary, 0, 0.5f, 1.0f);
    FVARP(ammobarscale, 0.1f, 0.5f, 1.0f);

    void drawammobarcounter(const vec2 &center, const fpsent *p, int gun)
    {
        vec2 icondrawpos = vec2(center).sub(HICON_SIZE / 2);
        int alpha = p->ammo[gun] ? 0xFF : 0x7F;
        gle::color(bvec(0xFF, 0xFF, 0xFF), alpha);
        drawicon(HICON_FIST + gun, icondrawpos.x, icondrawpos.y);
        hudrect(icondrawpos.x, icondrawpos.y, HICON_SIZE, HICON_SIZE);

        int fw, fh; text_bounds("000", fw, fh);
        float labeloffset = HICON_SIZE / 2.0f + ammobarcountsep + ammobarcountscale * (ammobarhorizontal ? fh : fw) / 2.0f;
        vec2 offsetdir = (ammobarhorizontal ? vec2(0, 1) : vec2(1, 0)).mul(ammobarflip ? -1 : 1);
        vec2 labelorigin = vec2(offsetdir).mul(labeloffset).add(center);

        pushhudmatrix();
        hudmatrix.translate(labelorigin.x, labelorigin.y, 0);
        hudmatrix.scale(ammobarcountscale, ammobarcountscale, 1);
        flushhudmatrix();

        defformatstring(label, "%d", p->ammo[gun]);
        int tw, th; text_bounds(label, tw, th);
        vec2 textdrawpos = vec2(-tw, -th).div(2);
        float ammoratio = (float)p->ammo[gun] / itemstats[gun-GUN_SG].add;
        bvec color = bvec::hexcolor(p->ammo[gun] == 0 || ammoratio >= 1.0f ? 0xFFFFFF : (ammoratio >= 0.5f ? 0xFFC040 : 0xFF0000));
        draw_text(label, textdrawpos.x, textdrawpos.y, color.r, color.g, color.b, alpha);
        hudrect(textdrawpos.x, textdrawpos.y, tw, th);

        pophudmatrix();
    }

    static inline bool ammobargunvisible(const fpsent *d, int gun)
    {
        return d->ammo[gun] > 0 || d->gunselect == gun;
    }

    void drawammobar(int w, int h, fpsent *p)
    {
        if(m_insta) return;
        hudscope part("ammobar", "Ammo bar");

        int NUMPLAYERGUNS = GUN_PISTOL - GUN_SG + 1;
        int numvisibleguns = NUMPLAYERGUNS;
        if(ammobarhideempty) loopi(NUMPLAYERGUNS) if(!ammobargunvisible(p, GUN_SG + i)) numvisibleguns--;

        vec2 origin = vec2(ammobarx, ammobary).mul(vec2(w, h).div(ammobarscale));
        vec2 offsetdir = ammobarhorizontal ? vec2(1, 0) : vec2(0, 1);
        float stepsize = HICON_SIZE + ammobarsep;
        float initialoffset = (ammobaralign - 1) * (numvisibleguns - 1) * stepsize / 2;

        pushhudmatrix();
        hudmatrix.scale(ammobarscale, ammobarscale, 1);
        flushhudmatrix();

        int numskippedguns = 0;
        loopi(NUMPLAYERGUNS) if(ammobargunvisible(p, GUN_SG + i) || !ammobarhideempty)
        {
            float offset = initialoffset + (i - numskippedguns) * stepsize;
            vec2 drawpos = vec2(offsetdir).mul(offset).add(origin);
            drawammobarcounter(drawpos, p, GUN_SG + i);
        }
        else numskippedguns++;

        pophudmatrix();
    }

    static const char *kdlogtext(const kdlogline &e)
    {
        if(!e.showreact || e.pending) return e.text;
        static string lined;
        string rtxt;
        formatreact(rtxt, sizeof(rtxt), e.fovms, e.xhms, e.approx);
        formatstring(lined, "%s %s", e.text, rtxt);
        return lined;
    }

    static int killloglen()
    {
        return (kdprev.length() ? 1 + kdprev.length() : 0) + 1 + max(kdlog.length(), 1);
    }

    static const char *killlogline(int i)
    {
        int nprev = kdprev.length() ? 1 + kdprev.length() : 0;
        if(i < nprev)
        {
            if(!i)
            {
                static string hdr;
                if(kdprevtitle[0]) formatstring(hdr, "\f4--- previous: %s ---", kdprevtitle);
                else copystring(hdr, "\f4--- previous ---");
                return hdr;
            }
            return kdlogtext(kdprev[i-1]);
        }
        i -= nprev;
        if(!i)
        {
            static string hdr;
            if(kdlogtitle[0]) formatstring(hdr, "\f0--- this game: %s ---", kdlogtitle);
            else copystring(hdr, "\f0--- this game ---");
            return hdr;
        }
        if(kdlog.empty()) return "\f4no kills or deaths yet";
        return kdlogtext(kdlog[i-1]);
    }

    void skipkilllog(int n)
    {
        int numl = killloglen();
        killlogskip = numl ? clamp(killlogskip + n, 0, numl-1) : 0;
    }

    bool killlogkey(int code, bool isdown)
    {
        if(!fullkilllog || !isdown) return false;
        switch(code)
        {
            case -4:
                skipkilllog(3);
                return true;
            case -5:
                skipkilllog(-3);
                return true;
            case SDLK_PAGEUP:
                skipkilllog(8);
                return true;
            case SDLK_PAGEDOWN:
                skipkilllog(-8);
                return true;
            case SDLK_HOME:
                if(commandmillis >= 0) return false;
                killlogskip = killloglen() ? killloglen()-1 : 0;
                return true;
            case SDLK_END:
                if(commandmillis >= 0) return false;
                killlogskip = 0;
                return true;
            default:
                return false;
        }
    }

    void drawkilllog(int conwidth, int conheight, int conoff)
    {
        int numl = killloglen();
        if(!numl) return;
        int skip = clamp(killlogskip, 0, numl-1);
        int last = numl - 1 - skip;
        int totalheight = 0, first = last;
        for(int i = last; i >= 0; i--)
        {
            int width, height;
            text_bounds(killlogline(i), width, height, conwidth);
            if(totalheight + height > conheight) break;
            totalheight += height;
            first = i;
        }
        int y = conoff;
        for(int i = first; i <= last; i++)
        {
            const char *line = killlogline(i);
            int width, height;
            text_bounds(line, width, height, conwidth);
            draw_text(line, conoff, y, 0xFF, 0xFF, 0xFF, 0xFF, -1, conwidth);
            y += height;
        }
    }

    void togglekilllog()
    {
        fullkilllog ^= 1;
        if(fullkilllog)
        {
            execute("fullconsole 0");
            execute("fullchat 0");
            killlogskip = 0;
        }
    }
    COMMAND(togglekilllog, "");

    void drawkillfeed(int w, int h)
    {
        int lifetime = killfeedfade * 1000;
        while(killfeedlines.length() && lastmillis - killfeedlines[0].millis >= lifetime)
            killfeedlines.remove(0);
        if(killfeedlines.empty() && !hudeditactive()) return;
        hudscope part("killfeed", "Kill feed");

        pushhudmatrix();
        hudmatrix.scale(killfeedscale, killfeedscale, 1);
        flushhudmatrix();

        int dummy, fonth;
        text_bounds("Ag", dummy, fonth);
        int iconsz = fonth * 5 / 4;
        int pad = max(fonth / 8, 4);
        int lineh = max(fonth, iconsz) + pad;
        int gap = pad * 2;

        float x = killfeedx * w / killfeedscale, y = killfeedy * h / killfeedscale;
        int shown = 0;
        for(int i = killfeedlines.length() - 1; i >= 0 && shown < killfeedmax; --i)
        {
            killfeedline &e = killfeedlines[i];
            int age = lastmillis - e.millis;
            if(age < 0 || age >= lifetime) continue;
            // "mine" / "my team" = the player whose view you have (spectated
            // player, demo recorder), yourself otherwise
            if(killfeedfilter)
            {
                fpsent *v = vsviewer();
                bool vmine = e.victimcn == v->clientnum || e.actorcn == v->clientnum;
                bool vteam = e.victimcn == v->clientnum || (!e.suicide && (e.actorcn == v->clientnum || isteam(e.actorteam, v->team)));
                if(killfeedfilter == 2 && !vmine) continue;
                if(killfeedfilter == 1 && !vteam) continue;
            }

            int alpha = 255;
            int fadetime = max(lifetime / 4, 1);
            if(age > lifetime - fadetime) alpha = 255 * (lifetime - age) / fadetime;

            int kw = 0, kh = 0, vw = 0, vh = 0;
            if(!e.suicide) text_bounds(e.killer, kw, kh);
            text_bounds(e.victim, vw, vh);
            bool hasgun = e.gun >= GUN_FIST && e.gun <= GUN_PISTOL;
            int iconw = hasgun ? iconsz : 0;
            string rtxt;
            rtxt[0] = 0;
            int rw = 0, rh = 0;
            if(e.showreact && !e.pending)
            {
                formatreact(rtxt, sizeof(rtxt), e.fovms, e.xhms, e.approx);
            }
            // the score of that moment, seen by the player you watch (a later frag does not change old lines)
            string vs = "";
            if(killfeedvs && !e.suicide)
            {
                fpsent *v = vsviewer();
                int k = -1, d = -1;
                if(v && e.actorcn == v->clientnum) { k = e.vsak; d = e.vsvk; }
                else if(v && e.victimcn == v->clientnum) { k = e.vsvk; d = e.vsak; }
                if(k >= 0 && (k || d)) formatstring(vs, "\f0%d\f7-\f3%d", k, d);
            }
            if(vs[0]) concformatstring(rtxt, "%s\f7[%s\f7]", rtxt[0] ? "  " : "", vs);
            if(rtxt[0]) text_bounds(rtxt, rw, rh);
            int totalw = (e.suicide ? vw : kw + gap + vw) + (iconw ? gap + iconw : 0) + (rw ? gap + rw : 0);

            float lx = x;
            if(killfeedalign > 0) lx -= totalw;
            else if(!killfeedalign) lx -= totalw / 2.0f;
            hudrect(lx, y, totalw, lineh);

            int texty = int(y) + (lineh - fonth) / 2;
            int icony = int(y) + (lineh - iconsz) / 2;

            if(e.suicide)
            {
                draw_text(e.victim, int(lx), texty, 255, 255, 255, alpha);
                lx += vw + gap;
                if(hasgun)
                {
                    gle::color(bvec(0xFF, 0xFF, 0xFF), alpha);
                    drawicon(HICON_FIST + e.gun, lx, icony, iconsz);
                    gle::color(bvec(0xFF, 0xFF, 0xFF), 255);
                }
            }
            else
            {
                draw_text(e.killer, int(lx), texty, 255, 255, 255, alpha);
                lx += kw + gap;
                if(hasgun)
                {
                    gle::color(bvec(0xFF, 0xFF, 0xFF), alpha);
                    drawicon(HICON_FIST + e.gun, lx, icony, iconsz);
                    gle::color(bvec(0xFF, 0xFF, 0xFF), 255);
                    lx += iconsz + gap;
                }
                draw_text(e.victim, int(lx), texty, 255, 255, 255, alpha);
                if(rtxt[0])
                {
                    lx += vw + gap;
                    draw_text(rtxt, int(lx), texty, 255, 255, 255, alpha);
                }
            }
            y += lineh;
            shown++;
        }
        if(!shown && hudeditactive())
        {
            // nothing to show: where the first line would go, so it can be moved
            float sw = 14*fonth, sx = x - (killfeedalign > 0 ? sw : (!killfeedalign ? sw/2 : 0));
            hudrect(sx, y, sw, lineh);
        }

        pophudmatrix();
    }

    void drawkillstreak(int w, int h)
    {
        int lifetime = killstreakfade * 1000;
        while(streakpopups.length() && lastmillis - streakpopups[0].millis >= lifetime)
            streakpopups.remove(0);
        if(!killstreak || (streakpopups.empty() && !hudeditactive())) return;
        hudscope part("killstreak", "Killing spree");

        float y = killstreaky * h;
        if(streakpopups.empty())
        {
            // editing with nothing to show: a sample line where it would be
            float scale = max(killstreakscale, 0.15f);
            int tw = 0, th = 0;
            text_bounds("PLAYER  5 STREAK", tw, th);
            pushhudmatrix();
            hudmatrix.scale(scale, scale, 1);
            flushhudmatrix();
            float x = killstreakx * w / scale;
            if(killstreakalign > 0) x -= tw;
            else if(!killstreakalign) x -= tw / 2.0f;
            hudrect(x, y / scale - th / 2.0f, tw, th);
            pophudmatrix();
        }
        loopv(streakpopups)
        {
            streakpopup &p = streakpopups[i];
            int age = lastmillis - p.millis;
            if(age < 0 || age >= lifetime) continue;

            int alpha = 255;
            int fadetime = max(lifetime / 4, 1);
            if(age > lifetime - fadetime) alpha = 255 * (lifetime - age) / fadetime;

            float scale = killstreakscale * (p.mine ? 1.0f : 0.7f);
            if(scale < 0.15f) scale = 0.15f;

            int tw = 0, th = 0;
            text_bounds(p.text, tw, th);

            pushhudmatrix();
            hudmatrix.scale(scale, scale, 1);
            flushhudmatrix();

            float x = killstreakx * w / scale;
            if(killstreakalign > 0) x -= tw;
            else if(!killstreakalign) x -= tw / 2.0f;
            float ly = y / scale - th / 2.0f;

            int r = p.mine ? 255 : 220, g = p.mine ? 220 : 220, b = p.mine ? 32 : 220;
            draw_text(p.text, int(x), int(ly), r, g, b, alpha);
            hudrect(x, ly, tw, th);

            pophudmatrix();
            y += th * scale + 8;
        }
    }

    // big CTF messages, apart from the kill feed: a flag stolen or picked
    // up, a flag scored (with the run time when it came from the base)
    VARP(flagfeed, 0, 1, 1);
    VARP(flagfeedfade, 1, 4, 20);
    struct flagfeedline { string text; int icon, millis; };
    static vector<flagfeedline> flagfeedlines;

    void addflagfeed(const char *text, int icon)
    {
        if(!flagfeed) return;
        while(flagfeedlines.length() >= 3) flagfeedlines.remove(0);
        flagfeedline &l = flagfeedlines.add();
        copystring(l.text, text);
        l.icon = icon;
        l.millis = lastmillis;
    }

    void drawflagfeed(int w, int h)
    {
        int lifetime = flagfeedfade*1000;
        while(flagfeedlines.length() && (lastmillis - flagfeedlines[0].millis >= lifetime || lastmillis < flagfeedlines[0].millis))
            flagfeedlines.remove(0);
        if(!flagfeed || (flagfeedlines.empty() && !hudeditactive())) return;
        hudscope part("flagfeed", "Flag messages (CTF)");

        float sc = h/1800.0f*1.3f;
        pushhudmatrix();
        hudmatrix.scale(sc, sc, 1);
        flushhudmatrix();

        int dummy, fonth;
        text_bounds("Ag", dummy, fonth);
        float iconsz = fonth*1.3f, gap = fonth*0.4f, lineh = iconsz + fonth*0.3f;
        float cx = w*0.5f/sc, y = h*0.2f/sc;
        if(flagfeedlines.empty())
        {
            // editing with nothing to show: where a line would go
            int tw, th;
            text_bounds("PLAYER stole the enemy flag", tw, th);
            float tot = iconsz + gap + tw;
            hudrect(cx - tot/2, y, tot, lineh);
        }
        loopv(flagfeedlines)
        {
            flagfeedline &l = flagfeedlines[i];
            int age = lastmillis - l.millis, alpha = 255, fadetime = max(lifetime/4, 1);
            if(age > lifetime - fadetime) alpha = 255*(lifetime - age)/fadetime;
            // a short pop when it arrives
            float pop = age < 150 ? 1.25f - 0.25f*age/150.0f : 1;
            int tw, th;
            text_bounds(l.text, tw, th);
            float tot = (l.icon >= 0 ? iconsz + gap : 0) + tw;
            pushhudmatrix();
            hudmatrix.translate(cx, y + lineh/2, 0);
            hudmatrix.scale(pop, pop, 1);
            flushhudmatrix();
            float x = -tot/2, ty = -th/2.0f;
            if(l.icon >= 0)
            {
                gle::color(bvec(0xFF, 0xFF, 0xFF), alpha);
                drawicon(l.icon, x, -iconsz/2, iconsz);
                gle::color(bvec(0xFF, 0xFF, 0xFF), 255);
                x += iconsz + gap;
            }
            draw_text(l.text, int(x), int(ty), 255, 255, 255, alpha);
            hudrect(-tot/2, -lineh/2, tot, lineh);
            pophudmatrix();
            y += lineh;
        }
        pophudmatrix();
    }

    void gameplayhud(int w, int h)
    {
        pushhudmatrix();
        hudmatrix.scale(h/1800.0f, h/1800.0f, 1);
        flushhudmatrix();

        if(player1->state==CS_SPECTATOR)
        {
            int pw, ph, tw, th, fw, fh;
            text_bounds("  ", pw, ph);
            text_bounds("SPECTATOR", tw, th);
            th = max(th, ph);
            fpsent *f = followingplayer();
            text_bounds(f ? colorname(f) : " ", fw, fh);
            fh = max(fh, ph);
            int specy = demoplayback ? 1480 : 1650;
            hudscope part("spectator", "Spectator / watched player");
            draw_text("SPECTATOR", w*1800/h - tw - pw, specy - th - fh);
            hudrect(w*1800/h - tw - pw, specy - th - fh, tw, th);
            if(f)
            {
                int color = demoplayback ? demohd::namecolor(f, statuscolor(f, 0xFFFFFF)) : statuscolor(f, 0xFFFFFF);
                color = friendnamecolor(f, color);
                draw_text(colorname(f), w*1800/h - fw - pw, specy - fh, (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF);
                hudrect(w*1800/h - fw - pw, specy - fh, fw, fh);
            }
            // small grey "HD" left of the watched player's name while his exact aim is shown
            const char *st = demohd::hudstatus();
            if(f && st && st[0])
            {
                int sw, sh;
                text_bounds(st, sw, sh);
                float sc = 0.6f;
                float sx = w*1800/h - fw - pw - (sw + pw/2)*sc, sy = specy - fh + fh*(1 - sc)/2;
                pushhudmatrix();
                hudmatrix.translate(sx, sy, 0);
                hudmatrix.scale(sc, sc, 1);
                flushhudmatrix();
                draw_text(st, 0, 0, 0xA0, 0xA0, 0xA0);
                pophudmatrix();
            }
        }

        fpsent *d = hudplayer();
        if(d->state!=CS_EDITING)
        {
            if(d->state!=CS_SPECTATOR) drawhudicons(d);
            if(cmode) cmode->drawhud(d, w, h);
        }
        // HUD editor: frames at the default place of the parts that only
        // show in some modes, so they can be placed from anywhere
        if(hudeditactive())
        {
            if(!cmode)
            {
                int s = 1800/4, x = 1800*w/h - s - s/10, y = s/10;
                hudscope part("radar", "Radar / minimap");
                hudrect(x - 0.04f*s, y - 0.04f*s, 1.08f*s, 1.08f*s);
            }
            if(!m_ctf)
            {
                extern int flagtimer;
                if(flagtimer)
                {
                    hudscope part("flagtimer", "Flag timer");
                    pushhudmatrix();
                    hudmatrix.scale(2, 2, 1);
                    flushhudmatrix();
                    int tw, th;
                    text_bounds("0:12.3", tw, th);
                    hudrect((1800*w/h - 40)/2 - tw, 1700/2 - th, tw, th);
                    pophudmatrix();
                }
            }
            if(player1->state!=CS_SPECTATOR)
            {
                int tw, th;
                text_bounds("SPECTATOR", tw, th);
                hudscope part("spectator", "Spectator / watched player");
                hudrect(w*1800/h - 2*tw, 1650 - 3*th, 2*tw - th, 3*th);
            }
        }

        pophudmatrix();

        if(demoplayback) demohd::drawhud(w, h);

        if(d->state!=CS_EDITING)
        {
            if(killfeed) drawkillfeed(w, h);
            drawkillstreak(w, h);
            drawflagfeed(w, h);
        }

        if(d->state!=CS_EDITING && d->state!=CS_SPECTATOR && d->state!=CS_DEAD)
        {
            if(ammobar) drawammobar(w, h, d);
        }

        if(!m_edit && !m_sp)
        {
            if(gameclock) drawgameclock(w, h);
            if(hudscore) drawhudscore(w, h);
        }
    }

    int clipconsole(int w, int h)
    {
        if(cmode) return cmode->clipconsole(w, h);
        return 0;
    }

    VARP(teamcrosshair, 0, 1, 1);
    VARP(hitcrosshair, 0, 425, 1000);
    VARP(crosshairreloaddim, 0, 0, 1); // 1 = crosshair at half brightness between shots (vanilla)

    const char *defaultcrosshair(int index)
    {
        switch(index)
        {
            case 2: return "data/hit.png";
            case 1: return "data/teammate.png";
            default: return "data/crosshair.png";
        }
    }

    int selectcrosshair(vec &color)
    {
        fpsent *d = hudplayer();
        if(d->state==CS_SPECTATOR || d->state==CS_DEAD) return -1;

        if(d->state!=CS_ALIVE) return 0;

        int crosshair = 0;
        if(lasthit && lastmillis - lasthit < hitcrosshair) crosshair = 2;
        else if(teamcrosshair)
        {
            dynent *o = intersectclosest(d->o, worldpos, d);
            if(o && o->type==ENT_PLAYER && isteam(((fpsent *)o)->team, d->team))
            {
                crosshair = 1;
                color = vec(0, 0, 1);
            }
        }

        bool tinted = false;
        if(crosshair==2 && lasthitcn >= 0)
        {
            // hit crosshair in the colour of the friend you hit
            fpsent *t = getclient(lasthitcn);
            if(t && isfriend(t) && friendhascolor(isfriendally(t)))
            {
                color = friendcolorvec(isfriendally(t));
                tinted = true;
            }
            else if(t)
            {
                // anyone else: blue if on the watched player's team, red otherwise (FFA too)
                int vt = demohd::visteam(t);
                bool ally = vt ? vt==1 : (m_teammode && isteam(t->team, d->team));
                color = ally ? vec(0.25f, 0.5f, 1) : vec(1, 0.2f, 0.1f);
                tinted = true;
            }
        }

        if(crosshair!=1 && !tinted && !editmode && !m_insta)
        {
            if(d->health<=25) color = vec(1, 0, 0);
            else if(d->health<=50) color = vec(1, 0.5f, 0);
        }
        if(d->gunwait && crosshairreloaddim) color.mul(0.5f);
        return crosshair;
    }

    void lighteffects(dynent *e, vec &color, vec &dir)
    {
        (void)e; (void)color; (void)dir;
    }

    int maxsoundradius(int n)
    {
        switch(n)
        {
            case S_JUMP:
            case S_LAND:
            case S_WEAPLOAD:
            case S_ITEMAMMO:
            case S_ITEMHEALTH:
            case S_ITEMARMOUR:
            case S_ITEMPUP:
            case S_ITEMSPAWN:
            case S_NOAMMO:
            case S_PUPOUT:
                return 340;
            default:
                return 500;
        }
    }

    VARP(mixweapons, 0, 100, 200);
    VARP(mixhits, 0, 100, 200);
    VARP(mixpain, 0, 100, 200);
    VARP(mixitems, 0, 100, 200);
    VARP(mixannounce, 0, 100, 200);
    VARP(mixflags, 0, 100, 200);
    VARP(mixmove, 0, 100, 200);
    VARP(mixworld, 0, 100, 200);

    int soundmixvol(int n, bool mapsound)
    {
        if(mapsound) return mixworld;
        switch(n)
        {
            case S_RIFLE: case S_PUNCH1: case S_SG: case S_CG:
            case S_RLFIRE: case S_RLHIT: case S_FLAUNCH: case S_FEXPLODE:
            case S_PISTOL: case S_CHAINSAW_ATTACK: case S_CHAINSAW_IDLE:
            case S_ICEBALL: case S_SLIMEBALL:
                return mixweapons;
            case S_HIT:
                return mixhits;
            case S_PAIN1: case S_PAIN2: case S_PAIN3: case S_PAIN4: case S_PAIN5: case S_PAIN6:
            case S_DIE1: case S_DIE2:
            case S_GRUNT1: case S_GRUNT2:
            case S_PAINO: case S_PAINR: case S_DEATHR:
            case S_PAINE: case S_DEATHE: case S_PAINS: case S_DEATHS:
            case S_PAINB: case S_DEATHB: case S_PAINP: case S_PIGGR2:
            case S_PAINH: case S_DEATHH: case S_PAIND: case S_DEATHD:
            case S_PIGR1: case S_BURN:
                return mixpain;
            case S_WEAPLOAD: case S_ITEMAMMO: case S_ITEMHEALTH:
            case S_ITEMARMOUR: case S_ITEMPUP: case S_ITEMSPAWN:
            case S_NOAMMO: case S_PUPOUT:
                return mixitems;
            case S_V_BASECAP: case S_V_BASELOST: case S_V_FIGHT:
            case S_V_BOOST: case S_V_BOOST10: case S_V_QUAD: case S_V_QUAD10:
            case S_V_RESPAWNPOINT:
                return mixannounce;
            case S_FLAGPICKUP: case S_FLAGDROP: case S_FLAGRETURN:
            case S_FLAGSCORE: case S_FLAGRESET: case S_FLAGFAIL:
                return mixflags;
            case S_JUMP: case S_LAND: case S_SPLASH1: case S_SPLASH2:
            case S_JUMPPAD: case S_TELEPORT: case S_RUMBLE:
                return mixmove;
            default:
                return 100;
        }
    }

    ICOMMAND(mixreset, "", (),
    {
        mixweapons = mixhits = mixpain = mixitems = 100;
        mixannounce = mixflags = mixmove = mixworld = 100;
    });

    bool serverinfostartcolumn(g3d_gui *g, int i)
    {
        static const char * const names[] = { "ping ", "players ", "mode ", "map ", "time ", "master ", "host ", "port ", "description " };
        static const float struts[] =       { 7,       7,          12.5f,   14,      7,      8,         14,      7,       24.5f };
        if(size_t(i) >= sizeof(names)/sizeof(names[0])) return false;
        g->pushlist();
        g->text(names[i], 0xFFFF80, !i ? " " : NULL);
        if(struts[i]) g->strut(struts[i]);
        g->mergehits(true);
        return true;
    }

    void serverinfoendcolumn(g3d_gui *g, int i)
    {
        g->mergehits(false);
        g->column(i);
        g->poplist();
    }

    const char *mastermodecolor(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodecolors)/sizeof(mastermodecolors[0])) ? mastermodecolors[n-MM_START] : unknown;
    }

    const char *mastermodeicon(int n, const char *unknown)
    {
        return (n>=MM_START && size_t(n-MM_START)<sizeof(mastermodeicons)/sizeof(mastermodeicons[0])) ? mastermodeicons[n-MM_START] : unknown;
    }

    bool serverinfoentry(g3d_gui *g, int i, const char *name, int port, const char *sdesc, const char *map, int ping, const vector<int> &attr, int np)
    {
        if(ping < 0 || attr.empty() || attr[0]!=PROTOCOL_VERSION)
        {
            switch(i)
            {
                case 0:
                    if(g->button(" ", 0xFFFFDD, "serverunk")&G3D_UP) return true;
                    break;

                case 1:
                case 2:
                case 3:
                case 4:
                case 5:
                    if(g->button(" ", 0xFFFFDD)&G3D_UP) return true;
                    break;

                case 6:
                    if(g->buttonf("%s ", 0xFFFFDD, NULL, name)&G3D_UP) return true;
                    break;

                case 7:
                    if(g->buttonf("%d ", 0xFFFFDD, NULL, port)&G3D_UP) return true;
                    break;

                case 8:
                    if(ping < 0)
                    {
                        if(g->button(sdesc, 0xFFFFDD)&G3D_UP) return true;
                    }
                    else if(g->buttonf("[%s protocol] ", 0xFFFFDD, NULL, attr.empty() ? "unknown" : (attr[0] < PROTOCOL_VERSION ? "older" : "newer"))&G3D_UP) return true;
                    break;
            }
            return false;
        }

        switch(i)
        {
            case 0:
            {
                const char *icon = attr.inrange(3) && np >= attr[3] ? "serverfull" : (attr.inrange(4) ? mastermodeicon(attr[4], "serverunk") : "serverunk");
                if(g->buttonf("%d ", 0xFFFFDD, icon, ping)&G3D_UP) return true;
                break;
            }

            case 1:
                if(attr.length()>=4)
                {
                    if(g->buttonf(np >= attr[3] ? "\f3%d/%d " : "%d/%d ", 0xFFFFDD, NULL, np, attr[3])&G3D_UP) return true;
                }
                else if(g->buttonf("%d ", 0xFFFFDD, NULL, np)&G3D_UP) return true;
                break;

            case 2:
                if(g->buttonf("%s ", 0xFFFFDD, NULL, attr.length()>=2 ? server::modename(attr[1], "") : "")&G3D_UP) return true;
                break;

            case 3:
                if(g->buttonf("%.25s ", 0xFFFFDD, NULL, map)&G3D_UP) return true;
                break;

            case 4:
                if(attr.length()>=3 && attr[2] > 0)
                {
                    int secs = clamp(attr[2], 0, 59*60+59),
                        mins = secs/60;
                    secs %= 60;
                    if(g->buttonf("%d:%02d ", 0xFFFFDD, NULL, mins, secs)&G3D_UP) return true;
                }
                else if(g->buttonf(" ", 0xFFFFDD)&G3D_UP) return true;
                break;
            case 5:
                if(g->buttonf("%s%s ", 0xFFFFDD, NULL, attr.length()>=5 ? mastermodecolor(attr[4], "") : "", attr.length()>=5 ? server::mastermodename(attr[4], "") : "")&G3D_UP) return true;
                break;

            case 6:
                if(g->buttonf("%s ", 0xFFFFDD, NULL, name)&G3D_UP) return true;
                break;

            case 7:
                if(g->buttonf("%d ", 0xFFFFDD, NULL, port)&G3D_UP) return true;
                break;

            case 8:
                if(g->buttonf("%.25s", 0xFFFFDD, NULL, sdesc)&G3D_UP) return true;
                break;
        }
        return false;
    }

    // any data written into this vector will get saved with the map data. Must take care to do own versioning, and endianess if applicable. Will not get called when loading maps from other games, so provide defaults.
    void writegamedata(vector<char> &extras) {}
    void readgamedata(vector<char> &extras) {}

    const char *savedconfig() { return "config.cfg"; }
    const char *restoreconfig() { return "restore.cfg"; }
    const char *defaultconfig() { return "data/defaults.cfg"; }
    const char *autoexec() { return "autoexec.cfg"; }
    const char *savedservers() { return "servers.cfg"; }

    void loadconfigs()
    {
        execident("playsong");

        execfile("auth.cfg", false);
    }
}

