#include <stdio.h>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#ifdef _WIN32
    #include <direct.h>
    #define lc_mkdir(p) _mkdir(p)
#else
    #include <unistd.h>
    #define lc_mkdir(p) mkdir((p), 0755)
#endif

#include "LuckyChicken.h"
#include "metamod_oslink.h"
#include "schemasystem/schemasystem.h"
#include "module.h"

LuckyChicken g_LuckyChicken;
PLUGIN_EXPOSE(LuckyChicken, g_LuckyChicken);

IVEngineServer2*   engine              = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;
CEntitySystem*     g_pEntitySystem     = nullptr;
CGlobalVars*       gpGlobals           = nullptr;
extern ISource2Server* g_pSource2Server;

IUtilsApi*        g_pUtils   = nullptr;
IPlayersApi*      g_pPlayers = nullptr;
IMenusApi*        g_pMenus   = nullptr;
IShopApi*         g_pShop    = nullptr;
IMySQLConnection* g_pDB      = nullptr;

struct Cfg
{
    int min_players = 4;
    int server_id   = 1;

    std::string commands_open_menu = "chickenstats;cstats";

    bool show_top_lucky   = true;
    bool show_top_unlucky = true;
    bool show_top_money   = true;
    bool show_in_shop     = true;

    int top_size = 10;

    int  min_credits = 10;
    int  max_credits = 100;
    bool ignore_mult = true;

    int chance = 30;

    bool damage_on_unlucky = true;
    int  min_damage  = 1;
    int  max_damage  = 5;
    int  damage_kind = 1;

    bool msg_lucky_all    = true;
    bool msg_lucky_self   = true;
    bool msg_unlucky_self = true;

    bool        play_sound  = true;
    std::string snd_lucky   = "BuyZone.Activate";
    std::string snd_unlucky = "Chicken.Pain";

    int min_count = 5;
    int max_count = 12;

    bool log_kills = true;

    std::string db_section = "luckychicken";
    std::string db_host;
    std::string db_user;
    std::string db_pass;
    std::string db_base;
    int         db_port = 3306;

    std::map<std::string, std::vector<Vector>> spots;
};

static Cfg g_Cfg;

struct Bird
{
    CHandle<CBaseEntity> h;
    int  ent   = -1;
    bool lucky = false;
    bool gone  = false;
};

static std::vector<Bird> g_Birds;

struct Stats
{
    int  lucky    = 0;
    int  unlucky  = 0;
    int  earned   = 0;
    bool fetched  = false;
};

static std::unordered_map<uint64_t, Stats> g_St;
static std::map<std::string, std::string>  g_Tr;
static std::string g_Map;
static const char* kCurrency = "credits";

static inline const char* Tbl() { return "luckychicken"; }

static inline const char* Tr(const char* k, const char* def = "")
{
    auto it = g_Tr.find(k);
    return (it != g_Tr.end() && !it->second.empty()) ? it->second.c_str() : def;
}

static void ReadPhrases()
{
    g_Tr.clear();
    KeyValues::AutoDelete kv("Phrases");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/translations/luckychicken.phrases.txt"))
        return;

    const char* lang = g_pUtils ? g_pUtils->GetLanguage() : "en";
    for (KeyValues* p = kv->GetFirstTrueSubKey(); p; p = p->GetNextTrueSubKey())
    {
        const char* val = p->GetString(lang, nullptr);
        if (!val || !*val) val = p->GetString("en", nullptr);
        g_Tr[p->GetName()] = val ? val : "";
    }
}

static inline void Say(int slot, const char* fmt, ...)
{
    if (!g_pUtils) return;
    char body[1024];
    va_list va; va_start(va, fmt);
    V_vsnprintf(body, sizeof(body), fmt, va);
    va_end(va);

    const char* tag = Tr("Chat_Prefix", " {DEFAULT}[{LIGHTGREEN}LuckyChicken{DEFAULT}]");
    char out[1200];
    V_snprintf(out, sizeof(out), "%s %s", tag, body);
    g_pUtils->PrintToChat(slot, "%s", out);
}

static inline void SayAll(const char* fmt, ...)
{
    if (!g_pUtils) return;
    char body[1024];
    va_list va; va_start(va, fmt);
    V_vsnprintf(body, sizeof(body), fmt, va);
    va_end(va);

    const char* tag = Tr("Chat_Prefix", " {DEFAULT}[{LIGHTGREEN}LuckyChicken{DEFAULT}]");
    char out[1200];
    V_snprintf(out, sizeof(out), "%s %s", tag, body);
    g_pUtils->PrintToChatAll("%s", out);
}

