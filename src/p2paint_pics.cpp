//=============================================================================
// p2paint pictures (see p2paint_pics.h).
//
// Adding a file runs MareTF (maretf create / edit) in a background thread:
//   1. decode at the picture's final size, RGBA, every frame      maretf create -f RGBA8888 --width W --height H
//   2. centre it on a transparent power-of-two canvas             (here: MareTF only makes mipmaps for
//                                                                   power-of-two sizes, and stretching to one
//                                                                   would distort the picture)
//   3. DXT5 + mipmaps + clamped edges                             maretf edit --set-format DXT5 --recompute-mips
// Size caps: the longer side is at most 1024 px (more never shows on a decal), the file at most 16 MiB - 1 like
// sprays (animations get smaller, then lose frames, to fit), and the engine's VTF rule
// floor(log2 w) + floor(log2 h) + floor(log2 frames) <= 25 (it refuses anything bigger).
// MareTF: the host's on Linux (the game runs in Steam's runtime container), maretf.exe next to us on Windows.
//=============================================================================
#include "p2paint_pics.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <mutex>

#include "p2paint_platform.h"

const float P2_DECAL_SCALE[8] = { 0.20f, 0.28f, 0.40f, 0.60f, 0.90f, 1.30f, 1.80f, 2.40f };

static const long long MAX_VTF_BYTES = 16777215;	// 16 MiB - 1, the spray cap
static const int MAX_SIDE = 1024;					// a still picture's longer side
static const int MIN_SIDE = 128;					// the engine's spray sizing needs 128+ px to be exact
static const int MIN_ANIM_SIDE = 256;				// animations drop frames before going below this
static const float SPRAY_FPS = 5.0f;				// Source plays sprays at 5 fps

static std::string s_GameDir, s_PicDir, s_LogoDir;
static std::vector<P2Picture> s_List;

static std::mutex s_StatusLock;
static P2AddStatus s_Status;
static bool s_bBusy;

//-----------------------------------------------------------------------------
// small file helpers
//-----------------------------------------------------------------------------
static bool ReadFile( const std::string &path, std::vector<uint8_t> &out, size_t limit = (size_t)-1 )
{
	FILE *f = fopen( path.c_str(), "rb" );
	if ( !f )
		return false;
	out.clear();
	uint8_t buf[65536];
	size_t n;
	while ( out.size() < limit && ( n = fread( buf, 1, std::min( sizeof( buf ), limit - out.size() ), f ) ) > 0 )
		out.insert( out.end(), buf, buf + n );
	bool ok = !ferror( f );
	fclose( f );
	return ok;
}

// write to <path>.tmp, then rename: the game never sees half a file
static bool WriteFileAtomic( const std::string &path, const void *data, size_t size )
{
	std::string tmp = path + ".tmp";
	FILE *f = fopen( tmp.c_str(), "wb" );
	if ( !f )
		return false;
	bool ok = fwrite( data, 1, size, f ) == size;
	ok = fclose( f ) == 0 && ok;
	if ( !ok || !P2Plat_Replace( tmp, path ) )
	{
		remove( tmp.c_str() );
		return false;
	}
	return true;
}

static bool WriteText( const std::string &path, const std::string &text )
{
	return WriteFileAtomic( path, text.data(), text.size() );
}

static bool Exists( const std::string &path ) { return P2Plat_FileSize( path ) >= 0; }
static long long FileSize( const std::string &path ) { return P2Plat_FileSize( path ); }
static void MakeDirs( const std::string &path ) { P2Plat_MakeDirs( path ); }

static std::string BaseName( const std::string &path )
{
	size_t s = path.rfind( '/' );
	return s == std::string::npos ? path : path.substr( s + 1 );
}

static std::string Stem( const std::string &file )
{
	size_t d = file.rfind( '.' );
	return d == std::string::npos || d == 0 ? file : file.substr( 0, d );
}

static std::string Lower( std::string s )
{
	for ( char &c : s )
		if ( c >= 'A' && c <= 'Z' )
			c += 'a' - 'A';
	return s;
}

