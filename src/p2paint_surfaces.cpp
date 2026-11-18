//=============================================================================
// p2paint: paint decals on the surfaces the engine can't decal (see p2paint_surfaces.h).
//
// A decal is a square on the splat's plane (P2Surf_DecalAxes, like the engine's), cut out of every triangle
// of its layer that faces the same way and lies within its reach in front of / behind it: texture
// coordinates come from the square, so it looks like the engine's decal and wraps the same way. The full
// reach (half the decal's size, as the engine's) only on the kind of surface the splat hit; other kinds get
// NEAR_DEPTH, so a floor splat beside a crate doesn't climb onto it.
//=============================================================================
#include <math.h>
#include <algorithm>

#include "p2paint_surfaces.h"
#include "p2paint_props.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imesh.h"
#include "tier0/platform.h"
#include "tier1/strtools.h"

#include "tier0/memdbgon.h"

static CP2Bsp s_Bsp;
static CP2Props s_Props;
static bool s_bLoaded;
static std::vector<int> s_OwnFaces;		// the faces that aren't world faces (the engine paints those)

namespace
{
	const float MIN_FACING = 0.2f;			// cos of the steepest surface a decal still goes on
	const float LIFT = 0.1f;				// off the surface (the decal material's depth bias does the rest)
	const float NEAR_DEPTH = 8.0f;			// reach onto kinds of surface the splat didn't hit
	const float FIND_TOLERANCE = 0.25f;		// a saved splat is this close to what it was painted on (traces stop 1/32 short)

	int FaceSurf( int kind )
	{
		switch ( kind )
		{
		case P2FACE_DISP:		return P2SURF_DISP;
		case P2FACE_ENTITY:		return P2SURF_ENTITY;
		case P2FACE_TRIGGER:	return P2SURF_TRIGGER;
		case P2FACE_CLIP:		return P2SURF_CLIP;
		case P2FACE_INVISIBLE:	return P2SURF_INVISIBLE;
		default:				return P2SURF_WORLD;
		}
	}
	int SurfLayer( int surf ) { return surf == P2SURF_TRIGGER ? P2LAYER_TRIGGER : surf == P2SURF_CLIP ? P2LAYER_CLIP : P2LAYER_SOLID; }

	// keeps the part of a convex polygon where dot( p, n ) <= d
	void Chop( std::vector<Vector> &poly, const Vector &n, float d )
	{
		std::vector<Vector> out;
		out.reserve( poly.size() + 2 );
		for ( size_t i = 0; i < poly.size(); i++ )
		{
			const Vector &a = poly[i], &b = poly[( i + 1 ) % poly.size()];
			float da = DotProduct( a, n ) - d, db = DotProduct( b, n ) - d;
			if ( da <= 0 )
				out.push_back( a );
			if ( ( da < 0 && db > 0 ) || ( da > 0 && db < 0 ) )
				out.push_back( a + ( b - a ) * ( da / ( da - db ) ) );
		}
		poly.swap( out );
	}

	Vector ClosestOnTriangle( const Vector &p, const Vector &a, const Vector &b, const Vector &c )
	{
		Vector ab = b - a, ac = c - a, ap = p - a;
		float d1 = DotProduct( ab, ap ), d2 = DotProduct( ac, ap );
		if ( d1 <= 0 && d2 <= 0 ) return a;
		Vector bp = p - b;
		float d3 = DotProduct( ab, bp ), d4 = DotProduct( ac, bp );
		if ( d3 >= 0 && d4 <= d3 ) return b;
		float vc = d1 * d4 - d3 * d2;
		if ( vc <= 0 && d1 >= 0 && d3 <= 0 ) return a + ab * ( d1 / ( d1 - d3 ) );
		Vector cp = p - c;
		float d5 = DotProduct( ab, cp ), d6 = DotProduct( ac, cp );
		if ( d6 >= 0 && d5 <= d6 ) return c;
		float vb = d5 * d2 - d1 * d6;
		if ( vb <= 0 && d2 >= 0 && d6 <= 0 ) return a + ac * ( d2 / ( d2 - d6 ) );
		float va = d3 * d6 - d5 * d4;
		if ( va <= 0 && ( d4 - d3 ) >= 0 && ( d5 - d6 ) >= 0 ) return b + ( c - b ) * ( ( d4 - d3 ) / ( ( d4 - d3 ) + ( d5 - d6 ) ) );
		float den = 1.0f / ( va + vb + vc );
		return a + ab * ( vb * den ) + ac * ( vc * den );
	}

