//=============================================================================
// p2paint: reads the surfaces of the current map from its .bsp (version 20, CS:S); see p2paint_bsp.h
//=============================================================================
#include <dirent.h>
#include <stdio.h>

#include "p2paint_bsp.h"
#include "tier1/strtools.h"
#include "tier1/lzmaDecoder.h"
#include "mathlib/mathlib.h"

#include "tier0/memdbgon.h"

namespace
{
	enum
	{
		LUMP_ENTITIES = 0, LUMP_PLANES = 1, LUMP_TEXDATA = 2, LUMP_VERTEXES = 3, LUMP_VISIBILITY = 4, LUMP_NODES = 5, LUMP_TEXINFO = 6,
		LUMP_FACES = 7, LUMP_LEAFS = 10, LUMP_EDGES = 12, LUMP_SURFEDGES = 13, LUMP_MODELS = 14, LUMP_LEAFBRUSHES = 17,
		LUMP_BRUSHES = 18, LUMP_BRUSHSIDES = 19, LUMP_DISPINFO = 26, LUMP_DISP_VERTS = 33, LUMP_GAME_LUMP = 35,
		LUMP_TEXDATA_STRING_DATA = 43, LUMP_TEXDATA_STRING_TABLE = 44, LUMP_FACES_HDR = 58, NUM_LUMPS = 64,
	};

	// surface flags (bspflags.h) that can never take paint (trigger faces are told apart by their texture)
	const int SURF_NOPAINT = 0x0008 /*WARP*/ | 0x0080 /*NODRAW*/ | 0x0100 /*HINT*/ | 0x0200 /*SKIP*/;
	const int SURF_SKYFACE = 0x0002 /*SKY2D*/ | 0x0004 /*SKY*/;	// skybox walls: painted, they're walls in bhop maps
	const int CONTENTS_SOLID = 0x1, CONTENTS_PLAYERCLIP = 0x10000, CONTENTS_MONSTERCLIP = 0x20000;

	// brush entities that aren't surfaces (volumes, vis helpers) or don't stay where they spawned
	const char *const s_SkipClasses[] =
	{
		"func_areaportal", "func_areaportalwindow", "func_occluder", "func_smokevolume", "func_dustmotes",
		"func_dustcloud", "func_precipitation", "func_viscluster", "func_rotating", "func_tracktrain", "func_train",
		"func_physbox", "func_physbox_multiplayer", "momentary_rot_button", "func_movelinear", "func_water_analog",
		"func_conveyor", "func_platrot", "func_ladderendpoint",
	};

	#pragma pack(push, 1)
	struct lump_t { int fileofs, filelen, version; int uncompressedSize; };
	struct dheader_t { int ident, version; lump_t lumps[NUM_LUMPS]; int mapRevision; };
	struct dplane_t { Vector normal; float dist; int type; };
	struct dtexdata_t { Vector reflectivity; int nameStringTableID; int width, height, view_width, view_height; };
	struct texinfo_t { float textureVecs[2][4]; float lightmapVecs[2][4]; int flags; int texdata; };
	struct dface_t
	{
		unsigned short planenum; unsigned char side, onNode; int firstedge; short numedges, texinfo, dispinfo,
		surfaceFogVolumeID; unsigned char styles[4]; int lightofs; float area; int lmMins[2], lmSize[2];
		int origFace; unsigned short numPrims, firstPrimID; unsigned int smoothingGroups;
	};
	struct dedge_t { unsigned short v[2]; };
	struct dmodel_t { Vector mins, maxs, origin; int headnode, firstface, numfaces; };
	struct dnode_t { int planenum; int children[2]; short mins[3], maxs[3]; unsigned short firstface, numfaces; short area, pad; };
	struct dleaf_t { int contents; short cluster, areaflags; short mins[3], maxs[3]; unsigned short firstleafface, numleaffaces,
					 firstleafbrush, numleafbrushes; short waterData, pad; };
	struct dbrush_t { int firstside, numsides, contents; };
	struct dbrushside_t { unsigned short planenum; short texinfo, dispinfo, bevel; };
	struct ddispinfo_t { Vector startPosition; int dispVertStart, dispTriStart, power; unsigned char rest[176 - 24]; };
	struct ddispvert_t { Vector vec; float dist, alpha; };
	struct dgamelump_t { int id; unsigned short flags, version; int fileofs, filelen; };
	#pragma pack(pop)

	// One lump, decompressed if needed
	bool ReadLump( const CUtlVector<unsigned char> &file, const dheader_t &h, int lump, CUtlVector<unsigned char> &out )
	{
		const lump_t &l = h.lumps[lump];
		out.RemoveAll();
		if ( l.filelen <= 0 || l.fileofs < 0 || l.fileofs + l.filelen > file.Count() )
			return l.filelen == 0;
		unsigned char *p = (unsigned char *)file.Base() + l.fileofs;
		if ( CLZMA::IsCompressed( p ) )
		{
			out.SetCount( CLZMA::GetActualSize( p ) );
			return CLZMA::Uncompress( p, out.Base() ) == (unsigned int)out.Count();
		}
		out.SetCount( l.filelen );
		memcpy( out.Base(), p, l.filelen );
		return true;
	}