static int FloorLog2( int v )
{
	int r = 0;
	while ( v > 1 )
	{
		v >>= 1;
		r++;
	}
	return r;
}

static int NextPow2( int v )
{
	int p = 1;
	while ( p < v )
		p <<= 1;
	return p;
}

static uint16_t LE16( const uint8_t *p ) { return p[0] | p[1] << 8; }
static uint32_t LE24( const uint8_t *p ) { return p[0] | p[1] << 8 | p[2] << 16; }
static uint32_t LE32( const uint8_t *p ) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t BE16( const uint8_t *p ) { return p[0] << 8 | p[1]; }
static uint32_t BE32( const uint8_t *p ) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

//-----------------------------------------------------------------------------
// VTF headers
//-----------------------------------------------------------------------------
struct VtfInfo
{
	int major = 0, minor = 0, headerSize = 0, width = 0, height = 0, frames = 1, depth = 1, format = -1, mips = 1;
	int lowFormat = -1, lowW = 0, lowH = 0;
	uint32_t flags = 0;
};

static bool ParseVtf( const uint8_t *d, size_t n, VtfInfo &v )
{
	if ( n < 64 || memcmp( d, "VTF\0", 4 ) )
		return false;
	v.major = LE32( d + 4 );
	v.minor = LE32( d + 8 );
	v.headerSize = LE32( d + 12 );
	v.width = LE16( d + 16 );
	v.height = LE16( d + 18 );
	v.flags = LE32( d + 20 );
	v.frames = std::max( 1, (int)LE16( d + 24 ) );
	v.format = (int)LE32( d + 52 );
	v.mips = d[56];
	v.lowFormat = (int)LE32( d + 57 );
	v.lowW = d[61];
	v.lowH = d[62];
	v.depth = v.minor >= 2 && n >= 65 ? std::max( 1, (int)LE16( d + 63 ) ) : 1;
	return v.major == 7 && v.minor <= 5 && v.width > 0 && v.height > 0;
}

static bool ReadVtfInfo( const std::string &path, VtfInfo &v )
{
	std::vector<uint8_t> d;
	return ReadFile( path, d, 80 ) && ParseVtf( d.data(), d.size(), v );
}

// what CS:S's VTF loader accepts (materialsystem refuses bigger: the texture would show as an error)
static bool EngineCanLoad( const VtfInfo &v )
{
	return FloorLog2( v.width ) + FloorLog2( v.height ) + FloorLog2( v.frames ) + FloorLog2( v.depth ) <= 25;
}

// 7.2 header, no low-res thumbnail: 80 bytes, then the image (one mip: frames x RGBA8888 rows)
static void WriteVtfHeader( uint8_t *h, int w, int hgt, int frames )
{
	memset( h, 0, 80 );
	memcpy( h, "VTF\0", 4 );
	h[4] = 7;
	h[8] = 2;
	h[12] = 80;
	h[16] = w & 255; h[17] = w >> 8;
	h[18] = hgt & 255; h[19] = hgt >> 8;
	uint32_t flags = 0x4 | 0x8 | 0x100 | 0x200 | 0x2000;	// clamp s / t, no mip (yet), no LOD, 8-bit alpha
	memcpy( h + 20, &flags, 4 );
	h[24] = frames & 255; h[25] = frames >> 8;
	float refl[3] = { 0.5f, 0.5f, 0.5f }, bump = 1.0f;
	memcpy( h + 32, refl, 12 );
	memcpy( h + 48, &bump, 4 );
	// +52 format 0 = RGBA8888
	h[56] = 1;							// mip count
	memset( h + 57, 0xff, 4 );			// low-res format: none
	h[63] = 1;							// depth
}

//-----------------------------------------------------------------------------
// image headers: size, frames and frame rate before decoding anything
//-----------------------------------------------------------------------------
struct ImageInfo { int w = 0, h = 0, frames = 1; float fps = 10.0f; bool known = false; };

