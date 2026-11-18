//=============================================================================
// p2paint: the props' triangles (see p2paint_props.h). Model reading from the archived gel version.
//=============================================================================
#include <map>
#include <string>

#include "p2paint_props.h"
#include "tier1/strtools.h"

#include "tier0/memdbgon.h"

namespace
{
	// --- model files (studio.h / optimize.h layouts as stored on disk) ---
	#pragma pack(push, 1)
	struct vvdHeader_t { int id, version, checksum, numLODs, numLODVertexes[8], numFixups, fixupTableStart, vertexDataStart, tangentDataStart; };
	struct vvdFixup_t { int lod, sourceVertexID, numVertexes; };
	struct vvdVertex_t { float weight[3]; char bone[3]; unsigned char numbones; Vector pos, normal; float u, v; };
	struct vtxHeader_t { int version, vertCacheSize; unsigned short maxBonesPerStrip, maxBonesPerTri; int maxBonesPerVert, checkSum,
						 numLODs, materialReplacementListOffset, numBodyParts, bodyPartOffset; };
	struct vtxBodyPart_t { int numModels, modelOffset; };
	struct vtxModel_t { int numLODs, lodOffset; };
	struct vtxModelLOD_t { int numMeshes, meshOffset; float switchPoint; };
	struct vtxMesh_t { int numStripGroups, stripGroupHeaderOffset; unsigned char flags; };
	struct vtxStripGroup_t { int numVerts, vertOffset, numIndices, indexOffset, numStrips, stripOffset; unsigned char flags; };
	struct vtxStrip_t { int numIndices, indexOffset, numVerts, vertOffset; short numBones; unsigned char flags; int numBoneStateChanges, boneStateChangeOffset; };
	struct vtxVertex_t { unsigned char boneWeightIndex[3], numBones; unsigned short origMeshVertID; char boneID[3]; };
	#pragma pack(pop)

	template <class T> const T *At( const CUtlVector<unsigned char> &f, int ofs, int count = 1 )
	{
		if ( ofs < 0 || count < 0 || (size_t)ofs + sizeof( T ) * (size_t)count > (size_t)f.Count() )
			return NULL;
		return (const T *)( f.Base() + ofs );
	}
	int ReadInt( const CUtlVector<unsigned char> &f, int ofs ) { const int *p = At<int>( f, ofs ); return p ? *p : 0; }

