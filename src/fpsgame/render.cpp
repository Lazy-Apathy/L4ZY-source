#include "game.h"

extern int hwrt;
extern int hwrtvelfreezepose;
extern bool hwrtvelskelpass;
extern void hwrtvelnotecolourmuzzle(const vec &m);
extern void hwrtvelmark();
extern void hwrtvelunmark();

struct spawninfo { const extentity *e; float weight; };
extern float gatherspawninfos(dynent *d, int tag, vector<spawninfo> &spawninfos);

namespace game
{      
    vector<fpsent *> bestplayers;
    vector<const char *> bestteams;

    VARP(ragdoll, 0, 1, 1);
    VARP(ragdollmillis, 0, 10000, 300000);
    VARP(ragdollfade, 0, 1000, 300000);
    VARFP(playermodel, 0, 0, 4, changedplayermodel());
    VARP(forceplayermodels, 0, 0, 1);
    VARP(hidedead, 0, 0, 2);

    vector<fpsent *> ragdolls;

    void saveragdoll(fpsent *d)
    {
        if(!d->ragdoll || !ragdollmillis || (!ragdollfade && lastmillis > d->lastpain + ragdollmillis)) return;
        fpsent *r = new fpsent(*d);
        r->lastupdate = ragdollfade && lastmillis > d->lastpain + max(ragdollmillis - ragdollfade, 0) ? lastmillis - max(ragdollmillis - ragdollfade, 0) : d->lastpain;
        r->edit = NULL;
        r->ai = NULL;
        r->attackchan = r->idlechan = -1;
        if(d==player1) r->playermodel = playermodel;
        ragdolls.add(r);
        d->ragdoll = NULL;   
    }

    void clearragdolls()
    {
        ragdolls.deletecontents();
    }

    void moveragdolls()
    {
        loopv(ragdolls)
        {
            fpsent *d = ragdolls[i];
            if(lastmillis > d->lastupdate + ragdollmillis)
            {
                delete ragdolls.remove(i--);
                continue;
            }
            moveragdoll(d);
        }
    }

    static const playermodelinfo playermodels[5] =
    {
        { "mrfixit", "mrfixit/blue", "mrfixit/red", "mrfixit/hudguns", NULL, "mrfixit/horns", { "mrfixit/armor/blue", "mrfixit/armor/green", "mrfixit/armor/yellow" }, "mrfixit", "mrfixit_blue", "mrfixit_red", true },
        { "snoutx10k", "snoutx10k/blue", "snoutx10k/red", "snoutx10k/hudguns", NULL, "snoutx10k/wings", { "snoutx10k/armor/blue", "snoutx10k/armor/green", "snoutx10k/armor/yellow" }, "snoutx10k", "snoutx10k_blue", "snoutx10k_red", true },
        //{ "ogro/green", "ogro/blue", "ogro/red", "mrfixit/hudguns", "ogro/vwep", NULL, { NULL, NULL, NULL }, "ogro", "ogro_blue", "ogro_red", false },
        { "ogro2", "ogro2/blue", "ogro2/red", "mrfixit/hudguns", NULL, "ogro2/quad", { "ogro2/armor/blue", "ogro2/armor/green", "ogro2/armor/yellow" }, "ogro", "ogro_blue", "ogro_red", true },
        { "inky", "inky/blue", "inky/red", "inky/hudguns", NULL, "inky/quad", { "inky/armor/blue", "inky/armor/green", "inky/armor/yellow" }, "inky", "inky_blue", "inky_red", true },
        { "captaincannon", "captaincannon/blue", "captaincannon/red", "captaincannon/hudguns", NULL, "captaincannon/quad", { "captaincannon/armor/blue", "captaincannon/armor/green", "captaincannon/armor/yellow" }, "captaincannon", "captaincannon_blue", "captaincannon_red", true }
    };

    int chooserandomplayermodel(int seed)
    {
        return (seed&0xFFFF)%(sizeof(playermodels)/sizeof(playermodels[0]));
    }

    const playermodelinfo *getplayermodelinfo(int n)
    {
        if(size_t(n) >= sizeof(playermodels)/sizeof(playermodels[0])) return NULL;
        return &playermodels[n];
    }

    const playermodelinfo &getplayermodelinfo(fpsent *d)
    {
        const playermodelinfo *mdl = getplayermodelinfo(d==player1 || forceplayermodels ? playermodel : d->playermodel);
        if(!mdl) mdl = getplayermodelinfo(playermodel);
        return *mdl;
    }

