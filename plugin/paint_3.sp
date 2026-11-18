#pragma semicolon 1
#pragma newdecls required

#include <sourcemod>
#include <sdktools>
#include <clientprefs>

// 5.0: the Portal 2 gel is gone (archived with the p2paint mod: branch archive/gel). Classic paint decals in
//      8 sizes, any colour and pictures for clients with the p2paint mod (version 10+: it draws the decals
//      itself, p2paint_decal), the original decals for everyone else. Saved paint, profiles, backups, copy,
//      erase and recolor as before.
// 5.1: paint lands on props, displacements, brush entities, clips (while the player sees them: !showclips),
//      triggers (while !st shows them) and invisible walls: the aim stops there again (as in 4.x), and the
//      surface + its normal are saved and sent; the p2paint mod (version 11) draws paint on them.
// 5.2: the p2paint menu has the backups / copy / clear pages itself: sm_paintui sends it the lists
//      (p2paint_ui_list) and takes load / delete / copy by list position, never a name from the client.
//      Copying paint into a backup profile no longer wipes "default" (it copies into the current profile).
// 5.3: (audit) a new paint load stops the one still running, loads page by id; backups / copies: one per
//      player every 3 s, at most MAX_PROFILES per map; nothing is saved or loaded before Steam has confirmed
//      the player; picture names capped; no chat line per 50 splats while loading.
#define PLUGIN_VERSION "5.3"
#define COPY_COOLDOWN 3.0
#define MAX_PROFILES 30			// per player per map, backups included
#define MAX_PICTURES 4096		// picture names remembered (any client can send any valid name)

#define CACHE_BLOCK 10			// cache entry: x y z radius rgb picture nx ny nz surface (picture = index into g_hPicNames + 1, 0 = colour)
#define MOD_VERSION 10			// p2paint_version this plugin talks to (older mods get the classic decals)

// what a splat landed on (saved in the surf column; -1 = not known, saved before 5.1). The p2paint mod 11+
// paints all of them; the engine's decals only stick to the world (and displacements).
#define PAINTSURF_UNKNOWN -1
#define PAINTSURF_WORLD 0
#define PAINTSURF_DISPLACEMENT 1
#define PAINTSURF_ENTITY 2			// brush entities that stay put: func_brush, func_illusionary, doors, ...
#define PAINTSURF_PROP 3
#define PAINTSURF_CLIP 4
#define PAINTSURF_MOVING 5			// brush entities that keep moving (func_rotating, trains, ...): no paint
#define PAINTSURF_TRIGGER 6

// brush entities that aren't surfaces (volumes) or keep moving
char g_cMovingClasses[][] =
{
    "func_rotating", "func_tracktrain", "func_train", "func_physbox", "func_physbox_multiplayer",
    "momentary_rot_button", "func_movelinear", "func_platrot", "func_conveyor", "func_water_analog"
};
char g_cVolumeClasses[][] =
{
    "func_areaportal", "func_areaportalwindow", "func_occluder", "func_smokevolume", "func_dustmotes",
    "func_dustcloud", "func_precipitation", "func_viscluster"
};

// what the player can see: triggers (improved-st, !st) and clips (showplayerclips, !showclips)
native bool ShowTriggers_ClientSees(int client, const char[] classname);
native bool ShowPlayerClips_ClientSees(int client);
#define LOAD_BATCH 50

// the 8 paint sizes: radius = what shows (61.5 px of the 128 px decal x $decalscale); the original three
// ("", _med, _large; on FastDL) are sizes 2, 5 and 8, and players without the mod get the nearest of them
float g_fDecalRadius[] = { 6.2, 8.6, 12.3, 18.5, 27.7, 40.0, 55.4, 73.8 };
int g_iOriginalSlot[] = { 0, 0, 0, 1, 1, 1, 2, 2 };		// size -> g_Sprites[][slot]
char g_cOriginalSuffix[][] = { "", "_med", "_large" };
// rows from before 4.0 only stored an original decal size (0-2)
float g_fLegacyRadius[] = { 8.6, 27.7, 73.8 };

Menu g_hMainMenu;

float g_fLastPaint[MAXPLAYERS + 1][3];
bool g_bIsPainting[MAXPLAYERS + 1];
char g_CurrentProfile[MAXPLAYERS + 1][64];
bool g_bHasMod[MAXPLAYERS + 1];			// client runs the p2paint mod (MOD_VERSION+)
bool g_bModKnown[MAXPLAYERS + 1];		// the p2paint_version query came back

// current paint settings: what the next splat gets
int g_PaintColourIdx[MAXPLAYERS + 1];	// 0 random, 1.. preset, -1 custom (g_PaintRGB)
int g_PaintRGB[MAXPLAYERS + 1][3];
float g_PaintRadius[MAXPLAYERS + 1];
int g_PaintPic[MAXPLAYERS + 1];			// 0 = the colour, else picture index + 1
bool g_bRecoloring[MAXPLAYERS + 1];		// recoloring right now: paint you aim at takes the current colour / picture
bool g_bRecolorTool[MAXPLAYERS + 1];	// menu switch: the paint key + left click recolors instead of painting

Database g_hDatabase;
Handle g_hCurrentProfileCookie;
Handle g_hPlayerPaintColour;			// legacy preset index, still written for older versions
Handle g_hPlayerPaintSize;				// legacy decal size index, read once
Handle g_hColourCookie;					// "random", "preset N" or "R G B"
Handle g_hRadiusCookie;
Handle g_hPicCookie;

char g_cPaintColours[][2][64] =
{
    {"Random", "random"},
    {"White", "paint_white"},
    {"Black", "paint_black"},
    {"Blue", "paint_blue"},
    {"Light Blue", "paint_lightblue"},
    {"Brown", "paint_brown"},
    {"Cyan", "paint_cyan"},
    {"Green", "paint_green"},
    {"Dark Green", "paint_darkgreen"},
    {"Red", "paint_red"},
    {"Orange", "paint_orange"},
    {"Yellow", "paint_yellow"},
    {"Pink", "paint_pink"},
    {"Light Pink", "paint_lightpink"},
    {"Purple", "paint_purple"}
};

// RGB of each preset (same as the classic decal materials)
int g_iPresetRGB[][3] =
{
    {0, 0, 0},
    {255, 255, 255},
    {0, 0, 0},
    {0, 0, 255},
    {135, 206, 250},
    {139, 69, 19},
    {0, 255, 255},
    {0, 255, 0},
    {0, 100, 0},
    {255, 0, 0},
    {255, 165, 0},
    {255, 255, 0},
    {255, 20, 147},
    {255, 182, 193},
    {128, 0, 128}
};

int g_Sprites[14][3];

// Pictures are the p2paint client's (made in its menu from any image / GIF, or the game's sprays): the server
// only keeps their names. The DB stores the name (pic column); the cache keeps an index into this list.
ArrayList g_hPicNames;

// Point-and-erase: per-client cache of this client's splats on the current map+profile
// (CACHE_BLOCK cells each). Kept in sync with the DB so erasing and redraws need no query.
ArrayList g_hPaintCache[MAXPLAYERS + 1];
// what the p2paint menu's lists show this player (positions in them are what the menu sends back)
ArrayList g_hUIBackups[MAXPLAYERS + 1];		// profile names
ArrayList g_hUIPainters[MAXPLAYERS + 1];	// steam ids
int g_iLoadGen[MAXPLAYERS + 1];				// bumped when a paint load starts: batches of an older load are dropped
int g_iLoadWaited[MAXPLAYERS + 1];			// Timer_LoadPaint's half-second waits for the mod query
float g_fNextCopy[MAXPLAYERS + 1];			// GetEngineTime() of the next backup / copy allowed
bool g_bErasing[MAXPLAYERS + 1];
bool g_bRedrawActive[MAXPLAYERS + 1];
bool g_bRedrawDirty[MAXPLAYERS + 1];
int g_iRedrawOffset[MAXPLAYERS + 1];

public Plugin myinfo =
{
    name = "super paint",
    author = "gangy",
    description = "saving; any colour and pictures with the p2paint client mod",
    version = PLUGIN_VERSION,
    url = ""
};

public APLRes AskPluginLoad2(Handle myself, bool late, char[] error, int err_max)
{
    MarkNativeAsOptional("ShowTriggers_ClientSees");
    MarkNativeAsOptional("ShowPlayerClips_ClientSees");
    return APLRes_Success;
}

public void OnPluginStart()
{
    CreateConVar("paint_version", PLUGIN_VERSION, "Paint plugin version", FCVAR_NOTIFY);

    g_hPlayerPaintColour = RegClientCookie("paint_playerpaintcolour", "paint_playerpaintcolour", CookieAccess_Protected);
    g_hPlayerPaintSize = RegClientCookie("paint_playerpaintsize", "paint_playerpaintsize", CookieAccess_Protected);
    g_hCurrentProfileCookie = RegClientCookie("paint_currentprofile", "Current paint profile name", CookieAccess_Protected);
    g_hColourCookie = RegClientCookie("paint_colour", "Paint colour: random, preset N or R G B", CookieAccess_Protected);
    g_hRadiusCookie = RegClientCookie("paint_radius", "Paint splat radius", CookieAccess_Protected);
    g_hPicCookie = RegClientCookie("paint_pic", "Paint picture name (empty = the colour)", CookieAccess_Protected);

    g_hPicNames = new ArrayList(ByteCountToCells(48));

    RegConsoleCmd("+paint", cmd_EnablePaint);
    RegConsoleCmd("-paint", cmd_DisablePaint);
    RegConsoleCmd("+erase", cmd_EnableErase);
    RegConsoleCmd("-erase", cmd_DisableErase);
    RegConsoleCmd("sm_paintfire", cmd_PaintFire);
    RegConsoleCmd("sm_sizeup", cmd_SizeUp);
    RegConsoleCmd("sm_sizedown", cmd_SizeDown);
    RegConsoleCmd("sm_paintcolour", cmd_PaintColour);
    RegConsoleCmd("sm_paintcolor", cmd_PaintColour);
    RegConsoleCmd("sm_paintrgb", cmd_PaintRGB);
    RegConsoleCmd("sm_paintsize", cmd_PaintSize);
    RegConsoleCmd("sm_clearpaint", cmd_ClearPaint);
    RegConsoleCmd("sm_loadbackup", cmd_LoadBackup);
    RegConsoleCmd("sm_paintbackups", cmd_LoadBackup);

    RegConsoleCmd("sm_paintmenu", cmd_PaintMenu);
    RegConsoleCmd("sm_paintmenuclassic", cmd_PaintMenuClassic);
    RegConsoleCmd("sm_paint", cmd_PaintMenu);
    RegConsoleCmd("sm_paintui", cmd_PaintUI);
    RegConsoleCmd("sm_paintwhy", cmd_PaintWhy, "What the paint aim hits where you look, and why (console)");

    g_hMainMenu = new Menu(MainMenuHandle, MenuAction_Select | MenuAction_DisplayItem);
    g_hMainMenu.SetTitle("Paint Menu:");
    g_hMainMenu.AddItem("color", "Colour");
    g_hMainMenu.AddItem("size", "Size");
    g_hMainMenu.AddItem("recolor", "Recolor tool");
    g_hMainMenu.AddItem("backups", "Backups");
    g_hMainMenu.AddItem("copy", "Copy Paint From Player");
    g_hMainMenu.AddItem("erase", "Point and Erase");
    g_hMainMenu.AddItem("clear", "Clear My Paint");

    ConnectToDatabase();

    for (int i = 1; i <= MaxClients; i++)
    {
        SetDefaultSettings(i);
        if (IsClientInGame(i) && !IsFakeClient(i))
        {
            QueryMod(i);
            if (AreClientCookiesCached(i))
            {
                OnClientCookiesCached(i);
            }
        }
    }
}

