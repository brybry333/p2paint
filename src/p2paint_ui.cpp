//=============================================================================
// p2paint: the paint menu, the hold-to-paint keys, the aim ghost and the hint bar.
//
// Plain VGUI panels of the client (IClientPanel on a VPANEL from IVGui::AllocPanel, parented to the client
// DLL's root panel), the way the game's own menus work: the menu is a popup with mouse and keyboard input,
// so VGUI shows the cursor and the game stops turning the view while it's open. No hooks.
// The menu: colour blocks (the default colours, random, your custom colour), CUSTOM COLOUR (a pixel colour picker)
// and IMAGES (pictures added in game with ADD A FILE, and the game's sprays). Esc closes it.
//
// The server plugin owns the settings: it sends p2paint_ui_state, and the menu answers with sm_paintui ...
// (forwarded to the server like any unknown console command).
//=============================================================================
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>

#include "tier1/convar.h"
#include "tier1/strtools.h"
#include "vgui/IClientPanel.h"
#include "vgui/IPanel.h"
#include "vgui/ISurface.h"
#include "vgui/IVGui.h"
#include "vgui/IInput.h"
#include "ienginevgui.h"
#include "cdll_int.h"
#include "engine/IEngineTrace.h"
#include "gametrace.h"
#include "bspflags.h"
#include "inputsystem/ButtonCode.h"
#include "inputsystem/iinputsystem.h"
#include "inetchannelinfo.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imesh.h"
#include "mathlib/vmatrix.h"

#include "ihandleentity.h"
#include "basehandle.h"

#include "p2paint_ui.h"
#include "p2paint_pics.h"
#include "p2paint_surfaces.h"
#include "p2paint_platform.h"

#include "tier0/memdbgon.h"

using namespace vgui;

static ISurface *s_pSurface;
static IPanel *s_pPanel;
static IVGui *s_pVGui;
static IInput *s_pInput;
static IEngineVGui *s_pEngineVGui;
static IEngineTrace *s_pTrace;
static IInputSystem *s_pInputSystem;	// InputSystemVersion001: IsButtonDown slot 11, GetButtonPressedTick 12,
										// StringToButtonCode 29 (CS:S inputsystem.so's vtable matches the SDK header)
static IVEngineClient *s_pEngine;
static IMaterialSystem *s_pMatSys;
static IMaterial *s_pGhostDecalMaterial;	// the paint decal's texture, coloured per vertex, both sides (aim ghost)
static IMaterial *s_pRingMaterial;
static Vector s_vecCamera;
static bool s_bHaveCamera;

static ConVar p2paint_lastdir( "p2paint_lastdir", "", FCVAR_ARCHIVE, "p2paint: the folder ADD A FILE opened last" );

//-----------------------------------------------------------------------------
// What the server told us (p2paint_ui_state)
//-----------------------------------------------------------------------------
struct Preset_t { const char *name; int r, g, b; };
static const Preset_t s_Presets[] =
{
	{ "Random", 0, 0, 0 }, { "White", 255, 255, 255 }, { "Black", 0, 0, 0 }, { "Blue", 0, 0, 255 }, { "Light Blue", 135, 206, 250 },
	{ "Brown", 139, 69, 19 }, { "Cyan", 0, 255, 255 }, { "Green", 0, 255, 0 }, { "Dark Green", 0, 100, 0 }, { "Red", 255, 0, 0 },
	{ "Orange", 255, 165, 0 }, { "Yellow", 255, 255, 0 }, { "Pink", 255, 20, 147 }, { "Light Pink", 255, 182, 193 }, { "Purple", 128, 0, 128 },
};
static const int NUM_PRESETS = ARRAYSIZE( s_Presets );

// the 8 paint sizes: radius = what shows (the plugin's numbers), half = the ghost quad's half width
// (= 32 x $decalscale: decals drawn as sprays are 64 x $decalscale units across)
static const float s_SizeRadius[8] = { 6.2f, 8.6f, 12.3f, 18.5f, 27.7f, 40.0f, 55.4f, 73.8f };
static const int NUM_SIZES = 8;
static int SizeIndex( float radius )
{
	int best = 0;
	for ( int i = 1; i < NUM_SIZES; i++ )
		if ( fabsf( logf( radius / s_SizeRadius[i] ) ) < fabsf( logf( radius / s_SizeRadius[best] ) ) )
			best = i;
	return best;
}

static struct State_t
{
	int colour = 10;					// 0 random, 1.. preset, -1 custom
	int rgb[3] = { 255, 165, 0 };
	float radius = 27.7f;
	bool painting = false, erasing = false, recoloring = false;
	bool clips = false;					// the aim stops at clips (the player sees them: !showclips)
	std::string triggers;				// ... and at triggers of these classes (comma-separated: what !st shows)
	std::string pic;					// painting with this picture ("" = the colour)
	std::string profile = "default";
	bool lists = false;					// the plugin sends backup / painter lists (paint_3 5.2+): the menu has those pages
} st;

// the paint key erases instead of painting (the menu's PAINT KEY switch; recolor is the server's recoloring flag)
static bool s_bPaintKeyErases;

// lists the server sends for the menu's BACKUPS and COPY pages; actions name an entry by its position
struct ListItem_t { std::string label; int count; bool current; };
static struct UIList_t { std::vector<ListItem_t> items; bool loading = false; } s_Backups, s_Painters;

static void Send( const char *fmt, ... )
{
	if ( !s_pEngine )
		return;
	char cmd[512];
	va_list ap;
	va_start( ap, fmt );
	V_vsnprintf( cmd, sizeof( cmd ), fmt, ap );
	va_end( ap );
	s_pEngine->ClientCmd_Unrestricted( cmd );
}

static void SetMenuOpen( bool open );
bool P2Paint_EnsureReady();		// p2paint_runtime.cpp: interfaces, the paint layer, these panels
bool P2Paint_EnsureUI();		// p2paint_runtime.cpp: these panels only
void P2Paint_WatchLevel();		// p2paint_runtime.cpp: every frame, sets up again after a level change

// Only servers running our paint plugin get the paint keys, the menu and our paint commands: the plugin says hello
// (p2paint_ui_state) on every map. It's remembered with the server's address and the map, so another server -
// even on the same map - starts without it. Anywhere else +paint / +erase go to the server like before.
IVEngineClient *P2Paint_Engine();	// p2paint_runtime.cpp: the engine, connected at load (ours only after P2UI_Init)
static std::string s_AwareOf;		// "<address> <map>" of the last hello
static std::string Connection()
{
	IVEngineClient *e = P2Paint_Engine();
	INetChannelInfo *ch = e ? e->GetNetChannelInfo() : NULL;
	return ch ? std::string( ch->GetAddress() ) + " " + e->GetLevelName() : "";
}
static void MarkServerAware()
{
	std::string c = Connection();
	if ( c != s_AwareOf )
		s_bPaintKeyErases = false;	// a new server or map: the paint key paints again (the server resets recolor too)
	s_AwareOf = c;
}
bool P2UI_ServerAware() { return !s_AwareOf.empty() && s_AwareOf == Connection(); }
static bool ServerAware()
{
	IVEngineClient *e = P2Paint_Engine();
	return e && e->IsInGame() && P2UI_ServerAware();
}

CON_COMMAND_F( p2paint_ui_state, "p2paint (from the server): paint settings as key value pairs", FCVAR_SERVER_CAN_EXECUTE )
{
	P2Paint_EnsureReady();
	MarkServerAware();
	for ( int i = 1; i + 1 < args.ArgC(); i += 2 )
	{
		const char *k = args[i], *v = args[i + 1];
		if ( !V_stricmp( k, "colour" ) ) st.colour = atoi( v );
		else if ( !V_stricmp( k, "rgb" ) && i + 3 < args.ArgC() )
		{
			st.rgb[0] = atoi( args[i + 1] ); st.rgb[1] = atoi( args[i + 2] ); st.rgb[2] = atoi( args[i + 3] );
			i += 2;
		}
		else if ( !V_stricmp( k, "size" ) ) st.radius = atof( v );
		else if ( !V_stricmp( k, "recoloring" ) ) st.recoloring = atoi( v ) != 0;
		else if ( !V_stricmp( k, "painting" ) ) st.painting = atoi( v ) != 0;
		else if ( !V_stricmp( k, "erasing" ) ) st.erasing = atoi( v ) != 0;
		else if ( !V_stricmp( k, "clips" ) ) st.clips = atoi( v ) != 0;
		else if ( !V_stricmp( k, "triggers" ) ) st.triggers = V_strcmp( v, "-" ) ? v : "";
		else if ( !V_stricmp( k, "profile" ) ) st.profile = v;
		else if ( !V_stricmp( k, "lists" ) ) st.lists = atoi( v ) != 0;
		else if ( !V_stricmp( k, "pic" ) ) st.pic = V_strcmp( v, "-" ) ? v : "";
	}
}

CON_COMMAND_F( p2paint_ui_open, "p2paint (from the server): open the paint menu", FCVAR_SERVER_CAN_EXECUTE )
{
	if ( !P2UI_ServerAware() )
		return;
	if ( !P2Paint_EnsureReady() )
		Warning( "p2paint: not in game yet\n" );
	SetMenuOpen( true );
}

// p2paint_ui_list backups|painters begin / item "label" [count current] / end
CON_COMMAND_F( p2paint_ui_list, "p2paint (from the server): a list for the paint menu", FCVAR_SERVER_CAN_EXECUTE )
{
	if ( args.ArgC() < 3 || !P2UI_ServerAware() )
		return;
	UIList_t *list = !V_stricmp( args[1], "backups" ) ? &s_Backups : !V_stricmp( args[1], "painters" ) ? &s_Painters : NULL;
	if ( !list )
		return;
	if ( !V_stricmp( args[2], "begin" ) )
	{
		list->items.clear();
		list->loading = true;
	}
	else if ( !V_stricmp( args[2], "end" ) )
		list->loading = false;
	else if ( !V_stricmp( args[2], "item" ) && args.ArgC() > 3 && list->items.size() < 1000 )
		list->items.push_back( { args[3], args.ArgC() > 4 ? atoi( args[4] ) : 0, args.ArgC() > 5 && atoi( args[5] ) != 0 } );
}

CON_COMMAND( p2paint_menu, "p2paint: open the paint menu (greyed out on servers without the paint plugin)" )
{
	if ( ServerAware() )
		Send( "sm_paintmenu" );		// the plugin sends the current settings and opens it
	else if ( P2Paint_EnsureUI() )
		SetMenuOpen( true );
}

//-----------------------------------------------------------------------------
// Drawing helpers
//-----------------------------------------------------------------------------
struct Col { int r, g, b, a; };
static const Col C_DIM = { 0, 0, 0, 140 }, C_BG = { 18, 17, 20, 250 }, C_PANEL2 = { 28, 26, 31, 255 }, C_TEXT = { 236, 230, 220, 255 },
				 C_SUB = { 150, 142, 132, 255 }, C_WOOD = { 222, 184, 135, 255 }, C_WOOD_LIGHT = { 240, 212, 170, 255 },
				 C_WOOD_DARK = { 92, 58, 30, 255 }, C_BTN = { 34, 31, 37, 255 }, C_BTN_HOVER = { 48, 44, 52, 255 },
				 C_DANGER = { 255, 110, 100, 255 }, C_BLACK = { 0, 0, 0, 255 }, C_WHITE = { 255, 255, 255, 255 };

enum { F_SMALL, F_BODY, F_BOLD, F_TITLE, F_COUNT };
static HFont s_Font[F_COUNT];
static int s_nFontsForHeight;
static float s_flScale = 1;
static int S( float v ) { return (int)( v * s_flScale + 0.5f ); }

static void SetupFonts( int screenTall )
{
	static int haveMono = -1;
	if ( haveMono < 0 )
		haveMono = s_pSurface->AddCustomFontFile( "Roboto Mono", "resource/p2paint/RobotoMono-Regular.ttf" ) &
				   s_pSurface->AddCustomFontFile( "Roboto Mono", "resource/p2paint/RobotoMono-Bold.ttf" );
	static const struct { int px, weight; } def[F_COUNT] = { { 12, 400 }, { 14, 400 }, { 14, 700 }, { 20, 700 } };
	for ( int i = 0; i < F_COUNT; i++ )
	{
		if ( !s_Font[i] )
			s_Font[i] = s_pSurface->CreateFont();
		if ( !haveMono || !s_pSurface->SetFontGlyphSet( s_Font[i], "Roboto Mono", S( def[i].px ), def[i].weight, 0, 0, ISurface::FONTFLAG_ANTIALIAS ) )
			s_pSurface->SetFontGlyphSet( s_Font[i], "Verdana", S( def[i].px ), def[i].weight, 0, 0, ISurface::FONTFLAG_ANTIALIAS );
	}
	s_nFontsForHeight = screenTall;
}

