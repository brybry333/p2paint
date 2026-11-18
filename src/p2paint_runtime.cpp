//=============================================================================
// p2paint runtime: draws the server's paint as engine decals, any colour, pictures, and the paint layer (an
// ordinary client renderable) that draws the aim ghost.
//
// Everything here goes through interfaces the game exports (CreateInterface):
//   engine.so          VEngineClient014        level name, view angles, console commands
//                      VEngineEffects001       PlayerDecalShoot (slot 3), checked against CS:S engine.so 2026-10-27
//                      VModelInfoClient006     the world model (GetModel slot 2, GetModelIndex slot 3, checked)
//   client.so          ClientLeafSystem002     AddRenderable (slot 4) / RenderableChanged (slot 12),
//                                              verified against CS:S client.so 2026-10-26
//   materialsystem.so  VMaterialSystem082      materials, render context
//   filesystem_stdio   VBaseFileSystem011      the map and prop models from VPKs / the map's pak
// Console commands: Linux = shaderlib (the loader loads us as a shader library: CShaderDLL::Connect ->
// ConVar_Register); Windows = P2Paint_MainInit (p2paint_platform_win.cpp starts us).
//
// Why PlayerDecalShoot (the spray call) and not DecalShoot: it takes any material, so a colour needs no
// precached decal per colour and pictures need no server precache at all (the engine's decal name table has
// 512 slots). Each paint decal gets its own "player" tag (userdata), which the engine uses to replace a
// player's previous spray: shooting again with a splat's tag removes exactly that splat. Spray decals are
// never recycled by other decals and skip the engine's overlap culling, so paint stays until we remove it.
//
// The engine only decals world faces. Splats on displacements, props, brush entities, clips, triggers and
// invisible walls are cut out of the map's own triangles and drawn by the paint layer, same material
// (p2paint_surfaces.cpp); the plugin says what each splat landed on (older saved paint: we look it up).
//=============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "tier1/convar.h"
#include "tier1/interface.h"
#include "tier1/tier1.h"
#include "tier1/strtools.h"
#include "cdll_int.h"
#include "iclientrenderable.h"
#include "iclientunknown.h"
#include "basehandle.h"
#include "worldsize.h"
#include "iefx.h"
#include "engine/ivmodelinfo.h"
#include "engine/IClientLeafSystem.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "mathlib/vmatrix.h"
#include "filesystem.h"

#include "p2paint_ui.h"
#include "p2paint_pics.h"
#include "p2paint_surfaces.h"
#include "p2paint_enginepatch.h"
#include "p2paint_platform.h"
#include "p2paint_files.h"		// game/ embedded by tools/embed.py (build/gen)

#include "tier0/memdbgon.h"

// The leading part of client.so's IClientLeafSystem vtable (game/client/clientleafsystem.h).
// Only AddRenderable is called; the slots in between keep the layout.
abstract_class IP2LeafSystem : public IClientLeafSystemEngine
{
public:
	virtual void AddRenderable( IClientRenderable *pRenderable, RenderGroup_t group ) = 0;		// slot 4
	virtual bool IsRenderableInPVS( IClientRenderable *pRenderable ) = 0;
	virtual void SetSubSystemDataInLeaf( int leaf, int nSubSystemIdx, void *pData ) = 0;
	virtual void *GetSubSystemDataInLeaf( int leaf, int nSubSystemIdx ) = 0;
	virtual void SetDetailObjectsInLeaf( int leaf, int firstDetailObject, int detailObjectCount ) = 0;
	virtual void GetDetailObjectsInLeaf( int leaf, int &firstDetailObject, int &detailObjectCount ) = 0;
	virtual void DrawDetailObjectsInLeaf( int leaf, int frameNumber, int &firstDetailObject, int &detailObjectCount ) = 0;
	virtual bool ShouldDrawDetailObjectsInLeaf( int leaf, int frameNumber ) = 0;
	virtual void RenderableChanged( ClientRenderHandle_t handle ) = 0;							// slot 12
};

static IVEngineClient *s_pEngine;
static IP2LeafSystem *s_pLeafSystem;
static IMaterialSystem *s_pMatSys;
static IVEfx *s_pEffects;
static IVModelInfoClient *s_pModelInfo;
static IBaseFileSystem *s_pFileSystem;

