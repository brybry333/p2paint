//=============================================================================
// p2paint: p2paint_platform.h for Windows (p2paint.dll). Whatever LoadLibrary's us (bhoptools.exe's code, or
// tools/wininject.exe for tests) is all the loading there is: DllMain starts a thread that patches the engine as
// soon as engine.dll is in, then runs the rest of the start on the game's main thread (P2Paint_MainInit).
//=============================================================================
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "p2paint_platform.h"

void P2Paint_PatchEngine();		// p2paint_runtime.cpp
void P2Paint_MainInit();

void *P2Plat_Factory( const char *module )
{
	char file[64];
	snprintf( file, sizeof( file ), "%s.dll", module );
	HMODULE h = GetModuleHandleA( file );
	return h ? (void *)GetProcAddress( h, "CreateInterface" ) : NULL;
}

void *P2Plat_Interface( const char *module, const char *name )
{
	typedef void *( *Fn_t )( const char *, int * );
	Fn_t fn = (Fn_t)P2Plat_Factory( module );
	return fn ? fn( name, NULL ) : NULL;
}

bool P2Plat_EngineCode( unsigned char **base, size_t *size )
{
	*base = NULL;
	*size = 0;
	HMODULE h = GetModuleHandleA( "engine.dll" );
	if ( !h )
		return false;
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)h;
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)( (const unsigned char *)h + dos->e_lfanew );
	const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION( nt );
	for ( int i = 0; i < nt->FileHeader.NumberOfSections; i++ )
		if ( !memcmp( sec[i].Name, ".text", 6 ) )
		{
			*base = (unsigned char *)h + sec[i].VirtualAddress;
			*size = sec[i].Misc.VirtualSize;
			return true;
		}
	return false;
}

bool P2Plat_WriteCode( unsigned char *dst, const unsigned char *src, size_t n )
{
	DWORD old;
	if ( !VirtualProtect( dst, n, PAGE_EXECUTE_READWRITE, &old ) )
		return false;
	memcpy( dst, src, n );
	VirtualProtect( dst, n, old, &old );
	FlushInstructionCache( GetCurrentProcess(), dst, n );
	return true;
}

static long long FileTimeToUnix( const FILETIME &ft )
{
	ULARGE_INTEGER t;
	t.LowPart = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	return (long long)( t.QuadPart / 10000000ULL ) - 11644473600LL;
}

bool P2Plat_ListDir( const std::string &dir, std::vector<P2PlatEntry> &out )
{
	out.clear();
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA( ( dir + "/*" ).c_str(), &fd );
	if ( h == INVALID_HANDLE_VALUE )
		return false;
	do
	{
		if ( fd.cFileName[0] != '.' )
			out.push_back( { fd.cFileName, ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) != 0, FileTimeToUnix( fd.ftLastWriteTime ) } );
	} while ( FindNextFileA( h, &fd ) );
	FindClose( h );
	return true;
}

bool P2Plat_IsDir( const std::string &path )
{
	DWORD a = GetFileAttributesA( path.c_str() );
	return a != INVALID_FILE_ATTRIBUTES && ( a & FILE_ATTRIBUTE_DIRECTORY );
}

static bool Attributes( const std::string &path, WIN32_FILE_ATTRIBUTE_DATA &a )
{
	return GetFileAttributesExA( path.c_str(), GetFileExInfoStandard, &a ) != 0;
}

long long P2Plat_FileSize( const std::string &path )
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	return Attributes( path, a ) ? (long long)a.nFileSizeHigh << 32 | a.nFileSizeLow : -1;
}

long long P2Plat_MTime( const std::string &path )
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	return Attributes( path, a ) ? FileTimeToUnix( a.ftLastWriteTime ) : -1;
}

void P2Plat_MakeDirs( const std::string &path )
{
	for ( size_t i = 1; i <= path.size(); i++ )
		if ( i == path.size() || ( ( path[i] == '/' || path[i] == '\\' ) && path[i - 1] != ':' ) )
			CreateDirectoryA( path.substr( 0, i ).c_str(), NULL );
}

bool P2Plat_Replace( const std::string &from, const std::string &to )
{
	return MoveFileExA( from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING ) != 0;
}

static std::string Slashes( std::string s )
{
	for ( char &c : s )
		if ( c == '\\' )
			c = '/';
	return s;
}

std::string P2Plat_Home()
{
	const char *h = getenv( "USERPROFILE" );
	return h && h[0] ? Slashes( h ) : "C:/";
}

std::string P2Plat_CacheDir()
{
	const char *t = getenv( "TEMP" );
	std::string d = Slashes( t && t[0] ? t : "." ) + "/p2paint";
	P2Plat_MakeDirs( d );
	return d;
}

std::string P2Plat_Root( const std::string &path )
{
	return path.size() >= 2 && path[1] == ':' ? path.substr( 0, 2 ) + "/" : "/";
}

static std::string OwnDir()
{
	char buf[MAX_PATH];
	HMODULE self = NULL;
	GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(void *)&OwnDir, &self );
	DWORD n = GetModuleFileNameA( self, buf, sizeof( buf ) );
	std::string p = Slashes( std::string( buf, n ) );
	return p.substr( 0, p.rfind( '/' ) + 1 );
}

