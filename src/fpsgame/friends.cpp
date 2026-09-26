#include "game.h"

// Local friends list: nicks + one clan tag. Never stores player IPs.
// Online search uses the public master list + extinfo, then connect()
// with the server hostname from that live list.

namespace game
{
    enum { MAXFRIENDS = 128, MAXFRIENDONLINE = 64 };

    static const int friendtexcolor[NUMFRIENDCOLORS] =
    {
        0x40FF80, // 0 green
        0x60A0FF, // 1 blue
        0xFFC040, // 2 yellow
        0xFF4040, // 3 red
        0x808080, // 4 gray
        0xC040C0, // 5 magenta
        0xFF8000, // 6 orange
        0xFFFFFF, // 7 white
        0x60F0FF, // 8 cyan
        0xFF80C8  // 9 rose (not in the old 0-8 set)
    };

    static int clampcoloridx(int i)
    {
        if(i < 0) return -1;
        return clamp(i, 0, NUMFRIENDCOLORS-1);
    }

    VARP(friendcolor, 0, 8, 9); // leftover 30 Aug; ally/enemy split below
    VARP(friendallycolor, -1, 8, 9);   // -1 none, 8 cyan — same team
    VARP(friendenemycolor, -1, 6, 9);  // -1 none, 6 orange — other team / FFA
    VARP(mycolor, -1, 9, 9);           // -1 none; your body + name, independent of friends
    SVARP(clantag, "");
    SVAR(friendaddname, "");
    VAR(numfriends, 0, 0, MAXFRIENDS);
    VAR(numfriendshere, 0, 0, 256);
    VAR(numfriendsonline, 0, 0, MAXFRIENDONLINE);
    SVAR(friendscanmsg, "");

    static vector<char *> friendnicks;
    static bool friendsloaded = false;

    enum { FEXT_ACK = -1, FEXT_PLAYERSTATS = 1, FEXT_RESP_IDS = -10, FEXT_RESP_STATS = -11 };

    struct friendonline
    {
        string name, servname, sdesc, map;
        int port, ping, seen;
    };
    static vector<friendonline> onlinelist;

    static ENetSocket extsock = ENET_SOCKET_NULL;
    static int scanidx = 0, lastscanms = 0, lastguims = 0;
    static bool wantscan = false, scanning = false;

    static const char *friendsfile() { return "friends.cfg"; }

    bool isfriendally(fpsent *d)
    {
        return d && player1 && m_teammode && isteam(d->team, player1->team);
    }

    static int friendcoloridx(bool ally)
    {
        int i = ally ? friendallycolor : friendenemycolor;
        return clampcoloridx(i);
    }

    bool friendhascolor(bool ally)
    {
        return friendcoloridx(ally) >= 0;
    }

    int friendcolorindex(bool ally)
    {
        return friendcoloridx(ally);
    }

    int friendrgb(bool ally)
    {
        int i = friendcoloridx(ally);
        if(i < 0) return 0xFFFFDD;
        return friendtexcolor[i];
    }

    char friendcolcode(bool ally)
    {
        int i = friendcoloridx(ally);
        if(i < 0) return '7';
        return char('0' + i);
    }

    vec friendcolorvecidx(int idx)
    {
        int c = friendtexcolor[clamp(idx, 0, NUMFRIENDCOLORS-1)];
        return vec(((c>>16)&0xFF)/255.0f, ((c>>8)&0xFF)/255.0f, (c&0xFF)/255.0f);
    }

    vec friendcolorvec(bool ally)
    {
        return friendcolorvecidx(friendcoloridx(ally));
    }

    int selfcolorindex()
    {
        return clampcoloridx(mycolor);
    }

    bool selfhascolor()
    {
        return selfcolorindex() >= 0;
    }

    int selfrgb()
    {
        int i = selfcolorindex();
        if(i < 0) return 0xFFFFDD;
        return friendtexcolor[i];
    }

    char selfcolcode()
    {
        int i = selfcolorindex();
        if(i < 0) return '7';
        return char('0' + i);
    }