static void Rect( int x, int y, int w, int h, Col c ) { s_pSurface->DrawSetColor( c.r, c.g, c.b, c.a ); s_pSurface->DrawFilledRect( x, y, x + w, y + h ); }
static void Outline( int x, int y, int w, int h, Col c ) { s_pSurface->DrawSetColor( c.r, c.g, c.b, c.a ); s_pSurface->DrawOutlinedRect( x, y, x + w, y + h ); }

// a box with pixel-art corners: a p-thick border whose corners step in by one pixel
static void PixelBox( int x, int y, int w, int h, int p, Col fill, Col border )
{
	Rect( x + p, y + p, w - 2 * p, h - 2 * p, fill );
	Rect( x + p, y, w - 2 * p, p, border );
	Rect( x + p, y + h - p, w - 2 * p, p, border );
	Rect( x, y + p, p, h - 2 * p, border );
	Rect( x + w - p, y + p, p, h - 2 * p, border );
}

static std::wstring Widen( const char *s )
{
	wchar_t buf[512];
	V_UTF8ToUnicode( s, buf, sizeof( buf ) );
	return buf;
}

static int TextW( int f, const char *s )
{
	int w = 0, h = 0;
	std::wstring t = Widen( s );
	if ( s_Font[f] && !t.empty() )
		s_pSurface->GetTextSize( s_Font[f], t.c_str(), w, h );
	return w;
}

static int TextH( int f ) { return s_Font[f] ? s_pSurface->GetFontTall( s_Font[f] ) : 0; }

static void Text( int f, int x, int y, Col c, const char *s )
{
	std::wstring t = Widen( s );
	if ( !s_Font[f] || t.empty() )
		return;
	s_pSurface->DrawSetTextFont( s_Font[f] );
	s_pSurface->DrawSetTextColor( c.r, c.g, c.b, c.a );
	s_pSurface->DrawSetTextPos( x, y );
	s_pSurface->DrawPrintText( t.c_str(), (int)t.size() );
}

// text cut to a width (keeps the end: "...ures/cats/funny.gif")
static std::string FitLeft( int f, const std::string &s, int w )
{
	if ( TextW( f, s.c_str() ) <= w )
		return s;
	for ( size_t cut = 1; cut < s.size(); cut++ )
	{
		std::string t = "..." + s.substr( cut );
		if ( TextW( f, t.c_str() ) <= w )
			return t;
	}
	return "...";
}

static std::string FitRight( int f, const std::string &s, int w )
{
	if ( TextW( f, s.c_str() ) <= w )
		return s;
	for ( size_t n = s.size(); n > 0; n-- )
	{
		std::string t = s.substr( 0, n ) + "...";
		if ( TextW( f, t.c_str() ) <= w )
			return t;
	}
	return "...";
}

static void Upper( char *s )
{
	for ( ; *s; s++ )
		if ( *s >= 'a' && *s <= 'z' )
			*s -= 'a' - 'A';
}

//-----------------------------------------------------------------------------
// Immediate-mode input: the cursor, the left button and the wheel, read once per paint
//-----------------------------------------------------------------------------
static int s_mx, s_my;
static bool s_bClick, s_bDown;
static int s_nWheel;
static int s_nWheelUpTick = -1, s_nWheelDownTick = -1;

static bool Hover( int x, int y, int w, int h ) { return s_mx >= x && s_mx < x + w && s_my >= y && s_my < y + h; }

// the wheel: the input system's last "pressed" tick of the wheel buttons (no VGUI messages: their KeyValues
// are the game's newer layout)
static void ReadWheel( bool reset )
{
	if ( !s_pInputSystem )
		return;
	int up = s_pInputSystem->GetButtonPressedTick( MOUSE_WHEEL_UP ), down = s_pInputSystem->GetButtonPressedTick( MOUSE_WHEEL_DOWN );
	if ( !reset )
		s_nWheel = ( up != s_nWheelUpTick ? 1 : 0 ) - ( down != s_nWheelDownTick ? 1 : 0 );
	s_nWheelUpTick = up;
	s_nWheelDownTick = down;
}

// a button: pixel box, lighter when hovered, wood when "on". Returns true when clicked.
static bool Button( int x, int y, int w, int h, const char *label, bool on = false, bool enabled = true, Col textCol = C_TEXT )
{
	bool hover = enabled && Hover( x, y, w, h );
	int p = MAX( 2, S( 3 ) );
	PixelBox( x, y, w, h, p, on ? C_WOOD : hover ? C_BTN_HOVER : C_BTN, on ? C_WOOD_LIGHT : hover ? C_WOOD : C_WOOD_DARK );
	Col tc = !enabled ? C_SUB : on ? C_BLACK : textCol;
	Text( F_BOLD, x + ( w - TextW( F_BOLD, label ) ) / 2, y + ( h - TextH( F_BOLD ) ) / 2, tc, label );
	return hover && s_bClick;
}

static void CurrentRGB( int *rgb )
{
	if ( st.colour > 0 && st.colour < NUM_PRESETS ) { rgb[0] = s_Presets[st.colour].r; rgb[1] = s_Presets[st.colour].g; rgb[2] = s_Presets[st.colour].b; }
	else if ( st.colour == 0 ) rgb[0] = rgb[1] = rgb[2] = 255;
	else { rgb[0] = st.rgb[0]; rgb[1] = st.rgb[1]; rgb[2] = st.rgb[2]; }
}

//-----------------------------------------------------------------------------
// The colour blocks (drawn like the IMAGES page's thumbnails, so both pages look the same)
//-----------------------------------------------------------------------------
static const int PAGE_W = 576, PAGE_H = 368;	// the menu's content area at 1080p
// the 14 presets in grid order: light to dark, warm to cool (2 rows of 7)
static const int s_GridOrder[14] = { 1, 13, 12, 9, 10, 11, 5, 7, 8, 6, 4, 3, 14, 2 };
static const Col s_Rainbow[5] = { { 255, 40, 40, 255 }, { 255, 220, 0, 255 }, { 0, 220, 60, 255 }, { 0, 120, 255, 255 }, { 170, 40, 255, 255 } };

static Col Shade( Col c, float f ) { return { (int)( c.r * f ), (int)( c.g * f ), (int)( c.b * f ), c.a }; }
static Col Lift( Col c, float f ) { return { c.r + (int)( ( 255 - c.r ) * f ), c.g + (int)( ( 255 - c.g ) * f ), c.b + (int)( ( 255 - c.b ) * f ), c.a }; }

// the ring around a thumbnail or colour block (white = picked, wood = under the cursor), cut to clipTop..clipBottom
static void ThumbRing( int tx, int ty, int size, Col ring, int clipTop = INT_MIN, int clipBottom = INT_MAX )
{
	int top = MAX( ty, clipTop ), bottom = MIN( ty + size, clipBottom );
	int p2 = MAX( 2, S( 3 ) );
	if ( ty - p2 >= clipTop ) Rect( tx, ty - p2, size, p2, ring );
	if ( ty + size + p2 <= clipBottom ) Rect( tx, ty + size, size, p2, ring );
	Rect( tx - p2, top, p2, bottom - top, ring );
	Rect( tx + size, top, p2, bottom - top, ring );
}

// one colour block (rainbow stripes for random); true when clicked
static bool Swatch( int x, int y, int size, Col c, bool sel, bool rainbow = false )
{
	if ( rainbow )
		for ( int k = 0; k < 5; k++ )
			Rect( x, y + k * size / 5, size, ( k + 1 ) * size / 5 - k * size / 5, s_Rainbow[k] );
	else
		Rect( x, y, size, size, c );
	Outline( x, y, size, size, { 255, 255, 255, 28 } );	// so black shows on the dark menu
	bool hover = Hover( x, y, size, size );
	if ( sel || hover )
		ThumbRing( x, y, size, sel ? C_WHITE : C_WOOD );
	return hover && s_bClick;
}

// a small pixel paint blob (custom colour preview)
static void DrawBlob( int x, int y, int px, Col c )
{
	static const char *const blob[] = { ".XXXX.", "XXXXXX", "XXXXXX", "XXXXXX", ".xxxx." };
	for ( int j = 0; j < 5; j++ )
		for ( int i = 0; i < 6; i++ )
		{
			char ch = blob[j][i];
			if ( ch == '.' )
				continue;
			Col col = ch == 'x' ? Shade( c, 0.7f ) : c;
			if ( j == 1 && ( i == 1 || i == 2 ) )
				col = Lift( c, 0.6f );
			Rect( x + i * px, y + j * px, px, px, col );
		}
}

//-----------------------------------------------------------------------------
// colours
//-----------------------------------------------------------------------------
static void HsvToRgb( float h, float s, float v, int *rgb )
{
	h = fmodf( h, 360.0f ) / 60.0f;
	int i = (int)h;
	float f = h - i, p = v * ( 1 - s ), q = v * ( 1 - s * f ), t = v * ( 1 - s * ( 1 - f ) );
	float r, g, b;
	switch ( i )
	{
		case 0: r = v; g = t; b = p; break;
		case 1: r = q; g = v; b = p; break;
		case 2: r = p; g = v; b = t; break;
		case 3: r = p; g = q; b = v; break;
		case 4: r = t; g = p; b = v; break;
		default: r = v; g = p; b = q; break;
	}
	rgb[0] = (int)( r * 255 + 0.5f ); rgb[1] = (int)( g * 255 + 0.5f ); rgb[2] = (int)( b * 255 + 0.5f );
}

static void RgbToHsv( const int *rgb, float *h, float *s, float *v )
{
	float r = rgb[0] / 255.0f, g = rgb[1] / 255.0f, b = rgb[2] / 255.0f;
	float mx = MAX( r, MAX( g, b ) ), mn = MIN( r, MIN( g, b ) ), d = mx - mn;
	*v = mx;
	*s = mx > 0 ? d / mx : 0;
	if ( d <= 0 ) *h = 0;
	else if ( mx == r ) *h = 60 * fmodf( ( g - b ) / d + 6, 6 );
	else if ( mx == g ) *h = 60 * ( ( b - r ) / d + 2 );
	else *h = 60 * ( ( r - g ) / d + 4 );
}

//-----------------------------------------------------------------------------
// A bare VGUI panel (what vgui_controls' Panel does for us normally)
//-----------------------------------------------------------------------------
class CP2BasePanel : public IClientPanel
{
public:
	CP2BasePanel( const char *name ) : m_VPanel( 0 ), m_pszName( name ) {}

	bool Create( bool popup )
	{
		if ( m_VPanel )
			return true;
		m_VPanel = s_pVGui->AllocPanel();
		if ( !m_VPanel )
			return false;
		s_pPanel->Init( m_VPanel, this );
		s_pPanel->SetParent( m_VPanel, s_pEngineVGui->GetPanel( PANEL_CLIENTDLL ) );
		int sw, sh;
		s_pSurface->GetScreenSize( sw, sh );
		s_pPanel->SetPos( m_VPanel, 0, 0 );
		s_pPanel->SetSize( m_VPanel, sw, sh );
		if ( popup )
		{
			s_pSurface->CreatePopup( m_VPanel, false, false, false, true, true );
			s_pPanel->SetMouseInputEnabled( m_VPanel, true );
			s_pPanel->SetKeyBoardInputEnabled( m_VPanel, true );
		}
		else
		{
			s_pPanel->SetMouseInputEnabled( m_VPanel, false );
			s_pPanel->SetKeyBoardInputEnabled( m_VPanel, false );
		}
		s_pPanel->SetVisible( m_VPanel, false );
		s_pVGui->AddTickSignal( m_VPanel, 0 );	// OnTick every frame (paint stops while the game menu is up)
		return true;
	}

	void FitScreen()
	{
		int sw, sh, w, h;
		s_pSurface->GetScreenSize( sw, sh );
		s_pPanel->GetSize( m_VPanel, w, h );
		if ( w != sw || h != sh )
			s_pPanel->SetSize( m_VPanel, sw, sh );
	}