    void changedplayermodel()
    {
        if(player1->clientnum < 0) player1->playermodel = playermodel;
        if(player1->ragdoll) cleanragdoll(player1);
        loopv(ragdolls) 
        {
            fpsent *d = ragdolls[i];
            if(!d->ragdoll) continue;
            if(!forceplayermodels)
            {
                const playermodelinfo *mdl = getplayermodelinfo(d->playermodel);
                if(mdl) continue;
            }
            cleanragdoll(d);
        }
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == player1 || !d->ragdoll) continue;
            if(!forceplayermodels)
            {
                const playermodelinfo *mdl = getplayermodelinfo(d->playermodel);
                if(mdl) continue;
            }
            cleanragdoll(d);
        }
    }

    void preloadplayermodel()
    {
        loopi(sizeof(playermodels)/sizeof(playermodels[0]))
        {
            const playermodelinfo *mdl = getplayermodelinfo(i);
            if(!mdl) break;
            // Vanilla only loads the local look. RT needs every look a stranger
            // can wear, otherwise the first time they walk in rebuilds the skin
            // atlas and freezes the frame. hwrt 0 keeps the old filter.
            if(!::hwrt && i != playermodel && (!multiplayer(false) || forceplayermodels)) continue;
            if(m_teammode || ::hwrt)
            {
                preloadmodel(mdl->blueteam);
                preloadmodel(mdl->redteam);
            }
            if(!m_teammode || ::hwrt) preloadmodel(mdl->ffa);
            if(mdl->vwep) preloadmodel(mdl->vwep);
            if(mdl->quad) preloadmodel(mdl->quad);
            loopj(3) if(mdl->armour[j]) preloadmodel(mdl->armour[j]);
            if(i == playermodel) loopj(NUMFRIENDCOLORS) hasfriendskin(i, j);
        }
    }

    bool hasfriendskin(int playermodelidx, int coloridx)
    {
        static char tried[5][NUMFRIENDCOLORS], ok[5][NUMFRIENDCOLORS];
        if(playermodelidx < 0 || playermodelidx > 4 || coloridx < 0 || coloridx >= NUMFRIENDCOLORS) return false;
        if(tried[playermodelidx][coloridx]) return ok[playermodelidx][coloridx] != 0;
        tried[playermodelidx][coloridx] = 1;
        string cfg;
        formatstring(cfg, "packages/models/%s/c%d/md5.cfg", playermodels[playermodelidx].ffa, coloridx);
        stream *f = openfile(cfg, "rb");
        if(!f)
        {
            formatstring(cfg, "packages/models/%s/c%d/iqm.cfg", playermodels[playermodelidx].ffa, coloridx);
            f = openfile(cfg, "rb");
        }
        if(!f) return false;
        delete f;
        defformatstring(mdlpath, "%s/c%d", playermodels[playermodelidx].ffa, coloridx);
        ok[playermodelidx][coloridx] = loadmodel(mdlpath, -1, false) ? 1 : 0;
        return ok[playermodelidx][coloridx] != 0;
    }

    static const char *friendskinmdl(int playermodelidx, int coloridx)
    {
        static string p;
        if(!hasfriendskin(playermodelidx, coloridx)) return NULL;
        formatstring(p, "%s/c%d", playermodels[playermodelidx].ffa, coloridx);
        return p;
    }

    VAR(testquad, 0, 0, 1);
    VAR(testarmour, 0, 0, 1);
    // Lot 4 slot test: 0 = testquad as usual, 1 = local player only, 2 = bots only.
    VAR(hwrtvelquadwho, 0, 0, 2);
    // Lot 4 slot test: pin every player's gun so a bot AI cannot swap vwep mid-trial.
    VAR(hwrtvelkeepgun, -1, -1, int(GUN_PISTOL));

    ICOMMAND(hwrtvelhudgun, "i", (int *g),
    {
        int gun = clamp(*g, int(GUN_FIST), int(GUN_PISTOL));
        hwrtvelkeepgun = gun;
        loopi(numdynents())
        {
            dynent *e = iterdynents(i);
            if(!e || e->type!=ENT_PLAYER) continue;
            fpsent *p = (fpsent *)e;
            p->gunselect = gun;
            if(p->ammo[gun] < 20) p->ammo[gun] = 50;
        }
        conoutf("hwrt velocity: hud gun %d", gun);
    });

    ICOMMAND(hwrtvelfire, "", (),
    {
        fpsent *d = hudplayer();
        if(!d) return;
        setvar("hwrtvelfreezepose", 0);
        d->lastaction = lastmillis;
        d->lastattackgun = d->gunselect;
        conoutf("hwrt velocity: fire anim gun %d millis %d", d->gunselect, lastmillis);
    });

    ICOMMAND(hwrtvelgiveequip, "iii", (int *atype, int *gun, int *quad),
    {
        if(*atype < 0)
        {
            testarmour = 0;
            loopi(numdynents())
            {
                dynent *d = iterdynents(i);
                if(d && d->type==ENT_PLAYER) ((fpsent *)d)->armour = 0;
            }
        }
        else
        {
            testarmour = 1;
            int t = clamp(*atype, 0, 2);
            loopi(numdynents())
            {
                dynent *d = iterdynents(i);
                if(!d || d->type!=ENT_PLAYER) continue;
                fpsent *p = (fpsent *)d;
                p->armourtype = t;
                p->armour = t==A_YELLOW ? 200 : 100;
            }
        }
        testquad = *quad ? 1 : 0;
        if(*quad) hwrtvelquadwho = 0;
        if(*gun >= GUN_FIST && *gun <= GUN_PISTOL)
        {
            loopi(numdynents())
            {
                dynent *d = iterdynents(i);
                if(!d || d->type!=ENT_PLAYER) continue;
                fpsent *p = (fpsent *)d;
                p->gunselect = *gun;
                if(p->ammo[*gun] < 20) p->ammo[*gun] = 50;
            }
        }
        conoutf("hwrt velocity: equip armourtype=%d gun=%d quad=%d (testarmour %d)", *atype, *gun, *quad, testarmour);
    });
    VAR(testteam, 0, 0, 3);

    enum { PV_IDLE = 0, PV_TAUNT, PV_WIN, PV_LOSE, PV_PAIN, PV_SHOOT, PV_LAG };

    static fpsent *previewent = NULL;
    static int pvmodel = -1, pvkind = PV_IDLE, pvstart = 0, pvdur = 0, pvlastms = 0;
    static bool pvintro = true, pvmanual = false, pvhold = false;
    static float pvyaw = 210;

    void rotateplayerpreview(float dyaw)
    {
        pvyaw = fmod(pvyaw + dyaw, 360.0f);
        if(pvyaw < 0) pvyaw += 360.0f;
        pvmanual = true;
    }

    void holdplayerpreview(bool on)
    {
        pvhold = on;
    }

    static void pickpreviewclip()
    {
        int r = rnd(100);
        if(r < 38) { pvkind = PV_TAUNT; pvdur = 1100; }
        else if(r < 58) { pvkind = PV_WIN; pvdur = 2600; }
        else if(r < 72) { pvkind = PV_SHOOT; pvdur = guns[previewent->gunselect].attackdelay + 90; }
        else if(r < 84) { pvkind = PV_LOSE; pvdur = 2300; }
        else if(r < 94) { pvkind = PV_PAIN; pvdur = 480; }
        else { pvkind = PV_LAG; pvdur = 1300; }
        pvstart = lastmillis;
    }

    static void tickpreviewanims(int model, int weap)
    {
        weap = clamp(weap, int(GUN_FIST), int(GUN_PISTOL));
        if(pvlastms && lastmillis - pvlastms > 500)
        {
            pvkind = PV_IDLE;
            pvstart = lastmillis;
            pvdur = 1200;
            pvintro = true;
            pvmanual = false;
            previewent->gunselect = weap;
        }
        if(model != pvmodel)
        {
            pvmodel = model;
            pvkind = PV_IDLE;
            pvstart = lastmillis;
            pvdur = 1200;
            pvintro = true;
            previewent->gunselect = weap;
        }
        if(lastmillis - pvstart >= pvdur)
        {
            if(pvkind != PV_IDLE)
            {
                pvkind = PV_IDLE;
                pvstart = lastmillis;
                pvdur = 3800 + rnd(2800);
                previewent->gunselect = rnd(100) < 40 ? GUN_FIST + rnd(GUN_PISTOL - GUN_FIST + 1) : weap;
            }
            else if(pvintro)
            {
                pvintro = false;
                pvkind = PV_TAUNT;
                pvdur = 1100;
                pvstart = lastmillis;
            }
            else pickpreviewclip();
        }
        int dt = pvlastms ? clamp(lastmillis - pvlastms, 0, 80) : 0;
        pvlastms = lastmillis;
        if(pvkind == PV_IDLE && !pvmanual && !pvhold) pvyaw = fmod(pvyaw + dt * (360.0f/40000.0f), 360.0f);
        previewent->yaw = pvyaw;
        previewent->pitch = 0;
        previewent->lastattackgun = pvkind == PV_SHOOT ? previewent->gunselect : -1;
        previewent->lastaction = pvkind == PV_SHOOT || pvkind == PV_TAUNT || pvkind == PV_PAIN ? pvstart : 0;
        previewent->lasttaunt = 0;
        previewent->lastpain = 0;
    }

    void renderplayer(fpsent *d, const playermodelinfo &mdl, int team, float fade, bool mainpass)
    {
        if(d != previewent && hwrtvelkeepgun >= GUN_FIST && hwrtvelkeepgun <= GUN_PISTOL)
        {
            d->gunselect = hwrtvelkeepgun;
            if(d->ammo[d->gunselect] < 20) d->ammo[d->gunselect] = 50;
        }
        int lastaction = d->lastaction, hold = mdl.vwep || d->gunselect==GUN_PISTOL ? 0 : (ANIM_HOLD1+d->gunselect)|ANIM_LOOP, attack = ANIM_ATTACK1+d->gunselect, delay = mdl.vwep ? 300 : guns[d->gunselect].attackdelay+50;
        if(d == previewent)
        {
            switch(pvkind)
            {
                case PV_TAUNT:
                    lastaction = pvstart;
                    hold = attack = ANIM_TAUNT;
                    delay = 1000;
                    break;
                case PV_WIN:
                    hold = ANIM_WIN|ANIM_LOOP;
                    attack = 0;
                    lastaction = 0;
                    delay = 0;
                    break;
                case PV_LOSE:
                    hold = ANIM_LOSE|ANIM_LOOP;
                    attack = 0;
                    lastaction = 0;
                    delay = 0;
                    break;
                case PV_PAIN:
                    lastaction = pvstart;
                    hold = attack = ANIM_PAIN;
                    delay = pvdur;
                    break;
                case PV_SHOOT:
                    lastaction = pvstart;
                    delay = mdl.vwep ? 300 : guns[d->gunselect].attackdelay+50;
                    break;
                case PV_LAG:
                    hold = ANIM_LAG|ANIM_LOOP;
                    attack = 0;
                    lastaction = 0;
                    delay = 0;
                    break;
            }
        }
        else if(intermission && d->state!=CS_DEAD)
        {
            lastaction = 0;
            hold = attack = ANIM_LOSE|ANIM_LOOP;
            delay = 0;
            if(m_teammode ? bestteams.htfind(d->team)>=0 : bestplayers.find(d)>=0) hold = attack = ANIM_WIN|ANIM_LOOP;
        }
        else if(d->state==CS_ALIVE && d->lasttaunt && lastmillis-d->lasttaunt<1000 && lastmillis-d->lastaction>delay)
        {
            lastaction = d->lasttaunt;
            hold = attack = ANIM_TAUNT;
            delay = 1000;
        }
        modelattach a[5];
        static const char * const vweps[] = {"vwep/fist", "vwep/shotg", "vwep/chaing", "vwep/rocket", "vwep/rifle", "vwep/gl", "vwep/pistol"};
        int ai = 0;
        if((!mdl.vwep || d->gunselect!=GUN_FIST) && d->gunselect<=GUN_PISTOL)
        {
            int vanim = ANIM_VWEP_IDLE|ANIM_LOOP, vtime = 0;
            if(lastaction && d->lastattackgun==d->gunselect && lastmillis < lastaction + delay)
            {
                vanim = ANIM_VWEP_SHOOT;
                vtime = lastaction;
            }
            a[ai++] = modelattach("tag_weapon", mdl.vwep ? mdl.vwep : vweps[d->gunselect], vanim, vtime);
        }
        if(d->state==CS_ALIVE)
        {
            bool wantquad = (testquad || d->quadmillis) != 0;
            if(hwrtvelquadwho == 1) wantquad = (d == player1) || d->quadmillis;
            else if(hwrtvelquadwho == 2) wantquad = (d != player1) || d->quadmillis;
            if(wantquad && mdl.quad)
                a[ai++] = modelattach("tag_powerup", mdl.quad, ANIM_POWERUP|ANIM_LOOP, 0);
            if(testarmour || d->armour)
            {
                int type = clamp(d->armourtype, (int)A_BLUE, (int)A_YELLOW);
                if(mdl.armour[type])
                    a[ai++] = modelattach("tag_shield", mdl.armour[type], ANIM_SHIELD|ANIM_LOOP, 0);
            }
        }
        if(mainpass)
        {
            d->muzzle = vec(-1, -1, -1);
            a[ai++] = modelattach("tag_muzzle", &d->muzzle);
        }
        const char *mdlname = mdl.ffa;
        switch(testteam ? testteam-1 : team)
        {
            case 1: mdlname = mdl.blueteam; break;
            case 2: mdlname = mdl.redteam; break;
        }
        int pmi = playermodel, cidx = -1;
        loopi(5) if(&playermodels[i]==&mdl) { pmi = i; break; }
        if(d==previewent && playerpreviewtint>=0) cidx = playerpreviewtint;
        else if(d->type==ENT_PLAYER)
        {
            fpsent *p = (fpsent *)d;
            if(isselfplayer(p) && selfhascolor()) cidx = selfcolorindex();
            else if(isfriend(p)) cidx = friendcolorindex(isfriendally(p));
        }
        if(cidx>=0)
        {
            const char *fs = friendskinmdl(pmi, cidx);
            if(fs) mdlname = fs;
        }
        renderclient(d, mdlname, a[0].tag ? a : NULL, hold, attack, delay, lastaction, intermission && d->state!=CS_DEAD ? 0 : d->lastpain, fade, ragdoll && mdl.ragdoll);
#if 0
        if(d->state!=CS_DEAD && d->quadmillis) 
        {
            entitylight light;
            rendermodel(&light, "quadrings", ANIM_MAPMODEL|ANIM_LOOP, vec(d->o).sub(vec(0, 0, d->eyeheight/2)), 360*lastmillis/1000.0f, 0, MDL_DYNSHADOW | MDL_CULL_VFC | MDL_CULL_DIST);
        }
#endif
    }

    const char *dynentmdlname(dynent *d)
    {
        if(!d) return NULL;
        if(d->type == ENT_AI) return monstermdlname(d);
        if(d->type == ENT_INANIMATE) return movablemdlname(d);
        if(d->type != ENT_PLAYER) return NULL;
        fpsent *p = (fpsent *)d;
        const playermodelinfo &mdl = getplayermodelinfo(p);
        int team = demohd::visteam(p);
        if(!team && (teamskins || m_teammode)) team = isteam(player1->team, p->team) ? 1 : 2;
        const char *mdlname = mdl.ffa;
        switch(testteam ? testteam-1 : team)
        {
            case 1: mdlname = mdl.blueteam; break;
            case 2: mdlname = mdl.redteam; break;
        }
        int pmi = playermodel, cidx = -1;
        loopi(5) if(&playermodels[i]==&mdl) { pmi = i; break; }
        if(isselfplayer(p) && selfhascolor()) cidx = selfcolorindex();
        else if(isfriend(p)) cidx = friendcolorindex(isfriendally(p));
        if(cidx>=0)
        {
            const char *fs = friendskinmdl(pmi, cidx);
            if(fs) mdlname = fs;
        }
        return mdlname;
    }

    VARP(teamskins, 0, 0, 1);

