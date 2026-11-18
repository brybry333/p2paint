//=============================================================================
// p2paint: the paint menu (!paint), the hold-to-paint keys and the aim ghost, as VGUI panels of the client
//=============================================================================
#ifndef P2PAINT_UI_H
#define P2PAINT_UI_H

#include "mathlib/vector.h"

class IVEngineClient;
class IMaterialSystem;

typedef void *( *P2InterfaceFn )( const char *moduleSuffix, const char *name );

// after the engine and material system are up (main thread); safe to repeat
bool P2UI_Init( P2InterfaceFn pfnInterface, IVEngineClient *pEngine, IMaterialSystem *pMatSys );
// every frame from the paint layer: where the camera is (for the aim ghost)
void P2UI_SetCamera( const Vector &camera );
// our paint plugin said hello on this server and map (p2paint_ui_state): only then the paint commands count
bool P2UI_ServerAware();
// from the paint layer's DrawModel: the ghost of the paint / erase brush where the player aims (while a key is held)
void P2UI_DrawWorld( class IMatRenderContext *pRenderContext );

#endif // P2PAINT_UI_H