static inline void SayK(int slot, const char* k, const char* fb, ...)
{
    const char* fmt = Tr(k, fb);
    char body[1024];
    va_list va; va_start(va, fb);
    V_vsnprintf(body, sizeof(body), fmt, va);
    va_end(va);
    Say(slot, "%s", body);
}

static inline void SayAllK(const char* k, const char* fb, ...)
{
    const char* fmt = Tr(k, fb);
    char body[1024];
    va_list va; va_start(va, fb);
    V_vsnprintf(body, sizeof(body), fmt, va);
    va_end(va);
    SayAll("%s", body);
}

static void Squawk(const char* fmt, ...)
{
    if (!g_Cfg.log_kills) return;

    lc_mkdir("addons");
    lc_mkdir("addons/logs");

    FILE* f = fopen("addons/logs/luckychicken.log", "a");
    if (!f) return;

    time_t t = time(nullptr);
    struct tm* lt = localtime(&t);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
        lt->tm_hour, lt->tm_min, lt->tm_sec);

    va_list va; va_start(va, fmt);
    vfprintf(f, fmt, va);
    va_end(va);

    fputc('\n', f);
    fclose(f);
}

static std::string Norm(const char* in)
{
    if (!in || !*in) return {};
    std::string s(in);
    size_t pp = s.find(" | ");
    if (pp != std::string::npos) s = s.substr(0, pp);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    size_t p = s.find_last_of('/');
    if (p != std::string::npos) s = s.substr(p + 1);
    p = s.find_last_of('.');
    if (p != std::string::npos) s = s.substr(0, p);
    return s;
}

static int Humans()
{
    int c = 0;
    for (int i = 0; i < 64; ++i)
    {
        if (!g_pPlayers->IsInGame(i) || g_pPlayers->IsFakeClient(i)) continue;
        ++c;
    }
    return c;
}

static int Alive()
{
    int c = 0;
    for (auto& b : g_Birds) if (!b.gone && b.h.Get()) ++c;
    return c;
}

static void BlastSound(int slot, const char* path)
{
    if (!path || !*path) return;
    if (!g_pPlayers || !g_pPlayers->IsInGame(slot) || g_pPlayers->IsFakeClient(slot)) return;
    g_pPlayers->EmitSound(slot, CEntityIndex(slot + 1), std::string(path), 100, 1.0f);
}

static void ReadCfg()
{
    g_Cfg = Cfg();

    KeyValues::AutoDelete kv("LuckyChicken");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/configs/LuckyChicken/LuckyChicken.ini"))
        return;

    g_Cfg.min_players        = kv->GetInt("min_players", 4);
    g_Cfg.server_id          = kv->GetInt("server_id", 1);
    g_Cfg.commands_open_menu = kv->GetString("commands_open_menu", "chickenstats;cstats");

    g_Cfg.show_top_lucky   = kv->GetInt("state_top_luckychicken", 1) != 0;
    g_Cfg.show_top_unlucky = kv->GetInt("state_top_unluckychicken", 1) != 0;
    g_Cfg.show_top_money   = kv->GetInt("state_top_money", 1) != 0;
    g_Cfg.show_in_shop     = kv->GetInt("state_shop_functions", 1) != 0;

    g_Cfg.top_size = kv->GetInt("number_players_top", 10);

    g_Cfg.min_credits = kv->GetInt("min_credits", 10);
    g_Cfg.max_credits = kv->GetInt("max_credits", 100);
    g_Cfg.ignore_mult = kv->GetInt("state_credits_multiplayer", 1) != 0;

    g_Cfg.chance = kv->GetInt("chance_luckychicken", 30);

    g_Cfg.damage_on_unlucky = kv->GetInt("state_damage_unlucky_chicken", 1) != 0;
    g_Cfg.min_damage  = kv->GetInt("min_damage", 1);
    g_Cfg.max_damage  = kv->GetInt("max_damage", 5);
    g_Cfg.damage_kind = kv->GetInt("type_damage", 1);

    g_Cfg.msg_lucky_all    = kv->GetInt("message_kill_luckychicken_all", 1) != 0;
    g_Cfg.msg_lucky_self   = kv->GetInt("message_kill_luckychicken_client", 1) != 0;
    g_Cfg.msg_unlucky_self = kv->GetInt("message_kill_unluckychicken_client", 1) != 0;

    g_Cfg.play_sound  = kv->GetInt("state_sound_client", 1) != 0;
    g_Cfg.snd_lucky   = kv->GetString("path_sound_luckychicken", "BuyZone.Activate");
    g_Cfg.snd_unlucky = kv->GetString("path_sound_unluckychicken", "Chicken.Pain");

    g_Cfg.min_count = kv->GetInt("min_count_chickenspawn", 5);
    g_Cfg.max_count = kv->GetInt("max_count_chickenspawn", 12);

    g_Cfg.log_kills = kv->GetInt("log_file", 1) != 0;

    g_Cfg.db_section = kv->GetString("db_section", "luckychicken");

    if (KeyValues* maps = kv->FindKey("Maps", false))
    {
        for (KeyValues* m = maps->GetFirstTrueSubKey(); m; m = m->GetNextTrueSubKey())
        {
            std::string name = m->GetName();
            std::vector<Vector> v;
            for (KeyValues* p = m->GetFirstValue(); p; p = p->GetNextValue())
            {
                const char* val = p->GetString();
                if (!val || !*val) continue;
                float x = 0, y = 0, z = 0;
                if (sscanf(val, "%f %f %f", &x, &y, &z) == 3)
                    v.emplace_back(x, y, z);
            }
            if (!v.empty()) g_Cfg.spots[name] = std::move(v);
        }
    }
}