    void cyclemycolor()
    {
        int n = selfcolorindex();
        if(n < 0) n = 0;
        else n = (n + 1) % NUMFRIENDCOLORS;
        mycolor = n;
    }
    ICOMMAND(cyclemycolor, "", (), cyclemycolor());

    static void syncnumfriends()
    {
        numfriends = friendnicks.length();
    }

    static void savefriends()
    {
        stream *f = openfile(friendsfile(), "w");
        if(!f) return;
        f->printf("# nicks only, never IPs\n");
        loopv(friendnicks) f->printf("%s\n", friendnicks[i]);
        delete f;
    }

    void loadfriends()
    {
        if(friendsloaded) return;
        friendsloaded = true;
        friendnicks.deletearrays();
        stream *f = openfile(friendsfile(), "r");
        if(f)
        {
            char buf[512];
            while(f->getline(buf, sizeof(buf)))
            {
                char *s = buf;
                while(iscubespace(*s)) s++;
                if(!*s || *s=='#') continue;
                char *e = s + strlen(s);
                while(e > s && iscubespace(e[-1])) *--e = '\0';
                string nick;
                filtertext(nick, s, false, false, MAXNAMELEN);
                if(!nick[0]) continue;
                bool dup = false;
                loopv(friendnicks) if(!strcmp(friendnicks[i], nick)) { dup = true; break; }
                if(!dup && friendnicks.length() < MAXFRIENDS) friendnicks.add(newstring(nick));
            }
            delete f;
        }
        syncnumfriends();
    }

    bool isfriendname(const char *name)
    {
        if(!name || !name[0]) return false;
        loadfriends();
        loopv(friendnicks) if(!strcmp(friendnicks[i], name)) return true;
        if(clantag && clantag[0])
        {
            string tag;
            filtertext(tag, clantag, false, false, MAXNAMELEN);
            if(tag[0] && strstr(name, tag)) return true;
        }
        return false;
    }

    bool isselfplayer(fpsent *d)
    {
        if(!d || !player1) return false;
        if(d == player1) return true;
        if(d->aitype != AI_NONE) return false;
        return d->clientnum >= 0 && d->clientnum == player1->clientnum;
    }

    bool isfriend(fpsent *d)
    {
        if(!d || isselfplayer(d) || d->aitype!=AI_NONE || !d->name[0]) return false;
        return isfriendname(d->name);
    }

    int friendnamecolor(fpsent *d, int fallback)
    {
        if(isselfplayer(d))
        {
            if(!selfhascolor()) return fallback;
            return selfrgb();
        }
        if(!isfriend(d) || !friendhascolor(isfriendally(d))) return fallback;
        return friendrgb(isfriendally(d));
    }

    static void addfriendnick(const char *raw)
    {
        loadfriends();
        string nick;
        filtertext(nick, raw ? raw : "", false, false, MAXNAMELEN);
        if(!nick[0])
        {
            conoutf(CON_ERROR, "\f4type a nick in the field first");
            return;
        }
        loopv(friendnicks) if(!strcmp(friendnicks[i], nick))
        {
            conoutf("\f4already on your friends list: %s", nick);
            return;
        }
        if(friendnicks.length() >= MAXFRIENDS)
        {
            conoutf(CON_ERROR, "\f4friends list is full");
            return;
        }
        friendnicks.add(newstring(nick));
        syncnumfriends();
        savefriends();
        if(friendhascolor(true)) conoutf("\f%cadded friend:\f7 %s", friendcolcode(true), nick);
        else conoutf("added friend:\f7 %s", nick);
        setsvar("friendaddname", "");
    }

    static void delfriendidx(int i)
    {
        loadfriends();
        if(!friendnicks.inrange(i)) return;
        conoutf("\f4removed friend: %s", friendnicks[i]);
        delete[] friendnicks[i];
        friendnicks.remove(i);
        syncnumfriends();
        savefriends();
    }

    ICOMMAND(addfriend, "", (), addfriendnick(friendaddname));
    ICOMMAND(delfriend, "i", (int *i), delfriendidx(*i));
    ICOMMAND(getfriend, "i", (int *i),
    {
        loadfriends();
        result(friendnicks.inrange(*i) ? friendnicks[*i] : "");
    });