	virtual void Paint() = 0;

	// IClientPanel
	virtual VPANEL GetVPanel() { return m_VPanel; }
	virtual void Think() {}
	virtual void PerformApplySchemeSettings() {}
	virtual void PaintTraverse( bool forceRepaint, bool allowForce )
	{
		if ( !m_VPanel || !s_pPanel->IsVisible( m_VPanel ) )
			return;
		FitScreen();
		s_pSurface->PushMakeCurrent( m_VPanel, false );
		Paint();
		s_pSurface->PopMakeCurrent( m_VPanel );
	}
	virtual void Repaint() { s_pSurface->Invalidate( m_VPanel ); }
	virtual VPANEL IsWithinTraverse( int x, int y, bool traversePopups )
	{
		if ( !m_VPanel || !s_pPanel->IsVisible( m_VPanel ) || !s_pPanel->IsMouseInputEnabled( m_VPanel ) )
			return 0;
		return m_VPanel;
	}
	virtual void GetInset( int &top, int &left, int &right, int &bottom ) { top = left = right = bottom = 0; }
	virtual void GetClipRect( int &x0, int &y0, int &x1, int &y1 )
	{
		s_pPanel->GetClipRect( m_VPanel, x0, y0, x1, y1 );
	}
	virtual void OnChildAdded( VPANEL child ) {}
	virtual void OnSizeChanged( int newWide, int newTall ) {}
	virtual void InternalFocusChanged( bool lost ) {}
	virtual bool RequestInfo( KeyValues *outputData ) { return false; }
	virtual void RequestFocus( int direction ) {}
	virtual bool RequestFocusPrev( VPANEL existingPanel ) { return false; }
	virtual bool RequestFocusNext( VPANEL existingPanel ) { return false; }
	// (messages are KeyValues in the game's newer layout: we don't read them; input is polled instead)
	virtual void OnMessage( const KeyValues *params, VPANEL ifromPanel ) {}
	virtual VPANEL GetCurrentKeyFocus() { return 0; }
	virtual int GetTabPosition() { return 0; }
	virtual const char *GetName() { return m_pszName; }
	virtual const char *GetClassName() { return m_pszName; }
	virtual HScheme GetScheme() { return 0; }
	virtual bool IsProportional() { return false; }
	virtual bool IsAutoDeleteSet() { return false; }
	virtual void DeletePanel() {}
	virtual void *QueryInterface( EInterfaceID id ) { return NULL; }
	virtual Panel *GetPanel() { return NULL; }
	virtual const char *GetModuleName() { return "p2paint"; }
	virtual void OnTick() {}

	bool IsOpen() const { return m_VPanel && s_pPanel->IsVisible( m_VPanel ); }
	void Show( bool show )
	{
		if ( !m_VPanel )
			return;
		s_pPanel->SetVisible( m_VPanel, show );
		if ( show )
		{
			s_pPanel->MoveToFront( m_VPanel );
			if ( s_pPanel->IsPopup( m_VPanel ) )
				s_pSurface->MovePopupToFront( m_VPanel );
		}
	}

protected:
	VPANEL m_VPanel;
	const char *m_pszName;
};

// a picture's menu thumbnail (materials/paintpics/<name>.vmt), one VGUI texture each, kept
static int PictureTexture( const std::string &name )
{
	static std::map<std::string, int> s_Textures;
	std::map<std::string, int>::iterator it = s_Textures.find( name );
	if ( it != s_Textures.end() )
		return it->second;
	int id = s_pSurface->CreateNewTextureID( false );
	char path[128];
	V_snprintf( path, sizeof( path ), "paintpics/%s", name.c_str() );
	s_pSurface->DrawSetTextureFile( id, path, true, false );
	s_Textures[name] = id;
	return id;
}

static std::string HomeDir() { return P2Plat_Home(); }
static bool IsDir( const std::string &path ) { return P2Plat_IsDir( path ); }

// the folder above (stops at the root: "/" or a drive "C:/")
static std::string UpDir( const std::string &dir )
{
	std::string root = P2Plat_Root( dir );
	size_t cut = dir.rfind( '/' );
	return dir.size() <= root.size() || cut == std::string::npos || cut < root.size() ? root : dir.substr( 0, cut );
}

static std::string Tilde( const std::string &path )
{
	std::string home = HomeDir();
	if ( path.compare( 0, home.size(), home ) == 0 )
		return "~" + path.substr( home.size() );
	return path;
}

//-----------------------------------------------------------------------------
// The menu
//-----------------------------------------------------------------------------
enum { VIEW_PALETTE, VIEW_CUSTOM, VIEW_IMAGES, VIEW_FILES, VIEW_BACKUPS, VIEW_COPY, VIEW_CLEAR, VIEW_COUNT };

class CP2Menu : public CP2BasePanel
{
public:
	CP2Menu() : CP2BasePanel( "P2PaintMenu" ) {}

	void Opened()
	{
		m_nView = VIEW_PALETTE;
		ReadWheel( true );
	}

	void SetView( int view )
	{
		if ( view == VIEW_CUSTOM )
			StartCustom();
		else if ( view == VIEW_IMAGES )
			P2Pics_Refresh();
		else if ( view == VIEW_FILES )
		{
			OpenFiles();
			return;
		}
		else if ( view == VIEW_BACKUPS || view == VIEW_COPY )
		{
			UIList_t &list = view == VIEW_BACKUPS ? s_Backups : s_Painters;
			list.items.clear();
			list.loading = true;
			Send( view == VIEW_BACKUPS ? "sm_paintui backups" : "sm_paintui painters" );
			m_nListScroll = 0;
			m_nConfirm = m_nDeleting = -1;
		}
		else if ( view == VIEW_CLEAR )
			m_bClearArmed = false;
		m_nView = view;
	}

	// Esc: the engine opens its pause menu on Esc before VGUI sees the key, and the client's panels don't paint
	// while that menu is up - so this checks every frame: close ours and put the pause menu away again
	virtual void OnTick()
	{
		if ( !IsOpen() )
			return;
		if ( s_pEngineVGui->IsGameUIVisible() )
		{
			s_pEngine->ClientCmd_Unrestricted( "gameui_hide" );
			SetMenuOpen( false );
		}
		else if ( s_pInput->WasKeyPressed( KEY_ESCAPE ) )
			SetMenuOpen( false );
	}

	virtual void Paint()
	{
		if ( !s_pEngine || !s_pEngine->IsInGame() )
		{
			SetMenuOpen( false );	// left the server with it open
			return;
		}
		if ( s_pEngineVGui->IsGameUIVisible() || s_pInput->WasKeyPressed( KEY_ESCAPE ) )
		{
			if ( s_pEngineVGui->IsGameUIVisible() )
				s_pEngine->ClientCmd_Unrestricted( "gameui_hide" );
			SetMenuOpen( false );
			return;
		}
		int sw, sh;
		s_pSurface->GetScreenSize( sw, sh );
		s_flScale = sh / 1080.0f;
		if ( s_nFontsForHeight != sh || !s_Font[F_BODY] || !s_pSurface->GetFontTall( s_Font[F_BODY] ) )
			SetupFonts( sh );
		s_pInput->GetCursorPos( s_mx, s_my );
		bool down = s_pInput->IsMouseDown( MOUSE_LEFT );
		s_bClick = down && !s_bDown;
		s_bDown = down;
		ReadWheel( false );
		CheckAdd();

		Rect( 0, 0, sw, sh, C_DIM );
		const int pad = S( 22 );
		const int W = S( PAGE_W ) + 2 * pad, H = S( 44 ) + S( 10 ) + S( PAGE_H ) + S( 14 ) + S( 42 ) + S( 14 ) + S( 22 ) + S( 18 );
		int x0 = ( sw - W ) / 2, y0 = ( sh - H ) / 2;
		const int border = MAX( 2, S( 4 ) );
		PixelBox( x0, y0, W, H, border, C_BG, C_WOOD_DARK );

		// title bar
		const char *titles[VIEW_COUNT] = { "PAINT", "CUSTOM COLOUR", "IMAGES", "ADD A FILE", "BACKUPS", "COPY PAINT", "CLEAR" };
		Text( F_TITLE, x0 + pad, y0 + S( 14 ), C_WOOD_LIGHT, titles[m_nView] );
		if ( Button( x0 + W - pad - S( 32 ), y0 + S( 12 ), S( 32 ), S( 28 ), "X" ) )
		{
			SetMenuOpen( false );
			return;
		}
		if ( m_nView != VIEW_PALETTE && Button( x0 + W - pad - S( 32 ) - S( 8 ) - S( 84 ), y0 + S( 12 ), S( 84 ), S( 28 ), "BACK" ) )
		{
			m_nView = m_nView == VIEW_FILES ? VIEW_IMAGES : VIEW_PALETTE;
			s_bClick = false;
		}

		int cx = x0 + pad, cy = y0 + S( 54 ), cw = W - 2 * pad, ch = H - S( 54 ) - pad;
		// no paint plugin here: the menu as it is, greyed out and dead (X / Esc still close it)
		const bool off = !P2UI_ServerAware();
		if ( off )
		{
			m_nView = VIEW_PALETTE;
			s_bClick = s_bDown = false;
			s_mx = s_my = -100000;
		}
		if ( m_nView == VIEW_PALETTE ) PaletteView( cx, cy, cw, ch );
		else if ( m_nView == VIEW_CUSTOM ) CustomView( cx, cy, cw, ch );
		else if ( m_nView == VIEW_IMAGES ) ImagesView( cx, cy, cw, ch );
		else if ( m_nView == VIEW_FILES ) FilesView( cx, cy, cw, ch );
		else if ( m_nView == VIEW_BACKUPS ) BackupsView( cx, cy, cw, ch );
		else if ( m_nView == VIEW_COPY ) CopyView( cx, cy, cw, ch );
		else ClearView( cx, cy, cw, ch );
		if ( off )
		{
			Rect( cx - S( 4 ), cy - S( 4 ), cw + S( 8 ), ch + S( 8 ), { 18, 17, 20, 215 } );
			Text( F_BOLD, x0 + pad + TextW( F_TITLE, titles[m_nView] ) + S( 16 ), y0 + S( 14 ) + TextH( F_TITLE ) - TextH( F_BOLD ), C_DANGER,
				  "NOT ON THIS SERVER" );
		}
	}

private:
	// ---- the colour blocks ----------------------------------------------------------------------------
	void PaletteView( int x, int y, int w, int h )
	{
		// same sizes as the IMAGES grid: 70 px blocks, 10 px apart, a label over each section
		const int thumb = S( 70 ), gap = S( 10 ), label = S( 22 ), perRow = 7;
		int picked = st.pic.empty() ? st.colour : -2;	// -1 custom, -2 a picture
		int hover = -2;									// preset under the cursor (-1 custom), -2 none
		int cy = y;
		Text( F_BOLD, x, cy, C_WOOD, "COLOURS" );
		cy += label;
		for ( int i = 0; i < ARRAYSIZE( s_GridOrder ); i++ )
		{
			int preset = s_GridOrder[i];
			int tx = x + ( i % perRow ) * ( thumb + gap ), ty = cy + ( i / perRow ) * ( thumb + gap );
			Col c = { s_Presets[preset].r, s_Presets[preset].g, s_Presets[preset].b, 255 };
			if ( Swatch( tx, ty, thumb, c, picked == preset ) )
				PickPreset( preset );
			if ( Hover( tx, ty, thumb, thumb ) )
				hover = preset;
		}
		cy += ( ( ARRAYSIZE( s_GridOrder ) + perRow - 1 ) / perRow ) * ( thumb + gap );
		Text( F_BOLD, x, cy, C_WOOD, "RANDOM & CUSTOM" );
		cy += label;
		if ( Swatch( x, cy, thumb, C_WHITE, picked == 0, true ) )
			PickPreset( 0 );
		if ( Hover( x, cy, thumb, thumb ) )
			hover = 0;
		// your custom colour (the last one from CUSTOM COLOUR): one click to paint with it again
		int cx = x + thumb + gap;
		if ( Swatch( cx, cy, thumb, { st.rgb[0], st.rgb[1], st.rgb[2], 255 }, picked == -1 ) )
		{
			st.colour = -1;
			st.pic.clear();
			Send( "sm_paintui set rgb %d %d %d", st.rgb[0], st.rgb[1], st.rgb[2] );
		}
		if ( Hover( cx, cy, thumb, thumb ) )
			hover = -1;

		// what the paint key does, right of random / custom (lined up with the 4th block column)
		const int bh = S( 42 ), sgap = S( 10 );
		int kx = x + 3 * ( thumb + gap ), kw = x + perRow * ( thumb + gap ) - gap - kx, kbw = ( kw - 2 * sgap ) / 3;
		Text( F_BOLD, kx, cy - label, C_WOOD, "PAINT KEY" );
		int ky = cy + ( thumb - bh ) / 2;
		if ( Button( kx, ky, kbw, bh, "PAINT", !st.recoloring && !s_bPaintKeyErases ) )
			SetPaintKey( 0 );
		if ( Button( kx + kbw + sgap, ky, kbw, bh, "RECOLOR", st.recoloring && !s_bPaintKeyErases ) )
			SetPaintKey( 1 );
		if ( Button( kx + 2 * ( kbw + sgap ), ky, kbw, bh, "ERASE", s_bPaintKeyErases ) )
			SetPaintKey( 2 );
		cy += thumb + gap;

		// the player's paint on this map: backups, copy someone's, clear (pages fed by the plugin)
		char mine[96];
		V_snprintf( mine, sizeof( mine ), "MY PAINT  \xC2\xB7  %s", st.profile.c_str() );
		Upper( mine );
		Text( F_BOLD, x, cy, C_WOOD, FitRight( F_BOLD, mine, w ).c_str() );
		cy += label;
		int rw = perRow * ( thumb + gap ) - gap, rbw = ( rw - 2 * sgap ) / 3;
		if ( st.lists )
		{
			if ( Button( x, cy, rbw, bh, "BACKUPS" ) )
				SetView( VIEW_BACKUPS );
			if ( Button( x + rbw + sgap, cy, rbw, bh, "COPY FROM PLAYER" ) )
				SetView( VIEW_COPY );
			if ( Button( x + 2 * ( rbw + sgap ), cy, rbw, bh, "CLEAR", false, true, C_DANGER ) )
				SetView( VIEW_CLEAR );
		}
		else if ( Button( x, cy, rw, bh, "BACKUPS / COPY / CLEAR (SERVER MENU)" ) )
		{
			// an older paint plugin without the lists: its numbered menu has them
			SetMenuOpen( false );
			Send( "sm_paintmenuclassic" );
		}

		int by = y + S( PAGE_H ) + S( 14 ), bw = ( rw - sgap ) / 2;	// lined up with the grid above
		if ( Button( x, by, bw, bh, "CUSTOM COLOUR", st.pic.empty() && st.colour < 0 ) )
		{
			StartCustom();
			m_nView = VIEW_CUSTOM;
		}
		if ( Button( x + bw + sgap, by, bw, bh, "IMAGES", !st.pic.empty() ) )
		{
			P2Pics_Refresh();
			m_nScroll = 0;
			m_nView = VIEW_IMAGES;
		}
		// what's picked, the size, how to paint
		int ly = by + bh + S( 14 );
		char line[256], what[96];
		if ( hover >= 0 )
			V_strncpy( what, s_Presets[hover].name, sizeof( what ) );
		else if ( hover == -1 )
			V_snprintf( what, sizeof( what ), "custom #%02X%02X%02X", st.rgb[0], st.rgb[1], st.rgb[2] );
		else if ( !st.pic.empty() )
		{
			const P2Picture *p = P2Pics_Find( st.pic );
			V_snprintf( what, sizeof( what ), "%s", p ? p->label.c_str() : st.pic.c_str() );
		}
		else if ( st.colour >= 0 && st.colour < NUM_PRESETS )
			V_strncpy( what, s_Presets[st.colour].name, sizeof( what ) );
		else
			V_snprintf( what, sizeof( what ), "custom #%02X%02X%02X", st.rgb[0], st.rgb[1], st.rgb[2] );
		Upper( what );
		const char *key = s_pEngine->Key_LookupBinding( "+paint" );
		char keyName[32];
		V_strncpy( keyName, key ? key : "your +paint key", sizeof( keyName ) );
		Upper( keyName );
		V_snprintf( line, sizeof( line ), "%s  \xC2\xB7  SIZE %d/8  \xC2\xB7  hold %s + click to %s, wheel = size", what, SizeIndex( st.radius ) + 1, keyName,
					s_bPaintKeyErases ? "erase" : st.recoloring ? "recolor" : "paint" );
		Text( F_SMALL, x, ly + ( S( 22 ) - TextH( F_SMALL ) ) / 2, C_SUB, FitRight( F_SMALL, line, rw ).c_str() );
	}