static void ReadDB()
{
    KeyValues::AutoDelete kv("Databases");
    if (!kv->LoadFromFile(g_pFullFileSystem, "addons/configs/databases.cfg")) return;

    KeyValues* e = kv->FindKey(g_Cfg.db_section.c_str(), false);
    if (!e) return;

    g_Cfg.db_host = e->GetString("host",     "127.0.0.1");
    g_Cfg.db_user = e->GetString("user",     "");
    g_Cfg.db_pass = e->GetString("pass",     "");
    g_Cfg.db_base = e->GetString("database", "");
    g_Cfg.db_port = e->GetInt("port",        3306);
}

static void DbInit()
{
    if (!g_pDB) return;
    char q[1024];
    V_snprintf(q, sizeof(q),
        "CREATE TABLE IF NOT EXISTS `%s` ("
        "`steamid64` BIGINT UNSIGNED NOT NULL,"
        "`server_id` INT UNSIGNED NOT NULL DEFAULT 1,"
        "`name` VARCHAR(64) NOT NULL DEFAULT 'unknown',"
        "`lucky_kills` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`unlucky_kills` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`total_earned` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`last_seen` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "PRIMARY KEY (`steamid64`, `server_id`)"
        ") DEFAULT CHARSET=utf8mb4;", Tbl());
    g_pDB->Query(q, [](ISQLQuery*) {});
}

static void DbConnect()
{
    if (g_Cfg.db_host.empty() || g_Cfg.db_base.empty()) return;

    int ret = 0;
    ISQLInterface* sql = (ISQLInterface*)g_SMAPI->MetaFactory(SQLMM_INTERFACE, &ret, nullptr);
    if (!sql || ret == META_IFACE_FAILED) return;

    IMySQLClient* cli = sql->GetMySQLClient();
    if (!cli) return;

    MySQLConnectionInfo info;
    info.host     = g_Cfg.db_host.c_str();
    info.user     = g_Cfg.db_user.c_str();
    info.pass     = g_Cfg.db_pass.c_str();
    info.database = g_Cfg.db_base.c_str();
    info.port     = g_Cfg.db_port;

    g_pDB = cli->CreateMySQLConnection(info);
    if (!g_pDB) return;

    g_pDB->Connect([](bool ok) {
        if (!ok) { g_pDB = nullptr; return; }
        DbInit();
    });
}

static void DbLoad(uint64_t sid)
{
    if (!g_pDB || sid == 0) return;
    char q[256];
    V_snprintf(q, sizeof(q),
        "SELECT lucky_kills, unlucky_kills, total_earned FROM `%s` "
        "WHERE steamid64=%llu AND server_id=%d LIMIT 1;",
        Tbl(), (unsigned long long)sid, g_Cfg.server_id);

    g_pDB->Query(q, [sid](ISQLQuery* qr) {
        if (!qr) return;
        ISQLResult* r = qr->GetResultSet();
        if (!r) return;
        Stats& s = g_St[sid];
        s.fetched = true;
        if (r->GetRowCount() > 0 && r->MoreRows())
        {
            r->FetchRow();
            s.lucky   = r->GetInt(0);
            s.unlucky = r->GetInt(1);
            s.earned  = r->GetInt(2);
        }
    });
}

