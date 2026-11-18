//=============================================================================
// p2paint: p2paint_platform.h for Linux (game_shader_generic_p2paint.so, started by the libp2paint.so loader)
//=============================================================================
#ifndef _WIN32
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "p2paint_platform.h"

extern char **environ;

struct FindModule_t { std::string suffix; char path[1024]; bool found; };

static int FindModuleCB( struct dl_phdr_info *info, size_t, void *data )
{
	FindModule_t *m = (FindModule_t *)data;
	size_t n = strlen( info->dlpi_name ), s = m->suffix.size();
	if ( n < s || strcmp( info->dlpi_name + n - s, m->suffix.c_str() ) )
		return 0;
	snprintf( m->path, sizeof( m->path ), "%s", info->dlpi_name );
	m->found = true;
	return 1;
}

void *P2Plat_Factory( const char *module )
{
	// (client.so: the game's, not another one with that name)
	FindModule_t m = { !strcmp( module, "client" ) ? "/cstrike/bin/linux64/client.so"
					   : !strcmp( module, "vstdlib" ) ? "/bin/linux64/libvstdlib.so"
					   : std::string( "/bin/linux64/" ) + module + ".so", "", false };
	dl_iterate_phdr( FindModuleCB, &m );
	if ( !m.found )
		return NULL;
	void *h = dlopen( m.path, RTLD_NOW | RTLD_NOLOAD );
	if ( !h )
		return NULL;
	void *fn = dlsym( h, "CreateInterface" );
	dlclose( h );	// NOLOAD handle; the module stays loaded
	return fn;
}

void *P2Plat_Interface( const char *module, const char *name )
{
	typedef void *( *Fn_t )( const char *, int * );
	Fn_t fn = (Fn_t)P2Plat_Factory( module );
	return fn ? fn( name, NULL ) : NULL;
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

bool P2Plat_EngineCode( unsigned char **base, size_t *size )
{
	Code_t code = { NULL, 0 };
	dl_iterate_phdr( FindEngineCode, &code );
	*base = code.base;
	*size = code.size;
	return code.base != NULL;
}

// code pages are read + execute: open them for the store, then close them again
bool P2Plat_WriteCode( unsigned char *dst, const unsigned char *src, size_t n )
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

bool P2Plat_ListDir( const std::string &dir, std::vector<P2PlatEntry> &out )
{
	out.clear();
	DIR *d = opendir( dir.c_str() );
	if ( !d )
		return false;
	while ( dirent *e = readdir( d ) )
	{
		if ( e->d_name[0] == '.' )
			continue;
		struct stat st;
		if ( stat( ( dir + "/" + e->d_name ).c_str(), &st ) != 0 )
			continue;
		out.push_back( { e->d_name, S_ISDIR( st.st_mode ), (long long)st.st_mtime } );
	}
	closedir( d );
	return true;
}

bool P2Plat_IsDir( const std::string &path )
{
	struct stat st;
	return stat( path.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
}

long long P2Plat_FileSize( const std::string &path )
{
	struct stat st;
	return stat( path.c_str(), &st ) == 0 ? (long long)st.st_size : -1;
}

long long P2Plat_MTime( const std::string &path )
{
	struct stat st;
	return stat( path.c_str(), &st ) == 0 ? (long long)st.st_mtime : -1;
}

void P2Plat_MakeDirs( const std::string &path )
{
	for ( size_t i = 1; i <= path.size(); i++ )
		if ( i == path.size() || path[i] == '/' )
			mkdir( path.substr( 0, i ).c_str(), 0755 );
}

bool P2Plat_Replace( const std::string &from, const std::string &to ) { return rename( from.c_str(), to.c_str() ) == 0; }

std::string P2Plat_Home()
{
	const char *h = getenv( "HOME" );
	return h ? h : "/";
}

std::string P2Plat_CacheDir()
{
	std::string d = P2Plat_Home() + "/.cache/p2paint";
	P2Plat_MakeDirs( d );
	return d;
}

std::string P2Plat_Root( const std::string & ) { return "/"; }

std::string P2Plat_FindMareTF()
{
	static const char *const paths[] = { "/run/host/usr/sbin/maretf", "/run/host/usr/bin/maretf", "/run/host/usr/local/bin/maretf",
		"/usr/sbin/maretf", "/usr/bin/maretf", "/usr/local/bin/maretf" };
	for ( const char *p : paths )
		if ( access( p, X_OK ) == 0 )
			return p;
	std::string p = P2Plat_Home() + "/.local/bin/maretf";
	return access( p.c_str(), X_OK ) == 0 ? p : "";
}

// with the game's environment minus its preloads
bool P2Plat_Run( const std::string &exe, const std::vector<std::string> &args, const std::string &log )
{
	std::vector<char *> argv;
	argv.push_back( (char *)exe.c_str() );
	for ( const std::string &a : args )
		argv.push_back( (char *)a.c_str() );
	argv.push_back( NULL );
	std::vector<char *> envp;
	for ( char **e = environ; *e; e++ )
		if ( strncmp( *e, "LD_PRELOAD=", 11 ) )
			envp.push_back( *e );
	envp.push_back( NULL );

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init( &fa );
	posix_spawn_file_actions_addopen( &fa, 0, "/dev/null", O_RDONLY, 0 );
	posix_spawn_file_actions_addopen( &fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644 );
	posix_spawn_file_actions_adddup2( &fa, 1, 2 );
	pid_t pid;
	int rc = posix_spawn( &pid, exe.c_str(), &fa, NULL, argv.data(), envp.data() );
	posix_spawn_file_actions_destroy( &fa );
	if ( rc != 0 )
		return false;
	int status = 0;
	while ( waitpid( pid, &status, 0 ) < 0 && errno == EINTR )
		;
	return WIFEXITED( status ) && WEXITSTATUS( status ) == 0;
}

struct Thread_t { void ( *fn )( void * ); void *arg; };

static void *ThreadMain( void *p )
{
	Thread_t t = *(Thread_t *)p;
	delete (Thread_t *)p;
	t.fn( t.arg );
	return NULL;
}

bool P2Plat_StartThread( void ( *fn )( void * ), void *arg )
{
	Thread_t *t = new Thread_t{ fn, arg };
	pthread_t id;
	pthread_attr_t attr;
	pthread_attr_init( &attr );
	pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
	int rc = pthread_create( &id, &attr, ThreadMain, t );
	pthread_attr_destroy( &attr );
	if ( rc != 0 )
		delete t;
	return rc == 0;
}
#endif