void ConnectToDatabase()
{
    char error[256];
    g_hDatabase = SQLite_UseDatabase("superpaint", error, sizeof(error));

    if (g_hDatabase == null)
    {
        LogError("Failed to connect to database: %s", error);
        SetFailState("Failed to connect to database");
        return;
    }

    char query[1024];
    Format(query, sizeof(query), "CREATE TABLE IF NOT EXISTS paint_data (id INTEGER PRIMARY KEY AUTOINCREMENT, steamid TEXT NOT NULL, map TEXT NOT NULL, pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL, pos_z FLOAT NOT NULL, paint_color INTEGER NOT NULL, paint_size INTEGER NOT NULL, profile TEXT DEFAULT 'default', player_name TEXT)");
    SQL_FastQuery(g_hDatabase, query);

    Format(query, sizeof(query), "CREATE TABLE IF NOT EXISTS player_names (steamid TEXT PRIMARY KEY, name TEXT NOT NULL, last_seen INTEGER NOT NULL)");
    SQL_FastQuery(g_hDatabase, query);

    // 4.x columns (nx ny nz surf: where paint landed, used again since 5.1; the other gel ones stay in the table,
    // unused, so copies of old paint keep them). SQLite has no
    // "ADD COLUMN IF NOT EXISTS": adding an existing column just fails, which is fine.
    static const char columns[][] =
    {
        "nx REAL", "ny REAL", "nz REAL", "radius REAL", "cr INTEGER", "cg INTEGER", "cb INTEGER", "bright REAL", "opacity REAL", "surf INTEGER", "glow REAL", "pic TEXT DEFAULT ''"
    };
    for (int i = 0; i < sizeof(columns); i++)
    {
        Format(query, sizeof(query), "ALTER TABLE paint_data ADD COLUMN %s", columns[i]);
        SQL_FastQuery(g_hDatabase, query);
    }
    SQL_FastQuery(g_hDatabase, "CREATE INDEX IF NOT EXISTS paint_owner ON paint_data (steamid, map, profile)");
}

void SetDefaultSettings(int client)
{
    g_PaintColourIdx[client] = 10;	// orange
    g_PaintRGB[client] = g_iPresetRGB[10];
    g_PaintRadius[client] = g_fDecalRadius[4];
    g_bRecoloring[client] = g_bRecolorTool[client] = false;
    g_PaintPic[client] = 0;
}

public void OnClientConnected(int client)
{
    strcopy(g_CurrentProfile[client], sizeof(g_CurrentProfile[]), "default");
    g_bHasMod[client] = false;
    g_bModKnown[client] = false;
    g_fNextCopy[client] = 0.0;
    SetDefaultSettings(client);
}

public void OnClientCookiesCached(int client)
{
    char sValue[64];

    GetClientCookie(client, g_hColourCookie, sValue, sizeof(sValue));
    if (sValue[0] != '\0')
    {
        ParseColourSetting(client, sValue);
    }
    else
    {
        // 3.x stored only a preset index
        GetClientCookie(client, g_hPlayerPaintColour, sValue, sizeof(sValue));
        if (sValue[0] != '\0')
        {
            int idx = StringToInt(sValue);
            if (idx >= 0 && idx < sizeof(g_cPaintColours))
            {
                SetPresetColour(client, idx, false);
            }
        }
    }

    GetClientCookie(client, g_hRadiusCookie, sValue, sizeof(sValue));
    if (sValue[0] != '\0')
    {
        g_PaintRadius[client] = g_fDecalRadius[DecalSize(ClampF(StringToFloat(sValue), 4.0, 160.0))];
    }
    else
    {
        // 3.x size: small / medium / large decal
        GetClientCookie(client, g_hPlayerPaintSize, sValue, sizeof(sValue));
        if (sValue[0] != '\0')
            g_PaintRadius[client] = g_fLegacyRadius[ClampI(StringToInt(sValue), 0, 2)];
    }

    GetClientCookie(client, g_hCurrentProfileCookie, sValue, sizeof(sValue));
    if (sValue[0] != '\0')
    {
        strcopy(g_CurrentProfile[client], sizeof(g_CurrentProfile[]), sValue);
    }

    GetClientCookie(client, g_hPicCookie, sValue, sizeof(sValue));
    g_PaintPic[client] = FindOrAddPicture(sValue);

    if (IsClientInGame(client) && !IsFakeClient(client))
    {
        UpdatePlayerName(client);
        if (g_bModKnown[client])
            SendUIState(client);	// the mod's ghost / hint bar show the saved settings
    }
}

float ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int ClampI(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void ParseColourSetting(int client, const char[] value)
{
    if (StrEqual(value, "random"))
    {
        g_PaintColourIdx[client] = 0;
        return;
    }
    char parts[3][8];
    if (StrContains(value, "preset ") == 0)
    {
        int idx = StringToInt(value[7]);
        if (idx >= 1 && idx < sizeof(g_cPaintColours))
        {
            g_PaintColourIdx[client] = idx;
            g_PaintRGB[client] = g_iPresetRGB[idx];
        }
        return;
    }
    if (ExplodeString(value, " ", parts, 3, 8) == 3)
    {
        g_PaintColourIdx[client] = -1;
        for (int i = 0; i < 3; i++)
            g_PaintRGB[client][i] = ClampI(StringToInt(parts[i]), 0, 255);
    }
}

public void OnClientDisconnect(int client)
{
    if (IsClientAuthorized(client) && !IsFakeClient(client))
    {
        UpdatePlayerName(client);
    }

    g_bErasing[client] = false;
    g_bHasMod[client] = false;
    g_bModKnown[client] = false;
    CancelPaintRedraw(client);
    delete g_hPaintCache[client];
    delete g_hUIBackups[client];
    delete g_hUIPainters[client];
}

// the Steam ID and map every paint row is keyed by. False until Steam has confirmed the player: an empty ID
// would put everyone's paint under one key.
bool PaintOwner(int client, char auth[64], char map[64])
{
    if (!GetClientAuthId(client, AuthId_Steam2, auth, sizeof(auth), true))
        return false;
    GetCurrentMap(map, sizeof(map));
    return true;
}

void UpdatePlayerName(int client)
{
    if (!IsClientInGame(client) || IsFakeClient(client) || g_hDatabase == null)
        return;

    char steamid[64];
    char name[MAX_NAME_LENGTH * 2 + 1];
    if (!GetClientAuthId(client, AuthId_Steam2, steamid, sizeof(steamid), true))
        return;
    GetClientName(client, name, sizeof(name));

    char escaped_name[MAX_NAME_LENGTH * 2 + 1];
    g_hDatabase.Escape(name, escaped_name, sizeof(escaped_name));

    char query[512];
    Format(query, sizeof(query), "REPLACE INTO player_names (steamid, name, last_seen) VALUES ('%s', '%s', %d)", steamid, escaped_name, GetTime());

    g_hDatabase.Query(SQL_ErrorCheckCallback, query);
}

public void OnMapStart()
{
    for (int i = 1; i <= MAXPLAYERS; i++)
    {
        g_bErasing[i] = false;
        g_bRecoloring[i] = false;
        g_iLoadGen[i]++;	// a load from the last map stops
        CancelPaintRedraw(i);
        delete g_hPaintCache[i];
    }

    // the classic decals, for players without the p2paint mod (the mod draws its own)
    char buffer[256];
    AddFileToDownloadsTable("materials/decals/paint/paint_decal.vtf");
    for (int colour = 1; colour < sizeof(g_cPaintColours); colour++)
    {
        for (int slot = 0; slot < 3; slot++)
        {
            Format(buffer, sizeof(buffer), "decals/paint/%s%s.vmt", g_cPaintColours[colour][1], g_cOriginalSuffix[slot]);
            g_Sprites[colour - 1][slot] = PrecachePaint(buffer);
        }
    }

    CreateTimer(0.1, Timer_Paint, _, TIMER_REPEAT | TIMER_FLAG_NO_MAPCHANGE);
}

// ==================== pictures ====================

// the name as the client wrote it: a-z 0-9 _ - (it becomes a material path and a console argument)
bool ValidPictureName(const char[] name)
{
    int len = strlen(name);
    if (len == 0 || len > 47)
        return false;
    for (int i = 0; i < len; i++)
    {
        int c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

// picture index + 1 for a name (added to the list the first time), 0 for none / a bad name
int FindOrAddPicture(const char[] name)
{
    if (!ValidPictureName(name))
        return 0;
    int idx = g_hPicNames.FindString(name);
    if (idx == -1)
    {
        if (g_hPicNames.Length >= MAX_PICTURES)
            return 0;	// shortcut: past the cap new pictures paint the colour; never emptied (cache entries index it)
        idx = g_hPicNames.PushString(name);
    }
    return idx + 1;
}

void PictureName(int pic, char[] buffer, int maxlength)
{
    if (pic > 0 && pic <= g_hPicNames.Length)
        g_hPicNames.GetString(pic - 1, buffer, maxlength);
    else
        strcopy(buffer, maxlength, "");
}

void SetPaintPicture(int client, const char[] name)
{
    g_PaintPic[client] = StrEqual(name, "-") || StrEqual(name, "none") ? 0 : FindOrAddPicture(name);
    char saved[48];
    PictureName(g_PaintPic[client], saved, sizeof(saved));
    if (AreClientCookiesCached(client))
        SetClientCookie(client, g_hPicCookie, saved);
}

// ==================== p2paint client detection ====================

void QueryMod(int client)
{
    QueryClientConVar(client, "p2paint_version", OnModQueried);
}

public void OnModQueried(QueryCookie cookie, int client, ConVarQueryResult result, const char[] cvarName, const char[] cvarValue)
{
    if (client > 0 && IsClientConnected(client))
    {
        g_bHasMod[client] = result == ConVarQuery_Okay && StringToInt(cvarValue) >= MOD_VERSION;
        g_bModKnown[client] = true;
        SendUIState(client);
    }
}

// ==================== painting ====================

public Action Timer_Paint(Handle timer)
{
    for (int i = 1; i <= MaxClients; i++)
    {
        if (!IsClientInGame(i) || !IsPlayerAlive(i))
        {
            continue;
        }

        if (g_bErasing[i])
        {
            // Sentinel start position: if the eye trace hits nothing it stays
            // untouched and matches no splat, instead of erasing around origin.
            float erasePos[3] = {-999999.0, -999999.0, -999999.0}, normal[3];
            int surf;
            TraceEye(i, erasePos, normal, surf);
            EraseNearPosition(i, erasePos);
            continue;
        }

        if (g_bRecoloring[i])
        {
            float aimPos[3] = {-999999.0, -999999.0, -999999.0}, normal[3];
            int surf;
            TraceEye(i, aimPos, normal, surf);
            RecolorNearPosition(i, aimPos);
            continue;
        }

        if (g_bIsPainting[i])
        {
            static float pos[3], normal[3];
            int surf;
            if (!TraceEye(i, pos, normal, surf) || surf == PAINTSURF_MOVING)
                continue;
            if (GetVectorDistance(pos, g_fLastPaint[i], true) > 1.0)
            {
                // "Random" picks a preset now and stores it, so the splat keeps its colour forever
                int rgb[3];
                if (g_PaintColourIdx[i] == 0)
                    rgb = g_iPresetRGB[GetRandomInt(1, 14)];
                else
                    rgb = g_PaintRGB[i];

                float splat[CACHE_BLOCK];
                MakeSplat(splat, pos, g_PaintRadius[i], rgb, g_PaintPic[i], normal, surf);
                if (!SavePaintToDatabase(i, splat))
                    continue;
                SendSplat(i, splat);
                PushPaintCache(i, splat);
                g_fLastPaint[i] = pos;
            }
        }
    }

    return Plugin_Continue;
}

void MakeSplat(float splat[CACHE_BLOCK], const float pos[3], float radius, const int rgb[3], int pic, const float normal[3], int surf)
{
    splat[0] = pos[0]; splat[1] = pos[1]; splat[2] = pos[2];
    splat[3] = radius;
    splat[4] = view_as<float>((rgb[0] << 16) | (rgb[1] << 8) | rgb[2]);
    splat[5] = view_as<float>(pic);
    splat[6] = normal[0]; splat[7] = normal[1]; splat[8] = normal[2];
    splat[9] = view_as<float>(surf);
}

void SplatRGB(const float splat[CACHE_BLOCK], int rgb[3])
{
    int packed = view_as<int>(splat[4]);
    rgb[0] = (packed >> 16) & 0xFF;
    rgb[1] = (packed >> 8) & 0xFF;
    rgb[2] = packed & 0xFF;
}

// the decal size nearest to a radius (by ratio)
int DecalSize(float radius)
{
    int best = 0;
    for (int i = 1; i < sizeof(g_fDecalRadius); i++)
    {
        if (FloatAbs(Logarithm(radius / g_fDecalRadius[i], 2.0)) < FloatAbs(Logarithm(radius / g_fDecalRadius[best], 2.0)))
            best = i;
    }
    return best;
}

// p2paint clients draw it themselves (any colour, 8 sizes, pictures, on any surface); everyone else gets the
// nearest classic decal where the engine can put one
void SendSplat(int client, const float splat[CACHE_BLOCK])
{
    int rgb[3];
    SplatRGB(splat, rgb);
    int size = DecalSize(splat[3]);
    int surf = view_as<int>(splat[9]);
    if (g_bHasMod[client])
    {
        char pic[48];
        PictureName(view_as<int>(splat[5]), pic, sizeof(pic));
        ClientCommand(client, "p2paint_decal %.2f %.2f %.2f %d %d %d %d %s %.3f %.3f %.3f %d", splat[0], splat[1], splat[2], size, rgb[0], rgb[1], rgb[2],
            pic[0] ? pic : "-", splat[6], splat[7], splat[8], surf);
        return;
    }
    if (surf != PAINTSURF_UNKNOWN && surf != PAINTSURF_WORLD && surf != PAINTSURF_DISPLACEMENT)
        return;
    float pos[3];
    pos[0] = splat[0]; pos[1] = splat[1]; pos[2] = splat[2];
    TE_SetupWorldDecal(pos, g_Sprites[NearestPreset(rgb) - 1][g_iOriginalSlot[size]]);
    TE_SendToClient(client);
}

void ClearClientView(int client)
{
    if (g_bHasMod[client])
        ClientCommand(client, "p2paint_clear");
    else
        ClientCommand(client, "r_cleardecals");
}

int NearestPreset(const int rgb[3])
{
    int best = 1, bestDist = 0x7FFFFFFF;
    for (int i = 1; i < sizeof(g_iPresetRGB); i++)
    {
        int dr = rgb[0] - g_iPresetRGB[i][0], dg = rgb[1] - g_iPresetRGB[i][1], db = rgb[2] - g_iPresetRGB[i][2];
        int dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist)
        {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

public void OnClientPutInServer(int client)
{
    if (!IsFakeClient(client))
    {
        QueryMod(client);
        CreateTimer(5.0, Timer_UpdatePlayerName, GetClientUserId(client), TIMER_FLAG_NO_MAPCHANGE);
        g_iLoadWaited[client] = 0;
        CreateTimer(3.0, Timer_LoadPaint, GetClientUserId(client), TIMER_FLAG_NO_MAPCHANGE);
    }
}

public Action Timer_UpdatePlayerName(Handle timer, any userid)
{
    int client = GetClientOfUserId(userid);
    if (client > 0 && IsClientInGame(client) && !IsFakeClient(client))
    {
        UpdatePlayerName(client);
    }
    return Plugin_Stop;
}

// waits (up to 5 more seconds) for the mod query, so paint goes the right way from the start, and for Steam to
// confirm the player (paint is saved under their Steam ID)
public Action Timer_LoadPaint(Handle timer, any userid)
{
    int client = GetClientOfUserId(userid);
    if (client <= 0 || !IsClientInGame(client))
        return Plugin_Stop;
    char auth[64], map[64];
    if (!PaintOwner(client, auth, map) || (!g_bModKnown[client] && g_iLoadWaited[client] < 10))
    {
        g_iLoadWaited[client]++;
        CreateTimer(0.5, Timer_LoadPaint, userid, TIMER_FLAG_NO_MAPCHANGE);
        return Plugin_Stop;
    }
    LoadClientPaint(client);
    return Plugin_Stop;
}

public Action cmd_EnablePaint(int client, int args)
{
    if (client <= 0)
        return Plugin_Handled;
    float normal[3];
    int surf;
    TraceEye(client, g_fLastPaint[client], normal, surf);
    g_bIsPainting[client] = true;
    // pressing paint means painting: leave erase / recolor mode
    if (g_bErasing[client] || g_bRecoloring[client])
    {
        g_bErasing[client] = g_bRecoloring[client] = false;
        PrintToChat(client, "[Paint] Erase / recolor mode off: painting.");
    }
    SendUIState(client);
    return Plugin_Handled;
}

public Action cmd_DisablePaint(int client, int args)
{
    if (client <= 0)
        return Plugin_Handled;
    g_bIsPainting[client] = false;
    SendUIState(client);
    return Plugin_Handled;
}

// hold to erase what you aim at (players without the p2paint mod; with it, the mod's own +erase is used)
public Action cmd_EnableErase(int client, int args)
{
    if (client <= 0)
        return Plugin_Handled;
    g_bErasing[client] = true;
    g_bIsPainting[client] = g_bRecoloring[client] = false;
    return Plugin_Handled;
}

public Action cmd_DisableErase(int client, int args)
{
    if (client > 0)
        g_bErasing[client] = false;
    return Plugin_Handled;
}

// From the p2paint mod: sm_paintfire paint|erase 1|0 = left click held / released while the paint or erase
// key is held. A click paints at once (the first tick doesn't wait for the aim to move).
public Action cmd_PaintFire(int client, int args)
{
    if (client <= 0 || args < 2)
        return Plugin_Handled;
    char what[8], on[4];
    GetCmdArg(1, what, sizeof(what));
    GetCmdArg(2, on, sizeof(on));
    bool down = StringToInt(on) != 0;
    if (down && !IsPlayerAlive(client))
    {
        PrintToChat(client, "[Paint] You can paint while you're alive (join a team / spawn first).");
        return Plugin_Handled;
    }
    if (StrEqual(what, "erase"))
    {
        g_bErasing[client] = down;
        if (down)
            g_bIsPainting[client] = g_bRecoloring[client] = false;
    }
    else
    {
        g_bErasing[client] = false;
        g_bIsPainting[client] = down && !g_bRecolorTool[client];
        g_bRecoloring[client] = down && g_bRecolorTool[client];
        g_fLastPaint[client][0] = g_fLastPaint[client][1] = g_fLastPaint[client][2] = -999999.0;
    }
    if (down)
        SendUIState(client);	// (clips: the aim ghost goes where the paint will, and !showclips may have changed)
    return Plugin_Handled;
}

// size steps for players without the p2paint mod (the mod has its own sm_sizeup / sm_sizedown)
public Action cmd_SizeUp(int client, int args)
{
    StepSize(client, 1);
    return Plugin_Handled;
}

public Action cmd_SizeDown(int client, int args)
{
    StepSize(client, -1);
    return Plugin_Handled;
}

void StepSize(int client, int dir)
{
    if (client <= 0)
        return;
    SetPaintRadius(client, g_fDecalRadius[ClampI(DecalSize(g_PaintRadius[client]) + dir, 0, sizeof(g_fDecalRadius) - 1)]);
    SendUIState(client);
}

// how far from the aim point erase / recolor reach: the current size, like the brush the mod shows
bool InBrush(int client, const float splatPos[3], float splatRadius, const float aimPos[3])
{
    return GetVectorDistance(splatPos, aimPos) <= g_PaintRadius[client] + 8.0 + splatRadius * 0.5;
}

public Action cmd_PaintColour(int client, int args)
{
    ShowColourMenu(client);
    return Plugin_Handled;
}

public Action cmd_PaintSize(int client, int args)
{
    if (args >= 1)
    {
        char arg[16];
        GetCmdArg(1, arg, sizeof(arg));
        int size = StringToInt(arg);
        // !paintsize 1-8 = a size, anything bigger = a radius
        SetPaintRadius(client, size >= 1 && size <= 8 ? g_fDecalRadius[size - 1] : StringToFloat(arg));
        SendUIState(client);
        return Plugin_Handled;
    }
    ShowSizeMenu(client);
    return Plugin_Handled;
}

public Action cmd_PaintRGB(int client, int args)
{
    if (args < 3)
    {
        ReplyToCommand(client, "[Paint] Usage: sm_paintrgb <r> <g> <b>  (0-255 each)");
        return Plugin_Handled;
    }
    char arg[8];
    int rgb[3];
    for (int i = 0; i < 3; i++)
    {
        GetCmdArg(i + 1, arg, sizeof(arg));
        rgb[i] = ClampI(StringToInt(arg), 0, 255);
    }
    SetCustomColour(client, rgb);
    SendUIState(client);
    return Plugin_Handled;
}

public Action cmd_ClearPaint(int client, int args)
{
    if (client <= 0 || !IsClientInGame(client))
        return Plugin_Handled;

    ClearClientView(client);
    ClearClientPaintData(client);

    PrintToChat(client, "[Paint] cleared.");
    return Plugin_Handled;
}

public Action cmd_PaintMenu(int client, int args)
{
    if (client > 0 && g_bHasMod[client])
    {
        SendUIState(client);
        ClientCommand(client, "p2paint_ui_open");
        return Plugin_Handled;
    }
    g_hMainMenu.Display(client, 20);
    return Plugin_Handled;
}

public Action cmd_PaintMenuClassic(int client, int args)
{
    g_hMainMenu.Display(client, 20);
    return Plugin_Handled;
}

// ==================== the p2paint client's menu ====================

// a word that's safe inside a console command line
void SafeArg(const char[] input, char[] output, int maxlength)
{
    strcopy(output, maxlength, input);
    for (int i = 0; output[i] != '\0'; i++)
    {
        if (output[i] == ';' || output[i] == '"' || output[i] == '\n' || output[i] == '\r' || output[i] == ' ')
            output[i] = '_';
    }
    if (output[0] == '\0')
        strcopy(output, maxlength, "_");
}

void SendUIState(int client)
{
    if (client <= 0 || !IsClientInGame(client) || !g_bHasMod[client])
        return;
    char profile[80], pic[48];
    SafeArg(g_CurrentProfile[client], profile, sizeof(profile));
    PictureName(g_PaintPic[client], pic, sizeof(pic));
    char triggers[256];
    SeenTriggerClasses(client, triggers, sizeof(triggers));
    ClientCommand(client, "p2paint_ui_state colour %d rgb %d %d %d size %.1f painting %d erasing %d recoloring %d clips %d triggers %s pic %s profile %s lists 1",
        g_PaintColourIdx[client], g_PaintRGB[client][0], g_PaintRGB[client][1], g_PaintRGB[client][2], g_PaintRadius[client],
        g_bIsPainting[client] ? 1 : 0, g_bErasing[client] ? 1 : 0, g_bRecolorTool[client] ? 1 : 0, SeesClips(client) ? 1 : 0, triggers,
        pic[0] ? pic : "-", profile);
}

public Action cmd_PaintUI(int client, int args)
{
    if (client <= 0 || !IsClientInGame(client) || args < 1)
        return Plugin_Handled;
    char what[16], a[64], v[48];
    GetCmdArg(1, what, sizeof(what));
    GetCmdArg(2, a, sizeof(a));
    GetCmdArg(3, v, sizeof(v));
    if (StrEqual(what, "set"))
    {
        if (StrEqual(a, "colour"))
        {
            int idx = StringToInt(v);
            if (idx >= 0 && idx < sizeof(g_cPaintColours))
                SetPresetColour(client, idx, false);
        }
        else if (StrEqual(a, "rgb"))
        {
            char g[8], bl[8];
            GetCmdArg(4, g, sizeof(g));
            GetCmdArg(5, bl, sizeof(bl));
            int rgb[3];
            rgb[0] = ClampI(StringToInt(v), 0, 255);
            rgb[1] = ClampI(StringToInt(g), 0, 255);
            rgb[2] = ClampI(StringToInt(bl), 0, 255);
            SetCustomColour(client, rgb);
        }
        else if (StrEqual(a, "size"))
            SetPaintRadius(client, StringToFloat(v), true);
        else if (StrEqual(a, "pic"))
            SetPaintPicture(client, v);
    }
    else if (StrEqual(what, "clear"))
    {
        ClearClientView(client);
        ClearClientPaintData(client);
    }
    else if (StrEqual(what, "recolor"))
    {
        g_bRecolorTool[client] = StringToInt(a) != 0;
        g_bRecoloring[client] = false;
    }
    else if (StrEqual(what, "backups"))
        ShowManageBackupsMenu(client, true);
    else if (StrEqual(what, "backup"))
    {
        // backup new | backup load N | backup delete N (N = position in the list we sent)
        char profile[64];
        if (StrEqual(a, "new"))
            CreateBackup(client, true);
        else if (UIListEntry(g_hUIBackups[client], v, profile, sizeof(profile)))
        {
            if (StrEqual(a, "load") && !StrEqual(profile, g_CurrentProfile[client]))
                LoadBackup(client, profile);
            else if (StrEqual(a, "delete"))
                DeleteBackup(client, profile);
            ShowManageBackupsMenu(client, true);
        }
    }
    else if (StrEqual(what, "painters"))
        ShowCopyPaintMenu(client, true);
    else if (StrEqual(what, "copy"))
    {
        char steamid[64];
        if (UIListEntry(g_hUIPainters[client], a, steamid, sizeof(steamid)))
            BackupAndCopyPaint(client, steamid);
    }
    SendUIState(client);
    return Plugin_Handled;
}

bool UIListEntry(ArrayList list, const char[] index, char[] out, int maxlength)
{
    int i = StringToInt(index);
    if (list == null || index[0] < '0' || index[0] > '9' || i < 0 || i >= list.Length)
        return false;
    list.GetString(i, out, maxlength);
    return true;
}

// a quoted argument for a command line the client runs (names can hold anything)
void QuoteArg(const char[] input, char[] output, int maxlength)
{
    strcopy(output, maxlength, input);
    for (int i = 0; output[i] != '\0'; i++)
    {
        if (output[i] == '"')
            output[i] = '\'';
        else if (output[i] == ';')
            output[i] = ',';
        else if (output[i] > 0 && output[i] < ' ')
            output[i] = ' ';
    }
}

public Action cmd_LoadBackup(int client, int args)
{
    ShowBackupsMenu(client);
    return Plugin_Handled;
}

// ==================== database ====================

bool SavePaintToDatabase(int client, const float splat[CACHE_BLOCK])
{
    char auth[64], map[64];
    if (g_hDatabase == null || !PaintOwner(client, auth, map))
        return false;

    char playerName[MAX_NAME_LENGTH * 2 + 1], escapedName[MAX_NAME_LENGTH * 2 + 1];
    GetClientName(client, playerName, sizeof(playerName));

    g_hDatabase.Escape(playerName, escapedName, sizeof(escapedName));

    int rgb[3];
    SplatRGB(splat, rgb);
    // paint_color / paint_size keep a classic-decal equivalent for older readers
    int legacySize = g_iOriginalSlot[DecalSize(splat[3])];
    char pic[48];
    PictureName(view_as<int>(splat[5]), pic, sizeof(pic));

    char query[1024];
    Format(query, sizeof(query), "INSERT INTO paint_data (steamid, map, pos_x, pos_y, pos_z, paint_color, paint_size, profile, player_name, radius, cr, cg, cb, pic, nx, ny, nz, surf) VALUES ('%s', '%s', %.2f, %.2f, %.2f, %d, %d, '%s', '%s', %.1f, %d, %d, %d, '%s', %.3f, %.3f, %.3f, %d)",
        auth, map, splat[0], splat[1], splat[2], NearestPreset(rgb), legacySize, g_CurrentProfile[client], escapedName,
        splat[3], rgb[0], rgb[1], rgb[2], pic, splat[6], splat[7], splat[8], view_as<int>(splat[9]));

    g_hDatabase.Query(SQL_InsertCallback, query);
    return true;
}

public void SQL_InsertCallback(Database db, DBResultSet results, const char[] error, any data)
{
    if (results == null)
    {
        LogError("Failed to insert paint data: %s", error);
    }
}

void LoadClientPaint(int client)
{
    if (g_hDatabase == null || !IsClientInGame(client))
        return;

    ClearPaintCache(client);
    int gen = ++g_iLoadGen[client];	// a load still running for this player stops at its next batch

    char auth[64], map[64];
    if (!PaintOwner(client, auth, map))
        return;

    char countQuery[512];
    Format(countQuery, sizeof(countQuery), "SELECT COUNT(*) FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile = '%s'", auth, map, g_CurrentProfile[client]);

    DataPack countPack = new DataPack();
    countPack.WriteCell(GetClientUserId(client));
    countPack.WriteCell(gen);

    g_hDatabase.Query(SQL_GetPaintCountCallback, countQuery, countPack);
}

public void SQL_GetPaintCountCallback(Database db, DBResultSet results, const char[] error, any data)
{
    DataPack pack = view_as<DataPack>(data);
    pack.Reset();
    int userid = pack.ReadCell();
    int gen = pack.ReadCell();
    delete pack;

    int client = GetClientOfUserId(userid);
    if (client <= 0 || gen != g_iLoadGen[client])
        return;

    if (results == null)
    {
        LogError("Failed to get paint count: %s", error);
        return;
    }

    if (!results.FetchRow())
    {
        LogError("No results from paint count query");
        return;
    }

    int total = results.FetchInt(0);
    if (total <= 0)
    {
        if (!StrEqual(g_CurrentProfile[client], "default"))
        {
            PrintToChat(client, "[Paint] No paint found in profile \"%s\".", g_CurrentProfile[client]);
        }
        return;
    }

    PrintToChat(client, "[Paint] Loading %d splats from profile \"%s\"...", total, g_CurrentProfile[client]);
    LoadPaintBatch(client, 0, total, gen);
}

#define SELECT_SPLAT_COLUMNS "id, pos_x, pos_y, pos_z, paint_color, paint_size, radius, cr, cg, cb, pic, nx, ny, nz, surf"

// pages by id, not OFFSET: rows erased while loading don't shift the pages (that skipped splats)
void LoadPaintBatch(int client, int afterId, int total, int gen)
{
    char auth[64], map[64];
    if (!IsClientInGame(client) || !PaintOwner(client, auth, map))
        return;

    char query[512];
    Format(query, sizeof(query), "SELECT " ... SELECT_SPLAT_COLUMNS ... " FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile = '%s' AND id > %d ORDER BY id LIMIT %d",
        auth, map, g_CurrentProfile[client], afterId, LOAD_BATCH);

    DataPack pack = new DataPack();
    pack.WriteCell(GetClientUserId(client));
    pack.WriteCell(gen);
    pack.WriteCell(total);

    g_hDatabase.Query(SQL_LoadBatchCallback, query, pack);
}

// One row of SELECT_SPLAT_COLUMNS -> splat. Rows from before 4.0 have no radius / colour columns: their size
// and colour come from the decal index; old "random" rows get a colour picked once from the row id and written
// back, so they never change again.
void RowToSplat(DBResultSet results, float splat[CACHE_BLOCK])
{
    int id = results.FetchInt(0);
    float pos[3];
    pos[0] = results.FetchFloat(1);
    pos[1] = results.FetchFloat(2);
    pos[2] = results.FetchFloat(3);
    int paint = results.FetchInt(4);
    int size = results.FetchInt(5);
    float radius = results.IsFieldNull(6) ? g_fLegacyRadius[ClampI(size, 0, 2)] : results.FetchFloat(6);

    int rgb[3];
    if (!results.IsFieldNull(7))
    {
        rgb[0] = results.FetchInt(7);
        rgb[1] = results.FetchInt(8);
        rgb[2] = results.FetchInt(9);
    }
    else
    {
        if (paint <= 0 || paint >= sizeof(g_iPresetRGB))
        {
            paint = 1 + ((id * 7919) & 0x7FFFFFFF) % 14;
            char query[256];
            Format(query, sizeof(query), "UPDATE paint_data SET paint_color = %d, cr = %d, cg = %d, cb = %d WHERE id = %d",
                paint, g_iPresetRGB[paint][0], g_iPresetRGB[paint][1], g_iPresetRGB[paint][2], id);
            g_hDatabase.Query(SQL_ErrorCheckCallback, query);
        }
        rgb = g_iPresetRGB[paint];
    }
    char pic[48];
    if (!results.IsFieldNull(10))
        results.FetchString(10, pic, sizeof(pic));
    // where it landed: rows from before 5.1 (and 4.x gel rows without a normal) don't know, the mod looks it up
    float normal[3];
    int surf = PAINTSURF_UNKNOWN;
    if (!results.IsFieldNull(11) && !results.IsFieldNull(14))
    {
        normal[0] = results.FetchFloat(11);
        normal[1] = results.FetchFloat(12);
        normal[2] = results.FetchFloat(13);
        surf = results.FetchInt(14);
    }
    MakeSplat(splat, pos, radius, rgb, FindOrAddPicture(pic), normal, surf);
}

public void SQL_LoadBatchCallback(Database db, DBResultSet results, const char[] error, any data)
{
    DataPack pack = view_as<DataPack>(data);
    pack.Reset();
    int userid = pack.ReadCell();
    int gen = pack.ReadCell();
    int total = pack.ReadCell();
    delete pack;

    int client = GetClientOfUserId(userid);
    if (client <= 0 || gen != g_iLoadGen[client])
        return;

    if (results == null)
    {
        LogError("Failed to load paint batch: %s", error);
        return;
    }

    int count = 0, lastId = 0;
    while (results.FetchRow())
    {
        float splat[CACHE_BLOCK];
        lastId = results.FetchInt(0);
        RowToSplat(results, splat);
        SendSplat(client, splat);
        PushPaintCache(client, splat);
        count++;
    }

    if (count == LOAD_BATCH)
    {
        DataPack timerPack = new DataPack();
        timerPack.WriteCell(userid);
        timerPack.WriteCell(lastId);
        timerPack.WriteCell(total);
        timerPack.WriteCell(gen);
        CreateTimer(0.1, Timer_NextBatch, timerPack);
    }
    else
    {
        PrintToChat(client, "[Paint] Loaded %d splats from profile \"%s\".", total, g_CurrentProfile[client]);
    }
}

public Action Timer_NextBatch(Handle timer, any data)
{
    DataPack pack = view_as<DataPack>(data);
    pack.Reset();
    int userid = pack.ReadCell();
    int afterId = pack.ReadCell();
    int total = pack.ReadCell();
    int gen = pack.ReadCell();
    delete pack;

    int client = GetClientOfUserId(userid);
    if (client <= 0 || gen != g_iLoadGen[client])
        return Plugin_Stop;

    LoadPaintBatch(client, afterId, total, gen);
    return Plugin_Stop;
}

void ClearClientPaintData(int client)
{
    ClearClientPaintDataWithCallback(client, SQL_DeleteCallback);
}

void ClearClientPaintDataWithCallback(int client, SQLQueryCallback callback, any data = 0)
{
    char auth[64], map[64];
    if (g_hDatabase == null || !PaintOwner(client, auth, map))
        return;

    ClearPaintCache(client);

    char query[512];
    Format(query, sizeof(query), "DELETE FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile = '%s'", auth, map, g_CurrentProfile[client]);

    g_hDatabase.Query(callback, query, data);
}

public void SQL_DeleteCallback(Database db, DBResultSet results, const char[] error, any data)
{
    if (results == null)
    {
        LogError("Failed to delete paint data: %s", error);
    }
}

int GetNextProfileNumber(int client, char[] usernameBuffer, int maxlength)
{
    if (g_hDatabase == null)
        return 1;

    char auth[64], map[64];
    if (!PaintOwner(client, auth, map))
        return 1;

    char username[128];
    GetClientName(client, username, sizeof(username));

    for (int i = 0; i < strlen(username); i++)
    {
        if (username[i] == ' ' || username[i] == '\t' || username[i] == '\\' || username[i] == '/' ||
            username[i] == ':' || username[i] == '*' || username[i] == '?' || username[i] == '"' ||
            username[i] == '<' || username[i] == '>' || username[i] == '|' || username[i] == '\'')
        {
            username[i] = '_';
        }
    }

    strcopy(usernameBuffer, maxlength, username);

    // highest number among this player's backups on this map (numeric order, not text order)
    char query[512];
    Format(query, sizeof(query), "SELECT profile FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile LIKE '%s_%%' GROUP BY profile", auth, map, username);

    // (a query on the game thread over the connection the threaded ones use: SourceMod wants it locked)
    SQL_LockDatabase(g_hDatabase);
    DBResultSet result = SQL_Query(g_hDatabase, query);
    char error[255];
    if (result == null)
        SQL_GetError(g_hDatabase, error, sizeof(error));
    SQL_UnlockDatabase(g_hDatabase);

    if (result == null)
    {
        LogError("Failed to query paint profiles: %s", error);
        return 1;
    }

    int nextNumber = 1;

    while (result.FetchRow())
    {
        char profile[64];
        result.FetchString(0, profile, sizeof(profile));

        int underscorePos = FindCharInString(profile, '_', true);
        if (underscorePos != -1)
        {
            int number = StringToInt(profile[underscorePos + 1]);
            if (number >= nextNumber)
            {
                nextNumber = number + 1;
            }
        }
    }

    delete result;
    return nextNumber;
}

// every column a splat has (the 4.x gel ones too, so a copy keeps old data whole)
#define ALL_SPLAT_COLUMNS "pos_x, pos_y, pos_z, paint_color, paint_size, nx, ny, nz, radius, cr, cg, cb, bright, opacity, surf, glow, pic"

void SaveProfileAs(int client, const char[] sourceProfile, const char[] targetProfile)
{
    char auth[64], map[64];
    if (g_hDatabase == null || !PaintOwner(client, auth, map))
        return;

    char query[1024];
    Format(query, sizeof(query), "INSERT INTO paint_data (steamid, map, profile, player_name, " ... ALL_SPLAT_COLUMNS ... ") SELECT steamid, map, '%s', player_name, " ... ALL_SPLAT_COLUMNS ... " FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile = '%s' ORDER BY id",
        targetProfile, auth, map, sourceProfile);

    g_hDatabase.Query(SQL_ErrorCheckCallback, query);
}

// ==================== backups ====================

void ShowBackupsMenu(int client)
{
    Menu menu = new Menu(BackupsMenuHandle);
    menu.SetTitle("Paint Backups:\nCurrent: %s\n ", g_CurrentProfile[client]);
    menu.AddItem("create", "Create backup of current paint");
    menu.AddItem("manage", "Manage backups (load / delete)");
    menu.ExitBackButton = true;
    menu.Display(client, 30);
}

public int BackupsMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        char item[16];
        menu.GetItem(choice, item, sizeof(item));
        if (StrEqual(item, "create"))
        {
            CreateBackup(client);
        }
        else
        {
            ShowManageBackupsMenu(client);
        }
    }
    else if (action == MenuAction_Cancel && choice == MenuCancel_ExitBack)
    {
        g_hMainMenu.Display(client, 20);
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }
    return 0;
}

// backups and copies duplicate whole profiles (thousands of rows): one per player every few seconds
bool CopyAllowed(int client)
{
    float now = GetEngineTime();
    if (now < g_fNextCopy[client])
    {
        PrintToChat(client, "[Paint] Wait a few seconds between backups / copies.");
        return false;
    }
    g_fNextCopy[client] = now + COPY_COOLDOWN;
    return true;
}

bool AtProfileLimit(int client, int profiles)
{
    if (profiles < MAX_PROFILES)
        return false;
    PrintToChat(client, "[Paint] %d paint profiles on this map is the limit: delete a backup first.", MAX_PROFILES);
    return true;
}

// this player's splats in the current profile, and how many profiles they have on this map
#define COUNT_FOR_BACKUP "SELECT COUNT(*), (SELECT COUNT(DISTINCT profile) FROM paint_data WHERE steamid = '%s' AND map = '%s') FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile = '%s'"

void CreateBackup(int client, bool forUI = false)
{
    char auth[64], map[64];
    if (!PaintOwner(client, auth, map) || !CopyAllowed(client))
        return;

    char query[512];
    Format(query, sizeof(query), COUNT_FOR_BACKUP, auth, map, auth, map, g_CurrentProfile[client]);
    DataPack pack = new DataPack();
    pack.WriteCell(GetClientUserId(client));
    pack.WriteCell(forUI);
    g_hDatabase.Query(SQL_CreateBackupCountCallback, query, pack);
}

public void SQL_CreateBackupCountCallback(Database db, DBResultSet results, const char[] error, DataPack pack)
{
    pack.Reset();
    int client = GetClientOfUserId(pack.ReadCell());
    bool forUI = pack.ReadCell();
    delete pack;
    if (client <= 0)
        return;
    if (results == null || !results.FetchRow())
    {
        LogError("Failed to count paint for backup: %s", error);
        PrintToChat(client, "[Paint] Backup failed.");
        return;
    }
    int count = results.FetchInt(0);
    if (count <= 0)
        PrintToChat(client, "[Paint] Nothing to back up: profile \"%s\" has no paint on this map.", g_CurrentProfile[client]);
    else if (!AtProfileLimit(client, results.FetchInt(1)))
    {
        char baseName[128], newProfile[64];
        int number = GetNextProfileNumber(client, baseName, sizeof(baseName));
        Format(newProfile, sizeof(newProfile), "%s_%d", baseName, number);
        SaveProfileAs(client, g_CurrentProfile[client], newProfile);
        PrintToChat(client, "[Paint] Backed up %d splats of \"%s\" as \"%s\" (you keep painting in \"%s\").",
            count, g_CurrentProfile[client], newProfile, g_CurrentProfile[client]);
    }
    if (forUI)
        ShowManageBackupsMenu(client, true);
    else
        ShowBackupsMenu(client);
}

void ShowManageBackupsMenu(int client, bool forUI = false)
{
    if (g_hDatabase == null)
        return;

    char auth[64], map[64];
    if (!PaintOwner(client, auth, map))
        return;

    char query[512];
    Format(query, sizeof(query), "SELECT profile, COUNT(*) FROM paint_data WHERE steamid = '%s' AND map = '%s' GROUP BY profile ORDER BY MIN(id)", auth, map);
    DataPack pack = new DataPack();
    pack.WriteCell(GetClientUserId(client));
    pack.WriteCell(forUI);
    g_hDatabase.Query(SQL_ManageBackupsCallback, query, pack);
}

public void SQL_ManageBackupsCallback(Database db, DBResultSet results, const char[] error, DataPack pack)
{
    pack.Reset();
    int client = GetClientOfUserId(pack.ReadCell());
    bool forUI = pack.ReadCell();
    delete pack;
    if (client <= 0)
        return;

    if (results == null)
    {
        LogError("Failed to load backups: %s", error);
        PrintToChat(client, "[Paint] Error loading backups.");
        return;
    }

    if (forUI)
    {
        delete g_hUIBackups[client];
        g_hUIBackups[client] = new ArrayList(ByteCountToCells(64));
        ClientCommand(client, "p2paint_ui_list backups begin");
        char name[64], quoted[64];
        while (results.FetchRow())
        {
            results.FetchString(0, name, sizeof(name));
            g_hUIBackups[client].PushString(name);
            QuoteArg(name, quoted, sizeof(quoted));
            ClientCommand(client, "p2paint_ui_list backups item \"%s\" %d %d", quoted, results.FetchInt(1), StrEqual(name, g_CurrentProfile[client]) ? 1 : 0);
        }
        ClientCommand(client, "p2paint_ui_list backups end");
        return;
    }

    Menu menu = new Menu(ManageBackupsMenuHandle);
    menu.SetTitle("Manage Backups (this map):");

    int count = 0;
    char profile[64], display[128];
    while (results.FetchRow())
    {
        results.FetchString(0, profile, sizeof(profile));
        int splats = results.FetchInt(1);
        Format(display, sizeof(display), "%s (%d)%s", profile, splats, StrEqual(profile, g_CurrentProfile[client]) ? " [current]" : "");
        menu.AddItem(profile, display);
        count++;
    }

    if (count == 0)
    {
        menu.AddItem("", "No paint backups found", ITEMDRAW_DISABLED);
    }

    menu.ExitBackButton = true;
    menu.Display(client, 30);
}

public int ManageBackupsMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        char profile[64];
        menu.GetItem(choice, profile, sizeof(profile));
        ShowBackupActionMenu(client, profile);
    }
    else if (action == MenuAction_Cancel && choice == MenuCancel_ExitBack)
    {
        ShowBackupsMenu(client);
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }
    return 0;
}

void ShowBackupActionMenu(int client, const char[] profile)
{
    Menu menu = new Menu(BackupActionMenuHandle);
    menu.SetTitle("Backup: %s\n ", profile);
    char info[80];
    Format(info, sizeof(info), "load %s", profile);
    menu.AddItem(info, "Load (switch to this paint)", StrEqual(profile, g_CurrentProfile[client]) ? ITEMDRAW_DISABLED : ITEMDRAW_DEFAULT);
    Format(info, sizeof(info), "delete %s", profile);
    menu.AddItem(info, "Delete");
    menu.ExitBackButton = true;
    menu.Display(client, 30);
}

public int BackupActionMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        char info[80];
        menu.GetItem(choice, info, sizeof(info));
        if (StrContains(info, "load ") == 0)
        {
            LoadBackup(client, info[5]);
            ShowBackupsMenu(client);
        }
        else
        {
            ShowDeleteBackupConfirm(client, info[7]);
        }
    }
    else if (action == MenuAction_Cancel && choice == MenuCancel_ExitBack)
    {
        ShowManageBackupsMenu(client);
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }
    return 0;
}

