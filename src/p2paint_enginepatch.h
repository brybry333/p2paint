// p2paint: the two engine.so code patches paint needs, made in memory (see p2paint_enginepatch.cpp)
#pragma once
#include <stddef.h>
#include <string>

// Patches engine.so code in a buffer. write() does the stores (in the game: unprotect, copy, protect again).
// Returns one line per patch: "<name> patched", "<name> already patched" or "<name> NOT FOUND".
std::string P2Patch_Buffer( unsigned char *base, size_t size, bool ( *write )( unsigned char *dst, const unsigned char *src, size_t n ) );

// Finds engine.so's code in this process and patches it.
std::string P2Patch_Engine();