	bool ReadFile( const char *path, CUtlVector<unsigned char> &out )
	{
		FILE *f = fopen( path, "rb" );
		if ( !f )
			return false;
		fseek( f, 0, SEEK_END );
		long n = ftell( f );
		fseek( f, 0, SEEK_SET );
		out.SetCount( n );
		bool ok = fread( out.Base(), 1, n, f ) == (size_t)n;
		fclose( f );
		return ok;
	}

	// maps can be loose in the game dir, in download/, or in any custom/<folder>/
	bool FindMap( const char *gameDir, const char *level, CUtlVector<unsigned char> &out, char *found, int foundLen )
	{
		char path[1024];
		V_snprintf( path, sizeof( path ), "%s/%s", gameDir, level );
		if ( ReadFile( path, out ) ) { V_strncpy( found, path, foundLen ); return true; }
		V_snprintf( path, sizeof( path ), "%s/download/%s", gameDir, level );
		if ( ReadFile( path, out ) ) { V_strncpy( found, path, foundLen ); return true; }
		char customDir[1024];
		V_snprintf( customDir, sizeof( customDir ), "%s/custom", gameDir );
		if ( DIR *d = opendir( customDir ) )
		{
			while ( struct dirent *e = readdir( d ) )
			{
				if ( e->d_name[0] == '.' )
					continue;
				V_snprintf( path, sizeof( path ), "%s/%s/%s", customDir, e->d_name, level );
				if ( ReadFile( path, out ) ) { closedir( d ); V_strncpy( found, path, foundLen ); return true; }
			}
			closedir( d );
		}
		return false;
	}

	template <class T> int LumpCount( const CUtlVector<unsigned char> &l ) { return l.Count() / sizeof( T ); }
	template <class T> const T *LumpData( const CUtlVector<unsigned char> &l ) { return (const T *)l.Base(); }

	// keeps the part of a convex polygon where dot( p, n ) <= d
	void ChopPolygon( CUtlVector<Vector> &poly, const Vector &n, float d )
	{
		CUtlVector<Vector> out;
		for ( int i = 0; i < poly.Count(); i++ )
		{
			const Vector &a = poly[i], &b = poly[( i + 1 ) % poly.Count()];
			float da = DotProduct( a, n ) - d, db = DotProduct( b, n ) - d;
			if ( da <= 0 )
				out.AddToTail( a );
			if ( ( da < 0 && db > 0 ) || ( da > 0 && db < 0 ) )
				out.AddToTail( a + ( b - a ) * ( da / ( da - db ) ) );
		}
		poly.Swap( out );
	}

	struct BrushEntity_t { char classname[64]; Vector origin; };

	// prop entities that stay where they are (the server says "prop" for these too; physics props get knocked about)
	bool StillProp( const char *classname ) { return !V_stricmp( classname, "prop_dynamic" ) || !V_stricmp( classname, "prop_dynamic_override" ); }

	// the entity lump: brush entities (model "*N": model number -> classname + origin) and prop entities
	void ParseEntities( const char *text, int len, CUtlVector<BrushEntity_t> &models, CUtlVector<P2StaticProp_t> &props )
	{
		const char *p = text, *end = text + len;
		while ( p < end )
		{
			while ( p < end && *p != '{' ) p++;
			if ( p >= end ) break;
			p++;
			char classname[64] = "", origin[64] = "", angles[64] = "", mdl[128] = "";
			bool parented = false;
			int model = -1;
			while ( p < end && *p != '}' )
			{
				char key[64], value[256];
				char *dst[2] = { key, value };
				int sizes[2] = { sizeof( key ), sizeof( value ) };
				bool ok = true;
				for ( int k = 0; k < 2 && ok; k++ )
				{
					while ( p < end && *p != '"' && *p != '}' ) p++;
					if ( p >= end || *p == '}' ) { ok = false; break; }
					p++;
					int n = 0;
					while ( p < end && *p != '"' ) { if ( n < sizes[k] - 1 ) dst[k][n++] = *p; p++; }
					dst[k][n] = 0;
					p++;
				}
				if ( !ok ) break;
				if ( !V_stricmp( key, "classname" ) ) V_strncpy( classname, value, sizeof( classname ) );
				else if ( !V_stricmp( key, "origin" ) ) V_strncpy( origin, value, sizeof( origin ) );
				else if ( !V_stricmp( key, "angles" ) ) V_strncpy( angles, value, sizeof( angles ) );
				else if ( !V_stricmp( key, "parentname" ) ) parented = value[0] != 0;
				else if ( !V_stricmp( key, "model" ) && value[0] == '*' ) model = atoi( value + 1 );
				else if ( !V_stricmp( key, "model" ) ) V_strncpy( mdl, value, sizeof( mdl ) );
			}
			if ( model > 0 && model < 65536 )
			{
				if ( models.Count() <= model )
				{
					int old = models.Count();
					models.SetCount( model + 1 );
					for ( int i = old; i <= model; i++ ) models[i].classname[0] = 0;
				}
				BrushEntity_t &e = models[model];
				V_strncpy( e.classname, classname[0] ? classname : "?", sizeof( e.classname ) );
				e.origin.Init();
				sscanf( origin, "%f %f %f", &e.origin.x, &e.origin.y, &e.origin.z );
			}
			else if ( mdl[0] && !parented && StillProp( classname ) && V_stristr( mdl, ".mdl" ) )
			{
				P2StaticProp_t &sp = props[props.AddToTail()];
				V_strncpy( sp.model, mdl, sizeof( sp.model ) );
				sp.origin.Init();
				sp.angles.Init();
				sscanf( origin, "%f %f %f", &sp.origin.x, &sp.origin.y, &sp.origin.z );
				sscanf( angles, "%f %f %f", &sp.angles.x, &sp.angles.y, &sp.angles.z );
			}
		}
	}
}

