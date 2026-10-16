// Bots that aim and play like a measured reference player (botperception 2, or a bot a clan server asks for).
// Not a standalone header: ai.cpp includes it once, inside namespace ai, right before aihumanproc.h.
// It sits on top of human perception (aihuman.h: a bot only knows what a player in its place could know) and
// changes, for these bots only:
//  - the aim: human gestures towards what the bot noticed (a fast bell-shaped flick that often overshoots, one or
//    more corrections, then a slightly late tracking), aimed at a random point on or around the body whose spread
//    follows the size of the target on screen (so accuracy hardly depends on distance), and a shot that leaves
//    soon after the crosshair gets there, at the same rhythm as the reference player (shortly after the weapon is
//    ready again); big turns are single fast flicks, and a target that drifts away is caught up with a new flick;
//  - the moves: under fire it holds its place, dodging sideways around the spot it stands on instead of running at
//    the enemy (so it keeps the distance it had), with steps and pauses of irregular lengths as measured (no
//    rhythm), a short stop being ended by a step aside just before the enemy can shoot again; it keeps that place a
//    moment after losing sight of him, never stands still for long without a reason (another way when one is shut,
//    a new place when its place is lost), and it jumps much less;
//  - with no enemy in mind (duel, free for all): it holds a place and looks at the spawn points one by one (not the
//    nearest), and around, then goes to another place with a clear view, looking around rather than along its way;
//    a defender with nothing to do keeps moving around its spot; while it waits for a lost enemy it looks at his
//    ways out one after the other rather than at a single one;
//  - when an enemy it was following comes back into view after a short break (a pillar, a corner), it needs to
//    notice him again before it can shoot; a big flick seldom ends in a shot at once: the hand settles first;
//  - after a frag it checks the spawn points by jumps of its view, now and then behind it, and while it searches
//    its view rests on what it checks instead of drifting as it moves;
//  - a few decisions: holding the line a lost enemy should come out of (duel), a quiet approach (no needless
//    jumps while closing in), turning to a shot that nearly hit, the most threatening enemy first, checking the
//    spawn points after a frag in a duel (never those the game cannot use, too close to the bot), guessing where
//    an enemy respawns in team games from where the bot's team mates stand, knowing where its killer was when he
//    survived, and forgetting an enemy after enough other events rather than after a fixed time.
// The numbers come from measured games of a reference player (HD demos): reaction, gesture speed and overshoot,
// corrections, accuracy, tracking delay, shot rhythm, moves under fire, and how much all this varies from one game
// to the next.
// botstylelevel sets how good these bots are in local games, 50 to 100 (100 = the reference player, 50 = about half
// the accuracy and slower; online always the bot's own skill). botstyleform 1 draws a different form for each bot
// and each game.

    // 0 ("each bot's own skill", the default of older versions) comes back as 100, 1 to 49 as 50 (as they counted)
    VARFP(botstylelevel, 0, 100, 100, { if(botstylelevel < 50) botstylelevel = botstylelevel ? 50 : 100; });
    VARP(botstyleform, 0, 1, 1);

    // measured on the reference player (instagib rifle unless said otherwise), then fitted on real target paths
    static const float SREACTFORM = 41,    // ms, spread of the median reaction from one game to the next
                       SREACTLEVEL = 3,    // ms added to the median reaction per level point under 100
                       SREACTDUEL = 60,    // ms less in a duel
                       // gesture duration = SDUR0 * amplitude^SDURK ms; fitted so that the peak speed follows the
                       // reference player's by size of gesture (240 deg/s at 10-20 deg up to 1 400 deg/s at 160-400 deg)
                       SDUR0 = 82.5f, SDURK = 0.24f, SDURSD = 0.25f,
                       SPROFK = 2.5f,      // bell of the gesture speed: (t(1-t))^SPROFK
                       SGAIN = 0.99f, SGAIN15 = 0.26f, SGAINK = -0.55f, // gesture gain, its spread at 15 degrees
                       SPERP = 3.57f,      // degrees, sideways error of a gesture
                       SLEAD = 0.41f,      // part of the target's movement during the gesture that is anticipated
                       SCORR = 88, SCORRSD = 0.6f,     // ms, delay before a correction
                       SLAG = 90,          // ms, the tracking follows where the target was that long ago,
                       SPRED = 1.0f,       //   carried on at the speed it had then (smooth pursuit)
                       STRACK = 0.124f, SJITTER = 1.31f, // tracking pull (per 5 ms) and tremor
                       SVMAX = 928,        // deg/s, fastest smooth tracking; beyond, the hand catches up with flicks
                       SCATCH = 1.5f, SCATCHDEG = 2.38f, SCATCHMS = 207, // a new flick when the target drifted further than
                                           //   SCATCH half widths (at least SCATCHDEG degrees), SCATCHMS after the last one
                       SOFF = 0.593f, SOFFV = 1.32f,     // aimed point around the body, in half widths of the target
                       // the shot leaves as soon as the crosshair passes close enough to the middle of the target: within
                       // SCLIC half widths (drawn for each shot, log-normal spread SCLICSD, wider against a target hard to
                       // predict, x2 after waiting SPATIENCE ms), or within SFLICKTOL for a part SFLICK of the shots (on the
                       // move); after a shot the hand notices the weapon is ready again SREPRISE ms late (median): measured,
                       // the reference player fires about 70 ms after the crosshair reaches the target, and about 220 ms
                       // after the weapon is ready again when the target is still in sight
                       SCLIC = 1.15f, SCLICSD = 0.325f, SFLICK = 0.108f, SFLICKTOL = 2.85f,
                       SREPRISE = 150, SREPRISESD = 0.66f,
                       STRIG = 40, STRIGSD = 0.5f,       // ms, trigger delay
                       SPATIENCE = 500,   // ms, the tolerance doubles after waiting this long
                       SMINSHOT = 60,     // ms, no shot earlier than this after a gesture starts
                       SLEVELERR = 0.8f,  // aiming error at level 50: x (1 + SLEVELERR)
                       SFORMACC = 0.12f,  // spread of the aiming error from one game to the next (log)
                       SFORMSPEED = 0.107f, // spread of the gesture speed from one game to the next (log)
                       // how hard the target is to hit (measured: the reference player's accuracy falls from 60 % on
                       // a target that kept its way for a second to 40 % on one that changed sides 3 times, and from
                       // 58 to 44 % along the linearity of his path; a target standing still is easier): the aimed
                       // point and the shot tolerance spread by exp(SUINV * changes of side in the last second + SULIN *
                       // (1 - linearity over 500 ms) - SUSTOP * standing still), worked out all along from what the
                       // target really does
                       SUINV = 0.107f, SULIN = 0.511f, SUSTOP = 0.11f,
                       // moves (measured in the reference player's duels: with the enemy in sight, sideways 3/4 of the
                       // time and only 15 % towards him, about 1.3 changes of side a second, within 128 u of a spot most of
                       // the time; 4 to 7 jumps a minute)
                       SDODGER = 112,      // u, radius of the place it holds around the spot it stood on
                       SKEEPMIN = 140,     // u, closer to the enemy than that it also steps back
                       SKEEPLOST = 1500,   // ms it keeps its place after losing sight of the enemy (team games)
                       SKEEPDUEL = 3000,   // in a duel, median (log-normal 0.5, 1.5 to 8 s): measured, the player stays within 64 u of
                                           // where they lost him 68 % of the time after 1-2 s, 58 % after 2-4 s, 43 % after 4-6 s
                       SJUMPKEEP = 0.15f,  // part of the random jumps of the base game it keeps
                       STURNBACK = 120,    // deg: following its way that far round is put off a moment
                       STURNMS = 450,      //   (median ms), it walks backwards or sideways meanwhile
                       // the dodge, measured in the reference player's duels with the enemy in sight: short sideways steps of
                       // very uneven lengths (median 180 ms, 90 % under 400 ms) and pauses between them (median 110 ms, 10 %
                       // over 350 ms), the other way three times out of four, a quarter of them also forwards or backwards,
                       // no rhythm; when they stand still, they step aside about 280 ms before the enemy's weapon is ready
                       // again (or about 400 ms after he starts looking at them)
                       SLEG = 250, SLEGSD = 0.55f,        // ms a sideways key is held (log-normal)
                       SPAUSEP = 0.25f, SPAUSE = 85, SPAUSESD = 0.8f, // a pause after a step: how often, how long (ms)
                       SSIDECHANGE = 0.8f,                // the next step goes the other way
                       SSTOPSHOT = 0.5f,                  // after a shot of the enemy it saw, how often it stops until just
                                                          //   before his weapon is ready again (to make its own shot sure)
                       SSTOPMAX = 1100,                   // ms, longest such stop or pause (with the slowing down, a stop stays
                                                          //   under 1.5 s; the player's stops in sight: 90 % under 1.2 s)
                       SDIAG = 0.12f,                     // a step also goes forwards or backwards
                       SDODGELEAD = 220, SDODGELEADSD = 0.5f, // ms before the enemy's weapon is ready again
                       SDODGESEEN = 320, SDODGESEENSD = 0.5f, // ms after the enemy starts aiming at it
                       SNOSTEP = 300,     // ms without any way to step: it leaves that place
                       // a line held standing still (measured: the player's stops in sight last 560 ms in median, 90 % under 1.2 s),
                       // then a step aside
                       SHOLDMS = 560, SHOLDSD = 0.4f, SHOLDMAX = 1300,
                       // while it waits for a lost enemy, it looks at his ways out one after the other (measured: 675 ms on
                       // one in median, 230 ms to 2.6 s), a way behind it now and then
                       SEXITDWELL = 675, SEXITDWELLSD = 0.8f, SEXITBACK = 0.15f,
                       // he may have gone round (the reference player is rarely taken from behind while they hold: 2
                       // times in 31 holds; "I look at them one by one, or see all the options at once by changing my
                       // angle"): after SEXITOWNAFTER ms (before, he most likely comes back where he went out: turning
                       // away sooner got the bot killed from behind), the ways into its own place too, more and more
                       // (SEXITOWN of the looks, SEXITOWNMAX 3 s later), or the middle of all of them when they hold in view
                       SEXITOWNAFTER = 1500, SEXITOWN = 0.3f, SEXITOWNMAX = 0.6f,
                       // no enemy in mind (duel, free for all): it holds a place, looking at spawn points one by one (225 ms
                       // each, measured: 1.2 a second, almost never the nearest first) and around, then moves to another
                       // place with a clear view; it looks along its way only now and then (measured: 17 % of the time)
                       SSEARCHHOLD = 2500, SSEARCHHOLDSD = 0.6f,
                       SSPAWNLOOK = 225, SSPAWNLOOKSD = 0.4f, SLOOKAROUND = 800, SLOOKAROUNDSD = 0.5f,
                       SLOOKSPAWNP = 0.4f, SLOOKWAYP = 0.2f,
                       // seen again after a short break (a pillar, a corner, less than PGAPMS ms: the eyes kept him), a
                       // player needs to notice him again before shooting (measured: after such a break they shoot 410 ms
                       // later in median when their aim was already on him, 520-620 ms otherwise; 2 to 9 % under 250 ms)
                       SREGAP = 60,        // ms: a shorter break does not stop the eyes following him
                       // a big flick seldom ends in a shot at once (measured on the player's first shots: fired during the gesture
                       // for a third of the gestures of 8-15 degrees, a quarter of 15-30, 7 % of 30-60, 4 % beyond); when
                       // it did not, the hand settles first (their shot comes 325 ms after the end of a 15-30 degree gesture
                       // in median, 430 ms after 30-60, 525 ms beyond): ms before the trigger can be pulled after a
                       // gesture of 15 / 30 / 60 degrees that did not end in a shot (added to the hand's own corrections)
                       SSETTLE15 = 160, SSETTLE30 = 330, SSETTLE60 = 300, SSETTLESD = 0.5f,
                       // after a frag, the spawn points by jumps of the view (measured: the first one checked is in front of
                       // them, the next ones 40 to 46 degrees from where they looked, 13 to 20 % behind them), 280-500 ms each
                       // (SWATCHBACK, SSEARCHBACK : how likely a spawn point behind it is chosen next, after a frag / while it searches)
                       // v5: calmer (the player's searches: big turns of 90 degrees and more 0.2 a second, the bots' 0.4)
                       SWATCHMS = 330, SWATCHRND = 300, SWATCHBACK = 0.2f, SSEARCHBACK = 0.15f,
                       // just lost from sight, it keeps the line where he went out, a little ahead along his way (measured:
                       // hidden 0.5 to 6 s, he comes back 9.5 degrees from where they last saw him, 8.5 from that point carried
                       // on 300 ms along his way, and their aim is 6 degrees from him; they keep that line 2 s and more)
                       SLINEMS = 2000, SLINESD = 0.5f, SLINELEAD = 300,
                       SLOOKMOVED = 48,   // u: a view rested on a point is set again once the bot moved that far
                       SRESTDEG = 8;      //   or once that point is that many degrees away
    // just respawned, a player first finds his bearings: no gesture and no shot before (measured on the reference player,
    // the enemy in sight from their respawn: their first gesture towards him starts 206 / 240 / 280 / 339 ms after the respawn,
    // q10 / q25 / median / q75, and their first shot comes 0.5 to 1.3 s after it, under 500 ms 3.6 % of the time); where their
    // killer was is only used once they have their bearings
    static const float SORIENT = 280, SORIENTSD = 0.25f;
    // where a lost enemy will come back into view (measured on the reference player's duels, each guess worked out 400 ms
    // before he came back into their line of sight, from where they stood then, angle to him in median: the line where he went
    // out, carried on 300 ms along his way, 6.4 deg in the first second and 9.5 the next; the spot where they last saw him
    // 13 deg after 2 s; his nearest way out by the waypoints 14 to 23 deg, his way towards them 13 to 23 deg, one of his ways
    // out at random (the v4 sweep) 15 to 25 deg; their own aim 5.5 deg): that line is held, then that spot, the view resting
    // on it; now and then, once he has been gone a while, a short glance at one of his ways out or at a way into its own
    // place (he may have gone round), then back to his line
    static const float SPRELINE = 350,    // ms: the seen line itself first (as before)
                       SPREPV = 1500,     // ms: his line carried on along his way until then, then the spot where it lost him
                       SPRESWITCH = 2000, // ms: from then on, now and then a glance elsewhere
                       SPREGLANCE = 0.2f, // how often a look is such a glance
                       SPREDWELL = 1500, SPREDWELLSD = 0.5f, // ms its line is watched before the next look is drawn
                       SPREGLANCEMS = 600, // ms a glance lasts (median)
                       SPREREST = 4, SPREMOVED = 24, // its view resting on his line is set again beyond that (degrees, u)
                       SPREKNOWN = 1000;  // ms: where its killer was is watched itself first
    // out of a fight a player does not walk from waypoint to waypoint: he heads for the furthest point of his way he can
    // walk to straight (corners cut, on flat enough ground), and keeps the keys he holds a little while rather than switching
    // them at each bend
    static const float SSMOOTH = 160,    // u, furthest point of its way it heads for
                       SKEYHYST = 15;    // degrees: the keys held are kept while the way is within that much more than half
                                         // a key direction (22.5 deg) of them
    // the strong position "cover and jump" (the reference player's words: "hidden by a cover high enough that nothing shows
    // (or very little), but low enough to look at what you want to shoot when you jump"; measured in the player's duels: about one
    // such hold a minute, 2 to 3 s in median (up to 7 s), still behind a cover whose top is between their head and their eye at the
    // top of a jump, about 10 u in front of them, one jump every 1.4 to 1.5 s (median), shots at the top of the jump and on the
    // way down): when it waits for a lost enemy it now and then takes such a spot close by, from which it does not see the
    // line it watches (nor is seen) standing but sees it at the top of a jump, and jumps to look over
    static const float SCOVERP = 0.9f,      // how often, at each look (about once a second), it takes a cover when one is there
                       SCOVERFIND = 128,    // u, how far a cover may be
                       SCOVERHOLD = 2800, SCOVERHOLDSD = 0.45f, // ms it stays there
                       SCOVERJUMP = 1450, SCOVERJUMPSD = 0.3f,  // ms between two jumps
                       SCOVERFIRST = 500,   // ms before the first jump once there
                       SCOVERAPEX = 16,     // u: its eye at the top of a jump is at least that much higher
                       SCOVERSTEP = 220, SCOVERPAUSE = 150; // ms of a step aside along its cover, of a pause between two
    static const int SHIST = 64, SPATH = 64, SPATHSTEP = 20;
    static const int stylekeys[8][2] = { { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } }; // keys by 45 degrees, as process()

    enum { STYLE_NONE = 0, STYLE_GESTURE, STYLE_WAIT, STYLE_TRACK };
    enum { STYLE_MV_NONE = 0, STYLE_MV_HOLD, STYLE_MV_CHASE, STYLE_MV_GUARD };  // moves: holds its place under fire, goes after
                                                                                 // the enemy, holds a place with nobody in sight
    enum { STYLE_SR_NONE = 0, STYLE_SR_HOLD, STYLE_SR_MOVE };  // search with no enemy in mind: holds a place, goes to another
    enum { STYLE_LK_NONE = 0, STYLE_LK_SPAWN, STYLE_LK_AROUND, STYLE_LK_WAY }; // what it looks at meanwhile

    struct aistyle
    {
        aiinfo *owner;
        int life, formgame, respawnlife;
        float formacc, formreact, formspeed;
        // aim
        int aimcn, aimlife, phase, ncorr, onset, fireat, readyat, lastshot, lastupdate, lastinsight, gend, reprise;
        float gy0, gp0, gdy, gdp, gdur, gt, wait, oy, op, spread, clic;
        bool flick;
        int histn, histhead, histt[SHIST];
        float histy[SHIST], histp[SHIST];
        // decisions
        int holduntil, holdcool, seenmillis, linedur;
        float seenpos[3], seenvel[3];
        bool holdseen, wassee;
        int watchcn, watchuntil, watchnext, watchdone;
        float watchat[3];
        bool watching;
        int killercn, killerlife, killermillis;
        float killerpos[3];
        int orientuntil;   // just respawned: finding its bearings until then (no gesture, no shot)
        bool killerpending; // where its killer was, used once it has its bearings
        // a lost enemy: his nearest ways out and the way he would take towards the bot (from where it lost him), worked out
        // again when the bot moved or after a while, and which one it watches until when (-1 none, 0..2 a way out, 3 the way
        // towards the bot, 4 a way into its own place)
        int prestamp, prenum, prepick, preuntil;
        float prebase[3], preexits[1][3], prepoint[3];
        // its way out of a fight: the waypoint it heads for straight (corners cut), when it was chosen, the keys it holds
        int smoothnode, smoothat, walkkey;
        int keeplost; // ms it keeps its place after losing sight of him this time
        // the strong position "cover and jump": 0 none, 1 going to the spot, 2 there (hidden, jumps to look over); until
        // when, the next jump, when it started, the next time it may look for one, since when the enemy is in sight
        int covermode, coveruntil, covernext, coverstart, covercheck, coverseen, coverstep, coverdir, covermoved;
        float coverat[3], coverlook[3], coverpos[2];
        // moves
        int mvmode, mvcn, mvlife, mvseen, dodgenext, dodgeside, turnat, stepat, stepmove, stepstrafe;
        float dodgeat[3];
        int steppause, progat, nostep, nostepgaveup, blockdir, blockuntil, holddone, walkto, walkstart;
        int walkcheck, nwalktried, walktried[4]; // walking off an isolated waypoint: headway, waypoints it could not reach
        float walkpos[2];
        float progpos[2];
        int seenshot, reloadplan, threatat, threatplan, stopforshot; // when it expects the enemy to shoot
        int escat, escuntil, escmove, escstrafe; // stuck (keys held, going nowhere): a few steps another way
        float escpos[2];
        // search with no enemy in mind, and what it looks at meanwhile
        int srmode, sruntil, srnode, srstart, lookmode, lookuntil, lookspawn;
        float lookat[3];
        unsigned int srdone[2];
        // the ways into its place (waypoints it sees that lead where it does not see), and when it last looked at each
        int nplaceexits, placeexitsat, placeexitlook[8];
        float coveryaw, coverspread; // where the ways into its place lie, how far apart (degrees)
        float placeexits[8][3], placeexitsfrom[3];
        // ways out of a lost enemy it looks at, one after the other
        int exitlook, exituntil, exitstamp; // exitlook: one of his ways out, -2 a way into its own place, -3 the middle of all
        float exitat[3];
        // seen again after a short break (no shot before regain), the gesture in progress (its size, whether it may end in a
        // shot, whether it goes to an enemy), until when the hand settles after it, and the direction its view rests on
        // while it searches or checks the spawn points (set again once it moved away from where it chose it)
        int seenlast, regain, settleuntil;
        float gamp;
        bool gflick, gtarget, gshot, regainpending;
        float restyaw, restpitch, restfrom[3];

        void reset(aiinfo *ai)
        {
            owner = ai;
            life = formgame = respawnlife = -1;
            formacc = formreact = formspeed = 0;
            aimcn = aimlife = -1;
            phase = ncorr = onset = fireat = readyat = lastshot = lastupdate = lastinsight = gend = reprise = 0;
            gy0 = gp0 = gdy = gdp = gdur = gt = wait = oy = op = 0;
            clic = SCLIC;
            spread = 1;
            flick = false;
            histn = histhead = 0;
            holduntil = holdcool = seenmillis = linedur = 0;
            seenpos[0] = seenpos[1] = seenpos[2] = 0;
            seenvel[0] = seenvel[1] = seenvel[2] = 0;
            holdseen = wassee = false;
            watchcn = -1;
            watchuntil = watchnext = watchdone = 0;
            watchat[0] = watchat[1] = watchat[2] = 0;
            watching = false;
            killercn = killerlife = -1;
            killermillis = 0;
            killerpos[0] = killerpos[1] = killerpos[2] = 0;
            orientuntil = keeplost = 0;
            killerpending = false;
            resetpreaim();
            resetcover();
            mvmode = STYLE_MV_NONE;
            mvcn = mvlife = -1;
            mvseen = dodgenext = turnat = stepat = stepmove = stepstrafe = 0;
            dodgeside = 1;
            dodgeat[0] = dodgeat[1] = dodgeat[2] = 0;
            resetmoves();
            seenshot = reloadplan = threatat = threatplan = stopforshot = 0;
            escat = escuntil = escmove = escstrafe = 0;
            escpos[0] = escpos[1] = 0;
            resetsearch();
            exitlook = -1;
            exituntil = exitstamp = 0;
            exitat[0] = exitat[1] = exitat[2] = 0;
            resetaim();
        }

        void resetaim()
        {
            seenlast = regain = settleuntil = 0;
            gamp = 0;
            gflick = true;
            gtarget = gshot = regainpending = false;
            restyaw = restpitch = 0;
            restfrom[0] = restfrom[1] = restfrom[2] = 0;
        }

        void resetpreaim()
        {
            prestamp = prenum = preuntil = 0;
            prepick = -1;
            loopi(3) { prebase[i] = prepoint[i] = preexits[0][i] = 0; }
        }

        void resetcover()
        {
            covermode = coveruntil = covernext = coverstart = covercheck = coverseen = coverstep = coverdir = covermoved = 0;
            loopi(3) coverat[i] = coverlook[i] = 0;
            coverpos[0] = coverpos[1] = 0;
        }

        void resetmoves()
        {
            smoothnode = walkkey = -1;
            smoothat = 0;
            steppause = progat = nostep = nostepgaveup = blockdir = blockuntil = holddone = walkstart = 0;
            walkto = -1;
            walkcheck = nwalktried = 0;
            walkpos[0] = walkpos[1] = 0;
            progpos[0] = progpos[1] = 0;
        }

        void resetsearch()
        {
            srmode = STYLE_SR_NONE;
            sruntil = srstart = 0;
            srnode = -1;
            lookmode = STYLE_LK_NONE;
            lookuntil = 0;
            lookspawn = -1;
            lookat[0] = lookat[1] = lookat[2] = 0;
            srdone[0] = srdone[1] = 0;
            nplaceexits = placeexitsat = 0;
            coveryaw = 0; coverspread = 360;
            placeexitsfrom[0] = placeexitsfrom[1] = placeexitsfrom[2] = 0;
        }
    };
    static aistyle aistyles[MAXBOTS];

    static inline bool stylebot(fpsent *d) { return botmode(d) == 2 && humanbotcheck(d); }
    static void styledrop(int slot) { aistyles[slot].owner = NULL; } // a new form and a fresh state next time

    static aistyle *style(fpsent *d)
    {
        int i = d->clientnum - MAXCLIENTS;
        return d->ai && i >= 0 && i < MAXBOTS ? &aistyles[i] : NULL;
    }

    static float stylelevel(fpsent *d) // 50..100, % of the reference player's level
    {
        // online, the skill the bot was added with: the player whose game plays it does not choose
        return !multiplayer(false) ? clamp(botstylelevel, 50, 100) : clamp(d->skill, 50, 100);
    }

    static float lognormal(float median, float sd);
    static float styleslow(fpsent *d);

    static aistyle &claimstyle(fpsent *d)
    {
        aistyle &s = *style(d);
        if(s.owner != d->ai || s.formgame != maptime)
        { // a new bot or a new game: a new form
            s.reset(d->ai);
            s.formgame = maptime;
            if(botstyleform)
            {
                s.formacc = clamp(rndgauss(), -2.0f, 2.0f);
                s.formreact = clamp(rndgauss(), -2.0f, 2.0f);
                s.formspeed = clamp(rndgauss(), -2.0f, 2.0f);
            }
        }
        if(s.life != d->lifesequence)
        { // a new life: aim, holds, moves and spawn checks start again; the killer is remembered
            s.life = d->lifesequence;
            s.aimcn = -1;
            s.phase = STYLE_NONE;
            s.holduntil = s.holdcool = 0;
            s.watchuntil = 0;
            s.watching = false;
            s.mvmode = STYLE_MV_NONE;
            s.mvcn = s.mvlife = -1;
            s.mvseen = s.turnat = 0;
            s.resetmoves();
            s.resetsearch();
            s.seenshot = s.reloadplan = s.threatat = s.threatplan = s.stopforshot = 0;
            s.escuntil = 0;
            s.exitlook = -1;
            s.exituntil = s.exitstamp = 0;
            s.resetaim();
            s.resetpreaim();
            s.resetcover();
            s.keeplost = 0;
            // a new life: it finds its bearings first (no gesture, no shot); its killer is remembered meanwhile
            s.orientuntil = lastmillis + int(clamp(lognormal(SORIENT, SORIENTSD), 150.0f, 600.0f)*styleslow(d));
            s.killerpending = s.killercn >= 0;
        }
        return s;
    }

    // level and form: errors grow and gestures slow down under level 100, and both vary from game to game
    static float styleerr(fpsent *d, aistyle &s) { return (1 + SLEVELERR*(100 - stylelevel(d))/50.0f)*expf(SFORMACC*s.formacc); }
    static float stylespeed(fpsent *d, aistyle &s) { return (1 + 0.3f*(100 - stylelevel(d))/50.0f)*expf(-SFORMSPEED*s.formspeed); }
    static float styleslow(fpsent *d) { return 1 + 0.5f*(100 - stylelevel(d))/50.0f; }

    static float stylereactshift(fpsent *d) // ms added to the median reaction of human perception
    {
        aistyle *s = style(d);
        float form = s && s->owner == d->ai ? s->formreact : 0;
        // a single enemy alive (a duel): expected, noticed sooner (measured: 240 ms in duels against 305 in team games)
        float duel = aliveenemies(d) == 1 ? -SREACTDUEL : 0;
        return SREACTFORM*form + SREACTLEVEL*(100 - stylelevel(d)) - (100 - clamp(d->skill, 1, 100))*2.2f + duel;
    }

    static float lognormal(float median, float sd) { return median*expf(sd*rndgauss()); }

    static float stylewrap(float a) { while(a > 180) a -= 360; while(a < -180) a += 360; return a; }

    // position along a gesture (0..1) whose speed follows the bell (t(1-t))^SPROFK
    static float styleprofile(float t)
    {
        static float table[65];
        static bool done = false;
        if(!done)
        {
            float sum = 0, v[257];
            loopi(257) { float x = i/256.0f; v[i] = powf(x*(1 - x), SPROFK); }
            table[0] = 0;
            loopi(64)
            {
                loopj(4) sum += (v[i*4 + j] + v[i*4 + j + 1])*0.5f;
                table[i + 1] = sum;
            }
            loopi(65) table[i] /= sum;
            done = true;
        }
        t = clamp(t, 0.0f, 1.0f)*64;
        int i = min(int(t), 63);
        return table[i] + (table[i + 1] - table[i])*(t - i);
    }

    static vec styleaimpoint(fpsent *d, fpsent *e) // like getaimpos, without its random offsets
    {
        vec o = e->o;
        if(d->gunselect == GUN_RL) o.z += (e->aboveeye*0.2f)-(0.8f*d->eyeheight);
        else if(d->gunselect != GUN_GL) o.z += (e->aboveeye-e->eyeheight)*0.5f;
        return o;
    }

    static void stylehistory(aistyle &s, float y, float p)
    {
        if(s.histn && fabs(stylewrap(y - s.histy[s.histhead])) + fabs(p - s.histp[s.histhead]) > 8) s.histn = 0; // a jump (new guess, new way): no speed from it
        s.histhead = (s.histhead + 1)%SHIST;
        s.histt[s.histhead] = lastmillis;
        s.histy[s.histhead] = y;
        s.histp[s.histhead] = p;
        if(s.histn < SHIST) s.histn++;
    }

    // where the target was ago ms earlier (yaw unwrapped around ref), and how fast it moved then (degrees per second)
    static void stylepast(aistyle &s, int ago, float ref, float &y, float &p, float &vy, float &vp)
    {
        int want = lastmillis - ago, k = s.histhead, n = 1;
        while(n < s.histn && s.histt[k] > want) { k = (k + SHIST - 1)%SHIST; n++; }
        y = ref + stylewrap(s.histy[k] - ref);
        p = s.histp[k];
        int j = k, m = n;
        while(m < s.histn && s.histt[k] - s.histt[j] < 50) { j = (j + SHIST - 1)%SHIST; m++; }
        int dt = s.histt[k] - s.histt[j];
        if(dt > 0) { vy = stylewrap(s.histy[k] - s.histy[j])*1000.0f/dt; vp = (s.histp[k] - s.histp[j])*1000.0f/dt; }
        else vy = vp = 0;
        vy = clamp(vy, -SVMAX, SVMAX); // a hand cannot follow faster than that smoothly (it catches up with a flick)
        vp = clamp(vp, -SVMAX, SVMAX);
    }

    // where each player went during the last second (every SPATHSTEP ms): how predictable a target is
    struct stylepath { int life, n, head, t[SPATH]; float x[SPATH], y[SPATH]; };
    static stylepath stylepaths[MAXCLIENTS+MAXBOTS];
    static int stylepathmillis = -1;

    static void stylesample() // once per SPATHSTEP ms for every player alive
    {
        if(stylepathmillis >= 0 && lastmillis - stylepathmillis < SPATHSTEP && lastmillis >= stylepathmillis) return;
        stylepathmillis = lastmillis;
        loopv(players)
        {
            fpsent *e = players[i];
            if(e->clientnum < 0 || e->clientnum >= MAXCLIENTS+MAXBOTS) continue;
            stylepath &h = stylepaths[e->clientnum];
            if(e->state != CS_ALIVE || h.life != e->lifesequence + 1 || (h.n && lastmillis - h.t[h.head] > 10*SPATHSTEP))
            { // a new life, or no news for a while (a new map): a new path
                h.n = h.head = 0;
                h.life = e->state == CS_ALIVE ? e->lifesequence + 1 : 0;
            }
            if(e->state != CS_ALIVE) continue;
            h.head = (h.head + 1)%SPATH;
            h.t[h.head] = lastmillis; h.x[h.head] = e->o.x; h.y[h.head] = e->o.y;
            if(h.n < SPATH) h.n++;
        }
    }

    // how hard e is to hit for d, from his last second of movement (as measured on the reference player's demos:
    // changes of side across d's line of sight, linearity, standing still); only the spread of the
    // aimed point depends on it
    static float styleunpred(fpsent *d, fpsent *e)
    {
        if(!e || e->clientnum < 0 || e->clientnum >= MAXCLIENTS+MAXBOTS) return 1;
        stylepath &h = stylepaths[e->clientnum];
        if(h.n < 3) return 1;
        vec2 los(e->o.x - d->o.x, e->o.y - d->o.y);
        float l = los.magnitude();
        if(l < 1) return 1;
        vec2 side(-los.y/l, los.x/l);
        int changes = 0, lastsign = 0, n = min(h.n, 1000/SPATHSTEP + 1);
        float path = 0, net = 0, speed = 0, prevlat = 0;
        int newest = h.head, k0 = (h.head - (n - 1) + SPATH)%SPATH;
        for(int j = 1; j < n; j++)
        {
            int a = (k0 + j - 1)%SPATH, b = (k0 + j)%SPATH, dt = max(h.t[b] - h.t[a], 1);
            float vx = (h.x[b] - h.x[a])*1000.0f/dt, vy = (h.y[b] - h.y[a])*1000.0f/dt, lat = vx*side.x + vy*side.y, sm = (lat + prevlat)*0.5f;
            prevlat = lat;
            int sign = sm > 25 ? 1 : (sm < -25 ? -1 : 0);
            if(sign)
            {
                if(lastsign && sign != lastsign) changes++;
                lastsign = sign;
            }
            if(h.t[newest] - h.t[a] <= 500) path += sqrtf((h.x[b] - h.x[a])*(h.x[b] - h.x[a]) + (h.y[b] - h.y[a])*(h.y[b] - h.y[a]));
        }
        int oldest = newest, recent = newest;
        for(int j = 0; j < n && h.t[newest] - h.t[(newest - j + SPATH)%SPATH] <= 500; j++) oldest = (newest - j + SPATH)%SPATH;
        for(int j = 0; j < n && h.t[newest] - h.t[(newest - j + SPATH)%SPATH] <= 100; j++) recent = (newest - j + SPATH)%SPATH;
        net = sqrtf((h.x[newest] - h.x[oldest])*(h.x[newest] - h.x[oldest]) + (h.y[newest] - h.y[oldest])*(h.y[newest] - h.y[oldest]));
        if(recent != newest)
        { // speed: how far he went during the last 100 ms
            float rx = h.x[newest] - h.x[recent], ry = h.y[newest] - h.y[recent];
            speed = sqrtf(rx*rx + ry*ry)*1000.0f/max(h.t[newest] - h.t[recent], 1);
        }
        float linear = path > 5 ? min(net/path, 1.0f) : 1.0f;
        return expf(SUINV*min(changes, 4) + SULIN*(1 - linear) - (speed < 30 ? SUSTOP : 0));
    }

    static void stylenewaim(aistyle &s, bool flickok) // a new point to aim at, around the body (scaled by s.spread)
    {
        s.oy = SOFF*rndgauss();
        s.op = SOFF*SOFFV*rndgauss();
        s.flick = flickok && rndscale(1) < SFLICK;
        s.clic = lognormal(SCLIC, SCLICSD);
    }

    // how often a gesture of a degrees towards an enemy may end in a shot before the hand settles, and how long the hand
    // takes to settle otherwise (median ms, before level and form), as measured on the reference player's first shots
    static float styleflickp(float a) { return a < 30 || a >= 60 ? 1.0f : 0.5f; }
    static float stylesettle(float a) { return a < 30 ? SSETTLE15 : (a < 60 ? SSETTLE30 : SSETTLE60); }

    // a gesture towards the aimed point (target + offset), partly ahead of where the target goes
    static void styleplan(fpsent *d, aistyle &s, float ty, float tp, float vy, float vp, float half)
    {
        float err = styleerr(d, s);
        ty += s.oy*s.spread*err*half; tp += s.op*s.spread*err*half;
        float dy0 = stylewrap(ty - d->yaw), dp0 = tp - d->pitch, a0 = sqrtf(dy0*dy0 + dp0*dp0),
              dur = max(30.0f, SDUR0*powf(max(a0, 1.0f), SDURK)*expf(SDURSD*rndgauss())*stylespeed(d, s)),
              dy = dy0 + vy*SLEAD*dur/1000.0f, dp = dp0 + vp*SLEAD*dur/1000.0f, a = sqrtf(dy*dy + dp*dp);
        float gsd = SGAIN15*powf(max(a, 2.0f)/15.0f, SGAINK)*err, g = SGAIN + gsd*rndgauss(),
              side = SPERP*err*rndgauss()*min(1.0f, a/10.0f), ux = a > 1e-4f ? dy/a : 1, uy = a > 1e-4f ? dp/a : 0;
        s.gy0 = d->yaw;
        s.gp0 = d->pitch;
        s.gdy = g*dy - side*uy;
        s.gdp = g*dp + side*ux;
        s.gdur = dur;
        s.gt = 0;
        s.phase = STYLE_GESTURE;
        s.gamp = sqrtf(s.gdy*s.gdy + s.gdp*s.gdp);
        s.gtarget = half > 0;
        s.gflick = !s.gtarget || rndscale(1) < styleflickp(s.gamp);
        s.gshot = false;
    }

    // one frame of the hand: gesture, wait for a correction, or tracking (late by SLAG ms, with a little tremor)
    static void stylemove(fpsent *d, aistyle &s, float ty, float tp, float half, bool target, int dt)
    {
        float lagy, lagp, vy, vp, nowy, nowp, nvy, nvp;
        stylepast(s, int(SLAG*styleslow(d)), d->yaw, lagy, lagp, vy, vp);
        stylepast(s, 0, d->yaw, nowy, nowp, nvy, nvp);
        float err = styleerr(d, s), offy = target ? s.oy*s.spread*err*half : 0, offp = target ? s.op*s.spread*err*half : 0;
        if(s.phase == STYLE_NONE) styleplan(d, s, ty, tp, nvy, nvp, target ? half : 0);
        if(s.phase == STYLE_GESTURE)
        {
            s.gt += dt;
            float f = styleprofile(s.gt/s.gdur);
            d->yaw = s.gy0 + s.gdy*f;
            d->pitch = s.gp0 + s.gdp*f;
            if(s.gt >= s.gdur)
            {
                s.ncorr++;
                s.phase = STYLE_WAIT;
                s.wait = lognormal(SCORR, SCORRSD)*styleslow(d);
                s.gend = lastmillis;
                // a big flick that did not end in a shot: the hand settles before the trigger is pulled
                if(s.gtarget && !s.gshot && s.gamp >= 15) s.settleuntil = max(s.settleuntil, lastmillis + int(lognormal(stylesettle(s.gamp), SSETTLESD)*styleslow(d)));
            }
        }
        else
        {
            float follow = s.phase == STYLE_TRACK ? 1.0f : 0.5f, k = 1 - powf(1 - STRACK, dt/5.0f),
                  shake = target ? SJITTER*0.05f*sqrtf(dt/5.0f)*err : 0.0f; // the hand trembles when aiming at someone
            float ahead = SPRED*SLAG*styleslow(d)/1000.0f;
            d->yaw += follow*vy*dt/1000.0f + k*stylewrap(lagy + vy*ahead + offy - d->yaw) + shake*rndgauss();
            d->pitch += follow*vp*dt/1000.0f + k*(lagp + vp*ahead + offp - d->pitch) + shake*rndgauss();
            if(s.phase == STYLE_WAIT && (s.wait -= dt) <= 0)
            {
                float ey = stylewrap(ty + offy - d->yaw), ep = tp + offp - d->pitch;
                if(sqrtf(ey*ey + ep*ep) > 2*max(half, 0.5f) && s.ncorr < 4) styleplan(d, s, ty, tp, nvy, nvp, target ? half : 0);
                else s.phase = STYLE_TRACK;
            }
            else if(s.phase == STYLE_TRACK && target && lastmillis - s.gend >= SCATCHMS*styleslow(d))
            { // the target got away from the crosshair (it circles, dodges): a new flick rather than a slow sweep
                float ey = stylewrap(ty + offy - d->yaw), ep = tp + offp - d->pitch;
                if(sqrtf(ey*ey + ep*ep) > max(SCATCH*half, SCATCHDEG))
                {
                    s.ncorr = 0;
                    styleplan(d, s, ty, tp, nvy, nvp, half);
                }
            }
        }
        d->pitch = clamp(d->pitch, -89.9f, 89.9f);
        while(d->yaw < 0.0f) d->yaw += 360.0f;
        while(d->yaw >= 360.0f) d->yaw -= 360.0f;
    }

    static void styledose(fpsent *d, int except, float amount) // other events take attention from the others
    {
        aiperception *s = perception(d);
        if(s) loopv(s->seen) if(s->seen[i].cn != except && s->seen[i].aware) s->seen[i].dose -= amount;
    }

    // something new about this enemy (kind 0 seen, 1 heard, 2 hit by him): full attention on him, a little less on
    // the others ("a fixed dose that goes down with the number of events", in the reference player's words)
    static void styleevent(fpsent *d, aiperceived &p, int kind)
    {
        p.dose = 1;
        p.dosemillis = lastmillis;
        styledose(d, p.cn, kind == 1 ? 0.05f : 0.15f);
    }

    static bool styleforget(fpsent *d, aiperceived &p) // the dose also wears out slowly with time
    {
        if(p.dosemillis) p.dose -= 0.03f*(lastmillis - p.dosemillis)/1000.0f;
        p.dosemillis = lastmillis;
        return p.dose <= 0;
    }

    static bool stylethreat(fpsent *d, fpsent *e) // e is aiming at the bot (a player sees where the others look)
    {
        vec to = vec(d->o).sub(e->o), f;
        float dist = to.magnitude();
        if(dist < 1) return true;
        vecfromyawpitch(e->yaw, e->pitch, 1, 0, f);
        return f.dot(to)/dist > cosf(15*RAD);
    }

    static int styledodgeplan(aistyle &s);

    // a hold: in a duel, when the enemy went out of sight and the bot guesses where he will come out, it sometimes
    // stops and keeps that line in its sights a moment (measured in duels: still for at least 0.7 s during 5 to 8 % of
    // the time against opponents of the same level, up to 18 % on a bad day; the aim was then often within 10 degrees
    // of where he came out); the stop is short (the player's stops in sight: 560 ms in median, 90 % under 1.2 s) and ends a little
    // before the moment it expects him to shoot, with a step aside
    static bool stylehold(fpsent *d, aistyle &s, aiperceived &p, const vec &dp, const vec &guess)
    {
        if(m_teammode || aliveenemies(d) != 1) return false;
        if(lastmillis < s.holduntil) return true; // a hold goes on even if a sound moves the guess a little
        if(s.mvmode == STYLE_MV_HOLD && s.steppause) return false; // already standing still: its step aside comes first
        if((d->move || d->strafe) && s.escat && lastmillis - s.escat >= 300)
        { // stuck against something (keys held, going nowhere): it gets out of there first, no line held now
            vec feet = d->feetpos();
            float mx = feet.x - s.escpos[0], my = feet.y - s.escpos[1];
            if(mx*mx + my*my < 4*4) return false;
        }
        if((p.curexit < 0 && p.numexits <= 0 && lastmillis - s.seenmillis >= 1000) || lastmillis < s.holdcool || guess.squaredist(dp) > 700*700 || !clearview(dp, guess)) return false;
        if(rnd(100) < 60)
        {
            s.holdseen = rnd(2) != 0;
            int len = int(clamp(lognormal(SHOLDMS, SHOLDSD), 200.0f, float(SHOLDMAX))), plan = styledodgeplan(s);
            if(plan > lastmillis + 150 && plan < lastmillis + len) len = plan - lastmillis;
            s.holduntil = lastmillis + len;
            s.holdcool = s.holduntil + 1500 + rnd(1500);
            return true;
        }
        s.holdcool = lastmillis + 1000;
        return false;
    }

    static void styleplaceexits(fpsent *d, aistyle &s, const vec &dp);
    static void stylecoverof(const vec &dp, float exits[][3], int n, float &center, float &spread);

    // where it looks while it waits for a lost enemy: his ways out one after the other (measured: 675 ms on one in
    // median, from 230 ms to 2.6 s), the next one nearest in angle, now and then one behind it rather than a single
    // bet; two ways out close to each other are both kept in sight. He may have gone round: more and more as time goes
    // by, also the ways into its own place, one by one (the one it has not looked at for the longest time, behind it
    // too), or the middle of all the ways when they hold in its view
    static vec styleexitsweep(fpsent *d, aistyle &s, aiperceived &p, const vec &dp, fpsent *e)
    {
        if(s.exitstamp != p.exitstamp || s.exitlook >= p.numexits) { s.exitlook = -1; s.exitstamp = p.exitstamp; }
        float fx = dp.x - s.placeexitsfrom[0], fy = dp.y - s.placeexitsfrom[1];
        if(!s.placeexitsat || lastmillis - s.placeexitsat > 2000 || fx*fx + fy*fy > 96*96) styleplaceexits(d, s, dp);
        vec f;
        vecfromyawpitch(d->yaw, 0, 1, 0, f);
        if(s.exitlook == -1 || s.exitlook == -4 || lastmillis >= s.exituntil)
        {
            int cur = s.exitlook, best = -1, worst = -1;
            s.exituntil = lastmillis + int(clamp(lognormal(SEXITDWELL, SEXITDWELLSD), 200.0f, 2600.0f));
            float all[16][3], center = 0, spread = 360;
            int n = 0;
            loopi(s.nplaceexits) if(n < 16) { loopk(3) all[n][k] = s.placeexits[i][k]; n++; }
            loopi(p.numexits) if(n < 16)
            {
                const vec &o = waypoints[p.exitnode[i]].o;
                all[n][0] = o.x; all[n][1] = o.y; all[n][2] = o.z + e->eyeheight*0.7f;
                n++;
            }
            stylecoverof(dp, all, n, center, spread);
            if(n > 1 && spread <= 100 && cur != -3 && rndscale(1) < 0.5f)
            { // they all hold in its view (100 degrees): the middle of them, a little to one side or the other
                vec dir;
                vecfromyawpitch(center + (rndscale(1) - 0.5f)*(100 - spread), 0, 1, 0, dir);
                vec at = vec(dir).mul(300).add(dp);
                s.exitlook = -3;
                s.exitat[0] = at.x; s.exitat[1] = at.y; s.exitat[2] = at.z;
                return at;
            }
            float lost = lastmillis - p.knownmillis,
                  own = lost < SEXITOWNAFTER ? 0.0f : clamp(SEXITOWN + (lost - SEXITOWNAFTER)/3000.0f*(SEXITOWNMAX - SEXITOWN), SEXITOWN, SEXITOWNMAX);
            if(s.nplaceexits > 0 && rndscale(1) < own)
            { // a way into its own place: the one it has not looked at for the longest time, now and then another one
                int k = 0;
                loopj(s.nplaceexits) if(s.placeexitlook[j] < s.placeexitlook[k]) k = j;
                if(s.nplaceexits > 1 && rnd(4) == 0) k = rnd(s.nplaceexits);
                s.placeexitlook[k] = lastmillis;
                s.exitlook = -2;
                loopj(3) s.exitat[j] = s.placeexits[k][j];
                return vec(s.exitat[0], s.exitat[1], s.exitat[2]);
            }
            float total = 0, w[AI_P_EXITS] = { 0, 0, 0, 0, 0, 0, 0, 0 }, worstang = -1;
            vec v(p.knownvel.x, p.knownvel.y, 0);
            float speed = v.magnitude();
            loopi(p.numexits)
            { // how likely he comes out there: on his way, not far (as the human perception guesses), the one it just
              // looked at less
                w[i] = 0;
                vec to = vec(waypoints[p.exitnode[i]].o).sub(dp), from = vec(waypoints[p.exitnode[i]].o).sub(p.knownpos);
                to.z = from.z = 0;
                float l = to.magnitude(), fl = from.magnitude();
                if(l < 1) continue;
                float ang = acosf(clamp(f.dot(to)/l, -1.0f, 1.0f));
                if(ang > worstang && i != cur) { worstang = ang; worst = i; }
                float dirw = speed > 20 && fl > 1 ? 0.25f + 0.75f*max(v.dot(from)/(speed*fl), 0.0f) : 1.0f;
                w[i] = dirw/(1 + p.exitlen[i]/256)*(i == cur && p.numexits > 1 ? 0.2f : 1.0f);
                total += w[i];
            }
            float r = rndscale(total);
            loopi(p.numexits) if(w[i] > 0) { best = i; if((r -= w[i]) <= 0) break; }
            s.exitlook = worst >= 0 && worst != best && rndscale(1) < SEXITBACK ? worst : best;
            if(s.exitlook < 0) s.exitlook = 0;
        }
        if(s.exitlook < 0) return vec(s.exitat[0], s.exitat[1], s.exitat[2]);
        vec at = vec(waypoints[p.exitnode[s.exitlook]].o).add(vec(0, 0, e->eyeheight*0.7f));
        vec ta = vec(at).sub(dp);
        ta.z = 0;
        float tl = ta.magnitude();
        if(tl > 1) loopi(p.numexits) if(i != s.exitlook)
        { // another way out within 40 degrees of it: aim between the two
            vec o = vec(waypoints[p.exitnode[i]].o).add(vec(0, 0, e->eyeheight*0.7f)), to = vec(o).sub(dp);
            to.z = 0;
            float l = to.magnitude();
            if(l > 1 && ta.dot(to)/(tl*l) > cosf(40*RAD)) { at.add(o).mul(0.5f); break; }
        }
        return at;
    }

    // no way out of a lost enemy is known (he was close, everything around is in sight): rather than staring at the spot
    // where it last knew him, more and more as time goes by (as when it watches his ways out) a way into its own place too,
    // one by one, the one it has not looked at for the longest time (he may have gone round); -4: where it last knew him
    static vec styleownways(fpsent *d, aistyle &s, aiperceived &p, const vec &dp, const vec &guess)
    {
        float fx = dp.x - s.placeexitsfrom[0], fy = dp.y - s.placeexitsfrom[1];
        if(!s.placeexitsat || lastmillis - s.placeexitsat > 2000 || fx*fx + fy*fy > 96*96) styleplaceexits(d, s, dp);
        if(s.nplaceexits <= 0) return guess;
        if(lastmillis >= s.exituntil || (s.exitlook != -2 && s.exitlook != -4))
        {
            s.exituntil = lastmillis + int(clamp(lognormal(SEXITDWELL, SEXITDWELLSD), 200.0f, 2600.0f));
            float lost = lastmillis - p.knownmillis,
                  own = clamp(SEXITOWN + (lost - SEXITOWNAFTER)/3000.0f*(SEXITOWNMAX - SEXITOWN), SEXITOWN, SEXITOWNMAX);
            if(rndscale(1) < own)
            {
                int k = 0;
                loopj(s.nplaceexits) if(s.placeexitlook[j] < s.placeexitlook[k]) k = j;
                if(s.nplaceexits > 1 && rnd(4) == 0) k = rnd(s.nplaceexits);
                s.placeexitlook[k] = lastmillis;
                s.exitlook = -2;
                loopj(3) s.exitat[j] = s.placeexits[k][j];
            }
            else s.exitlook = -4;
        }
        return s.exitlook == -2 ? vec(s.exitat[0], s.exitat[1], s.exitat[2]) : guess;
    }

    // a lost enemy: his nearest way out (the first waypoint in sight from the bot on the paths that leave the spot where it
    // lost him, nearest first, as human perception finds them); false when none (at most 96 waypoints looked at)
    static bool stylepreaimpoints(fpsent *d, aistyle &s, const vec &dp, const vec &base, fpsent *e)
    {
        s.prenum = 0;
        int start = closestwaypoint(vec(base).sub(vec(0, 0, e->eyeheight)), SIGHTMIN, true);
        if(!iswaypoint(start)) return false;
        static int node[PEXITNODES];
        static float len[PEXITNODES];
        static bool done[PEXITNODES];
        int num = 1;
        node[0] = start;
        len[0] = 0;
        done[0] = false;
        while(!s.prenum)
        {
            int cur = -1;
            loopi(num) if(!done[i] && (cur < 0 || len[i] < len[cur])) cur = i;
            if(cur < 0) break;
            done[cur] = true;
            const waypoint &w = waypoints[node[cur]];
            if(cur && clearview(dp, vec(w.o).add(vec(0, 0, e->eyeheight*0.7f))))
            {
                s.preexits[0][0] = w.o.x; s.preexits[0][1] = w.o.y; s.preexits[0][2] = w.o.z + e->eyeheight;
                s.prenum = 1;
                break;
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
        return s.prenum > 0;
    }

    // what it watches for a lost enemy: his line (where he went out, a little ahead along his way, then the spot itself),
    // the view resting on it; now and then, once he has been gone SPRESWITCH ms, a short glance at his nearest way out or a
    // way into its own place, then back to his line (prepick: 0 his line, 1 a way out, 4 a way into its place); false: no
    // guess (the base game's guess is used)
    static bool stylepreaim(fpsent *d, aistyle &s, aiperceived &p, const vec &dp, fpsent *e, vec &at)
    {
        // where from: where it lost him from sight (a sound heard since does not move that: it only tells roughly where he is,
        // often tens of units off); when it has not seen him in this life, where it knows him to be (a sound, its killer)
        bool seen = s.seenmillis && s.aimcn == e->clientnum && s.aimlife == e->lifesequence;
        vec base = seen ? vec(s.seenpos[0], s.seenpos[1], s.seenpos[2]) : p.knownpos;
        int lost = lastmillis - (seen ? s.seenmillis : p.knownmillis);
        // where its killer was (just respawned): that spot itself first, as before (he is often still there)
        if(!seen && lost < SPREKNOWN && p.knownsrc == AI_P_HIT) { s.prepick = -1; return false; }
        int stamp = e->clientnum*16 + (e->lifesequence&15) + 1;
        vec oldbase(s.prebase[0], s.prebase[1], s.prebase[2]);
        bool newbase = s.prestamp != stamp || base.squaredist(oldbase) > 16*16;
        if(newbase)
        {
            s.prestamp = stamp;
            s.prebase[0] = base.x; s.prebase[1] = base.y; s.prebase[2] = base.z;
            s.preuntil = 0;
            s.prepick = -1;
        }
        vec line = base;
        if(seen && lost < SPREPV)
        { // his line, carried on along his way (300 ms of his speed at most)
            float lead = min(lost, int(SLINELEAD))/1000.0f;
            line.add(vec(s.seenvel[0], s.seenvel[1], 0).mul(lead));
        }
        if(lastmillis >= s.preuntil || s.prepick < 0)
        { // the next look: his line, or now and then (he has been gone a while) a glance elsewhere
            s.prepick = 0;
            s.preuntil = lastmillis + int(clamp(lognormal(SPREDWELL, SPREDWELLSD), 500.0f, 4000.0f));
            if(lost >= SPRESWITCH && rndscale(1) < SPREGLANCE)
            {
                s.preuntil = lastmillis + int(clamp(lognormal(SPREGLANCEMS, 0.4f), 250.0f, 1500.0f));
                if(rnd(2))
                { // a way into its own place: the one it has not looked at for the longest time
                    float gx = dp.x - s.placeexitsfrom[0], gy = dp.y - s.placeexitsfrom[1];
                    if(!s.placeexitsat || lastmillis - s.placeexitsat > 2000 || gx*gx + gy*gy > 96*96) styleplaceexits(d, s, dp);
                    if(s.nplaceexits > 0)
                    {
                        int k = 0;
                        loopj(s.nplaceexits) if(s.placeexitlook[j] < s.placeexitlook[k]) k = j;
                        s.placeexitlook[k] = lastmillis;
                        loopj(3) s.prepoint[j] = s.placeexits[k][j];
                        s.prepick = 4;
                    }
                }
                else if(stylepreaimpoints(d, s, dp, base, e) && s.prenum > 0)
                { // his nearest way out
                    loopj(3) s.prepoint[j] = s.preexits[0][j];
                    s.prepick = 1;
                }
                if(!s.prepick) s.preuntil = lastmillis + int(clamp(lognormal(SPREDWELL, SPREDWELLSD), 500.0f, 4000.0f));
            }
        }
        at = s.prepick ? vec(s.prepoint[0], s.prepoint[1], s.prepoint[2]) : line;
        return true;
    }

    // moves under fire: the bot holds its place (the spot it stood on when it took up the fight) and dodges there with
    // steps of uneven lengths and pauses (keys, as a player does), changing side three times out of four, sometimes
    // forwards or backwards too, rather than running at the enemy, so it keeps the distance it had; a step back when he
    // is too close. A way that is shut (a wall, an edge, something deadly, the end of its place, or a way it did not get
    // through) is left for another one; a place it cannot step from any more is given up. The view stays where the
    // aim puts it (on the enemy, or on the line he should come out of).
    static bool stylestepok(fpsent *d, const vec &dir) // dir: unit, flat; no wall there, firm ground, nothing deadly
    {
        vec feet = d->feetpos();
        if(raycube(vec(feet).add(vec(0, 0, 5)), dir, 24) < 24 || raycube(vec(feet).add(vec(0, 0, 11)), dir, 24) < 24) return false;
        vec ahead = vec(dir).mul(24).add(feet).add(vec(0, 0, 4));
        float drop = raycube(ahead, vec(0, 0, -1), 64);
        if(drop > 22) return false;
        int mat = lookupmaterial(vec(ahead.x, ahead.y, ahead.z - drop + 1));
        return !isdeadly(mat&MATF_VOLUME) && !(mat&MAT_DEATH);
    }

    static inline int stylestepdir(int move, int strafe) { return (move + 1)*3 + strafe + 1; } // 0..8, 4 = still

    // out of a spot where no safe step is left (a ledge, a tight corner): of the 8 ways, the one with the longest clear way
    // at foot level, even if it drops down, never towards something deadly below; held a few hundred ms
    static void styleescape(fpsent *d, aistyle &s)
    {
        const int ways[8][2] = { { -1, 0 }, { 0, 1 }, { 0, -1 }, { -1, 1 }, { -1, -1 }, { 1, 1 }, { 1, -1 }, { 1, 0 } };
        vec feet = d->feetpos();
        int best = -1;
        float bestlen = 6;
        loopk(8)
        {
            vec dir;
            vecfromyawpitch(d->yaw, 0, ways[k][0], ways[k][1], dir);
            dir.normalize();
            float len = min(raycube(vec(feet).add(vec(0, 0, 5)), dir, 32), raycube(vec(feet).add(vec(0, 0, 11)), dir, 32));
            if(len <= bestlen) continue;
            vec ahead = vec(dir).mul(min(len, 24.0f)).add(feet).add(vec(0, 0, 4));
            float drop = raycube(ahead, vec(0, 0, -1), 512);
            int mat = lookupmaterial(vec(ahead.x, ahead.y, ahead.z - drop + 1));
            if(isdeadly(mat&MATF_VOLUME) || mat&MAT_DEATH) continue;
            bestlen = len;
            best = k;
        }
        if(best < 0) return;
        s.escmove = ways[best][0]; s.escstrafe = ways[best][1];
        s.escuntil = lastmillis + 400 + rnd(300);
    }

    static bool stylestep(fpsent *d, aistyle &s, int move, int strafe) // can it step that way and stay in its place?
    {
        if(!move && !strafe) return false;
        if(lastmillis < s.blockuntil && s.blockdir&(1<<stylestepdir(move, strafe))) return false; // it did not get through there
        vec dir;
        vecfromyawpitch(d->yaw, 0, move, strafe, dir);
        dir.normalize();
        vec next = vec(dir).mul(16).add(d->feetpos());
        float dx = next.x - s.dodgeat[0], dy = next.y - s.dodgeat[1];
        return dx*dx + dy*dy <= SDODGER*SDODGER && stylestepok(d, dir);
    }

    static void styleholdhere(fpsent *d, aistyle &s, int mode = STYLE_MV_HOLD) // it takes up its place where it stands
    {
        vec feet = d->feetpos();
        s.dodgeat[0] = feet.x; s.dodgeat[1] = feet.y; s.dodgeat[2] = feet.z;
        s.mvmode = mode;
        s.dodgenext = lastmillis;
        s.steppause = 1;
        s.dodgeside = rnd(2) ? 1 : -1;
        s.resetmoves();
        s.stepmove = s.stepstrafe = 0;
    }

    // the next step: the other way three times out of four, now and then forwards or backwards too (backwards when the
    // enemy is close); when that way is shut, the nearest one that is open; false when none is
    static bool stylepickstep(fpsent *d, aistyle &s, fpsent *e, bool newleg, bool guard, bool frompause = false)
    {
        int side = s.dodgeside ? s.dodgeside : 1, fb = 0;
        if(newleg)
        {
            if(rndscale(1) < SSIDECHANGE) side = -side;
            if(guard) { fb = rnd(3) - 1; if(!fb && rnd(3) == 0) side = 0; } // nobody in sight: it wanders about its spot
            else if(!frompause && rndscale(1) < SDIAG) // after a stop, a step aside
            {
                float dist = e ? e->o.dist(d->o) : 1e9f;
                fb = dist < SKEEPMIN + 60 ? -1 : (rnd(3) ? -1 : 1);
            }
        }
        else { side = s.stepstrafe; fb = s.stepmove; if(!side && !fb) side = s.dodgeside; }
        const int ways[8][2] = { { fb, side }, { 0, side }, { 0, -side }, { fb, -side }, { -1, side }, { -1, -side }, { 1, 0 }, { -1, 0 } };
        loopi(8)
        {
            int m = ways[i][0], st = ways[i][1];
            if((!m && !st) || (i && m == ways[0][0] && st == ways[0][1])) continue;
            if(stylestep(d, s, m, st))
            {
                s.stepmove = m;
                s.stepstrafe = st;
                if(st) s.dodgeside = st;
                return true;
            }
        }
        return false;
    }

    // when it expects the enemy to shoot: his weapon ready again (it saw or heard his last shot), or a moment after he
    // turned to it; a still bot steps aside just before
    static void styleexpect(fpsent *d, aistyle &s, fpsent *e, bool insight, bool shotknown)
    {
        if(shotknown && e->lastfiredmillis && e->lastfiredmillis != s.seenshot && lastmillis - e->lastfiredmillis < 1000)
        {
            s.seenshot = e->lastfiredmillis;
            int gun = e->gunselect >= 0 && e->gunselect < NUMGUNS ? e->gunselect : GUN_RIFLE;
            s.reloadplan = e->lastfiredmillis + guns[gun].attackdelay - int(clamp(lognormal(SDODGELEAD, SDODGELEADSD), 40.0f, 900.0f));
            s.stopforshot = insight && rndscale(1) < SSTOPSHOT ? 1 : 0;
        }
        bool threat = insight && stylethreat(d, e);
        if(threat && !s.threatat)
        {
            s.threatat = lastmillis;
            s.threatplan = lastmillis + int(clamp(lognormal(SDODGESEEN, SDODGESEENSD), 100.0f, 1500.0f));
        }
        else if(!threat) s.threatat = 0;
    }

    static int styledodgeplan(aistyle &s) // the next moment it expects a shot (0: none)
    {
        int plan = 0;
        if(s.reloadplan > lastmillis - 50) plan = s.reloadplan;
        if(s.threatat && s.threatplan > lastmillis - 50 && (!plan || s.threatplan < plan)) plan = s.threatplan;
        return plan;
    }

    // a style bot with nothing to do: a defender at its spot, or a bot holding a place while it searches; it moves
    // around that place rather than standing still
    static bool styleidle(fpsent *d, aistate &b)
    {
        return b.type == AI_S_DEFEND && b.idle == 1;
    }

    // the waypoint to walk to from a spot the waypoint graph does not reach: the nearest one that leads somewhere, at least
    // 24 u away (one right above or below the bot does not take it off the isolated waypoint), not one it could not reach
    static int stylewalktarget(fpsent *d, aistyle &s)
    {
        vec feet = d->feetpos();
        int best = -1;
        float bestd = 256*256;
        for(int i = 1; i < waypoints.length(); i++)
        {
            const waypoint &w = waypoints[i];
            if(!w.links[0]) continue;
            bool tried = false;
            loopj(s.nwalktried) if(s.walktried[j] == i) tried = true;
            if(tried) continue;
            float dx = w.o.x - feet.x, dy = w.o.y - feet.y, d2 = w.o.squaredist(feet);
            if(dx*dx + dy*dy < 24*24 || d2 >= bestd) continue;
            bestd = d2;
            best = i;
        }
        return best;
    }

    // called by process() after the moves of the base game, for a style bot holding a place (under fire, or with
    // nobody in sight), or walking to the waypoints from a spot the waypoints do not reach
    AI_HUMAN void stylesteps(fpsent *d)
    {
        if(!stylebot(d)) return;
        aistyle *ps = style(d);
        if(!ps || ps->owner != d->ai || ps->life != d->lifesequence) return;
        aistyle &s = *ps;
        aistate &b = d->ai->getstate();
        if(s.covermode)
        { // the strong position "cover and jump": it walks to its cover, then stays there and jumps now and then to look over
            vec feet = d->feetpos(), to = vec(s.coverat[0], s.coverat[1], s.coverat[2]).sub(feet);
            to.z = 0;
            float dist = to.magnitude();
            d->jumping = false;
            if(s.covermode == 1)
            {
                if(dist < 6)
                {
                    s.covermode = 2;
                    s.covernext = lastmillis + int(clamp(lognormal(SCOVERFIRST, 0.4f), 150.0f, 1500.0f));
                    s.covermoved = lastmillis;
                    s.coverpos[0] = feet.x; s.coverpos[1] = feet.y;
                }
                else if(lastmillis - s.coverstart > 2000) { s.covermode = 0; return; } // it did not get there
            }
            else if(dist > 24) { s.covermode = 0; return; } // pushed away from it
            else
            { // it must not stand still there for long (a spot it cannot shuffle in: it leaves it)
                float mx = feet.x - s.coverpos[0], my = feet.y - s.coverpos[1];
                if(mx*mx + my*my > 4*4) { s.covermoved = lastmillis; s.coverpos[0] = feet.x; s.coverpos[1] = feet.y; }
                else if(lastmillis - s.covermoved > 900) { s.covermode = 0; return; }
            }
            d->move = d->strafe = 0;
            if(s.covermode == 2 && dist < 16)
            { // behind its cover it shuffles along it rather than standing still (measured: 57 u/s in median during the player's holds,
              // never still more than about half a second): short steps aside and short pauses, back towards the spot
                if(lastmillis >= s.coverstep)
                {
                    if(s.coverdir) { s.coverdir = 0; s.coverstep = lastmillis + int(clamp(lognormal(SCOVERPAUSE, 0.5f), 60.0f, 600.0f)); }
                    else
                    {
                        vec right;
                        vecfromyawpitch(d->yaw, 0, 0, -1, right);
                        s.coverdir = dist > 8 ? (right.dot(to) > 0 ? -1 : 1) : (rnd(2) ? 1 : -1);
                        s.coverstep = lastmillis + int(clamp(lognormal(SCOVERSTEP, 0.5f), 80.0f, 500.0f));
                    }
                }
                if(s.coverdir)
                {
                    vec dir;
                    vecfromyawpitch(d->yaw, 0, 0, s.coverdir, dir);
                    dir.normalize();
                    // in the air only walls matter (it lands back where it took off)
                    bool open = d->physstate == PHYS_FALL ? raycube(vec(feet).add(vec(0, 0, 8)), dir, 16) >= 16 : stylestepok(d, dir);
                    if(open) d->strafe = s.coverdir;
                    else s.coverdir = -s.coverdir;
                }
            }
            else if(dist >= (s.covermode == 1 ? 1.0f : 5.0f))
            { // a few steps to the spot (keys relative to where it looks)
                float yaw = -atan2f(to.x, to.y)/RAD - d->yaw;
                while(yaw < 0) yaw += 360;
                while(yaw >= 360) yaw -= 360;
                int r = int(floorf((yaw + 22.5f)/45.0f))&7;
                d->move = stylekeys[r][0];
                d->strafe = stylekeys[r][1];
            }
            if(s.covermode == 2 && lastmillis >= s.covernext && d->physstate >= PHYS_SLOPE && !d->timeinair)
            { // up to look over the cover
                d->jumping = true;
                s.covernext = lastmillis + int(clamp(lognormal(SCOVERJUMP, SCOVERJUMPSD), 700.0f, 3000.0f));
            }
            return;
        }
        if(!iswaypoint(s.walkto) && d->ai->dontmove && d->ai->route.empty() && lastmillis >= s.holduntil &&
           (!iswaypoint(d->lastnode) || !waypoints[d->lastnode].haslinks()))
        { // off the waypoint graph wherever it is (whatever the base game tried): the nearest waypoint that leads somewhere
            s.nwalktried = 0;
            int w = stylewalktarget(d, s);
            if(iswaypoint(w) && w != d->lastnode) { s.walkto = w; s.walkstart = lastmillis; s.walkcheck = 0; }
        }
        if(iswaypoint(s.walkto))
        { // off the waypoints (a spawn point no way leads from): it walks to the nearest one that leads somewhere
            vec to = vec(waypoints[s.walkto].o).sub(d->feetpos());
            if((iswaypoint(d->lastnode) && waypoints[d->lastnode].haslinks()) || lastmillis - s.walkstart > 4000) { s.walkto = -1; return; }
            vec feet = d->feetpos();
            bool blocked = false;
            if(!s.walkcheck || lastmillis - s.walkcheck >= 500)
            { // no headway towards it in half a second (a ledge, a wall): it gives that one up
                float px = feet.x - s.walkpos[0], py = feet.y - s.walkpos[1];
                blocked = s.walkcheck && px*px + py*py < 4*4;
                s.walkcheck = lastmillis;
                s.walkpos[0] = feet.x; s.walkpos[1] = feet.y;
            }
            if(blocked || to.x*to.x + to.y*to.y < 8*8)
            { // could not get there, or under it but still on the isolated waypoint (it is higher up): the next one, from
              // where it stands now
                if(s.nwalktried < 4) s.walktried[s.nwalktried++] = s.walkto;
                int w = stylewalktarget(d, s);
                if(!iswaypoint(w) || w == s.walkto) { s.walkto = -1; return; }
                s.walkto = w;
                to = vec(waypoints[w].o).sub(feet);
            }
            float yaw = -atan2f(to.x, to.y)/RAD - d->yaw;
            while(yaw < 0) yaw += 360;
            while(yaw >= 360) yaw -= 360;
            const int aim[8][2] = { { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
            int r = int(floorf((yaw + 22.5f)/45.0f))&7;
            d->move = aim[r][0];
            d->strafe = aim[r][1];
            return;
        }
        if((s.mvmode == STYLE_MV_NONE || s.mvmode == STYLE_MV_CHASE) && !d->ai->dontmove && (d->move || d->strafe) && lastmillis >= s.escuntil)
        { // walking somewhere: the keys it holds are kept while its way stays within SKEYHYST degrees more than half a key
          // direction of them (a player does not switch keys at each bend), unless that way is shut
            static const int keys[8][2] = { { 1, 0 }, { 1, -1 }, { 0, -1 }, { -1, -1 }, { -1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };
            float yaw = d->ai->targyaw - d->yaw;
            while(yaw < 0) yaw += 360;
            while(yaw >= 360) yaw -= 360;
            int r = int(floorf((yaw + 22.5f)/45.0f))&7;
            if(s.walkkey >= 0 && s.walkkey != r && fabs(stylewrap(yaw - s.walkkey*45.0f)) < 22.5f + SKEYHYST)
            {
                vec dir;
                vecfromyawpitch(d->yaw, 0, keys[s.walkkey][0], keys[s.walkkey][1], dir);
                dir.normalize();
                if(stylestepok(d, dir)) r = s.walkkey;
            }
            s.walkkey = r;
            d->move = keys[r][0];
            d->strafe = keys[r][1];
        }
        else if(s.mvmode != STYLE_MV_HOLD && s.mvmode != STYLE_MV_GUARD) s.walkkey = -1;
        if((s.mvmode == STYLE_MV_NONE || s.mvmode == STYLE_MV_CHASE) && styleidle(d, b) && d->gunselect != GUN_FIST) styleholdhere(d, s, STYLE_MV_GUARD);
        if(s.mvmode != STYLE_MV_HOLD && s.mvmode != STYLE_MV_GUARD)
        { // keys held but going nowhere (a corner the base game walks into, and it does not notice when it waits on
          // purpose): a few steps another way
            vec feet = d->feetpos();
            if(lastmillis < s.escuntil) { d->move = s.escmove; d->strafe = s.escstrafe; return; }
            if(!(d->move || d->strafe) || d->ai->dontmove) { s.escat = lastmillis; s.escpos[0] = feet.x; s.escpos[1] = feet.y; }
            else if(lastmillis - s.escat >= 600 || lastmillis < s.escat)
            {
                float mx = feet.x - s.escpos[0], my = feet.y - s.escpos[1];
                if(mx*mx + my*my < 12*12)
                {
                    const int ways[8][2] = { { -1, 0 }, { 0, 1 }, { 0, -1 }, { -1, 1 }, { -1, -1 }, { 1, 1 }, { 1, -1 }, { 1, 0 } };
                    int k0 = rnd(8);
                    loopk(8)
                    {
                        int m = ways[(k0 + k)%8][0], st = ways[(k0 + k)%8][1];
                        if(m == d->move && st == d->strafe) continue;
                        vec dir;
                        vecfromyawpitch(d->yaw, 0, m, st, dir);
                        dir.normalize();
                        if(!stylestepok(d, dir)) continue;
                        s.escmove = m; s.escstrafe = st;
                        s.escuntil = lastmillis + 300 + rnd(400);
                        d->move = m; d->strafe = st;
                        break;
                    }
                }
                s.escat = lastmillis; s.escpos[0] = feet.x; s.escpos[1] = feet.y;
            }
        }
        if(s.mvmode == STYLE_MV_CHASE && d->ai->dontmove && lastmillis >= s.holduntil && b.type == AI_S_PURSUE && b.targtype == AI_T_PLAYER)
        { // after him, but the base game finds no way: it holds its place, dodging, rather than standing still
            if(!s.nostep) s.nostep = lastmillis;
            else if(lastmillis - s.nostep > SNOSTEP)
            {
                fpsent *t = getclient(b.target);
                if(t && t->state == CS_ALIVE) { styleholdhere(d, s); s.mvcn = t->clientnum; s.mvlife = t->lifesequence; }
                s.nostep = 0;
            }
        }
        else if(s.mvmode == STYLE_MV_CHASE) s.nostep = 0;
        if(s.mvmode != STYLE_MV_HOLD && s.mvmode != STYLE_MV_GUARD) return;
        fpsent *e = NULL;
        if(s.mvmode == STYLE_MV_HOLD)
        {
            e = getclient(s.mvcn);
            bool chasing = b.type == AI_S_PURSUE && b.targtype == AI_T_PLAYER, defending = b.type == AI_S_DEFEND;
            if(!e || e->state != CS_ALIVE || e->lifesequence != s.mvlife || !(chasing || defending) || (defending && lastmillis - s.mvseen >= SKEEPLOST))
            { // the fight is over (he died, he is another life, it does something else): its place is given up
                s.mvmode = STYLE_MV_NONE;
                return;
            }
        }
        else if(!(styleidle(d, b) || s.srmode == STYLE_SR_HOLD)) { s.mvmode = STYLE_MV_NONE; return; }
        bool guard = s.mvmode == STYLE_MV_GUARD;
        if(d->ai->dontmove && lastmillis < s.holduntil)
        { // a line held still on purpose (bounded); a step aside comes right after
            s.stepmove = s.stepstrafe = 0;
            s.steppause = 1;
            s.dodgenext = s.holduntil;
            return;
        }
        vec feet = d->feetpos();
        float ax = feet.x - s.dodgeat[0], ay = feet.y - s.dodgeat[1];
        if(ax*ax + ay*ay > (SDODGER + 16)*(SDODGER + 16)) styleholdhere(d, s, s.mvmode); // pushed out of its place (fall, jump pad): a new place here
        int plan = guard ? 0 : styledodgeplan(s);
        bool turn = false, newleg = false;
        if(s.steppause && plan && lastmillis >= plan && lastmillis - plan < 400)
        { // standing still: a step aside just before he can shoot
            turn = newleg = true;
            s.reloadplan = s.threatplan = 0;
        }
        else if(lastmillis >= s.dodgenext) { turn = true; newleg = s.steppause != 0; }
        else if(lastmillis < s.stepat || lastmillis - s.stepat >= 40) turn = !s.steppause; // 25 times a second: is the way still open?
        if(turn)
        {
            s.stepat = lastmillis;
            if(!s.steppause && !newleg && lastmillis >= s.dodgenext)
            { // the end of a step: a pause, or the next step at once
                if(!guard && s.stopforshot && s.reloadplan > lastmillis + 150 && s.reloadplan < lastmillis + SSTOPMAX)
                { // he just shot: it stops to make its own shot sure, and steps aside just before he can shoot again
                    s.stopforshot = 0;
                    s.steppause = 1;
                    s.stepmove = s.stepstrafe = 0;
                    s.dodgenext = s.reloadplan;
                }
                else if(rndscale(1) < (guard ? 0.4f : SPAUSEP))
                {
                    s.steppause = 1;
                    s.stepmove = s.stepstrafe = 0;
                    s.dodgenext = lastmillis + int(clamp(lognormal(guard ? 2*SPAUSE : SPAUSE, SPAUSESD), 40.0f, float(SSTOPMAX)));
                }
                else newleg = true;
            }
            if(!s.steppause || newleg)
            {
                bool ok = stylepickstep(d, s, e, newleg || lastmillis >= s.dodgenext, guard, s.steppause != 0);
                if(newleg || lastmillis >= s.dodgenext)
                {
                    if(s.steppause) s.holdcool = max(s.holdcool, lastmillis + 800); // after a stop its step aside goes through: no line held at once
                    s.steppause = 0;
                    s.dodgenext = lastmillis + int(clamp(lognormal(guard ? 1.6f*SLEG : SLEG, SLEGSD), 60.0f, guard ? 1500.0f : 900.0f));
                    s.progat = lastmillis; s.progpos[0] = feet.x; s.progpos[1] = feet.y;
                }
                if(!ok)
                {
                    s.stepmove = s.stepstrafe = 0;
                    if(!s.nostep) s.nostep = lastmillis;
                    else if(lastmillis - s.nostep > SNOSTEP)
                    { // no way to step from here: it gives up that place (under fire it goes after him, else it walks on),
                      // and a few steps get it out of a spot where no safe step was left (a ledge, a corner)
                        s.nostepgaveup = lastmillis;
                        s.mvmode = s.mvmode == STYLE_MV_HOLD ? STYLE_MV_CHASE : STYLE_MV_NONE;
                        if(s.srmode == STYLE_SR_HOLD) s.sruntil = lastmillis;
                        s.nostep = 0;
                        styleescape(d, s);
                        return;
                    }
                }
                else s.nostep = 0;
            }
            if(e && !s.steppause)
            { // too close: a step back too
                aiperceived *p = findperceived(d, e);
                vec at = p && p->visible && p->tracking ? e->o : (p && p->aware ? p->knownpos : e->o);
                if(at.squaredist(d->o) < SKEEPMIN*SKEEPMIN && stylestep(d, s, -1, s.stepstrafe)) s.stepmove = -1;
            }
        }
        if(!s.steppause && (s.stepmove || s.stepstrafe))
        { // it went that way and did not get through (something the rays miss): that way is shut for a second
            if(lastmillis - s.progat >= 150)
            {
                float mx = feet.x - s.progpos[0], my = feet.y - s.progpos[1];
                if(mx*mx + my*my < 3*3)
                {
                    if(lastmillis >= s.blockuntil) s.blockdir = 0;
                    s.blockdir |= 1<<stylestepdir(s.stepmove, s.stepstrafe);
                    s.blockuntil = lastmillis + 1000;
                    s.dodgenext = lastmillis; // another way at once
                }
                s.progat = lastmillis; s.progpos[0] = feet.x; s.progpos[1] = feet.y;
            }
        }
        d->move = s.stepmove;
        d->strafe = s.stepstrafe;
    }

    // a style bot that noticed an enemy while it was wandering to a node takes him up: the base game keeps such a bot
    // on its wandering (a wandering state only takes the enemy as its target), so the state becomes a pursuit in place
    static void stylepursuestate(fpsent *d, aistate &b, fpsent *e)
    {
        if(b.type == AI_S_INTEREST && b.targtype == AI_T_NODE && b.target == e->clientnum && b.millis == lastmillis)
        {
            b.type = AI_S_PURSUE;
            b.targtype = AI_T_PLAYER;
        }
    }

    // where a style bot goes while it is after an enemy (from humanpursue, about once a second): it holds its place
    // while it sees him or saw him a moment ago; otherwise it goes after him like a human-perception bot (quietly)
    AI_HUMAN int stylepursue(fpsent *d, aistate &b, fpsent *e, float guard, float wander)
    {
        if(style(d) && d->gunselect != GUN_FIST)
        {
            aistyle &s = claimstyle(d);
            vec feet = d->feetpos();
            float gx = feet.x - s.dodgeat[0], gy = feet.y - s.dodgeat[1];
            bool givenup = s.nostepgaveup && lastmillis - s.nostepgaveup < 1000 && gx*gx + gy*gy < 48*48; // a place it could not step from
            if(s.mvcn == e->clientnum && s.mvseen && lastmillis - s.mvseen < (s.keeplost ? s.keeplost : int(SKEEPLOST)) && !givenup)
            {
                if(s.mvmode != STYLE_MV_HOLD || s.mvlife != e->lifesequence) styleholdhere(d, s);
                s.mvlife = e->lifesequence;
                s.resetsearch();
                b.idle = 2; // it stays there on purpose: not stuck
                return 1;
            }
            s.mvmode = STYLE_MV_CHASE;
        }
        vec pos;
        return humanchase(d, e, pos) && patrol(d, b, pos, guard, wander) ? 1 : 0;
    }

    static bool stylewalkline(fpsent *d, const vec &to);

    // a cover close by for a line it watches (at, an eye position): standing there, nothing of the bot shows from that line
    // and it does not see it (an obstacle within 48 u in front, at eye and at waist height), but at the top of a jump its
    // eye sees it; firm ground, nothing deadly. The spot where it stands first, then the waypoints within SCOVERFIND u, the
    // nearest first (at most 12 tried, about 7 sight tests each); false when none
    static bool stylecoverfind(fpsent *d, aistyle &s, const vec &at)
    {
        vec feet = d->feetpos();
        int cand[33], ncand = 0;
        float cd[33];
        cand[ncand] = -1; cd[ncand] = 0; ncand++;
        for(int i = 1; i < waypoints.length(); i++)
        {
            const waypoint &w = waypoints[i];
            if(!w.links[0] || fabs(w.o.z - feet.z) > 12) continue;
            float d2 = w.o.squaredist(feet);
            if(d2 > SCOVERFIND*SCOVERFIND) continue;
            if(ncand < 33) { cand[ncand] = i; cd[ncand] = d2; ncand++; }
            else
            { // keep the nearest ones
                int worst = 1;
                for(int j = 2; j < ncand; j++) if(cd[j] > cd[worst]) worst = j;
                if(d2 < cd[worst]) { cand[worst] = i; cd[worst] = d2; }
            }
        }
        for(int tried = 0; tried < 12 && ncand > 0; tried++)
        {
            int best = 0;
            loopj(ncand) if(cd[j] < cd[best]) best = j;
            int w = cand[best];
            cand[best] = cand[ncand - 1]; cd[best] = cd[ncand - 1]; ncand--;
            vec c = w < 0 ? feet : waypoints[w].o,
                eye = vec(c).add(vec(0, 0, d->eyeheight)), head = vec(c).add(vec(0, 0, d->eyeheight + d->aboveeye)),
                waist = vec(c).add(vec(0, 0, d->eyeheight*0.5f)), top = vec(c).add(vec(0, 0, d->eyeheight + SCOVERAPEX));
            vec dir = vec(at).sub(eye);
            float dist = dir.magnitude();
            if(dist < 96 || dist > 450) continue;
            dir.mul(1/dist);
            if(raycube(eye, dir, 48) >= 48) continue;                      // the obstacle is close in front (a cover, not a far wall)
            vec wdir = vec(at).sub(waist).normalize();
            if(raycube(waist, wdir, 48) >= 48) continue;                   // it hides the body too
            if(clearview(eye, at) || clearview(at, head)) continue;        // nothing shows, nothing seen, standing
            if(!clearview(top, at)) continue;                              // but its eye sees over it at the top of a jump
            float drop = raycube(vec(c).add(vec(0, 0, 4)), vec(0, 0, -1), 32);
            if(drop > 12) continue;
            int mat = lookupmaterial(vec(c.x, c.y, c.z + 4 - drop + 1));
            if(isdeadly(mat&MATF_VOLUME) || mat&MAT_DEATH) continue;
            if(w >= 0 && !stylewalkline(d, c)) continue;                   // it walks there straight
            s.coverat[0] = c.x; s.coverat[1] = c.y; s.coverat[2] = c.z;
            return true;
        }
        return false;
    }

    // can it walk straight from where it stands to that waypoint: nearly flat ground all along (no step, no ledge, no gap),
    // nothing in the way at knee and waist height, nothing deadly underfoot
    static bool stylewalkline(fpsent *d, const vec &to)
    {
        vec feet = d->feetpos(), dir = vec(to).sub(feet);
        float dz = dir.z;
        dir.z = 0;
        float len = dir.magnitude();
        if(len < 1) return true;
        if(fabs(dz) > 8) return false;
        dir.mul(1/len);
        vec side(-dir.y*d->radius, dir.x*d->radius, 0);
        if(raycube(vec(feet).add(vec(0, 0, 6)), dir, len) < len || raycube(vec(feet).add(vec(0, 0, 12)), dir, len) < len ||
           raycube(vec(feet).add(side).add(vec(0, 0, 8)), dir, len) < len || raycube(vec(feet).sub(side).add(vec(0, 0, 8)), dir, len) < len) return false;
        for(float t = 12; t < len; t += 12)
        {
            float z = feet.z + dz*t/len;
            vec p = vec(dir).mul(t).add(vec(feet.x, feet.y, z + 6));
            float drop = raycube(p, vec(0, 0, -1), 32);
            if(drop > 14 || drop < 1) return false; // a gap or a step down, a step up (the probe starts in the ground)
            int mat = lookupmaterial(vec(p.x, p.y, p.z - drop + 1));
            if(isdeadly(mat&MATF_VOLUME) || mat&MAT_DEATH) return false;
        }
        return true;
    }

    // out of a fight (going somewhere, or after a lost enemy), the way it heads for: the furthest waypoint of its route,
    // a few ahead and at most SSMOOTH u away, it can walk to straight (worked out about 7 times a second); its keys and
    // its view follow that rather than the next waypoint
    static void stylesmooth(fpsent *d, aistyle &s, const vec &dp)
    {
        vector<int> &r = d->ai->route;
        if(d->ai->dontmove || r.length() < 3 || s.mvmode == STYLE_MV_HOLD || s.mvmode == STYLE_MV_GUARD || iswaypoint(s.walkto) ||
           lastmillis < s.escuntil || d->physstate == PHYS_FALL || d->inwater) { s.smoothnode = -1; return; }
        if(lastmillis - s.smoothat >= 150 || lastmillis < s.smoothat || (iswaypoint(s.smoothnode) && r.find(s.smoothnode) < 0))
        {
            s.smoothat = lastmillis;
            s.smoothnode = -1;
            vec feet = d->feetpos();
            for(int i = r.length() - 3, k = 0; i >= 0 && k < 5; i--, k++)
            {
                int w = r[i];
                if(!iswaypoint(w) || waypoints[w].o.squaredist(feet) > SSMOOTH*SSMOOTH || !stylewalkline(d, waypoints[w].o)) break;
                s.smoothnode = w;
            }
        }
        if(iswaypoint(s.smoothnode)) getyawpitch(dp, vec(waypoints[s.smoothnode].o).add(vec(0, 0, d->eyeheight)), d->ai->targyaw, d->ai->targpitch);
    }

    // the enemy part of process() for a style bot (called by processhuman): same knowledge as human perception
    static int styleenemy(fpsent *d, aistate &b, fpsent *e, aiperceived &p, const vec &dp, bool insight, bool quick, const vec &guess)
    {
        aistyle &s = claimstyle(d);
        int dt = s.lastupdate ? clamp(lastmillis - s.lastupdate, 0, 100) : 0;
        s.lastupdate = lastmillis;
        if(lastmillis < s.orientuntil)
        { // just respawned: it finds its bearings, its view does not move yet and it does not shoot
            if(insight) d->ai->enemyseen = lastmillis;
            s.fireat = 0;
            return insight ? 2 : 1;
        }
        if(!insight) stylesmooth(d, s, dp);
        if(insight)
        {
            s.mvseen = lastmillis;
            s.mvcn = e->clientnum;
            if(b.type == AI_S_DEFEND && (s.mvmode != STYLE_MV_HOLD || s.mvlife != e->lifesequence) && d->gunselect != GUN_FIST) { styleholdhere(d, s); s.mvlife = e->lifesequence; }
            s.resetsearch();
        }
        styleexpect(d, s, e, insight, insight || (p.knownsrc == AI_P_HEARD && lastmillis - p.knownmillis < 300));
        // seen again after a short break that the eyes kept him through (a pillar, a corner: less than PGAPMS ms): a
        // player notices him again before aiming at him and shooting, a reaction time as for a new sighting; meanwhile
        // the aim stays on where he was
        if(insight)
        {
            int gap = s.seenlast ? lastmillis - s.seenlast : 0;
            if(gap >= SREGAP && gap <= PGAPMS + PSIGHTMS && s.aimcn == e->clientnum && s.aimlife == e->lifesequence)
            {
                vec to = vec(e->o).sub(dp), f;
                float l = to.magnitude();
                vecfromyawpitch(d->yaw, d->pitch, 1, 0, f);
                float ecc = l > 1 ? acosf(clamp(f.dot(to)/l, -1.0f, 1.0f))/RAD : 0;
                s.regain = lastmillis + reactiontime(d, false, ecc, 3, trackedother(d, e->clientnum));
                s.regainpending = true;
            }
            s.seenlast = lastmillis;
        }
        bool see = insight && lastmillis >= s.regain; // it knows he is in sight (insight) and its eyes are on him again (see)
        if(see && s.regainpending)
        { // noticed again: a new gesture towards him
            s.regainpending = false;
            s.phase = STYLE_NONE;
            s.ncorr = 0;
            s.histn = 0;
            s.onset = lastmillis;
            s.fireat = 0;
        }
        // just lost from sight: he mostly comes back near where he went out (measured in the player's duels: hidden 0.5 to 6 s, he
        // comes back 9.5 degrees from where they last saw him, 8.5 from that point carried on 300 ms along his way), so that
        // line first, a little ahead along his way, for about 2 s (longer when it stopped to hold it); then the guessed
        // ways out one after the other, and the ways into its own place
        if(see && s.aimcn == e->clientnum && s.aimlife == e->lifesequence)
        {
            if(!s.wassee)
            {
                s.linedur = int(clamp(lognormal(SLINEMS, SLINESD), 1000.0f, 4000.0f)); // how long it will keep his line once he is gone
                s.keeplost = !m_teammode && aliveenemies(d) == 1 ? int(clamp(lognormal(SKEEPDUEL, 0.5f), 1500.0f, 8000.0f)) : int(SKEEPLOST); // and its place
            }
            s.seenmillis = lastmillis;
            s.seenpos[0] = e->o.x; s.seenpos[1] = e->o.y; s.seenpos[2] = e->o.z;
            vec v = vec(e->vel).add(e->falling);
            v.z = 0;
            if(v.magnitude() > 200) v.mul(200/v.magnitude());
            s.seenvel[0] = v.x; s.seenvel[1] = v.y; s.seenvel[2] = 0;
        }
        s.wassee = see;
        int lost = lastmillis - s.seenmillis;
        // just lost (or seen again but not noticed yet): the line where he went out; then his nearest way out, then the way
        // he would take towards the bot (stylepreaim); without any known way, as before (his guessed ways out, the ways
        // into its own place)
        bool seenline = !see && s.seenmillis && s.aimcn == e->clientnum && s.aimlife == e->lifesequence && (lost < SPRELINE || insight);
        float lead = min(lost, int(SLINELEAD))/1000.0f;
        vec line;
        bool pre = false;
        if(seenline) line = vec(s.seenpos[0] + s.seenvel[0]*lead, s.seenpos[1] + s.seenvel[1]*lead, s.seenpos[2]);
        else if(!insight && stylepreaim(d, s, p, dp, e, line)) pre = true;
        else line = !insight && p.numexits > 0 && lastmillis - p.knownmillis >= PNEARMS ? styleexitsweep(d, s, p, dp, e) :
                    (!insight && p.numexits <= 0 && lastmillis - p.knownmillis >= SEXITOWNAFTER ? styleownways(d, s, p, dp, guess) : guess);
        if(!pre) s.prepick = -1;
        // the strong position "cover and jump" (duel, free for all): now and then, while it watches where a lost enemy should
        // come back, a cover close by from which it sees that line only at the top of a jump; there it watches that line
        if(s.covermode)
        {
            bool ground = d->physstate >= PHYS_SLOPE && !d->timeinair;
            if(insight && ground) { if(!s.coverseen) s.coverseen = lastmillis; } else if(!insight) s.coverseen = 0;
            if(m_teammode || lastmillis >= s.coveruntil || (s.coverseen && lastmillis - s.coverseen > 300) || lastmillis - d->lastpain < 300) s.covermode = 0;
            else if(!insight)
            {
                if(s.seenmillis > s.coverstart && s.aimcn == e->clientnum && s.aimlife == e->lifesequence)
                { // seen over its cover during this hold: the next jumps look where it last saw him (as the player does)
                    s.coverlook[0] = s.seenpos[0]; s.coverlook[1] = s.seenpos[1]; s.coverlook[2] = s.seenpos[2];
                }
                line = vec(s.coverlook[0], s.coverlook[1], s.coverlook[2]);
            }
        }
        else if(!m_teammode && pre && !insight && !quick && lastmillis >= s.covercheck && d->gunselect != GUN_FIST)
        {
            s.covercheck = lastmillis + 800 + rnd(400);
            vec last(s.seenpos[0], s.seenpos[1], s.seenpos[2]), look = line;
            float dd = line.dist(dp);
            bool found = false;
            if(rndscale(1) < SCOVERP)
            { // the line it watches, else where it lost him
                if(dd > 96 && dd < 450 && stylecoverfind(d, s, line)) found = true;
                else if(s.seenmillis && s.aimcn == e->clientnum && s.aimlife == e->lifesequence && last.dist(dp) > 96 && last.dist(dp) < 450 && stylecoverfind(d, s, last)) { found = true; look = last; }
            }
            if(found)
            {
                s.covermode = 1;
                s.coverstart = lastmillis;
                s.coveruntil = lastmillis + 600 + int(clamp(lognormal(SCOVERHOLD, SCOVERHOLDSD), 1500.0f, 8000.0f));
                s.coverlook[0] = look.x; s.coverlook[1] = look.y; s.coverlook[2] = look.z;
                s.coverseen = 0;
            }
        }
        vec ep = see ? styleaimpoint(d, e) : guessaimpos(d, e, line);
        float ty, tp, dist = dp.dist(ep);
        getyawpitch(dp, ep, ty, tp);
        if(!see)
        { // its view rests on the line it watches while it steps about (no drift with each step): set again when that line
          // moves away by more than SRESTDEG degrees or the bot moved SLOOKMOVED u (a guessed way out: SPREREST, SPREMOVED)
            float ry = stylewrap(ty - s.restyaw), rp = tp - s.restpitch, fx = dp.x - s.restfrom[0], fy = dp.y - s.restfrom[1],
                  rest = pre ? SPREREST : SRESTDEG, moved = pre ? SPREMOVED : SLOOKMOVED;
            if(ry*ry + rp*rp > rest*rest || fx*fx + fy*fy > moved*moved)
            {
                s.restyaw = ty; s.restpitch = tp;
                s.restfrom[0] = dp.x; s.restfrom[1] = dp.y; s.restfrom[2] = dp.z;
            }
            ty = s.restyaw; tp = s.restpitch;
        }
        if(s.aimcn != e->clientnum || s.aimlife != e->lifesequence || (see && lastmillis - s.lastinsight > PGAPMS))
        { // a new target, or the same one seen again: a new gesture
            if(s.aimcn != e->clientnum || s.aimlife != e->lifesequence) s.seenmillis = 0;
            s.aimcn = e->clientnum;
            s.aimlife = e->lifesequence;
            s.phase = STYLE_NONE;
            s.ncorr = 0;
            s.histn = 0;
            s.onset = lastmillis;
            s.fireat = 0;
            s.readyat = lastmillis;
            stylenewaim(s, guns[d->gunselect].attackdelay > 200);
        }
        if(see)
        {
            s.lastinsight = lastmillis;
            s.spread = styleunpred(d, e); // how hard he is to hit right now
        }
        stylehistory(s, ty, tp);
        float half = see ? atan2f(e->radius, max(dist, 1.0f))/RAD : 0;
        bool hold = !insight && !quick && !s.covermode && stylehold(d, s, p, dp, line);
        if(!see && s.phase == STYLE_TRACK)
        { // following a guess: a new gesture when the guess moves away
            float ey = stylewrap(ty - d->yaw), ez = tp - d->pitch;
            if(ey*ey + ez*ez > 25) s.phase = STYLE_NONE;
        }
        stylemove(d, s, ty, tp, half, see, dt);
        if(hold)
        {
            d->ai->dontmove = true;
            d->jumping = false;
        }
        if(insight) d->ai->enemyseen = lastmillis;
        if(!insight && !quick) { s.fireat = 0; return 1; }
        bool automatic = guns[d->gunselect].attackdelay <= 200, burst = automatic && lastmillis - s.lastshot < 250;
        // the weapon is ready again, but the hand notices it a moment later
        if(!canshoot(d, e) || (s.reprise && lastmillis - s.lastshot < guns[d->gunselect].attackdelay + s.reprise))
        {
            s.fireat = 0;
            s.readyat = lastmillis;
            return 2;
        }
        if(s.fireat)
        {
            if(lastmillis < s.fireat) return 2;
            s.fireat = 0;
            if(!burst) stylenewaim(s, !automatic);
            s.lastshot = s.readyat = lastmillis;
            s.reprise = automatic ? 0 : max(1, int(lognormal(SREPRISE, SREPRISESD)*styleslow(d)));
            d->attacking = true;
            d->ai->lastaction = lastmillis;
            styledose(d, e->clientnum, 0.02f);
            return 3;
        }
        // not before its eyes are on him again, nor in the middle of a big flick that is not to end in a shot, nor before
        // the hand has settled after it
        if(!see || lastmillis - s.onset < SMINSHOT*styleslow(d) || lastmillis < s.settleuntil || (s.phase == STYLE_GESTURE && !s.gflick)) return 2;
        float ey = stylewrap(ty - d->yaw), ez = tp - d->pitch,
              off = sqrtf(ey*ey + ez*ez)/max(half, 0.05f), // how far the crosshair is from the middle of the target
              waited = max(0, lastmillis - max(s.readyat, s.onset)),
              tol = (s.flick ? SFLICKTOL : s.clic)*styleerr(d, s)*s.spread*(1 + waited/SPATIENCE);
        if(burst) tol = max(tol, 1.5f);
        if(off < tol)
        {
            s.fireat = lastmillis + (burst ? 0 : max(1, int(lognormal(STRIG, STRIGSD)*styleslow(d))));
            if(s.phase == STYLE_GESTURE) s.gshot = true;
        }
        return 2;
    }

    // team games: where the enemies should come from, the middle of their spawn points (the reference player in CTF:
    // "I wait for the enemies to come and look where I think they are, often near their spawn")
    static bool styleenemyside(fpsent *d, const vec &dp, vec &at)
    {
        static int cachemap = -1, cachetag = -1, cachen = 0;
        static vec cachepos(0, 0, 0);
        if(!m_teammode || !cmode) return false;
        int tag = -1;
        loopv(players) if(players[i] != d && !isteam(d->team, players[i]->team)) { tag = cmode->getspawngroup(players[i]); break; }
        if(tag < 0) return false;
        if(cachemap != maptime || cachetag != tag)
        {
            cachemap = maptime;
            cachetag = tag;
            cachen = 0;
            cachepos = vec(0, 0, 0);
            const vector<extentity *> &ents = entities::getents();
            loopv(ents) if(ents[i]->type == PLAYERSTART && ents[i]->attr2 == tag) { cachepos.add(ents[i]->o); cachen++; }
            if(cachen) cachepos.div(cachen);
        }
        if(!cachen) return false;
        at = vec(cachepos).add(vec(0, 0, d->eyeheight));
        return at.squaredist(dp) > 128*128;
    }

    // search with no enemy in mind (duel, free for all; measured on the reference player: they hold a place, 95 % of
    // the time within 128 u, still moving most of the time, and looks at the spawn points quickly one by one, 1.2 a
    // second, 225 ms each, almost never the nearest first; they look along their way only 17 % of the time; calmly)
    static int stylespawntag(fpsent *d)
    { // spawn points an enemy may use: every one in free for all, those of the other team in team games
        if(!m_teammode || !cmode) return 0;
        loopv(players) if(players[i] != d && !isteam(d->team, players[i]->team)) return cmode->getspawngroup(players[i]);
        return -1;
    }

    // the next spawn point to look at: in sight, not looked at yet, never the nearest to the bot (nor one too close); the
    // first one nearest in angle to where it looks, then by jumps of its view rather than the next one along, now and then
    // behind it (measured on the reference player: 30-90 degree gestures twice as often as 10-30 while they search)
    static bool stylenextspawn(fpsent *d, aistyle &s, const vec &dp, vec &at)
    {
        int tag = stylespawntag(d);
        if(tag < 0) return false;
        const vector<extentity *> &ents = entities::getents();
        float nearest = 1e9f;
        loopv(ents) if(ents[i]->type == PLAYERSTART && ents[i]->attr2 == tag) nearest = min(nearest, ents[i]->o.dist(d->feetpos()));
        vec f;
        vecfromyawpitch(d->yaw, 0, 1, 0, f);
        bool first = !(s.srdone[0] || s.srdone[1]);
        for(int pass = 0; pass < 2; pass++)
        {
            int n = 0, nc = 0, bits[32];
            float ang[32], w[32], total = 0;
            vec eyes[32];
            loopv(ents)
            {
                const extentity &en = *ents[i];
                if(en.type != PLAYERSTART || en.attr2 != tag) continue;
                int bit = n++;
                if(bit >= 64 || nc >= 32 || s.srdone[bit>>5]&(1u<<(bit&31))) continue;
                float dist = en.o.dist(d->feetpos());
                if(dist < 128 || dist <= nearest + 1 || dist > 1500) continue;
                vec eye = vec(en.o).add(vec(0, 0, d->eyeheight)), to = vec(eye).sub(dp);
                to.z = 0;
                float l = to.magnitude();
                if(l < 1) continue;
                float a = acosf(clamp(f.dot(to)/l, -1.0f, 1.0f))/RAD;
                bits[nc] = bit; ang[nc] = a; eyes[nc] = eye;
                w[nc] = first ? 1.0f : (a < 20 ? 0.25f : (a < 100 ? 1.0f : SSEARCHBACK));
                total += w[nc]; nc++;
            }
            loopk(6)
            { // one of them (a few sight tests at most): one out of sight is dropped and another one drawn
                if(total <= 0) break;
                int pick = -1;
                if(first) { loopi(nc) if(w[i] > 0 && (pick < 0 || ang[i] < ang[pick])) pick = i; }
                else { float r = rndscale(total); loopi(nc) if(w[i] > 0) { pick = i; if((r -= w[i]) <= 0) break; } }
                if(pick < 0) break;
                if(!clearview(dp, eyes[pick])) { total -= w[pick]; w[pick] = 0; continue; }
                s.srdone[bits[pick]>>5] |= 1u<<(bits[pick]&31);
                s.lookspawn = bits[pick];
                at = eyes[pick];
                return true;
            }
            if(pass || !(s.srdone[0] || s.srdone[1])) break;
            s.srdone[0] = s.srdone[1] = 0; // all looked at: again from the start
            first = false;
        }
        return false;
    }

    // where the view rests while it checks a point (a spawn point, a way in): the direction to it, chosen once and kept
    // while the bot steps about (it does not drift along with each step), set again once the bot has moved SLOOKMOVED u
    static void styleresting(fpsent *d, aistyle &s, const vec &dp, const vec &at)
    {
        float fx = dp.x - s.restfrom[0], fy = dp.y - s.restfrom[1];
        if(fx*fx + fy*fy > SLOOKMOVED*SLOOKMOVED)
        {
            getyawpitch(dp, at, s.restyaw, s.restpitch);
            s.restfrom[0] = dp.x; s.restfrom[1] = dp.y; s.restfrom[2] = dp.z;
        }
    }

    // a place with a clear view to go to next: a waypoint 200 to 700 u away from which many spawn points are in sight,
    // not the same as before, a little away from the middle of the map
    static int stylevantage(fpsent *d, aistyle &s, const vec &dp)
    {
        static int cachemap = -1;
        static vec middle(0, 0, 0);
        if(waypoints.length() <= 1) return -1;
        if(cachemap != maptime)
        {
            cachemap = maptime;
            middle = vec(0, 0, 0);
            for(int i = 1; i < waypoints.length(); i++) middle.add(waypoints[i].o);
            middle.div(max(waypoints.length() - 1, 1));
        }
        int tag = stylespawntag(d);
        const vector<extentity *> &ents = entities::getents();
        vec feet = d->feetpos();
        int best = -1;
        float bestscore = 0;
        int tests = 0;
        loopk(24)
        { // a few waypoints drawn at random (at most about 200 sight tests in all)
            int w = 1 + rnd(waypoints.length() - 1);
            if(w == s.srnode || !waypoints[w].haslinks()) continue;
            float dist = waypoints[w].o.dist(feet);
            if(dist < 200 || dist > 700) continue;
            if(tests > 200) break;
            vec eye = vec(waypoints[w].o).add(vec(0, 0, d->eyeheight));
            int seen = 0;
            loopv(ents) if(ents[i]->type == PLAYERSTART && ents[i]->attr2 == tag && ents[i]->o.dist(waypoints[w].o) > 128 && ents[i]->o.dist(waypoints[w].o) < 1200)
            {
                tests++;
                if(clearview(eye, vec(ents[i]->o).add(vec(0, 0, d->eyeheight)))) seen++;
            }
            float score = (1 + seen)*(waypoints[w].o.dist(middle) < 120 ? 0.7f : 1.0f);
            if(score > bestscore) { bestscore = score; best = w; }
        }
        return best;
    }

    // a way to look around: of a few directions, the one with the longest clear view at eye level
    // the ways into its place: waypoints within 600 u it sees (at most 100 sight tests) (at eye height x 0.7) that lead to a waypoint it does not
    // see, grouped by direction (40 degrees); at most 8
    static int styleexitsfrom(fpsent *d, const vec &dp, float out[8][3], int maxtests);

    // how the ways into a place lie as seen from it: the middle of the smallest angle that holds them all, and that angle
    static void stylecoverof(const vec &dp, float exits[][3], int n, float &center, float &spread)
    {
        float y[16];
        n = min(n, 16);
        loopi(n) y[i] = -atan2f(exits[i][0] - dp.x, exits[i][1] - dp.y)/RAD;
        if(n <= 1) { center = n ? y[0] : 0; spread = 0; return; }
        loopi(n) loopj(n - 1 - i) if(y[j] > y[j + 1]) swap(y[j], y[j + 1]);
        float gap = y[0] + 360 - y[n - 1];
        int after = 0; // the largest gap is before y[after]
        loopi(n - 1) if(y[i + 1] - y[i] > gap) { gap = y[i + 1] - y[i]; after = i + 1; }
        spread = 360 - gap;
        center = y[after] + spread/2;
    }

    static void styleplaceexits(fpsent *d, aistyle &s, const vec &dp)
    {
        float old[8][3];
        int oldlook[8], nold = s.nplaceexits;
        loopk(nold) { loopj(3) old[k][j] = s.placeexits[k][j]; oldlook[k] = s.placeexitlook[k]; }
        s.placeexitsat = lastmillis;
        s.placeexitsfrom[0] = dp.x; s.placeexitsfrom[1] = dp.y; s.placeexitsfrom[2] = dp.z;
        s.nplaceexits = styleexitsfrom(d, dp, s.placeexits, 100);
        loopk(s.nplaceexits)
        { // the same way in as before (same direction): it remembers when it looked at it
            s.placeexitlook[k] = 0;
            vec v = vec(s.placeexits[k][0], s.placeexits[k][1], s.placeexits[k][2]).sub(dp);
            if(v.magnitude() < 1) continue;
            v.normalize();
            loopj(nold)
            {
                vec o = vec(old[j][0], old[j][1], old[j][2]).sub(dp);
                if(o.magnitude() > 1 && v.dot(o.normalize()) > cosf(40*RAD)) { s.placeexitlook[k] = oldlook[j]; break; }
            }
        }
        stylecoverof(dp, s.placeexits, s.nplaceexits, s.coveryaw, s.coverspread);
    }

    static int styleexitsfrom(fpsent *d, const vec &dp, float out[8][3], int maxtests)
    {
        int nout = 0;
        static vector<int> seen;
        static vector<uchar> sees;
        seen.setsize(0);
        sees.setsize(0);
        loopv(waypoints) sees.add(0);
        int tests = 0;
        for(int i = 1; i < waypoints.length() && tests < maxtests; i++)
        { // cheap: the waypoints within 600 u, at most 100 sight tests
            if(waypoints[i].o.squaredist(dp) > 600*600) continue;
            tests++;
            if(clearview(dp, vec(waypoints[i].o).add(vec(0, 0, d->eyeheight*0.7f)))) { sees[i] = 1; seen.add(i); }
        }
        loopv(seen)
        {
            waypoint &w = waypoints[seen[i]];
            bool edge = false;
            loopj(MAXWAYPOINTLINKS)
            {
                int l = w.links[j];
                if(!l) break;
                if(iswaypoint(l) && l < sees.length() && !sees[l] && waypoints[l].o.squaredist(dp) <= 640*640) { edge = true; break; }
            }
            if(!edge) continue;
            vec at = vec(w.o).add(vec(0, 0, d->eyeheight*0.7f)), dir = vec(at).sub(dp);
            if(dir.magnitude() < 48) continue;
            dir.normalize();
            bool merged = false;
            loopk(nout)
            {
                vec o = vec(out[k][0], out[k][1], out[k][2]).sub(dp);
                if(o.magnitude() > 1 && dir.dot(o.normalize()) > cosf(40*RAD)) { merged = true; break; }
            }
            if(merged || nout >= 8) continue;
            out[nout][0] = at.x; out[nout][1] = at.y; out[nout][2] = at.z;
            nout++;
        }
        return nout;
    }

    // a place where the ways in hold in its view: from here if they do; else one or two waypoints close by, the one where
    // they lie closest together if it is clearly better (-1: stay)
    static int stylecoverspot(fpsent *d, aistyle &s, const vec &dp)
    {
        styleplaceexits(d, s, dp);
        if(s.nplaceexits <= 1 || s.coverspread <= 110) return -1;
        vec feet = d->feetpos();
        int best = -1, tries = 0;
        float bestspread = s.coverspread - 30;
        loopk(12)
        {
            if(tries >= 2) break;
            int w = 1 + rnd(waypoints.length() - 1);
            if(!waypoints[w].haslinks()) continue;
            float dist = waypoints[w].o.dist(feet);
            if(dist < 64 || dist > 220) continue;
            tries++;
            float ex[8][3], c, sp;
            vec eye = vec(waypoints[w].o).add(vec(0, 0, d->eyeheight));
            int n = styleexitsfrom(d, eye, ex, 50);
            if(n < 1) continue;
            stylecoverof(eye, ex, n, c, sp);
            if(sp < bestspread) { bestspread = sp; best = w; }
        }
        return best;
    }

    static void stylearound(fpsent *d, aistyle &s, const vec &dp)
    {
        float fx = dp.x - s.placeexitsfrom[0], fy = dp.y - s.placeexitsfrom[1];
        if(!s.placeexitsat || lastmillis - s.placeexitsat > 2000 || fx*fx + fy*fy > 96*96) styleplaceexits(d, s, dp);
        if(s.nplaceexits > 1 && s.coverspread <= 110 && rnd(10) < 6)
        { // the ways in hold in its view: it looks between them, a little to one side or the other
            vec dir;
            vecfromyawpitch(s.coveryaw + (rndscale(1) - 0.5f)*min(s.coverspread, 60.0f), 0, 1, 0, dir);
            vec at = vec(dir).mul(300).add(dp);
            s.lookat[0] = at.x; s.lookat[1] = at.y; s.lookat[2] = at.z;
            return;
        }
        if(s.nplaceexits > 0)
        { // the way in it has not looked at for the longest time (behind it too), now and then another one
            int best = -1;
            loopk(s.nplaceexits) if(best < 0 || s.placeexitlook[k] < s.placeexitlook[best]) best = k;
            if(s.nplaceexits > 1 && rnd(4) == 0) best = rnd(s.nplaceexits);
            s.placeexitlook[best] = lastmillis;
            s.lookat[0] = s.placeexits[best][0]; s.lookat[1] = s.placeexits[best][1]; s.lookat[2] = s.placeexits[best][2];
            return;
        }
        float bestlen = -1, besty = d->yaw;
        loopk(4)
        {
            float y = d->yaw + (rnd(2) ? 1 : -1)*(40 + rnd(110));
            vec dir;
            vecfromyawpitch(y, 0, 1, 0, dir);
            float len = raycube(dp, dir, 1024);
            if(len > bestlen) { bestlen = len; besty = y; }
        }
        vec dir;
        vecfromyawpitch(besty, 0, 1, 0, dir);
        vec at = vec(dir).mul(clamp(bestlen, 64.0f, 512.0f)).add(dp);
        s.lookat[0] = at.x; s.lookat[1] = at.y; s.lookat[2] = at.z;
    }

    // the search itself, once per frame while the bot has no enemy in mind: holds a place, then goes to another one; and
    // what it looks at meanwhile (a spawn point, around, now and then along its way); false when it looks along its way
    static bool stylesearch(fpsent *d, aistate &b, const vec &dp, vec &at)
    {
        aistyle &s = claimstyle(d);
        bool search = !m_teammode && (b.type == AI_S_INTEREST || b.type == AI_S_WAIT) && d->gunselect != GUN_FIST,
             guard = styleidle(d, b);
        if(!search && !guard) { if(s.srmode != STYLE_SR_NONE) s.resetsearch(); return false; }
        if(search)
        {
            if(s.srmode == STYLE_SR_NONE)
            {
                s.srmode = STYLE_SR_HOLD;
                s.srstart = lastmillis;
                s.sruntil = lastmillis + int(clamp(lognormal(SSEARCHHOLD, SSEARCHHOLDSD), 800.0f, 8000.0f));
                styleholdhere(d, s, STYLE_MV_GUARD);
                int spot = stylecoverspot(d, s, dp);
                if(spot >= 0 && makeroute(d, b, spot))
                { // a few steps to a better angle first
                    s.srmode = STYLE_SR_MOVE;
                    s.srnode = spot;
                    s.sruntil = lastmillis + 3000;
                    s.mvmode = STYLE_MV_NONE;
                    if(b.type == AI_S_INTEREST) { b.targtype = AI_T_NODE; b.target = spot; b.millis = lastmillis; }
                }
            }
            if(s.srmode == STYLE_SR_HOLD)
            {
                if(s.mvmode != STYLE_MV_GUARD && s.mvmode != STYLE_MV_HOLD) styleholdhere(d, s, STYLE_MV_GUARD);
                if(lastmillis >= s.sruntil)
                { // on to another place with a clear view
                    int node = stylevantage(d, s, dp);
                    if(node >= 0 && makeroute(d, b, node))
                    {
                        s.srmode = STYLE_SR_MOVE;
                        s.srnode = node;
                        s.sruntil = lastmillis + 8000;
                        s.mvmode = STYLE_MV_NONE;
                        if(b.type == AI_S_INTEREST) { b.targtype = AI_T_NODE; b.target = node; b.millis = lastmillis; }
                    }
                    else s.sruntil = lastmillis + 1000;
                }
            }
            else if(s.srmode == STYLE_SR_MOVE && (!iswaypoint(s.srnode) || lastmillis >= s.sruntil || d->feetpos().squaredist(waypoints[s.srnode].o) < 48*48))
            { // there: it holds that place
                s.srmode = STYLE_SR_HOLD;
                s.sruntil = lastmillis + int(clamp(lognormal(SSEARCHHOLD, SSEARCHHOLDSD), 800.0f, 8000.0f));
                styleholdhere(d, s, STYLE_MV_GUARD);
                styleplaceexits(d, s, dp);
            }
        }
        if(lastmillis >= s.lookuntil || s.lookmode == STYLE_LK_NONE)
        {
            vec sp(0, 0, 0);
            if((s.srmode == STYLE_SR_MOVE && rndscale(1) < SLOOKWAYP) || (!search && rndscale(1) < 0.7f)) // a defender mostly looks to their side
            {
                s.lookmode = STYLE_LK_WAY;
                s.lookuntil = lastmillis + int(clamp(lognormal(SLOOKAROUND, SLOOKAROUNDSD), 150.0f, 2000.0f));
            }
            else if(rndscale(1) < SLOOKSPAWNP && stylenextspawn(d, s, dp, sp))
            {
                s.lookmode = STYLE_LK_SPAWN;
                s.lookat[0] = sp.x; s.lookat[1] = sp.y; s.lookat[2] = sp.z;
                s.lookuntil = lastmillis + int(clamp(lognormal(SSPAWNLOOK, SSPAWNLOOKSD), 100.0f, 900.0f));
            }
            else
            {
                s.lookmode = STYLE_LK_AROUND;
                stylearound(d, s, dp);
                s.lookuntil = lastmillis + int(clamp(lognormal(SLOOKAROUND, SLOOKAROUNDSD), 150.0f, 2000.0f));
            }
            if(s.lookmode != STYLE_LK_WAY)
            { // the view will rest on that direction
                getyawpitch(dp, vec(s.lookat[0], s.lookat[1], s.lookat[2]), s.restyaw, s.restpitch);
                s.restfrom[0] = dp.x; s.restfrom[1] = dp.y; s.restfrom[2] = dp.z;
            }
            s.phase = STYLE_NONE; // a new gesture
        }
        if(s.lookmode == STYLE_LK_WAY) return false;
        at = vec(s.lookat[0], s.lookat[1], s.lookat[2]);
        return true;
    }

    // no enemy in mind (or only an old one far away): after a frag in a duel, look at the spawn points one by one
    // (only those the game is likely to use: never one too close to the bot), a few hundred ms each; in team games,
    // towards where the enemy it remembers should be or, when it guards a place (defends, waits, hardly moves),
    // towards their spawn; otherwise the view follows where the bot goes with the same human gestures (no instant
    // turn). The bot keeps moving either way.
    static int stylelook(fpsent *d, aistate &b, const vec &dp, const vec *guess = NULL)
    {
        aistyle &s = claimstyle(d);
        int dt = s.lastupdate ? clamp(lastmillis - s.lastupdate, 0, 100) : 0;
        s.lastupdate = lastmillis;
        if(lastmillis < s.orientuntil) return 1; // just respawned: it finds its bearings, its view does not move yet
        s.covermode = 0; // no enemy in mind: no cover held
        stylesmooth(d, s, dp);
        vec look;
        bool searching = !guess && stylesearch(d, b, dp, look); // its place is kept also while it checks the spawn points
        fpsent *v = s.watchcn >= 0 && lastmillis < s.watchuntil ? getclient(s.watchcn) : NULL;
        if(!v) s.watching = false;
        else if(!s.watching || lastmillis >= s.watchnext)
        { // the next spawn point (not looked at yet, in sight, usable by the game): the first one in front of the bot, then
          // by jumps of its view, now and then behind it (measured on the reference player after a frag)
            const vector<extentity *> &ents = entities::getents();
            int tag = cmode ? cmode->getspawngroup(v) : 0, best = -1, n = 0, ncand = 0, cand[32];
            float bestang = 1e9f, candang[32], total = 0;
            vec f, candat[32];
            vecfromyawpitch(d->yaw, d->pitch, 1, 0, f);
            loopv(ents)
            {
                const extentity &en = *ents[i];
                if(en.type != PLAYERSTART || en.attr2 != tag) continue;
                int bit = n++;
                if(bit >= 31 || s.watchdone&(1<<bit)) continue;
                vec at = vec(en.o).add(vec(0, 0, v->eyeheight)), to = vec(at).sub(dp);
                float dist = to.magnitude();
                if(dist < 32 || dist > 1500 || game::ratespawn(v, en) < 0.5f || !clearview(dp, at)) continue;
                float ang = acosf(clamp(f.dot(to)/dist, -1.0f, 1.0f))/RAD;
                cand[ncand] = bit; candang[ncand] = ang; candat[ncand] = at; ncand++;
                if(ang < bestang) { bestang = ang; best = ncand - 1; }
            }
            if(ncand && s.watchdone)
            { // after the first one: a jump of the view rather than the next one along
                float w[32];
                loopi(ncand) { w[i] = candang[i] < 20 ? 0.25f : (candang[i] < 100 ? 1.0f : SWATCHBACK); total += w[i]; }
                float r = rndscale(total);
                loopi(ncand) { best = i; if((r -= w[i]) <= 0) break; }
            }
            if(best < 0) { s.watchuntil = 0; s.watching = false; }
            else
            {
                s.watchdone |= 1<<cand[best];
                s.watchat[0] = candat[best].x; s.watchat[1] = candat[best].y; s.watchat[2] = candat[best].z;
                getyawpitch(dp, candat[best], s.restyaw, s.restpitch);
                s.restfrom[0] = dp.x; s.restfrom[1] = dp.y; s.restfrom[2] = dp.z;
                s.watching = true;
                s.watchnext = lastmillis + int(SWATCHMS) + rnd(int(SWATCHRND));
                s.aimcn = -3;
                s.phase = STYLE_NONE;
                s.histn = 0;
            }
        }
        float ty, tp;
        if(s.watching)
        { // the view rests on that spawn point (set again only once the bot has moved away)
            styleresting(d, s, dp, vec(s.watchat[0], s.watchat[1], s.watchat[2]));
            ty = s.restyaw; tp = s.restpitch;
        }
        else
        {
            ty = d->ai->targyaw;
            tp = d->ai->targpitch;
            // the way to go is right under or over the bot (it guards the spot it stands on): it looks ahead at eye
            // level rather than at its feet
            if(fabs(tp) > 60) { ty = d->yaw; tp = 0; }
            vec side;
            if(searching) { styleresting(d, s, dp, look); ty = s.restyaw; tp = s.restpitch; } // searching: spawn points, around (the view rests)
            else if(m_teammode && guess && guess->squaredist(dp) > 128*128) getyawpitch(dp, *guess, ty, tp);
            else if((b.type == AI_S_DEFEND || b.type == AI_S_WAIT || d->vel.magnitude2() < 50) && styleenemyside(d, dp, side))
                getyawpitch(dp, side, ty, tp);
            tp = clamp(tp, -60.0f, 60.0f);
            // its way turns far round: it does not swing round at once, it walks backwards or sideways a moment, then
            // turns with one quick flick (measured: big turns of 160 deg and more 1.3 times a minute)
            if(fabs(stylewrap(ty - d->yaw)) > STURNBACK)
            {
                if(!s.turnat) s.turnat = lastmillis + int(lognormal(STURNMS, 0.4f));
                if(lastmillis < s.turnat) { ty = d->yaw; tp = d->pitch; }
            }
            else s.turnat = 0;
            if(s.aimcn != -2) { s.aimcn = -2; s.phase = STYLE_NONE; s.histn = 0; }
            else if(s.phase == STYLE_TRACK)
            { // a new gesture when the way turns
                float ey = stylewrap(ty - d->yaw), ez = tp - d->pitch;
                if(ey*ey + ez*ez > 20*20) s.phase = STYLE_NONE;
            }
        }
        stylehistory(s, ty, tp);
        stylemove(d, s, ty, tp, 0, false, dt);
        return 1;
    }

    // deaths: the killer of a style bot is remembered where he was, if he survives; a style bot that took out the
    // only enemy it had (a duel) checks the spawn points
    AI_HUMAN void humankilled(fpsent *d, fpsent *e)
    {
        if(!e || d == e || isteam(d->team, e->team)) return;
        if(stylebot(d) && style(d) && e->state == CS_ALIVE)
        {
            aistyle &s = claimstyle(d);
            s.killercn = e->clientnum;
            s.killerlife = e->lifesequence;
            s.killermillis = lastmillis;
            s.killerpos[0] = e->o.x; s.killerpos[1] = e->o.y; s.killerpos[2] = e->o.z;
        }
        if(stylebot(e) && style(e) && e->state == CS_ALIVE && aliveenemies(e) == 0)
        {
            aistyle &s = claimstyle(e);
            s.watchcn = d->clientnum;
            s.watchuntil = lastmillis + 2500;
            s.watchdone = 0;
            s.watching = false;
            s.resetsearch(); // then a new search
        }
    }

    // the base game's waiting state, for a style bot with nothing to do (before it picks a random waypoint): off the
    // waypoints (a spawn point no way leads from), it walks to the nearest waypoint that leads somewhere; in a duel or
    // free for all it searches (holds a place, looks at the spawn points, then goes to another place with a clear view)
    AI_HUMAN bool stylewait(fpsent *d, aistate &b)
    {
        if(!stylebot(d) || !style(d) || d->state != CS_ALIVE) return false;
        aistyle &s = claimstyle(d);
        if(!iswaypoint(d->lastnode) || !waypoints[d->lastnode].haslinks())
        {
            if(!iswaypoint(s.walkto))
            {
                s.nwalktried = 0;
                int w = stylewalktarget(d, s);
                if(!iswaypoint(w)) return false;
                s.walkto = w;
                s.walkstart = lastmillis;
                s.walkcheck = 0;
            }
            d->ai->switchstate(b, AI_S_INTEREST, AI_T_NODE, s.walkto);
            return true;
        }
        if(m_teammode || d->gunselect == GUN_FIST) return false;
        d->ai->switchstate(b, AI_S_INTEREST, AI_T_NODE, s.srmode == STYLE_SR_MOVE && iswaypoint(s.srnode) ? s.srnode : d->lastnode);
        return true;
    }

    // the base game's state of going to a waypoint, for a style bot: while it walks off a spawn point or holds a place
    // searching it stays in that state (it is not lost); on its way to another place, the way there; -1: as the base game
    AI_HUMAN int styleinterest(fpsent *d, aistate &b)
    {
        if(!stylebot(d) || !style(d)) return -1;
        aistyle &s = claimstyle(d);
        if(iswaypoint(s.walkto)) { b.idle = 2; return 1; }
        if(m_teammode) return -1;
        if(s.srmode == STYLE_SR_HOLD) { b.idle = 2; return 1; }
        if(s.srmode == STYLE_SR_MOVE && iswaypoint(s.srnode)) return makeroute(d, b, waypoints[s.srnode].o) ? 1 : 0;
        return -1;
    }

    static void stylerespawn(fpsent *d) // a new life, its bearings found: the killer is where he was, if he is still alive
    {
        aistyle &s = claimstyle(d);
        if(s.killercn < 0 || lastmillis - s.killermillis > 15000) { s.killercn = -1; return; }
        fpsent *e = getclient(s.killercn);
        s.killercn = -1;
        if(!e || e->lifesequence != s.killerlife || !targetable(d, e)) return;
        aiperceived *q = findperceived(d, e);
        if(q && ((q->visible && q->tracking) || (q->aware && q->knownmillis > s.killermillis))) return; // it knows better by now
        aiperceived &p = getperceived(d, e);
        p.aware = true;
        p.knownpos = vec(s.killerpos[0], s.killerpos[1], s.killerpos[2]);
        p.knownvel = vec(0, 0, 0);
        p.knownmillis = lastmillis;
        p.knownsrc = AI_P_HIT;
        p.numexits = 0;
        p.curexit = -1;
        p.reachedlost = false;
        p.dose = 1;
        p.dosemillis = lastmillis;
    }

    static void styleperceive(fpsent *d) // called by perceive() once per frame for a living style bot
    {
        stylesample();
        if(!style(d)) return;
        aistyle &s = claimstyle(d);
        if(s.respawnlife != d->lifesequence) s.respawnlife = d->lifesequence;
        if(s.killerpending && lastmillis >= s.orientuntil)
        { // its bearings found: where its killer was
            s.killerpending = false;
            stylerespawn(d);
        }
    }

    // team games: an enemy respawns; a player guesses where from the spawn rules and where his own team mates are
    // (the game avoids spawning an enemy near them): the most likely spawn point becomes a rough cue
    static void stylerespawned(fpsent *e)
    {
        if(!m_teammode || !cmode) return;
        const vector<extentity *> &ents = entities::getents();
        int tag = cmode->getspawngroup(e);
        const extentity *best = NULL;
        float rate = 0;
        loopv(ents)
        {
            const extentity &en = *ents[i];
            if(en.type != PLAYERSTART || en.attr2 != tag) continue;
            float r = game::ratespawn(e, en);
            if(r > rate) { rate = r; best = &en; }
        }
        if(!best) return;
        vec at = vec(best->o).add(vec(0, 0, e->eyeheight));
        loopv(players)
        {
            fpsent *d = players[i];
            if(!stylebot(d) || d->state != CS_ALIVE || !targetable(d, e) || d->o.squaredist(at) > 1500*1500) continue;
            aiperceived *p = findperceived(d, e);
            if(p && (p->aware || p->visible)) continue;
            cue(d, e, at, AI_P_HEARD);
        }
    }

    // a shot that passes close to a style bot makes it turn towards the shooter, heard or not
    static void stylenearmiss(fpsent *e)
    {
        if(e->state != CS_ALIVE) return;
        vec f;
        vecfromyawpitch(e->yaw, e->pitch, 1, 0, f);
        float range = guns[e->lastfiredgun >= 0 && e->lastfiredgun < NUMGUNS ? e->lastfiredgun : GUN_RIFLE].range;
        loopv(players)
        {
            fpsent *d = players[i];
            if(d == e || !stylebot(d) || d->state != CS_ALIVE || !targetable(d, e)) continue;
            vec to = vec(d->o).sub(vec(0, 0, d->eyeheight*0.5f)).sub(e->o);
            float along = to.dot(f);
            if(along < 8 || along > range) continue;
            vec miss = vec(f).mul(along).sub(to);
            if(miss.x*miss.x + miss.y*miss.y > 12*12 || fabs(miss.z) > d->eyeheight) continue;
            if(!clearview(e->o, d->o)) continue;
            feelhit(d, e);
        }
    }

    // jumps: none for nothing while closing in on an enemy it cannot see (a quiet approach), and only a few of the
    // random jumps of the base game otherwise; a way that goes up is walked (stairs and slopes: the game steps up by
    // itself), the bot jumps only when it bumps into a ledge (measured: 4 to 7 jumps a minute in duels)
    AI_HUMAN bool stylejump(fpsent *d, aistate &b, bool sequenced, float offz)
    {
        if(!stylebot(d)) return true;
        aistyle *s = style(d);
        if(s && s->owner == d->ai && s->covermode) return false; // behind its cover it jumps on its own
        bool holding = s && s->owner == d->ai && s->mvmode == STYLE_MV_HOLD;
        // unsticking: when it is really stuck (blocked, or its way has not moved on for several seconds), not after it
        // waited on purpose (holding its place or a line)
        if(sequenced && !holding && (d->blocked || d->ai->blockseq || d->ai->targseq >= 2)) return true;
        if(offz >= JUMPMIN)
        {
            vec off = vec(d->ai->spot).sub(d->feetpos());
            return d->blocked && off.x*off.x + off.y*off.y < 32*32;
        }
        if(lastmillis < d->ai->jumprand) return false; // only the timer of the random jumps is left
        fpsent *e = getclient(d->ai->enemy);
        aiperceived *p = e ? findperceived(d, e) : NULL;
        bool quiet = b.type == AI_S_PURSUE && b.targtype == AI_T_PLAYER && !(p && p->visible && p->tracking);
        if(quiet || rndscale(1) >= SJUMPKEEP)
        {
            int seed = (111-d->skill)*(d->inwater ? 3 : 5)*(b.idle ? 50 : 25);
            d->ai->jumprand = lastmillis+seed+rnd(seed);
            return false;
        }
        return true;
    }