	// 0 paint, 1 recolor (the server's recolor tool), 2 erase (ours: the paint key acts as +erase)
	void SetPaintKey( int mode )
	{
		s_bPaintKeyErases = mode == 2;
		bool recolor = mode == 1;
		if ( recolor != st.recoloring )
		{
			st.recoloring = recolor;
			Send( "sm_paintui recolor %d", recolor ? 1 : 0 );
		}
	}

	// ---- backups / copy / clear -----------------------------------------------------------------------
	// a scrolling list of rows; row( i, x, y, w, h ) draws one
	template <typename RowFn>
	void ListRows( int x, int y, int w, int h, const UIList_t &list, const char *empty, RowFn row )
	{
		if ( list.loading && list.items.empty() )
		{
			Text( F_BODY, x, y + S( 6 ), C_SUB, "loading..." );
			return;
		}
		if ( list.items.empty() )
		{
			Text( F_BODY, x, y + S( 6 ), C_SUB, empty );
			return;
		}
		const int rowH = S( 38 ), gw = w - S( 14 );
		int rows = (int)list.items.size(), visible = MAX( 1, h / rowH );
		int maxScroll = MAX( 0, rows - visible );
		if ( Hover( x, y, w, h ) && s_nWheel )
			m_nListScroll -= s_nWheel * 2;
		m_nListScroll = clamp( m_nListScroll, 0, maxScroll );
		for ( int r = 0; r < visible && m_nListScroll + r < rows; r++ )
		{
			int i = m_nListScroll + r, ry = y + r * rowH;
			Rect( x, ry, gw, rowH - S( 4 ), Hover( x, ry, gw, rowH - S( 4 ) ) ? C_BTN_HOVER : C_PANEL2 );
			row( i, x, ry, gw, rowH - S( 4 ) );
		}
		if ( maxScroll > 0 )
		{
			int bx = x + w - S( 6 ), bh = MAX( S( 30 ), h * visible / rows ), byy = y + ( h - bh ) * m_nListScroll / maxScroll;
			Rect( bx, y, S( 6 ), h, C_PANEL2 );
			Rect( bx, byy, S( 6 ), bh, C_WOOD_DARK );
			if ( s_bDown && Hover( bx - S( 8 ), y, S( 20 ), h ) )
				m_nListScroll = clamp( ( s_my - y - bh / 2 ) * maxScroll / MAX( 1, h - bh ), 0, maxScroll );
		}
	}

	// a button at the right end of a row, left of the ones already there (right moves past it); true when clicked
	bool RowButton( int &right, int ry, int rh, const char *label, bool enabled = true, Col textCol = C_TEXT )
	{
		int bw = TextW( F_BOLD, label ) + S( 24 );
		right -= bw + S( 6 );
		return Button( right, ry + S( 4 ), bw, rh - S( 8 ), label, false, enabled, textCol );
	}

	void BackupsView( int x, int y, int w, int h )
	{
		if ( !s_Backups.loading )
			m_nDeleting = -1;
		if ( Button( x, y, S( 190 ), S( 36 ), "+ BACK UP NOW" ) )
		{
			Send( "sm_paintui backup new" );
			s_Backups.loading = true;
		}
		char cur[96];
		V_snprintf( cur, sizeof( cur ), "painting in: %s", st.profile.c_str() );
		Text( F_SMALL, x + S( 204 ), y + ( S( 36 ) - TextH( F_SMALL ) ) / 2, C_SUB, FitRight( F_SMALL, cur, w - S( 204 ) ).c_str() );
		int ly = y + S( 50 );
		ListRows( x, ly, w, h - S( 50 ), s_Backups, "no paint on this map yet", [&]( int i, int rx, int ry, int rw, int rh )
		{
			const ListItem_t &it = s_Backups.items[i];
			int right = rx + rw - S( 2 );
			if ( m_nDeleting == i )
			{
				Text( F_BOLD, rx + S( 12 ), ry + ( rh - TextH( F_BOLD ) ) / 2, C_SUB, FitRight( F_BOLD, "DELETING " + it.label + "...", rw - S( 24 ) ).c_str() );
				return;
			}
			if ( m_nConfirm == i )
			{
				// NO where DELETE was: a quick double click doesn't delete
				if ( RowButton( right, ry, rh, "NO" ) )
					m_nConfirm = -1;
				if ( RowButton( right, ry, rh, "YES, DELETE", true, C_DANGER ) )
				{
					Send( "sm_paintui backup delete %d", i );
					s_Backups.loading = true;
					m_nConfirm = -1;
					m_nDeleting = i;
				}
				Text( F_BOLD, rx + S( 12 ), ry + ( rh - TextH( F_BOLD ) ) / 2, C_DANGER, FitRight( F_BOLD, "DELETE " + it.label + "?", right - rx - S( 20 ) ).c_str() );
				return;
			}
			if ( RowButton( right, ry, rh, "DELETE", true, C_DANGER ) )
				m_nConfirm = i;
			if ( RowButton( right, ry, rh, "LOAD", !it.current ) )
			{
				Send( "sm_paintui backup load %d", i );
				s_Backups.loading = true;
			}
			char n[48];
			V_snprintf( n, sizeof( n ), it.current ? "%d splats  \xC2\xB7  NOW" : "%d splats", it.count );
			int nw = TextW( F_SMALL, n );
			Text( F_SMALL, right - S( 12 ) - nw, ry + ( rh - TextH( F_SMALL ) ) / 2, it.current ? C_WOOD_LIGHT : C_SUB, n );
			Text( F_BODY, rx + S( 12 ), ry + ( rh - TextH( F_BODY ) ) / 2, C_TEXT, FitRight( F_BODY, it.label, right - S( 24 ) - nw - rx - S( 12 ) ).c_str() );
		} );
	}

	void CopyView( int x, int y, int w, int h )
	{
		Text( F_SMALL, x, y, C_SUB, "their paint on this map replaces yours (yours is backed up first)" );
		int ly = y + S( 30 );
		ListRows( x, ly, w, h - S( 30 ), s_Painters, "nobody else has paint on this map", [&]( int i, int rx, int ry, int rw, int rh )
		{
			const ListItem_t &it = s_Painters.items[i];
			int right = rx + rw - S( 2 );
			if ( m_nConfirm == i )
			{
				if ( RowButton( right, ry, rh, "NO" ) )
					m_nConfirm = -1;
				if ( RowButton( right, ry, rh, "YES, COPY", true, C_WOOD_LIGHT ) )
				{
					Send( "sm_paintui copy %d", i );
					m_nConfirm = -1;
					m_nView = VIEW_PALETTE;
				}
				Text( F_BOLD, rx + S( 12 ), ry + ( rh - TextH( F_BOLD ) ) / 2, C_WOOD_LIGHT, FitRight( F_BOLD, "REPLACE YOURS?", right - rx - S( 20 ) ).c_str() );
				return;
			}
			if ( RowButton( right, ry, rh, "COPY" ) )
				m_nConfirm = i;
			Text( F_BODY, rx + S( 12 ), ry + ( rh - TextH( F_BODY ) ) / 2, C_TEXT, FitRight( F_BODY, it.label, right - rx - S( 24 ) ).c_str() );
		} );
	}

