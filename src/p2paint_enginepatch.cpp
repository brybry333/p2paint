//=============================================================================
// p2paint: two engine.so code patches paint needs, made in memory when we load (code that is already
// patched in the file is left alone):
//   decals      R_DecalInit sizes the decal pool from r_decals' DEFAULT string, never its value: always 2048
//               decals. -> 32768 (slots are 16-bit; nothing walks the pool per frame).
//   sprayscale  R_DecalCreate forces decals shot as sprays to scale 1.0, so every paint size would be 64 units.
//               -> keep the material's $decalscale (stock spray materials have none: real sprays don't change).
// Found in CS:S build 11003710 bin/linux64/engine.so (objdump, 2026-10-27). A game update that moves this code
// makes the scan miss: the log says NOT FOUND and paint still works, with 2048 decals and one size.
// Plain C++ / POSIX: tools/patchtest.cpp runs it on a copy of engine.so.
//=============================================================================
#include <link.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "p2paint_enginepatch.h"

struct Edit_t { int at; const char *bytes; };
struct Patch_t { const char *name, *pattern; Edit_t edits[2]; };	// pattern: hex bytes, "??" = any

static const Patch_t s_Patches[] =
{
	// mov $0x40,%ebx ... cmp $0x40,%eax; cmovge %eax,%ebx  ->  mov $0x8000,%ebx ... 3-byte nop
	{ "decals", "bb 40 00 00 00 48 83 ec 08 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 89 c7 e8 ?? ?? ?? ?? 44 8b 05 ?? ?? ?? ?? "
				"c7 05 ?? ?? ?? ?? 00 00 00 00 c7 05 ?? ?? ?? ?? 00 00 00 00 83 f8 40 0f 4d d8",
	  { { 1, "00 80 00 00" }, { 59, "0f 1f 00" } } },
	// and $0x10,%dh; je; movl $0,0x50(%r14); movl $1.0,0x44(%r14)  ->  the last store becomes an 8-byte nop
	{ "sprayscale", "80 e6 10 74 10 41 c7 46 50 00 00 00 00 41 c7 46 44 00 00 80 3f",
	  { { 13, "0f 1f 84 00 00 00 00 00" }, { -1, NULL } } },
};

// "bb 40 ?? 00" -> bytes, -1 = any
static std::vector<int> Parse( const char *hex )
{
	std::vector<int> out;
	for ( const char *p = hex; *p; )
	{
		if ( *p == ' ' ) { p++; continue; }
		out.push_back( p[0] == '?' ? -1 : (int)strtol( std::string( p, 2 ).c_str(), NULL, 16 ) );
		p += 2;
	}
	return out;
}

static int Count( const unsigned char *base, size_t size, const std::vector<int> &pat, size_t *where )
{
	int n = 0;
	for ( size_t i = 0; i + pat.size() <= size; i++ )
	{
		size_t k = 0;
		while ( k < pat.size() && ( pat[k] < 0 || pat[k] == base[i + k] ) )
			k++;
		if ( k == pat.size() )
		{
			*where = i;
			n++;
		}
	}
	return n;
}

std::string P2Patch_Buffer( unsigned char *base, size_t size, bool ( *write )( unsigned char *dst, const unsigned char *src, size_t n ) )
{
	std::string log;
	for ( const Patch_t &p : s_Patches )
	{
		std::vector<int> orig = Parse( p.pattern ), done = orig;
		for ( const Edit_t &e : p.edits )
			if ( e.bytes )
			{
				std::vector<int> b = Parse( e.bytes );
				for ( size_t k = 0; k < b.size(); k++ )
					done[e.at + k] = b[k];
			}
		size_t at = 0, atDone = 0;
		int nOrig = Count( base, size, orig, &at ), nDone = Count( base, size, done, &atDone );
		const char *result = "NOT FOUND";
		if ( nDone == 1 && nOrig == 0 )
			result = "already patched";
		else if ( nOrig == 1 && nDone == 0 )
		{
			result = "patched";
			for ( const Edit_t &e : p.edits )
			{
				if ( !e.bytes )
					continue;
				std::vector<int> b = Parse( e.bytes );
				std::vector<unsigned char> bytes( b.begin(), b.end() );
				if ( !write( base + at + e.at, bytes.data(), bytes.size() ) )
					result = "WRITE FAILED";
			}
		}
		log += std::string( p.name ) + " " + result + "\n";
	}
	return log;
}

// code pages are read + execute: open them for the store, then close them again
static bool WriteCode( unsigned char *dst, const unsigned char *src, size_t n )
{
	long page = sysconf( _SC_PAGESIZE );
	unsigned char *start = (unsigned char *)( (uintptr_t)dst & ~( (uintptr_t)page - 1 ) );
	size_t len = dst + n - start;
	if ( mprotect( start, len, PROT_READ | PROT_WRITE | PROT_EXEC ) )
		return false;
	memcpy( dst, src, n );
	mprotect( start, len, PROT_READ | PROT_EXEC );
	return true;
}

struct Code_t { unsigned char *base; size_t size; };

static int FindEngineCode( struct dl_phdr_info *info, size_t, void *data )
{
	static const char suffix[] = "/bin/linux64/engine.so";
	size_t n = strlen( info->dlpi_name ), s = sizeof( suffix ) - 1;
	if ( n < s || strcmp( info->dlpi_name + n - s, suffix ) )
		return 0;
	for ( int i = 0; i < info->dlpi_phnum; i++ )
	{
		const ElfW( Phdr ) &ph = info->dlpi_phdr[i];
		if ( ph.p_type == PT_LOAD && ( ph.p_flags & PF_X ) )
		{
			Code_t *c = (Code_t *)data;
			c->base = (unsigned char *)( info->dlpi_addr + ph.p_vaddr );
			c->size = ph.p_memsz;
			return 1;
		}
	}
	return 0;
}

std::string P2Patch_Engine()
{
	Code_t code = { NULL, 0 };
	dl_iterate_phdr( FindEngineCode, &code );
	if ( !code.base )
		return "engine.so code not found\n";
	return P2Patch_Buffer( code.base, code.size, WriteCode );
}