static void PlaneAxes( const Vector &n, Vector *u, Vector *v )
{
	Vector up = fabsf( n.z ) > 0.9f ? Vector( 0, 1, 0 ) : Vector( 0, 0, 1 );
	*u = CrossProduct( up, n );
	VectorNormalize( *u );
	*v = CrossProduct( n, *u );
}

// a flat convex polygon
static void AddFlatFace( CP2Bsp &bsp, const Vector *pVerts, int n, const Vector &normal, float dist, int kind )
{
	P2Face_t &out = bsp.m_Faces[bsp.m_Faces.AddToTail()];
	out.normal = normal;
	out.dist = dist;
	out.firstVert = bsp.m_Verts.Count();
	out.numVerts = n;
	out.dispPower = 0;
	out.kind = kind;
	out.entClass = -1;
	out.mins.Init( FLT_MAX, FLT_MAX, FLT_MAX );
	out.maxs.Init( -FLT_MAX, -FLT_MAX, -FLT_MAX );
	for ( int k = 0; k < n; k++ )
	{
		bsp.m_Verts.AddToTail( pVerts[k] );
		VectorMin( out.mins, pVerts[k], out.mins );
		VectorMax( out.maxs, pVerts[k], out.maxs );
	}
	bsp.m_nCount[kind]++;
}

// the two triangles of grid cell (i, j) as corner offsets (di, dj), in the engine's diagonal pattern
static void CellTriangles( int ndx, int tri[2][3][2] )
{
	static const int odd[2][3][2] = { { { 0, 0 }, { 1, 0 }, { 0, 1 } }, { { 0, 1 }, { 1, 0 }, { 1, 1 } } };	// TLtoBR
	static const int even[2][3][2] = { { { 0, 0 }, { 1, 0 }, { 1, 1 } }, { { 0, 0 }, { 1, 1 }, { 0, 1 } } };	// BLtoTR
	memcpy( tri, ( ndx & 1 ) ? odd : even, sizeof( odd ) );
}

void CP2Bsp::DispTriangle( const P2Face_t &f, int i, int j, int which, Vector *a, Vector *b, Vector *c ) const
{
	int w = DispSide( f ), tri[2][3][2];
	CellTriangles( i * w + j, tri );
	const Vector *v = &m_Verts[f.firstVert];
	*a = v[( i + tri[which][0][0] ) * w + j + tri[which][0][1]];
	*b = v[( i + tri[which][1][0] ) * w + j + tri[which][1][1]];
	*c = v[( i + tri[which][2][0] ) * w + j + tri[which][2][1]];
}

void CP2Bsp::Clear()
{
	m_StaticProps.Purge();
	m_FaceClusters.Purge();
	m_EntClasses.clear();
	m_Nodes.Purge();
	m_Leafs.Purge();
	m_Vis.Purge();
	m_nClusters = 0;
	m_nHeadNode = 0;
	m_Faces.Purge();
	m_Verts.Purge();
	memset( m_nCount, 0, sizeof( m_nCount ) );
	m_szLevel[0] = 0;
}