static void DbSave(uint64_t sid, const char* nick, const Stats& s)
{
    if (!g_pDB || sid == 0) return;
    std::string n = g_pDB->Escape(nick ? nick : "unknown");
    if (n.size() > 60) n.resize(60);
    char q[1024];
    V_snprintf(q, sizeof(q),
        "INSERT INTO `%s` (steamid64, server_id, name, lucky_kills, unlucky_kills, total_earned) "
        "VALUES (%llu, %d, '%s', %d, %d, %d) "
        "ON DUPLICATE KEY UPDATE "
        "name=VALUES(name), lucky_kills=VALUES(lucky_kills), unlucky_kills=VALUES(unlucky_kills), total_earned=VALUES(total_earned);",
        Tbl(), (unsigned long long)sid, g_Cfg.server_id, n.c_str(),
        s.lucky, s.unlucky, s.earned);
    g_pDB->Query(q, [](ISQLQuery*) {});
}

enum TopKind { T_LUCKY = 0, T_UNLUCKY = 1, T_MONEY = 2 };
struct Row { std::string name; int v; };
typedef std::function<void(std::vector<Row>)> TopCb;

static void DbTop(TopKind k, int n, TopCb cb)
{
    if (!g_pDB) { cb({}); return; }
    const char* col = "lucky_kills";
    if      (k == T_UNLUCKY) col = "unlucky_kills";
    else if (k == T_MONEY)   col = "total_earned";

    char q[512];
    V_snprintf(q, sizeof(q),
        "SELECT name, %s FROM `%s` "
        "WHERE server_id=%d AND %s > 0 "
        "ORDER BY %s DESC LIMIT %d;",
        col, Tbl(), g_Cfg.server_id, col, col, n);

    g_pDB->Query(q, [cb](ISQLQuery* qr) {
        std::vector<Row> rows;
        if (qr) {
            ISQLResult* r = qr->GetResultSet();
            if (r) {
                while (r->MoreRows()) {
                    r->FetchRow();
                    Row row;
                    const char* nm = r->GetString(0);
                    row.name = nm ? nm : "?";
                    row.v = r->GetInt(1);
                    rows.push_back(std::move(row));
                }
            }
        }
        cb(std::move(rows));
    });
}

static void Wipeout()
{
    for (auto& b : g_Birds)
    {
        CBaseEntity* e = b.h.Get();
        if (e) g_pUtils->RemoveEntity((CEntityInstance*)e);
    }
    g_Birds.clear();
}

static CBaseEntity* HatchOne(const Vector& pos)
{
    CBaseEntity* e = (CBaseEntity*)g_pUtils->CreateEntityByName("chicken", CEntityIndex(-1));
    if (!e) return nullptr;
    QAngle a(0, (float)(rand() % 360), 0);
    CEntityKeyValues* kv = new CEntityKeyValues();
    g_pUtils->DispatchSpawn((CEntityInstance*)e, kv);
    g_pUtils->TeleportEntity(e, &pos, &a, nullptr);
    return e;
}

static void RattleTheCoop()
{
    Wipeout();
    if (Humans() < g_Cfg.min_players) return;

    auto it = g_Cfg.spots.find(g_Map);
    if (it == g_Cfg.spots.end() || it->second.empty()) return;

    const auto& pts = it->second;
    int lo = std::max(0, g_Cfg.min_count);
    int hi = std::max(lo, g_Cfg.max_count);
    int want = lo + (rand() % (hi - lo + 1));
    want = std::min(want, (int)pts.size());

    std::vector<int> idx;
    idx.reserve(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) idx.push_back((int)i);
    for (int i = (int)idx.size() - 1; i > 0; --i)
    {
        int j = rand() % (i + 1);
        std::swap(idx[i], idx[j]);
    }

    for (int i = 0; i < want && i < (int)idx.size(); ++i)
    {
        CBaseEntity* e = HatchOne(pts[idx[i]]);
        if (!e) continue;
        Bird b;
        b.h     = CHandle<CBaseEntity>(e);
        b.ent   = e->entindex();
        b.lucky = (rand() % 100) < g_Cfg.chance;
        g_Birds.push_back(b);
    }
}