//-----------------------------------------------------------------------------
// Interfaces from already-loaded game modules (p2paint_platform_*.cpp)
//-----------------------------------------------------------------------------
static void *GameInterface( const char *module, const char *name ) { return P2Plat_Interface( module, name ); }

static bool ConnectInterfaces()
{
	if ( !s_pEngine )		s_pEngine = (IVEngineClient *)GameInterface( "engine", "VEngineClient014" );
	if ( !s_pLeafSystem )	s_pLeafSystem = (IP2LeafSystem *)GameInterface( "client", "ClientLeafSystem002" );
	if ( !s_pMatSys )		s_pMatSys = (IMaterialSystem *)GameInterface( "materialsystem", "VMaterialSystem082" );
	if ( !s_pEffects )		s_pEffects = (IVEfx *)GameInterface( "engine", VENGINE_EFFECTS_INTERFACE_VERSION );
	if ( !s_pModelInfo )	s_pModelInfo = (IVModelInfoClient *)GameInterface( "engine", VMODELINFO_CLIENT_INTERFACE_VERSION );
	if ( !s_pFileSystem )	s_pFileSystem = (IBaseFileSystem *)GameInterface( "filesystem_stdio", "VBaseFileSystem011" );
	return s_pEngine && s_pLeafSystem && s_pMatSys && s_pEffects && s_pModelInfo;
}

// a file from the game's search paths (VPKs, custom/, download/, the current map's pak)
static bool ReadGameFile( const char *path, CUtlVector<unsigned char> &out )
{
	if ( !s_pFileSystem )
		return false;
	FileHandle_t h = s_pFileSystem->Open( path, "rb", "GAME" );
	if ( !h )
		return false;
	unsigned int n = s_pFileSystem->Size( h );
	out.SetCount( n );
	bool ok = !n || s_pFileSystem->Read( out.Base(), n, h ) == (int)n;
	s_pFileSystem->Close( h );
	return ok;
}

//-----------------------------------------------------------------------------
// Paint decals
//-----------------------------------------------------------------------------
struct Decal_t
{
	Vector pos;
	int size;
	int tag;		// PlayerDecalShoot userdata: shooting with it again replaces / removes this splat
	int surf;		// P2Surf_t: what it landed on
	Vector normal;	// of that surface (zero: unknown, the engine's decal only)
	bool engine;	// has an engine decal (world faces)
	// our own part, on what the engine can't decal (drawn by the paint layer)
	IMaterial *pMat;
	unsigned char rgba[4];
	P2DecalMesh_t mesh;
};
static std::vector<Decal_t> s_Decals;
static int s_nNextTag = 0x100000;		// far above player indices (1-64), which real sprays use as their tag
static const model_t *s_pWorld;
// out of the game (loading, disconnected) since the paint was set up? The engine wipes every decal at level
// change, and the client purges every renderable at level shutdown, the same map reloading included.
static bool s_bLeftGame = true;

static IMaterial *Material( const char *name )
{
	static std::map<std::string, IMaterial *> s_Cache;
	std::map<std::string, IMaterial *>::iterator it = s_Cache.find( name );
	if ( it != s_Cache.end() )
		return it->second;
	// shipped / written VMTs only (never CreateMaterial with our KeyValues: the game's layout is newer, and the
	// engine deleting ours crashed CS:S at every quit)
	IMaterial *pMat = s_pMatSys->FindMaterial( name, TEXTURE_GROUP_DECAL, false );
	if ( pMat && pMat->IsErrorMaterial() )
		pMat = NULL;
	if ( pMat )
		pMat->IncrementReferenceCount();
	s_Cache[name] = pMat;
	return pMat;
}

static IMaterial *PaintMaterial( int size, const char *pic )
{
	char name[96];
	if ( pic && pic[0] && V_strcmp( pic, "-" ) && P2Pics_ValidName( pic ) )
	{
		V_snprintf( name, sizeof( name ), "paintpics/%s_s%d", pic, size );
		if ( IMaterial *pMat = Material( name ) )
			return pMat;	// (a picture that's gone: the splat's colour)
	}
	V_snprintf( name, sizeof( name ), "p2paint/paint_s%d", size );
	return Material( name );
}