bool CP2Bsp::Load( const char *pszGameDir, const char *pszLevel, P2ReadFileFn pfnRead )
{
	Clear();
	CUtlVector<unsigned char> file;
	char path[1024];
	if ( !FindMap( pszGameDir, pszLevel, file, path, sizeof( path ) ) )
	{
		if ( !pfnRead || !pfnRead( pszLevel, file ) )
		{
			Warning( "p2paint: can't read %s\n", pszLevel );
			return false;
		}
		V_snprintf( path, sizeof( path ), "%s (game filesystem)", pszLevel );
	}
	if ( file.Count() < (int)sizeof( dheader_t ) )
		return false;
	dheader_t h;
	memcpy( &h, file.Base(), sizeof( h ) );
	if ( h.ident != ( ( 'P' << 24 ) | ( 'S' << 16 ) | ( 'B' << 8 ) | 'V' ) || h.version < 19 || h.version > 21 )
	{
		Warning( "p2paint: %s is not a supported BSP (version %d)\n", path, h.version );
		return false;
	}

	CUtlVector<unsigned char> ents, planes, texdata, verts, nodes, texinfo, faces, leafs, edges, surfedges, models, leafbrushes,
		brushes, brushsides, dispinfo, dispverts, strData, strTable, vis;
	bool ok = ReadLump( file, h, LUMP_ENTITIES, ents ) && ReadLump( file, h, LUMP_PLANES, planes ) &&
			  ReadLump( file, h, LUMP_TEXDATA, texdata ) && ReadLump( file, h, LUMP_VERTEXES, verts ) &&
			  ReadLump( file, h, LUMP_NODES, nodes ) && ReadLump( file, h, LUMP_TEXINFO, texinfo ) &&
			  ReadLump( file, h, LUMP_FACES, faces ) && ReadLump( file, h, LUMP_LEAFS, leafs ) &&
			  ReadLump( file, h, LUMP_EDGES, edges ) && ReadLump( file, h, LUMP_SURFEDGES, surfedges ) &&
			  ReadLump( file, h, LUMP_MODELS, models ) && ReadLump( file, h, LUMP_LEAFBRUSHES, leafbrushes ) &&
			  ReadLump( file, h, LUMP_BRUSHES, brushes ) && ReadLump( file, h, LUMP_BRUSHSIDES, brushsides ) &&
			  ReadLump( file, h, LUMP_DISPINFO, dispinfo ) && ReadLump( file, h, LUMP_DISP_VERTS, dispverts ) &&
			  ReadLump( file, h, LUMP_TEXDATA_STRING_DATA, strData ) && ReadLump( file, h, LUMP_TEXDATA_STRING_TABLE, strTable ) &&
			  ReadLump( file, h, LUMP_VISIBILITY, vis );
	if ( ok && faces.Count() == 0 )
		ok = ReadLump( file, h, LUMP_FACES_HDR, faces );
	if ( !ok || models.Count() < (int)sizeof( dmodel_t ) || h.lumps[LUMP_LEAFS].version != 1 )
	{
		Warning( "p2paint: %s: unreadable lumps\n", path );
		return false;
	}

	const dplane_t *pPlanes = LumpData<dplane_t>( planes );
	const dtexdata_t *pTexdata = LumpData<dtexdata_t>( texdata );
	const Vector *pVerts = LumpData<Vector>( verts );
	const texinfo_t *pTexinfo = LumpData<texinfo_t>( texinfo );
	const dface_t *pFaces = LumpData<dface_t>( faces );
	const dedge_t *pEdges = LumpData<dedge_t>( edges );
	const int *pSurfedges = LumpData<int>( surfedges );
	const int *pStrTable = LumpData<int>( strTable );
	const dmodel_t *pModels = LumpData<dmodel_t>( models );
	const ddispinfo_t *pDispInfo = LumpData<ddispinfo_t>( dispinfo );
	const ddispvert_t *pDispVerts = LumpData<ddispvert_t>( dispverts );
	const int nPlanes = LumpCount<dplane_t>( planes ), nTexinfo = LumpCount<texinfo_t>( texinfo );

	// the world's BSP tree and PVS, for visibility and "is this point inside something solid"
	{
		const dnode_t *pNodes = LumpData<dnode_t>( nodes );
		for ( int i = 0; i < LumpCount<dnode_t>( nodes ); i++ )
		{
			Node_t &n = m_Nodes[m_Nodes.AddToTail()];
			int pn = pNodes[i].planenum;
			n.normal = pn >= 0 && pn < nPlanes ? pPlanes[pn].normal : Vector( 0, 0, 1 );
			n.dist = pn >= 0 && pn < nPlanes ? pPlanes[pn].dist : 0.0f;
			n.children[0] = pNodes[i].children[0];
			n.children[1] = pNodes[i].children[1];
		}
		const dleaf_t *pLeafs = LumpData<dleaf_t>( leafs );
		for ( int i = 0; i < LumpCount<dleaf_t>( leafs ); i++ )
		{
			Leaf_t &l = m_Leafs[m_Leafs.AddToTail()];
			l.contents = pLeafs[i].contents;
			l.cluster = pLeafs[i].cluster;
		}
		m_Vis.Swap( vis );
		m_nClusters = m_Vis.Count() >= 4 ? *(const int *)m_Vis.Base() : 0;
		if ( m_nClusters < 0 || 4 + m_nClusters * 8 > m_Vis.Count() )
			m_nClusters = 0;
		m_nHeadNode = pModels[0].headnode;
	}

	// texture name of a texinfo ("" if none)
	auto TexName = [&]( int ti ) -> const char *
	{
		if ( ti < 0 || ti >= nTexinfo )
			return "";
		int td = pTexinfo[ti].texdata;
		if ( td < 0 || td >= LumpCount<dtexdata_t>( texdata ) )
			return "";
		int sid = pTexdata[td].nameStringTableID;
		if ( sid < 0 || sid >= LumpCount<int>( strTable ) || pStrTable[sid] >= strData.Count() )
			return "";
		return (const char *)strData.Base() + pStrTable[sid];
	};
	// visible tool textures take paint like any other; trigger / clip / invisible ones only where asked
	auto IsTool = [&]( const char *name, const char *tool ) { return !V_strnicmp( name, "TOOLS/", 6 ) && !V_stricmp( name + 6, tool ); };
	auto VisibleTool = [&]( const char *name ) { return IsTool( name, "TOOLSBLACK" ) || IsTool( name, "TOOLSWHITE" ); };

	CUtlVector<Vector> poly;
	auto FacePolygon = [&]( const dface_t &f, const Vector &offset )
	{
		poly.RemoveAll();
		for ( int e = 0; e < f.numedges; e++ )
		{
			int se = pSurfedges[f.firstedge + e];
			poly.AddToTail( pVerts[se >= 0 ? pEdges[se].v[0] : pEdges[-se].v[1]] + offset );
		}
	};

	// world faces and displacements
	const dmodel_t &world = pModels[0];
	for ( int fi = world.firstface; fi < world.firstface + world.numfaces && fi < LumpCount<dface_t>( faces ); fi++ )
	{
		const dface_t &f = pFaces[fi];
		if ( f.numedges < 3 || f.texinfo < 0 || f.texinfo >= nTexinfo || f.planenum >= nPlanes )
			continue;
		if ( pTexinfo[f.texinfo].flags & SURF_NOPAINT )
			continue;
		const bool sky = ( pTexinfo[f.texinfo].flags & SURF_SKYFACE ) != 0;
		const char *tex = TexName( f.texinfo );
		if ( !sky && !V_strnicmp( tex, "TOOLS/", 6 ) && !VisibleTool( tex ) )
			continue;
		const dplane_t &pl = pPlanes[f.planenum];
		Vector normal = f.side ? -pl.normal : pl.normal;
		float dist = f.side ? -pl.dist : pl.dist;
		FacePolygon( f, vec3_origin );

		if ( f.dispinfo == -1 )
		{
			AddFlatFace( *this, poly.Base(), poly.Count(), normal, dist, sky ? P2FACE_INVISIBLE : P2FACE_WORLD );
			continue;
		}
		if ( f.dispinfo < 0 || f.dispinfo >= LumpCount<ddispinfo_t>( dispinfo ) || poly.Count() != 4 )
			continue;
		const ddispinfo_t &di = pDispInfo[f.dispinfo];
		int w = ( 1 << di.power ) + 1, n = w - 1;
		if ( di.power < 1 || di.power > 4 || di.dispVertStart < 0 || di.dispVertStart + w * w > LumpCount<ddispvert_t>( dispverts ) )
			continue;
		// corner 0 = the one closest to startPosition, then the face's winding
		int start = 0;
		for ( int k = 1; k < 4; k++ )
			if ( poly[k].DistToSqr( di.startPosition ) < poly[start].DistToSqr( di.startPosition ) )
				start = k;
		Vector c[4];
		for ( int k = 0; k < 4; k++ )
			c[k] = poly[( k + start ) % 4];

		P2Face_t &out = m_Faces[m_Faces.AddToTail()];
		out.normal = normal;
		out.dist = dist;
		out.firstVert = m_Verts.Count();
		out.numVerts = w * w;
		out.dispPower = di.power;
		out.kind = P2FACE_DISP;
		out.entClass = -1;
		out.mins.Init( FLT_MAX, FLT_MAX, FLT_MAX );
		out.maxs.Init( -FLT_MAX, -FLT_MAX, -FLT_MAX );
		for ( int i = 0; i < w; i++ )
		{
			Vector left = c[0] + ( c[1] - c[0] ) * ( (float)i / n ), right = c[3] + ( c[2] - c[3] ) * ( (float)i / n );
			for ( int j = 0; j < w; j++ )
			{
				Vector flat = left + ( right - left ) * ( (float)j / n );
				const ddispvert_t &dv = pDispVerts[di.dispVertStart + i * w + j];
				Vector pos = flat + dv.vec * dv.dist;
				m_Verts.AddToTail( pos );
				VectorMin( out.mins, pos, out.mins );
				VectorMax( out.maxs, pos, out.maxs );
			}
		}
		m_nCount[P2FACE_DISP]++;
	}

	// brush entities that stay where they spawn: their faces, moved to the entity's origin
	CUtlVector<BrushEntity_t> ents2;
	CUtlVector<P2StaticProp_t> entProps;
	ParseEntities( (const char *)ents.Base(), ents.Count(), ents2, entProps );
	for ( int m = 1; m < ents2.Count() && m < LumpCount<dmodel_t>( models ); m++ )
	{
		const BrushEntity_t &e = ents2[m];
		if ( !e.classname[0] )
			continue;
		bool skip = false;
		for ( int k = 0; k < (int)ARRAYSIZE( s_SkipClasses ) && !skip; k++ )
			skip = !V_stricmp( e.classname, s_SkipClasses[k] );
		if ( skip )
			continue;
		const dmodel_t &mdl = pModels[m];
		int classIdx = -1;
		for ( size_t k = 0; k < m_EntClasses.size() && classIdx < 0; k++ )
			if ( !V_stricmp( m_EntClasses[k].c_str(), e.classname ) )
				classIdx = (int)k;
		if ( classIdx < 0 )
		{
			classIdx = (int)m_EntClasses.size();
			m_EntClasses.push_back( e.classname );
		}
		// triggers by what they are (the plugin goes by FSOLID_TRIGGER), whatever texture the mapper gave them
		const bool triggerClass = !V_strnicmp( e.classname, "trigger_", 8 ) || !V_stricmp( e.classname, "func_buyzone" ) ||
								  !V_stricmp( e.classname, "func_bomb_target" ) || !V_stricmp( e.classname, "func_hostage_rescue" );
		for ( int fi = mdl.firstface; fi < mdl.firstface + mdl.numfaces && fi < LumpCount<dface_t>( faces ); fi++ )
		{
			const dface_t &f = pFaces[fi];
			if ( f.numedges < 3 || f.dispinfo != -1 || f.texinfo < 0 || f.texinfo >= nTexinfo || f.planenum >= nPlanes )
				continue;
			if ( pTexinfo[f.texinfo].flags & SURF_NOPAINT )
				continue;
			const char *tex = TexName( f.texinfo );
			int kind = P2FACE_ENTITY;
			if ( triggerClass || IsTool( tex, "TOOLSTRIGGER" ) || ( pTexinfo[f.texinfo].flags & 0x0040 /*SURF_TRIGGER*/ ) )
				kind = P2FACE_TRIGGER;
			else if ( IsTool( tex, "TOOLSCLIP" ) || IsTool( tex, "TOOLSPLAYERCLIP" ) )
				kind = P2FACE_CLIP;
			else if ( IsTool( tex, "TOOLSINVISIBLE" ) )
				kind = P2FACE_INVISIBLE;
			else if ( !V_strnicmp( tex, "TOOLS/", 6 ) && !VisibleTool( tex ) )
				continue;
			const dplane_t &pl = pPlanes[f.planenum];
			Vector normal = f.side ? -pl.normal : pl.normal;
			float dist = ( f.side ? -pl.dist : pl.dist ) + DotProduct( normal, e.origin );
			FacePolygon( f, e.origin );
			AddFlatFace( *this, poly.Base(), poly.Count(), normal, dist, kind );
			m_Faces.Tail().entClass = classIdx;
		}
	}

	// clips and invisible walls: world brushes have no faces for those, so build them from the brush planes
	{
		const int nFlat = m_Faces.Count();
		const dnode_t *pNodes = LumpData<dnode_t>( nodes );
		const dleaf_t *pLeafs = LumpData<dleaf_t>( leafs );
		const unsigned short *pLeafBrushes = LumpData<unsigned short>( leafbrushes );
		const dbrush_t *pBrushes = LumpData<dbrush_t>( brushes );
		const dbrushside_t *pSides = LumpData<dbrushside_t>( brushsides );
		const int nBrushes = LumpCount<dbrush_t>( brushes ), nSides = LumpCount<dbrushside_t>( brushsides );
		CUtlVector<unsigned char> seen;
		seen.SetCount( nBrushes );
		memset( seen.Base(), 0, nBrushes );
		// the world's brushes = those in the world tree's leaves
		CUtlVector<int> stack;
		stack.AddToTail( world.headnode );
		while ( stack.Count() )
		{
			int node = stack.Tail();
			stack.RemoveMultipleFromTail( 1 );
			if ( node >= 0 )
			{
				if ( node < LumpCount<dnode_t>( nodes ) )
					stack.AddToTail( pNodes[node].children[0] ), stack.AddToTail( pNodes[node].children[1] );
				continue;
			}
			int leaf = -1 - node;
			if ( leaf >= LumpCount<dleaf_t>( leafs ) )
				continue;
			const dleaf_t &l = pLeafs[leaf];
			for ( int k = 0; k < l.numleafbrushes; k++ )
			{
				int lb = l.firstleafbrush + k;
				if ( lb < LumpCount<unsigned short>( leafbrushes ) && pLeafBrushes[lb] < nBrushes )
					seen[pLeafBrushes[lb]] = 1;
			}
		}
		for ( int b = 0; b < nBrushes; b++ )
		{
			if ( !seen[b] )
				continue;
			const dbrush_t &br = pBrushes[b];
			if ( br.firstside < 0 || br.firstside + br.numsides > nSides )
				continue;
			bool clipBrush = ( br.contents & ( CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP ) ) && !( br.contents & CONTENTS_SOLID );
			bool solidBrush = ( br.contents & CONTENTS_SOLID ) != 0;
			for ( int s = 0; s < br.numsides; s++ )
			{
				const dbrushside_t &side = pSides[br.firstside + s];
				if ( side.bevel || side.planenum >= nPlanes )
					continue;
				// clip brushes: every side; solid brushes: the sides nobody draws (they're walls in the game)
				const char *sideTex = TexName( side.texinfo );
				if ( !clipBrush && !( solidBrush && ( IsTool( sideTex, "TOOLSINVISIBLE" ) || IsTool( sideTex, "TOOLSNODRAW" ) ||
													  IsTool( sideTex, "TOOLSSKYBOX" ) ) ) )
					continue;
				const dplane_t &pl = pPlanes[side.planenum];
				// a huge square on the plane, cut down by the brush's other planes
				Vector u, v;
				PlaneAxes( pl.normal, &u, &v );
				Vector o = pl.normal * pl.dist;
				const float big = 65536.0f;
				poly.RemoveAll();
				poly.AddToTail( o - u * big - v * big );
				poly.AddToTail( o - u * big + v * big );
				poly.AddToTail( o + u * big + v * big );
				poly.AddToTail( o + u * big - v * big );
				if ( DotProduct( CrossProduct( poly[1] - poly[0], poly[2] - poly[0] ), pl.normal ) > 0 )
					V_swap( poly[1], poly[3] );		// the BSP's winding: clockwise seen from the front
				for ( int s2 = 0; s2 < br.numsides && poly.Count() >= 3; s2++ )
				{
					const dbrushside_t &other = pSides[br.firstside + s2];
					if ( s2 == s || other.planenum >= nPlanes )
						continue;
					ChopPolygon( poly, pPlanes[other.planenum].normal, pPlanes[other.planenum].dist );
				}
				if ( poly.Count() < 3 )
					continue;
				Vector centre( 0, 0, 0 );
				for ( int k = 0; k < poly.Count(); k++ )
					centre += poly[k];
				centre /= poly.Count();
				// only sides that face open space (buried ones are inside walls); sampled at the centre and near each corner
				bool exposed = false;
				for ( int k = -1; k < poly.Count() && !exposed; k++ )
				{
					Vector p = k < 0 ? centre : poly[k] + ( centre - poly[k] ) * 0.2f;
					int leaf = LeafAt( p + pl.normal * 1.0f );
					exposed = leaf >= 0 && !( m_Leafs[leaf].contents & CONTENTS_SOLID );
				}
				if ( !exposed )
					continue;
				// skip it where a visible world face already lies on the same plane
				bool covered = false;
				for ( int fi = 0; fi < nFlat && !covered; fi++ )
				{
					const P2Face_t &f = m_Faces[fi];
					if ( f.dispPower || ( f.kind != P2FACE_WORLD && f.kind != P2FACE_INVISIBLE ) || DotProduct( f.normal, pl.normal ) < 0.999f ||
						 fabsf( f.dist - pl.dist ) > 0.5f )
						continue;
					int pos = 0, neg = 0;
					for ( int k = 0; k < f.numVerts; k++ )
					{
						const Vector &a = m_Verts[f.firstVert + k], &c = m_Verts[f.firstVert + ( k + 1 ) % f.numVerts];
						float d = DotProduct( CrossProduct( c - a, centre - a ), f.normal );
						if ( d > 0.01f ) pos++; else if ( d < -0.01f ) neg++;
					}
					covered = !( pos && neg );
				}
				if ( !covered )
					AddFlatFace( *this, poly.Base(), poly.Count(), pl.normal, pl.dist, clipBrush ? P2FACE_CLIP : P2FACE_INVISIBLE );
			}
		}
	}

	// static props: where they are (the models are CP2Props' job)
	{
		const lump_t &gl = h.lumps[LUMP_GAME_LUMP];
		const unsigned char *base = file.Base();
		int count = gl.filelen >= 4 ? *(const int *)( base + gl.fileofs ) : 0;
		for ( int i = 0; i < count && 4 + ( i + 1 ) * (int)sizeof( dgamelump_t ) <= gl.filelen; i++ )
		{
			const dgamelump_t &g = *(const dgamelump_t *)( base + gl.fileofs + 4 + i * sizeof( dgamelump_t ) );
			if ( g.id != ( ( 's' << 24 ) | ( 'p' << 16 ) | ( 'r' << 8 ) | 'p' ) || g.fileofs < 0 || g.fileofs + g.filelen > file.Count() )
				continue;
			CUtlVector<unsigned char> sprp;
			unsigned char *src = (unsigned char *)base + g.fileofs;
			if ( CLZMA::IsCompressed( src ) )
			{
				sprp.SetCount( CLZMA::GetActualSize( src ) );
				if ( CLZMA::Uncompress( src, sprp.Base() ) != (unsigned int)sprp.Count() )
					break;
			}
			else
			{
				sprp.SetCount( g.filelen );
				memcpy( sprp.Base(), src, g.filelen );
			}
			const unsigned char *p = sprp.Base(), *end = p + sprp.Count();
			if ( p + 4 > end ) break;
			int nDict = *(const int *)p; p += 4;
			if ( nDict < 0 || p + nDict * 128 > end ) break;
			const char *dict = (const char *)p; p += nDict * 128;
			if ( p + 4 > end ) break;
			int nLeafs = *(const int *)p; p += 4 + nLeafs * 2;
			if ( p + 4 > end ) break;
			int nProps = *(const int *)p; p += 4;
			if ( nProps <= 0 ) break;
			int size = (int)( end - p ) / nProps;
			if ( size < 56 ) break;
			for ( int k = 0; k < nProps; k++ )
			{
				const unsigned char *q = p + k * size;
				int type = *(const unsigned short *)( q + 24 );
				if ( type >= nDict )
					continue;
				P2StaticProp_t &sp = m_StaticProps[m_StaticProps.AddToTail()];
				V_strncpy( sp.model, dict + type * 128, MIN( (int)sizeof( sp.model ), 128 ) );
				memcpy( &sp.origin, q, 12 );
				memcpy( &sp.angles, q + 12, 12 );
			}
			break;
		}
	}

	const int nStatic = m_StaticProps.Count();
	m_StaticProps.AddVectorToTail( entProps );

	// the vis clusters each surface is in (from its bounds)
	{
		std::vector<int> clusters;
		for ( int i = 0; i < m_Faces.Count(); i++ )
		{
			P2Face_t &f = m_Faces[i];
			BoxClusters( f.mins - Vector( 1, 1, 1 ), f.maxs + Vector( 1, 1, 1 ), clusters );
			f.firstCluster = m_FaceClusters.Count();
			f.numClusters = (int)clusters.size();
			for ( size_t k = 0; k < clusters.size(); k++ )
				m_FaceClusters.AddToTail( clusters[k] );
		}
	}

	V_strncpy( m_szLevel, pszLevel, sizeof( m_szLevel ) );
	Msg( "p2paint: %s: %d world faces, %d displacements, %d brush entity faces, %d trigger faces, %d clip faces, %d sky / invisible wall faces, %d static props, %d prop entities; %d vis clusters\n",
		 path, m_nCount[P2FACE_WORLD], m_nCount[P2FACE_DISP], m_nCount[P2FACE_ENTITY], m_nCount[P2FACE_TRIGGER], m_nCount[P2FACE_CLIP],
		 m_nCount[P2FACE_INVISIBLE], nStatic, entProps.Count(), m_nClusters );
	return true;
}