void DeleteBackup(int client, const char[] profileIn)
{
    char profile[64];
    strcopy(profile, sizeof(profile), profileIn);
    char auth[64], map[64], escaped[160];
    if (!PaintOwner(client, auth, map))
        return;
    g_hDatabase.Escape(profile, escaped, sizeof(escaped));
    char query[512];
    Format(query, sizeof(query), "DELETE FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile = '%s'", auth, map, escaped);
    SQL_LockDatabase(g_hDatabase);
    SQL_FastQuery(g_hDatabase, query);
    SQL_UnlockDatabase(g_hDatabase);
    PrintToChat(client, "[Paint] Deleted backup \"%s\".", profile);
    if (StrEqual(profile, g_CurrentProfile[client]))
    {
        ClearClientView(client);
        ClearPaintCache(client);
        strcopy(g_CurrentProfile[client], sizeof(g_CurrentProfile[]), "default");
        SetClientCookie(client, g_hCurrentProfileCookie, "default");
        LoadClientPaint(client);
    }
}

void LoadBackup(int client, const char[] profile)
{
    ClearClientView(client);
    strcopy(g_CurrentProfile[client], sizeof(g_CurrentProfile[]), profile);
    SetClientCookie(client, g_hCurrentProfileCookie, profile);
    PrintToChat(client, "[Paint] Switched to \"%s\". New paint goes into it.", profile);
    LoadClientPaint(client);
    SendUIState(client);
}

