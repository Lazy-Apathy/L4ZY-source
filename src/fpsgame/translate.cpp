#include "game.h"

namespace game
{

// Local HTTP client for the Python translation service (127.0.0.1:8747).
// Incoming chat is shown as one line: translation first, original in grey
// parentheses. The green vanilla line is replaced, not stacked.

enum { TBUF = 512, TMAXJOBS = 16 };

VARP(translatechat, 0, 1, 1);
VARP(translateteamchat, 0, 1, 1);
VARP(translateauto, 0, 1, 2);    // 0=off, 1=I send in, 2=last spoken language
VARP(translatepreview, 0, 0, 1); // 0 = code keep, GPU free for incoming
VARP(translatefemme, 0, 0, 1);   // feminine grammar on /tsay and incoming "you"
VARP(translatepreviewdelay, 100, 400, 2000);
VARP(translateport, 1, 8747, 65535);
VARP(translatetimeout, 100, 20000, 120000);
SVARP(translatehost, "127.0.0.1");
SVARP(translatelang, "fr");
SVARP(translateoutlang, "en");
SVARP(texamplelang, "");
VARP(texampledir, 0, 0, 1); // 0 = incoming chat, 1 = what I send
VARP(texamplekind, 0, 0, 1); // 0 = exact translation, 1 = few-shot example
SVAR(texamplesrc, "");
SVAR(texampledst, "");
VAR(texamplecount, 0, 0, 256);
VAR(texampletotal, 0, 0, 9999);
SVAR(tmodelurl, "");
SVAR(tmodelkey, "");
SVAR(tmodelapimodel, "");
VAR(tmodelcustomcount, 0, 0, 32);
VAR(tupdateavail, 0, 0, 3); // 0 none, 1 available, 2 downloading, 3 restart

struct tjob
{
    int uid, cn;
    int type;
    bool outgoing, preview, team, done, ok, reply, own, force, serv, board;
    int femme;
    char name[MAXSTRLEN];
    char speaker[MAXSTRLEN];
    char original[TBUF];
    char lang[16];
    char detected[16];
    char srclang[16];
    char lastspk[MAXSTRLEN];
    char last[TBUF];
    char mention[MAXNAMELEN+8];
    char result[TBUF];
};

static vector<tjob *> tjobs;
static SDL_mutex *tmutex = NULL;
static int tfailstreak = 0, tcooloff = 0;

static char tprev_src[TBUF], tprev_dst[TBUF], tprev_watch[TBUF], tprev_draw[TBUF];
static char tprev_lang[16], tprev_watchlang[16], tprev_mention[MAXNAMELEN+8], tprev_watchmention[MAXNAMELEN+8];
static int tprev_watchms = 0, tprev_femme = 0;
static bool tprev_ok = false, tprev_reply = false, tprev_watchreply = false;
static char tlast_in_text[TBUF], tlast_in_name[MAXSTRLEN];

struct tfixline
{
    bool used, outgoing, team;
    int uid, femme;
    char name[MAXSTRLEN];
    char original[TBUF];
    char shown[TBUF];
    char lang[16];
    char mention[MAXNAMELEN+8];
};
static tfixline tlast_in_fix, tlast_out_fix;

// Last incoming chat for F9 / /tretry. Tagged MOTD / [INFO] is a separate slot
// so hopmod stats / flagruns / assists cannot steal F9 from chat.
// F10 numbered /tretry looks up chat records by uid.
struct tretryline
{
    bool used, serv, team, own;
    int uid, femme;
    char name[MAXSTRLEN];
    char speaker[MAXSTRLEN];
    char original[TBUF];
    char mention[MAXNAMELEN+8];
    char lang[16];
};
static tretryline tlast_retry;
static tretryline tlast_retry_serv;
static vector<tretryline> tretryhist;

struct tspeak
{
    int cn, when;
    char name[MAXNAMELEN+1];
    char text[TBUF];
};
static vector<tspeak> tspeaks;

enum { TLANGMAX = 8 };
struct tlangrec
{
    int cn, n;
    char name[MAXNAMELEN+1];
    char codes[TLANGMAX][8];
};
static vector<tlangrec> tlangs;

static bool tlangok(const char *code)
{
    if(!code || !code[0]) return false;
    int n = 0;
    for(; code[n]; n++) if(!isalpha((uchar)code[n])) return false;
    return n >= 2 && n <= 8;
}

static void tlangadd(int cn, const char *name, const char *code)
{
    char buf[8];
    if(!tlangok(code)) return;
    int n = 0;
    for(; code[n] && n < 7; n++) buf[n] = char(tolower((uchar)code[n]));
    buf[n] = 0;

    tlangrec *r = NULL;
    if(cn >= 0) loopv(tlangs) if(tlangs[i].cn == cn) { r = &tlangs[i]; break; }
    if(!r && name && name[0]) loopv(tlangs) if(!strcasecmp(tlangs[i].name, name)) { r = &tlangs[i]; break; }
    if(!r)
    {
        r = &tlangs.add();
        r->cn = cn;
        r->n = 0;
        r->name[0] = 0;
    }
    if(cn >= 0) r->cn = cn;
    if(name && name[0]) copystring(r->name, name, MAXNAMELEN+1);
    loopi(r->n) if(!strcmp(r->codes[i], buf)) return;
    if(r->n >= TLANGMAX) return;
    copystring(r->codes[r->n++], buf, 8);
}

const char *translatelangsof(fpsent *d)
{
    static char buf[64];
    buf[0] = 0;
    if(!d) return buf;
    tlangrec *r = NULL;
    if(d->clientnum >= 0) loopv(tlangs) if(tlangs[i].cn == d->clientnum) { r = &tlangs[i]; break; }
    if(!r && d->name[0]) loopv(tlangs) if(!strcasecmp(tlangs[i].name, d->name)) { r = &tlangs[i]; break; }
    if(!r || r->n <= 0) return buf;
    int pos = 0;
    loopi(r->n)
    {
        if(pos && pos < 63) buf[pos++] = ' ';
        for(const char *s = r->codes[i]; *s && pos < 63; s++) buf[pos++] = *s;
    }
    buf[pos] = 0;
    return buf;
}

static int tcomp_i = -1;
static char tcomp_prefix[64];
static char tcomp_cmd[32];

const char *chatpaintmention(const char *text, char restcolor);
static void tsendnow(const char *msg, bool team);
static void tjoinmention(const char *mention, const char *msg, char *dst, int dstlen);
static void ttrim(char *s);
static const char *tskipword(const char *s, char *dst, int dstlen);

static const char *tlangcopy(const char *s, const char *fallback)
{
    static char buf[2][16];
    static int turn = 0;
    turn ^= 1;
    if(!s || !s[0]) s = fallback;
    int n = 0;
    for(; s[n] && n < 15; n++) buf[turn][n] = char(tolower((uchar)s[n]));
    buf[turn][n] = 0;
    return buf[turn];
}

static const char *thost()
{
    return translatehost && translatehost[0] ? translatehost : "127.0.0.1";
}

// L4ZY.exe choisit un port libre pour le service de SON installation et
// le donne ici. Il n'est pas sauvegarde dans config.cfg : un autre client
// partageant le profil garde son propre reglage translateport.
static int tport()
{
    static int envport = -1;
    if(envport < 0)
    {
        const char *e = getenv("L4ZY_SERVICE_PORT");
        int p = e ? atoi(e) : 0;
        envport = p > 0 && p < 65536 ? p : 0;
    }
    return envport ? envport : translateport;
}

static const char *tlangin()
{
    return tlangcopy(translatelang, "fr");
}

static const char *tlangout()
{
    return tlangcopy(translateoutlang, "en");
}

static const char *tautolabel()
{
    if(translateauto >= 2) return "last spoken";
    if(translateauto == 1) return "base language";
    return "off";
}

static bool tautolast()
{
    return translateauto >= 2;
}

static void tlock()
{
    if(!tmutex) tmutex = SDL_CreateMutex();
    if(tmutex) SDL_LockMutex(tmutex);
}

static void tunlock()
{
    if(tmutex) SDL_UnlockMutex(tmutex);
}

static bool tsameish(const char *a, const char *b)
{
    if(!a || !b) return false;
    while(*a && *b)
    {
        if(tolower((uchar)*a) != tolower((uchar)*b)) return false;
        a++;
        b++;
    }
    return !*a && !*b;
}

static void turlenc(const char *s, char *dst, int dstlen)
{
    static const char *hex = "0123456789ABCDEF";
    int n = 0;
    for(; s && *s && n < dstlen-4; s++)
    {
        uchar c = (uchar)*s;
        if((c>='A'&&c<='Z') || (c>='a'&&c<='z') || (c>='0'&&c<='9') || c=='-' || c=='_' || c=='.' || c=='~')
            dst[n++] = char(c);
        else
        {
            dst[n++] = '%';
            dst[n++] = hex[c>>4];
            dst[n++] = hex[c&15];
        }
    }
    dst[n] = 0;
}

static bool tconnect(SOCKET sock, const sockaddr_in &addr, int timeoutms)
{
    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);
    int rc = connect(sock, (const sockaddr *)&addr, sizeof(addr));
    if(rc == 0)
    {
        nb = 0;
        ioctlsocket(sock, FIONBIO, &nb);
        return true;
    }
    if(WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINPROGRESS)
        return false;
    fd_set wset, eset;
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    FD_SET(sock, &wset);
    FD_SET(sock, &eset);
    timeval tv;
    tv.tv_sec = timeoutms / 1000;
    tv.tv_usec = (timeoutms % 1000) * 1000;
    rc = select(0, NULL, &wset, &eset, &tv);
    nb = 0;
    ioctlsocket(sock, FIONBIO, &nb);
    if(rc <= 0) return false;
    int err = 0;
    int errlen = sizeof(err);
    if(getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) < 0 || err)
        return false;
    return true;
}

static int tprobe()
{
    // 0 = ok, 1 = not started, 2 = unreachable
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock == INVALID_SOCKET) return 2;
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((ushort)tport());
    unsigned long ip = inet_addr(thost());
    if(ip == INADDR_NONE || ip == INADDR_ANY)
    {
        hostent *he = gethostbyname(thost());
        if(!he) { closesocket(sock); return 2; }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    else addr.sin_addr.s_addr = ip;
    bool ok = tconnect(sock, addr, 800);
    closesocket(sock);
    if(ok) return 0;
    return 1;
}

static void thdrlang(const char *resp, const char *end, const char *key, char *dst, int dstlen)
{
    if(!dst || dstlen <= 0) return;
    dst[0] = 0;
    int klen = (int)strlen(key);
    for(const char *h = resp; h && h < end; )
    {
        const char *nl = strstr(h, "\r\n");
        if(!nl || nl > end) break;
        if(!strncasecmp(h, key, klen))
        {
            h += klen;
            while(*h==' ' || *h=='\t') h++;
            int n = 0;
            while(h+n < nl && n < dstlen-1)
            {
                uchar c = (uchar)h[n];
                if(!isalnum(c) && c!='-') break;
                dst[n] = char(tolower(c));
                n++;
            }
            dst[n] = 0;
            return;
        }
        h = nl + 2;
    }
}