    static void counthere()
    {
        int n = 0;
        loopv(players)
        {
            fpsent *o = players[i];
            if(o && o!=player1 && o->name[0] && isfriendname(o->name)) n++;
        }
        numfriendshere = n;
    }

    ICOMMAND(getfriendhere, "i", (int *idx),
    {
        int n = 0;
        loopv(players)
        {
            fpsent *o = players[i];
            if(!o || o==player1 || !o->name[0] || !isfriendname(o->name)) continue;
            if(n==*idx) { result(o->name); return; }
            n++;
        }
        result("");
    });

    ICOMMAND(getfriendhererel, "i", (int *idx),
    {
        int n = 0;
        loopv(players)
        {
            fpsent *o = players[i];
            if(!o || o==player1 || !o->name[0] || !isfriendname(o->name)) continue;
            if(n==*idx)
            {
                if(o->state==CS_SPECTATOR) result("Spec");
                else if(m_teammode && isteam(o->team, player1->team)) result("With you");
                else result("Against you");
                return;
            }
            n++;
        }
        result("");
    });

    static bool friendherealready(const char *name)
    {
        loopv(players) if(players[i] && !strcmp(players[i]->name, name)) return true;
        return false;
    }

    static void addonline(const char *name, const gameserverinfo &si)
    {
        if(!isfriendname(name)) return;
        if(player1 && !strcmp(name, player1->name)) return;
        if(friendherealready(name)) return;
        loopv(onlinelist)
        {
            friendonline &o = onlinelist[i];
            if(!strcmp(o.name, name) && !strcmp(o.servname, si.name) && o.port==si.port)
            {
                o.seen = totalmillis;
                o.ping = si.ping;
                return;
            }
        }
        if(onlinelist.length() >= MAXFRIENDONLINE) return;
        friendonline &o = onlinelist.add();
        copystring(o.name, name);
        copystring(o.servname, si.name);
        copystring(o.sdesc, si.sdesc ? si.sdesc : "");
        copystring(o.map, si.map ? si.map : "");
        o.port = si.port;
        o.ping = si.ping;
        o.seen = totalmillis;
        numfriendsonline = onlinelist.length();
    }

    static void drostaleonline()
    {
        loopvrev(onlinelist) if(totalmillis - onlinelist[i].seen > 20000) onlinelist.remove(i);
        numfriendsonline = onlinelist.length();
    }

    ICOMMAND(getfriendonline, "i", (int *i),
    {
        if(!onlinelist.inrange(*i)) { result(""); return; }
        friendonline &o = onlinelist[*i];
        static string line;
        const char *desc = o.sdesc[0] ? o.sdesc : o.servname;
        formatstring(line, "%s  %s  %s", o.name, desc, o.map);
        result(line);
    });

    ICOMMAND(getfriendonlinename, "i", (int *i),
    {
        result(onlinelist.inrange(*i) ? onlinelist[*i].name : "");
    });

    ICOMMAND(getfriendonlinemap, "i", (int *i),
    {
        result(onlinelist.inrange(*i) ? onlinelist[*i].map : "");
    });

    ICOMMAND(getfriendonlinehere, "i", (int *i),
    {
        intret(onlinelist.inrange(*i) && friendherealready(onlinelist[*i].name) ? 1 : 0);
    });

    static void joinfriendidx(int i)
    {
        if(!onlinelist.inrange(i)) return;
        friendonline &o = onlinelist[i];
        if(isconnected()) disconnect();
        connectserv(o.servname, o.port, NULL);
    }

    ICOMMAND(joinfriend, "i", (int *i), joinfriendidx(*i));

    static void initsock()
    {
        if(extsock != ENET_SOCKET_NULL) return;
        extsock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
        if(extsock == ENET_SOCKET_NULL) return;
        enet_socket_set_option(extsock, ENET_SOCKOPT_NONBLOCK, 1);
    }