int CP2Bsp::LeafAt( const Vector &p ) const
{
	int node = m_nHeadNode;
	for ( int guard = 0; node >= 0 && guard < 4096; guard++ )
	{
		if ( node >= m_Nodes.Count() )
			return -1;
		const Node_t &n = m_Nodes[node];
		node = n.children[DotProduct( n.normal, p ) - n.dist >= 0 ? 0 : 1];
	}
	int leaf = -1 - node;
	return leaf >= 0 && leaf < m_Leafs.Count() ? leaf : -1;
}

int CP2Bsp::ClusterAt( const Vector &p ) const
{
	int leaf = LeafAt( p );
	return leaf >= 0 ? m_Leafs[leaf].cluster : -1;
}

bool CP2Bsp::ClusterVisibility( int cluster, std::vector<unsigned char> &bits ) const
{
	if ( cluster < 0 || cluster >= m_nClusters )
		return false;
	bits.assign( ( m_nClusters + 7 ) / 8, 0 );
	int ofs = ( (const int *)m_Vis.Base() )[1 + cluster * 2];	// byteofs[cluster][DVIS_PVS]
	if ( ofs <= 0 || ofs >= m_Vis.Count() )
		return false;
	// run-length: a zero byte is followed by how many zero bytes it stands for
	const unsigned char *in = m_Vis.Base() + ofs, *end = m_Vis.Base() + m_Vis.Count();
	size_t out = 0;
	while ( out < bits.size() && in < end )
	{
		if ( *in )
		{
			bits[out++] = *in++;
			continue;
		}
		if ( in + 1 >= end )
			break;
		out += in[1];
		in += 2;
	}
	return true;
}