static Bird* WhoIs(int ent)
{
    for (auto& b : g_Birds)
        if (b.ent == ent && !b.gone) return &b;
    return nullptr;
}

static void Pluck(int slot, Bird* b, const char* weapon)
{
    if (!b || b->gone) return;
    b->gone = true;

    if (slot < 0 || slot >= 64) return;
    if (!g_pPlayers->IsInGame(slot) || g_pPlayers->IsFakeClient(slot)) return;

    uint64_t sid = g_pPlayers->GetSteamID64(slot);
    const char* nick = g_pPlayers->GetPlayerName(slot);
    Stats& s = g_St[sid];

    if (b->lucky)
    {
        int credits = g_Cfg.min_credits;
        if (g_Cfg.max_credits > g_Cfg.min_credits)
            credits += rand() % (g_Cfg.max_credits - g_Cfg.min_credits + 1);

        s.lucky  += 1;
        s.earned += credits;

        if (g_pShop && g_pShop->CoreIsLoaded())
        {
            CurrencyType ct = g_Cfg.ignore_mult ? CurrencyType::Native : CurrencyType::ItemBuy;
            g_pShop->GiveClientCurrency(slot, credits, kCurrency, "lucky_chicken", ct, false);
        }

        if (g_Cfg.play_sound) BlastSound(slot, g_Cfg.snd_lucky.c_str());

        if (g_Cfg.msg_lucky_all)
        {
            SayAllK("Lucky_Chicken_Kill_All",
                "{LIGHTGREEN}%s{DEFAULT} got {LIGHTGREEN}%d{DEFAULT} credits for a {LIGHTGREEN}lucky{DEFAULT} chicken.",
                nick ? nick : "Player", credits);
        }
        else if (g_Cfg.msg_lucky_self)
        {
            SayK(slot, "Lucky_Chicken_Kill",
                "You got {LIGHTGREEN}%d{DEFAULT} credits for a {LIGHTGREEN}lucky{DEFAULT} chicken.",
                credits);
        }

        Squawk("LUCKY  | %s (%llu) | weapon=%s | +%d credits | total=%d",
            nick ? nick : "?", (unsigned long long)sid,
            weapon && *weapon ? weapon : "?", credits, s.earned);
    }
    else
    {
        s.unlucky += 1;

        if (g_Cfg.damage_on_unlucky)
        {
            int dmg = g_Cfg.min_damage;
            if (g_Cfg.max_damage > g_Cfg.min_damage)
                dmg += rand() % (g_Cfg.max_damage - g_Cfg.min_damage + 1);

            CCSPlayerController* pc = CCSPlayerController::FromSlot(slot);
            if (pc)
            {
                CCSPlayerPawn* pawn = pc->GetPlayerPawn();
                if (pawn && pawn->IsAlive())
                {
                    int hp  = pawn->m_iHealth();
                    int now = std::max(1, hp - dmg);
                    pawn->m_iHealth(now);
                    g_pUtils->SetStateChanged((CBaseEntity*)pawn, "CBaseEntity", "m_iHealth");

                    if (g_Cfg.msg_unlucky_self)
                        SayK(slot, "Unlucky_Chicken_Kill",
                            "You killed a {LIGHTRED}sad{DEFAULT} chicken — {LIGHTRED}-%d HP{DEFAULT}.", dmg);
                }
            }
        }
        else if (g_Cfg.msg_unlucky_self)
        {
            SayK(slot, "Unlucky_Chicken_Kill_NoDamage",
                "You killed a {LIGHTRED}sad{DEFAULT} chicken.");
        }

        if (g_Cfg.play_sound) BlastSound(slot, g_Cfg.snd_unlucky.c_str());

        Squawk("SAD    | %s (%llu) | weapon=%s",
            nick ? nick : "?", (unsigned long long)sid,
            weapon && *weapon ? weapon : "?");
    }

    DbSave(sid, nick, s);
}

static void OnDeath(const char*, IGameEvent* e, bool)
{
    if (!e) return;
    int otherId  = e->GetInt("otherid", -1);
    int slot     = e->GetInt("attacker", -1);
    const char* w = e->GetString("weapon", "");
    Bird* b = WhoIs(otherId);
    if (!b) return;
    Pluck(slot, b, w);
}