	// LOD 0 triangles of a model: .mdl (body part / model / mesh layout) + .vvd (vertices) + .dx90.vtx (indices)
	bool LoadModel( const char *mdlPath, P2ReadFileFn pfnRead, std::vector<Vector> &pos, std::vector<Vector> &normal, std::vector<int> &tris )
	{
		char base[260], path[272];
		V_strncpy( base, mdlPath, sizeof( base ) );
		V_StripExtension( base, base, sizeof( base ) );
		CUtlVector<unsigned char> mdl, vvd, vtx;
		V_snprintf( path, sizeof( path ), "%s.vvd", base );
		if ( !pfnRead( mdlPath, mdl ) || !pfnRead( path, vvd ) )
			return false;
		V_snprintf( path, sizeof( path ), "%s.dx90.vtx", base );
		if ( !pfnRead( path, vtx ) )
		{
			V_snprintf( path, sizeof( path ), "%s.dx80.vtx", base );
			if ( !pfnRead( path, vtx ) )
				return false;
		}
		if ( ReadInt( mdl, 0 ) != ( ( 'T' << 24 ) | ( 'S' << 16 ) | ( 'D' << 8 ) | 'I' ) )
			return false;

		// vertices for LOD 0 (fixups reorder them when the model has LODs)
		const vvdHeader_t *vh = At<vvdHeader_t>( vvd, 0 );
		if ( !vh || vh->id != ( ( 'V' << 24 ) | ( 'S' << 16 ) | ( 'D' << 8 ) | 'I' ) || vh->numLODVertexes[0] <= 0 )
			return false;
		const vvdVertex_t *all = At<vvdVertex_t>( vvd, vh->vertexDataStart, vh->numLODVertexes[0] );
		if ( !all )
			return false;
		std::vector<const vvdVertex_t *> verts;
		if ( vh->numFixups > 0 )
		{
			const vvdFixup_t *fx = At<vvdFixup_t>( vvd, vh->fixupTableStart, vh->numFixups );
			if ( !fx )
				return false;
			for ( int i = 0; i < vh->numFixups; i++ )
			{
				if ( fx[i].lod < 0 )
					continue;
				const vvdVertex_t *src = At<vvdVertex_t>( vvd, vh->vertexDataStart + fx[i].sourceVertexID * (int)sizeof( vvdVertex_t ), fx[i].numVertexes );
				if ( !src )
					return false;
				for ( int k = 0; k < fx[i].numVertexes; k++ )
					verts.push_back( src + k );
			}
		}
		else
		{
			for ( int k = 0; k < vh->numLODVertexes[0]; k++ )
				verts.push_back( all + k );
		}

		const vtxHeader_t *xh = At<vtxHeader_t>( vtx, 0 );
		int numBodyParts = ReadInt( mdl, 232 ), bodyPartIndex = ReadInt( mdl, 236 );
		if ( !xh || xh->version != 7 || xh->numBodyParts != numBodyParts )
			return false;
		const int base0 = (int)pos.size();
		for ( size_t k = 0; k < verts.size(); k++ )
		{
			pos.push_back( verts[k]->pos );
			normal.push_back( verts[k]->normal );
		}
		for ( int bp = 0; bp < numBodyParts; bp++ )
		{
			int mbp = bodyPartIndex + bp * 16;		// mstudiobodyparts_t: sznameindex, nummodels, base, modelindex
			if ( ReadInt( mdl, mbp + 4 ) <= 0 )
				continue;
			int mmodel = mbp + ReadInt( mdl, mbp + 12 );	// body 0 = the first model of each part
			int nummeshes = ReadInt( mdl, mmodel + 72 ), meshindex = ReadInt( mdl, mmodel + 76 );
			int modelFirstVert = ReadInt( mdl, mmodel + 84 ) / (int)sizeof( vvdVertex_t );

			int xbp = xh->bodyPartOffset + bp * (int)sizeof( vtxBodyPart_t );
			const vtxBodyPart_t *bph = At<vtxBodyPart_t>( vtx, xbp );
			if ( !bph || bph->numModels <= 0 )
				continue;
			int xm = xbp + bph->modelOffset;
			const vtxModel_t *mh = At<vtxModel_t>( vtx, xm );
			if ( !mh || mh->numLODs <= 0 )
				continue;
			int xl = xm + mh->lodOffset;
			const vtxModelLOD_t *lh = At<vtxModelLOD_t>( vtx, xl );
			if ( !lh || lh->numMeshes != nummeshes )
				continue;
			for ( int m = 0; m < nummeshes; m++ )
			{
				int mmesh = mmodel + meshindex + m * 116;		// mstudiomesh_t on disk
				int meshVertOffset = ReadInt( mdl, mmesh + 12 );
				int xmesh = xl + lh->meshOffset + m * (int)sizeof( vtxMesh_t );
				const vtxMesh_t *meh = At<vtxMesh_t>( vtx, xmesh );
				if ( !meh )
					continue;
				for ( int g = 0; g < meh->numStripGroups; g++ )
				{
					int xg = xmesh + meh->stripGroupHeaderOffset + g * (int)sizeof( vtxStripGroup_t );
					const vtxStripGroup_t *gh = At<vtxStripGroup_t>( vtx, xg );
					if ( !gh )
						continue;
					const vtxVertex_t *gv = At<vtxVertex_t>( vtx, xg + gh->vertOffset, gh->numVerts );
					const unsigned short *gi = At<unsigned short>( vtx, xg + gh->indexOffset, gh->numIndices );
					if ( !gv || !gi )
						continue;
					auto Vert = [&]( int idx ) -> int
					{
						if ( idx < 0 || idx >= gh->numVerts )
							return -1;
						int v = modelFirstVert + meshVertOffset + gv[idx].origMeshVertID;
						return v >= 0 && v < (int)verts.size() ? base0 + v : -1;
					};
					for ( int s = 0; s < gh->numStrips; s++ )
					{
						const vtxStrip_t *sh = At<vtxStrip_t>( vtx, xg + gh->stripOffset + s * (int)sizeof( vtxStrip_t ) );
						if ( !sh || sh->indexOffset < 0 || sh->indexOffset + sh->numIndices > gh->numIndices )
							continue;
						if ( sh->flags & 0x02 )	// tristrip
						{
							for ( int k = 0; k + 2 < sh->numIndices; k++ )
							{
								int a = Vert( gi[sh->indexOffset + k] ), b = Vert( gi[sh->indexOffset + k + 1] ), c = Vert( gi[sh->indexOffset + k + 2] );
								if ( a < 0 || b < 0 || c < 0 || a == b || b == c || a == c )
									continue;
								if ( k & 1 ) V_swap( b, c );
								tris.push_back( a ); tris.push_back( b ); tris.push_back( c );
							}
						}
						else
						{
							for ( int k = 0; k + 2 < sh->numIndices; k += 3 )
							{
								int a = Vert( gi[sh->indexOffset + k] ), b = Vert( gi[sh->indexOffset + k + 1] ), c = Vert( gi[sh->indexOffset + k + 2] );
								if ( a < 0 || b < 0 || c < 0 )
									continue;
								tris.push_back( a ); tris.push_back( b ); tris.push_back( c );
							}
						}
					}
				}
			}
		}
		return !tris.empty();
	}
}