#if 0
    // for testing spawns

    float hsv2rgb(float h, float s, float v, int n)
    {
        float k = fmod(n + h / 60.0f, 6.0f);
        return v - v * s * max(min(min(k, 4.0f - k), 1.0f), 0.0f);
    }

    vec hsv2rgb(float h, float s, float v)
    {
        return vec(hsv2rgb(h, s, v, 5), hsv2rgb(h, s, v, 3), hsv2rgb(h, s, v, 1));
    }

    void renderspawn(const vec &o, int rating, float probability)
    {
        defformatstring(score, "%d", rating);
        defformatstring(percentage, "(%.2f%%)", probability * 100);
        bvec colorvec = bvec::fromcolor(hsv2rgb(rating * 1.2f, 0.8, 1));
        int color = (colorvec.r << 16) + (colorvec.g << 8) + colorvec.b;
        particle_textcopy(vec(o).addz(5), score, PART_TEXT, 1, color, 5.0f);
        particle_textcopy(vec(o).addz(1), percentage, PART_TEXT, 1, color, 4.0f);
    }

    void renderspawns()
    {
        vector<spawninfo> spawninfos;
        float ratingsum = gatherspawninfos(player1, 0, spawninfos);
        loopv(spawninfos) renderspawn(spawninfos[i].e->o, spawninfos[i].weight * 100, spawninfos[i].weight / ratingsum);
    }

    VAR(dbgspawns, 0, 0, 1);