static void RoundStart(const char*, IGameEvent*, bool)
{
    g_pUtils->CreateTimer(0.20f, []() -> float { RattleTheCoop(); return -1.0f; });
}

static void RoundEnd(const char*, IGameEvent*, bool) { Wipeout(); }
static void RoundPre(const char*, IGameEvent*, bool) { Wipeout(); }

static void OnSpawn(const char*, IGameEvent* e, bool)
{
    if (!e) return;
    int slot = e->GetInt("userid", -1);
    if (slot < 0 || slot >= 64) return;
    if (!g_pPlayers->IsInGame(slot) || g_pPlayers->IsFakeClient(slot)) return;
    uint64_t sid = g_pPlayers->GetSteamID64(slot);
    if (sid == 0) return;
    auto it = g_St.find(sid);
    if (it == g_St.end() || !it->second.fetched) DbLoad(sid);
}

static void MapStart(const char* name)
{
    g_Map = Norm(name);
    if (g_Map.empty())
    {
        CGlobalVars* gv = g_pUtils->GetCGlobalVars();
        if (gv) g_Map = Norm(gv->mapname.ToCStr());
    }
    ReadCfg();
    ReadPhrases();
    g_St.clear();
    g_Birds.clear();
}

static void MapEnd() { Wipeout(); }

static void Menu_Main(int slot);
static void Menu_Mine(int slot);
static void Menu_Top(int slot, TopKind k);
static void Menu_Online(int slot);