static void Shoot( IMaterial *pMat, int tag, const Vector &pos, const color32 &c )
{
	if ( !pMat || !s_pWorld )
		return;
	s_pEffects->PlayerDecalShoot( pMat, (void *)(intp)tag, 0, s_pWorld, vec3_origin, vec3_angle, pos, NULL, 0, c );
}

// removes a splat: its tag with a decal shot far outside the world, where it lands on nothing
static void Unshoot( int tag )
{
	static const color32 none = { 0, 0, 0, 0 };
	Shoot( PaintMaterial( 0, NULL ), tag, Vector( 60000, 60000, 60000 ), none );
}

static color32 Colour( const CCommand &args, int first, bool picture )
{
	color32 c = { 255, 255, 255, 255 };
	if ( !picture )
	{
		c.r = (unsigned char)clamp( atoi( args[first] ), 0, 255 );
		c.g = (unsigned char)clamp( atoi( args[first + 1] ), 0, 255 );
		c.b = (unsigned char)clamp( atoi( args[first + 2] ), 0, 255 );
	}
	return c;
}

static bool HasPicture( const char *pic, int size )
{
	if ( !pic || !pic[0] || !V_strcmp( pic, "-" ) || !P2Pics_ValidName( pic ) )
		return false;
	char name[96];
	V_snprintf( name, sizeof( name ), "paintpics/%s_s%d", pic, size );
	return Material( name ) != NULL;
}

static float ArgF( const CCommand &args, int i, float def ) { return args.ArgC() > i ? atof( args[i] ) : def; }

static ConVar p2paint_surfaces( "p2paint_surfaces", "1", FCVAR_ARCHIVE,
	"p2paint: 1 = paint on props, displacements, brush entities, clips, triggers, invisible walls too (0 = only where the engine decals: world faces)" );

// a decal's half size along its axes: the engine's spray rule (64 x $decalscale along the longer side)
static void HalfSize( int size, const char *pic, bool picture, float *halfS, float *halfT )
{
	*halfS = *halfT = 32.0f * P2_DECAL_SCALE[size];
	const P2Picture *p = picture ? P2Pics_Find( pic ) : NULL;
	if ( p && p->width > 0 && p->height > 0 )
	{
		float w, h;
		P2Pics_DecalSize( p->width, p->height, size, &w, &h );
		*halfS = w * 0.5f;
		*halfT = h * 0.5f;
	}
}

// (re)draws a splat in its colour / picture: the engine's decal (same tag: replaces the old one) and our part
static void Paint( Decal_t &d, const char *pic, color32 c )
{
	bool picture = HasPicture( pic, d.size );
	if ( picture )
		c.r = c.g = c.b = 255;
	d.pMat = PaintMaterial( d.size, pic );
	if ( d.engine )
		Shoot( d.pMat, d.tag, d.pos, c );
	d.rgba[0] = c.r; d.rgba[1] = c.g; d.rgba[2] = c.b; d.rgba[3] = c.a;
	float hs, ht;
	HalfSize( d.size, pic, picture, &hs, &ht );
	if ( p2paint_surfaces.GetBool() )
		P2Surf_Build( d.pos, d.normal, hs, ht, d.surf, d.mesh );
	else
		d.mesh.Clear();
}

// a new splat; surf P2SURF_UNKNOWN (paint saved before the plugin said): looked up from the map
static void AddSplat( const Vector &pos, int size, const char *pic, const color32 &c, const Vector &normal, int surf )
{
	Decal_t d;
	d.pos = pos;
	d.size = clamp( size, 0, 7 );
	d.tag = s_nNextTag++;
	d.normal = normal;
	d.surf = surf;
	if ( d.surf < P2SURF_WORLD || d.surf >= P2SURF_KINDS || d.normal.LengthSqr() < 0.5f )
	{
		d.normal.Init();
		d.surf = P2Surf_Find( pos, &d.normal );
	}
	d.engine = P2Surf_EngineToo( d.surf );
	s_Decals.push_back( d );
	Paint( s_Decals.back(), pic, c );
}