	// a triangle's normal, turned to the side 'front' says is the outside; false if it has no area
	bool TriNormal( const Vector &a, const Vector &b, const Vector &c, const Vector &front, Vector *pN )
	{
		*pN = CrossProduct( b - a, c - a );
		if ( VectorNormalize( *pN ) < 1e-6f )
			return false;
		if ( DotProduct( *pN, front ) < 0 )
			*pN = -*pN;
		return true;
	}

	bool BoxesTouch( const Vector &amin, const Vector &amax, const Vector &bmin, const Vector &bmax )
	{
		return amin.x <= bmax.x && amax.x >= bmin.x && amin.y <= bmax.y && amax.y >= bmin.y && amin.z <= bmax.z && amax.z >= bmin.z;
	}
}

void P2Surf_DecalAxes( const Vector &n, Vector &s, Vector &t )
{
	if ( fabsf( n.z ) > 0.7071f )
	{
		s.Init( 1, 0, 0 );
		t = CrossProduct( s, n );
		s = CrossProduct( n, t );
	}
	else
	{
		t.Init( 0, 0, -1 );
		s = CrossProduct( n, t );
		t = CrossProduct( s, n );
	}
	s.NormalizeInPlace();
	t.NormalizeInPlace();
}

static int s_nVisCluster = -2;
static bool s_bVisAll = true;
static std::vector<unsigned char> s_VisBits;

bool P2Surf_Load( const char *pszGameDir, const char *pszLevel, P2ReadFileFn pfnRead )
{
	if ( s_bLoaded && !V_strcmp( pszLevel, s_Bsp.Level() ) )
		return true;
	double t0 = Plat_FloatTime();
	s_bLoaded = false;
	s_OwnFaces.clear();
	s_Props.Clear();
	s_nVisCluster = -2;
	if ( !s_Bsp.Load( pszGameDir, pszLevel, pfnRead ) )
	{
		Warning( "p2paint: no paint on props, clips, displacements etc. on this map (its surfaces can't be read)\n" );
		return false;
	}
	s_Props.Load( s_Bsp, pfnRead );
	for ( int i = 0; i < s_Bsp.m_Faces.Count(); i++ )
		if ( s_Bsp.m_Faces[i].kind != P2FACE_WORLD )
			s_OwnFaces.push_back( i );
	s_bLoaded = true;
	Msg( "p2paint: surfaces of %s read in %.0f ms\n", pszLevel, ( Plat_FloatTime() - t0 ) * 1000.0 );
	return true;
}

CP2Bsp *P2Surf_Bsp() { return &s_Bsp; }	// (tools/surftest)

void P2Surf_Info()
{
	if ( !s_bLoaded )
	{
		Msg( "p2paint: no map surfaces loaded (only the engine paints)\n" );
		return;
	}
	Msg( "p2paint: %s: %d displacements, %d brush entity / %d trigger / %d clip / %d invisible wall faces, %d props (%d models)\n",
		 s_Bsp.Level(), s_Bsp.m_nCount[P2FACE_DISP], s_Bsp.m_nCount[P2FACE_ENTITY], s_Bsp.m_nCount[P2FACE_TRIGGER],
		 s_Bsp.m_nCount[P2FACE_CLIP], s_Bsp.m_nCount[P2FACE_INVISIBLE], s_Props.Count(), s_Props.Models() );
}