	// two clicks on the same button, like the plugin's "are you really sure" (the cursor leaving it disarms)
	void ClearView( int x, int y, int w, int h )
	{
		char q[128];
		V_snprintf( q, sizeof( q ), "CLEAR ALL YOUR PAINT ON THIS MAP (%s)?", st.profile.c_str() );
		Upper( q );
		Text( F_BOLD, x, y + S( 10 ), C_TEXT, FitRight( F_BOLD, q, w ).c_str() );
		Text( F_SMALL, x, y + S( 10 ) + TextH( F_BOLD ) + S( 8 ), C_SUB, "it can't be undone - BACKUPS keeps a copy" );
		int by = y + S( 80 ), bw = ( w - S( 12 ) ) / 2, bh = S( 42 );
		if ( Button( x, by, bw, bh, "NO, KEEP IT" ) )
			m_nView = VIEW_PALETTE;
		int yx = x + bw + S( 12 );
		if ( m_bClearArmed && !Hover( yx, by, bw, bh ) )
			m_bClearArmed = false;
		if ( Button( yx, by, bw, bh, m_bClearArmed ? "SURE? CLICK AGAIN" : "YES, CLEAR IT", m_bClearArmed, true, C_DANGER ) )
		{
			if ( !m_bClearArmed )
				m_bClearArmed = true;
			else
			{
				Send( "sm_paintui clear" );
				m_bClearArmed = false;
				m_nView = VIEW_PALETTE;
			}
		}
		if ( Button( x, by + bh + S( 12 ), w, bh, "BACKUPS" ) )
			SetView( VIEW_BACKUPS );
	}

	void PickPreset( int preset )
	{
		st.colour = preset;
		st.pic.clear();
		Send( "sm_paintui set colour %d", preset );
	}

	// ---- custom colour --------------------------------------------------------------------------------
	enum { SV_CELLS = 24, HUE_CELLS = 24 };

	void StartCustom()
	{
		int rgb[3];
		CurrentRGB( rgb );
		if ( st.colour == 0 )
		{
			rgb[0] = 255; rgb[1] = 165; rgb[2] = 0;
		}
		float hh, ss, vv;
		RgbToHsv( rgb, &hh, &ss, &vv );
		m_nHue = (int)( hh / 360.0f * HUE_CELLS + 0.5f ) % HUE_CELLS;
		m_nSat = clamp( (int)( ss * ( SV_CELLS - 1 ) + 0.5f ), 0, SV_CELLS - 1 );
		m_nVal = clamp( (int)( ( 1 - vv ) * ( SV_CELLS - 1 ) + 0.5f ), 0, SV_CELLS - 1 );
		m_bGreys = ss < 0.02f;
	}

	void PickedRGB( int *rgb )
	{
		float s = m_bGreys ? 0.0f : m_nSat / (float)( SV_CELLS - 1 ), v = 1.0f - m_nVal / (float)( SV_CELLS - 1 );
		HsvToRgb( m_nHue * 360.0f / HUE_CELLS, s, v, rgb );
	}

	void CustomView( int x, int y, int w, int h )
	{
		const int cell = S( 13 ), sv = SV_CELLS * cell;
		// saturation (across) x brightness (down) for the chosen hue
		bool dragging = s_bDown && Hover( x, y, sv, sv );
		for ( int j = 0; j < SV_CELLS; j++ )
			for ( int i = 0; i < SV_CELLS; i++ )
			{
				int rgb[3];
				HsvToRgb( m_nHue * 360.0f / HUE_CELLS, m_bGreys ? 0.0f : i / (float)( SV_CELLS - 1 ), 1.0f - j / (float)( SV_CELLS - 1 ), rgb );
				Rect( x + i * cell, y + j * cell, cell, cell, { rgb[0], rgb[1], rgb[2], 255 } );
			}
		if ( dragging )
		{
			m_nSat = clamp( ( s_mx - x ) / cell, 0, SV_CELLS - 1 );
			m_nVal = clamp( ( s_my - y ) / cell, 0, SV_CELLS - 1 );
		}
		int mx0 = x + ( m_bGreys ? 0 : m_nSat ) * cell, my0 = y + m_nVal * cell;
		Outline( mx0 - 1, my0 - 1, cell + 2, cell + 2, C_BLACK );
		Outline( mx0, my0, cell, cell, C_WHITE );

		// hue: a column of cells, plus one grey cell at the bottom
		int hx = x + sv + S( 14 ), hw = S( 26 ), hc = sv / ( HUE_CELLS + 1 );
		for ( int i = 0; i <= HUE_CELLS; i++ )
		{
			int rgb[3];
			if ( i < HUE_CELLS )
				HsvToRgb( i * 360.0f / HUE_CELLS, 1, 1, rgb );
			else
				rgb[0] = rgb[1] = rgb[2] = 150;
			int cy = y + i * hc;
			Rect( hx, cy, hw, hc, { rgb[0], rgb[1], rgb[2], 255 } );
			bool sel = i < HUE_CELLS ? !m_bGreys && m_nHue == i : m_bGreys;
			if ( sel )
			{
				Outline( hx - 2, cy - 1, hw + 4, hc + 2, C_BLACK );
				Outline( hx - 1, cy, hw + 2, hc, C_WHITE );
			}
			if ( s_bDown && Hover( hx - S( 4 ), cy, hw + S( 8 ), hc ) )
			{
				m_bGreys = i == HUE_CELLS;
				if ( !m_bGreys )
					m_nHue = i;
			}
		}

		// the result
		int rx = hx + hw + S( 26 ), rw = x + w - rx;
		int rgb[3];
		PickedRGB( rgb );
		Col c = { rgb[0], rgb[1], rgb[2], 255 };
		const int bpx = S( 14 );
		PixelBox( rx, y, rw, S( 110 ), MAX( 2, S( 3 ) ), C_WOOD, C_WOOD_DARK );
		DrawBlob( rx + ( rw - 6 * bpx ) / 2, y + ( S( 110 ) - 5 * bpx ) / 2, bpx, c );
		char hex[32];
		V_snprintf( hex, sizeof( hex ), "#%02X%02X%02X", rgb[0], rgb[1], rgb[2] );
		Text( F_BOLD, rx, y + S( 124 ), C_TEXT, hex );
		V_snprintf( hex, sizeof( hex ), "%d %d %d", rgb[0], rgb[1], rgb[2] );
		Text( F_SMALL, rx, y + S( 124 ) + TextH( F_BOLD ) + S( 4 ), C_SUB, hex );
		if ( Button( rx, y + sv - S( 42 ), rw, S( 42 ), "USE IT" ) )
		{
			st.colour = -1;
			st.pic.clear();
			st.rgb[0] = rgb[0]; st.rgb[1] = rgb[1]; st.rgb[2] = rgb[2];
			Send( "sm_paintui set rgb %d %d %d", rgb[0], rgb[1], rgb[2] );
			m_nView = VIEW_PALETTE;
		}
	}

	// ---- images ---------------------------------------------------------------------------------------
	void CheckAdd()
	{
		P2AddStatus s = P2Pics_Status();
		if ( s.serial == m_nAddSerial )
			return;
		m_nAddSerial = s.serial;
		if ( s.state == P2ADD_DONE )
		{
			P2Pics_Refresh();
			if ( !s.added.empty() )
			{
				st.pic = s.added;
				Send( "sm_paintui set pic %s", s.added.c_str() );
			}
			m_nScroll = 0;
		}
	}

	void ImagesView( int x, int y, int w, int h )
	{
		P2AddStatus status = P2Pics_Status();
		bool busy = status.state == P2ADD_BUSY;
		if ( Button( x, y, S( 190 ), S( 36 ), busy ? "ADDING..." : "+ ADD A FILE", false, !busy ) )
		{
			OpenFiles();
			return;
		}
		if ( status.state != P2ADD_IDLE )
		{
			Col sc = status.state == P2ADD_FAILED ? C_DANGER : status.state == P2ADD_DONE ? C_WOOD_LIGHT : C_SUB;
			Text( F_SMALL, x + S( 204 ), y + ( S( 36 ) - TextH( F_SMALL ) ) / 2, sc, FitRight( F_SMALL, status.text, w - S( 204 ) ).c_str() );
		}

		// the grid: pictures, then sprays
		const std::vector<P2Picture> &list = P2Pics_List();
		int gy = y + S( 50 ), gh = h - S( 50 ) - S( 30 ), gw = w - S( 14 );
		const int thumb = S( 70 ), gap = S( 10 ), perRow = MAX( 1, ( gw + gap ) / ( thumb + gap ) ), label = S( 22 );
		int pics = 0;
		for ( const P2Picture &p : list )
			pics += !p.spray;
		int sprays = (int)list.size() - pics;
		int rowsPics = ( pics + perRow - 1 ) / perRow, rowsSprays = ( sprays + perRow - 1 ) / perRow;
		int content = label + ( pics ? rowsPics * ( thumb + gap ) : S( 26 ) ) + ( sprays ? label + rowsSprays * ( thumb + gap ) : 0 );
		int maxScroll = MAX( 0, content - gh );
		if ( Hover( x, gy, w, gh ) && s_nWheel )
			m_nScroll -= s_nWheel * ( thumb + gap );
		m_nScroll = clamp( m_nScroll, 0, maxScroll );

		int clip0, clip1, clip2, clip3;
		s_pPanel->GetClipRect( m_VPanel, clip0, clip1, clip2, clip3 );
		const P2Picture *hoverPic = NULL;
		int cy = gy - m_nScroll;
		auto section = [&]( const char *title, bool spray, int count )
		{
			if ( cy + label > gy && cy < gy + gh )
				Text( F_BOLD, x, cy, C_WOOD, title );
			cy += label;
			if ( !count )
			{
				if ( cy > gy && cy + S( 20 ) < gy + gh )
					Text( F_SMALL, x, cy, C_SUB, spray ? "no sprays in materials/vgui/logos" : "none yet - ADD A FILE turns a picture or GIF into one" );
				cy += S( 26 );
				return;
			}
			int i = 0;
			for ( const P2Picture &p : list )
			{
				if ( p.spray != spray )
					continue;
				int tx = x + ( i % perRow ) * ( thumb + gap ), ty = cy + ( i / perRow ) * ( thumb + gap );
				i++;
				if ( ty + thumb <= gy || ty >= gy + gh )
					continue;	// (scrolled out: partly visible rows still draw, clipped below)
				DrawThumb( p, tx, ty, thumb, gy, gy + gh );
				if ( Hover( tx, MAX( ty, gy ), thumb, MIN( ty + thumb, gy + gh ) - MAX( ty, gy ) ) )
				{
					hoverPic = &p;
					if ( s_bClick )
					{
						st.pic = p.name;
						Send( "sm_paintui set pic %s", p.name.c_str() );
					}
				}
			}
			cy += ( ( count + perRow - 1 ) / perRow ) * ( thumb + gap );
		};
		section( "PICTURES", false, pics );
		if ( sprays )
			section( "SPRAYS", true, sprays );

		// scroll bar
		if ( maxScroll > 0 )
		{
			int bx = x + w - S( 6 ), bh = MAX( S( 30 ), gh * gh / content ), byy = gy + ( gh - bh ) * m_nScroll / maxScroll;
			Rect( bx, gy, S( 6 ), gh, C_PANEL2 );
			Rect( bx, byy, S( 6 ), bh, C_WOOD_DARK );
			if ( s_bDown && Hover( bx - S( 8 ), gy, S( 20 ), gh ) )
				m_nScroll = clamp( ( s_my - gy - bh / 2 ) * maxScroll / MAX( 1, gh - bh ), 0, maxScroll );
		}

		// the name under the cursor (or what's picked)
		const P2Picture *shown = hoverPic ? hoverPic : P2Pics_Find( st.pic );
		if ( shown )
		{
			char line[160];
			V_snprintf( line, sizeof( line ), "%s%s%s  \xC2\xB7  %dx%d%s", shown == hoverPic ? "" : "picked: ", shown->label.c_str(),
						shown->spray ? " (spray)" : "", shown->width, shown->height, shown->frames > 1 ? "  \xC2\xB7  animated" : "" );
			Text( F_SMALL, x, y + h - S( 20 ), shown == hoverPic ? C_TEXT : C_SUB, FitRight( F_SMALL, line, w ).c_str() );
		}
	}