static void RemoveSplat( int i )
{
	if ( s_Decals[i].engine )
		Unshoot( s_Decals[i].tag );
	s_Decals.erase( s_Decals.begin() + i );
}

// our part of the paint, in the order it was painted (runs that share a material go in one batch)
static void DrawOwnDecals( IMatRenderContext *ctx, const Vector &camera )
{
	P2Surf_BeginFrame( camera );
	static std::vector<const P2DecalMesh_t *> s_Run;
	static std::vector<unsigned char> s_RunColours;
	IMaterial *pRunMat = NULL;
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PushMatrix();
	ctx->LoadIdentity();
	for ( size_t i = 0; i <= s_Decals.size(); i++ )
	{
		const Decal_t *d = i < s_Decals.size() ? &s_Decals[i] : NULL;
		if ( d && ( !d->pMat || !P2Surf_Visible( d->mesh ) ) )
			continue;
		if ( !d || d->pMat != pRunMat )
		{
			P2Surf_DrawRun( ctx, pRunMat, s_Run.data(), s_RunColours.data(), (int)s_Run.size() );
			s_Run.clear();
			s_RunColours.clear();
			if ( !d )
				break;
			pRunMat = d->pMat;
		}
		s_Run.push_back( &d->mesh );
		s_RunColours.insert( s_RunColours.end(), d->rgba, d->rgba + 4 );
	}
	ctx->MatrixMode( MATERIAL_MODEL );
	ctx->PopMatrix();
}

//-----------------------------------------------------------------------------
// The paint layer: a renderable so we draw in the world (the aim ghost) and know the camera
//-----------------------------------------------------------------------------
class CP2PaintLayer : public CDefaultClientRenderable
{
public:
	CP2PaintLayer()
	{
		m_szLevel[0] = 0;
		SetIdentityMatrix( m_Identity );
	}

	// IHandleEntity / IClientUnknown: not an entity
	virtual void SetRefEHandle( const CBaseHandle &handle ) {}
	virtual const CBaseHandle &GetRefEHandle() const { static CBaseHandle h; return h; }
	virtual ICollideable *GetCollideable() { return NULL; }
	virtual IClientNetworkable *GetClientNetworkable() { return NULL; }
	virtual IClientRenderable *GetClientRenderable() { return this; }
	virtual IClientEntity *GetIClientEntity() { return NULL; }
	virtual C_BaseEntity *GetBaseEntity() { return NULL; }
	virtual IClientThinkable *GetClientThinkable() { return NULL; }
	virtual IClientUnknown *GetIClientUnknown() { return this; }

	// IClientRenderable: sorted at the camera (drawn after the other translucent things in its leaf), culled
	// as big as the world
	virtual const Vector &GetRenderOrigin() { return m_vecCamera; }
	virtual const QAngle &GetRenderAngles() { return vec3_angle; }
	virtual const matrix3x4_t &RenderableToWorldTransform() { return m_Identity; }
	virtual bool ShouldDraw() { return true; }
	virtual bool IsTransparent() { return true; }
	virtual void GetRenderBounds( Vector &mins, Vector &maxs )
	{
		mins.Init( -1, -1, -1 );
		maxs.Init( 1, 1, 1 );
	}
	// the base version calls DefaultRenderBoundsWorldspace(), which lives (unexported) in client.so
	virtual void GetRenderBoundsWorldspace( Vector &mins, Vector &maxs )
	{
		mins.Init( -MAX_COORD_FLOAT, -MAX_COORD_FLOAT, -MAX_COORD_FLOAT );
		maxs.Init( MAX_COORD_FLOAT, MAX_COORD_FLOAT, MAX_COORD_FLOAT );
	}

	virtual int DrawModel( int flags )
	{
		CMatRenderContextPtr pRenderContext( s_pMatSys );
		VMatrix view;
		pRenderContext->GetMatrix( MATERIAL_VIEW, &view );
		m_vecCamera = view.InverseTR().GetTranslation();
		P2UI_SetCamera( m_vecCamera );
		DrawOwnDecals( pRenderContext, m_vecCamera );
		P2UI_DrawWorld( pRenderContext );
		return 1;
	}