#endif

    VARP(statusicons, 0, 1, 1);

    void renderstatusicons(fpsent *d, int team, float yoffset)
    {
        vec p = d->abovehead().madd(camup, yoffset);
        int icons = 0;
        const itemstat &boost = itemstats[I_BOOST-I_SHELLS];
        if(statusicons && (d->state==CS_ALIVE || d->state==CS_LAGGED))
        {
            if(d->quadmillis) icons++;
            if(d->maxhealth>100) icons += (min(d->maxhealth, boost.max) - 100 + boost.info-1) / boost.info;
            if(d->armour>0 && d->armourtype>=A_GREEN && !m_noitems) icons++;
        }
        if(icons) concatstring(d->info, " ");
        int ncolor = team ? (team==1 ? 0x6496FF : 0xFF4B19) : 0x1EC850;
        ncolor = friendnamecolor(d, ncolor);
        particle_text(p, d->info, PART_TEXT, 1, ncolor, 2.0f, 0, icons);
        if(icons)
        {
            float tw, th;
            text_boundsf(d->info, tw, th);
            float offset = (tw - icons*th)/2;
            if(d->armour>0 && d->armourtype>=A_GREEN && !m_noitems)
            {
                int icon = itemstats[(d->armourtype==A_YELLOW ? I_YELLOWARMOUR : I_GREENARMOUR)-I_SHELLS].icon;
                particle_texticon(p, icon%4, icon/4, offset, PART_TEXT_ICON, 1, 0xFFFFFF, 2.0f);
                offset += th;
            }
            for(int i = 100; i < min(d->maxhealth, boost.max); i += boost.info)
            {
                particle_texticon(p, boost.icon%4, boost.icon/4, offset, PART_TEXT_ICON, 1, 0xFFFFFF, 2.0f);
                offset += th;
            }
            if(d->quadmillis)
            {
                int icon = itemstats[I_QUAD-I_SHELLS].icon;
                particle_texticon(p, icon%4, icon/4, offset, PART_TEXT_ICON, 1, 0xFFFFFF, 2.0f);
                offset += th;
            }
        }
    }

    VARP(statusbars, 0, 1, 2);
    FVARP(statusbarscale, 0, 1, 2);
    VARP(lookselfshadow, 0, 1, 1);

    float renderstatusbars(fpsent *d, int team)
    {
        if(!statusbars || m_insta || (player1->state==CS_SPECTATOR ? statusbars <= 1 : team != 1) || (d->state!=CS_ALIVE && d->state!=CS_LAGGED)) return 0;
        vec p = d->abovehead().msub(camdir, 50/80.0f).msub(camup, 2.0f);
        float offset = 0;
        float scale = statusbarscale;
        if(d->armour > 0)
        {
            int limit = d->armourtype==A_YELLOW ? 200 : (d->armourtype==A_GREEN ? 100 : 50);
            int color = d->armourtype==A_YELLOW ? 0xFFC040 : (d->armourtype==A_GREEN ? 0x008C00 : 0x0B5899);
            float size = scale*sqrtf(max(d->armour, limit)/100.0f);
            float fill = float(d->armour)/limit;
            offset += size;
            particle_meter(vec(p).madd(camup, offset), fill, PART_METER, 1, color, 0, size);
        }
        int color = d->health<=25 ? 0xFF0000 : (d->health<=50 ? 0xFF8000 : (d->health<=100 ? 0x40FF80 : 0x40C0FF));
        float size = scale*sqrtf(max(d->health, d->maxhealth)/100.0f);
        float fill = float(d->health)/d->maxhealth;
        offset += size;
        particle_meter(vec(p).madd(camup, offset), fill, PART_METER, 1, color, 0, size);
        return offset;
    }

    void rendergame(bool mainpass)
    {
        if(mainpass) ai::render();

        if(intermission)
        {
            bestteams.shrink(0);
            bestplayers.shrink(0);
            if(m_teammode) getbestteams(bestteams);
            else getbestplayers(bestplayers);
        }

        startmodelbatches();

        // First-person hides the local / followed body. Still draw it into the
        // shadow map so you get a sun-shadow on the ground. Other players already
        // cast when shadowmap is on. Uncheck lookselfshadow if indoor maps look wrong.
        bool shadowself = shadowmapping && lookselfshadow;
        fpsent *exclude = (isthirdperson() || shadowself) ? NULL : followingplayer();
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == player1 || d->state==CS_SPECTATOR || d->state==CS_SPAWNING || d->lifesequence < 0 || d == exclude || (d->state==CS_DEAD && hidedead)) continue;
            int team = demohd::visteam(d);
            if(!team && (teamskins || m_teammode)) team = isteam(player1->team, d->team) ? 1 : 2;
            renderplayer(d, getplayermodelinfo(d), team, 1, mainpass);

            vec dir = vec(d->o).sub(camera1->o);
            float dist = dir.magnitude();
            dir.div(dist);
            if(d->state!=CS_EDITING && raycube(camera1->o, dir, dist, 0) < dist)
            {
                d->info[0] = '\0';
                continue;
            }

            copystring(d->info, colorname(d));
            if(d->state!=CS_DEAD)
            {
                float offset = renderstatusbars(d, team);
                renderstatusicons(d, team, offset);
            }
        }
        loopv(ragdolls)
        {
            fpsent *d = ragdolls[i];
            int team = demohd::visteam(d);
            if(!team && (teamskins || m_teammode)) team = isteam(player1->team, d->team) ? 1 : 2;
            float fade = 1.0f;
            if(ragdollmillis && ragdollfade) 
                fade -= clamp(float(lastmillis - (d->lastupdate + max(ragdollmillis - ragdollfade, 0)))/min(ragdollmillis, ragdollfade), 0.0f, 1.0f);
            renderplayer(d, getplayermodelinfo(d), team, fade, mainpass);
        } 
        if((isthirdperson() || shadowself) && !followingplayer() && player1->state!=CS_SPECTATOR && (player1->state!=CS_DEAD || hidedead != 1)) renderplayer(player1, getplayermodelinfo(player1), teamskins || m_teammode ? 1 : 0, 1, mainpass);
        rendermonsters();
        rendermovables();
        // Colour stencil: rendergame() runs after hwrtvelunmark(), so pickups
        // would be EXCLUDE and the object MV pass could not overwrite them.
        // Mark the same hwrtentxform instances the rigid velocity pass draws.
        hwrtvelmark();
        entities::renderentities();
        hwrtvelunmark();
        renderbouncers();
        renderprojectiles();
        if(cmode) cmode->rendergame();