    static void sendextinfo(const ENetAddress &addr)
    {
        if(extsock == ENET_SOCKET_NULL) return;
        uchar data[32];
        ucharbuf p(data, sizeof(data));
        putint(p, 0);
        putint(p, FEXT_PLAYERSTATS);
        putint(p, -1);
        ENetBuffer buf;
        buf.data = data;
        buf.dataLength = p.length();
        enet_socket_send(extsock, &addr, &buf, 1);
    }

    static bool lookupserver(const ENetAddress &addr, gameserverinfo &si)
    {
        int n = numgameservers();
        loopi(n)
        {
            if(!getgameserver(i, si)) continue;
            if(si.address.host==addr.host && si.address.port==addr.port) return true;
        }
        return false;
    }

    static void recvextinfo()
    {
        if(extsock == ENET_SOCKET_NULL) return;
        uchar data[MAXTRANS];
        ENetBuffer buf;
        buf.data = data;
        buf.dataLength = sizeof(data);
        ENetAddress addr;
        for(;;)
        {
            int len = enet_socket_receive(extsock, &addr, &buf, 1);
            if(len <= 0) break;
            ucharbuf p(data, len);
            if(getint(p) != 0) continue;
            if(getint(p) != FEXT_PLAYERSTATS) continue;
            getint(p); // echoed cn
            if(getint(p) != FEXT_ACK) continue;
            getint(p); // version
            if(getint(p)) continue; // error
            int resp = getint(p);
            if(resp != FEXT_RESP_STATS) continue;
            getint(p); // cn
            getint(p); // ping
            string raw;
            getstring(raw, p);
            if(p.overread()) continue;
            string nick;
            filtertext(nick, raw, false, false, MAXNAMELEN);
            if(!nick[0]) continue;
            gameserverinfo si;
            if(!lookupserver(addr, si)) continue;
            addonline(nick, si);
        }
    }

    static void sendscans()
    {
        int n = numgameservers();
        if(n <= 0)
        {
            setsvar("friendscanmsg", "No public servers yet - wait a second, or Refresh");
            return;
        }
        if(scanidx >= n) scanidx = 0;
        int sent = 0, visited = 0;
        while(sent < 8 && visited < n)
        {
            gameserverinfo si;
            int i = scanidx;
            if(++scanidx >= n) scanidx = 0;
            visited++;
            if(!getgameserver(i, si)) continue;
            if(si.address.host == ENET_HOST_ANY) continue;
            if(si.ping < 0 || si.ping == INT_MAX) continue;
            if(si.numplayers <= 0) continue;
            sendextinfo(si.address);
            sent++;
        }
        defformatstring(msg, "Searching public servers...  %d found", onlinelist.length());
        setsvar("friendscanmsg", msg);
        scanning = true;
    }

    void pollfriends()
    {
        loadfriends();
        recvextinfo();
        drostaleonline();
        if(!wantscan) return;
        if(totalmillis - lastguims > 800)
        {
            wantscan = false;
            if(scanning && !onlinelist.length())
                setsvar("friendscanmsg", "None found - private games are not listed");
            scanning = false;
            return;
        }
        if(totalmillis - lastscanms < 250) return;
        lastscanms = totalmillis;
        initsock();
        sendscans();
    }

    ICOMMAND(friendsync, "", (),
    {
        loadfriends();
        counthere();
        lastguims = totalmillis;
        bool needscan = friendnicks.length() || (clantag && clantag[0]);
        if(!needscan) { wantscan = false; return; }
        wantscan = true;
        if(!updatedservers) updatefrommaster();
        refreshservers();
        if(!onlinelist.length() && !scanning)
            setsvar("friendscanmsg", "Searching public servers...");
    });

    ICOMMAND(friendrefresh, "", (),
    {
        onlinelist.setsize(0);
        numfriendsonline = 0;
        scanidx = 0;
        scanning = false;
        wantscan = true;
        lastguims = totalmillis;
        lastscanms = 0;
        setsvar("friendscanmsg", "Refreshing public list...");
        if(!updatedservers) updatefrommaster();
        else refreshservers();
    });
}