	void DrawThumb( const P2Picture &p, int tx, int ty, int size, int clipTop, int clipBottom )
	{
		bool sel = st.pic == p.name;
		int top = MAX( ty, clipTop ), bottom = MIN( ty + size, clipBottom );
		if ( bottom <= top )
			return;
		// checkerboard behind (transparent pictures)
		int ck = MAX( 2, size / 8 );
		for ( int j = 0; j < size; j += ck )
			for ( int i = 0; i < size; i += ck )
			{
				int y0 = ty + j, y1 = MIN( ty + j + ck, ty + size );
				y0 = MAX( y0, top ); y1 = MIN( y1, bottom );
				if ( y1 > y0 )
					Rect( tx + i, y0, MIN( ck, size - i ), y1 - y0, ( ( i + j ) / ck ) & 1 ? Col{ 46, 43, 50, 255 } : Col{ 36, 34, 40, 255 } );
			}
		// the picture, fitted (its texture can be wider than tall or the other way)
		float aw = (float)p.width, ah = (float)p.height;
		int dw = aw >= ah ? size : (int)( size * aw / ah ), dh = aw >= ah ? (int)( size * ah / aw ) : size;
		int dx = tx + ( size - dw ) / 2, dy = ty + ( size - dh ) / 2;
		int sy0 = MAX( dy, top ), sy1 = MIN( dy + dh, bottom );
		if ( sy1 > sy0 && dh > 0 )
		{
			s_pSurface->DrawSetTexture( PictureTexture( p.name ) );
			s_pSurface->DrawSetColor( 255, 255, 255, 255 );
			s_pSurface->DrawTexturedSubRect( dx, sy0, dx + dw, sy1, 0.0f, ( sy0 - dy ) / (float)dh, 1.0f, ( sy1 - dy ) / (float)dh );
		}
		bool hover = Hover( tx, top, size, bottom - top );
		if ( sel || hover )
			ThumbRing( tx, ty, size, sel ? C_WHITE : C_WOOD, clipTop, clipBottom );
	}

	// ---- the file picker ------------------------------------------------------------------------------
	void OpenFiles()
	{
		std::string dir = p2paint_lastdir.GetString();
		if ( dir.empty() || !IsDir( dir ) )
			dir = IsDir( HomeDir() + "/Pictures" ) ? HomeDir() + "/Pictures" : HomeDir();
		GoTo( dir );
		m_nView = VIEW_FILES;
	}

	void GoTo( const std::string &dir )
	{
		std::vector<P2DirEntry> entries;
		if ( !P2Pics_ListDir( dir, entries ) )
			return;		// can't open: stay where we are
		m_Dir = dir;
		m_Entries.swap( entries );
		m_nFileScroll = 0;
		p2paint_lastdir.SetValue( m_Dir.c_str() );
	}

	void FilesView( int x, int y, int w, int h )
	{
		// where we are + quick places
		Text( F_SMALL, x, y, C_SUB, FitLeft( F_SMALL, Tilde( m_Dir ), w ).c_str() );
		int qy = y + S( 22 ), qh = S( 30 );
		struct { const char *label; std::string path; } places[] =
		{
			{ "UP", UpDir( m_Dir ) },
			{ "HOME", HomeDir() }, { "PICTURES", HomeDir() + "/Pictures" }, { "DOWNLOADS", HomeDir() + "/Downloads" },
			{ "DESKTOP", HomeDir() + "/Desktop" }, { "VIDEOS", HomeDir() + "/Videos" },
		};
		int n = ARRAYSIZE( places ), qw = ( w - S( 8 ) * ( n - 1 ) ) / n;
		for ( int i = 0; i < n; i++ )
		{
			bool exists = IsDir( places[i].path );
			if ( Button( x + i * ( qw + S( 8 ) ), qy, qw, qh, places[i].label, false, exists ) )
				GoTo( places[i].path );
		}

		// the list
		int ly = qy + qh + S( 12 ), lh = h - ( ly - y ), rowH = S( 30 ), gw = w - S( 14 );
		int rows = (int)m_Entries.size(), visible = lh / rowH;
		int maxScroll = MAX( 0, rows - visible );
		if ( Hover( x, ly, w, lh ) && s_nWheel )
			m_nFileScroll -= s_nWheel * 3;
		m_nFileScroll = clamp( m_nFileScroll, 0, maxScroll );
		if ( m_Entries.empty() )
			Text( F_BODY, x, ly + S( 6 ), C_SUB, "no pictures, GIFs or folders here" );
		std::string open;
		for ( int r = 0; r < visible && m_nFileScroll + r < rows; r++ )
		{
			const P2DirEntry &e = m_Entries[m_nFileScroll + r];
			int ry = ly + r * rowH;
			bool hover = Hover( x, ry, gw, rowH - S( 2 ) );
			Rect( x, ry, gw, rowH - S( 2 ), hover ? C_BTN_HOVER : C_PANEL2 );
			// a little pixel icon: folder or picture
			int ip = MAX( 1, S( 3 ) ), ix = x + S( 10 ), iy = ry + ( rowH - S( 2 ) - 5 * ip ) / 2;
			if ( e.dir )
			{
				Rect( ix, iy, 3 * ip, ip, C_WOOD );
				Rect( ix, iy + ip, 6 * ip, 4 * ip, C_WOOD );
			}
			else
			{
				Rect( ix, iy, 6 * ip, 5 * ip, C_TEXT );
				Rect( ix + ip, iy + ip, 4 * ip, 3 * ip, { 90, 160, 230, 255 } );
				Rect( ix + ip, iy + 3 * ip, 4 * ip, ip, { 80, 170, 80, 255 } );
				Rect( ix + 3 * ip, iy + ip, ip, ip, { 255, 220, 0, 255 } );
			}
			std::string name = e.dir ? e.name + "/" : e.name;
			Text( F_BODY, x + S( 40 ), ry + ( rowH - S( 2 ) - TextH( F_BODY ) ) / 2, e.dir ? C_WOOD_LIGHT : C_TEXT,
				  FitRight( F_BODY, name, gw - S( 50 ) ).c_str() );
			if ( hover && s_bClick )
				open = e.name;
		}
		if ( maxScroll > 0 )
		{
			int bx = x + w - S( 6 ), bh = MAX( S( 30 ), lh * visible / MAX( 1, rows ) ), byy = ly + ( lh - bh ) * m_nFileScroll / maxScroll;
			Rect( bx, ly, S( 6 ), lh, C_PANEL2 );
			Rect( bx, byy, S( 6 ), bh, C_WOOD_DARK );
			if ( s_bDown && Hover( bx - S( 8 ), ly, S( 20 ), lh ) )
				m_nFileScroll = clamp( ( s_my - ly - bh / 2 ) * maxScroll / MAX( 1, lh - bh ), 0, maxScroll );
		}
		if ( !open.empty() )
		{
			std::string path = !m_Dir.empty() && m_Dir.back() == '/' ? m_Dir + open : m_Dir + "/" + open;
			if ( IsDir( path ) )
				GoTo( path );
			else if ( P2Pics_Add( path ) )
				m_nView = VIEW_IMAGES;
		}
	}

	int m_nView = VIEW_PALETTE;
	int m_nHue = 2, m_nSat = SV_CELLS - 1, m_nVal = 0;
	bool m_bGreys = false;
	int m_nScroll = 0, m_nFileScroll = 0, m_nAddSerial = 0;
	int m_nListScroll = 0, m_nConfirm = -1;		// BACKUPS / COPY: scroll, the row asking "sure?"
	int m_nDeleting = -1;						// BACKUPS: the row being deleted, until the new list is in
	bool m_bClearArmed = false;
	std::string m_Dir;
	std::vector<P2DirEntry> m_Entries;
};

static CP2Menu s_Menu;

// for testing from the console (and adding a picture without the menu)
void P2Paint_TestDecal( const Vector &pos, const Vector &normal, int surf, int size, int r, int g, int b, const char *pic );	// p2paint_runtime.cpp
static bool AimPoint( Vector *pPos, Vector *pNormal );
static ConVar p2paint_test_surf( "p2paint_test_surf", "-1", FCVAR_DONTRECORD,
	"p2paint_test: what the splat is on, like the plugin says (-1 look it up, 0 world, 1 displacement, 2 brush entity, 3 prop, 4 clip, 6 trigger); 4 = aim at clips" );

CON_COMMAND( p2paint_test, "p2paint_test [size 1-8] [r g b] [picture]: paint one splat where you look (only you see it, not saved)" )
{
	Vector pos, n;
	if ( !P2Paint_EnsureReady() || !AimPoint( &pos, &n ) )
	{
		Msg( "p2paint: nothing under the crosshair (or not in game)\n" );
		return;
	}
	int size = args.ArgC() > 1 ? atoi( args[1] ) - 1 : 4;
	int r = args.ArgC() > 4 ? atoi( args[2] ) : 255, g = args.ArgC() > 4 ? atoi( args[3] ) : 165, b = args.ArgC() > 4 ? atoi( args[4] ) : 0;
	const char *pic = args.ArgC() > 5 ? args[5] : args.ArgC() == 3 ? args[2] : NULL;
	P2Paint_TestDecal( pos, n, p2paint_test_surf.GetInt(), size, r, g, b, pic );
	Msg( "p2paint: test splat size %d at %.1f %.1f %.1f (normal %.2f %.2f %.2f)%s%s\n", clamp( size, 0, 7 ) + 1, pos.x, pos.y, pos.z, n.x, n.y, n.z,
		 pic ? ", picture " : "", pic ? pic : "" );
}

CON_COMMAND( p2paint_addfile, "p2paint_addfile <path>: turn a picture / GIF / VTF into a paint picture (what ADD A FILE does)" )
{
	if ( args.ArgC() < 2 )
	{
		Msg( "usage: p2paint_addfile <path>\n" );
		return;
	}
	P2Paint_EnsureReady();
	// one argument: the console took the quotes off; more: a path with spaces typed without quotes
	if ( !P2Pics_Add( args.ArgC() == 2 ? args[1] : args.ArgS() ) )
		Msg( "p2paint: still adding the last one\n" );
}

CON_COMMAND( p2paint_ui_view, "p2paint_ui_view palette|custom|images|files|backups|copy|clear: switch the paint menu's page (testing)" )
{
	static const char *const names[VIEW_COUNT] = { "palette", "custom", "images", "files", "backups", "copy", "clear" };
	for ( int i = 0; i < VIEW_COUNT; i++ )
		if ( args.ArgC() > 1 && !V_stricmp( args[1], names[i] ) )
			s_Menu.SetView( i );
}

//-----------------------------------------------------------------------------
// Paint / erase keys. Hold the paint key (+paint, e.g. V) or the erase key (+erase), then left click or drag to
// paint / erase; the wheel changes the size. While the key is held the mod borrows MOUSE1 and the wheel
// (bind -> ours, the player's binds back on release); the borrowed binds are also written to a file so a crash
// mid-hold can't leave them changed (P2UI_Init puts them back).
//-----------------------------------------------------------------------------
enum { TOOL_NONE, TOOL_PAINT, TOOL_ERASE };
static int s_nTool;
static bool s_bFiring;
static const struct { const char *key; ButtonCode_t code; const char *ours; } s_Borrowed[3] =
{
	{ "MOUSE1", MOUSE_LEFT, "+p2paint_fire" }, { "MWHEELUP", MOUSE_WHEEL_UP, "sm_sizeup" }, { "MWHEELDOWN", MOUSE_WHEEL_DOWN, "sm_sizedown" },
};
static const int NUM_BORROWED = 3;
static std::string s_SavedBind[NUM_BORROWED];
// a plain (not +) bind like sm_erase can't tell us when its key goes up: watch that key instead
static ButtonCode_t s_HoldKey = BUTTON_CODE_NONE;
static bool s_bBorrowed;

static std::string BorrowFile()
{
	return P2Plat_CacheDir() + "/borrowed-binds.txt";
}

static void SetBind( const char *key, const std::string &cmd )
{
	if ( cmd.empty() )
		Send( "unbind %s", key );
	else
	{
		std::string c = cmd;
		for ( size_t i = 0; i < c.size(); i++ )
			if ( c[i] == '"' )
				c[i] = '\'';
		Send( "bind %s \"%s\"", key, c.c_str() );
	}
}

static void BorrowKeys()
{
	if ( s_bBorrowed || !s_pEngine )
		return;
	std::string file;
	for ( int k = 0; k < NUM_BORROWED; k++ )
	{
		const char *b = s_pEngine->Key_BindingForKey( s_Borrowed[k].code );
		s_SavedBind[k] = b ? b : "";
		if ( s_SavedBind[k] == s_Borrowed[k].ours )
			s_SavedBind[k] = k ? "" : "+attack";	// left over from a crash before the file existed
		if ( k && ( s_SavedBind[k] == "sm_sizeup" || s_SavedBind[k] == "sm_sizedown" ) )
			s_SavedBind[k] = s_Borrowed[k].ours;	// already the player's own bind: keep it
		file += std::string( s_Borrowed[k].key ) + "\t" + s_SavedBind[k] + "\n";
	}
	if ( FILE *f = fopen( BorrowFile().c_str(), "w" ) )
	{
		fputs( file.c_str(), f );
		fclose( f );
	}
	for ( int k = 0; k < NUM_BORROWED; k++ )
		SetBind( s_Borrowed[k].key, s_Borrowed[k].ours );
	s_bBorrowed = true;
}