#if 0
        if(dbgspawns) renderspawns();
#endif

        endmodelbatches();
    }

    VARP(hudgun, 0, 1, 1);
    VARP(hudgunsway, 0, 1, 1);
    VARP(teamhudguns, 0, 1, 1);
    VARP(chainsawhudgun, 0, 1, 1);
    VAR(testhudgun, 0, 0, 1);

    FVAR(swaystep, 1, 35.0f, 100);
    FVAR(swayside, 0, 0.04f, 1);
    FVAR(swayup, -1, 0.05f, 1);

    float swayfade = 0, swayspeed = 0, swaydist = 0;
    vec swaydir(0, 0, 0);

    void swayhudgun(int curtime)
    {
        fpsent *d = hudplayer();
        if(d->state != CS_SPECTATOR)
        {
            if(d->physstate >= PHYS_SLOPE)
            {
                swayspeed = min(sqrtf(d->vel.x*d->vel.x + d->vel.y*d->vel.y), d->maxspeed);
                swaydist += swayspeed*curtime/1000.0f;
                swaydist = fmod(swaydist, 2*swaystep);
                swayfade = 1;
            }
            else if(swayfade > 0)
            {
                swaydist += swayspeed*swayfade*curtime/1000.0f;
                swaydist = fmod(swaydist, 2*swaystep);
                swayfade -= 0.5f*(curtime*d->maxspeed)/(swaystep*1000.0f);
            }

            float k = pow(0.7f, curtime/10.0f);
            swaydir.mul(k);
            vec vel(d->vel);
            vel.add(d->falling);
            swaydir.add(vec(vel).mul((1-k)/(15*max(vel.magnitude(), d->maxspeed))));
        }
    }

    struct hudent : dynent
    {
        hudent() { type = ENT_CAMERA; }
    } guninterp;

    SVARP(hudgunsdir, "");

    void drawhudmodel(fpsent *d, int anim, float speed = 0, int base = 0)
    {
        if(d->gunselect>GUN_PISTOL) return;

        vec sway;
        vecfromyawpitch(d->yaw, 0, 0, 1, sway);
        float steps = swaydist/swaystep*M_PI;
        sway.mul(swayside*cosf(steps));
        sway.z = swayup*(fabs(sinf(steps)) - 1);
        sway.add(swaydir).add(d->o);
        if(!hudgunsway) sway = d->o;

#if 0
        if(player1->state!=CS_DEAD && player1->quadmillis)
        {
            float t = 0.5f + 0.5f*sinf(2*M_PI*lastmillis/1000.0f);
            color.y = color.y*(1-t) + t;
        }
#endif
        const playermodelinfo &mdl = getplayermodelinfo(d);
        defformatstring(gunname, "%s/%s", hudgunsdir[0] ? hudgunsdir : mdl.hudguns, guns[d->gunselect].file);
        // first-person arm follows the body tint: you, or the friend you spectate / watch in a demo
        int hcidx = -1;
        if(isselfplayer(d) && selfhascolor()) hcidx = selfcolorindex();
        else if(isfriend(d) && friendhascolor(isfriendally(d))) hcidx = friendcolorindex(isfriendally(d));
        if(hcidx >= 0)
        {
            string fallback;
            copystring(fallback, gunname);
            defformatstring(suf, "/c%d", hcidx);
            concatstring(gunname, suf);
            if(!loadmodel(gunname, -1, false)) copystring(gunname, fallback);
        }
        else if(int vt = demohd::visteam(d)) concatstring(gunname, vt==1 ? "/blue" : "/red");
        else if((m_teammode || teamskins) && teamhudguns)
            concatstring(gunname, d==player1 || isteam(d->team, player1->team) ? "/blue" : "/red");
        else if(testteam > 1)
            concatstring(gunname, testteam==2 ? "/blue" : "/red");
        modelattach a[2];
        d->muzzle = vec(-1, -1, -1);
        a[0] = modelattach("tag_muzzle", &d->muzzle);
        dynent *interp = NULL;
        if(d->gunselect==GUN_FIST && chainsawhudgun)
        {
            anim |= ANIM_LOOP;
            base = 0;
            interp = &guninterp;
        }
        rendermodel(NULL, gunname, anim, sway, testhudgun ? 0 : d->yaw+90, testhudgun ? 0 : d->pitch, MDL_LIGHT|MDL_HUD, interp, a, base, (int)ceil(speed));
        if(d->muzzle.x >= 0) d->muzzle = calcavatarpos(d->muzzle, 12);
        if(!hwrtvelskelpass) hwrtvelnotecolourmuzzle(d->muzzle);
    }

    void drawhudgun()
    {
        fpsent *d = hudplayer();
        if(d->state==CS_SPECTATOR || d->state==CS_EDITING || !hudgun || editmode) 
        { 
            d->muzzle = player1->muzzle = vec(-1, -1, -1);
            return;
        }

        int rtime = guns[d->gunselect].attackdelay;
        if(hwrtvelfreezepose)
        {
            drawhudmodel(d, ANIM_GUN_IDLE|ANIM_START);
        }
        else if(d->lastaction && d->lastattackgun==d->gunselect && lastmillis-d->lastaction<rtime)
        {
            drawhudmodel(d, ANIM_GUN_SHOOT|ANIM_SETSPEED, rtime/17.0f, d->lastaction);
        }
        else
        {
            drawhudmodel(d, ANIM_GUN_IDLE|ANIM_LOOP);
        }
    }

    void renderavatar()
    {
        drawhudgun();
    }

    VAR(playerpreviewtint, -1, -1, 9);

    void renderplayerpreview(int model, int team, int weap)
    {
        if(!previewent)
        {
            previewent = new fpsent;
            previewent->light.color = vec(1, 1, 1);
            previewent->light.dir = vec(0, -1, 2).normalize();
            loopi(GUN_PISTOL-GUN_FIST) previewent->ammo[GUN_FIST+1+i] = 1;
        }
        tickpreviewanims(model, weap);
        float height = previewent->eyeheight + previewent->aboveeye,
              zrad = height/2;
        vec2 xyrad = vec2(previewent->xradius, previewent->yradius).max(height/4);
        float posyaw = previewent->yaw;
        previewent->o = calcmodelpreviewpos(vec(xyrad, zrad), posyaw).addz(previewent->eyeheight - zrad);
        previewent->yaw = pvyaw;
        previewent->light.dir = vec(0, -1, 2).normalize();
        previewent->light.color = vec(1, 1, 1);
        previewent->light.millis = -1;
        const playermodelinfo *mdlinfo = getplayermodelinfo(model);
        if(!mdlinfo) return;
        renderplayer(previewent, *mdlinfo, team >= 0 && team <= 2 ? team : 0, 1, false);
    }

    vec hudgunorigin(int gun, const vec &from, const vec &to, fpsent *d)
    {
        if(d->muzzle.x >= 0) return d->muzzle;
        vec offset(from);
        if(d!=hudplayer() || isthirdperson())
        {
            vec front, right;
            vecfromyawpitch(d->yaw, d->pitch, 1, 0, front);
            offset.add(front.mul(d->radius));
            if(d->type!=ENT_AI)
            {
                offset.z += (d->aboveeye + d->eyeheight)*0.75f - d->eyeheight;
                vecfromyawpitch(d->yaw, 0, 0, -1, right);
                offset.add(right.mul(0.5f*d->radius));
                offset.add(front);
            }
            return offset;
        }
        offset.add(vec(to).sub(from).normalize().mul(2));
        if(hudgun)
        {
            offset.sub(vec(camup).mul(1.0f));
            offset.add(vec(camright).mul(0.8f));
        }
        else offset.sub(vec(camup).mul(0.8f));
        return offset;
    }

    void preloadweapons()
    {
        const playermodelinfo &mdl = getplayermodelinfo(player1);
        loopi(NUMGUNS)
        {
            const char *file = guns[i].file;
            if(!file) continue;
            string fname;
            if(selfhascolor())
            {
                formatstring(fname, "%s/%s/c%d", hudgunsdir[0] ? hudgunsdir : mdl.hudguns, file, selfcolorindex());
                preloadmodel(fname);
            }
            else if((m_teammode || teamskins) && teamhudguns)
            {
                formatstring(fname, "%s/%s/blue", hudgunsdir[0] ? hudgunsdir : mdl.hudguns, file);
                preloadmodel(fname);
            }
            else
            {
                formatstring(fname, "%s/%s", hudgunsdir[0] ? hudgunsdir : mdl.hudguns, file);
                preloadmodel(fname);
            }
            formatstring(fname, "vwep/%s", file);
            preloadmodel(fname);
        }
    }

    void preloadsounds()
    {
        for(int i = S_JUMP; i <= S_SPLASH2; i++) preloadsound(i);
        for(int i = S_JUMPPAD; i <= S_PISTOL; i++) preloadsound(i);
        for(int i = S_V_BOOST; i <= S_V_QUAD10; i++) preloadsound(i);
        for(int i = S_BURN; i <= S_HIT; i++) preloadsound(i);
    }

    void preload()
    {
        if(hudgun) preloadweapons();
        preloadbouncers();
        preloadplayermodel();
        preloadsounds();
        entities::preloadentities();
        if(m_sp) preloadmonsters();
    }

    void preloadallplayermodelsforrt()
    {
        loopi(sizeof(playermodels)/sizeof(playermodels[0]))
        {
            const playermodelinfo *mdl = getplayermodelinfo(i);
            if(!mdl) break;
            if(m_teammode || teamskins)
            {
                hwrtpreloadmodel(mdl->blueteam);
                hwrtpreloadmodel(mdl->redteam);
            }
            else hwrtpreloadmodel(mdl->ffa);
            if(mdl->vwep) hwrtpreloadmodel(mdl->vwep);
            if(mdl->quad) hwrtpreloadmodel(mdl->quad);
            loopj(3) if(mdl->armour[j]) hwrtpreloadmodel(mdl->armour[j]);
        }
        static const char * const vweps[] = { "vwep/fist", "vwep/shotg", "vwep/chaing", "vwep/rocket", "vwep/rifle", "vwep/gl", "vwep/pistol" };
        loopi(int(sizeof(vweps)/sizeof(vweps[0]))) hwrtpreloadmodel(vweps[i]);
    }

    // Same rule as entities::preloadentities() and the server's canspawnitem():
    // a pickup the current mode can never spawn (insta, no ammo) is not worth a
    // BLAS slot or a skin layer.
    bool itemcanspawnforrt(int type)
    {
        if(type >= I_SHELLS && type <= I_CARTRIDGES) return !m_noitems && !m_noammo;
        if(type >= I_HEALTH && type <= I_QUAD) return !m_noitems;
        if(type == CARROT || type == RESPAWNPOINT) return m_classicsp;
        return true;
    }
}