void ShowDeleteBackupConfirm(int client, const char[] profile)
{
    Menu menu = new Menu(DeleteBackupConfirmHandle);
    menu.SetTitle("Delete backup \"%s\" for good?\n ", profile);
    char info[80];
    Format(info, sizeof(info), "yes %s", profile);
    menu.AddItem("no", "No, keep it");
    menu.AddItem(info, "Yes, delete it");
    menu.Pagination = MENU_NO_PAGINATION;
    menu.ExitButton = false;
    menu.Display(client, 30);
}

public int DeleteBackupConfirmHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        char info[80];
        menu.GetItem(choice, info, sizeof(info));
        if (StrContains(info, "yes ") == 0)
            DeleteBackup(client, info[4]);
        ShowManageBackupsMenu(client);
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }
    return 0;
}

// ==================== copy paint from another player ====================

void ShowCopyPaintMenu(int client, bool forUI = false)
{
    if (g_hDatabase == null)
        return;

    char map[64];
    GetCurrentMap(map, sizeof(map));

    char query[512];
    Format(query, sizeof(query), "SELECT DISTINCT p.steamid, n.name FROM paint_data p LEFT JOIN player_names n ON p.steamid = n.steamid WHERE p.map = '%s'", map);

    DataPack pack = new DataPack();
    pack.WriteCell(GetClientUserId(client));
    pack.WriteCell(forUI);

    g_hDatabase.Query(SQL_GetPaintersCallback, query, pack);
}