static void ParseGif( const std::vector<uint8_t> &d, ImageInfo &info )
{
	size_t n = d.size(), p = 13;
	if ( n < 13 )
		return;
	info.w = LE16( &d[6] );
	info.h = LE16( &d[8] );
	info.known = info.w > 0 && info.h > 0;
	if ( d[10] & 0x80 )
		p += 3u << ( ( d[10] & 7 ) + 1 );
	int frames = 0;
	double delaySum = 0;
	int delays = 0;
	auto skipBlocks = [&]() { while ( p < n && d[p] ) p += d[p] + 1; p++; };
	while ( p < n )
	{
		uint8_t b = d[p++];
		if ( b == 0x21 && p < n )
		{
			uint8_t label = d[p++];
			if ( label == 0xF9 && p + 4 < n && d[p] == 4 )
			{
				int cs = LE16( &d[p + 2] );
				delaySum += cs < 2 ? 10 : cs;	// browsers play 0 / 1 as 10 (1/100 s)
				delays++;
			}
			skipBlocks();
		}
		else if ( b == 0x2C && p + 9 <= n )
		{
			frames++;
			uint8_t fl = d[p + 8];
			p += 9;
			if ( fl & 0x80 )
				p += 3u << ( ( fl & 7 ) + 1 );
			p++;	// LZW minimum code size
			skipBlocks();
		}
		else
			break;	// trailer (0x3B) or something we don't know
	}
	info.frames = std::max( 1, frames );
	if ( delays )
		info.fps = (float)( 100.0 / ( delaySum / delays ) );
}

static void ParseWebp( const std::vector<uint8_t> &d, ImageInfo &info )
{
	size_t n = d.size();
	if ( n < 30 )
		return;
	const uint8_t *c = &d[12];
	if ( !memcmp( c, "VP8 ", 4 ) )
	{
		info.w = LE16( &d[26] ) & 0x3FFF;
		info.h = LE16( &d[28] ) & 0x3FFF;
	}
	else if ( !memcmp( c, "VP8L", 4 ) )
	{
		const uint8_t *b = &d[21];
		info.w = 1 + ( ( b[0] | b[1] << 8 ) & 0x3FFF );
		info.h = 1 + ( ( b[1] >> 6 | b[2] << 2 | b[3] << 10 ) & 0x3FFF );
	}
	else if ( !memcmp( c, "VP8X", 4 ) )
	{
		info.w = 1 + LE24( &d[24] );
		info.h = 1 + LE24( &d[27] );
		if ( d[20] & 0x02 )	// animated: count ANMF chunks
		{
			int frames = 0;
			double ms = 0;
			for ( size_t p = 12; p + 8 <= n; )
			{
				uint32_t len = LE32( &d[p + 4] );
				if ( !memcmp( &d[p], "ANMF", 4 ) && p + 24 <= n )
				{
					frames++;
					uint32_t dur = LE24( &d[p + 8 + 12] );
					ms += dur ? dur : 100;
				}
				p += 8 + len + ( len & 1 );
			}
			if ( frames )
			{
				info.frames = frames;
				info.fps = (float)( 1000.0 / ( ms / frames ) );
			}
		}
	}
	info.known = info.w > 0 && info.h > 0;
}

static void ParseJpeg( const std::vector<uint8_t> &d, ImageInfo &info )
{
	size_t n = d.size(), p = 2;
	while ( p + 4 <= n )
	{
		if ( d[p] != 0xFF )
			return;
		uint8_t m = d[p + 1];
		p += 2;
		if ( m == 0xD8 || m == 0x01 || ( m >= 0xD0 && m <= 0xD7 ) || m == 0xFF )
		{
			if ( m == 0xFF )
				p--;
			continue;
		}
		uint16_t len = BE16( &d[p] );
		if ( m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC && p + 7 <= n )
		{
			info.h = BE16( &d[p + 3] );
			info.w = BE16( &d[p + 5] );
			info.known = info.w > 0 && info.h > 0;
			return;
		}
		p += len;
	}
}