int P2Surf_Find( const Vector &pos, Vector *pNormal )
{
	if ( !s_bLoaded )
		return P2SURF_WORLD;
	int best = P2SURF_WORLD;
	float bestD = FIND_TOLERANCE;
	const Vector tol( FIND_TOLERANCE, FIND_TOLERANCE, FIND_TOLERANCE );
	const Vector bmin = pos - tol, bmax = pos + tol;
	// world faces too: the nearest surface it's in front of is the one it was painted on. Not clips / triggers:
	// the aim went through those until the plugin started saving where paint lands.
	for ( int i = 0; i < s_Bsp.m_Faces.Count(); i++ )
	{
		const P2Face_t &f = s_Bsp.m_Faces[i];
		if ( P2FaceLayer( f.kind ) != P2LAYER_SOLID || !BoxesTouch( bmin, bmax, f.mins, f.maxs ) )
			continue;
		if ( !f.dispPower )
		{
			float d = DotProduct( f.normal, pos ) - f.dist;
			if ( d < -0.05f || fabsf( d ) >= bestD )
				continue;
			int in = 0, out = 0;
			for ( int k = 0; k < f.numVerts; k++ )
			{
				const Vector &a = s_Bsp.m_Verts[f.firstVert + k], &b = s_Bsp.m_Verts[f.firstVert + ( k + 1 ) % f.numVerts];
				float s = DotProduct( CrossProduct( b - a, pos - a ), f.normal );
				if ( s > 0.01f ) in++; else if ( s < -0.01f ) out++;
			}
			if ( in && out )
				continue;
			best = FaceSurf( f.kind );
			bestD = fabsf( d );
			*pNormal = f.normal;
			continue;
		}
		int n = s_Bsp.DispSide( f ) - 1;
		for ( int r = 0; r < n; r++ )
			for ( int c = 0; c < n; c++ )
				for ( int which = 0; which < 2; which++ )
				{
					Vector a, b, cc, tn;
					s_Bsp.DispTriangle( f, r, c, which, &a, &b, &cc );
					if ( !TriNormal( a, b, cc, f.normal, &tn ) || DotProduct( pos - a, tn ) < -0.05f )
						continue;
					float d = ClosestOnTriangle( pos, a, b, cc ).DistTo( pos );
					if ( d >= bestD )
						continue;
					best = P2SURF_DISP;
					bestD = d;
					*pNormal = tn;
				}
	}
	std::vector<int> props;
	s_Props.PropsInBox( bmin, bmax, props );
	for ( size_t i = 0; i < props.size(); i++ )
		for ( int t = 0; t < s_Props.Triangles( props[i] ); t++ )
		{
			Vector a, b, c, smooth, tn;
			s_Props.Triangle( props[i], t, &a, &b, &c, &smooth );
			if ( !TriNormal( a, b, c, smooth, &tn ) || DotProduct( pos - a, tn ) < -0.05f )
				continue;
			float d = ClosestOnTriangle( pos, a, b, c ).DistTo( pos );
			if ( d >= bestD )
				continue;
			best = P2SURF_PROP;
			bestD = d;
			*pNormal = tn;
		}
	return best;
}

