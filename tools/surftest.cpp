//=============================================================================
// Offline test of p2paint_surfaces (no game): loads a map + its props like the game does (loose files, the
// game's VPKs, the map's pakfile), then runs every splat from a file through P2Surf_Find / P2Surf_Build.
//   make surftest; LD_LIBRARY_PATH=<game>/bin/linux64 build/surftest <game root> <map> [splats.txt]
//   splats.txt: "x y z size [nx ny nz surf]" per line
//=============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

#include "tier0/platform.h"
#include "tier1/strtools.h"
#include "tier1/utlvector.h"
#include "p2paint_surfaces.h"

const float P2_DECAL_SCALE[8] = { 0.20f, 0.28f, 0.40f, 0.60f, 0.90f, 1.30f, 1.80f, 2.40f };

static std::string s_Game;		// .../cstrike
static std::string s_Root;		// .../Counter-Strike Source

static bool ReadWhole( const std::string &path, CUtlVector<unsigned char> &out )
{
	FILE *f = fopen( path.c_str(), "rb" );
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

// --- VPK v1/v2 directories: path -> (archive, offset, length, preload) ---
struct VpkEntry { std::string dir; int archive; unsigned offset, length; std::vector<unsigned char> preload; };
static std::map<std::string, VpkEntry> s_Vpk;

static void MountVpk( const std::string &dirFile )
{
	CUtlVector<unsigned char> d;
	if ( !ReadWhole( dirFile, d ) || d.Count() < 12 )
		return;
	const unsigned char *p = d.Base();
	unsigned sig = *(const unsigned *)p, ver = *(const unsigned *)( p + 4 ), tree = *(const unsigned *)( p + 8 );
	if ( sig != 0x55aa1234 )
		return;
	int header = ver == 2 ? 28 : 12;
	const unsigned char *q = p + header, *end = q + tree;
	std::string base = dirFile.substr( 0, dirFile.size() - 8 );	// strip "_dir.vpk"
	auto Str = [&]() { std::string s( (const char *)q ); q += s.size() + 1; return s; };
	int n = 0;
	while ( q < end )
	{
		std::string ext = Str();
		if ( ext.empty() ) break;
		while ( q < end )
		{
			std::string path = Str();
			if ( path.empty() ) break;
			while ( q < end )
			{
				std::string name = Str();
				if ( name.empty() ) break;
				VpkEntry e;
				e.dir = base;
				unsigned short preload = *(const unsigned short *)( q + 4 );
				e.archive = *(const unsigned short *)( q + 6 );
				e.offset = *(const unsigned *)( q + 8 );
				e.length = *(const unsigned *)( q + 12 );
				q += 18;
				e.preload.assign( q, q + preload );
				q += preload;
				if ( e.archive == 0x7fff )
					e.offset += header + tree, e.dir = dirFile, e.archive = -1;
				std::string full = ( path == " " ? "" : path + "/" ) + name + "." + ext;
				for ( char &c : full ) c = (char)tolower( c );
				s_Vpk[full] = e;
				n++;
			}
		}
	}
	printf( "mounted %s (%d files)\n", dirFile.c_str(), n );
}

static bool ReadVpk( const std::string &path, CUtlVector<unsigned char> &out )
{
	std::map<std::string, VpkEntry>::iterator it = s_Vpk.find( path );
	if ( it == s_Vpk.end() )
		return false;
	const VpkEntry &e = it->second;
	out.SetCount( (int)( e.preload.size() + e.length ) );
	if ( !e.preload.empty() )
		memcpy( out.Base(), e.preload.data(), e.preload.size() );
	if ( !e.length )
		return true;
	char arch[1024];
	if ( e.archive < 0 )
		V_strncpy( arch, e.dir.c_str(), sizeof( arch ) );
	else
		V_snprintf( arch, sizeof( arch ), "%s_%03d.vpk", e.dir.c_str(), e.archive );
	FILE *f = fopen( arch, "rb" );
	if ( !f )
		return false;
	fseek( f, e.offset, SEEK_SET );
	bool ok = fread( out.Base() + e.preload.size(), 1, e.length, f ) == e.length;
	fclose( f );
	return ok;
}

// --- the map's pakfile (lump 40, a zip of stored files) ---
static std::map<std::string, std::vector<unsigned char> > s_Pak;
static void MountPak( const CUtlVector<unsigned char> &bsp )
{
	const int *lumps = (const int *)( bsp.Base() + 8 );
	int ofs = lumps[40 * 4], len = lumps[40 * 4 + 1];
	if ( len <= 0 || ofs + len > bsp.Count() )
		return;
	const unsigned char *z = bsp.Base() + ofs;
	// end of central directory
	int eocd = -1;
	for ( int i = len - 22; i >= 0 && i >= len - 65557; i-- )
		if ( *(const unsigned *)( z + i ) == 0x06054b50 ) { eocd = i; break; }
	if ( eocd < 0 )
		return;
	int count = *(const unsigned short *)( z + eocd + 10 ), cd = *(const int *)( z + eocd + 16 );
	const unsigned char *p = z + cd;
	int stored = 0, other = 0;
	for ( int i = 0; i < count; i++ )
	{
		if ( *(const unsigned *)p != 0x02014b50 ) break;
		int method = *(const unsigned short *)( p + 10 ), csize = *(const int *)( p + 20 );
		int nlen = *(const unsigned short *)( p + 28 ), xlen = *(const unsigned short *)( p + 30 ), clen = *(const unsigned short *)( p + 32 );
		int local = *(const int *)( p + 42 );
		std::string name( (const char *)p + 46, nlen );
		for ( char &c : name ) c = (char)tolower( c );
		const unsigned char *lh = z + local;
		int dataOfs = local + 30 + *(const unsigned short *)( lh + 26 ) + *(const unsigned short *)( lh + 28 );
		if ( method == 0 )
			s_Pak[name].assign( z + dataOfs, z + dataOfs + csize ), stored++;
		else
			other++;
		p += 46 + nlen + xlen + clen;
	}
	printf( "map pak: %d stored files (%d compressed, skipped)\n", stored, other );
}

static bool ReadGameFile( const char *path, CUtlVector<unsigned char> &out )
{
	std::string p( path );
	for ( char &c : p ) c = (char)tolower( c );
	for ( char &c : p ) if ( c == '\\' ) c = '/';
	if ( ReadWhole( s_Game + "/" + p, out ) || ReadWhole( s_Game + "/download/" + p, out ) )
		return true;
	std::map<std::string, std::vector<unsigned char> >::iterator it = s_Pak.find( p );
	if ( it != s_Pak.end() )
	{
		out.SetCount( (int)it->second.size() );
		memcpy( out.Base(), it->second.data(), it->second.size() );
		return true;
	}
	return ReadVpk( p, out );
}

int main( int argc, char **argv )
{
	if ( argc < 3 )
	{
		fprintf( stderr, "usage: surftest <game root> <map> [splats.txt]\n" );
		return 1;
	}
	s_Root = argv[1];
	s_Game = s_Root + "/cstrike";
	MountVpk( s_Game + "/cstrike_pak_dir.vpk" );
	MountVpk( s_Root + "/hl2/hl2_misc_dir.vpk" );
	std::string level = std::string( "maps/" ) + argv[2] + ".bsp";
	CUtlVector<unsigned char> bsp;
	if ( ReadGameFile( level.c_str(), bsp ) )
		MountPak( bsp );
	if ( !P2Surf_Load( s_Game.c_str(), level.c_str(), ReadGameFile ) )
		return 1;
	P2Surf_Info();
	if ( argc < 4 )
		return 0;
	if ( !strcmp( argv[3], "aim" ) && argc > 10 )
	{
		// the aim ghost's own part: from (sx sy sz) towards (ex ey ez), triggers of these classes
		Vector a( atof( argv[4] ), atof( argv[5] ), atof( argv[6] ) ), b( atof( argv[7] ), atof( argv[8] ), atof( argv[9] ) ), hit, n;
		Vector dir = b - a;
		float len = VectorNormalize( dir );
		if ( P2Surf_AimOwn( a, dir, len, argv[10], &hit, &n ) )
			printf( "aim stops at %.1f %.1f %.1f normal %.2f %.2f %.2f\n", hit.x, hit.y, hit.z, n.x, n.y, n.z );
		else
			printf( "aim goes through to %.1f %.1f %.1f\n", b.x, b.y, b.z );
		return 0;
	}
	if ( !strcmp( argv[3], "list" ) )
	{
		// faces of a kind (argv[4], P2FaceKind_t): centre, normal, size
		extern CP2Bsp *P2Surf_Bsp();
		CP2Bsp &b = *P2Surf_Bsp();
		int kind = argc > 4 ? atoi( argv[4] ) : P2FACE_CLIP, shown = 0;
		for ( int i = 0; i < b.m_Faces.Count() && shown < 100000; i++ )
		{
			const P2Face_t &f = b.m_Faces[i];
			if ( f.kind != kind )
				continue;
			Vector c = ( f.mins + f.maxs ) * 0.5f, e = f.maxs - f.mins;
			printf( "face %d: centre %.0f %.0f %.0f normal %.2f %.2f %.2f extent %.0f %.0f %.0f\n", i, c.x, c.y, c.z, f.normal.x, f.normal.y, f.normal.z, e.x, e.y, e.z );
			shown++;
		}
		return 0;
	}

	FILE *f = fopen( argv[3], "r" );
	if ( !f )
		return 1;
	int counts[P2SURF_KINDS] = {}, built[P2SURF_KINDS] = {}, tris[P2SURF_KINDS] = {}, n = 0, badWinding = 0, badUV = 0;
	double tFind = 0, tBuild = 0;
	char line[512];
	while ( fgets( line, sizeof( line ), f ) )
	{
		Vector pos, normal( 0, 0, 0 );
		int size = 4, surf = P2SURF_UNKNOWN;
		int got = sscanf( line, "%f %f %f %d %f %f %f %d", &pos.x, &pos.y, &pos.z, &size, &normal.x, &normal.y, &normal.z, &surf );
		if ( got < 4 )
			continue;
		n++;
		double t0 = Plat_FloatTime();
		if ( surf < 0 || normal.LengthSqr() < 0.5f )
		{
			normal.Init();
			surf = P2Surf_Find( pos, &normal );
		}
		double t1 = Plat_FloatTime();
		float half = 32.0f * P2_DECAL_SCALE[clamp( size, 0, 7 )];
		P2DecalMesh_t mesh;
		P2Surf_Build( pos, normal, half, half, surf, mesh );
		double t2 = Plat_FloatTime();
		tFind += t1 - t0;
		tBuild += t2 - t1;
		counts[surf]++;
		if ( !mesh.verts.empty() )
			built[surf]++;
		tris[surf] += (int)mesh.verts.size() / 3;
		for ( size_t k = 0; k + 2 < mesh.verts.size(); k += 3 )
		{
			const P2DecalVert_t *v = &mesh.verts[k];
			Vector cr = CrossProduct( v[1].pos - v[0].pos, v[2].pos - v[0].pos );
			if ( DotProduct( cr, normal ) > 1e-4f )
			{
				badWinding++;
				if ( getenv( "VERBOSE" ) )
				{
					float area = cr.Length() * 0.5f;
					cr.NormalizeInPlace();
					printf( "    facing away: surf %d area %.4f, cos to the splat normal %.3f\n", surf, area, DotProduct( cr, normal ) );
				}
			}
			for ( int j = 0; j < 3; j++ )
				if ( v[j].u < -0.001f || v[j].u > 1.001f || v[j].v < -0.001f || v[j].v > 1.001f )
					badUV++;
		}
		if ( getenv( "VERBOSE" ) && surf != P2SURF_WORLD )
			printf( "  %.1f %.1f %.1f size %d -> surf %d normal %.2f %.2f %.2f: %d triangles, %d clusters\n", pos.x, pos.y, pos.z, size, surf,
					normal.x, normal.y, normal.z, (int)mesh.verts.size() / 3, (int)mesh.clusters.size() );
	}
	fclose( f );
	static const char *names[] = { "world", "displacement", "brush entity", "prop", "clip", "moving", "trigger", "invisible wall" };
	printf( "%d splats; find %.2f ms, build %.2f ms in all; %d triangles facing away, %d vertices outside the decal\n", n, tFind * 1000, tBuild * 1000,
			badWinding, badUV );
	for ( int k = 0; k < P2SURF_KINDS; k++ )
		if ( counts[k] )
			printf( "  %-15s %5d splats, %5d with our own decal, %6d triangles\n", names[k], counts[k], built[k], tris[k] );
	return 0;
}