static ImageInfo ProbeImage( const std::vector<uint8_t> &d )
{
	ImageInfo info;
	size_t n = d.size();
	if ( n >= 24 && !memcmp( d.data(), "\x89PNG\r\n\x1a\n", 8 ) )
	{
		info.w = BE32( &d[16] );
		info.h = BE32( &d[20] );
		info.known = info.w > 0 && info.h > 0;
	}
	else if ( n >= 13 && ( !memcmp( d.data(), "GIF87a", 6 ) || !memcmp( d.data(), "GIF89a", 6 ) ) )
		ParseGif( d, info );
	else if ( n >= 4 && d[0] == 0xFF && d[1] == 0xD8 )
		ParseJpeg( d, info );
	else if ( n >= 16 && !memcmp( d.data(), "RIFF", 4 ) && !memcmp( &d[8], "WEBP", 4 ) )
		ParseWebp( d, info );
	else if ( n >= 26 && d[0] == 'B' && d[1] == 'M' )
	{
		info.w = (int)LE32( &d[18] );
		info.h = abs( (int)LE32( &d[22] ) );
		info.known = info.w > 0 && info.h > 0;
	}
	info.fps = std::min( 50.0f, std::max( 1.0f, info.fps ) );
	return info;
}

//-----------------------------------------------------------------------------
// MareTF
//-----------------------------------------------------------------------------
static std::string FindMareTF() { return P2Plat_FindMareTF(); }

static bool RunMareTF( const std::vector<std::string> &args, const std::string &log )
{
	std::string exe = FindMareTF();
	return !exe.empty() && P2Plat_Run( exe, args, log );
}

//-----------------------------------------------------------------------------
// materials
//-----------------------------------------------------------------------------
static std::string ProxyBlock( int frames, float fps )
{
	if ( frames <= 1 )
		return "";
	char buf[256];
	snprintf( buf, sizeof( buf ), "\t\"Proxies\"\n\t{\n\t\t\"AnimatedTexture\"\n\t\t{\n\t\t\t\"animatedTextureVar\" \"$basetexture\"\n"
		"\t\t\t\"animatedTextureFrameNumVar\" \"$frame\"\n\t\t\t\"animatedTextureFrameRate\" \"%.2f\"\n\t\t}\n\t}\n", fps );
	return buf;
}

// the 8 decal sizes, the menu thumbnail and the aim ghost for one picture
static bool WritePictureMaterials( const std::string &name, const std::string &texture, int frames, float fps )
{
	std::string proxy = ProxyBlock( frames, fps ), base = s_PicDir + "/" + name;
	bool ok = true;
	for ( int i = 0; i < 8; i++ )
	{
		char vmt[512];
		snprintf( vmt, sizeof( vmt ), "\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"%s\"\n\t\"$decal\" 1\n\t\"$decalscale\" %.2f\n\t\"$translucent\" 1\n",
			texture.c_str(), P2_DECAL_SCALE[i] );
		ok &= WriteText( base + "_s" + std::to_string( i ) + ".vmt", vmt + proxy + "}\n" );
	}
	char vmt[512];
	snprintf( vmt, sizeof( vmt ), "\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"%s\"\n\t\"$translucent\" 1\n\t\"$vertexcolor\" 1\n\t\"$vertexalpha\" 1\n\t\"$ignorez\" 1\n",
		texture.c_str() );
	ok &= WriteText( base + ".vmt", vmt + proxy + "}\n" );
	snprintf( vmt, sizeof( vmt ), "\"UnlitGeneric\"\n{\n\t\"$basetexture\" \"%s\"\n\t\"$translucent\" 1\n\t\"$vertexcolor\" 1\n\t\"$vertexalpha\" 1\n\t\"$nocull\" 1\n",
		texture.c_str() );
	ok &= WriteText( base + "_g.vmt", vmt + proxy + "}\n" );
	return ok;
}

//-----------------------------------------------------------------------------
// the list
//-----------------------------------------------------------------------------
bool P2Pics_ValidName( const std::string &name )
{
	if ( name.empty() || name.size() > 47 )
		return false;
	for ( char c : name )
		if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-' ) )
			return false;
	return true;
}