void P2Surf_Build( const Vector &c, const Vector &n, float halfS, float halfT, int surf, P2DecalMesh_t &out )
{
	out.Clear();
	if ( !s_bLoaded || surf == P2SURF_MOVING || n.LengthSqr() < 0.5f )
		return;
	Vector S, T;
	P2Surf_DecalAxes( n, S, T );
	const int layer = SurfLayer( surf );
	const float reach = MAX( halfS, halfT );
	Vector ext;
	for ( int k = 0; k < 3; k++ )
		ext[k] = fabsf( S[k] ) * halfS + fabsf( T[k] ) * halfT + fabsf( n[k] ) * reach;
	const Vector bmin = c - ext, bmax = c + ext;
	const float cs = DotProduct( c, S ), ct = DotProduct( c, T ), cn = DotProduct( c, n );
	Vector mmin( FLT_MAX, FLT_MAX, FLT_MAX ), mmax( -FLT_MAX, -FLT_MAX, -FLT_MAX );

	std::vector<Vector> poly;
	// the part of a convex polygon (facing 'front') inside the decal, as triangles
	auto Emit = [&]( const Vector &front, float depth )
	{
		Chop( poly, S, cs + halfS );
		Chop( poly, -S, -cs + halfS );
		Chop( poly, T, ct + halfT );
		Chop( poly, -T, -ct + halfT );
		Chop( poly, n, cn + depth );
		Chop( poly, -n, -cn + depth );
		if ( poly.size() < 3 )
			return;
		// clockwise seen from the front (the BSP's winding, what the game culls by); relative to a corner,
		// map coordinates are too big for float otherwise
		Vector newell( 0, 0, 0 );
		for ( size_t k = 1; k + 1 < poly.size(); k++ )
			newell += CrossProduct( poly[k] - poly[0], poly[k + 1] - poly[0] );
		if ( DotProduct( newell, front ) > 0 )
			std::reverse( poly.begin(), poly.end() );
		for ( size_t k = 1; k + 1 < poly.size(); k++ )
		{
			const Vector *tri[3] = { &poly[0], &poly[k], &poly[k + 1] };
			for ( int v = 0; v < 3; v++ )
			{
				P2DecalVert_t dv;
				dv.pos = *tri[v] + front * LIFT;
				dv.u = 0.5f + ( DotProduct( *tri[v], S ) - cs ) / ( 2.0f * halfS );
				dv.v = 0.5f + ( DotProduct( *tri[v], T ) - ct ) / ( 2.0f * halfT );
				out.verts.push_back( dv );
				VectorMin( mmin, dv.pos, mmin );
				VectorMax( mmax, dv.pos, mmax );
			}
		}
	};
	auto Depth = [&]( int kind ) { return kind == surf ? reach : MIN( reach, NEAR_DEPTH ); };
	auto TriInBox = [&]( const Vector &a, const Vector &b, const Vector &cc )
	{
		Vector tmin = a, tmax = a;
		VectorMin( tmin, b, tmin ); VectorMin( tmin, cc, tmin );
		VectorMax( tmax, b, tmax ); VectorMax( tmax, cc, tmax );
		return BoxesTouch( tmin, tmax, bmin, bmax );
	};

	for ( size_t i = 0; i < s_OwnFaces.size(); i++ )
	{
		const P2Face_t &f = s_Bsp.m_Faces[s_OwnFaces[i]];
		const int fs = FaceSurf( f.kind );
		if ( SurfLayer( fs ) != layer || !BoxesTouch( bmin, bmax, f.mins, f.maxs ) )
			continue;
		if ( !f.dispPower )
		{
			if ( DotProduct( f.normal, n ) < MIN_FACING )
				continue;
			poly.assign( &s_Bsp.m_Verts[f.firstVert], &s_Bsp.m_Verts[f.firstVert] + f.numVerts );
			Emit( f.normal, Depth( fs ) );
			continue;
		}
		int side = s_Bsp.DispSide( f ) - 1;
		for ( int r = 0; r < side; r++ )
			for ( int col = 0; col < side; col++ )
				for ( int which = 0; which < 2; which++ )
				{
					Vector a, b, cc, tn;
					s_Bsp.DispTriangle( f, r, col, which, &a, &b, &cc );
					if ( !TriInBox( a, b, cc ) || !TriNormal( a, b, cc, f.normal, &tn ) || DotProduct( tn, n ) < MIN_FACING )
						continue;
					poly.assign( { a, b, cc } );
					Emit( tn, Depth( fs ) );
				}
	}
	if ( layer == P2LAYER_SOLID )
	{
		std::vector<int> props;
		s_Props.PropsInBox( bmin, bmax, props );
		for ( size_t i = 0; i < props.size(); i++ )
			for ( int t = 0; t < s_Props.Triangles( props[i] ); t++ )
			{
				Vector a, b, cc, smooth, tn;
				s_Props.Triangle( props[i], t, &a, &b, &cc, &smooth );
				if ( !TriInBox( a, b, cc ) || !TriNormal( a, b, cc, smooth, &tn ) || DotProduct( tn, n ) < MIN_FACING )
					continue;
				poly.assign( { a, b, cc } );
				Emit( tn, Depth( P2SURF_PROP ) );
			}
	}
	if ( !out.verts.empty() )
		s_Bsp.BoxClusters( mmin - Vector( 1, 1, 1 ), mmax + Vector( 1, 1, 1 ), out.clusters );
}

