//=============================================================================
// p2paint pictures: what you can paint with besides colours.
//
//   materials/paintpics/<name>.vtf       a picture added in game (ADD A FILE), made with MareTF
//   materials/paintpics/<name>_s0..7.vmt the decal in the 8 paint sizes      (paintpics/<name>_sN)
//   materials/paintpics/<name>.vmt       the menu thumbnail                   (paintpics/<name>)
//   materials/paintpics/<name>_g.vmt     the aim ghost (both sides, see-through)
// The game's sprays (materials/vgui/logos/*.vtf) are pictures too: "logo_<spray>", same materials, pointing
// at the spray's own texture (nothing is copied).
//
// Plain C++ / POSIX only (no engine calls), so tools can build it outside the game.
//=============================================================================
#ifndef P2PAINT_PICS_H
#define P2PAINT_PICS_H

#include <string>
#include <vector>

struct P2Picture
{
	std::string name;			// what the server stores; materials paintpics/<name>...
	std::string label;			// shown in the menu
	int width = 0, height = 0;	// the texture's size (the decal's shape follows it)
	int frames = 1;
	bool spray = false;			// one of the game's sprays
};

struct P2DirEntry
{
	std::string name;
	bool dir = false;
};

enum { P2ADD_IDLE, P2ADD_BUSY, P2ADD_DONE, P2ADD_FAILED };
struct P2AddStatus
{
	int state = P2ADD_IDLE;
	std::string text;		// for the menu: "adding cat.gif ...", "added cat", "failed: ..."
	std::string added;		// the new picture's name (P2ADD_DONE)
	int serial = 0;			// bumps on every change
};

// gameDir = the mod folder (.../cstrike)
void P2Pics_Init( const std::string &gameDir );
// rescans both folders; materials for sprays seen for the first time are written here (main thread, fast)
void P2Pics_Refresh();
const std::vector<P2Picture> &P2Pics_List();
const P2Picture *P2Pics_Find( const std::string &name );
bool P2Pics_ValidName( const std::string &name );

// turns a file (picture, GIF, VTF) into a picture in a background thread; false if one is still being made
bool P2Pics_Add( const std::string &path );
P2AddStatus P2Pics_Status();

// the file picker: sub folders and the files ADD A FILE can take, sorted, hidden ones left out
bool P2Pics_ListDir( const std::string &dir, std::vector<P2DirEntry> &out );
bool P2Pics_IsAddable( const std::string &fileName );

// $decalscale per paint size, for the colour paint (materials/p2paint/paint_sN.vmt) and pictures alike.
// The decals are drawn as sprays (PlayerDecalShoot), which the engine sizes as 64 x $decalscale units along
// the texture's longer side (textures of 128 px and up).
extern const float P2_DECAL_SCALE[8];
// world size of a picture decal (units) for its texture size, the engine's spray rule
void P2Pics_DecalSize( int texW, int texH, int size, float *w, float *h );

#endif // P2PAINT_PICS_H