// "Cat Pic (1).PNG" -> "cat_pic_1"
static std::string MakeName( const std::string &file )
{
	std::string s;
	for ( char c : Lower( Stem( BaseName( file ) ) ) )
	{
		bool ok = ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '-';
		if ( ok )
			s += c;
		else if ( !s.empty() && s.back() != '_' )
			s += '_';
	}
	while ( !s.empty() && s.back() == '_' )
		s.pop_back();
	if ( s.empty() )
		s = "picture";
	if ( s.compare( 0, 5, "logo_" ) == 0 )
		s = "pic_" + s;		// logo_ = the game's sprays
	s = s.substr( 0, 40 );
	std::string name = s;
	for ( int i = 2; Exists( s_PicDir + "/" + name + ".vtf" ) || Exists( s_PicDir + "/" + name + "_s0.vmt" ); i++ )
		name = s + "_" + std::to_string( i );
	return name;
}

void P2Pics_Init( const std::string &gameDir )
{
	s_GameDir = gameDir;
	s_PicDir = gameDir + "/materials/paintpics";
	s_LogoDir = gameDir + "/materials/vgui/logos";
	MakeDirs( s_PicDir );
}

static bool ByName( const P2Picture &a, const P2Picture &b ) { return a.name < b.name; }

void P2Pics_Refresh()
{
	std::vector<P2Picture> pics, sprays;
	std::vector<P2PlatEntry> entries;
	if ( P2Plat_ListDir( s_PicDir, entries ) )
	{
		for ( const P2PlatEntry &e : entries )
		{
			std::string f = e.name;
			if ( f.size() < 5 || Lower( f.substr( f.size() - 4 ) ) != ".vtf" )
				continue;
			P2Picture p;
			p.name = Stem( f );
			VtfInfo v;
			if ( !P2Pics_ValidName( p.name ) || p.name.compare( 0, 5, "logo_" ) == 0 || !ReadVtfInfo( s_PicDir + "/" + f, v ) )
				continue;
			if ( !Exists( s_PicDir + "/" + p.name + "_s0.vmt" ) )
				WritePictureMaterials( p.name, "paintpics/" + p.name, v.frames, 10.0f );	// materials went missing
			p.label = p.name;
			p.width = v.width;
			p.height = v.height;
			p.frames = v.frames;
			pics.push_back( p );
		}
	}
	if ( P2Plat_ListDir( s_LogoDir, entries ) )
	{
		for ( const P2PlatEntry &e : entries )
		{
			std::string f = e.name, stem = Stem( f );
			if ( f.size() < 5 || Lower( f.substr( f.size() - 4 ) ) != ".vtf" )
				continue;
			P2Picture p;
			p.name = "logo_" + Lower( stem );
			VtfInfo v;
			// (a spray whose file name isn't a plain word can't be named in a material path / console command)
			if ( !P2Pics_ValidName( p.name ) || !ReadVtfInfo( s_LogoDir + "/" + f, v ) || !EngineCanLoad( v ) )
				continue;
			// materials for a new spray, or a spray file that changed (frames decide the animation)
			long long made = P2Plat_MTime( s_PicDir + "/" + p.name + "_s0.vmt" );
			if ( made < 0 || e.mtime > made )
				WritePictureMaterials( p.name, "vgui/logos/" + stem, v.frames, SPRAY_FPS );
			p.label = stem;
			p.width = v.width;
			p.height = v.height;
			p.frames = v.frames;
			p.spray = true;
			sprays.push_back( p );
		}
	}
	std::sort( pics.begin(), pics.end(), ByName );
	std::sort( sprays.begin(), sprays.end(), ByName );
	pics.insert( pics.end(), sprays.begin(), sprays.end() );
	s_List.swap( pics );
}

const std::vector<P2Picture> &P2Pics_List() { return s_List; }

const P2Picture *P2Pics_Find( const std::string &name )
{
	for ( const P2Picture &p : s_List )
		if ( p.name == name )
			return &p;
	return NULL;
}