static void Menu_Main(int slot)
{
    if (!g_pMenus) return;

    Menu m;
    char title[128];
    V_snprintf(title, sizeof(title), Tr("Menu_Main_Title", "Chickens | live: %d"), Alive());
    g_pMenus->SetTitleMenu(m, title);

    g_pMenus->AddItemMenu(m, "mine", Tr("Menu_My_Stats", "My stats"), ITEM_DEFAULT);
    if (g_Cfg.show_top_lucky)
        g_pMenus->AddItemMenu(m, "tl", Tr("Menu_Top_Lucky", "Top: lucky"), ITEM_DEFAULT);
    if (g_Cfg.show_top_unlucky)
        g_pMenus->AddItemMenu(m, "tu", Tr("Menu_Top_Unlucky", "Top: unlucky"), ITEM_DEFAULT);
    if (g_Cfg.show_top_money)
        g_pMenus->AddItemMenu(m, "tm", Tr("Menu_Top_Money", "Top: earnings"), ITEM_DEFAULT);
    g_pMenus->AddItemMenu(m, "on", Tr("Menu_Online_Stats", "Online players"), ITEM_DEFAULT);

    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int s) {
        if (!back || !*back) return;
        if      (!strcmp(back, "mine")) Menu_Mine(s);
        else if (!strcmp(back, "tl"))   Menu_Top(s, T_LUCKY);
        else if (!strcmp(back, "tu"))   Menu_Top(s, T_UNLUCKY);
        else if (!strcmp(back, "tm"))   Menu_Top(s, T_MONEY);
        else if (!strcmp(back, "on"))   Menu_Online(s);
    });

    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void Menu_Mine(int slot)
{
    if (!g_pMenus) return;
    uint64_t sid = g_pPlayers->GetSteamID64(slot);
    Stats st;
    auto it = g_St.find(sid);
    if (it != g_St.end()) st = it->second;

    Menu m;
    g_pMenus->SetTitleMenu(m, Tr("Menu_PersonalStats_Title", "My stats"));

    char buf[128];
    V_snprintf(buf, sizeof(buf), Tr("Menu_PersonalStats_Lucky",   "Lucky: %d"),   st.lucky);
    g_pMenus->AddItemMenu(m, "_l", buf, ITEM_DISABLED);
    V_snprintf(buf, sizeof(buf), Tr("Menu_PersonalStats_Unlucky", "Unlucky: %d"), st.unlucky);
    g_pMenus->AddItemMenu(m, "_u", buf, ITEM_DISABLED);
    V_snprintf(buf, sizeof(buf), Tr("Menu_PersonalStats_Earned",  "Earned: %d"),  st.earned);
    g_pMenus->AddItemMenu(m, "_e", buf, ITEM_DISABLED);

    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int s) {
        if (back && !strcmp(back, "back")) Menu_Main(s);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static void Menu_Top(int slot, TopKind k)
{
    if (!g_pMenus) return;
    if (!g_pDB) { SayK(slot, "Menu_DB_NotConnected", "{LIGHTRED}DB not connected."); return; }

    {
        Menu wait;
        g_pMenus->SetTitleMenu(wait, Tr("Menu_DB_Loading", "Loading..."));
        g_pMenus->SetExitMenu(wait, true);
        g_pMenus->DisplayPlayerMenu(wait, slot, true, true);
    }

    int n = std::max(1, g_Cfg.top_size);
    DbTop(k, n, [slot, k](std::vector<Row> rows) {
        Menu m;
        const char* tk = "Menu_TopLucky_Title";
        const char* tf = "Top: lucky";
        if      (k == T_UNLUCKY) { tk = "Menu_TopUnlucky_Title"; tf = "Top: unlucky"; }
        else if (k == T_MONEY)   { tk = "Menu_TopMoney_Title";   tf = "Top: earnings"; }
        g_pMenus->SetTitleMenu(m, Tr(tk, tf));

        if (rows.empty())
        {
            g_pMenus->AddItemMenu(m, "_e", Tr("Menu_Top_Empty", "No data yet"), ITEM_DISABLED);
        }
        else
        {
            char ln[256];
            int i = 1;
            for (const auto& r : rows)
            {
                V_snprintf(ln, sizeof(ln), Tr("Menu_Top_Entry", "%d. %s — %d"),
                    i++, r.name.c_str(), r.v);
                g_pMenus->AddItemMenu(m, "_r", ln, ITEM_DISABLED);
            }
        }

        g_pMenus->SetBackMenu(m, true);
        g_pMenus->SetExitMenu(m, true);
        g_pMenus->SetCallback(m, [](const char* back, const char*, int, int s) {
            if (back && !strcmp(back, "back")) Menu_Main(s);
        });
        g_pMenus->DisplayPlayerMenu(m, slot, true, true);
    });
}

static void Menu_Online(int slot)
{
    if (!g_pMenus) return;

    Menu m;
    g_pMenus->SetTitleMenu(m, Tr("Menu_Online_Title", "Online players"));

    std::vector<std::pair<std::string, Stats>> rows;
    for (int i = 0; i < 64; ++i)
    {
        if (!g_pPlayers->IsInGame(i) || g_pPlayers->IsFakeClient(i)) continue;
        uint64_t sid = g_pPlayers->GetSteamID64(i);
        if (sid == 0) continue;
        Stats st;
        auto it = g_St.find(sid);
        if (it != g_St.end()) st = it->second;
        const char* nm = g_pPlayers->GetPlayerName(i);
        rows.emplace_back(nm ? nm : "?", st);
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b){ return a.second.earned > b.second.earned; });

    if (rows.empty())
    {
        g_pMenus->AddItemMenu(m, "_e", Tr("Menu_Top_Empty", "No data yet"), ITEM_DISABLED);
    }
    else
    {
        char ln[256];
        int i = 1;
        for (const auto& kv : rows)
        {
            V_snprintf(ln, sizeof(ln), "%d. %s (%d/%d, $%d)",
                i++, kv.first.c_str(), kv.second.lucky, kv.second.unlucky, kv.second.earned);
            g_pMenus->AddItemMenu(m, "_r", ln, ITEM_DISABLED);
        }
    }

    g_pMenus->SetBackMenu(m, true);
    g_pMenus->SetExitMenu(m, true);
    g_pMenus->SetCallback(m, [](const char* back, const char*, int, int s) {
        if (back && !strcmp(back, "back")) Menu_Main(s);
    });
    g_pMenus->DisplayPlayerMenu(m, slot, true, true);
}

static bool Cmd_Stats(int slot, const char*)
{
    if (slot < 0) return true;
    if (Humans() < g_Cfg.min_players)
        SayK(slot, "Min_Players_Required",
            "Minimum {LIGHTGREEN}%d{DEFAULT} players required.", g_Cfg.min_players);
    Menu_Main(slot);
    return true;
}

static void Boot()
{
    g_pGameEntitySystem = g_pUtils->GetCGameEntitySystem();
    g_pEntitySystem     = g_pUtils->GetCEntitySystem();
    gpGlobals           = g_pUtils->GetCGlobalVars();
    ReadDB();
    DbConnect();
}

CGameEntitySystem* GameEntitySystem()
{
    return g_pUtils ? g_pUtils->GetCGameEntitySystem() : nullptr;
}