std::string P2Plat_FindMareTF()
{
	std::string p = OwnDir() + "maretf.exe";
	if ( GetFileAttributesA( p.c_str() ) != INVALID_FILE_ATTRIBUTES )
		return p;
	char found[MAX_PATH];
	return SearchPathA( NULL, "maretf.exe", NULL, sizeof( found ), found, NULL ) ? Slashes( found ) : "";
}

// one command-line argument the way CommandLineToArgvW / the CRT read it back
static void Quote( std::string &cmd, const std::string &a )
{
	cmd += '"';
	int slashes = 0;
	for ( char c : a )
	{
		if ( c == '\\' )
			slashes++;
		else
		{
			if ( c == '"' )
				cmd.append( slashes + 1, '\\' );
			slashes = 0;
		}
		cmd += c;
	}
	cmd.append( slashes, '\\' );
	cmd += "\" ";
}

bool P2Plat_Run( const std::string &exe, const std::vector<std::string> &args, const std::string &log )
{
	std::string cmd;
	Quote( cmd, exe );
	for ( const std::string &a : args )
		Quote( cmd, a );
	SECURITY_ATTRIBUTES sa = { sizeof( sa ), NULL, TRUE };
	HANDLE out = CreateFileA( log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
	STARTUPINFOA si = { sizeof( si ) };
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = NULL;
	si.hStdOutput = si.hStdError = out;
	PROCESS_INFORMATION pi;
	BOOL ok = CreateProcessA( NULL, &cmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi );
	if ( out != INVALID_HANDLE_VALUE )
		CloseHandle( out );
	if ( !ok )
		return false;
	WaitForSingleObject( pi.hProcess, INFINITE );
	DWORD code = 1;
	GetExitCodeProcess( pi.hProcess, &code );
	CloseHandle( pi.hProcess );
	CloseHandle( pi.hThread );
	return code == 0;
}

struct Thread_t { void ( *fn )( void * ); void *arg; };

static DWORD WINAPI ThreadMain( LPVOID p )
{
	Thread_t t = *(Thread_t *)p;
	delete (Thread_t *)p;
	t.fn( t.arg );
	return 0;
}

bool P2Plat_StartThread( void ( *fn )( void * ), void *arg )
{
	Thread_t *t = new Thread_t{ fn, arg };
	HANDLE h = CreateThread( NULL, 0, ThreadMain, t, 0, NULL );
	if ( !h )
	{
		delete t;
		return false;
	}
	CloseHandle( h );
	return true;
}

//-----------------------------------------------------------------------------
// Starting: the main thread from ours through a one-shot message hook on the game window's thread (SDL pumps its
// messages there every frame). No game code is hooked. (Same as triggerlook / CS:TV.)
//-----------------------------------------------------------------------------
static volatile LONG s_bMainPending;
static HHOOK s_hHook;

static LRESULT CALLBACK MessageHook( int code, WPARAM wParam, LPARAM lParam )
{
	if ( InterlockedExchange( &s_bMainPending, 0 ) )
		P2Paint_MainInit();
	HHOOK h = s_hHook;
	LRESULT r = CallNextHookEx( h, code, wParam, lParam );
	if ( h && !s_bMainPending )
	{
		s_hHook = NULL;
		UnhookWindowsHookEx( h );
	}
	return r;
}

struct FindWindow_t { DWORD pid, tid; };

static BOOL CALLBACK FindGameWindow( HWND hwnd, LPARAM lp )
{
	FindWindow_t *f = (FindWindow_t *)lp;
	DWORD pid = 0, tid = GetWindowThreadProcessId( hwnd, &pid );
	if ( pid != f->pid || !IsWindowVisible( hwnd ) || GetWindow( hwnd, GW_OWNER ) )
		return TRUE;
	f->tid = tid;
	return FALSE;
}

static DWORD WINAPI StartThread( LPVOID )
{
	// the engine patches as soon as engine.dll is in: before the first map sizes the decal pool
	for ( int i = 0; i < 1200 && !GetModuleHandleA( "engine.dll" ); i++ )
		Sleep( 100 );
	P2Paint_PatchEngine();
	s_bMainPending = 1;
	HMODULE self = NULL;
	GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)&MessageHook, &self );
	for ( int i = 0; i < 1200 && s_bMainPending; i++ )	// up to 2 minutes for the window
	{
		FindWindow_t f = { GetCurrentProcessId(), 0 };
		EnumWindows( FindGameWindow, (LPARAM)&f );
		if ( f.tid && ( s_hHook = SetWindowsHookExW( WH_GETMESSAGE, MessageHook, self, f.tid ) ) )
		{
			PostThreadMessageW( f.tid, WM_NULL, 0, 0 );	// (a message so the hook runs even if the game is idle)
			return 0;
		}
		Sleep( 100 );
	}
	return 0;
}

// not in DllMain itself (loader lock): a thread that runs right after it
BOOL WINAPI DllMain( HINSTANCE hinst, DWORD reason, LPVOID )
{
	if ( reason == DLL_PROCESS_ATTACH )
	{
		DisableThreadLibraryCalls( hinst );
		if ( HANDLE t = CreateThread( NULL, 0, StartThread, NULL, 0, NULL ) )
			CloseHandle( t );
	}
	return TRUE;
}
#endif