void P2Pics_DecalSize( int texW, int texH, int size, float *w, float *h )
{
	float ds = P2_DECAL_SCALE[std::min( 7, std::max( 0, size ) )];
	int m = std::max( texW / 64, texH / 64 );
	if ( m < 1 )
		m = 1;
	*w = texW * ds / m;
	*h = texH * ds / m;
}

//-----------------------------------------------------------------------------
// adding a file (background thread)
//-----------------------------------------------------------------------------
static void SetStatus( int state, const std::string &text, const std::string &added = "" )
{
	std::lock_guard<std::mutex> lock( s_StatusLock );
	s_Status.state = state;
	s_Status.text = text;
	s_Status.added = added;
	s_Status.serial++;
	if ( state != P2ADD_BUSY )
		s_bBusy = false;
}

P2AddStatus P2Pics_Status()
{
	std::lock_guard<std::mutex> lock( s_StatusLock );
	return s_Status;
}

// largest canvas (longer side L, power of two) and frame step that fit the caps
struct Plan { int w, h, cw, ch, step; };

static long long Dxt5WithMips( int w, int h )
{
	long long total = 0;
	for ( ;; )
	{
		total += (long long)( ( w + 3 ) / 4 ) * ( ( h + 3 ) / 4 ) * 16;
		if ( w == 1 && h == 1 )
			break;
		w = std::max( 1, w / 2 );
		h = std::max( 1, h / 2 );
	}
	return total;
}

static int MaxFrames( int cw, int ch )
{
	long long bytes = ( MAX_VTF_BYTES - 4096 ) / Dxt5WithMips( cw, ch );	// (header + resources slack)
	int k = 25 - FloorLog2( cw ) - FloorLog2( ch );
	long long rule = k < 0 ? 0 : k >= 30 ? ( 1LL << 30 ) : ( 1LL << ( k + 1 ) ) - 1;
	return (int)std::max( 0LL, std::min( std::min( bytes, rule ), 65535LL ) );
}

static Plan MakePlan( int srcW, int srcH, int frames )
{
	int longest = std::max( srcW, srcH );
	int top = std::max( MIN_SIDE, NextPow2( std::min( longest, MAX_SIDE ) ) );	// (capped first: a huge header size overflowed NextPow2)
	int bottom = frames > 1 ? std::min( top, MIN_ANIM_SIDE ) : top;
	Plan p = {};
	for ( int L = top; ; L /= 2 )
	{
		p.w = srcW >= srcH ? L : std::max( 1, (int)lround( (double)L * srcW / srcH ) );
		p.h = srcW >= srcH ? std::max( 1, (int)lround( (double)L * srcH / srcW ) ) : L;
		p.cw = NextPow2( p.w );
		p.ch = NextPow2( p.h );
		int maxF = MaxFrames( p.cw, p.ch );
		p.step = maxF > 0 ? ( frames + maxF - 1 ) / maxF : frames;
		if ( p.step <= 1 || L <= bottom )
			break;
	}
	p.step = std::max( 1, p.step );
	return p;
}

struct AddJob { std::string path; };