static void ReturnKeys()
{
	if ( !s_bBorrowed )
		return;
	for ( int k = 0; k < NUM_BORROWED; k++ )
		SetBind( s_Borrowed[k].key, s_SavedBind[k] );
	remove( BorrowFile().c_str() );
	s_bBorrowed = false;
}

// binds borrowed when the game went away (crash) go back
static void RecoverKeys()
{
	FILE *f = fopen( BorrowFile().c_str(), "r" );
	if ( !f )
		return;
	char line[512];
	while ( fgets( line, sizeof( line ), f ) )
	{
		char *tab = strchr( line, '\t' );
		if ( !tab )
			continue;
		*tab = 0;
		std::string cmd = tab + 1;
		while ( !cmd.empty() && ( cmd.back() == '\n' || cmd.back() == '\r' ) )
			cmd.pop_back();
		SetBind( line, cmd );
	}
	fclose( f );
	remove( BorrowFile().c_str() );
	Msg( "p2paint: put back the mouse binds borrowed before the game closed\n" );
}

static const char *ToolName() { return s_nTool == TOOL_ERASE ? "erase" : "paint"; }

// Servers without our plugin: the key goes to their paint plugin like before the mod. Not through Send: our
// s_pEngine only arrives with P2UI_Init, which runs after our plugin's hello, so on those servers it's NULL.
static void ForwardTool( int tool, bool down )
{
	if ( IVEngineClient *pEngine = P2Paint_Engine() )
		pEngine->ClientCmd_Unrestricted( tool == TOOL_ERASE ? ( down ? "cmd +erase" : "cmd -erase" ) : ( down ? "cmd +paint" : "cmd -paint" ) );
}

static void ToolDown( int tool )
{
	if ( !ServerAware() )
	{
		ForwardTool( tool, true );
		return;
	}
	if ( !P2Paint_EnsureReady() || s_nTool != TOOL_NONE )
		return;
	if ( s_Menu.IsOpen() )
		SetMenuOpen( false );
	s_nTool = tool;
	BorrowKeys();
}

static void ToolUp( int tool )
{
	if ( s_nTool != tool )
	{
		if ( s_nTool == TOOL_NONE && !ServerAware() )
			ForwardTool( tool, false );
		return;
	}
	if ( s_bFiring )
		Send( "sm_paintfire %s 0", ToolName() );
	s_bFiring = false;
	s_nTool = TOOL_NONE;
	s_HoldKey = BUTTON_CODE_NONE;
	ReturnKeys();
}

// sm_erase bound as a plain command: hold its key (we watch it), left click to erase
static void PlainToolDown( int tool, const char *command )
{
	if ( s_nTool != TOOL_NONE || !s_pEngine || !s_pInputSystem )
		return;
	const char *key = s_pEngine->Key_LookupBinding( command );
	ButtonCode_t code = key ? s_pInputSystem->StringToButtonCode( key ) : BUTTON_CODE_INVALID;
	if ( code == BUTTON_CODE_INVALID || code == BUTTON_CODE_NONE || !s_pInputSystem->IsButtonDown( code ) )
	{
		Msg( "p2paint: bind a key to %s and hold it (or bind %s)\n", command, tool == TOOL_ERASE ? "+erase" : "+paint" );
		return;
	}
	ToolDown( tool );
	if ( s_nTool == tool )
		s_HoldKey = code;
}

// every frame (the HUD panel's tick): the watched key went up -> done
static void WatchHoldKey()
{
	if ( s_nTool != TOOL_NONE && s_HoldKey != BUTTON_CODE_NONE && s_pInputSystem && !s_pInputSystem->IsButtonDown( s_HoldKey ) )
		ToolUp( s_nTool );
}

CON_COMMAND( sm_erase, "p2paint: hold this key, then left click / drag to erase (works as a plain bind: bind b sm_erase)" )
{
	PlainToolDown( TOOL_ERASE, "sm_erase" );
}

// the paint key: paints, or erases when the menu's PAINT KEY switch says ERASE (only with our plugin: elsewhere
// it stays the server's +paint)
static int s_nPaintKeyTool = TOOL_PAINT;
static void CmdPaintDown( const CCommand & )
{
	if ( s_nTool == TOOL_NONE )
		s_nPaintKeyTool = s_bPaintKeyErases && ServerAware() ? TOOL_ERASE : TOOL_PAINT;
	ToolDown( s_nPaintKeyTool );
}
static void CmdPaintUp( const CCommand & )
{
	ToolUp( s_nPaintKeyTool );
	s_nPaintKeyTool = TOOL_PAINT;	// (the hint bar tells the keys apart by this)
}
static void CmdEraseDown( const CCommand & ) { ToolDown( TOOL_ERASE ); }
static void CmdEraseUp( const CCommand & ) { ToolUp( TOOL_ERASE ); }
static void CmdFireDown( const CCommand & )
{
	if ( s_nTool == TOOL_NONE || s_bFiring )
		return;
	s_bFiring = true;
	Send( "sm_paintfire %s 1", ToolName() );
}
static void CmdFireUp( const CCommand & )
{
	if ( !s_bFiring )
		return;
	s_bFiring = false;
	Send( "sm_paintfire %s 0", ToolName() );
}
static ConCommand s_cmdPaintDown( "+paint", CmdPaintDown, "p2paint: hold, then left click / drag to paint (wheel: size)" );
static ConCommand s_cmdPaintUp( "-paint", CmdPaintUp, "p2paint" );
static ConCommand s_cmdEraseDown( "+erase", CmdEraseDown, "p2paint: hold, then left click / drag to erase (wheel: size)" );
static ConCommand s_cmdEraseUp( "-erase", CmdEraseUp, "p2paint" );
static ConCommand s_cmdFireDown( "+p2paint_fire", CmdFireDown, "p2paint: left click while the paint / erase key is held" );
static ConCommand s_cmdFireUp( "-p2paint_fire", CmdFireUp, "p2paint" );

static void ChangeSize( int dir )
{
	st.radius = s_SizeRadius[clamp( SizeIndex( st.radius ) + ( dir > 0 ? 1 : -1 ), 0, NUM_SIZES - 1 )];
	Send( "sm_paintui set size %.1f", st.radius );	// the server keeps it: the next hold starts at this size
}

CON_COMMAND( sm_sizeup, "p2paint: paint / erase size up (bind it, e.g. bind MWHEELUP sm_sizeup)" ) { ChangeSize( 1 ); }
CON_COMMAND( sm_sizedown, "p2paint: paint / erase size down (bind it, e.g. bind MWHEELDOWN sm_sizedown)" ) { ChangeSize( -1 ); }

//-----------------------------------------------------------------------------
// While a key is held: a ghost of what you'll paint where you aim, and a hint bar
//-----------------------------------------------------------------------------
// what the plugin's aim stops at: the world, props, brush entities (no players), clips while they're shown
class CP2AimFilter : public ITraceFilter
{
public:
	virtual bool ShouldHitEntity( IHandleEntity *pEntity, int contentsMask )
	{
		int i = pEntity ? pEntity->GetRefEHandle().GetEntryIndex() : 0;
		return i <= 0 || i > s_pEngine->GetMaxClients();
	}
	virtual TraceType_t GetTraceType() const { return TRACE_EVERYTHING; }
};

static bool AimPoint( Vector *pPos, Vector *pNormal )
{
	if ( !s_bHaveCamera || !s_pTrace || !s_pEngine )
		return false;
	QAngle ang;
	s_pEngine->GetViewAngles( ang );
	Vector fwd;
	AngleVectors( ang, &fwd );
	Ray_t ray;
	ray.Init( s_vecCamera, s_vecCamera + fwd * 16384.0f );
	CP2AimFilter filter;
	trace_t tr;
	int mask = MASK_SHOT | CONTENTS_GRATE;
	if ( st.clips || p2paint_test_surf.GetInt() == 4 )
		mask |= CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP;
	s_pTrace->TraceRay( ray, mask, &filter, &tr );
	// in front of that: the triggers the player sees and non-solid brushes (the engine's trace goes through them)
	Vector hit, n;
	if ( !tr.startsolid && P2Surf_AimOwn( s_vecCamera, fwd, tr.fraction * 16384.0f, st.triggers.c_str(), &hit, &n ) )
	{
		*pPos = hit;
		*pNormal = n;
		return true;
	}
	if ( tr.fraction >= 1.0f || tr.startsolid )
		return false;
	*pPos = tr.endpos;
	*pNormal = tr.plane.normal;
	return true;
}

// the axes a decal gets on a surface (p2paint_surfaces.cpp: upright on walls, along X on floors / ceilings)
static void DecalAxes( const Vector &n, Vector &s, Vector &t ) { P2Surf_DecalAxes( n, s, t ); }