	// (Re)register with the leaf system on a new session (the client purged our handle; the engine wiped the
	// decals, so ours are forgotten too) and set the rest up once.
	bool EnsureReady()
	{
		if ( !ConnectInterfaces() )
			return false;
		if ( !s_pEngine->IsInGame() )
		{
			s_bLeftGame = true;
			return false;
		}
		const char *level = s_pEngine->GetLevelName();
		if ( RenderHandle() != INVALID_CLIENT_RENDER_HANDLE && !s_bLeftGame && !V_strcmp( level, m_szLevel ) )
			return true;
		static bool s_bOnce;
		if ( !s_bOnce )
		{
			s_bOnce = true;
			if ( !P2UI_Init( GameInterface, s_pEngine, s_pMatSys ) )
				Warning( "p2paint: the paint menu isn't available (VGUI interfaces missing)\n" );
			P2Pics_Init( s_pEngine->GetGameDirectory() );
			P2Pics_Refresh();
		}
		// up to 32768 decals (the engine's pool, raised from 2048 in P2Paint_Init; if that patch missed, the
		// engine still stops at 2048)
		s_pEngine->ClientCmd_Unrestricted( "r_decals 32768\n" );
		int idx = s_pModelInfo->GetModelIndex( level );
		s_pWorld = idx > 0 ? s_pModelInfo->GetModel( idx ) : NULL;
		if ( !s_pWorld )
			s_pWorld = s_pModelInfo->GetModel( 1 );		// the world is model 1
		s_Decals.clear();
		// what the engine can't decal (props, clips, displacements, ...): from the map file, once per map
		P2Surf_Load( s_pEngine->GetGameDirectory(), level, ReadGameFile );
		RenderHandle() = INVALID_CLIENT_RENDER_HANDLE;	// stale after a level change
		s_pLeafSystem->AddRenderable( this, RENDER_GROUP_TRANSLUCENT_ENTITY );
		V_strncpy( m_szLevel, level, sizeof( m_szLevel ) );
		s_bLeftGame = false;
		Msg( "p2paint: paint ready on %s (world model %s)\n", level, s_pWorld ? "found" : "MISSING" );
		return true;
	}

	const char *Level() const { return m_szLevel; }

private:
	matrix3x4_t m_Identity;
	Vector m_vecCamera;
	char m_szLevel[MAX_PATH];
};

static CP2PaintLayer s_Layer;

//-----------------------------------------------------------------------------
// Console commands: what the server plugin sends (server-executable)
//-----------------------------------------------------------------------------
// the server plugin checks this (QueryClientConVar): 10+ = it sends p2paint_decal and our menu;
// 11: also paint on props, displacements, brush entities, clips, triggers, invisible walls
static ConVar p2paint_version( "p2paint_version", "11", FCVAR_DONTRECORD, "p2paint client mod version" );

CON_COMMAND_F( p2paint_decal, "p2paint_decal x y z size(0-7) r g b [picture [nx ny nz surface]]: one paint splat", FCVAR_SERVER_CAN_EXECUTE )
{
	if ( args.ArgC() < 8 )
	{
		Msg( "usage: p2paint_decal x y z size(0-7) r g b [picture [nx ny nz surface]]\n" );
		return;
	}
	if ( !P2UI_ServerAware() || !s_Layer.EnsureReady() )
		return;
	const char *pic = args.ArgC() > 8 ? args[8] : NULL;
	Vector normal( ArgF( args, 9, 0 ), ArgF( args, 10, 0 ), ArgF( args, 11, 0 ) );
	int surf = args.ArgC() > 12 ? atoi( args[12] ) : P2SURF_UNKNOWN;
	Vector pos( atof( args[1] ), atof( args[2] ), atof( args[3] ) );
	AddSplat( pos, atoi( args[4] ), pic, Colour( args, 5, false ), normal, surf );
}

// p2paint_test (p2paint_ui.cpp): one splat where you look, drawn like p2paint_decal (not saved anywhere)
void P2Paint_TestDecal( const Vector &pos, const Vector &normal, int surf, int size, int r, int g, int b, const char *pic )
{
	if ( !s_Layer.EnsureReady() )
		return;
	color32 c = { (unsigned char)clamp( r, 0, 255 ), (unsigned char)clamp( g, 0, 255 ), (unsigned char)clamp( b, 0, 255 ), 255 };
	AddSplat( pos, size, pic, c, normal, surf );
	const Decal_t &d = s_Decals.back();
	Msg( "p2paint: test splat on surface %d (%s), %d triangles of our own\n", d.surf, d.engine ? "engine decal too" : "no engine decal",
		 (int)d.mesh.verts.size() / 3 );
}

