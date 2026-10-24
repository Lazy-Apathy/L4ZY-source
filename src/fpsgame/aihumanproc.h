// Human perception for bots (see aihuman.h): the enemy part of process().
// Not a standalone header: ai.cpp includes it once, inside namespace ai, right before process().

    // the enemy part of process() for a human-perception bot: same aim and shooting, but it
    // only knows what it noticed, aims at where it believes the enemy is when it cannot see him,
    // and at the part of him it sees when the middle of his body is hidden
    AI_HUMAN int processhuman(fpsent *d, aistate &b, const vec &dp, bool idle, float frame, int skmod)
    {
        int result = 0;
        fpsent *e = getclient(d->ai->enemy);
        aiperceived *p = e && targetable(d, e) ? findperceived(d, e) : NULL;
        bool enemyok = p && p->aware;
        if(!enemyok || d->skill >= 50)
        {
            fpsent *f = (fpsent *)intersectclosest(dp, d->ai->target, d);
            if(f)
            {
                if(targetable(d, f))
                {
                    aiperceived *q = findperceived(d, f);
                    // under its crosshair, and noticed: another enemy passing there does not take its eyes off the one
                    // it sees; he becomes the one it goes for when that one is out of its sight (or it had none)
                    if(f != e && q && q->visible && q->tracking && !(enemyok && p->visible && p->tracking))
                    {
                        violence(d, b, f, d->gunselect == GUN_FIST ? 1 : 0);
                        enemyok = true;
                        e = f;
                    }
                }
                else enemyok = false;
            }
            else if(!enemyok && target(d, b, d->gunselect == GUN_FIST ? 1 : 0, false, SIGHTMIN))
            {
                e = getclient(d->ai->enemy);
                enemyok = e != NULL;
            }
        }
        p = enemyok && e ? findperceived(d, e) : NULL;
        if(p && p->aware)
        {
            bool insight = p->visible && p->tracking,
                 quick = !insight && p->knownsrc == AI_P_SEEN && lastmillis - p->vislast <= (d->gunselect == GUN_CG ? 300 : skmod)+30;
            vec guess = insight ? e->o : guesspos(d, e, *p);
            // an old guess far away (a duel: never forgotten) is kept in mind, but it looks where it goes
            if(!insight && !quick && lastmillis - p->knownmillis > botmemory*1000 && guess.squaredist(dp) > 512*512) return botmode(d) == 2 ? stylelook(d, b, dp, &guess) : 0;
            if(botmode(d) == 2) return styleenemy(d, b, e, *p, dp, insight, quick, guess);
            vec ep = insight ? humanaim(d, e, *p) : guessaimpos(d, e, guess);
            float yaw, pitch;
            getyawpitch(dp, ep, yaw, pitch);
            if(!insight) guesspitch(d, dp, ep, pitch);
            fixrange(yaw, pitch);
            if(insight) d->ai->enemyseen = lastmillis;
            float sskew = insight || d->skill > 100 ? 1.5f : 1.f;
            if(insight && lockon(d, e, 16))
            {
                d->ai->targyaw = yaw;
                d->ai->targpitch = pitch;
                if(!idle) frame *= 2;
                d->ai->becareful = false;
            }
            scaleyawpitch(d->yaw, d->pitch, yaw, pitch, frame, sskew);
            if(insight || quick)
            {
                if(canshoot(d, e) && hastarget(d, b, e, yaw, pitch, dp.squaredist(ep)))
                {
                    d->attacking = true;
                    d->ai->lastaction = lastmillis;
                    result = 3;
                }
                else result = 2;
            }
            else result = 1;
        }
        else
        {
            d->ai->enemy = -1;
            d->ai->enemyseen = d->ai->enemymillis = 0;
            if(botmode(d) == 2) result = stylelook(d, b, dp);
        }
        return result;
    }
    #include "ainet.h" // Tiny Apa: its moves from a small network (botnet 1)