void CP2Bsp::BoxClusters( const Vector &mins, const Vector &maxs, std::vector<int> &out ) const
{
	out.clear();
	Vector centre = ( mins + maxs ) * 0.5f, half = ( maxs - mins ) * 0.5f;
	int stack[512], n = 0;
	stack[n++] = m_nHeadNode;
	while ( n )
	{
		int node = stack[--n];
		if ( node < 0 )
		{
			int leaf = -1 - node;
			if ( leaf < m_Leafs.Count() && m_Leafs[leaf].cluster >= 0 )
			{
				int c = m_Leafs[leaf].cluster;
				bool have = false;
				for ( size_t k = 0; k < out.size() && !have; k++ )
					have = out[k] == c;
				if ( !have )
					out.push_back( c );
			}
			continue;
		}
		if ( node >= m_Nodes.Count() )
			continue;
		const Node_t &nd = m_Nodes[node];
		float d = DotProduct( nd.normal, centre ) - nd.dist;
		float r = fabsf( nd.normal.x ) * half.x + fabsf( nd.normal.y ) * half.y + fabsf( nd.normal.z ) * half.z;
		if ( d > -r && n < 511 )
			stack[n++] = nd.children[0];
		if ( d < r && n < 511 )
			stack[n++] = nd.children[1];
	}
}