CON_COMMAND_F( p2paint_erase, "p2paint_erase x y z [tolerance]: remove the splats centred within tolerance (default 0.5) of x y z", FCVAR_SERVER_CAN_EXECUTE )
{
	if ( args.ArgC() < 4 || !P2UI_ServerAware() || !s_Layer.EnsureReady() )
		return;
	Vector at( atof( args[1] ), atof( args[2] ), atof( args[3] ) );
	float tol = ArgF( args, 4, 0.5f );
	for ( int i = (int)s_Decals.size() - 1; i >= 0; i-- )
		if ( s_Decals[i].pos.DistToSqr( at ) <= tol * tol )
			RemoveSplat( i );
}

CON_COMMAND_F( p2paint_recolor, "p2paint_recolor x y z tolerance r g b [picture]: new colour / picture for the splats centred there", FCVAR_SERVER_CAN_EXECUTE )
{
	if ( args.ArgC() < 8 || !P2UI_ServerAware() || !s_Layer.EnsureReady() )
		return;
	Vector at( atof( args[1] ), atof( args[2] ), atof( args[3] ) );
	float tol = atof( args[4] );
	const char *pic = args.ArgC() > 8 ? args[8] : NULL;
	for ( Decal_t &d : s_Decals )
		if ( d.pos.DistToSqr( at ) <= tol * tol )
			Paint( d, pic, Colour( args, 5, false ) );
}

CON_COMMAND_F( p2paint_clear, "p2paint: remove all paint", FCVAR_SERVER_CAN_EXECUTE )
{
	if ( !P2UI_ServerAware() || !s_Layer.EnsureReady() )
		return;
	for ( const Decal_t &d : s_Decals )
		if ( d.engine )
			Unshoot( d.tag );
	s_Decals.clear();
}

CON_COMMAND( p2paint_info, "p2paint: show runtime state" )
{
	bool ok = ConnectInterfaces();
	Msg( "p2paint %s: interfaces %s (engine %p, effects %p, modelinfo %p, leafsystem %p, materialsystem %p)\n", p2paint_version.GetString(),
		 ok ? "OK" : "MISSING", (void *)s_pEngine, (void *)s_pEffects, (void *)s_pModelInfo, (void *)s_pLeafSystem, (void *)s_pMatSys );
	if ( s_pEngine )
		Msg( "p2paint: in game %d, level \"%s\", layer level \"%s\", handle %d, world model %p\n", s_pEngine->IsInGame(),
			 s_pEngine->GetLevelName(), s_Layer.Level(), (int)s_Layer.RenderHandle(), (const void *)s_pWorld );
	int own = 0, ownTris = 0, engine = 0;
	for ( const Decal_t &d : s_Decals )
	{
		engine += d.engine;
		own += !d.mesh.verts.empty();
		ownTris += (int)d.mesh.verts.size() / 3;
	}
	Msg( "p2paint: %d paint splats: %d engine decals (r_printdecalinfo: the engine's count), %d of ours (%d triangles); %d pictures\n",
		 (int)s_Decals.size(), engine, own, ownTris, (int)P2Pics_List().size() );
	P2Surf_Info();
}

// every frame (the HUD's tick): notice leaving the game. Setting up again is our plugin's hello's job, so maps on
// servers without it cost nothing (no map file read, no draw hook).
void P2Paint_WatchLevel()
{
	if ( ConnectInterfaces() && !s_pEngine->IsInGame() )
		s_bLeftGame = true;
}

bool P2Paint_EnsureReady()
{
	return s_Layer.EnsureReady();
}

// the engine without the paint layer / UI (connected at load): +paint / +erase on servers without our plugin
IVEngineClient *P2Paint_Engine()
{
	ConnectInterfaces();
	return s_pEngine;
}

