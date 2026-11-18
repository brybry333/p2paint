// build/patchtest <engine.so in> <out>: runs the in-memory engine patches (p2paint_enginepatch.cpp) on a file
// and writes the result (what the mod does to the game's engine code when it loads).
#include <stdio.h>
#include <string.h>
#include <vector>
#include "p2paint_enginepatch.h"

static bool Store( unsigned char *dst, const unsigned char *src, size_t n ) { memcpy( dst, src, n ); return true; }

int main( int argc, char **argv )
{
	if ( argc != 3 )
		return fprintf( stderr, "usage: patchtest <engine.so> <out>\n" ), 2;
	FILE *f = fopen( argv[1], "rb" );
	if ( !f )
		return perror( argv[1] ), 1;
	std::vector<unsigned char> buf;
	unsigned char chunk[65536];
	for ( size_t n; ( n = fread( chunk, 1, sizeof( chunk ), f ) ) > 0; )
		buf.insert( buf.end(), chunk, chunk + n );
	fclose( f );
	fputs( P2Patch_Buffer( buf.data(), buf.size(), Store ).c_str(), stdout );
	f = fopen( argv[2], "wb" );
	if ( !f || fwrite( buf.data(), 1, buf.size(), f ) != buf.size() )
		return perror( argv[2] ), 1;
	return fclose( f ) ? 1 : 0;
}