static bool ConvertImage( const std::string &in, const std::string &name, const std::string &tmp, std::string &err )
{
	std::vector<uint8_t> file;
	if ( !ReadFile( in, file ) )
	{
		err = "can't read the file";
		return false;
	}
	ImageInfo info = ProbeImage( file );
	file.clear();
	file.shrink_to_fit();
	std::string log = tmp + "/maretf.log";
	remove( log.c_str() );
	if ( !info.known )
	{
		// a format we can't read the size of: let MareTF decode it once at full size to find out
		std::string probe = tmp + "/probe.vtf";
		if ( !RunMareTF( { "create", "-y", "-v", "7.2", "-f", "RGBA8888", "--no-mips", "--no-thumbnail", "--resize-method", "NONE", "-i", in, "-o", probe }, log ) )
		{
			err = "MareTF can't read this file";
			return false;
		}
		VtfInfo v;
		if ( !ReadVtfInfo( probe, v ) )
		{
			err = "MareTF made no picture";
			return false;
		}
		info.w = v.width;
		info.h = v.height;
		info.frames = v.frames;
		info.known = true;
		remove( probe.c_str() );
	}
	Plan plan = MakePlan( info.w, info.h, info.frames );

	// 1. decode at the final size
	std::string dec = tmp + "/decoded.vtf";
	if ( !RunMareTF( { "create", "-y", "-v", "7.2", "-f", "RGBA8888", "--no-mips", "--no-thumbnail", "--resize-method", "NONE",
					   "--width", std::to_string( plan.w ), "--height", std::to_string( plan.h ), "-i", in, "-o", dec }, log ) )
	{
		err = FindMareTF().empty() ? "MareTF isn't installed" : "MareTF can't read this file";
		return false;
	}
	std::vector<uint8_t> d;
	VtfInfo v;
	if ( !ReadFile( dec, d ) || !ParseVtf( d.data(), d.size(), v ) || v.format != 0 || v.width != plan.w || v.height != plan.h )
	{
		err = "MareTF made something unexpected";
		return false;
	}
	remove( dec.c_str() );
	size_t frameBytes = (size_t)plan.w * plan.h * 4;
	size_t offset = v.headerSize;
	if ( v.lowW && v.lowH && v.lowFormat == 13 )	// a DXT1 thumbnail (we asked for none)
		offset += (size_t)( ( v.lowW + 3 ) / 4 ) * ( ( v.lowH + 3 ) / 4 ) * 8;
	int frames = v.frames;
	if ( d.size() < offset + frameBytes * frames )
	{
		err = "MareTF's picture is cut short";
		return false;
	}
	// the real frame count can differ from the header's guess (animated PNG / WebP)
	int maxF = MaxFrames( plan.cw, plan.ch );
	int step = std::max( plan.step, maxF > 0 ? ( frames + maxF - 1 ) / maxF : frames );
	int outFrames = ( frames + step - 1 ) / step;

	// 2. centre it on the power-of-two canvas
	std::vector<uint8_t> pad( 80 + (size_t)plan.cw * plan.ch * 4 * outFrames, 0 );
	WriteVtfHeader( pad.data(), plan.cw, plan.ch, outFrames );
	int ox = ( plan.cw - plan.w ) / 2, oy = ( plan.ch - plan.h ) / 2;
	for ( int f = 0; f < outFrames; f++ )
	{
		const uint8_t *src = &d[offset + frameBytes * ( (size_t)f * step )];
		uint8_t *dst = &pad[80 + (size_t)plan.cw * plan.ch * 4 * f];
		for ( int y = 0; y < plan.h; y++ )
			memcpy( dst + ( (size_t)( y + oy ) * plan.cw + ox ) * 4, src + (size_t)y * plan.w * 4, (size_t)plan.w * 4 );
	}
	d.clear();
	d.shrink_to_fit();
	std::string padded = tmp + "/canvas.vtf";
	if ( !WriteFileAtomic( padded, pad.data(), pad.size() ) )
	{
		err = "can't write a temporary file";
		return false;
	}
	pad.clear();
	pad.shrink_to_fit();

	// 3. compress, mipmaps
	std::string out = tmp + "/out.vtf";
	if ( !RunMareTF( { "edit", "-y", "--set-format", "DXT5", "--recompute-mips", "--add-flag", "CLAMP_S", "--add-flag", "CLAMP_T", "-o", out, padded }, log ) )
	{
		err = "MareTF couldn't compress it";
		return false;
	}
	remove( padded.c_str() );
	VtfInfo o;
	long long size = FileSize( out );
	if ( !ReadVtfInfo( out, o ) || o.width != plan.cw || o.height != plan.ch || o.frames != outFrames || !EngineCanLoad( o ) )
	{
		err = "MareTF's result looks wrong";
		return false;
	}
	if ( size > MAX_VTF_BYTES )
	{
		err = "came out bigger than 16 MB";
		return false;
	}
	if ( !P2Plat_Replace( out, s_PicDir + "/" + name + ".vtf" ) )
	{
		err = "can't write to materials/paintpics";
		return false;
	}
	float fps = std::max( 1.0f, info.fps / step );
	if ( !WritePictureMaterials( name, "paintpics/" + name, outFrames, fps ) )
	{
		err = "can't write the materials";
		return false;
	}
	return true;
}