void hwrtpreloadplayermodels()
{
    game::preloadallplayermodelsforrt();
}

bool hwrtitemcanspawn(const extentity &e)
{
    return game::itemcanspawnforrt(e.type);
}

const char *hwrtdynentmdlname(dynent *d)
{
    return game::dynentmdlname(d);
}

void hwrtentxform(const extentity &e, vec &o, float &yaw)
{
    entities::entxform(e, o, yaw);
}

dynent *hwrtcameradynent()
{
    return game::followingplayer();
}

int hwrtdynentattach(dynent *d, modelattach *dst, int maxa)
{
    if(!d || !dst || maxa < 2) return 0;
    memset(dst, 0, sizeof(modelattach)*maxa);
    if(d->type != ENT_PLAYER) return 0;
    fpsent *p = (fpsent *)d;
    const game::playermodelinfo &mdl = game::getplayermodelinfo(p);
    static const char * const vweps[] = {"vwep/fist", "vwep/shotg", "vwep/chaing", "vwep/rocket", "vwep/rifle", "vwep/gl", "vwep/pistol"};
    int n = 0;
    if((!mdl.vwep || p->gunselect!=GUN_FIST) && p->gunselect<=GUN_PISTOL && n < maxa-1)
    {
        int vanim = ANIM_VWEP_IDLE|ANIM_LOOP;
        dst[n++] = modelattach("tag_weapon", mdl.vwep ? mdl.vwep : vweps[p->gunselect], vanim, 0);
    }
    if(p->state==CS_ALIVE && n < maxa-1)
    {
        if(p->quadmillis && mdl.quad)
            dst[n++] = modelattach("tag_powerup", mdl.quad, ANIM_POWERUP|ANIM_LOOP, 0);
        if(p->armour && n < maxa-1)
        {
            int type = clamp(p->armourtype, (int)A_BLUE, (int)A_YELLOW);
            if(mdl.armour[type])
                dst[n++] = modelattach("tag_shield", mdl.armour[type], ANIM_SHIELD|ANIM_LOOP, 0);
        }
    }
    return n;
}