public void SQL_GetPaintersCallback(Database db, DBResultSet results, const char[] error, any data)
{
    DataPack pack = view_as<DataPack>(data);
    pack.Reset();
    int userid = pack.ReadCell();
    bool forUI = pack.ReadCell();
    delete pack;

    int client = GetClientOfUserId(userid);
    if (client <= 0)
        return;

    if (results == null)
    {
        LogError("Failed to get painters: %s", error);
        PrintToChat(client, "[Paint] Error loading painter list.");
        return;
    }

    Menu copyMenu = forUI ? null : new Menu(CopyPaintMenuHandle);
    if (forUI)
    {
        delete g_hUIPainters[client];
        g_hUIPainters[client] = new ArrayList(ByteCountToCells(64));
        ClientCommand(client, "p2paint_ui_list painters begin");
    }
    else
        copyMenu.SetTitle("Copy Paint From:");

    int count = 0;
    char steamid[64], name[MAX_NAME_LENGTH + 1];
    char displayName[192];
    char clientSteamID[64];
    GetClientAuthId(client, AuthId_Steam2, clientSteamID, sizeof(clientSteamID), true);

    while (results.FetchRow())
    {
        results.FetchString(0, steamid, sizeof(steamid));

        if (StrEqual(steamid, clientSteamID))
        {
            continue;
        }

        results.FetchString(1, name, sizeof(name));

        if (name[0] == '\0')
        {
            bool foundName = false;
            for (int i = 1; i <= MaxClients; i++)
            {
                if (IsClientInGame(i))
                {
                    char playerSteamID[64];
                    GetClientAuthId(i, AuthId_Steam2, playerSteamID, sizeof(playerSteamID), true);

                    if (StrEqual(steamid, playerSteamID))
                    {
                        GetClientName(i, name, sizeof(name));
                        foundName = true;
                        break;
                    }
                }
            }

            if (!foundName)
            {
                Format(name, sizeof(name), "Player (%s)", steamid);
            }
        }

        if (forUI)
        {
            g_hUIPainters[client].PushString(steamid);
            QuoteArg(name, displayName, sizeof(displayName));
            ClientCommand(client, "p2paint_ui_list painters item \"%s\"", displayName);
            continue;
        }
        Format(displayName, sizeof(displayName), "%s", name);
        copyMenu.AddItem(steamid, displayName);
        count++;
    }

    if (forUI)
    {
        ClientCommand(client, "p2paint_ui_list painters end");
        return;
    }

    if (count == 0)
    {
        copyMenu.AddItem("", "No other paint profiles found", ITEMDRAW_DISABLED);
    }

    copyMenu.ExitBackButton = true;
    copyMenu.Display(client, 20);
}

public int CopyPaintMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        char steamid[64];
        menu.GetItem(choice, steamid, sizeof(steamid));

        BackupAndCopyPaint(client, steamid);
    }
    else if (action == MenuAction_Cancel && choice == MenuCancel_ExitBack)
    {
        g_hMainMenu.Display(client, 20);
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }

    return 0;
}

void BackupAndCopyPaint(int client, const char[] targetSteamID)
{
    char auth[64], map[64];
    if (g_hDatabase == null || !PaintOwner(client, auth, map) || !CopyAllowed(client))
        return;

    char query[512];
    Format(query, sizeof(query), COUNT_FOR_BACKUP, auth, map, auth, map, g_CurrentProfile[client]);

    DataPack pack = new DataPack();
    pack.WriteCell(GetClientUserId(client));
    pack.WriteString(targetSteamID);

    g_hDatabase.Query(SQL_CheckPaintForBackupCallback, query, pack);
}

public void SQL_CheckPaintForBackupCallback(Database db, DBResultSet results, const char[] error, any data)
{
    DataPack pack = view_as<DataPack>(data);
    pack.Reset();
    int userid = pack.ReadCell();

    char targetSteamID[64];
    pack.ReadString(targetSteamID, sizeof(targetSteamID));
    delete pack;

    int client = GetClientOfUserId(userid);
    if (client <= 0)
        return;

    if (results == null)
    {
        LogError("Failed to check paint count: %s", error);
        return;
    }

    if (results.FetchRow() && results.FetchInt(0) > 0)
    {
        if (AtProfileLimit(client, results.FetchInt(1)))
            return;	// (copying first saves the current paint as a backup)
        char baseName[128];
        int nextNumber = GetNextProfileNumber(client, baseName, sizeof(baseName));

        char newProfile[64];
        Format(newProfile, sizeof(newProfile), "%s_%d", baseName, nextNumber);

        SaveProfileAs(client, g_CurrentProfile[client], newProfile);

        PrintToChat(client, "[Paint] Your current paint was saved to profile '%s'", newProfile);
    }

    // into the current profile (just backed up): switching to "default" here used to wipe default's paint
    ClearClientView(client);

    DataPack newPack = new DataPack();
    newPack.WriteCell(GetClientUserId(client));
    newPack.WriteString(targetSteamID);

    ClearClientPaintDataWithCallback(client, SQL_ClearThenCopyPaint, newPack);
}

public void SQL_ClearThenCopyPaint(Database db, DBResultSet results, const char[] error, any data)
{
    DataPack pack = view_as<DataPack>(data);
    pack.Reset();
    int userid = pack.ReadCell();

    char targetSteamID[64];
    pack.ReadString(targetSteamID, sizeof(targetSteamID));
    delete pack;

    int client = GetClientOfUserId(userid);
    if (client <= 0)
        return;

    if (results == null)
    {
        LogError("Failed to clear paint data: %s", error);
        return;
    }

    CopyPaintFromPlayer(client, targetSteamID);
}

// Copies the other player's current profile (their "default" profile if they're offline) into this player's
// current profile in one statement, then loads it like any other paint.
void CopyPaintFromPlayer(int client, const char[] targetSteamID)
{
    if (g_hDatabase == null)
        return;

    // an offline player's current profile isn't known: use the one they painted in last
    char sourceProfile[64] = "";
    for (int i = 1; i <= MaxClients; i++)
    {
        if (IsClientInGame(i) && !IsFakeClient(i))
        {
            char sid[64];
            GetClientAuthId(i, AuthId_Steam2, sid, sizeof(sid), true);
            if (StrEqual(sid, targetSteamID))
            {
                strcopy(sourceProfile, sizeof(sourceProfile), g_CurrentProfile[i]);
                break;
            }
        }
    }

    char clientSteamID[64], map[64], playerName[MAX_NAME_LENGTH * 2 + 1], escapedName[MAX_NAME_LENGTH * 2 + 1];
    if (!PaintOwner(client, clientSteamID, map))
        return;
    GetClientName(client, playerName, sizeof(playerName));
    g_hDatabase.Escape(playerName, escapedName, sizeof(escapedName));

    char profileMatch[160];
    if (sourceProfile[0] != '\0')
    {
        char escaped[130];
        g_hDatabase.Escape(sourceProfile, escaped, sizeof(escaped));
        Format(profileMatch, sizeof(profileMatch), "'%s'", escaped);
    }
    else
    {
        Format(profileMatch, sizeof(profileMatch), "(SELECT profile FROM paint_data WHERE steamid = '%s' AND map = '%s' ORDER BY id DESC LIMIT 1)", targetSteamID, map);
    }

    char query[1536];
    Format(query, sizeof(query), "INSERT INTO paint_data (steamid, map, profile, player_name, " ... ALL_SPLAT_COLUMNS ... ") SELECT '%s', map, '%s', '%s', " ... ALL_SPLAT_COLUMNS ... " FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile = %s ORDER BY id",
        clientSteamID, g_CurrentProfile[client], escapedName, targetSteamID, map, profileMatch);
    g_hDatabase.Query(SQL_CopyDoneCallback, query, GetClientUserId(client));
}

public void SQL_CopyDoneCallback(Database db, DBResultSet results, const char[] error, any userid)
{
    int client = GetClientOfUserId(userid);
    if (client <= 0)
        return;
    if (results == null)
    {
        LogError("Failed to copy paint: %s", error);
        PrintToChat(client, "[Paint] Failed to copy paint profile.");
        return;
    }
    PrintToChat(client, "[Paint] Copied %d splats.", results.AffectedRows);
    LoadClientPaint(client);
    SendUIState(client);
}

public void SQL_ErrorCheckCallback(Database db, DBResultSet results, const char[] error, any data)
{
    if (results == null)
    {
        LogError("Database error: %s", error);
    }
}

// ==================== settings menus ====================

public int MainMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_DisplayItem)
    {
        char item[16], display[96];
        menu.GetItem(choice, item, sizeof(item));
        if (StrEqual(item, "color"))
        {
            char name[48];
            ColourName(client, name, sizeof(name));
            Format(display, sizeof(display), "Colour: %s", name);
        }
        else if (StrEqual(item, "size"))
            Format(display, sizeof(display), "Size: %d/8", DecalSize(g_PaintRadius[client]) + 1);
        else if (StrEqual(item, "recolor"))
            Format(display, sizeof(display), "Recolor tool (paint key recolors): %s", g_bRecolorTool[client] ? "ON" : "off");
        else
            return 0;
        return RedrawMenuItem(display);
    }
    if (action == MenuAction_Select)
    {
        char item[64];
        menu.GetItem(choice, item, sizeof(item));

        if (StrEqual(item, "color"))
            ShowColourMenu(client);
        else if (StrEqual(item, "size"))
            ShowSizeMenu(client);
        else if (StrEqual(item, "recolor"))
        {
            g_bRecolorTool[client] = !g_bRecolorTool[client];
            g_bRecoloring[client] = false;
            PrintToChat(client, "[Paint] Recolor tool %s.", g_bRecolorTool[client] ? "\x04ON\x01: the paint key recolors the paint you aim at (current colour / picture)" : "off");
            SendUIState(client);
            g_hMainMenu.Display(client, 20);
        }
        else if (StrEqual(item, "backups"))
            ShowBackupsMenu(client);
        else if (StrEqual(item, "clear"))
            ShowClearConfirmMenu(client);
        else if (StrEqual(item, "copy"))
            ShowCopyPaintMenu(client);
        else if (StrEqual(item, "erase"))
            ShowEraseMenu(client);
    }

    return 0;
}

void ColourName(int client, char[] buffer, int maxlength)
{
    if (g_PaintPic[client] > 0)
    {
        char pic[48];
        PictureName(g_PaintPic[client], pic, sizeof(pic));
        Format(buffer, maxlength, "picture %s", pic);
    }
    else if (g_PaintColourIdx[client] >= 0)
        strcopy(buffer, maxlength, g_cPaintColours[g_PaintColourIdx[client]][0]);
    else
        Format(buffer, maxlength, "Custom (%d %d %d)", g_PaintRGB[client][0], g_PaintRGB[client][1], g_PaintRGB[client][2]);
}

void ShowColourMenu(int client)
{
    Menu menu = new Menu(ColourMenuHandle);
    char name[48];
    ColourName(client, name, sizeof(name));
    menu.SetTitle("Paint Colour: %s\nCustom: !paintrgb <r> <g> <b>\n ", name);
    for (int i = 0; i < sizeof(g_cPaintColours); i++)
    {
        menu.AddItem(g_cPaintColours[i][0], g_cPaintColours[i][0]);
    }
    menu.ExitBackButton = true;
    menu.Display(client, 30);
}

public int ColourMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        SetPresetColour(client, choice, true);
        SendUIState(client);
        g_hMainMenu.Display(client, 20);
    }
    else if (action == MenuAction_Cancel && choice == MenuCancel_ExitBack)
    {
        g_hMainMenu.Display(client, 20);
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }
    return 0;
}

void SetPresetColour(int client, int idx, bool announce)
{
    SetPaintPicture(client, "-");	// a colour paints the colour, not the picture
    g_PaintColourIdx[client] = idx;
    if (idx > 0)
        g_PaintRGB[client] = g_iPresetRGB[idx];
    char val[32];
    if (idx == 0)
        strcopy(val, sizeof(val), "random");
    else
        Format(val, sizeof(val), "preset %d", idx);
    if (AreClientCookiesCached(client))
    {
        SetClientCookie(client, g_hColourCookie, val);
        IntToString(idx, val, sizeof(val));
        SetClientCookie(client, g_hPlayerPaintColour, val);
    }
    if (announce)
        PrintToChat(client, "[Paint] Colour set to \x04%s", g_cPaintColours[idx][0]);
}