static void AddThread( void *arg )
{
	AddJob *job = (AddJob *)arg;
	std::string file = BaseName( job->path ), err;
	std::string name = MakeName( file );
	// temporary files next to the pictures: same drive, so the result can be renamed into place
	std::string tmp = s_PicDir + "/.convert";
	MakeDirs( tmp );

	bool ok = false;
	std::vector<uint8_t> head;
	if ( !ReadFile( job->path, head, 80 ) )
		err = "can't read the file";
	else if ( head.size() >= 4 && !memcmp( head.data(), "VTF\0", 4 ) )
	{
		// already a texture: take it as it is
		VtfInfo v;
		long long size = FileSize( job->path );
		std::vector<uint8_t> all;
		if ( !ParseVtf( head.data(), head.size(), v ) )
			err = "not a VTF the game can read";
		else if ( !EngineCanLoad( v ) )
			err = "too many frames for its size (the game refuses it)";
		else if ( size > MAX_VTF_BYTES )
			err = "bigger than 16 MB";
		else if ( !ReadFile( job->path, all ) || !WriteFileAtomic( s_PicDir + "/" + name + ".vtf", all.data(), all.size() ) )
			err = "can't copy it to materials/paintpics";
		else
			ok = WritePictureMaterials( name, "paintpics/" + name, v.frames, SPRAY_FPS ) || ( err = "can't write the materials", false );
	}
	else
		ok = ConvertImage( job->path, name, tmp, err );

	if ( ok )
	{
		remove( ( tmp + "/maretf.log" ).c_str() );	// (kept after a failure: what MareTF said)
		SetStatus( P2ADD_DONE, "added " + name, name );
	}
	else
		SetStatus( P2ADD_FAILED, file + ": " + err );
	delete job;
}

bool P2Pics_Add( const std::string &path )
{
	{
		std::lock_guard<std::mutex> lock( s_StatusLock );
		if ( s_bBusy )
			return false;
		s_bBusy = true;
		s_Status.state = P2ADD_BUSY;
		s_Status.text = "adding " + BaseName( path ) + " ...";
		s_Status.added.clear();
		s_Status.serial++;
	}
	AddJob *job = new AddJob{ path };
	if ( !P2Plat_StartThread( AddThread, job ) )
	{
		delete job;
		SetStatus( P2ADD_FAILED, "couldn't start" );
		return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// the file picker
//-----------------------------------------------------------------------------
bool P2Pics_IsAddable( const std::string &fileName )
{
	static const char *const ext[] = { ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".tga", ".psd", ".vtf", ".apng", ".qoi" };
	std::string f = Lower( fileName );
	for ( const char *e : ext )
	{
		size_t n = strlen( e );
		if ( f.size() > n && !f.compare( f.size() - n, n, e ) )
			return true;
	}
	return false;
}

static bool DirFirst( const P2DirEntry &a, const P2DirEntry &b )
{
	if ( a.dir != b.dir )
		return a.dir;
	return Lower( a.name ) < Lower( b.name );
}

bool P2Pics_ListDir( const std::string &dir, std::vector<P2DirEntry> &out )
{
	out.clear();
	std::vector<P2PlatEntry> entries;
	if ( !P2Plat_ListDir( dir, entries ) )
		return false;
	for ( const P2PlatEntry &e : entries )
		if ( e.dir || P2Pics_IsAddable( e.name ) )
		{
			P2DirEntry entry;
			entry.name = e.name;
			entry.dir = e.dir;
			out.push_back( entry );
		}
	std::sort( out.begin(), out.end(), DirFirst );
	return true;
}