void CP2Props::Load( const CP2Bsp &bsp, P2ReadFileFn pfnRead )
{
	Clear();
	if ( !pfnRead )
		return;
	std::map<std::string, int> byName;
	int failed = 0;
	for ( int i = 0; i < bsp.m_StaticProps.Count(); i++ )
	{
		const P2StaticProp_t &sp = bsp.m_StaticProps[i];
		char name[128];
		V_strncpy( name, sp.model, sizeof( name ) );
		V_strlower( name );
		V_FixSlashes( name, '/' );
		std::map<std::string, int>::iterator it = byName.find( name );
		int model;
		if ( it != byName.end() )
			model = it->second;
		else
		{
			Model_t m;
			if ( !LoadModel( name, pfnRead, m.pos, m.normal, m.tris ) )
			{
				byName[name] = -1;
				failed++;
				continue;
			}
			m.mins.Init( FLT_MAX, FLT_MAX, FLT_MAX );
			m.maxs.Init( -FLT_MAX, -FLT_MAX, -FLT_MAX );
			for ( size_t k = 0; k < m.pos.size(); k++ )
				VectorMin( m.mins, m.pos[k], m.mins ), VectorMax( m.maxs, m.pos[k], m.maxs );
			model = (int)m_Models.size();
			m_Models.push_back( m );
			byName[name] = model;
		}
		if ( model < 0 )
			continue;
		Prop_t p;
		p.model = model;
		AngleMatrix( sp.angles, sp.origin, p.toWorld );
		TransformAABB( p.toWorld, m_Models[model].mins, m_Models[model].maxs, p.mins, p.maxs );
		m_Props.push_back( p );
	}
	Msg( "p2paint: %d props take paint (%d models%s)\n", (int)m_Props.size(), (int)m_Models.size(), failed ? ", some models unreadable" : "" );
}

void CP2Props::Clear()
{
	m_Models.clear();
	m_Props.clear();
}

void CP2Props::PropsInBox( const Vector &mins, const Vector &maxs, std::vector<int> &out ) const
{
	out.clear();
	for ( size_t i = 0; i < m_Props.size(); i++ )
	{
		const Prop_t &p = m_Props[i];
		if ( mins.x <= p.maxs.x && maxs.x >= p.mins.x && mins.y <= p.maxs.y && maxs.y >= p.mins.y && mins.z <= p.maxs.z && maxs.z >= p.mins.z )
			out.push_back( (int)i );
	}
}

int CP2Props::Triangles( int prop ) const
{
	return (int)m_Models[m_Props[prop].model].tris.size() / 3;
}

void CP2Props::Triangle( int prop, int tri, Vector *a, Vector *b, Vector *c, Vector *pNormal ) const
{
	const Prop_t &p = m_Props[prop];
	const Model_t &m = m_Models[p.model];
	int ia = m.tris[tri * 3], ib = m.tris[tri * 3 + 1], ic = m.tris[tri * 3 + 2];
	VectorTransform( m.pos[ia], p.toWorld, *a );
	VectorTransform( m.pos[ib], p.toWorld, *b );
	VectorTransform( m.pos[ic], p.toWorld, *c );
	VectorRotate( m.normal[ia] + m.normal[ib] + m.normal[ic], p.toWorld, *pNormal );
	VectorNormalize( *pNormal );
}
