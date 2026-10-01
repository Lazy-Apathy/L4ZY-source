// L4ZY.exe : point d'entree unique d'une installation L4ZY.
//
// Mode normal : verrou de l'installation, lance le jeu, puis (jeu initialise)
// le service prive (Python embarque + traduction + mises a jour) dans un job
// Windows. A la fermeture du jeu : arret du service, du llama-server de CETTE
// installation (job), rien d'autre. Si une mise a jour a ete preparee et
// demandee depuis le jeu, une copie du lanceur placee dans state\update\helper
// l'applique une fois que plus rien de l'installation ne tourne.
//
// Modes internes (copie helper) :
//   --apply    applique state\update\stage\plan.txt (sauvegarde + retour arriere)
//   --recover  termine ou annule une application interrompue (journal)
//   --rollback revient a la version precedente (derniere sauvegarde)
//
// Aucun Python, aucune DLL de l'installation n'est necessaire pour appliquer :
// le helper est un binaire statique qui vit hors des fichiers remplaces.

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <knownfolders.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstdarg>

using std::wstring;
using std::string;
using std::vector;

static const int LAUNCHER_API = 1;
static wstring g_root, g_state, g_logpath;
static bool g_importonly = false;

// ---------------------------------------------------------------- utilitaires

static wstring widen(const string &s)
{
    if(s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static string narrow(const wstring &w)
{
    if(w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), NULL, 0, NULL, NULL);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

static wstring join(const wstring &a, const wstring &b)
{
    if(a.empty()) return b;
    wchar_t last = a[a.size()-1];
    if(last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

static wstring relwin(const string &rel)
{
    wstring w = widen(rel);
    for(size_t i = 0; i < w.size(); i++) if(w[i] == L'/') w[i] = L'\\';
    return w;
}

static bool exists(const wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool isdir(const wstring &p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static wstring parentdir(const wstring &p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == wstring::npos ? wstring() : p.substr(0, s);
}

static void mkdirs(const wstring &p)
{
    if(p.empty() || isdir(p)) return;
    mkdirs(parentdir(p));
    CreateDirectoryW(p.c_str(), NULL);
}

static void logf(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(g_logpath.empty()) return;
    FILE *f = _wfopen(g_logpath.c_str(), L"ab");
    if(!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d [%lu] %s\r\n", st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(), buf);
    fclose(f);
}

static bool readfile(const wstring &p, string &out)
{
    out.clear();
    FILE *f = _wfopen(p.c_str(), L"rb");
    if(!f) return false;
    char buf[65536];
    size_t n;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static bool writefile_durable(const wstring &p, const string &data)
{
    wstring tmp = p + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, NULL) && w == data.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if(!ok) return false;
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static bool appendline_durable(const wstring &p, const string &line)
{
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(h == INVALID_HANDLE_VALUE) return false;
    string l = line + "\n";
    DWORD w = 0;
    bool ok = WriteFile(h, l.data(), (DWORD)l.size(), &w, NULL) && w == l.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

static vector<string> splitlines(const string &s)
{
    vector<string> out;
    size_t i = 0;
    while(i <= s.size())
    {
        size_t j = s.find('\n', i);
        if(j == string::npos) j = s.size();
        string l = s.substr(i, j - i);
        if(!l.empty() && l[l.size()-1] == '\r') l.erase(l.size()-1);
        out.push_back(l);
        i = j + 1;
    }
    return out;
}

static string trim(const string &s)
{
    size_t a = 0, b = s.size();
    while(a < b && (unsigned char)s[a] <= ' ') a++;
    while(b > a && (unsigned char)s[b-1] <= ' ') b--;
    return s.substr(a, b - a);
}

// INI UTF-8 minimal : [section] cle = valeur ; # et ; commentaires.
typedef std::map<string, string> inimap;
static inimap readini(const wstring &p)
{
    inimap m;
    string data;
    if(!readfile(p, data)) return m;
    if(data.size() >= 3 && (unsigned char)data[0] == 0xEF) data = data.substr(3);
    string section;
    vector<string> lines = splitlines(data);
    for(size_t i = 0; i < lines.size(); i++)
    {
        string l = trim(lines[i]);
        if(l.empty() || l[0] == '#' || l[0] == ';') continue;
        if(l[0] == '[') { section = trim(l.substr(1, l.find(']') - 1)); continue; }
        size_t eq = l.find('=');
        if(eq == string::npos) continue;
        string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        m[section.empty() ? k : section + "." + k] = v;
    }
    return m;
}

static string iniget(const inimap &m, const string &k, const string &def = "")
{
    inimap::const_iterator it = m.find(k);
    return it == m.end() ? def : it->second;
}

static void message(const wchar_t *text, UINT icon = MB_ICONINFORMATION)
{
    logf("message: %s", narrow(text).c_str());
    // Essais automatises : aucune fenetre bloquante, tout est dans le journal.
    const char *quiet = getenv("L4ZY_NO_DIALOGS");
    if(quiet && quiet[0] == '1') return;
    MessageBoxW(NULL, text, L"L4ZY", MB_OK | icon | MB_SETFOREGROUND);
}

static wstring knownfolder(REFKNOWNFOLDERID id)
{
    PWSTR p = NULL;
    wstring out;
    if(SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p)) && p) out = p;
    if(p) CoTaskMemFree(p);
    return out;
}

static string sha256file(const wstring &p, long long &size)
{
    size = -1;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if(f == INVALID_HANDLE_VALUE) return "";
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hh = NULL;
    string out;
    if(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
       BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0)
    {
        static unsigned char buf[1 << 20];
        DWORD n = 0;
        long long total = 0;
        bool ok = true;
        while(ReadFile(f, buf, sizeof(buf), &n, NULL) && n > 0)
        {
            if(BCryptHashData(hh, buf, n, 0) != 0) { ok = false; break; }
            total += n;
        }
        unsigned char dig[32];
        if(ok && BCryptFinishHash(hh, dig, 32, 0) == 0)
        {
            static const char *hex = "0123456789abcdef";
            for(int i = 0; i < 32; i++) { out += hex[dig[i] >> 4]; out += hex[dig[i] & 15]; }
            size = total;
        }
    }
    if(hh) BCryptDestroyHash(hh);
    if(alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}

static string randomhex(int bytes)
{
    vector<unsigned char> b(bytes);
    BCryptGenRandom(NULL, &b[0], bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char *hex = "0123456789abcdef";
    string out;
    for(int i = 0; i < bytes; i++) { out += hex[b[i] >> 4]; out += hex[b[i] & 15]; }
    return out;
}

static wstring quotearg(const wstring &a)
{
    if(!a.empty() && a.find_first_of(L" \t\"") == wstring::npos) return a;
    wstring out = L"\"";
    int bs = 0;
    for(size_t i = 0; i < a.size(); i++)
    {
        if(a[i] == L'\\') { bs++; continue; }
        if(a[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out += L'"'; bs = 0; continue; }
        out.append(bs, L'\\'); bs = 0;
        out += a[i];
    }
    out.append(bs * 2, L'\\');
    out += L"\"";
    return out;
}

static wstring cmdline(const vector<wstring> &args)
{
    wstring c;
    for(size_t i = 0; i < args.size(); i++) { if(i) c += L" "; c += quotearg(args[i]); }
    return c;
}

// Le jeu recoit ses arguments en page de code ANSI. Un chemin qui ne s'y
// represente pas (profil sous un nom exotique) passe en nom court 8.3.
static wstring ansisafe(const wstring &p)
{
    BOOL lossy = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, p.c_str(), -1, NULL, 0, NULL, &lossy);
    if(!lossy) return p;
    mkdirs(p);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetShortPathNameW(p.c_str(), buf, MAX_PATH * 2);
    if(n > 0 && n < MAX_PATH * 2) return wstring(buf, n);
    return p;
}

// ------------------------------------------------------------ verrou unique

static HANDLE g_lock = INVALID_HANDLE_VALUE;

static bool takelock(int waitms)
{
    wstring p = join(g_state, L"install.lock");
    mkdirs(g_state);
    DWORD start = GetTickCount();
    for(;;)
    {
        g_lock = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if(g_lock != INVALID_HANDLE_VALUE) return true;
        if((int)(GetTickCount() - start) >= waitms) return false;
        Sleep(250);
    }
}

static void droplock()
{
    if(g_lock != INVALID_HANDLE_VALUE) CloseHandle(g_lock);
    g_lock = INVALID_HANDLE_VALUE;
}

// ------------------------------------------------ application d'une mise a jour

struct planop { bool put; string rel; long long size; string sha; };

static bool relok(const string &rel)
{
    static const char *tops[] = { "bin64", "data", "packages", "runtime", "service", "docs", NULL };
    static const char *rootfiles[] = { "L4ZY.exe", "autoexec.cfg", "traduction.cfg", "LISEZMOI.txt", "README.txt", NULL };
    if(rel.empty() || rel.size() > 240 || rel[0] == '/') return false;
    if(rel.find('\\') != string::npos || rel.find(':') != string::npos) return false;
    vector<string> parts;
    size_t i = 0;
    while(i <= rel.size())
    {
        size_t j = rel.find('/', i);
        if(j == string::npos) j = rel.size();
        parts.push_back(rel.substr(i, j - i));
        i = j + 1;
    }
    for(size_t k = 0; k < parts.size(); k++)
    {
        const string &p = parts[k];
        if(p.empty() || p == "." || p == ".." || p[p.size()-1] == '.' || p[p.size()-1] == ' ') return false;
        for(size_t c = 0; c < p.size(); c++) if((unsigned char)p[c] < 32 || strchr("<>\"|?*", p[c])) return false;
    }
    if(parts.size() == 1)
    {
        for(int k = 0; rootfiles[k]; k++) if(parts[0] == rootfiles[k]) return true;
        return false;
    }
    for(int k = 0; tops[k]; k++) if(parts[0] == tops[k]) return true;
    return false;
}

// Refuse d'ecrire a travers une jonction ou un lien, meme deja present.
static bool noreparse(const wstring &root, const string &rel)
{
    wstring cur = root;
    wstring w = relwin(rel);
    size_t i = 0;
    while(i <= w.size())
    {
        size_t j = w.find(L'\\', i);
        if(j == wstring::npos) j = w.size();
        cur = join(cur, w.substr(i, j - i));
        DWORD a = GetFileAttributesW(cur.c_str());
        if(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        i = j + 1;
    }
    return true;
}

static bool readplan(const wstring &dir, string &version, vector<planop> &ops, string &err)
{
    string data;
    if(!readfile(join(dir, L"plan.txt"), data)) { err = "plan missing"; return false; }
    vector<string> lines = splitlines(data);
    if(lines.empty() || lines[0] != "l4zy-plan 1") { err = "unknown plan format"; return false; }
    bool ended = false;
    for(size_t i = 1; i < lines.size(); i++)
    {
        const string &l = lines[i];
        if(l.empty()) continue;
        if(l == "END") { ended = true; break; }
        if(l.compare(0, 8, "version ") == 0) { version = l.substr(8); continue; }
        if(l.compare(0, 5, "from ") == 0) continue;
        vector<string> f;
        size_t a = 0;
        while(a <= l.size())
        {
            size_t b = l.find('\t', a);
            if(b == string::npos) b = l.size();
            f.push_back(l.substr(a, b - a));
            a = b + 1;
        }
        planop op;
        if(f[0] == "PUT" && f.size() == 4) { op.put = true; op.rel = f[1]; op.size = _atoi64(f[2].c_str()); op.sha = f[3]; }
        else if(f[0] == "DEL" && f.size() == 2) { op.put = false; op.rel = f[1]; op.size = 0; }
        else { err = "bad plan line"; return false; }
        if(!relok(op.rel)) { err = "plan path refused: " + op.rel; return false; }
        if(!noreparse(g_root, op.rel)) { err = "junction or link in the way: " + op.rel; return false; }
        ops.push_back(op);
    }
    if(!ended) { err = "plan truncated"; return false; }
    return true;
}

static bool movefile(const wstring &from, const wstring &to)
{
    mkdirs(parentdir(to));
    for(int tries = 0; tries < 20; tries++)
    {
        if(MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        DWORD e = GetLastError();
        if(e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION) return false;
        Sleep(250); // antivirus ou indexeur qui lit le fichier un instant
    }
    return false;
}

static const wchar_t *STATEFILES[] = { L"installed.json", L"installed.ini", NULL };

// Remet l'installation dans l'etat d'avant, uniquement d'apres le disque :
// sauvegarde presente = restaurer ; fichier place sans sauvegarde = il etait
// nouveau, on le range. Rejouable autant de fois que necessaire.
static bool rollbackdir(const wstring &dir, const vector<planop> &ops)
{
    bool ok = true;
    wstring files = join(dir, L"files"), backup = join(dir, L"backup");
    for(size_t k = ops.size(); k-- > 0;)
    {
        const planop &op = ops[k];
        wstring w = relwin(op.rel);
        wstring tgt = join(g_root, w), bak = join(backup, w), src = join(files, w);
        if(op.put)
        {
            if(exists(bak))
            {
                if(exists(tgt))
                {
                    if(!exists(src)) movefile(tgt, src); else DeleteFileW(tgt.c_str());
                }
                if(!movefile(bak, tgt)) { ok = false; logf("rollback: cannot restore %s (%lu)", op.rel.c_str(), GetLastError()); }
            }
            else if(exists(tgt) && !exists(src))
            {
                if(!movefile(tgt, src)) { ok = false; logf("rollback: cannot remove new %s", op.rel.c_str()); }
            }
        }
        else if(exists(bak) && !exists(tgt))
        {
            if(!movefile(bak, tgt)) { ok = false; logf("rollback: cannot restore deleted %s", op.rel.c_str()); }
        }
    }
    for(int i = 0; STATEFILES[i]; i++)
    {
        wstring bak = join(join(dir, L"backup-state"), STATEFILES[i]);
        if(exists(bak) && !movefile(bak, join(g_state, STATEFILES[i]))) ok = false;
    }
    return ok;
}

static void deltree(const wstring &dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(dir, L"*").c_str(), &fd);
    if(h != INVALID_HANDLE_VALUE)
    {
        do
        {
            wstring n = fd.cFileName;
            if(n == L"." || n == L"..") continue;
            wstring p = join(dir, n);
            if((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) deltree(p);
            else if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
            else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
        } while(FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

static void writeresult(bool ok, const string &version, const string &msg)
{
    string s = string("ok=") + (ok ? "1" : "0") + "\nversion=" + version + "\nmessage=" + msg + "\n";
    writefile_durable(join(join(g_state, L"update"), L"last-result.txt"), s);
    logf("result: ok=%d version=%s %s", ok ? 1 : 0, version.c_str(), msg.c_str());
}

// Apres COMMIT : la sauvegarde devient le point de retour arriere.
static void finalize(const wstring &stage)
{
    wstring upd = join(g_state, L"update");
    wstring rb = join(upd, L"rollback");
    if(exists(rb)) deltree(rb);
    deltree(join(stage, L"files"));
    DeleteFileW(join(stage, L"READY").c_str());
    DeleteFileW(join(stage, L"APPLY").c_str());
    DeleteFileW(join(stage, L"installed.json.new").c_str());
    DeleteFileW(join(stage, L"installed.ini.new").c_str());
    if(!MoveFileExW(stage.c_str(), rb.c_str(), MOVEFILE_WRITE_THROUGH)) deltree(stage);
    DeleteFileW(join(upd, L"journal.txt").c_str());
}

static bool applyplan(string &version, string &err)
{
    wstring upd = join(g_state, L"update");
    wstring stage = join(upd, L"stage");
    wstring journal = join(upd, L"journal.txt");
    vector<planop> ops;
    if(!readplan(stage, version, ops, err)) return false;
    string ready;
    readfile(join(stage, L"READY"), ready);
    if(trim(ready) != version) { err = "staged update is not complete"; return false; }
    wstring files = join(stage, L"files"), backup = join(stage, L"backup");

    // Avant de toucher quoi que ce soit : chaque fichier prepare est la.
    for(size_t k = 0; k < ops.size(); k++)
    {
        if(!ops[k].put) continue;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        wstring src = join(files, relwin(ops[k].rel));
        if(!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa) ||
           ((long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) != ops[k].size)
        { err = "prepared file missing: " + ops[k].rel; return false; }
    }
    for(int i = 0; STATEFILES[i]; i++)
        if(!exists(join(stage, wstring(STATEFILES[i]) + L".new"))) { err = "prepared state missing"; return false; }

    if(!writefile_durable(journal, "BEGIN " + version + "\n")) { err = "cannot write journal"; return false; }
    logf("apply %s: %d operations", version.c_str(), (int)ops.size());

    // Essai de coupure : arret brutal apres N operations (simulation de
    // panne de courant). Sans cette variable, aucun effet.
    const char *crash = getenv("L4ZY_TEST_CRASH_AFTER");
    int crashafter = crash ? atoi(crash) : -1;
    bool ok = true;
    for(size_t k = 0; k < ops.size() && ok; k++)
    {
        if(crashafter >= 0 && (int)k == crashafter) { logf("TEST: simulated crash after %d operations", crashafter); ExitProcess(99); }
        const planop &op = ops[k];
        wstring w = relwin(op.rel);
        wstring tgt = join(g_root, w), bak = join(backup, w);
        if(exists(tgt) && !exists(bak))
        {
            if(!movefile(tgt, bak)) { ok = false; err = "file in use or protected: " + op.rel; break; }
        }
        if(op.put && !movefile(join(files, w), tgt)) { ok = false; err = "cannot place " + op.rel; }
    }
    if(ok)
    {
        for(size_t k = 0; k < ops.size() && ok; k++)
        {
            if(!ops[k].put) continue;
            long long sz;
            string sha = sha256file(join(g_root, relwin(ops[k].rel)), sz);
            if(sha != ops[k].sha || sz != ops[k].size) { ok = false; err = "verification failed: " + ops[k].rel; }
        }
    }
    if(ok)
    {
        wstring bs = join(stage, L"backup-state");
        mkdirs(bs);
        for(int i = 0; STATEFILES[i] && ok; i++)
        {
            wstring cur = join(g_state, STATEFILES[i]);
            if(exists(cur) && !exists(join(bs, STATEFILES[i])) && !movefile(cur, join(bs, STATEFILES[i]))) ok = false;
            if(ok && !movefile(join(stage, wstring(STATEFILES[i]) + L".new"), cur)) ok = false;
            if(!ok) err = "cannot record the installed version";
        }
    }
    if(!ok)
    {
        logf("apply failed: %s -> rollback", err.c_str());
        bool rb = rollbackdir(stage, ops);
        DeleteFileW(journal.c_str());
        DeleteFileW(join(stage, L"APPLY").c_str());
        if(!rb) err += " (restore incomplete, see state\\logs\\launcher.log)";
        return false;
    }
    appendline_durable(journal, "COMMIT");
    finalize(stage);
    return true;
}

static bool recoverjournal(string &version, string &err, bool &committed)
{
    wstring upd = join(g_state, L"update");
    wstring stage = join(upd, L"stage");
    wstring journal = join(upd, L"journal.txt");
    string j;
    committed = false;
    if(!readfile(journal, j)) return true;
    committed = j.find("COMMIT") != string::npos;
    vector<string> lines = splitlines(j);
    if(!lines.empty() && lines[0].compare(0, 6, "BEGIN ") == 0) version = lines[0].substr(6);
    if(committed) { finalize(stage); return true; }
    vector<planop> ops;
    string v2;
    if(!readplan(stage, v2, ops, err)) { err = "interrupted update, plan unreadable: " + err; return false; }
    bool ok = rollbackdir(stage, ops);
    DeleteFileW(journal.c_str());
    DeleteFileW(join(stage, L"APPLY").c_str());
    if(!ok) err = "interrupted update could not be fully undone";
    return ok;
}

static bool rollbacklast(string &version, string &err)
{
    wstring rb = join(join(g_state, L"update"), L"rollback");
    vector<planop> ops;
    if(!exists(join(rb, L"plan.txt"))) { err = "no previous version kept"; return false; }
    if(!readplan(rb, version, ops, err)) return false;
    // Les fichiers "nouveaux" sont ranges dans rollback\files puis supprimes.
    if(!rollbackdir(rb, ops)) { err = "previous version could not be fully restored"; return false; }
    deltree(rb);
    return true;
}

static void relaunch()
{
    wstring exe = join(g_root, L"L4ZY.exe");
    vector<wstring> a;
    a.push_back(exe);
    a.push_back(L"--after-update");
    wstring c = cmdline(a);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if(CreateProcessW(exe.c_str(), &c[0], NULL, NULL, FALSE, 0, NULL, g_root.c_str(), &si, &pi))
    { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    else logf("relaunch failed (%lu)", GetLastError());
}

static int helpermain(const wstring &mode, DWORD waitpid, bool relaunchafter)
{
    if(waitpid)
    {
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, waitpid);
        if(p) { WaitForSingleObject(p, 120000); CloseHandle(p); }
    }
    if(!takelock(90000))
    {
        message(L"La mise à jour attend que L4ZY soit fermé. Réessaie en relançant L4ZY.\n\n"
                L"The update is waiting for L4ZY to close. Start L4ZY again to retry.", MB_ICONWARNING);
        return 3;
    }
    string version, err;
    bool ok;
    if(mode == L"--apply")
    {
        bool committed = false;
        string jv;
        if(exists(join(join(g_state, L"update"), L"journal.txt")) && !recoverjournal(jv, err, committed)) ok = false;
        else ok = applyplan(version, err);
        writeresult(ok, version, ok ? "installed" : err);
    }
    else if(mode == L"--recover")
    {
        bool committed = false;
        bool had = exists(join(join(g_state, L"update"), L"journal.txt"));
        ok = recoverjournal(version, err, committed);
        if(!had) logf("recover: nothing to do");
        else
        {
            writeresult(ok && committed, version, committed ? "installed" : (ok ? "interrupted, previous version restored" : err));
            if(ok && !committed)
                message(L"Une mise à jour avait été interrompue : la version précédente a été remise en place.\n\n"
                        L"An update was interrupted: the previous version was restored.");
        }
    }
    else
    {
        ok = rollbacklast(version, err);
        writeresult(false, version, ok ? "rolled back to the previous version" : err);
        message(ok ? L"Version précédente restaurée.\n\nPrevious version restored."
                   : L"Retour arrière impossible (voir state\\logs\\launcher.log).\n\nRollback failed.", ok ? MB_ICONINFORMATION : MB_ICONERROR);
    }
    if(!ok && mode == L"--apply")
    {
        wstring w = L"La mise à jour n'a pas pu être installée. Ta version actuelle a été conservée.\n\n"
                    L"The update could not be installed. Your current version was kept.\n\n" + widen(err);
        message(w.c_str(), MB_ICONWARNING);
    }
    droplock();
    if(relaunchafter) relaunch();
    return ok ? 0 : 1;
}

// Copie le lanceur hors des fichiers remplaces et lui confie le travail.
static bool spawnhelper(const wchar_t *mode, bool relaunchafter)
{
    wstring hdir = join(join(g_state, L"update"), L"helper");
    mkdirs(hdir);
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(NULL, self, MAX_PATH * 4);
    wstring helper = join(hdir, L"L4ZY-update.exe");
    if(!CopyFileW(self, helper.c_str(), FALSE)) { logf("cannot copy helper (%lu)", GetLastError()); return false; }
    vector<wstring> a;
    a.push_back(helper);
    a.push_back(mode);
    a.push_back(L"--root");
    a.push_back(g_root);
    a.push_back(L"--wait-pid");
    wchar_t pid[32];
    swprintf(pid, 32, L"%lu", GetCurrentProcessId());
    a.push_back(pid);
    if(relaunchafter) a.push_back(L"--relaunch");
    wstring c = cmdline(a);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if(!CreateProcessW(helper.c_str(), &c[0], NULL, NULL, FALSE, 0, NULL, hdir.c_str(), &si, &pi))
    { logf("cannot start helper (%lu)", GetLastError()); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf("helper %ls started", mode);
    return true;
}

// ------------------------------------------------------------- mode normal

static bool portfree(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s == INVALID_SOCKET) return false;
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = bind(s, (sockaddr *)&a, sizeof(a)) == 0;
    closesocket(s);
    return ok;
}

static int pickport(int preferred)
{
    if(preferred > 0 && portfree(preferred)) return preferred;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 0;
    if(bind(s, (sockaddr *)&a, sizeof(a)) == 0)
    {
        int len = sizeof(a);
        getsockname(s, (sockaddr *)&a, &len);
        port = ntohs(a.sin_port);
    }
    closesocket(s);
    return port;
}

static void httppost(int port, const string &path, int timeoutms)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s == INVALID_SOCKET) return;
    DWORD tv = timeoutms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(connect(s, (sockaddr *)&a, sizeof(a)) == 0)
    {
        string req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(s, req.data(), (int)req.size(), 0);
        char buf[256];
        recv(s, buf, sizeof(buf), 0);
    }
    closesocket(s);
}

static bool fileshas(const wstring &p, const char *needle)
{
    string d;
    return readfile(p, d) && d.find(needle) != string::npos;
}

struct findwin { DWORD pid; HWND hwnd; };
static BOOL CALLBACK findwinproc(HWND h, LPARAM lp)
{
    findwin *f = (findwin *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if(pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->hwnd = h; return FALSE; }
    return TRUE;
}

// Met la fenetre du jeu au premier plan (une fois visible).
static bool bringtofront(DWORD pid)
{
    findwin f = { pid, NULL };
    EnumWindows(findwinproc, (LPARAM)&f);
    if(!f.hwnd) return false;
    if(IsIconic(f.hwnd)) ShowWindow(f.hwnd, SW_RESTORE);
    SetForegroundWindow(f.hwnd);
    return true;
}

static HANDLE startgame(const wstring &exe, const vector<wstring> &args, const wstring &env, DWORD &pid)
{
    vector<wstring> a;
    a.push_back(exe);
    a.insert(a.end(), args.begin(), args.end());
    wstring c = cmdline(a);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if(!CreateProcessW(exe.c_str(), &c[0], NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT,
                       (LPVOID)env.c_str(), g_root.c_str(), &si, &pi))
    {
        logf("game start failed (%lu)", GetLastError());
        return NULL;
    }
    CloseHandle(pi.hThread);
    pid = pi.dwProcessId;
    return pi.hProcess;
}

static wstring buildenv(const std::map<wstring, wstring> &set, bool dropython)
{
    // Bloc d'environnement trie, sans variables Python heritees : le Python
    // embarque ne doit jamais lire la configuration d'un Python personnel.
    std::map<wstring, wstring> env;
    LPWCH blk = GetEnvironmentStringsW();
    for(LPWCH p = blk; *p; p += wcslen(p) + 1)
    {
        wstring e = p;
        size_t eq = e.find(L'=', 1);
        if(eq == wstring::npos) continue;
        wstring k = e.substr(0, eq);
        wstring ku = k;
        for(size_t i = 0; i < ku.size(); i++) ku[i] = towupper(ku[i]);
        if(dropython && (ku.compare(0, 6, L"PYTHON") == 0 || ku == L"VIRTUAL_ENV" || ku == L"CONDA_PREFIX")) continue;
        // Variable d'outil de laboratoire : le jeu installe prend toujours
        // ses propres DLL NGX (bin64/ngx-*), jamais un dossier externe.
        if(ku == L"SAUER_NGX_DIR") continue;
        env[k] = e.substr(eq + 1);
    }
    FreeEnvironmentStringsW(blk);
    for(std::map<wstring, wstring>::const_iterator it = set.begin(); it != set.end(); ++it) env[it->first] = it->second;
    wstring out;
    for(std::map<wstring, wstring>::iterator it = env.begin(); it != env.end(); ++it)
    { out += it->first; out += L"="; out += it->second; out += L'\0'; }
    out += L'\0';
    return out;
}

// ------------------------------------------------ reprise des reglages existants

static bool filetime(const wstring &p, ULONGLONG &t)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if(!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    t = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    return true;
}

static void copytree_missing(const wstring &src, const wstring &dst, int &n)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(src, L"*").c_str(), &fd);
    if(h == INVALID_HANDLE_VALUE) return;
    mkdirs(dst);
    do
    {
        wstring nm = fd.cFileName;
        if(nm == L"." || nm == L"..") continue;
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        wstring s = join(src, nm), d = join(dst, nm);
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copytree_missing(s, d, n);
        else if(!exists(d) && CopyFileW(s.c_str(), d.c_str(), TRUE)) n++;
    } while(FindNextFileW(h, &fd));
    FindClose(h);
}

// Premier lancement d'un profil L4ZY : reprend les reglages du client utilise
// le plus recemment (Sauer-RT, ancien client de traduction, Sauerbraten
// d'origine) : touches, pseudo, options, serveurs, amis, cartes telechargees.
// Une seule fois (marqueur). Les profils d'origine ne sont que lus.
static void importsettings(const wstring &profile)
{
    wstring marker = join(profile, L"l4zy-import.txt");
    if(exists(marker)) return;
    wstring games = join(knownfolder(FOLDERID_Documents), L"My Games");
    static const wchar_t *cands[] = { L"Sauer-RT", L"Sauerbraten-traduction", L"Sauerbraten", NULL };
    wstring best, bestname;
    ULONGLONG bestt = 0;
    for(int i = 0; cands[i]; i++)
    {
        wstring d = join(games, cands[i]);
        if(lstrcmpiW(d.c_str(), profile.c_str()) == 0) continue;
        ULONGLONG t;
        if(filetime(join(d, L"config.cfg"), t) && t > bestt) { bestt = t; best = d; bestname = cands[i]; }
    }
    string note = "none found";
    if(!best.empty())
    {
        mkdirs(profile);
        // Un config.cfg deja cree par L4ZY est garde a cote, pas perdu.
        wstring cur = join(profile, L"config.cfg");
        if(exists(cur)) MoveFileExW(cur.c_str(), join(profile, L"config.cfg.l4zy-avant-import").c_str(), MOVEFILE_REPLACE_EXISTING);
        static const wchar_t *files[] = { L"config.cfg", L"autoexec.cfg", L"init.cfg", L"servers.cfg", L"friends.cfg", NULL };
        int n = 0;
        for(int i = 0; files[i]; i++)
        {
            wstring s = join(best, files[i]), d = join(profile, files[i]);
            if(!exists(s)) continue;
            if(lstrcmpW(files[i], L"config.cfg") != 0 && exists(d)) continue;
            if(CopyFileW(s.c_str(), d.c_str(), FALSE)) n++;
        }
        // Le menu de traduction doit rester charge par l'autoexec du joueur.
        wstring ae = join(profile, L"autoexec.cfg");
        string aetext;
        if(readfile(ae, aetext) && aetext.find("traduction.cfg") == string::npos)
        {
            FILE *f = _wfopen(ae.c_str(), L"ab");
            if(f) { fputs("\r\n// L4ZY : menu de traduction du chat\r\nexec \"traduction.cfg\"\r\n", f); fclose(f); }
        }
        // Cartes telechargees depuis les serveurs : tous les profils, sans ecraser.
        int maps = 0;
        for(int i = 0; cands[i]; i++) copytree_missing(join(join(games, cands[i]), L"packages"), join(profile, L"packages"), maps);
        note = "from " + narrow(bestname) + " (" + std::to_string(n) + " files, " + std::to_string(maps) + " map files)";
    }
    writefile_durable(marker, "imported settings: " + note + "\n");
    logf("settings import: %s", note.c_str());
}

static int normalmain(const vector<wstring> &passthrough)
{
    if(!takelock(1500))
    {
        message(L"L4ZY est déjà ouvert depuis ce dossier (ou une mise à jour est en cours).\n\n"
                L"L4ZY is already running from this folder (or an update is being installed).");
        return 2;
    }
    wstring upd = join(g_state, L"update");
    if(exists(join(upd, L"journal.txt")))
    {
        logf("interrupted update found, starting recovery");
        droplock();
        if(spawnhelper(L"--recover", true)) return 0;
        message(L"Impossible de reprendre une mise à jour interrompue.\n\nCannot resume an interrupted update.", MB_ICONERROR);
        return 1;
    }

    inimap ini = readini(join(g_root, L"l4zy.ini"));
    inimap inst = readini(join(g_state, L"installed.ini"));
    string version = iniget(inst, "version", "?");
    bool portable = iniget(ini, "paths.portable", "0") == "1";
    wstring profile = widen(iniget(ini, "paths.profile")), userdata = widen(iniget(ini, "paths.userdata"));
    if(portable)
    {
        if(profile.empty()) profile = join(join(g_root, L"userdata"), L"profile");
        if(userdata.empty()) userdata = join(join(g_root, L"userdata"), L"local");
    }
    else
    {
        if(profile.empty()) profile = join(join(knownfolder(FOLDERID_Documents), L"My Games"), L"L4ZY");
        if(userdata.empty()) userdata = join(knownfolder(FOLDERID_LocalAppData), L"L4ZY");
    }
    if(profile.size() < 2 || profile[1] != L':') profile = join(g_root, profile);
    if(userdata.size() < 2 || userdata[1] != L':') userdata = join(g_root, userdata);
    wstring data = join(userdata, L"translation"), models = join(userdata, L"models");
    mkdirs(profile); mkdirs(data); mkdirs(models); mkdirs(join(data, L"logs"));
    logf("start: version %s, profile %s", version.c_str(), narrow(profile).c_str());

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    int port = pickport(8747);
    string token = randomhex(16);
    bool envport = iniget(inst, "service_port_env", "0") == "1";

    wstring gameexe = join(join(g_root, L"bin64"), L"sauerbraten.exe");
    if(!exists(gameexe))
    {
        message(L"Fichiers du jeu manquants : réinstalle L4ZY.\n\nGame files are missing: reinstall L4ZY.", MB_ICONERROR);
        return 1;
    }
    // Un -q / -g donne en argument (outil de laboratoire, raccourci perso)
    // remplace le profil et le journal par defaut, sans doublon.
    wstring logname = L"log.txt";
    bool customprofile = false;
    vector<wstring> extra;
    for(size_t i = 0; i < passthrough.size(); i++)
    {
        const wstring &a = passthrough[i];
        if(a.size() > 2 && a.compare(0, 2, L"-q") == 0) { profile = a.substr(2); customprofile = true; }
        else if(a.size() > 2 && a.compare(0, 2, L"-g") == 0) logname = a.substr(2);
        else extra.push_back(a);
    }
    mkdirs(profile);
    // Profil impose en argument (outil, laboratoire) : pas de reprise.
    if(!customprofile) importsettings(profile);
    if(g_importonly) { droplock(); return 0; }
    vector<wstring> gargs;
    gargs.push_back(L"-q" + ansisafe(profile));
    gargs.push_back(L"-g" + logname);
    // Ancien client (sans lecture de L4ZY_SERVICE_PORT) : toujours donner le
    // port, sinon une valeur sauvegardee lors d un lancement precedent resterait.
    if(!envport)
    {
        wchar_t x[64];
        swprintf(x, 64, L"-xtranslateport %d", port);
        gargs.push_back(x);
    }
    gargs.insert(gargs.end(), extra.begin(), extra.end());
    std::map<wstring, wstring> genv;
    wchar_t pbuf[16];
    swprintf(pbuf, 16, L"%d", port);
    genv[L"L4ZY_SERVICE_PORT"] = pbuf;
    genv[L"L4ZY_VERSION"] = widen(version);
    genv[L"L4ZY_MODELS_DIR"] = ansisafe(models);
    wstring gameenv = buildenv(genv, false);

    wstring gamelog = join(profile, logname);
    HANDLE game = NULL;
    DWORD gamepid = 0;
    for(int attempt = 0; attempt < 2; attempt++)
    {
        DeleteFileW(gamelog.c_str());
        game = startgame(gameexe, gargs, gameenv, gamepid);
        if(!game)
        {
            message(L"Le jeu n'a pas pu démarrer.\n\nThe game could not start.", MB_ICONERROR);
            return 1;
        }
        // Le modele de traduction ne charge qu'une fois le jeu initialise :
        // la carte graphique est au jeu pendant sa phase d'allocation.
        // Le lanceur n'a pas de fenetre : sans cela Windows peut refuser le
        // premier plan au jeu, et l'image HDR n'est presentee qu'apres une
        // activation manuelle (touche Windows). On le lui donne explicitement.
        AllowSetForegroundWindow(gamepid);
        bool lost = false, focused = false;
        for(int t = 0; t < 80; t++)
        {
            if(WaitForSingleObject(game, 250) == WAIT_OBJECT_0) break;
            if(!focused) focused = bringtofront(gamepid);
            if(fileshas(gamelog, "init: mainloop")) { bringtofront(gamepid); break; }
            if(fileshas(gamelog, "DEVICE_LOST")) { lost = true; break; }
        }
        if(!lost) break;
        Sleep(3000);
        if(fileshas(gamelog, "init: mainloop")) break;
        if(WaitForSingleObject(game, 10000) != WAIT_OBJECT_0 || attempt == 1) break;
        logf("graphics device lost during start, one retry");
        CloseHandle(game);
        Sleep(5000);
    }
    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(game, &code);
    if(code != STILL_ACTIVE)
    {
        logf("game ended during start (code %lu)", code);
    }

    // Service prive dans un job : fermer le job arrete aussi llama-server.
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof(li));
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    HANDLE svc = NULL;
    wstring py = join(g_root, L"runtime\\python\\pythonw.exe");
    wstring script = join(g_root, L"service\\translate_server.py");
    if(code == STILL_ACTIVE && exists(py) && exists(script))
    {
        vector<wstring> a;
        a.push_back(py);
        a.push_back(L"-X"); a.push_back(L"utf8");
        a.push_back(script);
        a.push_back(L"--port"); a.push_back(pbuf);
        a.push_back(L"--data-dir"); a.push_back(data);
        a.push_back(L"--models-dir"); a.push_back(models);
        a.push_back(L"--state-dir"); a.push_back(g_state);
        a.push_back(L"--profile-dir"); a.push_back(profile);
        a.push_back(L"--log"); a.push_back(join(join(data, L"logs"), L"service.log"));
        wchar_t mypid[32];
        swprintf(mypid, 32, L"%lu", GetCurrentProcessId());
        a.push_back(L"--parent-pid"); a.push_back(mypid);
        a.push_back(L"--token"); a.push_back(widen(token));
        string starter = iniget(ini, "translation.starter_model", "");
        if(!starter.empty()) { a.push_back(L"--starter"); a.push_back(widen(starter)); }
        wstring c = cmdline(a);
        std::map<wstring, wstring> senv;
        senv[L"PYTHONNOUSERSITE"] = L"1";
        senv[L"PYTHONDONTWRITEBYTECODE"] = L"1";
        wstring svcenv = buildenv(senv, true);
        // buildenv retire PYTHON* herites puis remet ces deux-la, voulus.
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        wstring svcdir = join(g_root, L"service");
        if(CreateProcessW(py.c_str(), &c[0], NULL, NULL, FALSE,
                          CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                          (LPVOID)svcenv.c_str(), svcdir.c_str(), &si, &pi))
        {
            AssignProcessToJobObject(job, pi.hProcess);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            svc = pi.hProcess;
            logf("service started on port %d (pid %lu)", port, pi.dwProcessId);
        }
        else logf("service start failed (%lu)", GetLastError());
    }
    else if(code == STILL_ACTIVE) logf("service files missing, game runs without translation/updates");

    WaitForSingleObject(game, INFINITE);
    CloseHandle(game);
    logf("game closed");

    if(svc)
    {
        httppost(port, "/shutdown?token=" + token, 3000);
        if(WaitForSingleObject(svc, 10000) != WAIT_OBJECT_0) logf("service did not stop in time, job terminated");
        CloseHandle(svc);
    }
    TerminateJobObject(job, 0);
    CloseHandle(job);
    WSACleanup();

    wstring stage = join(upd, L"stage");
    if(exists(join(stage, L"READY")) && exists(join(stage, L"APPLY")))
    {
        bool restart = fileshas(join(stage, L"APPLY"), "restart=1");
        logf("update requested, handing over to helper (restart=%d)", restart ? 1 : 0);
        droplock();
        if(!spawnhelper(L"--apply", restart))
            message(L"La mise à jour n'a pas pu démarrer. Ta version actuelle reste en place.\n\n"
                    L"The update could not start. Your current version is unchanged.", MB_ICONWARNING);
        return 0;
    }
    droplock();
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t self[MAX_PATH * 4];
    GetModuleFileNameW(NULL, self, MAX_PATH * 4);
    g_root = parentdir(self);

    wstring mode;
    DWORD waitpid = 0;
    bool relaunchafter = false;
    vector<wstring> passthrough;
    for(int i = 1; i < argc; i++)
    {
        wstring a = argv[i];
        if(a == L"--apply" || a == L"--recover" || a == L"--rollback") mode = a;
        else if(a == L"--root" && i + 1 < argc) g_root = argv[++i];
        else if(a == L"--wait-pid" && i + 1 < argc) waitpid = wcstoul(argv[++i], NULL, 10);
        else if(a == L"--relaunch") relaunchafter = true;
        else if(a == L"--after-update") {}
        else if(a == L"--import-only") g_importonly = true;
        else if(a == L"--version")
        {
            message(L"L4ZY launcher API 1");
            return 0;
        }
        else passthrough.push_back(a);
    }
    g_state = join(g_root, L"state");
    mkdirs(join(g_state, L"logs"));
    g_logpath = join(join(g_state, L"logs"), L"launcher.log");
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    int rc;
    // Deja la copie helper (lancee a la main) : pas de recopie sur soi-meme.
    wstring helperpath = join(join(join(g_state, L"update"), L"helper"), L"L4ZY-update.exe");
    bool ishelper = lstrcmpiW(self, helperpath.c_str()) == 0;
    if(mode == L"--rollback" && waitpid == 0 && !ishelper)
    {
        // Demande du joueur (raccourci "revenir a la version precedente") :
        // on passe par une copie helper, comme pour une mise a jour.
        if(!takelock(1500))
        {
            message(L"Ferme d'abord L4ZY.\n\nClose L4ZY first.", MB_ICONWARNING);
            return 2;
        }
        droplock();
        rc = spawnhelper(L"--rollback", false) ? 0 : 1;
    }
    else if(!mode.empty()) rc = helpermain(mode, waitpid, relaunchafter);
    else rc = normalmain(passthrough);
    LocalFree(argv);
    return rc;
}