// the menu / HUD panels without being in game (p2paint_selftest starts from the main menu)
bool P2Paint_EnsureUI()
{
	static bool s_bUI;
	if ( !s_bUI && ConnectInterfaces() )
		s_bUI = P2UI_Init( GameInterface, s_pEngine, s_pMatSys );
	return s_bUI;
}

// Our materials and menu font (game/, embedded) into the game folder, rewritten only when they differ: no install
// step, and cstrike/custom/ folders are only mounted at startup.
static void WriteGameFiles()
{
	std::string dir = s_pEngine->GetGameDirectory();
	for ( const P2GameFile_t &f : s_GameFiles )
	{
		std::string path = dir + "/" + f.path;
		if ( P2Plat_FileSize( path ) == (long long)f.size )
		{
			CUtlVector<unsigned char> old;
			old.SetCount( f.size );
			FILE *in = fopen( path.c_str(), "rb" );
			bool same = in && fread( old.Base(), 1, f.size, in ) == f.size && !memcmp( old.Base(), f.data, f.size );
			if ( in )
				fclose( in );
			if ( same )
				continue;
		}
		P2Plat_MakeDirs( path.substr( 0, path.rfind( '/' ) ) );
		std::string tmp = path + ".tmp";
		FILE *out = fopen( tmp.c_str(), "wb" );
		bool ok = out && fwrite( f.data, 1, f.size, out ) == f.size;
		if ( out )
			ok = fclose( out ) == 0 && ok;
		if ( !ok || !P2Plat_Replace( tmp, path ) )
		{
			remove( tmp.c_str() );
			Warning( "p2paint: can't write %s\n", path.c_str() );
		}
	}
}

#ifndef _WIN32
// shaderlib's CShaderDLL is how the game takes us in on Linux: the ShaderDLL004 interface the material system loads
// us by, and its Connect registers our console commands (ConVar_Register). No shader references it any more, so
// this keeps it (and tier1's CreateInterface) in the build.
class IShaderDLL;
IShaderDLL *GetShaderDLL();
P2_EXPORT void *P2Paint_ShaderDLL() { return GetShaderDLL(); }
#endif

static std::string s_EngineLog;

// the engine code patches (p2paint_enginepatch.cpp), once: Linux = from P2Paint_Init, Windows = as soon as
// engine.dll is in (p2paint_platform_win.cpp), both before the first map sizes the decal pool
void P2Paint_PatchEngine()
{
	if ( s_EngineLog.empty() )
		s_EngineLog = P2Patch_Engine();
}

// Linux: called by the loader (libp2paint.so) right after the shader system loaded us. Windows: P2Paint_MainInit.
// Both on the main thread, before the first map.
P2_EXPORT int P2Paint_Init()
{
	P2Paint_PatchEngine();
	if ( !ConnectInterfaces() )
		return 0;
	static bool s_bFiles;
	if ( !s_bFiles )
	{
		s_bFiles = true;
		WriteGameFiles();
	}
	return 1;
}

// what P2Paint_Init did to the engine code, for the loader's log (the console doesn't exist yet at that point)
P2_EXPORT const char *P2Paint_EngineLog()
{
	return s_EngineLog.c_str();
}

#ifdef _WIN32
// Windows: on the game's main thread once its window is up (p2paint_platform_win.cpp): our console commands
// (tier1, through vstdlib's ICvar; Linux gets them from shaderlib), then the rest like Linux
void P2Paint_MainInit()
{
	CreateInterfaceFn factory = (CreateInterfaceFn)P2Plat_Factory( "vstdlib" );
	if ( factory )
	{
		ConnectTier1Libraries( &factory, 1 );
		ConVar_Register( 0 );
	}
	if ( !P2Paint_Init() )
	{
		Warning( "p2paint: game interfaces missing\n" );
		return;
	}
	Msg( "p2paint: ready, engine code: %s", s_EngineLog.c_str() );
	// tests: a +p2paint_selftest on the command line runs before an injected DLL is in, so it comes this way
	if ( const char *script = getenv( "P2PAINT_SELFTEST" ) )
	{
		char cmd[1024];
		V_snprintf( cmd, sizeof( cmd ), "p2paint_selftest \"%s\"\n", script );
		s_pEngine->ClientCmd_Unrestricted( cmd );
	}
}
#endif
