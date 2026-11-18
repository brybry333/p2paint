//=============================================================================
// p2paint: the surfaces of the current map the engine can't put a decal on, read from its .bsp:
// displacements, the faces of brush entities that stay put (func_brush, func_illusionary, triggers, ...),
// the sides of clip brushes and of invisible / skybox walls, and where the props are. World faces are kept
// too (the engine paints those; clip sides lying on one are left out). Also the map's PVS.
// (From the archived gel version, branch archive/gel, minus its paint maps.)
//=============================================================================
#ifndef P2PAINT_BSP_H
#define P2PAINT_BSP_H

#include "mathlib/vector.h"
#include "tier1/utlvector.h"

#include <string>
#include <vector>

enum P2FaceKind_t
{
	P2FACE_WORLD,
	P2FACE_DISP,			// displacement
	P2FACE_ENTITY,			// visible brush entity
	P2FACE_TRIGGER,			// trigger brush (tools/toolstrigger)
	P2FACE_CLIP,			// side of a clip brush (players / NPCs only)
	P2FACE_INVISIBLE,		// solid but not drawn: skybox walls, exposed nodraw / invisible brush sides
	P2FACE_KINDS
};

// paint layers: a splat only lands on surfaces of its layer, because triggers and clips float right next to
// walls and floors (0 = what's solid, props included)
enum { P2LAYER_SOLID, P2LAYER_TRIGGER, P2LAYER_CLIP };
inline int P2FaceLayer( int kind ) { return kind == P2FACE_TRIGGER ? P2LAYER_TRIGGER : kind == P2FACE_CLIP ? P2LAYER_CLIP : P2LAYER_SOLID; }

struct P2Face_t
{
	Vector	normal;			// facing side of the surface (displacements: of the base face)
	float	dist;			// plane: dot( normal, p ) == dist
	Vector	mins, maxs;		// world AABB
	int		firstVert;		// into CP2Bsp::m_Verts
	int		numVerts;		// convex polygon, clockwise seen from the front; displacements: the
							// (2^power+1)^2 grid, row-major
	int		dispPower;		// 0 = flat polygon
	int		kind;			// P2FaceKind_t
	int		entClass;		// brush entity faces: their classname (CP2Bsp::m_EntClasses), else -1
	int		firstCluster;	// into CP2Bsp::m_FaceClusters: the vis clusters the surface is in
	int		numClusters;
};

struct P2StaticProp_t
{
	char	model[128];		// "models/....mdl"
	Vector	origin;
	QAngle	angles;
};

typedef bool ( *P2ReadFileFn )( const char *path, CUtlVector<unsigned char> &out );

class CP2Bsp
{
public:
	CP2Bsp() : m_nClusters( 0 ), m_nHeadNode( 0 ) { m_szLevel[0] = 0; memset( m_nCount, 0, sizeof( m_nCount ) ); }

	// Loads the surfaces of a map. pszGameDir = engine->GetGameDirectory(), pszLevel = engine->GetLevelName()
	// ("maps/foo.bsp"). Returns false and keeps nothing on error. pfnRead (may be NULL) reads game files
	// (VPKs, the map's pak), for maps that aren't loose files.
	bool Load( const char *pszGameDir, const char *pszLevel, P2ReadFileFn pfnRead = NULL );
	void Clear();

	int DispSide( const P2Face_t &f ) const { return ( 1 << f.dispPower ) + 1; }
	// the triangles of a displacement, 2 per grid cell (i, j < side - 1), in the engine's diagonal pattern
	void DispTriangle( const P2Face_t &f, int i, int j, int which, Vector *a, Vector *b, Vector *c ) const;

	const char *Level() const { return m_szLevel; }

	// visibility (the map's PVS, like the engine uses to skip what can't be seen from where the camera is)
	int ClusterAt( const Vector &p ) const;			// -1 outside the map / in solid
	int LeafAt( const Vector &p ) const;
	// clusters visible from 'cluster' as a bitset (resized to the cluster count); false = draw everything
	bool ClusterVisibility( int cluster, std::vector<unsigned char> &bits ) const;
	void BoxClusters( const Vector &mins, const Vector &maxs, std::vector<int> &out ) const;

	CUtlVector<P2Face_t>	m_Faces;
	CUtlVector<Vector>		m_Verts;
	CUtlVector<P2StaticProp_t>	m_StaticProps;	// static props + prop entities that don't move
	CUtlVector<int>			m_FaceClusters;
	std::vector<std::string> m_EntClasses;	// classnames of the brush entities with faces

	struct Node_t { Vector normal; float dist; int children[2]; };
	struct Leaf_t { int contents; int cluster; };
	CUtlVector<Node_t>		m_Nodes;
	CUtlVector<Leaf_t>		m_Leafs;
	CUtlVector<unsigned char> m_Vis;		// LUMP_VISIBILITY as stored
	int						m_nClusters;
	int						m_nHeadNode;
	int						m_nCount[P2FACE_KINDS];

private:
	char m_szLevel[260];
};

#endif // P2PAINT_BSP_H