void SetCustomColour(int client, const int rgb[3])
{
    SetPaintPicture(client, "-");
    g_PaintColourIdx[client] = -1;
    g_PaintRGB[client] = rgb;
    char val[32];
    Format(val, sizeof(val), "%d %d %d", rgb[0], rgb[1], rgb[2]);
    if (AreClientCookiesCached(client))
        SetClientCookie(client, g_hColourCookie, val);
}

void ShowSizeMenu(int client)
{
    Menu menu = new Menu(SizeMenuHandle);
    menu.SetTitle("Paint Size: %d/8\n(p2paint: hold the paint key + wheel)\n ", DecalSize(g_PaintRadius[client]) + 1);
    // players without the mod only have sizes 2, 5 and 8
    if (g_bHasMod[client])
    {
        char info[4], display[16];
        for (int i = 0; i < sizeof(g_fDecalRadius); i++)
        {
            IntToString(i, info, sizeof(info));
            Format(display, sizeof(display), "%d", i + 1);
            menu.AddItem(info, display);
        }
    }
    else
    {
        menu.AddItem("1", "Small");
        menu.AddItem("4", "Medium");
        menu.AddItem("7", "Large");
    }
    menu.ExitBackButton = true;
    menu.Display(client, 30);
}

public int SizeMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        char info[4];
        menu.GetItem(choice, info, sizeof(info));
        SetPaintRadius(client, g_fDecalRadius[ClampI(StringToInt(info), 0, sizeof(g_fDecalRadius) - 1)]);
        SendUIState(client);
        g_hMainMenu.Display(client, 20);
    }
    else if (action == MenuAction_Cancel && choice == MenuCancel_ExitBack)
        g_hMainMenu.Display(client, 20);
    else if (action == MenuAction_End)
        delete menu;
    return 0;
}

// decals come in 8 fixed sizes
void SetPaintRadius(int client, float radius, bool quiet = false)
{
    g_PaintRadius[client] = g_fDecalRadius[DecalSize(ClampF(radius, 4.0, 160.0))];
    char val[16];
    FloatToString(g_PaintRadius[client], val, sizeof(val));
    if (AreClientCookiesCached(client))
        SetClientCookie(client, g_hRadiusCookie, val);
    if (!quiet)
        PrintToChat(client, "[Paint] Size set to \x04%d/8", DecalSize(g_PaintRadius[client]) + 1);
}

// ==================== helpers ====================

int PrecachePaint(const char[] filename)
{
    char path[256];
    Format(path, sizeof(path), "materials/%s", filename);
    AddFileToDownloadsTable(path);
    return PrecacheDecal(filename, true);
}

void TE_SetupWorldDecal(const float vecOrigin[3], int index)
{
    TE_Start("World Decal");
    TE_WriteVector("m_vecOrigin", vecOrigin);
    TE_WriteNum("m_nIndex", index);
}

// the trigger classes the player sees with !st, comma-separated ("-" none): the mod's aim ghost stops at those
char g_cTriggerClasses[][] =
{
    "trigger_multiple", "trigger_push", "trigger_teleport", "trigger_teleport_relative", "trigger_once", "trigger_hurt", "trigger_gravity"
};

void SeenTriggerClasses(int client, char[] out, int maxlen)
{
    out[0] = '\0';
    if (GetFeatureStatus(FeatureType_Native, "ShowTriggers_ClientSees") == FeatureStatus_Available)
    {
        for (int i = 0; i < sizeof(g_cTriggerClasses); i++)
        {
            if (!ShowTriggers_ClientSees(client, g_cTriggerClasses[i]))
                continue;
            if (out[0])
                StrCat(out, maxlen, ",");
            StrCat(out, maxlen, g_cTriggerClasses[i]);
        }
    }
    if (!out[0])
        strcopy(out, maxlen, "-");
}

bool SeesClips(int client)
{
    return GetFeatureStatus(FeatureType_Native, "ShowPlayerClips_ClientSees") == FeatureStatus_Available && ShowPlayerClips_ClientSees(client);
}

// position, surface normal and kind of surface where the client is looking. Besides what bullets hit, the aim
// stops at grates, clips (while the player sees them), non-solid brushes (func_illusionary, ...) and the
// triggers the player sees with !st.
int g_iEnumClient;
float g_fEnumBest;
int g_iEnumEntity;
float g_fEnumPos[3], g_fEnumNormal[3];

bool TraceEye(int client, float pos[3], float normal[3], int &surf)
{
    float angles[3], origin[3], fwd[3], end[3];
    GetClientEyePosition(client, origin);
    GetClientEyeAngles(client, angles);
    GetAngleVectors(angles, fwd, NULL_VECTOR, NULL_VECTOR);
    ScaleVector(fwd, 32768.0);
    AddVectors(origin, fwd, end);

    int mask = MASK_SHOT | CONTENTS_GRATE;
    if (SeesClips(client))
        mask |= CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP;
    TR_TraceRayFilter(origin, end, mask, RayType_EndPoint, TraceEntityFilterPlayer);
    bool hit = TR_DidHit();
    float hitPos[3];
    if (hit)
    {
        TR_GetEndPosition(hitPos);
        TR_GetPlaneNormal(INVALID_HANDLE, normal);
        int entity = TR_GetEntityIndex();
        char surface[64];
        TR_GetSurfaceName(INVALID_HANDLE, surface, sizeof(surface));
        if (entity > 0)
            surf = EntitySurface(entity);
        else if (StrEqual(surface, "**studio**"))
            surf = PAINTSURF_PROP;
        else if (StrEqual(surface, "**displacement**") || (TR_GetDisplacementFlags() & DISPSURF_FLAG_SURFACE))
            surf = PAINTSURF_DISPLACEMENT;
        else
        {
            // clips report their sides as nodraw: tell them by what's just behind the hit point
            float inside[3];
            inside = normal;
            ScaleVector(inside, -0.5);
            AddVectors(hitPos, inside, inside);
            int contents = TR_GetPointContents(inside);
            // (invisible walls, skybox and nodraw walls are solid: the mod paints them like the world)
            if ((contents & (CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP)) && !(contents & CONTENTS_SOLID))
                surf = PAINTSURF_CLIP;
            else
                surf = PAINTSURF_WORLD;
        }
    }
    else
    {
        hitPos = end;
    }

    // non-solid brushes and visible triggers in front of that
    g_iEnumClient = client;
    g_fEnumBest = 0.999;
    g_iEnumEntity = -1;
    TR_EnumerateEntities(origin, hitPos, PARTITION_TRIGGER_EDICTS | PARTITION_NON_STATIC_EDICTS, RayType_EndPoint, EnumPaintable);
    if (g_iEnumEntity != -1)
    {
        pos = g_fEnumPos;
        normal = g_fEnumNormal;
        surf = (GetEntProp(g_iEnumEntity, Prop_Data, "m_usSolidFlags") & 0x0008) ? PAINTSURF_TRIGGER : EntitySurface(g_iEnumEntity);
        return true;
    }
    if (hit)
        pos = hitPos;
    return hit;
}

// sm_paintwhy: the aim step by step, for "why doesn't paint go on this?"
int g_iWhyClient;

public Action cmd_PaintWhy(int client, int args)
{
    if (client <= 0)
        return Plugin_Handled;
    float angles[3], origin[3], fwd[3], end[3], hitPos[3], normal[3];
    GetClientEyePosition(client, origin);
    GetClientEyeAngles(client, angles);
    GetAngleVectors(angles, fwd, NULL_VECTOR, NULL_VECTOR);
    ScaleVector(fwd, 32768.0);
    AddVectors(origin, fwd, end);
    bool clips = SeesClips(client);
    int mask = MASK_SHOT | CONTENTS_GRATE;
    if (clips)
        mask |= CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP;
    PrintToConsole(client, "[paintwhy] eye %.1f %.1f %.1f, sees clips %d (showplayerclips native %s), !st native %s", origin[0], origin[1], origin[2], clips,
        GetFeatureStatus(FeatureType_Native, "ShowPlayerClips_ClientSees") == FeatureStatus_Available ? "ok" : "MISSING",
        GetFeatureStatus(FeatureType_Native, "ShowTriggers_ClientSees") == FeatureStatus_Available ? "ok" : "MISSING");
    TR_TraceRayFilter(origin, end, mask, RayType_EndPoint, TraceEntityFilterPlayer);
    hitPos = end;
    if (TR_DidHit())
    {
        TR_GetEndPosition(hitPos);
        TR_GetPlaneNormal(INVALID_HANDLE, normal);
        char surface[64], classname[64] = "world";
        TR_GetSurfaceName(INVALID_HANDLE, surface, sizeof(surface));
        int entity = TR_GetEntityIndex();
        if (entity > 0)
            GetEntityClassname(entity, classname, sizeof(classname));
        PrintToConsole(client, "[paintwhy] bullet trace hits %s (%d) surface \"%s\" at %.1f %.1f %.1f normal %.2f %.2f %.2f, contents %d",
            classname, entity, surface, hitPos[0], hitPos[1], hitPos[2], normal[0], normal[1], normal[2], TR_GetPointContents(hitPos));
    }
    else
        PrintToConsole(client, "[paintwhy] bullet trace hits nothing");
    g_iWhyClient = client;
    for (int pass = 0; pass < 3; pass++)
    {
        int partition = pass == 0 ? PARTITION_TRIGGER_EDICTS : pass == 1 ? PARTITION_NON_STATIC_EDICTS : PARTITION_SOLID_EDICTS;
        PrintToConsole(client, "[paintwhy] entities along the line, partition %d:", partition);
        TR_EnumerateEntities(origin, hitPos, partition, RayType_EndPoint, EnumWhy);
    }
    int surf;
    float pos[3];
    bool ok = TraceEye(client, pos, normal, surf);
    PrintToConsole(client, "[paintwhy] => paint goes to %.1f %.1f %.1f, surface %d (%s)", pos[0], pos[1], pos[2], surf, ok ? "paints" : "nothing");
    PrintToChat(client, "[Paint] Aim: surface %d at %.0f %.0f %.0f. Details in console.", surf, pos[0], pos[1], pos[2]);
    return Plugin_Handled;
}

public bool EnumWhy(int entity)
{
    int client = g_iWhyClient;
    char classname[64], model[64];
    if (!IsValidEntity(entity))
    {
        PrintToConsole(client, "[paintwhy]   %d: not a valid entity", entity);
        return true;
    }
    GetEntityClassname(entity, classname, sizeof(classname));
    GetEntPropString(entity, Prop_Data, "m_ModelName", model, sizeof(model));
    int solidFlags = GetEntProp(entity, Prop_Data, "m_usSolidFlags");
    int solidType = GetEntProp(entity, Prop_Data, "m_nSolidType");
    bool sees = GetFeatureStatus(FeatureType_Native, "ShowTriggers_ClientSees") == FeatureStatus_Available && ShowTriggers_ClientSees(client, classname);
    TR_ClipCurrentRayToEntity(MASK_ALL, entity);
    PrintToConsole(client, "[paintwhy]   %d %s model \"%s\" solid type %d flags 0x%x, !st shows it %d; ray: hit %d startsolid %d fraction %.4f",
        entity, classname, model, solidType, solidFlags, sees, TR_DidHit(), TR_StartSolid(), TR_GetFraction());
    return true;
}

int EntitySurface(int entity)
{
    char model[4];
    GetEntPropString(entity, Prop_Data, "m_ModelName", model, sizeof(model));
    if (model[0] != '*')
        return PAINTSURF_PROP;
    char classname[64];
    GetEntityClassname(entity, classname, sizeof(classname));
    for (int i = 0; i < sizeof(g_cMovingClasses); i++)
    {
        if (StrEqual(classname, g_cMovingClasses[i]))
            return PAINTSURF_MOVING;
    }
    return PAINTSURF_ENTITY;
}

