//=============================================================================
// p2paint: paint on what the engine can't put a decal on. The engine paints world faces (PlayerDecalShoot);
// on displacements, props, brush entities, clips, triggers and invisible walls we cut the decal out of the
// map's own triangles (p2paint_bsp / p2paint_props) and draw it ourselves, with the same material.
//=============================================================================
#ifndef P2PAINT_SURFACES_H
#define P2PAINT_SURFACES_H

#include "mathlib/vector.h"

#include <vector>

#include "p2paint_bsp.h"

class IMaterial;
class IMatRenderContext;

// what a splat landed on: the plugin's PAINTSURF_* numbers (paint_3.sp), as it sends and saves them
enum P2Surf_t
{
	P2SURF_UNKNOWN = -1,	// saved before the plugin sent it (5.0 and older): we look it up
	P2SURF_WORLD = 0,
	P2SURF_DISP = 1,
	P2SURF_ENTITY = 2,		// brush entities that stay put
	P2SURF_PROP = 3,
	P2SURF_CLIP = 4,
	P2SURF_MOVING = 5,		// brush entities that keep moving: no paint
	P2SURF_TRIGGER = 6,
	P2SURF_INVISIBLE = 7,	// (ours only, from the lookup) skybox / nodraw walls
	P2SURF_KINDS
};

// does the engine's decal belong to a splat on this? (world faces next to it get their part)
inline bool P2Surf_EngineToo( int surf ) { return surf == P2SURF_WORLD || surf == P2SURF_DISP || surf == P2SURF_INVISIBLE; }

struct P2DecalVert_t { Vector pos; float u, v; };
struct P2DecalMesh_t
{
	std::vector<P2DecalVert_t> verts;	// triangles, clockwise seen from the front
	std::vector<int> clusters;			// vis clusters it's in
	void Clear() { verts.clear(); clusters.clear(); }
};

// the axes a decal gets on a surface: upright on walls, along the world's X on floors and ceilings
void P2Surf_DecalAxes( const Vector &n, Vector &s, Vector &t );

// the map's surfaces (once per map; returns false if they can't be read: then only the engine paints)
bool P2Surf_Load( const char *pszGameDir, const char *pszLevel, P2ReadFileFn pfnRead );
void P2Surf_Info();

// a splat saved without its surface: what it sits on (P2SURF_WORLD when it's a world face or nothing we know)
int P2Surf_Find( const Vector &pos, Vector *pNormal );
// the decal (centre, normal, half sizes along P2Surf_DecalAxes) on the surfaces of its kind / layer
void P2Surf_Build( const Vector &c, const Vector &n, float halfS, float halfT, int surf, P2DecalMesh_t &out );

// what the plugin's aim stops at besides what the engine's trace hits: brush entity faces (func_illusionary
// and other non-solid ones) and the faces of triggers of the classes the player sees (comma-separated, from
// the plugin). Nearest within maxDist, front sides only.
bool P2Surf_AimOwn( const Vector &start, const Vector &dir, float maxDist, const char *pszTriggerClasses, Vector *pHit, Vector *pNormal );

// drawing: once per frame the camera, then runs of decals that share a material
void P2Surf_BeginFrame( const Vector &camera );
bool P2Surf_Visible( const P2DecalMesh_t &m );
void P2Surf_DrawRun( IMatRenderContext *ctx, IMaterial *pMaterial, const P2DecalMesh_t *const *ppMeshes, const unsigned char *pColours, int count );

#endif // P2PAINT_SURFACES_H
