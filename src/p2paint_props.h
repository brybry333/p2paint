//=============================================================================
// p2paint: the props of the current map as triangles (LOD 0 of each model, placed in the world), for paint
// decals on props: static props and prop_dynamic entities that stay put (CP2Bsp::m_StaticProps).
//=============================================================================
#ifndef P2PAINT_PROPS_H
#define P2PAINT_PROPS_H

#include "mathlib/vector.h"
#include "mathlib/mathlib.h"
#include "tier1/utlvector.h"

#include <vector>

#include "p2paint_bsp.h"

class CP2Props
{
public:
	// loads the models of the map's props (bsp.m_StaticProps) through pfnRead ("models/..." paths)
	void Load( const CP2Bsp &bsp, P2ReadFileFn pfnRead );
	void Clear();

	int Count() const { return (int)m_Props.size(); }
	int Models() const { return (int)m_Models.size(); }
	// the props whose world bounds touch the box
	void PropsInBox( const Vector &mins, const Vector &maxs, std::vector<int> &out ) const;
	int Triangles( int prop ) const;
	// one triangle in world space, and the model's (smoothed) normal there
	void Triangle( int prop, int tri, Vector *a, Vector *b, Vector *c, Vector *pNormal ) const;

private:
	struct Model_t
	{
		std::vector<Vector> pos, normal;	// model space
		std::vector<int> tris;				// 3 per triangle
		Vector mins, maxs;
	};
	struct Prop_t
	{
		int model;
		matrix3x4_t toWorld;
		Vector mins, maxs;				// world bounds
	};

	std::vector<Model_t> m_Models;
	std::vector<Prop_t> m_Props;
};

#endif // P2PAINT_PROPS_H
