//=============================================================================
// p2paint: what differs between Linux and Windows. Plain C++ only (windows.h and the Source SDK headers don't
// mix): p2paint_platform_linux.cpp / p2paint_platform_win.cpp. Paths use / on both (Windows takes it).
//=============================================================================
#pragma once
#include <stddef.h>
#include <string>
#include <vector>

#ifdef _WIN32
#define P2_EXPORT extern "C" __declspec( dllexport )
#else
#define P2_EXPORT extern "C" __attribute__( ( visibility( "default" ), used ) )
#endif

// a game module's CreateInterface / an interface from it: module = "engine", "client", "materialsystem", "vgui2",
// "vguimatsurface", "inputsystem", "filesystem_stdio", "vstdlib"
void *P2Plat_Factory( const char *module );
void *P2Plat_Interface( const char *module, const char *name );

// engine.so / engine.dll code, for the in-memory patches (p2paint_enginepatch.cpp); false = not loaded (yet)
bool P2Plat_EngineCode( unsigned char **base, size_t *size );
bool P2Plat_WriteCode( unsigned char *dst, const unsigned char *src, size_t n );

struct P2PlatEntry { std::string name; bool dir; long long mtime; };
// the entries of a folder (no "." ones); false = can't read it
bool P2Plat_ListDir( const std::string &dir, std::vector<P2PlatEntry> &out );
bool P2Plat_IsDir( const std::string &path );
long long P2Plat_FileSize( const std::string &path );	// -1 = no such file
long long P2Plat_MTime( const std::string &path );		// -1 = no such file
void P2Plat_MakeDirs( const std::string &path );
bool P2Plat_Replace( const std::string &from, const std::string &to );	// rename, over an existing file too

std::string P2Plat_Home();			// ~ | %USERPROFILE%
std::string P2Plat_CacheDir();		// ~/.cache/p2paint | %TEMP%/p2paint (created)
// the drive / root a path starts on ("/" | "C:/"): the file picker's UP stops there
std::string P2Plat_Root( const std::string &path );

// MareTF (pictures): its path, "" = not found. Linux: the host's (the game runs in Steam's container);
// Windows: maretf.exe next to our DLL or on PATH.
std::string P2Plat_FindMareTF();
// runs exe with args, its output appended to log; true = exit code 0
bool P2Plat_Run( const std::string &exe, const std::vector<std::string> &args, const std::string &log );

bool P2Plat_StartThread( void ( *fn )( void * ), void *arg );	// detached