int hwrtdynentseq(dynent *d)
{
    if(!d || d->type != ENT_PLAYER) return 0;
    return ((fpsent *)d)->lifesequence;
}

int hwrtdynentuid(dynent *d)
{
    if(!d || d->type != ENT_PLAYER) return 0;
    fpsent *p = (fpsent *)d;
    return (p->clientnum + 1) * 10007 + p->lifesequence;
}

// Same tests rendergame() uses to decide whether a player is rasterised at
// all, so the TLAS holds exactly the bodies on screen. Monsters and movables
// have no hidedead equivalent.
bool hwrtdynentvisible(dynent *d)
{
    if(!d) return false;
    if(d->type != ENT_PLAYER) return true;
    fpsent *p = (fpsent *)d;
    if(p == game::player1)
    {
        if(game::followingplayer()) return false;
        return p->state != CS_SPECTATOR && (p->state != CS_DEAD || game::hidedead != 1);
    }
    if(p->lifesequence < 0) return false;
    return !(p->state == CS_DEAD && game::hidedead);
}

// The corpses kept after their owner respawned. rendergame() draws this whole
// list unconditionally; hidedead never fills it, because a hidden player is
// never rendered and so never grows the ragdoll saveragdoll() copies.
int hwrtnumragdolls()
{
    return game::ragdolls.length();
}

dynent *hwrtragdoll(int i)
{
    return game::ragdolls.inrange(i) ? game::ragdolls[i] : NULL;
}