bool LuckyChicken::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();
    GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
    GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetEngineFactory, engine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
    GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
    if (!g_pSource2Server)
        GET_V_IFACE_ANY(GetServerFactory, g_pSource2Server, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);

    srand((unsigned)time(nullptr));
    g_SMAPI->AddListener(this, this);
    return true;
}

bool LuckyChicken::Unload(char* error, size_t maxlen)
{
    ConVar_Unregister();
    if (g_pUtils) { Wipeout(); g_pUtils->ClearAllHooks(g_PLID); }
    if (g_pDB) { g_pDB->Destroy(); g_pDB = nullptr; }
    return true;
}

void LuckyChicken::AllPluginsLoaded()
{
    char err[64]; int ret = 0;

    g_pUtils = (IUtilsApi*)g_SMAPI->MetaFactory(Utils_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pUtils)
    {
        g_SMAPI->Format(err, sizeof(err), "Missing Utils API");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), err);
        if (engine) { std::string s = "meta unload " + std::to_string(g_PLID); engine->ServerCommand(s.c_str()); }
        return;
    }

    g_pPlayers = (IPlayersApi*)g_SMAPI->MetaFactory(PLAYERS_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED || !g_pPlayers)
    {
        g_SMAPI->Format(err, sizeof(err), "Missing Players API");
        ConColorMsg(Color(255, 0, 0, 255), "[%s] %s\n", GetLogTag(), err);
        if (engine) { std::string s = "meta unload " + std::to_string(g_PLID); engine->ServerCommand(s.c_str()); }
        return;
    }

    g_pMenus = (IMenusApi*)g_SMAPI->MetaFactory(Menus_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) g_pMenus = nullptr;

    g_pShop = (IShopApi*)g_SMAPI->MetaFactory(SHOP_INTERFACE, &ret, nullptr);
    if (ret == META_IFACE_FAILED) g_pShop = nullptr;

    ReadCfg();
    ReadPhrases();

    g_pUtils->StartupServer(g_PLID, Boot);
    g_pUtils->MapStartHook(g_PLID, MapStart);
    g_pUtils->MapEndHook(g_PLID, MapEnd);

    g_pUtils->HookEvent(g_PLID, "round_start",    RoundStart);
    g_pUtils->HookEvent(g_PLID, "round_end",      RoundEnd);
    g_pUtils->HookEvent(g_PLID, "round_prestart", RoundPre);
    g_pUtils->HookEvent(g_PLID, "other_death",    OnDeath);
    g_pUtils->HookEvent(g_PLID, "player_spawn",   OnSpawn);

    std::vector<std::string> chat;
    {
        std::string s = g_Cfg.commands_open_menu;
        size_t st = 0;
        while (true)
        {
            size_t p = s.find(';', st);
            std::string c = (p == std::string::npos) ? s.substr(st) : s.substr(st, p - st);
            while (!c.empty() && (c.front() == ' ' || c.front() == '\t')) c.erase(c.begin());
            while (!c.empty() && (c.back()  == ' ' || c.back()  == '\t')) c.pop_back();
            if (!c.empty())
            {
                if (c[0] != '!' && c[0] != '/') c = "!" + c;
                chat.push_back(c);
            }
            if (p == std::string::npos) break;
            st = p + 1;
        }
    }
    g_pUtils->RegCommand(g_PLID, {"mm_chickenstats"}, chat, Cmd_Stats);

    if (g_pShop)
    {
        g_pShop->HookOnCoreIsReady(g_PLID, []() {
            if (g_Cfg.show_in_shop)
                g_pShop->RegisterFunction("luckychicken_stats", [](int s) { Menu_Main(s); });
        });
    }
}

const char *LuckyChicken::GetLicense()
{
    return "Public";
}

const char *LuckyChicken::GetVersion()
{
    return "1.0.0";
}

const char *LuckyChicken::GetDate()
{
    return __DATE__;
}

const char *LuckyChicken::GetLogTag()
{
    return "[LuckyChicken]";
}

const char *LuckyChicken::GetAuthor()
{
    return "_ded_cookies";
}

const char *LuckyChicken::GetDescription()
{
    return "LuckyChicken module for Shop Core by Pisex";
}

const char *LuckyChicken::GetName()
{
    return "LuckyChicken";
}

const char *LuckyChicken::GetURL()
{
    return "https://api.onlypublic.net/";
}