// one decal like the engine draws it: halfS / halfT along the decal's axes, the colour per vertex
static void DrawDecalQuad( IMatRenderContext *ctx, IMaterial *pMaterial, const Vector &c, const Vector &n, float halfS, float halfT,
						   const int *rgb, int alpha )
{
	if ( !pMaterial )
		return;
	Vector s, t;
	DecalAxes( n, s, t );
	ctx->Bind( pMaterial );
	IMesh *pMesh = ctx->GetDynamicMesh( true );
	CMeshBuilder mb;
	mb.Begin( pMesh, MATERIAL_QUADS, 1 );
	const float q[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	for ( int k = 0; k < 4; k++ )
	{
		Vector p = c + s * ( q[k][0] * halfS ) + t * ( q[k][1] * halfT );
		mb.Position3fv( p.Base() );
		mb.Color4ub( rgb[0], rgb[1], rgb[2], alpha );
		mb.TexCoord2f( 0, ( q[k][0] + 1 ) * 0.5f, ( q[k][1] + 1 ) * 0.5f );
		mb.AdvanceVertex();
	}
	mb.End();
	pMesh->Draw();
}

// a flat ring on the surface (radius r, a band of width w)
static void DrawRing( IMatRenderContext *ctx, const Vector &c, const Vector &n, float r, float w, int cr, int cg, int cb, int ca )
{
	if ( !s_pRingMaterial )
		return;
	Vector u, v;
	DecalAxes( n, u, v );
	const int segs = 48;
	ctx->Bind( s_pRingMaterial );
	IMesh *pMesh = ctx->GetDynamicMesh( true );
	CMeshBuilder mb;
	mb.Begin( pMesh, MATERIAL_QUADS, segs );
	for ( int i = 0; i < segs; i++ )
	{
		float a0 = i * 2 * M_PI_F / segs, a1 = ( i + 1 ) * 2 * M_PI_F / segs;
		Vector d0 = u * cosf( a0 ) + v * sinf( a0 ), d1 = u * cosf( a1 ) + v * sinf( a1 );
		Vector q[4] = { c + d0 * ( r - w ), c + d0 * r, c + d1 * r, c + d1 * ( r - w ) };
		for ( int k = 0; k < 4; k++ )
		{
			mb.Position3fv( q[k].Base() );
			mb.Color4ub( cr, cg, cb, ca );
			mb.TexCoord2f( 0, 0, 0 );
			mb.AdvanceVertex();
		}
	}
	mb.End();
	pMesh->Draw();
}

// a picture's ghost material (its texture, vertex colour + alpha, both sides), one per picture, kept
static IMaterial *GhostPictureMaterial( const std::string &name )
{
	static std::map<std::string, IMaterial *> s_Materials;
	std::map<std::string, IMaterial *>::iterator it = s_Materials.find( name );
	if ( it != s_Materials.end() )
		return it->second;
	char matName[160];
	V_snprintf( matName, sizeof( matName ), "paintpics/%s_g", name.c_str() );
	IMaterial *pMat = s_pMatSys->FindMaterial( matName, TEXTURE_GROUP_DECAL, false );
	if ( pMat && pMat->IsErrorMaterial() )
		pMat = NULL;
	if ( pMat )
		pMat->IncrementReferenceCount();
	s_Materials[name] = pMat;
	return pMat;
}

void P2UI_DrawWorld( IMatRenderContext *ctx )
{
	if ( s_nTool == TOOL_NONE || s_Menu.IsOpen() )
		return;
	Vector pos, n;
	if ( !AimPoint( &pos, &n ) )
		return;
	Vector c = pos + n * 0.6f;
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PushMatrix();
	ctx->LoadIdentity();
	if ( s_nTool == TOOL_ERASE )
	{
		// what goes: a see-through red disc with a solid rim
		DrawRing( ctx, c, n, st.radius + 8.0f, st.radius + 8.0f, 255, 70, 70, 70 );
		DrawRing( ctx, c + n * 0.1f, n, st.radius + 8.0f, MAX( 1.5f, st.radius * 0.06f ), 255, 90, 90, 230 );
	}
	else
	{
		int size = SizeIndex( st.radius );
		int rgb[3];
		int colour = st.colour;
		if ( colour == 0 )
		{
			colour = 1 + (int)( Plat_FloatTime() * 2.0 ) % ( NUM_PRESETS - 1 );	// random: cycle
			rgb[0] = s_Presets[colour].r; rgb[1] = s_Presets[colour].g; rgb[2] = s_Presets[colour].b;
		}
		else
			CurrentRGB( rgb );
		const P2Picture *pic = !st.pic.empty() ? P2Pics_Find( st.pic ) : NULL;
		IMaterial *pPicMat = pic ? GhostPictureMaterial( pic->name ) : NULL;
		if ( pic && pPicMat )
		{
			// the picture as it will land, a little see-through
			static const int white[3] = { 255, 255, 255 };
			float w, h;
			P2Pics_DecalSize( pic->width, pic->height, size, &w, &h );
			DrawDecalQuad( ctx, pPicMat, c + n * 0.3f, n, w * 0.5f, h * 0.5f, white, 215 );
		}
		else
		{
			float half = 32.0f * P2_DECAL_SCALE[size];
			DrawDecalQuad( ctx, s_pGhostDecalMaterial, c + n * 0.3f, n, half, half, rgb, 215 );
		}
		// recolor: the reach of the tool
		if ( st.recoloring )
			DrawRing( ctx, c, n, st.radius + 8.0f, MAX( 1.0f, ( st.radius + 8.0f ) * 0.04f ), rgb[0], rgb[1], rgb[2], 230 );
	}
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PopMatrix();
}

//-----------------------------------------------------------------------------
// p2paint_selftest <file>: runs console commands from a file, timed in seconds (testing). Each line is
// "<seconds> <command>" (run that long after the line before), "ingame" (wait until in a game, then 3 s more),
// "# ..." a comment. Runs from the HUD panel's tick, so it works from the main menu too.
//-----------------------------------------------------------------------------
bool P2Paint_EnsureUI();	// p2paint_runtime.cpp
static std::vector<std::pair<float, std::string> > s_Script;
static size_t s_nScriptPos;
static double s_flScriptNext;
static bool s_bScriptWaitGame;

static void RunScript()
{
	while ( s_nScriptPos < s_Script.size() && s_pEngine )
	{
		if ( s_bScriptWaitGame )
		{
			if ( !s_pEngine->IsInGame() )
				return;
			s_bScriptWaitGame = false;
			s_flScriptNext = Plat_FloatTime() + 3.0 + s_Script[s_nScriptPos].first;
		}
		if ( Plat_FloatTime() < s_flScriptNext )
			return;
		const std::string &cmd = s_Script[s_nScriptPos++].second;
		if ( cmd == "ingame" )
			s_bScriptWaitGame = true;
		else
		{
			Msg( "p2paint selftest: %s\n", cmd.c_str() );
			Send( "%s\n", cmd.c_str() );
		}
		if ( s_nScriptPos < s_Script.size() && !s_bScriptWaitGame )
			s_flScriptNext = Plat_FloatTime() + s_Script[s_nScriptPos].first;
	}
}

CON_COMMAND( p2paint_selftest, "p2paint_selftest <file>: run timed console commands from a file (testing; see p2paint_ui.cpp)" )
{
	if ( args.ArgC() < 2 || !P2Paint_EnsureUI() )
		return;
	FILE *f = fopen( args[1], "r" );
	if ( !f )
	{
		Warning( "p2paint: can't open %s\n", args[1] );
		return;
	}
	s_Script.clear();
	char line[512];
	while ( fgets( line, sizeof( line ), f ) )
	{
		char *p = line;
		while ( *p == ' ' || *p == '\t' )
			p++;
		size_t n = strlen( p );
		while ( n && ( p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ' ) )
			p[--n] = 0;
		if ( !*p || *p == '#' )
			continue;
		if ( !V_strcmp( p, "ingame" ) )
		{
			s_Script.push_back( { 0.0f, "ingame" } );
			continue;
		}
		char *end;
		float delay = strtof( p, &end );
		while ( *end == ' ' || *end == '\t' )
			end++;
		if ( end != p && *end )
			s_Script.push_back( { delay, end } );
	}
	fclose( f );
	s_nScriptPos = 0;
	s_bScriptWaitGame = false;
	s_flScriptNext = Plat_FloatTime() + ( s_Script.empty() ? 0.0f : s_Script[0].first );
	Msg( "p2paint: selftest with %d lines\n", (int)s_Script.size() );
}

class CP2Hud : public CP2BasePanel
{
public:
	CP2Hud() : CP2BasePanel( "P2PaintHud" ) {}

	virtual void OnTick()
	{
		P2Paint_WatchLevel();
		WatchHoldKey();
		RunScript();
		// ADD A FILE / p2paint_addfile results in the console too
		P2AddStatus s = P2Pics_Status();
		if ( s.serial != m_nAddSerial )
		{
			m_nAddSerial = s.serial;
			if ( s.state != P2ADD_IDLE )
				Msg( "p2paint: %s\n", s.text.c_str() );
			if ( s.state == P2ADD_DONE )
				P2Pics_Refresh();
		}
	}
	int m_nAddSerial = 0;

	// the hint bar while a key is held
	virtual void Paint()
	{
		if ( s_nTool == TOOL_NONE || !s_pEngine || !s_pEngine->IsInGame() || s_pEngineVGui->IsGameUIVisible() || s_Menu.IsOpen() )
			return;
		int sw, sh;
		s_pSurface->GetScreenSize( sw, sh );
		s_flScale = sh / 1080.0f;
		if ( s_nFontsForHeight != sh || !s_Font[F_BODY] || !s_pSurface->GetFontTall( s_Font[F_BODY] ) )
			SetupFonts( sh );
		char head[160], keys[200], what[96];
		const P2Picture *pic = !st.pic.empty() ? P2Pics_Find( st.pic ) : NULL;
		if ( pic )
			V_strncpy( what, pic->label.c_str(), sizeof( what ) );
		else if ( st.colour >= 0 && st.colour < NUM_PRESETS )
			V_strncpy( what, s_Presets[st.colour].name, sizeof( what ) );
		else
			V_snprintf( what, sizeof( what ), "#%02X%02X%02X", st.rgb[0], st.rgb[1], st.rgb[2] );
		int size = SizeIndex( st.radius ) + 1;
		if ( s_nTool == TOOL_ERASE )
			V_snprintf( head, sizeof( head ), "ERASE  \xC2\xB7  size %d/8", size );
		else
			V_snprintf( head, sizeof( head ), "%s  \xC2\xB7  %s  \xC2\xB7  size %d/8", st.recoloring ? "RECOLOR" : "PAINT", what, size );
		const char *key = s_pEngine->Key_LookupBinding( s_HoldKey != BUTTON_CODE_NONE ? "sm_erase" : s_nTool == TOOL_ERASE && s_nPaintKeyTool != TOOL_ERASE ? "+erase" : "+paint" );
		V_snprintf( keys, sizeof( keys ), "Left click: %s  \xC2\xB7  Wheel: size  \xC2\xB7  Release %s: done",
					s_nTool == TOOL_ERASE ? "erase" : st.recoloring ? "recolor" : "paint", key ? key : "the key" );
		int bw = MIN( sw - S( 40 ), MAX( TextW( F_BOLD, head ), TextW( F_SMALL, keys ) ) + S( 56 ) );
		int bh = S( 16 ) + TextH( F_BOLD ) + S( 6 ) + TextH( F_SMALL ) + S( 14 );
		int bx = ( sw - bw ) / 2, by = sh - bh - S( 140 );
		PixelBox( bx, by, bw, bh, MAX( 2, S( 3 ) ), { 18, 17, 20, 225 }, s_nTool == TOOL_ERASE ? C_DANGER : C_WOOD_DARK );
		if ( s_nTool != TOOL_ERASE )
		{
			int rgb[3];
			CurrentRGB( rgb );
			Rect( bx + S( 12 ), by + S( 14 ), S( 8 ), TextH( F_BOLD ), pic ? C_WOOD : Col{ rgb[0], rgb[1], rgb[2], 255 } );
		}
		Text( F_BOLD, bx + S( 28 ), by + S( 14 ), C_TEXT, head );
		Text( F_SMALL, bx + S( 28 ), by + S( 14 ) + TextH( F_BOLD ) + S( 6 ), C_SUB, keys );
	}
};

static CP2Hud s_Hud;

static void SetMenuOpen( bool open )
{
	if ( !s_pSurface || !s_Menu.GetVPanel() )
		return;
	if ( open && s_pEngine && s_pEngine->Con_IsVisible() )
		s_pEngine->ClientCmd_Unrestricted( "hideconsole" );
	s_bDown = true;		// a click that opened it doesn't count
	if ( open && !s_Menu.IsOpen() )
		s_Menu.Opened();
	s_Menu.Show( open );
}

//-----------------------------------------------------------------------------
bool P2UI_Init( P2InterfaceFn pfnInterface, IVEngineClient *pEngine, IMaterialSystem *pMatSys )
{
	s_pEngine = pEngine;
	s_pMatSys = pMatSys;
	if ( !s_pSurface )		s_pSurface = (ISurface *)pfnInterface( "vguimatsurface", VGUI_SURFACE_INTERFACE_VERSION );
	if ( !s_pPanel )		s_pPanel = (IPanel *)pfnInterface( "vgui2", VGUI_PANEL_INTERFACE_VERSION );
	if ( !s_pVGui )			s_pVGui = (IVGui *)pfnInterface( "vgui2", VGUI_IVGUI_INTERFACE_VERSION );
	if ( !s_pInput )		s_pInput = (IInput *)pfnInterface( "vgui2", VGUI_INPUT_INTERFACE_VERSION );
	if ( !s_pEngineVGui )	s_pEngineVGui = (IEngineVGui *)pfnInterface( "engine", VENGINE_VGUI_VERSION );
	if ( !s_pTrace )		s_pTrace = (IEngineTrace *)pfnInterface( "engine", INTERFACEVERSION_ENGINETRACE_CLIENT );
	if ( !s_pInputSystem )	s_pInputSystem = (IInputSystem *)pfnInterface( "inputsystem", INPUTSYSTEM_INTERFACE_VERSION );
	if ( !s_pSurface || !s_pPanel || !s_pVGui || !s_pInput || !s_pEngineVGui )
		return false;
	// shipped VMTs (materials/p2paint/), never CreateMaterial with our KeyValues (newer layout in the game:
	// deleting ours at quit crashed)
	if ( !s_pGhostDecalMaterial && s_pMatSys )
	{
		s_pGhostDecalMaterial = s_pMatSys->FindMaterial( "p2paint/ghost_decal", TEXTURE_GROUP_OTHER, true );
		s_pGhostDecalMaterial->IncrementReferenceCount();
	}
	if ( !s_pRingMaterial && s_pMatSys )
	{
		s_pRingMaterial = s_pMatSys->FindMaterial( "p2paint/ui_ring", TEXTURE_GROUP_OTHER, true );
		s_pRingMaterial->IncrementReferenceCount();
	}
	static bool s_bRecovered;
	if ( !s_bRecovered )
	{
		s_bRecovered = true;
		RecoverKeys();
	}
	bool ok = s_Menu.Create( true ) && s_Hud.Create( false );
	if ( ok )
		s_Hud.Show( true );
	return ok;
}

void P2UI_SetCamera( const Vector &camera )
{
	s_vecCamera = camera;
	s_bHaveCamera = true;
	// the HUD panel is always "visible" to VGUI; it decides each frame whether to draw anything
	if ( s_Hud.GetVPanel() && !s_Hud.IsOpen() )
		s_Hud.Show( true );
}