static bool thttp(const char *lang, const char *speaker, const char *dir, int team, int preview, int femme, int reply, int force, int board, const char *last, const char *lastspk, const char *body, char *out, int outlen, char *langout, int langoutlen, char *srcout, int srcoutlen)
{
    out[0] = 0;
    if(langout && langoutlen) langout[0] = 0;
    if(srcout && srcoutlen) srcout[0] = 0;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock == INVALID_SOCKET) return false;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((ushort)tport());
    unsigned long ip = inet_addr(thost());
    if(ip == INADDR_NONE || ip == INADDR_ANY)
    {
        hostent *he = gethostbyname(thost());
        if(!he) { closesocket(sock); return false; }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    else addr.sin_addr.s_addr = ip;

    int timeout = clamp(translatetimeout, 100, 120000);
    if(!tconnect(sock, addr, min(timeout, 3000)))
    {
        closesocket(sock);
        return false;
    }

    DWORD tv = (DWORD)timeout;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));

    char encspk[MAXSTRLEN*3];
    turlenc(speaker ? speaker : "", encspk, sizeof(encspk));
    char enclast[TBUF*3], enclastspk[MAXSTRLEN*3];
    enclast[0] = enclastspk[0] = 0;
    if(reply && last && last[0])
    {
        char lastutf[TBUF*3];
        size_t n = encodeutf8((uchar *)lastutf, sizeof(lastutf)-1, (const uchar *)last, strlen(last));
        lastutf[n] = 0;
        turlenc(lastutf, enclast, sizeof(enclast));
        turlenc(lastspk ? lastspk : "", enclastspk, sizeof(enclastspk));
    }
    char query[2048];
    if(board)
        nformatstring(query, sizeof(query), "lang=%s&speaker=%s&dir=in&board=1", lang, encspk);
    else if(reply)
        nformatstring(query, sizeof(query), "lang=%s&speaker=%s&dir=%s&team=%d&preview=%d&genre=%s&reply=1&force=%d&lastspk=%s&last=%s",
            lang, encspk, dir, team, preview, femme ? "f" : "", force, enclastspk, enclast);
    else
        nformatstring(query, sizeof(query), "lang=%s&speaker=%s&dir=%s&team=%d&preview=%d&genre=%s&force=%d",
            lang, encspk, dir, team, preview, femme ? "f" : "", force);

    char utf8body[TBUF*3];
    size_t bodylen = 0;
    if(body && body[0])
    {
        bodylen = encodeutf8((uchar *)utf8body, sizeof(utf8body)-1, (const uchar *)body, strlen(body));
        utf8body[bodylen] = 0;
    }

    char hdr[3072];
    nformatstring(hdr, sizeof(hdr),
        "POST /translate?%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        query, thost(), (int)bodylen);

    if(send(sock, hdr, (int)strlen(hdr), 0) < 0) { closesocket(sock); return false; }
    if(bodylen && send(sock, utf8body, (int)bodylen, 0) < 0) { closesocket(sock); return false; }

    char resp[4096];
    int got = 0;
    while(got < (int)sizeof(resp)-1)
    {
        int n = recv(sock, resp+got, (int)sizeof(resp)-1-got, 0);
        if(n <= 0) break;
        got += n;
    }
    closesocket(sock);
    resp[got] = 0;

    if(strncmp(resp, "HTTP/1.", 7) || !strstr(resp, " 200 ")) return false;
    char *start = strstr(resp, "\r\n\r\n");
    if(!start) return false;
    thdrlang(resp, start, "X-Lang:", langout, langoutlen);
    thdrlang(resp, start, "X-Src-Lang:", srcout, srcoutlen);
    start += 4;
    while(*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    char *eol = strpbrk(start, "\r\n");
    size_t rawlen = eol ? (size_t)(eol - start) : strlen(start);
    char cubebuf[TBUF];
    size_t n = decodeutf8((uchar *)cubebuf, sizeof(cubebuf)-1, (const uchar *)start, rawlen);
    cubebuf[n] = 0;
    filtertext(out, cubebuf, true, true, outlen-1);
    return true;
}

static bool tconnectservice(SOCKET &sock, int timeoutms)
{
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(sock == INVALID_SOCKET) return false;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((ushort)tport());
    unsigned long ip = inet_addr(thost());
    if(ip == INADDR_NONE || ip == INADDR_ANY)
    {
        hostent *he = gethostbyname(thost());
        if(!he) { closesocket(sock); sock = INVALID_SOCKET; return false; }
        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }
    else addr.sin_addr.s_addr = ip;

    if(!tconnect(sock, addr, timeoutms))
    {
        closesocket(sock);
        sock = INVALID_SOCKET;
        return false;
    }

    DWORD tv = (DWORD)timeoutms;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    return true;
}

static bool tplainhttp(const char *method, const char *pathq, const char *body, int bodylen, char *resp, int resplen, int timeoutms)
{
    if(!resp || resplen <= 1) return false;
    resp[0] = 0;
    SOCKET sock;
    int timeout = timeoutms > 0 ? timeoutms : 2000;
    if(!tconnectservice(sock, timeout)) return false;

    char hdr[1024];
    nformatstring(hdr, sizeof(hdr),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, pathq, thost(), bodylen > 0 ? bodylen : 0);

    bool ok = send(sock, hdr, (int)strlen(hdr), 0) >= 0;
    if(ok && bodylen > 0 && body) ok = send(sock, body, bodylen, 0) >= 0;
    int got = 0;
    while(ok && got < resplen-1)
    {
        int n = recv(sock, resp+got, resplen-1-got, 0);
        if(n <= 0) break;
        got += n;
    }
    closesocket(sock);
    resp[got] = 0;
    if(!ok) return false;
    return !strncmp(resp, "HTTP/1.", 7) && strstr(resp, " 200 ");
}

static bool tlearnhttp(const char *lang, const char *dir, int femme, const char *src, const char *dst, int kind)
{
    char usrc[TBUF*3], udst[TBUF*3], body[TBUF*6+4], pathq[256];
    size_t nsrc = encodeutf8((uchar *)usrc, sizeof(usrc)-1, (const uchar *)(src ? src : ""), src ? strlen(src) : 0);
    usrc[nsrc] = 0;
    size_t ndst = encodeutf8((uchar *)udst, sizeof(udst)-1, (const uchar *)(dst ? dst : ""), dst ? strlen(dst) : 0);
    udst[ndst] = 0;
    if(nsrc + 1 + ndst >= sizeof(body)) return false;
    memcpy(body, usrc, nsrc);
    body[nsrc] = '\n';
    memcpy(body+nsrc+1, udst, ndst);
    int bodylen = (int)(nsrc + 1 + ndst);

    nformatstring(pathq, sizeof(pathq), "/learn?lang=%s&dir=%s&genre=%s&kind=%s", lang ? lang : "fr", dir ? dir : "in", femme ? "f" : "", kind ? "ex" : "exact");
    char resp[1024];
    return tplainhttp("POST", pathq, body, bodylen, resp, sizeof(resp), 2000);
}

enum { TEXAMPLE_MENU_MAX = 64, TEXAMPLE_RESP = 65536 };

struct texamplepair
{
    bool femme;
    int kind; // 0 exact, 1 few-shot
    char src[TBUF];
    char dst[TBUF];
};
static vector<texamplepair> texamplelist;
static char texample_fetched_lang[16];
static int texample_fetched_dir = -1, texample_fetched_femme = -1, texample_failms = 0;
static int texample_shots = 0, texample_exacts = 0;
static bool texample_stale = true, texample_ok = false;

static void texampleclip(char *dst, int dstlen, const char *src, int keep)
{
    if(!dst || dstlen <= 1) return;
    if(!src) src = "";
    int n = (int)strlen(src);
    if(n < keep) keep = n;
    if(keep > dstlen-1) keep = dstlen-1;
    if(n <= keep)
    {
        memcpy(dst, src, n);
        dst[n] = 0;
        return;
    }
    if(keep < 2) keep = 2;
    memcpy(dst, src, keep-2);
    dst[keep-2] = '.';
    dst[keep-1] = '.';
    dst[keep] = 0;
}

static int thdrint(const char *resp, const char *end, const char *key, int fallback)
{
    if(!resp || !key) return fallback;
    int klen = (int)strlen(key);
    for(const char *h = resp; h && (!end || h < end); )
    {
        const char *nl = strstr(h, "\r\n");
        if(!nl || (end && nl > end)) break;
        if(!strncasecmp(h, key, klen))
        {
            h += klen;
            while(*h==' ' || *h=='\t') h++;
            return atoi(h);
        }
        if(nl[0]=='\r' && nl[1]=='\n' && nl[2]=='\r' && nl[3]=='\n') break;
        h = nl + 2;
    }
    return fallback;
}

static bool texampleparseitem(char *line, texamplepair &p)
{
    ttrim(line);
    if(!line[0] || line[0]=='#') return false;
    char *sep1 = strstr(line, " | ");
    if(!sep1) return false;
    *sep1 = 0;
    char *rest = sep1 + 3;
    char *sep2 = strstr(rest, " | ");
    if(!sep2) return false;
    *sep2 = 0;
    char tok[16];
    const char *s = tskipword(line, tok, sizeof(tok));
    s = tskipword(s, tok, sizeof(tok));
    if(strcmp(tok, "in") && strcmp(tok, "out")) return false;
    p.femme = false;
    p.kind = 1;
    while(*s)
    {
        s = tskipword(s, tok, sizeof(tok));
        if(!tok[0]) break;
        if(!strcasecmp(tok, "f") || !strcasecmp(tok, "femme")) p.femme = true;
        else if(!strcasecmp(tok, "exact") || !strcasecmp(tok, "x")) p.kind = 0;
        else if(!strcasecmp(tok, "ex") || !strcasecmp(tok, "example") || !strcasecmp(tok, "shot")) p.kind = 1;
    }
    copystring(p.src, rest, TBUF);
    copystring(p.dst, sep2 + 3, TBUF);
    ttrim(p.src);
    ttrim(p.dst);
    return p.src[0] && p.dst[0];
}

static bool tforgethttp(const char *lang, const char *dir, int femme, const char *src)
{
    char usrc[TBUF*3], pathq[256];
    size_t nsrc = encodeutf8((uchar *)usrc, sizeof(usrc)-1, (const uchar *)(src ? src : ""), src ? strlen(src) : 0);
    usrc[nsrc] = 0;
    nformatstring(pathq, sizeof(pathq), "/unlearn?lang=%s&dir=%s&genre=%s", lang ? lang : "fr", dir ? dir : "in", femme ? "f" : "");
    char resp[1024];
    return tplainhttp("POST", pathq, usrc, (int)nsrc, resp, sizeof(resp), 2000);
}

static const char *texamplecurlang()
{
    return tlangcopy(texamplelang && texamplelang[0] ? texamplelang : tlangin(), tlangin());
}

static void texamplesfetch(bool force)
{
    if(!force && texample_failms && totalmillis - texample_failms < 2000) return;

    const char *lang = texamplecurlang();
    const char *dir = texampledir ? "out" : "in";
    if(!texamplelang[0]) setsvar("texamplelang", lang);

    char pathq[256];
    nformatstring(pathq, sizeof(pathq), "/examples?lang=%s&dir=%s&genre=%s", lang, dir, translatefemme ? "f" : "");
    static char resp[TEXAMPLE_RESP];
    bool ok = tplainhttp("GET", pathq, NULL, 0, resp, sizeof(resp), 2000);
    texamplelist.setsize(0);
    texamplecount = 0;
    texampletotal = 0;
    texample_shots = 0;
    texample_exacts = 0;
    texample_ok = ok;
    if(!ok)
    {
        texample_failms = totalmillis;
        texample_stale = false;
        return;
    }
    texample_failms = 0;

    char *hdrend = strstr(resp, "\r\n\r\n");
    int hdrcount = thdrint(resp, hdrend, "X-Count:", -1);
    texample_shots = thdrint(resp, hdrend, "X-Shots:", -1);
    texample_exacts = thdrint(resp, hdrend, "X-Exact:", -1);

    const char *raw = hdrend ? hdrend + 4 : "";
    static char cube[TEXAMPLE_RESP];
    size_t n = decodeutf8((uchar *)cube, sizeof(cube)-1, (const uchar *)raw, strlen(raw));
    cube[n] = 0;

    vector<texamplepair> all;
    char *p = cube;
    while(*p)
    {
        char *nl = strpbrk(p, "\r\n");
        if(nl) *nl = 0;
        texamplepair item;
        if(texampleparseitem(p, item)) all.add(item);
        if(!nl) break;
        p = nl + 1;
        while(*p=='\r' || *p=='\n') p++;
    }

    texampletotal = hdrcount >= 0 ? hdrcount : all.length();
    if(texample_shots < 0 || texample_exacts < 0)
    {
        texample_shots = texample_exacts = 0;
        loopv(all)
        {
            if(all[i].kind) texample_shots++;
            else texample_exacts++;
        }
    }
    loopvrev(all)
    {
        if(texamplelist.length() >= TEXAMPLE_MENU_MAX) break;
        texamplelist.add(all[i]);
    }
    texamplecount = texamplelist.length();
    copystring(texample_fetched_lang, lang, sizeof(texample_fetched_lang));
    texample_fetched_dir = texampledir;
    texample_fetched_femme = translatefemme;
    texample_stale = false;
}

static void texamplesync()
{
    static int passms = -1;
    if(passms == totalmillis) return;
    passms = totalmillis;
    const char *lang = texamplecurlang();
    if(!texample_stale
        && texample_ok
        && texample_fetched_femme == translatefemme
        && texample_fetched_dir == texampledir
        && !strcmp(texample_fetched_lang, lang))
        return;
    texamplesfetch(false);
}

static void texamplesave()
{
    char src[TBUF], dst[TBUF];
    filtertext(src, texamplesrc ? texamplesrc : "", true, true, TBUF-1);
    filtertext(dst, texampledst ? texampledst : "", true, true, TBUF-1);
    ttrim(src);
    ttrim(dst);
    if(!src[0] || !dst[0])
    {
        conoutf("\f4type the original phrase and how it should read, then save");
        return;
    }
    const char *lang = texamplecurlang();
    const char *dir = texampledir ? "out" : "in";
    const char *teach = dst;
    if(tsameish(dst, src)) teach = "=";
    if(!tlearnhttp(lang, dir, translatefemme, src, teach, texamplekind))
    {
        conoutf("\f4could not save the example (is the translation service running?)");
        return;
    }
    if(tsameish(teach, "=")) conoutf("saved: leave untranslated (%s)", src);
    else if(texamplekind) conoutf("saved as few-shot example: %s -> %s", src, dst);
    else conoutf("saved as exact translation: %s -> %s", src, dst);
    setsvar("texamplesrc", "");
    setsvar("texampledst", "");
    texample_stale = true;
    texamplesfetch(true);
}

static void texampleforgetidx(int i)
{
    if(!texamplelist.inrange(i))
    {
        conoutf("\f4no example %d", i+1);
        return;
    }
    texamplepair p = texamplelist[i];
    const char *lang = texamplecurlang();
    const char *dir = texampledir ? "out" : "in";
    if(!tforgethttp(lang, dir, p.femme ? 1 : 0, p.src))
    {
        conoutf("\f4could not forget that example (is the translation service running?)");
        return;
    }
    conoutf("forgot: %s", p.src);
    texample_stale = true;
    texamplesfetch(true);
}

static void texampleforgetsrc()
{
    char src[TBUF];
    filtertext(src, texamplesrc ? texamplesrc : "", true, true, TBUF-1);
    ttrim(src);
    if(!src[0])
    {
        conoutf("\f4type the original phrase to forget, or use forget on a line");
        return;
    }
    const char *lang = texamplecurlang();
    const char *dir = texampledir ? "out" : "in";
    if(!tforgethttp(lang, dir, translatefemme, src))
    {
        conoutf("\f4could not forget that example (is the translation service running?)");
        return;
    }
    conoutf("forgot: %s", src);
    setsvar("texamplesrc", "");
    texample_stale = true;
    texamplesfetch(true);
}

static void texampleload(int i)
{
    if(!texamplelist.inrange(i)) return;
    setsvar("texamplesrc", texamplelist[i].src);
    setsvar("texampledst", texamplelist[i].dst);
    texamplekind = texamplelist[i].kind ? 1 : 0;
}

static void texamplelinecmd(int i)
{
    if(!texamplelist.inrange(i)) { result(""); return; }
    texamplepair &p = texamplelist[i];
    char asrc[40];
    char adst[40];
    texampleclip(asrc, sizeof(asrc), p.src, 36);
    texampleclip(adst, sizeof(adst), p.dst, 36);
    static char buf[160];
    const char *k = p.kind ? "Example" : "Exact";
    if(p.femme) nformatstring(buf, sizeof(buf), "%s f | %s  ->  %s", k, asrc, adst);
    else nformatstring(buf, sizeof(buf), "%s | %s  ->  %s", k, asrc, adst);
    result(buf);
}

static void texamplestatuscmd()
{
    static char buf[256];
    if(!texample_ok)
    {
        result("translation service busy or not running (start the game with L4ZY.exe)");
        return;
    }
    if(texampletotal <= 0)
    {
        result("none for this language yet");
        return;
    }
    if(texampletotal > texamplelist.length())
        nformatstring(buf, sizeof(buf), "%d exact, %d few-shots (all shots go to the AI). showing %d newest", texample_exacts, texample_shots, texamplelist.length());
    else
        nformatstring(buf, sizeof(buf), "%d exact, %d few-shots sent to the AI", texample_exacts, texample_shots);
    result(buf);
}

enum { TMODEL_CUSTOM_MAX = 12, TMODEL_RESP = 8192 };

struct tmodelcustom
{
    char name[128];
    char size[16];
    char state[16];
};
static tmodelcustom tmodelcustoms[TMODEL_CUSTOM_MAX];
static int tmodel_customn = 0;
static char tmodel_backend[16];
static char tmodel_active[80];
static char tmodel_msg[256] = "loading...";
static char tmodel_savedurl[MAXSTRLEN];
static char tmodel_savedmodel[80];
static char tmodel_small[16];
static char tmodel_normal[16];
static char tmodel_large[16];
static char tmodel_smallabout[96] = "Qwen3-4B ~2.5 GB";
static char tmodel_normalabout[96] = "Qwen3-8B ~5 GB";
static char tmodel_largeabout[96] = "Qwen3-14B ~9 GB";
static int tmodel_progress = 0, tmodel_api = 0, tmodel_failms = 0, tmodel_fetchedms = 0;
static bool tmodel_ok = false, tmodel_stale = true, tmodel_url_loaded = false;
static bool tmodel_busy = false, tmodel_have_resp = false, tmodel_resp_ok = false, tmodel_queued = false, tmodel_last_post = false;
static char tmodel_work_method[8], tmodel_work_path[256], tmodel_work_body[2048];
static int tmodel_work_bodylen = 0;
static char tmodel_qmethod[8], tmodel_qpath[256], tmodel_qbody[2048];
static int tmodel_qbodylen = 0;
static char tmodel_resp[TMODEL_RESP];
static SDL_Thread *tmodel_th = NULL;

static void tmodelcopyascii(char *dst, int dstlen, const char *src)
{
    int n = 0;
    for(; src && *src && n < dstlen-1; src++)
    {
        uchar c = (uchar)*src;
        if(c < 32 || c == 127) continue;
        dst[n++] = char(c);
    }
    dst[n] = 0;
}

static void tmodelresetcustoms()
{
    tmodel_customn = 0;
    tmodelcustomcount = 0;
    loopi(TMODEL_CUSTOM_MAX)
    {
        tmodelcustoms[i].name[0] = 0;
        tmodelcustoms[i].size[0] = 0;
        tmodelcustoms[i].state[0] = 0;
    }
}

static void tmodelsetkv(const char *k, const char *v)
{
    if(!k || !v) return;
    if(!strcmp(k, "backend")) copystring(tmodel_backend, v, sizeof(tmodel_backend));
    else if(!strcmp(k, "active")) copystring(tmodel_active, v, sizeof(tmodel_active));
    else if(!strcmp(k, "msg")) copystring(tmodel_msg, v, sizeof(tmodel_msg));
    else if(!strcmp(k, "apiurl")) copystring(tmodel_savedurl, v, sizeof(tmodel_savedurl));
    else if(!strcmp(k, "apimodel")) copystring(tmodel_savedmodel, v, sizeof(tmodel_savedmodel));
    else if(!strcmp(k, "small")) copystring(tmodel_small, v, sizeof(tmodel_small));
    else if(!strcmp(k, "normal")) copystring(tmodel_normal, v, sizeof(tmodel_normal));
    else if(!strcmp(k, "large")) copystring(tmodel_large, v, sizeof(tmodel_large));
    else if(!strcmp(k, "smallabout")) copystring(tmodel_smallabout, v, sizeof(tmodel_smallabout));
    else if(!strcmp(k, "normalabout")) copystring(tmodel_normalabout, v, sizeof(tmodel_normalabout));
    else if(!strcmp(k, "largeabout")) copystring(tmodel_largeabout, v, sizeof(tmodel_largeabout));
    else if(!strcmp(k, "progress")) tmodel_progress = clamp(atoi(v), 0, 100);
    else if(!strcmp(k, "api") || !strcmp(k, "apikey")) tmodel_api = atoi(v) ? 1 : 0;
    else if(!strcmp(k, "customcount")) tmodelcustomcount = clamp(atoi(v), 0, (int)TMODEL_CUSTOM_MAX);
    else if(!strncmp(k, "custom", 6) && isdigit((uchar)k[6]))
    {
        const char *p = k+6;
        int i = 0;
        while(isdigit((uchar)*p)) i = i*10 + (*p++ - '0');
        if(i < 0 || i >= TMODEL_CUSTOM_MAX) return;
        if(!*p)
        {
            copystring(tmodelcustoms[i].name, v, sizeof(tmodelcustoms[i].name));
            if(i+1 > tmodel_customn) tmodel_customn = i+1;
        }
        else if(!strcmp(p, "size")) copystring(tmodelcustoms[i].size, v, sizeof(tmodelcustoms[i].size));
        else if(!strcmp(p, "state")) copystring(tmodelcustoms[i].state, v, sizeof(tmodelcustoms[i].state));
    }
}

static void tmodelconerr(const char *resp)
{
    if(!resp || !resp[0] || strncmp(resp, "HTTP/1.", 7))
    {
        conoutf("model menu: translation service busy or not running (start the game with L4ZY.exe)");
        return;
    }
    char *start = strstr(resp, "\r\n\r\n");
    const char *raw = start ? start + 4 : resp;
    char cube[512];
    size_t n = decodeutf8((uchar *)cube, sizeof(cube)-1, (const uchar *)raw, strlen(raw));
    cube[n] = 0;
    char *eol = strpbrk(cube, "\r\n");
    if(eol) *eol = 0;
    ttrim(cube);
    if(cube[0]) conoutf("model menu: %s", cube);
    else conoutf("model menu: request failed");
}

static void tmodelparse(const char *resp)
{
    tmodel_ok = true;
    tmodel_failms = 0;
    tmodel_fetchedms = totalmillis;
    tmodel_stale = false;
    tmodelresetcustoms();

    char *hdrend = strstr(resp, "\r\n\r\n");
    const char *raw = hdrend ? hdrend + 4 : "";
    static char cube[TMODEL_RESP];
    size_t n = decodeutf8((uchar *)cube, sizeof(cube)-1, (const uchar *)raw, strlen(raw));
    cube[n] = 0;

    char *p = cube;
    while(*p)
    {
        char *nl = strpbrk(p, "\r\n");
        if(nl) *nl = 0;
        char *eq = strchr(p, '=');
        if(eq)
        {
            *eq = 0;
            tmodelsetkv(p, eq+1);
        }
        if(!nl) break;
        p = nl + 1;
        while(*p=='\r' || *p=='\n') p++;
    }
    if(tmodelcustomcount > tmodel_customn) tmodelcustomcount = tmodel_customn;
    else if(tmodel_customn && tmodelcustomcount <= 0) tmodelcustomcount = tmodel_customn;

    if(!tmodel_url_loaded)
    {
        if(tmodel_savedurl[0]) setsvar("tmodelurl", tmodel_savedurl);
        if(tmodel_savedmodel[0]) setsvar("tmodelapimodel", tmodel_savedmodel);
        tmodel_url_loaded = true;
    }
}

static int SDLCALL tmodelworker(void *)
{
    char method[8], path[256], body[2048], resp[TMODEL_RESP];
    int bodylen;
    tlock();
    copystring(method, tmodel_work_method, sizeof(method));
    copystring(path, tmodel_work_path, sizeof(path));
    bodylen = tmodel_work_bodylen;
    if(bodylen > 0)
    {
        if(bodylen > (int)sizeof(body)) bodylen = (int)sizeof(body);
        memcpy(body, tmodel_work_body, bodylen);
    }
    tunlock();

    bool ok = tplainhttp(method, path, bodylen > 0 ? body : NULL, bodylen, resp, sizeof(resp), 800);

    tlock();
    copystring(tmodel_resp, resp, sizeof(tmodel_resp));
    tmodel_resp_ok = ok;
    tmodel_have_resp = true;
    tmodel_busy = false;
    tunlock();
    return 0;
}

static void tmodelkick(const char *method, const char *pathq, const char *body, int bodylen)
{
    tlock();
    if(tmodel_busy)
    {
        if(method && !strcmp(method, "POST"))
        {
            copystring(tmodel_qmethod, method, sizeof(tmodel_qmethod));
            copystring(tmodel_qpath, pathq ? pathq : "", sizeof(tmodel_qpath));
            tmodel_qbodylen = 0;
            if(body && bodylen > 0)
            {
                tmodel_qbodylen = bodylen < (int)sizeof(tmodel_qbody) ? bodylen : (int)sizeof(tmodel_qbody);
                memcpy(tmodel_qbody, body, tmodel_qbodylen);
            }
            tmodel_queued = true;
        }
        tunlock();
        return;
    }
    copystring(tmodel_work_method, method ? method : "GET", sizeof(tmodel_work_method));
    copystring(tmodel_work_path, pathq ? pathq : "/models", sizeof(tmodel_work_path));
    tmodel_work_bodylen = 0;
    if(body && bodylen > 0)
    {
        tmodel_work_bodylen = bodylen < (int)sizeof(tmodel_work_body) ? bodylen : (int)sizeof(tmodel_work_body);
        memcpy(tmodel_work_body, body, tmodel_work_bodylen);
    }
    tmodel_last_post = method && !strcmp(method, "POST");
    tmodel_busy = true;
    tunlock();

    if(tmodel_th)
    {
        SDL_WaitThread(tmodel_th, NULL);
        tmodel_th = NULL;
    }
    tmodel_th = SDL_CreateThread(tmodelworker, "tmodel", NULL);
    if(!tmodel_th)
    {
        tlock();
        tmodel_busy = false;
        tunlock();
        copystring(tmodel_msg, "could not start model request", sizeof(tmodel_msg));
    }
}

static void tmodelsync()
{
    static int passms = -1;
    if(passms == totalmillis) return;
    passms = totalmillis;

    static char local[TMODEL_RESP];
    bool have = false, ok = false, busy = false;
    bool queued = false;
    char qmethod[8], qpath[256], qbody[2048];
    int qlen = 0;

    tlock();
    if(tmodel_have_resp)
    {
        copystring(local, tmodel_resp, sizeof(local));
        ok = tmodel_resp_ok;
        tmodel_have_resp = false;
        have = true;
    }
    busy = tmodel_busy;
    if(!busy && tmodel_queued)
    {
        copystring(qmethod, tmodel_qmethod, sizeof(qmethod));
        copystring(qpath, tmodel_qpath, sizeof(qpath));
        qlen = tmodel_qbodylen;
        if(qlen > 0) memcpy(qbody, tmodel_qbody, qlen < (int)sizeof(qbody) ? qlen : (int)sizeof(qbody));
        tmodel_queued = false;
        queued = true;
    }
    tunlock();

    if(have)
    {
        if(tmodel_th)
        {
            SDL_WaitThread(tmodel_th, NULL);
            tmodel_th = NULL;
        }
        if(ok) tmodelparse(local);
        else
        {
            tmodel_ok = false;
            tmodel_failms = totalmillis;
            tmodel_fetchedms = totalmillis;
            tmodel_stale = false;
            copystring(tmodel_msg, "translation service busy or not running (start the game with L4ZY.exe)", sizeof(tmodel_msg));
            if(tmodel_last_post) tmodelconerr(local);
        }
        tmodel_last_post = false;
        busy = false;
    }

    if(queued)
    {
        tmodelkick(qmethod, qpath, qlen > 0 ? qbody : NULL, qlen);
        return;
    }

    if(busy) return;

    int interval = 2000;
    if(tmodel_ok && (!strcmp(tmodel_backend, "downloading") || !strcmp(tmodel_backend, "loading")))
        interval = 500;
    if(!tmodel_stale && tmodel_ok && totalmillis - tmodel_fetchedms < interval) return;
    if(tmodel_failms && totalmillis - tmodel_failms < 3000) return;
    tmodelkick("GET", "/models", NULL, 0);
}

static void tmodelpost(const char *pathq, const char *body)
{
    char utf[2048];
    int bodylen = 0;
    const char *send = NULL;
    if(body && body[0])
    {
        size_t n = encodeutf8((uchar *)utf, sizeof(utf)-1, (const uchar *)body, strlen(body));
        utf[n] = 0;
        send = utf;
        bodylen = (int)n;
    }
    copystring(tmodel_msg, "working...", sizeof(tmodel_msg));
    tmodelkick("POST", pathq, send, bodylen);
}

static bool tmodelofficial(const char *id)
{
    return id && (!strcmp(id, "small") || !strcmp(id, "normal") || !strcmp(id, "large"));
}

static const char *tmodelstateof(const char *id)
{
    if(!id) return "missing";
    if(!strcmp(id, "small")) return tmodel_small[0] ? tmodel_small : "missing";
    if(!strcmp(id, "normal")) return tmodel_normal[0] ? tmodel_normal : "missing";
    if(!strcmp(id, "large")) return tmodel_large[0] ? tmodel_large : "missing";
    return "missing";
}

static const char *tmodelbtnof(const char *id)
{
    const char *st = tmodelstateof(id);
    if(!strcmp(st, "downloading")) return "Installing...";
    if(!strcmp(st, "active")) return "In Use";
    if(!strcmp(st, "installed")) return "Use";
    return "Install";
}

static void tmodelinstallcmd(const char *id)
{
    if(!tmodelofficial(id)) return;
    char pathq[64];
    nformatstring(pathq, sizeof(pathq), "/models/install?id=%s", id);
    tmodelpost(pathq, NULL);
}

static void tmodelusecmd(const char *id)
{
    if(!tmodelofficial(id)) return;
    char pathq[64];
    nformatstring(pathq, sizeof(pathq), "/models/activate?id=%s", id);
    tmodelpost(pathq, NULL);
}

static void tmodelclickcmd(const char *id)
{
    const char *st = tmodelstateof(id);
    if(!strcmp(st, "installed")) tmodelusecmd(id);
    else if(strcmp(st, "active")) tmodelinstallcmd(id);
}

static void tmodeldeletecmd(const char *id)
{
    if(!tmodelofficial(id)) return;
    char pathq[64];
    nformatstring(pathq, sizeof(pathq), "/models/delete?id=%s", id);
    tmodelpost(pathq, NULL);
}

static void tmodelsaveapi()
{
    char url[MAXSTRLEN], key[MAXSTRLEN], model[80], body[MAXSTRLEN*2 + 96];
    tmodelcopyascii(url, sizeof(url), tmodelurl);
    tmodelcopyascii(key, sizeof(key), tmodelkey);
    tmodelcopyascii(model, sizeof(model), tmodelapimodel);
    nformatstring(body, sizeof(body), "%s\n%s\n%s", url, model, key);
    tmodelpost("/models/api", body);
    setsvar("tmodelkey", "");
    conoutf("API saved. chat will be sent to the internet until you clear the key.");
}

static void tmodelclearapi()
{
    tmodelpost("/models/clearapi", NULL);
    setsvar("tmodelkey", "");
    conoutf("API key cleared. using the local model.");
}

static void tmodelcustomuse(int i)
{
    if(i < 0 || i >= tmodel_customn || !tmodelcustoms[i].name[0]) return;
    char enc[400], pathq[512];
    turlenc(tmodelcustoms[i].name, enc, sizeof(enc));
    nformatstring(pathq, sizeof(pathq), "/models/activate?id=custom&name=%s", enc);
    tmodelpost(pathq, NULL);
}

static void tmodelcustomdel(int i)
{
    if(i < 0 || i >= tmodel_customn || !tmodelcustoms[i].name[0]) return;
    char enc[400], pathq[512];
    turlenc(tmodelcustoms[i].name, enc, sizeof(enc));
    nformatstring(pathq, sizeof(pathq), "/models/delete?id=custom&name=%s", enc);
    tmodelpost(pathq, NULL);
}

enum { TUPDATE_RESP = 4096 };

// Mises a jour L4ZY : le service de CETTE installation les recherche,
// telecharge et verifie ; le lanceur les applique une fois le jeu ferme.
// Ici : affichage et actions explicites du joueur, rien d'automatique.
static char tupdate_state[16] = "ok";
static char tupdate_ver[32];
static char tupdate_installed[32];
static char tupdate_channel[16];
static char tupdate_size[24];
static char tupdate_msg[256];
static char tupdate_pending[128];
static int tupdate_progress = 0, tupdate_failms = 0, tupdate_fetchedms = 0;
static bool tupdate_ok = false, tupdate_stale = true, tupdate_busy = false, tupdate_have_resp = false, tupdate_resp_ok = false, tupdate_last_post = false, tupdate_announced = false;
static bool tupdate_restart = false, tupdate_wantrestart = false, tupdate_quitting = false, tupdate_reportcheck = false;
static char tupdate_work_method[8], tupdate_work_path[256], tupdate_resp[TUPDATE_RESP];
static SDL_Thread *tupdate_th = NULL;

static void tupdatesetkv(const char *k, const char *v)
{
    if(!k || !v) return;
    if(!strcmp(k, "state")) copystring(tupdate_state, v, sizeof(tupdate_state));
    else if(!strcmp(k, "ver") || !strcmp(k, "remote")) copystring(tupdate_ver, v, sizeof(tupdate_ver));
    else if(!strcmp(k, "installed")) copystring(tupdate_installed, v, sizeof(tupdate_installed));
    else if(!strcmp(k, "channel")) copystring(tupdate_channel, v, sizeof(tupdate_channel));
    else if(!strcmp(k, "size")) copystring(tupdate_size, v, sizeof(tupdate_size));
    else if(!strcmp(k, "restart")) tupdate_restart = atoi(v) != 0;
    else if(!strcmp(k, "msg")) tmodelcopyascii(tupdate_msg, sizeof(tupdate_msg), v);
    else if(!strcmp(k, "notes") && !tupdate_msg[0]) tmodelcopyascii(tupdate_msg, sizeof(tupdate_msg), v);
    else if(!strcmp(k, "progress")) tupdate_progress = clamp(atoi(v), 0, 100);
    else if(!strcmp(k, "avail")) tupdateavail = clamp(atoi(v), 0, 3);
}

static void tupdateparse(const char *resp)
{
    tupdate_ok = true;
    tupdate_failms = 0;
    tupdate_fetchedms = totalmillis;
    tupdate_stale = false;
    tupdate_state[0] = 0;
    tupdate_ver[0] = 0;
    tupdate_size[0] = 0;
    tupdate_msg[0] = 0;
    tupdate_progress = 0;
    tupdate_restart = false;
    tupdateavail = 0;

    char *hdrend = strstr(resp, "\r\n\r\n");
    const char *raw = hdrend ? hdrend + 4 : "";
    static char cube[TUPDATE_RESP];
    size_t n = decodeutf8((uchar *)cube, sizeof(cube)-1, (const uchar *)raw, strlen(raw));
    cube[n] = 0;

    char *p = cube;
    while(*p)
    {
        char *nl = strpbrk(p, "\r\n");
        if(nl) *nl = 0;
        char *eq = strchr(p, '=');
        if(eq)
        {
            *eq = 0;
            tupdatesetkv(p, eq+1);
        }
        if(!nl) break;
        p = nl + 1;
        while(*p=='\r' || *p=='\n') p++;
    }
    if(!tupdate_state[0]) copystring(tupdate_state, "ok", sizeof(tupdate_state));
    if(tupdateavail == 1 && !tupdate_announced && tupdate_ver[0])
    {
        tupdate_announced = true;
        conoutf("\f0L4ZY %s is available - Esc, then Updates", tupdate_ver);
    }
    if(tupdate_reportcheck && strcmp(tupdate_state, "checking"))
    {
        tupdate_reportcheck = false;
        if(tupdate_msg[0]) conoutf("updates: %s", tupdate_msg);
    }
}

static int SDLCALL tupdateworker(void *)
{
    char method[8], path[256], resp[TUPDATE_RESP];
    tlock();
    copystring(method, tupdate_work_method, sizeof(method));
    copystring(path, tupdate_work_path, sizeof(path));
    tunlock();

    // Une recherche explicite interroge Internet : on lui laisse le temps.
    bool post = !strcmp(method, "POST");
    bool ok = tplainhttp(method, path, NULL, 0, resp, sizeof(resp), post ? 45000 : 2000);

    tlock();
    copystring(tupdate_resp, resp, sizeof(tupdate_resp));
    tupdate_resp_ok = ok;
    tupdate_have_resp = true;
    tupdate_busy = false;
    tunlock();
    return 0;
}

static bool tupdatekick(const char *method, const char *pathq)
{
    tlock();
    if(tupdate_busy)
    {
        tunlock();
        return false;
    }
    copystring(tupdate_work_method, method ? method : "GET", sizeof(tupdate_work_method));
    copystring(tupdate_work_path, pathq ? pathq : "/update", sizeof(tupdate_work_path));
    tupdate_last_post = method && !strcmp(method, "POST");
    tupdate_busy = true;
    tunlock();

    if(tupdate_th)
    {
        SDL_WaitThread(tupdate_th, NULL);
        tupdate_th = NULL;
    }
    tupdate_th = SDL_CreateThread(tupdateworker, "tupdate", NULL);
    if(!tupdate_th)
    {
        tlock();
        tupdate_busy = false;
        tunlock();
        copystring(tupdate_msg, "could not start update request", sizeof(tupdate_msg));
        return false;
    }
    return true;
}

static void tupdatepath(char *dst, int dstlen)
{
    nformatstring(dst, dstlen, "/update?proto=2&local=%s", SAUER_TRAD_VERSION);
}

// Les clics sont mis en file : une requete de fond en cours ne les perd pas.
static void tupdatepost(const char *action)
{
    nformatstring(tupdate_pending, sizeof(tupdate_pending), "/update?proto=2&action=%s", action);
}

static void tupdatesync()
{
    static int passms = -1;
    if(passms == totalmillis) return;
    passms = totalmillis;

    static char local[TUPDATE_RESP];
    bool have = false, ok = false, busy = false, waspost = false;

    tlock();
    if(tupdate_have_resp)
    {
        copystring(local, tupdate_resp, sizeof(local));
        ok = tupdate_resp_ok;
        tupdate_have_resp = false;
        have = true;
    }
    busy = tupdate_busy;
    tunlock();

    if(have)
    {
        if(tupdate_th)
        {
            SDL_WaitThread(tupdate_th, NULL);
            tupdate_th = NULL;
        }
        waspost = tupdate_last_post;
        if(ok) tupdateparse(local);
        else
        {
            tupdate_ok = false;
            tupdate_failms = totalmillis;
            tupdate_fetchedms = totalmillis;
            tupdate_stale = false;
            tupdate_quitting = false;
            if(waspost)
            {
                const char *body = strstr(local, "\r\n\r\n");
                if(tupdateavail == 2) tupdateavail = 1;
                if(!strncmp(local, "HTTP/1.", 7) && body && body[4])
                    tmodelcopyascii(tupdate_msg, sizeof(tupdate_msg), body + 4);
                else
                    copystring(tupdate_msg, "update service not running - start the game with L4ZY.exe", sizeof(tupdate_msg));
                conoutf("updates: %s", tupdate_msg);
            }
        }
        tupdate_last_post = false;
        busy = false;
        // "Redemarrer" confirme par le service : fermeture propre du jeu, le
        // lanceur applique la mise a jour puis relance.
        if(ok && waspost && tupdate_quitting && tupdateavail == 3 && tupdate_restart)
        {
            conoutf("\f0L4ZY: closing to install the update...");
            execute("quit");
            return;
        }
    }

    if(busy) return;

    if(tupdate_pending[0])
    {
        char pathq[128];
        copystring(pathq, tupdate_pending, sizeof(pathq));
        if(tupdatekick("POST", pathq)) tupdate_pending[0] = 0;
        return;
    }

    int interval = 4000;
    if(tupdateavail == 2 || !strcmp(tupdate_state, "checking")) interval = 400;
    if(!tupdate_stale && tupdate_ok && totalmillis - tupdate_fetchedms < interval) return;
    if(tupdate_failms && totalmillis - tupdate_failms < 8000) return;
    char pathq[64];
    tupdatepath(pathq, sizeof(pathq));
    tupdatekick("GET", pathq);
}

// "Update and restart" : telechargement verifie avec progression. Une fois
// pret, le menu Updates redemarre seul si le joueur y attend hors partie ;
// sinon il propose "Restart now". Jamais de fermeture sur simple decouverte.
static void tupdateapply()
{
    if(tupdateavail == 2 || tupdateavail == 3) return;
    tupdate_wantrestart = true;
    copystring(tupdate_msg, "starting download...", sizeof(tupdate_msg));
    tupdateavail = 2;
    tupdatepost("download");
    tupdatesync();
}

static void tupdaterestart()
{
    if(tupdateavail != 3 || tupdate_quitting) return;
    tupdate_quitting = true;
    copystring(tupdate_msg, "closing the game to install the update...", sizeof(tupdate_msg));
    tupdatepost("restart");
    tupdatesync();
}

static void tupdatecheck()
{
    if(tupdateavail == 2 || tupdateavail == 3) return;
    tupdate_reportcheck = true;
    copystring(tupdate_msg, "checking for updates...", sizeof(tupdate_msg));
    copystring(tupdate_state, "checking", sizeof(tupdate_state));
    tupdatepost("check");
    tupdatesync();
}

static const char *tupdatelabelof()
{
    static char buf[128];
    if(tupdateavail == 2)
        nformatstring(buf, sizeof(buf), "downloading %d%%", tupdate_progress);
    else if(tupdateavail == 3)
        copystring(buf, "\f0Restart now to install", sizeof(buf));
    else if(tupdate_ver[0] && tupdate_size[0])
        nformatstring(buf, sizeof(buf), "\f0Update and restart  (%s, %s)", tupdate_ver, tupdate_size);
    else if(tupdate_ver[0])
        nformatstring(buf, sizeof(buf), "\f0Update and restart  (%s)", tupdate_ver);
    else
        copystring(buf, "\f0Update and restart", sizeof(buf));
    return buf;
}


static int SDLCALL tworker(void *data)
{
    tjob *j = (tjob *)data;
    char out[TBUF], detected[16], srclang[16];
    detected[0] = srclang[0] = 0;
    bool ok = thttp(j->lang, j->speaker, j->outgoing ? "out" : "in", j->team ? 1 : 0, j->preview ? 1 : 0, j->femme, j->reply ? 1 : 0, j->force ? 1 : 0, j->board ? 1 : 0, j->last, j->lastspk, j->original, out, sizeof(out), detected, sizeof(detected), srclang, sizeof(srclang));
    tlock();
    j->ok = ok;
    if(ok) copystring(j->result, out, TBUF);
    else j->result[0] = 0;
    if(ok && detected[0]) copystring(j->detected, detected, sizeof(j->detected));
    if(ok && srclang[0]) copystring(j->srclang, srclang, sizeof(j->srclang));
    j->done = true;
    tunlock();
    return 0;
}

static bool tcooling()
{
    return tcooloff && totalmillis < tcooloff;
}

static void tnoteconn(bool ok)
{
    if(ok) { tfailstreak = 0; tcooloff = 0; return; }
    tfailstreak++;
    if(tfailstreak >= 2) tcooloff = totalmillis + 15000;
}

static bool tqueue(tjob *j)
{
    if(tjobs.length() >= TMAXJOBS) return false;
    j->done = j->ok = false;
    j->result[0] = 0;
    tlock();
    tjobs.add(j);
    tunlock();
    SDL_Thread *th = SDL_CreateThread(tworker, "translate", j);
    if(!th)
    {
        tlock();
        tjobs.removeobj(j);
        tunlock();
        return false;
    }
    SDL_DetachThread(th);
    return true;
}

static void trememberfix(bool outgoing, int uid, bool team, const char *name, const char *original, const char *shown, const char *lang, const char *mention, int femme)
{
    tfixline &L = outgoing ? tlast_out_fix : tlast_in_fix;
    L.used = true;
    L.outgoing = outgoing;
    L.team = team;
    L.uid = uid;
    L.femme = femme;
    copystring(L.name, name ? name : "", MAXSTRLEN);
    copystring(L.original, original ? original : "", TBUF);
    copystring(L.shown, shown ? shown : "", TBUF);
    copystring(L.lang, lang ? lang : "", sizeof(L.lang));
    copystring(L.mention, mention ? mention : "", sizeof(L.mention));
}

static void tretryhistput(const tretryline &L)
{
    if(!L.used || !L.uid) return;
    loopv(tretryhist) if(tretryhist[i].uid == L.uid) { tretryhist[i] = L; return; }
    tretryhist.add(L);
}

static tretryline *tretryhistfind(int uid)
{
    if(!uid) return NULL;
    loopvrev(tretryhist) if(tretryhist[i].uid == uid) return &tretryhist[i];
    return NULL;
}

void chatlogdropped(int uid)
{
    loopvrev(tretryhist) if(tretryhist[i].uid == uid) tretryhist.remove(i);
    if(tlast_retry.uid == uid) tlast_retry.uid = 0;
}

void chatlogcleared()
{
    tretryhist.setsize(0);
}

static void trememberretry(bool serv, int uid, bool team, const char *name, const char *speaker, const char *original, const char *mention, const char *lang, int femme)
{
    tretryline &L = serv ? tlast_retry_serv : tlast_retry;
    L.used = true;
    L.serv = serv;
    L.team = team;
    L.own = false;
    L.uid = uid;
    L.femme = femme;
    copystring(L.name, name ? name : "", MAXSTRLEN);
    copystring(L.speaker, speaker ? speaker : "", MAXSTRLEN);
    copystring(L.original, original ? original : "", TBUF);
    copystring(L.mention, mention ? mention : "", sizeof(L.mention));
    copystring(L.lang, lang ? lang : "", sizeof(L.lang));
    if(!serv) tretryhistput(L);
}

static void tshowpair(int uid, bool team, const char *name, const char *trad, const char *original, const char *mention)
{
    char shown[TBUF], origfull[TBUF];
    tjoinmention(mention, trad && trad[0] ? trad : original, shown, sizeof(shown));
    tjoinmention(mention, original, origfull, sizeof(origfull));
    bool skip = !trad || !trad[0] || trad[0]=='=' || tsameish(trad, original);
    const char *body = skip ? origfull : shown;
    if(skip)
    {
        if(team) conreplaceid(uid, CON_TEAMCHAT, "\fs\f8[team]\fr %s: \f8%s", name, chatpaintmention(body, '8'));
        else conreplaceid(uid, CON_CHAT, "%s:\f0 %s", name, chatpaintmention(body, '0'));
    }
    else
    {
        if(team) conreplaceid(uid, CON_TEAMCHAT, "\fs\f8[team]\fr %s: \f8%s \f4(%s)", name, chatpaintmention(shown, '8'), origfull);
        else conreplaceid(uid, CON_CHAT, "%s:\f0 %s \f4(%s)", name, chatpaintmention(shown, '0'), origfull);
    }
}

static void tshowserv(int uid, const char *trad, const char *original)
{
    bool skip = !trad || !trad[0] || trad[0]=='=' || tsameish(trad, original);
    bool replaced = skip ? conreplaceid(uid, CON_INFO, "%s", original)
                         : conreplaceid(uid, CON_INFO, "%s \f4(%s)", trad, original);
    if(!replaced)
    {
        if(skip) conoutf("%s", original);
        else conoutf("%s \f4(%s)", trad, original);
    }
}

static void tshowfinal(tjob *j)
{
    const char *trad = j->result;
    bool skip = !j->ok || !trad[0] || trad[0]=='=' || tsameish(trad, j->original);
    const char *shown = skip ? j->original : trad;
    if(j->serv)
    {
        tshowserv(j->uid, shown, j->original);
        trememberretry(true, j->uid, false, "", "", j->original, "", j->lang, j->femme);
        return;
    }
    if(!j->own)
    {
        trememberfix(false, j->uid, j->team, j->name, j->original, shown, j->lang, j->mention, j->femme);
        trememberretry(false, j->uid, j->team, j->name, j->speaker, j->original, j->mention, j->lang, j->femme);
    }
    tshowpair(j->uid, j->team, j->name, shown, j->original, j->mention);
}

static const char *tjoblang(tjob *j)
{
    return (j->reply && j->detected[0]) ? j->detected : j->lang;
}

static void tsendnow(const char *msg, bool team)
{
    if(team) sayteam(const_cast<char *>(msg));
    else toserver(const_cast<char *>(msg));
}

static const char *tchooseout(const char *original, const char *result, bool ok)
{
    if(!ok || !result[0] || result[0]=='=' || tsameish(result, original)) return original;
    return result;
}

static void tstorepreview(const char *src, const char *lang, const char *dst, bool reply, const char *mention)
{
    copystring(tprev_src, src, TBUF);
    copystring(tprev_lang, lang, sizeof(tprev_lang));
    copystring(tprev_dst, dst, TBUF);
    copystring(tprev_mention, mention ? mention : "", sizeof(tprev_mention));
    tprev_femme = translatefemme;
    tprev_ok = true;
    tprev_reply = reply;
}

static bool tlangtok(const char *s, int n, char *dst, int dstlen)
{
    if(n < 2 || n > 8 || n >= dstlen) return false;
    loopi(n) if(!isalpha((uchar)s[i])) return false;
    loopi(n) dst[i] = char(tolower((uchar)s[i]));
    dst[n] = 0;
    return true;
}

static void ttrim(char *s)
{
    char *a = s;
    while(*a == ' ' || *a == '\t') a++;
    if(a != s) memmove(s, a, strlen(a)+1);
    int n = (int)strlen(s);
    while(n > 0 && (s[n-1]==' ' || s[n-1]=='\t')) s[--n] = 0;
}

static const char *tskipword(const char *s, char *dst, int dstlen)
{
    while(*s == ' ' || *s == '\t') s++;
    int n = 0;
    while(s[n] && s[n] != ' ' && s[n] != '\t' && n < dstlen-1) n++;
    memcpy(dst, s, n);
    dst[n] = 0;
    s += n;
    while(*s == ' ' || *s == '\t') s++;
    return s;
}

static void tlowercmd(char *cmd)
{
    for(; *cmd; cmd++) *cmd = char(tolower((uchar)*cmd));
}

static int tnamecmdkind(const char *cmd, bool *team)
{
    *team = false;
    if(!strcmp(cmd, "tsaytoteam")) { *team = true; return 1; }
    if(!strcmp(cmd, "treptoteam")) { *team = true; return 2; }
    if(!strcmp(cmd, "toteam")) { *team = true; return 3; }
    if(!strcmp(cmd, "saytoteam")) { *team = true; return 0; }
    if(!strcmp(cmd, "tsayto")) return 1;
    if(!strcmp(cmd, "trepto")) return 2;
    if(!strcmp(cmd, "to")) return 3;
    if(!strcmp(cmd, "sayto")) return 0;
    return -1;
}

static int tresolvekind(int kind)
{
    if(kind != 3) return kind;
    if(translateauto >= 2) return 2;
    if(translateauto == 1) return 1;
    return 0;
}

static const char *tkindhint(int kind, int rawkind)
{
    if(rawkind == 3) return "follows auto-translate, @name in front";
    if(kind == 2) return "reply in that player's language";
    if(kind == 1) return "translate using I send in, @name in front";
    return "send as typed, @name in front";
}

static const char *tparsename(const char *s, char *dst, int dstlen)
{
    while(*s == ' ' || *s == '\t') s++;
    int n = 0;
    if(*s == '"')
    {
        s++;
        while(s[n] && s[n] != '"' && n < dstlen-1) n++;
        memcpy(dst, s, n);
        dst[n] = 0;
        s += n;
        if(*s == '"') s++;
    }
    else
    {
        while(s[n] && s[n] != ' ' && s[n] != '\t' && n < dstlen-1) n++;
        memcpy(dst, s, n);
        dst[n] = 0;
        s += n;
    }
    while(*s == ' ' || *s == '\t') s++;
    return s;
}

static const char *tcasestr(const char *hay, const char *needle)
{
    if(!needle || !needle[0]) return hay;
    for(const char *p = hay; *p; p++)
    {
        const char *a = p, *b = needle;
        while(*a && *b && tolower((uchar)*a) == tolower((uchar)*b)) { a++; b++; }
        if(!*b) return p;
    }
    return NULL;
}

static int tnamefit(const char *name, const char *pref)
{
    if(!pref || !pref[0]) return 0;
    int n = (int)strlen(pref);
    if(!strncasecmp(name, pref, n)) return 0;
    const char *br = strrchr(name, ']');
    if(br && br[1] && !strncasecmp(br+1, pref, n)) return 1;
    if(tcasestr(name, pref)) return 2;
    return -1;
}

static int tlastwhen(int cn)
{
    loopv(tspeaks) if(tspeaks[i].cn == cn) return tspeaks[i].when;
    return 0;
}

static void tremember(fpsent *d, const char *text)
{
    if(!d || d == player1 || !text || !text[0]) return;
    copystring(tlast_in_text, text, TBUF);
    copystring(tlast_in_name, d->name, MAXSTRLEN);
    loopv(tspeaks) if(tspeaks[i].cn == d->clientnum)
    {
        tspeaks[i].when = totalmillis;
        copystring(tspeaks[i].name, d->name, MAXNAMELEN+1);
        copystring(tspeaks[i].text, text, TBUF);
        return;
    }
    tspeak &s = tspeaks.add();
    s.cn = d->clientnum;
    s.when = totalmillis;
    copystring(s.name, d->name, MAXNAMELEN+1);
    copystring(s.text, text, TBUF);
}

static bool tlastfor(fpsent *d, char *text, int textlen, char *spk, int spklen)
{
    if(!d) return false;
    loopv(tspeaks) if(tspeaks[i].cn == d->clientnum && tspeaks[i].text[0])
    {
        copystring(text, tspeaks[i].text, textlen);
        copystring(spk, d->name, spklen);
        return true;
    }
    return false;
}

void translategone(int cn)
{
    loopv(tspeaks) if(tspeaks[i].cn == cn) tspeaks.remove(i--);
}

void translateclear()
{
    tspeaks.setsize(0);
    tlangs.setsize(0);
    tlast_in_text[0] = tlast_in_name[0] = 0;
    tlast_retry.used = false;
    tlast_retry_serv.used = false;
    tretryhist.setsize(0);
    tcomp_i = -1;
    tcomp_prefix[0] = tcomp_cmd[0] = 0;
}

static fpsent *tfindplayer(const char *typed, bool *ambiguous)
{
    if(ambiguous) *ambiguous = false;
    if(!typed || !typed[0]) return NULL;
    fpsent *insen = NULL;
    loopv(players)
    {
        fpsent *o = players[i];
        if(!o || o == player1 || !o->name[0]) continue;
        if(!strcmp(o->name, typed)) return o;
        if(!insen && !strcasecmp(o->name, typed)) insen = o;
    }
    if(insen) return insen;
    fpsent *sub = NULL;
    int nsub = 0;
    loopv(players)
    {
        fpsent *o = players[i];
        if(!o || o == player1 || !o->name[0]) continue;
        if(tnamefit(o->name, typed) >= 0) { sub = o; nsub++; }
    }
    if(nsub > 1)
    {
        if(ambiguous) *ambiguous = true;
        return NULL;
    }
    return sub;
}

static void tjoinmention(const char *mention, const char *msg, char *dst, int dstlen)
{
    if(mention && mention[0] && msg && msg[0]) nformatstring(dst, dstlen, "%s %s", mention, msg);
    else if(mention && mention[0]) copystring(dst, mention, dstlen);
    else copystring(dst, msg ? msg : "", dstlen);
}

static void tsendout(const char *msg, bool team, const char *mention)
{
    if(mention && mention[0])
    {
        char out[TBUF];
        tjoinmention(mention, msg, out, sizeof(out));
        tsendnow(out, team);
    }
    else tsendnow(msg, team);
}

static void tsendtranslated(const char *original, const char *sent, bool team, const char *mention, const char *lang)
{
    trememberfix(true, 0, team, player1 ? player1->name : "", original, sent ? sent : original, lang, mention, translatefemme);
    tsendout(sent ? sent : original, team, mention);
}

const char *chatpaintmention(const char *text, char restcolor)
{
    static char buf[2][TBUF];
    static int turn = 0;
    turn ^= 1;
    if(!text || text[0] != '@')
    {
        copystring(buf[turn], text ? text : "", TBUF);
        return buf[turn];
    }
    const char *sp = text + 1;
    while(*sp && *sp != ' ' && *sp != '\t') sp++;
    int n = (int)(sp - text);
    if(n < 2)
    {
        copystring(buf[turn], text, TBUF);
        return buf[turn];
    }
    nformatstring(buf[turn], TBUF, "\f3%.*s\f%c%s", n, text, restcolor, sp);
    return buf[turn];
}

static void tsplitat(const char *text, char *mention, int mlen, char *body, int blen)
{
    mention[0] = 0;
    copystring(body, text ? text : "", blen);
    if(!text || text[0] != '@') return;
    const char *sp = text + 1;
    while(*sp && *sp != ' ' && *sp != '\t') sp++;
    int n = (int)(sp - text);
    if(n < 2 || n >= mlen) return;
    memcpy(mention, text, n);
    mention[n] = 0;
    while(*sp == ' ' || *sp == '\t') sp++;
    if(*sp) copystring(body, sp, blen);
    else body[0] = 0;
}

void translaterecordown(int uid, bool team, const char *text)
{
    if(!uid || !text || !text[0]) return;
    char mention[MAXNAMELEN+8], body[TBUF];
    mention[0] = body[0] = 0;
    tsplitat(text, mention, sizeof(mention), body, sizeof(body));
    const char *forjob = body[0] ? body : text;
    tretryline L;
    memset(&L, 0, sizeof(L));
    L.used = true;
    L.own = true;
    L.team = team;
    L.uid = uid;
    L.femme = translatefemme;
    copystring(L.name, player1 ? chatcolorname(player1) : "", MAXSTRLEN);
    copystring(L.speaker, player1 ? player1->name : "", MAXSTRLEN);
    copystring(L.original, forjob, TBUF);
    if(body[0]) copystring(L.mention, mention, sizeof(L.mention));
    copystring(L.lang, tlangin(), sizeof(L.lang));
    tretryhistput(L);
}

static bool tparsecmd(const char *buf, char *text, int textlen, char *lang, int langlen, bool *team, bool *reply, char *mention, int mentionlen, char *last, int lastlen, char *lastspk, int lastspklen)
{
    text[0] = 0;
    if(mention && mentionlen) mention[0] = 0;
    if(last && lastlen) last[0] = 0;
    if(lastspk && lastspklen) lastspk[0] = 0;
    *team = false;
    *reply = false;
    copystring(lang, tlangout(), langlen);
    if(!buf) return false;

    const char *s = buf;
    while(*s == ' ' || *s == '\t') s++;
    if(!*s) return false;

    if(s[0] != '/')
    {
        if(!translateauto) return false;
        copystring(text, s, textlen);
        ttrim(text);
        *team = commandprompt && strstr(commandprompt, "[team]");
        *reply = tautolast();
        return text[0] != 0;
    }

    s++;
    while(*s == ' ' || *s == '\t') s++;

    char cmd[32];
    s = tskipword(s, cmd, sizeof(cmd));
    tlowercmd(cmd);

    int namekind = tresolvekind(tnamecmdkind(cmd, team));
    if(namekind >= 0)
    {
        if(namekind == 0) return false;
        char typed[MAXNAMELEN+1];
        s = tparsename(s, typed, sizeof(typed));
        if(!typed[0]) return false;
        fpsent *d = tfindplayer(typed, NULL);
        if(!d) return false;
        if(mention && mentionlen) nformatstring(mention, mentionlen, "@%s", d->name);
        copystring(text, s, textlen);
        ttrim(text);
        if(!text[0]) return false;
        if(namekind == 2 && last && lastspk && tlastfor(d, last, lastlen, lastspk, lastspklen))
            *reply = true;
        return true;
    }
    if(!strcmp(cmd, "trepteam") || !strcmp(cmd, "trep"))
    {
        *reply = true;
        *team = !strcmp(cmd, "trepteam");
        copystring(text, s, textlen);
    }
    else if(!strcmp(cmd, "tsayteam") || !strcmp(cmd, "tsay"))
    {
        *team = !strcmp(cmd, "tsayteam");
        copystring(text, s, textlen);
    }
    else if(!strncmp(cmd, "tsayt", 5) && cmd[5])
    {
        if(!tlangtok(cmd+5, (int)strlen(cmd+5), lang, langlen)) return false;
        copystring(text, s, textlen);
        *team = true;
    }
    else if(!strncmp(cmd, "tsay", 4) && cmd[4])
    {
        if(!tlangtok(cmd+4, (int)strlen(cmd+4), lang, langlen)) return false;
        copystring(text, s, textlen);
    }
    else return false;

    ttrim(text);
    return text[0] != 0;
}

static bool tpreviewbusy()
{
    bool busy = false;
    tlock();
    loopv(tjobs) if(tjobs[i]->preview && !tjobs[i]->done) { busy = true; break; }
    tunlock();
    return busy;
}

static bool tmatchjob(tjob *j, const char *text, const char *lang, bool reply, const char *mention)
{
    if(!j->outgoing || j->reply != reply || strcmp(j->original, text) || j->femme != translatefemme) return false;
    if(strcmp(j->mention, mention ? mention : "")) return false;
    if(reply) return true;
    return !strcmp(j->lang, lang);
}

static bool ttakejob(const char *text, const char *lang, bool team, bool reply, const char *mention)
{
    tlock();
    loopv(tjobs)
    {
        tjob *j = tjobs[i];
        if(!tmatchjob(j, text, lang, reply, mention)) continue;
        if(!j->done)
        {
            j->preview = false;
            j->team = team;
            tunlock();
            return true;
        }
        tjobs.remove(i);
        tunlock();
        tnoteconn(j->ok);
        tsendtranslated(j->original, tchooseout(j->original, j->result, j->ok), team, mention, tjoblang(j));
        delete j;
        return true;
    }
    tunlock();
    return false;
}

static void tstart(const char *text, const char *lang, bool team, bool preview, bool reply, const char *mention, const char *last, const char *lastspk)
{
    tjob *j = new tjob;
    memset(j, 0, sizeof(*j));
    j->outgoing = true;
    j->preview = preview;
    j->team = team;
    j->reply = reply;
    copystring(j->name, player1 ? player1->name : "");
    copystring(j->speaker, j->name);
    copystring(j->original, text, TBUF);
    copystring(j->lang, lang, sizeof(j->lang));
    if(mention) copystring(j->mention, mention, sizeof(j->mention));
    if(reply)
    {
        copystring(j->last, last && last[0] ? last : tlast_in_text, TBUF);
        copystring(j->lastspk, lastspk && lastspk[0] ? lastspk : tlast_in_name, MAXSTRLEN);
    }
    j->femme = translatefemme;
    if(!tqueue(j))
    {
        if(!preview) tsendtranslated(text, text, team, mention, lang);
        delete j;
    }
}

static void tpollpreview()
{
    tprev_draw[0] = 0;
    if(!translatepreview) return;
    if(commandmillis < 0) return;

    char text[TBUF], lang[16], mention[MAXNAMELEN+8], last[TBUF], lastspk[MAXSTRLEN];
    bool team = false, reply = false;
    if(!tparsecmd(commandbuf, text, sizeof(text), lang, sizeof(lang), &team, &reply, mention, sizeof(mention), last, sizeof(last), lastspk, sizeof(lastspk))) return;
    if((int)strlen(text) < 3) return;

    if(tprev_ok && !strcmp(text, tprev_src) && tprev_femme == translatefemme && tprev_reply == reply && !strcmp(mention, tprev_mention) && (reply || !strcmp(lang, tprev_lang)))
    {
        if(mention[0]) nformatstring(tprev_draw, sizeof(tprev_draw), "\f4%s: \f3%s\f4 %s", tprev_reply ? (tprev_lang[0] ? tprev_lang : "reply") : lang, mention, tprev_dst);
        else nformatstring(tprev_draw, sizeof(tprev_draw), "\f4%s: %s", tprev_reply ? (tprev_lang[0] ? tprev_lang : "reply") : lang, tprev_dst);
        return;
    }

    if(mention[0]) nformatstring(tprev_draw, sizeof(tprev_draw), "\f4%s: \f3%s\f4 ...", reply ? "reply" : lang, mention);
    else nformatstring(tprev_draw, sizeof(tprev_draw), "\f4%s: ...", reply ? "reply" : lang);

    bool same = !strcmp(text, tprev_watch) && !strcmp(lang, tprev_watchlang) && tprev_watchreply == reply && !strcmp(mention, tprev_watchmention);
    if(!same)
    {
        copystring(tprev_watch, text, TBUF);
        copystring(tprev_watchlang, lang, sizeof(tprev_watchlang));
        copystring(tprev_watchmention, mention, sizeof(tprev_watchmention));
        tprev_watchreply = reply;
        tprev_watchms = totalmillis;
    }
    if(tcooling()) return;
    if(totalmillis - tprev_watchms < translatepreviewdelay) return;
    if(tpreviewbusy()) return;
    if(tjobs.length() >= TMAXJOBS-2) return;

    tlock();
    bool already = false;
    loopv(tjobs) if(tmatchjob(tjobs[i], text, lang, reply, mention)) { already = true; break; }
    tunlock();
    if(already) return;

    tstart(text, lang, team, true, reply, mention, last, lastspk);
}

void polltranslation()
{
    tlock();
    loopv(tjobs)
    {
        tjob *j = tjobs[i];
        if(!j->done) continue;
        tjobs.remove(i--);
        tunlock();
        tnoteconn(j->ok);
        if(!j->own && !j->serv && !j->outgoing && j->srclang[0])
            tlangadd(j->cn, j->speaker, j->srclang);
        if(j->board) { delete j; tlock(); continue; }
        if(j->preview)
        {
            if(j->ok)
            {
                const char *shown = j->reply && j->detected[0] ? j->detected : j->lang;
                tstorepreview(j->original, shown, tchooseout(j->original, j->result, j->ok), j->reply, j->mention);
            }
        }
        else if(j->outgoing) tsendtranslated(j->original, tchooseout(j->original, j->result, j->ok), j->team, j->mention, tjoblang(j));
        else tshowfinal(j);
        delete j;
        tlock();
    }
    tunlock();
    tpollpreview();
    tupdatesync();
}

const char *translatepreviewline()
{
    return tprev_draw[0] ? tprev_draw : NULL;
}

bool translateincoming(fpsent *d, const char *text, bool team)
{
    tremember(d, text);
    char mention[MAXNAMELEN+8], body[TBUF];
    mention[0] = body[0] = 0;
    if(text && text[0]) tsplitat(text, mention, sizeof(mention), body, sizeof(body));
    const char *forjob = body[0] ? body : (text ? text : "");
    bool own = d && d == player1;
    bool mute = team ? !translateteamchat : !translatechat;

    if(mute && !own && d && forjob[0] && !tcooling())
    {
        if(tjobs.length() < TMAXJOBS-2)
        {
            tjob *b = new tjob;
            memset(b, 0, sizeof(*b));
            b->board = true;
            b->cn = d->clientnum;
            copystring(b->speaker, d->name);
            copystring(b->original, forjob, TBUF);
            copystring(b->lang, tlangin(), sizeof(b->lang));
            if(!tqueue(b)) delete b;
        }
    }

    if(!d || !text || !text[0]) return false;

    if(mute || tcooling())
    {
        int uid;
        if(team) uid = conoutfid(CON_TEAMCHAT, "\fs\f8[team]\fr %s: \f8%s", chatcolorname(d), chatpaintmention(text, '8'));
        else uid = conoutfid(CON_CHAT, "%s:\f0 %s", chatcolorname(d), chatpaintmention(text, '0'));
        if(own) translaterecordown(uid, team, text);
        else trememberretry(false, uid, team, chatcolorname(d), d->name, forjob, body[0] ? mention : "", tlangin(), translatefemme);
        return true;
    }

    tjob *j = new tjob;
    memset(j, 0, sizeof(*j));
    j->type = team ? CON_TEAMCHAT : CON_CHAT;
    j->team = team;
    j->outgoing = false;
    j->cn = d->clientnum;
    copystring(j->name, chatcolorname(d));
    copystring(j->speaker, d->name);
    copystring(j->original, forjob, TBUF);
    if(body[0]) copystring(j->mention, mention, sizeof(j->mention));
    copystring(j->lang, tlangin(), sizeof(j->lang));
    j->femme = translatefemme;
    j->own = own;

    if(team) j->uid = conoutfid(CON_TEAMCHAT, "\fs\f8[team]\fr %s: \f4%s", j->name, chatpaintmention(text, '4'));
    else j->uid = conoutfid(CON_CHAT, "%s:\f4 %s", j->name, chatpaintmention(text, '4'));

    if(own) translaterecordown(j->uid, team, text);
    else trememberretry(false, j->uid, j->team, j->name, j->speaker, j->original, j->mention, j->lang, j->femme);

    if(!tqueue(j))
    {
        if(!j->own)
            trememberfix(false, j->uid, j->team, j->name, j->original, j->original, j->lang, j->mention, j->femme);
        if(team) conreplaceid(j->uid, CON_TEAMCHAT, "\fs\f8[team]\fr %s: \f8%s", j->name, chatpaintmention(text, '8'));
        else conreplaceid(j->uid, CON_CHAT, "%s:\f0 %s", j->name, chatpaintmention(text, '0'));
        delete j;
        return true;
    }
    return true;
}

// /tretry is for skipped chat and tagged MOTD / [INFO], not hopmod stats / flagruns / assists.
static bool tservwantretry(const char *plain)
{
    if(!plain || !plain[0]) return false;

    char buf[TBUF];
    copystring(buf, plain, TBUF);
    for(char *p = buf; *p; p++)
        if(*p >= 'A' && *p <= 'Z') *p = char(*p - 'A' + 'a');

    if(strstr(buf, "assisted by") || strstr(buf, "assiste par")) return false;
    if(strstr(buf, "global stats") || strstr(buf, "flagrun") || strstr(buf, "scored a flag")) return false;

    if(strstr(buf, "[info]") || strstr(buf, "[motd]") || strstr(buf, "[announce")
       || strstr(buf, "[broadcast]") || strstr(buf, "[news]") || strstr(buf, "[notice]")
       || strstr(buf, "motd:"))
        return true;

    return false;
}

void translateservmsg(const char *text)
{
    if(!text) text = "";
    char plain[TBUF];
    filtertext(plain, text, true, true, TBUF-1);
    if(plain[0] && tservwantretry(plain))
    {
        int uid = conoutfid(CON_INFO, "%s", text);
        trememberretry(true, uid, false, "", "", plain, "", tlangin(), translatefemme);
        return;
    }
    conoutf("%s", text);
}

static bool tuidbusy(int uid)
{
    if(!uid) return false;
    bool busy = false;
    tlock();
    loopv(tjobs) if(tjobs[i]->uid == uid && !tjobs[i]->done && !tjobs[i]->outgoing) { busy = true; break; }
    tunlock();
    return busy;
}

static void tretryfrom(tretryline src, bool inplace)
{
    if(!src.used || !src.original[0])
    {
        conoutf("/tretry : no chat, MOTD or [INFO] line to retranslate yet");
        return;
    }
    if(tuidbusy(src.uid))
    {
        conoutf("\f4already retranslating that line");
        return;
    }

    tjob *j = new tjob;
    memset(j, 0, sizeof(*j));
    j->force = true;
    j->serv = src.serv;
    j->outgoing = false;
    j->own = src.own;
    j->team = src.team;
    j->type = src.serv ? int(CON_INFO) : (src.team ? int(CON_TEAMCHAT) : int(CON_CHAT));
    j->femme = translatefemme;
    copystring(j->name, src.name);
    copystring(j->speaker, src.speaker);
    copystring(j->original, src.original, TBUF);
    copystring(j->lang, tlangin(), sizeof(j->lang));
    copystring(j->mention, src.mention, sizeof(j->mention));

    char waittext[TBUF];
    tjoinmention(j->mention, src.original, waittext, sizeof(waittext));

    if(src.uid)
    {
        j->uid = src.uid;
        bool replaced;
        if(j->serv) replaced = conreplaceid(j->uid, CON_INFO, "\f4%s", src.original);
        else if(j->team) replaced = conreplaceid(j->uid, CON_TEAMCHAT, "\fs\f8[team]\fr %s: \f4%s", j->name, chatpaintmention(waittext, '4'));
        else replaced = conreplaceid(j->uid, CON_CHAT, "%s:\f4 %s", j->name, chatpaintmention(waittext, '4'));
        if(!replaced) src.uid = 0;
    }
    if(!src.uid)
    {
        if(inplace)
        {
            delete j;
            conoutf("/tretry : that line is no longer in the chat log");
            return;
        }
        if(j->serv) j->uid = conoutfid(CON_INFO, "\f4%s", src.original);
        else if(j->team) j->uid = conoutfid(CON_TEAMCHAT, "\fs\f8[team]\fr %s: \f4%s", j->name, chatpaintmention(waittext, '4'));
        else j->uid = conoutfid(CON_CHAT, "%s:\f4 %s", j->name, chatpaintmention(waittext, '4'));
    }

    src.uid = j->uid;
    if(!src.own)
    {
        if(src.serv) tlast_retry_serv = src;
        else
        {
            tlast_retry = src;
            tretryhistput(tlast_retry);
        }
    }
    else tretryhistput(src);

    if(!tqueue(j))
    {
        if(j->serv) tshowserv(j->uid, j->original, j->original);
        else tshowpair(j->uid, j->team, j->name, j->original, j->original, j->mention);
        delete j;
        conoutf("\f4translation busy, try /tretry again in a moment");
    }
}

static void tretrydo(int n)
{
    if(n > 0)
    {
        int uid, type;
        if(!chatlogfind(n, uid, type))
        {
            conoutf("/tretry %d : no line %d (F10 has %d)", n, n, chatloglen());
            return;
        }
        (void)type;
        tretryline *L = tretryhistfind(uid);
        if(!L || !L->used || !L->original[0])
        {
            conoutf("/tretry %d : cannot retranslate that line", n);
            return;
        }
        tretryfrom(*L, true);
        return;
    }
    tretryfrom(tlast_retry.used ? tlast_retry : tlast_retry_serv, false);
}

static void tretryarg(const char *s)
{
    while(s && (*s==' ' || *s=='\t')) s++;
    if(!s || !s[0]) { tretrydo(0); return; }
    const char *p = s;
    if(*p=='+') p++;
    if(*p < '0' || *p > '9')
    {
        conoutf("/tretry : last chat, MOTD or [INFO]  ·  /tretry N : F10 line N");
        return;
    }
    int n = 0;
    while(*p >= '0' && *p <= '9')
    {
        if(n > 100000000) { n = -1; break; }
        n = n*10 + (*p++ - '0');
    }
    while(*p==' ' || *p=='\t') p++;
    if(*p || n < 1)
    {
        conoutf("/tretry : last chat, MOTD or [INFO]  ·  /tretry N : F10 line N");
        return;
    }
    tretrydo(n);
}

static void tusage(const char *cmd, const char *hint)
{
    conoutf("/%s <name> <message> : %s", cmd, hint);
}

static void tsaydo(char *text, bool team, bool reply, const char *mention, const char *last, const char *lastspk)
{
    if(!text || !text[0]) return;
    char buf[TBUF];
    copystring(buf, text, TBUF);
    ttrim(buf);
    if(!buf[0]) return;
    if(reply && !(last && last[0]) && !tlast_in_text[0] && !(mention && mention[0]))
        conoutf("\f4no chat to reply to, using %s", tlangout());
    if(tcooling())
    {
        tsendtranslated(buf, buf, team, mention, tlangout());
        return;
    }
    const char *lang = tlangout();
    if(tprev_ok && !strcmp(buf, tprev_src) && tprev_femme == translatefemme && tprev_reply == reply && !strcmp(mention ? mention : "", tprev_mention) && (reply || !strcmp(lang, tprev_lang)))
    {
        tsendtranslated(buf, tprev_dst, team, mention, tprev_reply && tprev_lang[0] ? tprev_lang : lang);
        return;
    }
    if(ttakejob(buf, lang, team, reply, mention)) return;
    tstart(buf, lang, team, false, reply, mention, last, lastspk);
}

static void trepdo(char *text, bool team)
{
    if(!text || !text[0])
    {
        conoutf("/trep <message> : reply in the language of the last chat line");
        return;
    }
    tsaydo(text, team, true, NULL, NULL, NULL);
}

static void tfixhint(const tfixline &L, bool outgoing)
{
    if(outgoing) conoutf("/tfixout <how it should read> : last line you sent was translated wrong");
    else conoutf("/tfix <how it should read> : last chat line was translated wrong");
    if(!L.used)
    {
        conoutf("\f4no line to correct yet");
        return;
    }
    conoutf("\f4original: %s", L.original);
    conoutf("\f4shown as: %s", L.shown);
}

static void tfixdo(const char *s, bool outgoing)
{
    tfixline &L = outgoing ? tlast_out_fix : tlast_in_fix;
    char raw[TBUF], buf[TBUF];
    copystring(raw, s ? s : "", TBUF);
    ttrim(raw);
    if(!raw[0] || !L.used)
    {
        tfixhint(L, outgoing);
        return;
    }
    filtertext(buf, raw, true, true, TBUF-1);
    ttrim(buf);
    if(!buf[0])
    {
        tfixhint(L, outgoing);
        return;
    }
    const char *lang = L.lang[0] ? L.lang : (outgoing ? tlangout() : tlangin());
    const char *dst = buf;
    if(tsameish(buf, L.original)) dst = "=";
    if(!tlearnhttp(lang, outgoing ? "out" : "in", L.femme, L.original, dst, texamplekind))
    {
        conoutf("\f4could not save the example (is the translation service running?)");
        return;
    }
    const char *shown = tsameish(dst, "=") ? L.original : buf;
    copystring(L.shown, shown, TBUF);
    if(!outgoing) tshowpair(L.uid, L.team, L.name, shown, L.original, L.mention);
    if(tsameish(dst, "=")) conoutf("saved: leave untranslated (%s)", L.original);
    else if(texamplekind) conoutf("saved as few-shot example: %s -> %s", L.original, buf);
    else conoutf("saved as exact translation: %s -> %s", L.original, buf);
}

bool trytranslatedcmd(const char *buf)
{
    if(!buf || buf[0] != '/') return false;
    const char *s = buf + 1;
    while(*s == ' ' || *s == '\t') s++;
    char cmd[32];
    s = tskipword(s, cmd, sizeof(cmd));
    tlowercmd(cmd);
    if(!strcmp(cmd, "tfix"))
    {
        tfixdo(s, false);
        return true;
    }
    if(!strcmp(cmd, "tfixout"))
    {
        tfixdo(s, true);
        return true;
    }
    if(!strcmp(cmd, "tretry"))
    {
        tretryarg(s);
        return true;
    }
    bool team = false;
    int rawkind = tnamecmdkind(cmd, &team);
    if(rawkind < 0) return false;
    int kind = tresolvekind(rawkind);

    char typed[MAXNAMELEN+1];
    s = tparsename(s, typed, sizeof(typed));
    if(!typed[0] || !*s)
    {
        tusage(cmd, tkindhint(kind, rawkind));
        return true;
    }
    bool ambiguous = false;
    fpsent *d = tfindplayer(typed, &ambiguous);
    if(!d)
    {
        conoutf(ambiguous ? "\f4several players match %s, press Tab" : "\f4no player matching %s", typed);
        return true;
    }
    char mention[MAXNAMELEN+8], msg[TBUF];
    nformatstring(mention, sizeof(mention), "@%s", d->name);
    copystring(msg, s, TBUF);
    ttrim(msg);
    if(!msg[0])
    {
        tusage(cmd, tkindhint(kind, rawkind));
        return true;
    }
    if(kind == 0)
    {
        tsendout(msg, team, mention);
        return true;
    }
    char last[TBUF], lastspk[MAXSTRLEN];
    last[0] = lastspk[0] = 0;
    bool has = kind == 2 && tlastfor(d, last, sizeof(last), lastspk, sizeof(lastspk));
    tsaydo(msg, team, has, mention, last, lastspk);
    return true;
}

void resetplayercomplete()
{
    tcomp_i = -1;
    tcomp_prefix[0] = 0;
    tcomp_cmd[0] = 0;
}

struct tcompcand { fpsent *d; int fit, spoken; };
struct tcompcmp
{
    bool operator()(const tcompcand &a, const tcompcand &b) const
    {
        if(a.fit != b.fit) return a.fit < b.fit;
        if(a.spoken != b.spoken) return a.spoken > b.spoken;
        return strcmp(a.d->name, b.d->name) < 0;
    }
};

bool completeplayername(char *s, int maxlen, int dir, bool tab)
{
    if(!s || !s[0]) return false;
    const char *p = s;
    if(*p == '/') p++;
    const char *cmdend = p;
    while(*cmdend && *cmdend != ' ' && *cmdend != '\t') cmdend++;
    if(!*cmdend) return false;
    char cmd[32];
    int clen = (int)(cmdend - p);
    if(clen <= 0 || clen >= (int)sizeof(cmd)) return false;
    memcpy(cmd, p, clen);
    cmd[clen] = 0;
    tlowercmd(cmd);
    bool team = false;
    if(tnamecmdkind(cmd, &team) < 0) return false;

    const char *sp = cmdend;
    while(*sp == ' ' || *sp == '\t') sp++;
    bool inquotes = *sp == '"';
    const char *namebeg = inquotes ? sp + 1 : sp;
    const char *nameend = namebeg;
    if(inquotes) { while(*nameend && *nameend != '"') nameend++; }
    else { while(*nameend && *nameend != ' ' && *nameend != '\t') nameend++; }
    const char *after = nameend;
    if(inquotes && *after == '"') after++;
    while(*after == ' ' || *after == '\t') after++;
    if(*after) return false;

    if(!tab && tcomp_i < 0) return false;

    char prefix[64];
    int n = (int)(nameend - namebeg);
    if(n < 0) n = 0;
    if(n > (int)sizeof(prefix)-1) n = sizeof(prefix)-1;
    memcpy(prefix, namebeg, n);
    prefix[n] = 0;

    if(tcomp_i < 0 || strcmp(tcomp_cmd, cmd))
    {
        copystring(tcomp_prefix, prefix, sizeof(tcomp_prefix));
        copystring(tcomp_cmd, cmd, sizeof(tcomp_cmd));
        tcomp_i = -1;
    }

    vector<tcompcand> cands;
    loopv(players)
    {
        fpsent *o = players[i];
        if(!o || o == player1 || !o->name[0]) continue;
        int fit = tnamefit(o->name, tcomp_prefix);
        if(fit < 0) continue;
        tcompcand &c = cands.add();
        c.d = o;
        c.fit = fit;
        c.spoken = tlastwhen(o->clientnum);
    }
    if(!cands.length()) return true;
    cands.sort(tcompcmp());

    if(tcomp_i < 0) tcomp_i = dir < 0 ? cands.length()-1 : 0;
    else
    {
        tcomp_i += dir < 0 ? -1 : 1;
        if(tcomp_i < 0) tcomp_i = cands.length()-1;
        else if(tcomp_i >= cands.length()) tcomp_i = 0;
    }

    const char *slash = (s[0] == '/') ? "/" : "";
    nformatstring(s, maxlen, "%s%s \"%s\" ", slash, cmd, cands[tcomp_i].d->name);
    return true;
}

bool trytranslatedsay(const char *text, const char *action)
{
    if(!translateauto || !text || !text[0] || text[0]=='/') return false;
    bool team = action && strstr(action, "sayteam");
    tsaydo(const_cast<char *>(text), team, tautolast(), NULL, NULL, NULL);
    return true;
}

static void tcmdline(const char *cmd, char *s)
{
    char line[TBUF+64];
    nformatstring(line, sizeof(line), "/%s %s", cmd, s ? s : "");
    trytranslatedcmd(line);
}

ICOMMAND(tsay, "C", (char *s), tsaydo(s, false, false, NULL, NULL, NULL));
ICOMMAND(tsayteam, "C", (char *s), tsaydo(s, true, false, NULL, NULL, NULL));
ICOMMAND(trep, "C", (char *s), trepdo(s, false));
ICOMMAND(trepteam, "C", (char *s), trepdo(s, true));
ICOMMAND(to, "C", (char *s), tcmdline("to", s));
ICOMMAND(sayto, "C", (char *s), tcmdline("sayto", s));
ICOMMAND(toteam, "C", (char *s), tcmdline("toteam", s));
ICOMMAND(saytoteam, "C", (char *s), tcmdline("saytoteam", s));
ICOMMAND(tsayto, "C", (char *s), tcmdline("tsayto", s));
ICOMMAND(tsaytoteam, "C", (char *s), tcmdline("tsaytoteam", s));
ICOMMAND(trepto, "C", (char *s), tcmdline("trepto", s));
ICOMMAND(treptoteam, "C", (char *s), tcmdline("treptoteam", s));
ICOMMAND(tfix, "C", (char *s), tfixdo(s, false));
ICOMMAND(tfixout, "C", (char *s), tfixdo(s, true));
ICOMMAND(tretry, "C", (char *s), tretryarg(s));
ICOMMAND(texamplesync, "", (), texamplesync());
ICOMMAND(texamplerefresh, "", (), { texample_stale = true; texamplesfetch(true); });
ICOMMAND(texamplesave, "", (), texamplesave());
ICOMMAND(texampleforget, "i", (int *i), texampleforgetidx(*i));
ICOMMAND(texampleforgetsrc, "", (), texampleforgetsrc());
ICOMMAND(texampleload, "i", (int *i), texampleload(*i));
ICOMMAND(texamplesetlang, "s", (char *s), {
    setsvar("texamplelang", s ? s : "");
    texample_stale = true;
    texamplesfetch(true);
});
ICOMMAND(texampleline, "i", (int *i), texamplelinecmd(*i));
ICOMMAND(texamplestatus, "", (), texamplestatuscmd());

ICOMMAND(tmodelsync, "", (), tmodelsync());
ICOMMAND(tmodelstatus, "", (), {
    if(!tmodel_ok && !tmodel_msg[0]) result("translation service busy or not running (start the game with L4ZY.exe)");
    else result(tmodel_msg);
});
ICOMMAND(tmodelstate, "s", (char *id), result(tmodelstateof(id)));
ICOMMAND(tmodellabel, "s", (char *id), {
    static char buf[128];
    if(id && !strcmp(id, "large"))
        nformatstring(buf, sizeof(buf), "Large  %s", tmodel_largeabout[0] ? tmodel_largeabout : "Qwen3-14B");
    else if(id && !strcmp(id, "normal"))
        nformatstring(buf, sizeof(buf), "Normal  %s", tmodel_normalabout[0] ? tmodel_normalabout : "Qwen3-8B");
    else
        nformatstring(buf, sizeof(buf), "Small  %s", tmodel_smallabout[0] ? tmodel_smallabout : "Qwen3-4B");
    result(buf);
});
ICOMMAND(tmodelbtn, "s", (char *id), result(tmodelbtnof(id)));
ICOMMAND(tmodelclick, "s", (char *id), tmodelclickcmd(id));
ICOMMAND(tmodelhint, "", (), result(tmodel_api
    ? "\f3API key is saved: chat is sent to the internet"
    : "\f4no API key: the local model below translates on this PC"));
ICOMMAND(tmodelapi, "", (), result(tmodel_api ? "1" : "0"));
ICOMMAND(tmodelinstall, "s", (char *id), tmodelinstallcmd(id));
ICOMMAND(tmodeluse, "s", (char *id), tmodelusecmd(id));
ICOMMAND(tmodeldelete, "s", (char *id), tmodeldeletecmd(id));
ICOMMAND(tmodelsaveapi, "", (), tmodelsaveapi());
ICOMMAND(tmodelclearapi, "", (), tmodelclearapi());
ICOMMAND(tmodelcustomname, "i", (int *i), {
    static char buf[160];
    if(*i < 0 || *i >= tmodel_customn || !tmodelcustoms[*i].name[0]) { result(""); return; }
    tmodelcustom &c = tmodelcustoms[*i];
    if(!strcmp(c.state, "active"))
        nformatstring(buf, sizeof(buf), "^f0%s  ~%s MB  In Use", c.name, c.size[0] ? c.size : "?");
    else
        nformatstring(buf, sizeof(buf), "%s  ~%s MB", c.name, c.size[0] ? c.size : "?");
    result(buf);
});
ICOMMAND(tmodelcustomstate, "i", (int *i), {
    if(*i < 0 || *i >= tmodel_customn) result("missing");
    else result(tmodelcustoms[*i].state[0] ? tmodelcustoms[*i].state : "installed");
});
ICOMMAND(tmodelcustomuse, "i", (int *i), tmodelcustomuse(*i));
ICOMMAND(tmodelcustomdel, "i", (int *i), tmodelcustomdel(*i));

ICOMMAND(tupdatesync, "", (), tupdatesync());
ICOMMAND(tupdateapply, "", (), tupdateapply());
ICOMMAND(tupdatelabel, "", (), result(tupdatelabelof()));
ICOMMAND(tupdatemsg, "", (), result(tupdate_msg));
ICOMMAND(tupdatecheck, "", (), tupdatecheck());
ICOMMAND(tupdaterestart, "", (), tupdaterestart());
ICOMMAND(tupdateinstalled, "", (), result(tupdate_installed[0] ? tupdate_installed : "?"));
ICOMMAND(tupdateremote, "", (), result(tupdate_ver));
ICOMMAND(tupdatesize, "", (), result(tupdate_size));
ICOMMAND(tupdatechannel, "", (), result(tupdate_channel));
ICOMMAND(tupdatestate, "", (), result(tupdate_state));
ICOMMAND(tupdateprogress, "", (), intret(tupdate_progress));
ICOMMAND(tupdatewantrestart, "", (), intret(tupdate_wantrestart && tupdate_restart ? 1 : 0));
ICOMMAND(tupdateservice, "", (), intret(tupdate_ok ? 1 : 0));

ICOMMAND(translatestatus, "", (),
{
    const char *on = translatechat ? "active" : "inactive";
    const char *svc = "injoignable";
    switch(tprobe())
    {
        case 0: svc = "ok"; break;
        case 1: svc = "non demarre"; break;
        default: svc = "injoignable"; break;
    }
    tlock();
    int pending = tjobs.length();
    tunlock();
    conoutf("traduction : %s, recu -> %s, envoi -> %s, auto-translate %s, femme %s, service %s:%d (%s), %d en attente",
        on, tlangin(), tlangout(), tautolabel(), translatefemme ? "on" : "off", thost(), tport(), svc, pending);
});

}