public bool EnumPaintable(int entity)
{
    if (entity <= MaxClients || !IsValidEntity(entity))
        return true;
    char model[4];
    GetEntPropString(entity, Prop_Data, "m_ModelName", model, sizeof(model));
    if (model[0] != '*')
        return true;
    char classname[64];
    GetEntityClassname(entity, classname, sizeof(classname));
    for (int i = 0; i < sizeof(g_cVolumeClasses); i++)
    {
        if (StrEqual(classname, g_cVolumeClasses[i]))
            return true;
    }
    // triggers only while the player sees them (!st); other non-solid brushes always
    bool trigger = (GetEntProp(entity, Prop_Data, "m_usSolidFlags") & 0x0008) != 0;	// FSOLID_TRIGGER
    if (trigger && (GetFeatureStatus(FeatureType_Native, "ShowTriggers_ClientSees") != FeatureStatus_Available ||
                    !ShowTriggers_ClientSees(g_iEnumClient, classname)))
        return true;
    TR_ClipCurrentRayToEntity(MASK_ALL, entity);
    if (TR_DidHit() && !TR_StartSolid() && TR_GetFraction() < g_fEnumBest)
    {
        g_fEnumBest = TR_GetFraction();
        g_iEnumEntity = entity;
        TR_GetEndPosition(g_fEnumPos);
        TR_GetPlaneNormal(INVALID_HANDLE, g_fEnumNormal);
    }
    return true;
}

public bool TraceEntityFilterPlayer(int entity, int contentsMask)
{
    return entity > MaxClients || !entity;
}

// ==================== Clear confirmation ====================

void ShowClearConfirmMenu(int client)
{
    Menu menu = new Menu(ClearConfirmMenuHandle);
    menu.SetTitle("Clear ALL paint in profile '%s'?", g_CurrentProfile[client]);

    static const char decoys[][16] =
    {
        "No",
        "Nope",
        "Definitely not",
        "Absolutely not",
        "Cancel",
        "Keep my paint",
        "Go back"
    };

    // The real option lands on a random slot each time, so 7 of the 8 keys
    // cancel and there is no muscle-memory way to clear by accident.
    int correct = GetRandomInt(0, 7);
    int decoy = 0;
    for (int slot = 0; slot < 8; slot++)
    {
        if (slot == correct)
        {
            menu.AddItem("yes", "Yes, clear my paint");
        }
        else
        {
            menu.AddItem("no", decoys[decoy]);
            decoy++;
        }
    }

    menu.Pagination = MENU_NO_PAGINATION;
    menu.ExitButton = false;
    menu.Display(client, 30);
}

public int ClearConfirmMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        char item[16];
        menu.GetItem(choice, item, sizeof(item));

        if (StrEqual(item, "yes"))
        {
            ShowClearConfirmMenu2(client);
        }
        else
        {
            PrintToChat(client, "[Paint] Clear cancelled.");
            g_hMainMenu.Display(client, 20);
        }
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }

    return 0;
}

void ShowClearConfirmMenu2(int client)
{
    Menu menu = new Menu(ClearConfirm2MenuHandle);
    menu.SetTitle("Are you really sure you want to clear ALL paint????");
    menu.AddItem("no", "No");
    menu.AddItem("yes", "Yes");
    menu.Pagination = MENU_NO_PAGINATION;
    menu.ExitButton = false;
    menu.Display(client, 30);
}

public int ClearConfirm2MenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        char item[16];
        menu.GetItem(choice, item, sizeof(item));

        if (StrEqual(item, "yes"))
        {
            ClearClientView(client);
            ClearClientPaintData(client);
            PrintToChat(client, "[Paint] cleared.");
        }
        else
        {
            PrintToChat(client, "[Paint] Clear cancelled.");
        }

        g_hMainMenu.Display(client, 20);
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }

    return 0;
}

// ==================== Point and Erase ====================

void ShowEraseMenu(int client)
{
    g_bIsPainting[client] = false;

    Menu menu = new Menu(EraseMenuHandle);
    menu.SetTitle("Point and Erase:\nLook at your paint to erase it.\n ");
    menu.AddItem("stop", "Stop erasing");
    menu.Pagination = MENU_NO_PAGINATION;
    menu.ExitButton = false;

    if (menu.Display(client, MENU_TIME_FOREVER))
    {
        g_bErasing[client] = true;
        PrintToChat(client, "[Paint] Erase mode \x04ON\x01. Look at your paint to erase it.");
    }
    else
    {
        delete menu;
    }
}

public int EraseMenuHandle(Menu menu, MenuAction action, int client, int choice)
{
    if (action == MenuAction_Select)
    {
        g_bErasing[client] = false;
        PrintToChat(client, "[Paint] Erase mode off.");
        g_hMainMenu.Display(client, 20);
    }
    else if (action == MenuAction_Cancel)
    {
        g_bErasing[client] = false;
        if (choice != MenuCancel_Disconnected && IsClientInGame(client))
        {
            PrintToChat(client, "[Paint] Erase mode off.");
        }
    }
    else if (action == MenuAction_End)
    {
        delete menu;
    }

    return 0;
}

void EraseNearPosition(int client, const float aimPos[3])
{
    ArrayList cache = g_hPaintCache[client];
    char auth[64], map[64];
    if (cache == null || cache.Length == 0 || g_hDatabase == null || !PaintOwner(client, auth, map))
        return;

    bool erased = false;
    char query[512];

    for (int idx = cache.Length - 1; idx >= 0; idx--)
    {
        float splat[CACHE_BLOCK];
        cache.GetArray(idx, splat, sizeof(splat));

        float splatPos[3];
        splatPos[0] = splat[0];
        splatPos[1] = splat[1];
        splatPos[2] = splat[2];

        if (InBrush(client, splatPos, splat[3], aimPos))
        {
            // Positions are stored with %.2f, so match with a small tolerance.
            Format(query, sizeof(query), "DELETE FROM paint_data WHERE steamid = '%s' AND map = '%s' AND profile = '%s' AND ABS(pos_x - (%.2f)) < 0.05 AND ABS(pos_y - (%.2f)) < 0.05 AND ABS(pos_z - (%.2f)) < 0.05",
                auth, map, g_CurrentProfile[client], splatPos[0], splatPos[1], splatPos[2]);
            g_hDatabase.Query(SQL_DeleteCallback, query);

            // the mod removes exactly that splat; classic decals need a redraw
            if (g_bHasMod[client])
                ClientCommand(client, "p2paint_erase %.2f %.2f %.2f 0.05", splatPos[0], splatPos[1], splatPos[2]);
            else
                erased = true;

            cache.Erase(idx);
        }
    }

    if (erased)
    {
        RequestPaintRedraw(client);
    }
}

// Recolor tool: the splats around where the player looks take the current colour / picture
// (in place: same position and size, so the paint keeps its shape)
void RecolorNearPosition(int client, const float aimPos[3])
{
    ArrayList cache = g_hPaintCache[client];
    if (cache == null || cache.Length == 0 || g_hDatabase == null)
        return;

    char auth[64], map[64], query[768], pic[48];
    if (!PaintOwner(client, auth, map))
        return;
    PictureName(g_PaintPic[client], pic, sizeof(pic));
    bool redraw = false;

    for (int idx = 0; idx < cache.Length; idx++)
    {
        float splat[CACHE_BLOCK];
        cache.GetArray(idx, splat, sizeof(splat));
        float splatPos[3];
        splatPos[0] = splat[0];
        splatPos[1] = splat[1];
        splatPos[2] = splat[2];
        if (!InBrush(client, splatPos, splat[3], aimPos))
            continue;

        int old[3], rgb[3];
        SplatRGB(splat, old);
        if (g_PaintColourIdx[client] == 0)
        {
            // random: once per splat (one that already has a preset colour keeps it unless it's the only change)
            bool isPreset = false;
            for (int k = 1; k < sizeof(g_iPresetRGB) && !isPreset; k++)
                isPreset = old[0] == g_iPresetRGB[k][0] && old[1] == g_iPresetRGB[k][1] && old[2] == g_iPresetRGB[k][2];
            rgb = isPreset ? old : g_iPresetRGB[GetRandomInt(1, 14)];
        }
        else
            rgb = g_PaintRGB[client];
        if (rgb[0] == old[0] && rgb[1] == old[1] && rgb[2] == old[2] && view_as<int>(splat[5]) == g_PaintPic[client])
            continue;

        splat[4] = view_as<float>((rgb[0] << 16) | (rgb[1] << 8) | rgb[2]);
        splat[5] = view_as<float>(g_PaintPic[client]);
        cache.SetArray(idx, splat, sizeof(splat));

        Format(query, sizeof(query), "UPDATE paint_data SET cr = %d, cg = %d, cb = %d, paint_color = %d, pic = '%s' WHERE steamid = '%s' AND map = '%s' AND profile = '%s' AND ABS(pos_x - (%.2f)) < 0.05 AND ABS(pos_y - (%.2f)) < 0.05 AND ABS(pos_z - (%.2f)) < 0.05",
            rgb[0], rgb[1], rgb[2], NearestPreset(rgb), pic, auth, map, g_CurrentProfile[client], splatPos[0], splatPos[1], splatPos[2]);
        g_hDatabase.Query(SQL_ErrorCheckCallback, query);

        if (g_bHasMod[client])
            ClientCommand(client, "p2paint_recolor %.2f %.2f %.2f 0.05 %d %d %d %s", splatPos[0], splatPos[1], splatPos[2],
                rgb[0], rgb[1], rgb[2], pic[0] ? pic : "-");
        else
            redraw = true;
    }
    if (redraw)
        RequestPaintRedraw(client);
}

void PushPaintCache(int client, const float splat[CACHE_BLOCK])
{
    if (g_hPaintCache[client] == null)
    {
        g_hPaintCache[client] = new ArrayList(CACHE_BLOCK);
    }
    g_hPaintCache[client].PushArray(splat, CACHE_BLOCK);
}

void ClearPaintCache(int client)
{
    CancelPaintRedraw(client);

    if (g_hPaintCache[client] != null)
    {
        g_hPaintCache[client].Clear();
    }
}

void CancelPaintRedraw(int client)
{
    g_bRedrawActive[client] = false;
    g_bRedrawDirty[client] = false;
    g_iRedrawOffset[client] = 0;
}

// Classic decals can't be removed one at a time, so erasing one wipes the client's decals and repaints the
// splats that are left from the cache, one chunk per frame (p2paint clients remove single splats instead).
// While a pass is running, further erases only mark it dirty instead of restarting it: the pass reads the live
// cache (so erased splats that haven't been drawn yet never reappear) and one follow-up pass at the end catches
// splats that were drawn before their erase. This keeps the paint visible and the erasing live instead of
// thrashing wipe/redraw from the start on every erase tick.
void RequestPaintRedraw(int client)
{
    if (g_bRedrawActive[client])
    {
        g_bRedrawDirty[client] = true;
        return;
    }

    g_bRedrawActive[client] = true;
    g_bRedrawDirty[client] = false;
    g_iRedrawOffset[client] = 0;
    ClearClientView(client);
    RequestFrame(Frame_RedrawChunk, GetClientUserId(client));
}

public void Frame_RedrawChunk(any userid)
{
    int client = GetClientOfUserId(userid);
    if (client <= 0 || !IsClientInGame(client) || !g_bRedrawActive[client])
        return;

    ArrayList cache = g_hPaintCache[client];
    if (cache == null)
    {
        g_bRedrawActive[client] = false;
        return;
    }

    int offset = g_iRedrawOffset[client];
    int end = offset + 40;
    if (end > cache.Length)
    {
        end = cache.Length;
    }

    for (int idx = offset; idx < end; idx++)
    {
        float splat[CACHE_BLOCK];
        cache.GetArray(idx, splat, sizeof(splat));
        SendSplat(client, splat);
    }

    g_iRedrawOffset[client] = end;

    if (end < cache.Length)
    {
        RequestFrame(Frame_RedrawChunk, userid);
        return;
    }

    g_bRedrawActive[client] = false;

    if (g_bRedrawDirty[client])
    {
        g_bRedrawDirty[client] = false;
        RequestPaintRedraw(client);
    }
}