bool P2Surf_AimOwn( const Vector &start, const Vector &dir, float maxDist, const char *pszTriggerClasses, Vector *pHit, Vector *pNormal )
{
	if ( !s_bLoaded )
		return false;
	// which classes' triggers count (recomputed when the list changes)
	static std::string s_List = "?";
	static std::string s_Level;
	static std::vector<char> s_TriggerOn;
	if ( s_List != pszTriggerClasses || s_Level != s_Bsp.Level() )
	{
		s_List = pszTriggerClasses;
		s_Level = s_Bsp.Level();
		s_TriggerOn.assign( s_Bsp.m_EntClasses.size(), 0 );
		std::string list = "," + s_List + ",";
		for ( size_t k = 0; k < s_Bsp.m_EntClasses.size(); k++ )
		{
			std::string key = "," + s_Bsp.m_EntClasses[k] + ",";
			s_TriggerOn[k] = V_stristr( list.c_str(), key.c_str() ) != NULL;
		}
	}
	float best = maxDist;
	bool hit = false;
	for ( size_t i = 0; i < s_OwnFaces.size(); i++ )
	{
		const P2Face_t &f = s_Bsp.m_Faces[s_OwnFaces[i]];
		if ( f.dispPower || f.entClass < 0 )
			continue;
		if ( f.kind == P2FACE_TRIGGER ? !s_TriggerOn[f.entClass] : f.kind != P2FACE_ENTITY )
			continue;
		float denom = DotProduct( f.normal, dir );
		if ( denom > -1e-6f )
			continue;	// back side or parallel
		float t = ( f.dist - DotProduct( f.normal, start ) ) / denom;
		if ( t < 0 || t >= best )
			continue;
		Vector p = start + dir * t;
		int in = 0, out = 0;
		for ( int k = 0; k < f.numVerts; k++ )
		{
			const Vector &a = s_Bsp.m_Verts[f.firstVert + k], &b = s_Bsp.m_Verts[f.firstVert + ( k + 1 ) % f.numVerts];
			float s = DotProduct( CrossProduct( b - a, p - a ), f.normal );
			if ( s > 0.01f ) in++; else if ( s < -0.01f ) out++;
		}
		if ( in && out )
			continue;
		best = t;
		hit = true;
		*pHit = p;
		*pNormal = f.normal;
	}
	return hit;
}

void P2Surf_BeginFrame( const Vector &camera )
{
	if ( !s_bLoaded )
	{
		s_bVisAll = true;
		return;
	}
	int cluster = s_Bsp.ClusterAt( camera );
	if ( cluster == s_nVisCluster )
		return;
	s_nVisCluster = cluster;
	s_bVisAll = !s_Bsp.ClusterVisibility( cluster, s_VisBits );	// in solid / no vis: everything
}

bool P2Surf_Visible( const P2DecalMesh_t &m )
{
	if ( m.verts.empty() )
		return false;
	if ( s_bVisAll || m.clusters.empty() )
		return true;
	for ( size_t i = 0; i < m.clusters.size(); i++ )
	{
		int c = m.clusters[i];
		if ( ( c >> 3 ) < (int)s_VisBits.size() && ( s_VisBits[c >> 3] & ( 1 << ( c & 7 ) ) ) )
			return true;
	}
	return false;
}

void P2Surf_DrawRun( IMatRenderContext *ctx, IMaterial *pMaterial, const P2DecalMesh_t *const *ppMeshes, const unsigned char *pColours, int count )
{
	if ( !pMaterial || count <= 0 )
		return;
	int total = 0;
	for ( int i = 0; i < count; i++ )
		total += (int)ppMeshes[i]->verts.size();
	if ( !total )
		return;
	ctx->Bind( pMaterial );
	int mesh = 0, vert = 0;
	while ( total > 0 )
	{
		IMesh *pMesh = ctx->GetDynamicMesh( true );
		int maxVerts = 0, maxIndices = 0;
		ctx->GetMaxToRender( pMesh, false, &maxVerts, &maxIndices );
		int batch = MIN( total, MIN( MAX( maxVerts, 3 ), 8190 ) );
		batch -= batch % 3;
		CMeshBuilder mb;
		mb.Begin( pMesh, MATERIAL_TRIANGLES, batch / 3 );
		for ( int k = 0; k < batch; k++ )
		{
			while ( vert >= (int)ppMeshes[mesh]->verts.size() )
				mesh++, vert = 0;
			const P2DecalVert_t &v = ppMeshes[mesh]->verts[vert++];
			mb.Position3fv( v.pos.Base() );
			mb.Color4ubv( pColours + mesh * 4 );
			mb.TexCoord2f( 0, v.u, v.v );
			mb.AdvanceVertex();
		}
		mb.End();
		pMesh->Draw();
		total -= batch;
	}
}
