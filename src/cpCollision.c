/* Copyright (c) 2007 Scott Lembcke
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <string.h>

#include "chipmunk_private.h"
#include "ChipmunkDemo.h"

#if DEBUG
#define DRAW_ALL 0
#define DRAW_GJK (0 || DRAW_ALL)
#define DRAW_EPA (0 || DRAW_ALL)
#define DRAW_CLOSEST (0 || DRAW_ALL)
#define DRAW_CLIP (0 || DRAW_ALL)

#define PRINT_LOG 0
#endif

#define ENABLE_CACHING 1

#define MAX_GJK_ITERATIONS 30
#define MAX_EPA_ITERATIONS 30
#define WARN_GJK_ITERATIONS 20
#define WARN_EPA_ITERATIONS 20

// Add contact points for circle to circle collisions.
// Used by several collision tests.
// TODO should accept hash parameter
static void
circle2circleQuery(const cpVect p1, const cpVect p2, const cpFloat r1, const cpFloat r2, cpHashValue hash, cpCollisionInfo *info)
{
	cpFloat mindist = r1 + r2;
	cpVect delta = cpvsub(p2, p1);
	cpFloat distsq = cpvlengthsq(delta);
	
	if(distsq < mindist*mindist){
		cpFloat dist = cpfsqrt(distsq);
		cpVect n = (dist ? cpvmult(delta, 1.0f/dist) : cpv(1.0f, 0.0f));
//		cpContactInit(con, cpvlerp(p1, p2, r1/(r1 + r2)), n, dist - mindist, hash);
		
		// TODO calculate r1 and r2 independently
		cpVect p = cpvlerp(p1, p2, r1/(r1 + r2));
		cpCollisionInfoPushContact(info, cpvsub(p, info->a->body->p), cpvsub(p, info->b->body->p), n, dist - mindist, hash);
	}
}

//MARK: Support Points and Edges:

static inline int
cpSupportPointIndex(const int count, const cpVect *verts, const cpVect n)
{
	cpFloat max = -INFINITY;
	int index = 0;
	
	for(int i=0; i<count; i++){
		cpVect v = verts[i];
		cpFloat d = cpvdot(v, n);
		if(d > max){
			max = d;
			index = i;
		}
	}
	
	return index;
}

struct SupportPoint {
	cpVect p;
	cpCollisionID id;
};

static inline struct SupportPoint
SupportPointNew(cpVect p, cpCollisionID id)
{
	struct SupportPoint point = {p, id};
	return point;
}

static inline struct SupportPoint
cpSupportPoint(const int count, const cpVect *verts, const cpVect n)
{
	int i = cpSupportPointIndex(count, verts, n);
	return SupportPointNew(verts[i], i);
}

struct MinkowskiPoint {
	cpVect a, b;
	cpVect ab;
	cpCollisionID id;
};

static inline struct MinkowskiPoint
MinkoskiPointNew(const struct SupportPoint a, const struct SupportPoint b)
{
	struct MinkowskiPoint point = {a.p, b.p, cpvsub(b.p, a.p), (a.id & 0xFF)<<8 | (b.id & 0xFF)};
	return point;
}

struct SupportContext {
	const cpShape *shape1, *shape2;
	const int count1, count2;
	const cpVect *verts1, *verts2;
};

static inline struct MinkowskiPoint
Support(const struct SupportContext *ctx, const cpVect n)
{
	struct SupportPoint a = cpSupportPoint(ctx->count1, ctx->verts1, cpvneg(n));
	struct SupportPoint b = cpSupportPoint(ctx->count2, ctx->verts2, n);
	return MinkoskiPointNew(a, b);
}

struct EdgePoint {
	cpVect p;
	cpHashValue hash;
};

struct Edge {
	struct EdgePoint a, b;
	cpFloat r;
	cpVect n;
};

static inline struct Edge
EdgeNew(cpVect va, cpVect vb, cpHashValue ha, cpHashValue hb, cpFloat r)
{
	struct Edge edge = {{va, ha}, {vb, hb}, r, cpvnormalize(cpvperp(cpvsub(vb, va)))};
	return edge;
}

static struct Edge
SupportEdgeForPoly(const cpPolyShape *poly, const cpVect n)
{
	int numVerts = poly->numVerts;
	int i1 = cpSupportPointIndex(poly->numVerts, poly->tVerts, n);
	
	// TODO get rid of mod eventually, very expensive on ARM
	int i0 = (i1 - 1 + numVerts)%numVerts;
	int i2 = (i1 + 1)%numVerts;
	
	cpVect *verts = poly->tVerts;
	if(cpvdot(n, poly->tPlanes[i1].n) > cpvdot(n, poly->tPlanes[i2].n)){
		return (struct Edge){{verts[i0], CP_HASH_PAIR(poly, i0)}, {verts[i1], CP_HASH_PAIR(poly, i1)}, poly->r, poly->tPlanes[i1].n};
	} else {
		return (struct Edge){{verts[i1], CP_HASH_PAIR(poly, i1)}, {verts[i2], CP_HASH_PAIR(poly, i2)}, poly->r, poly->tPlanes[i2].n};
	}
}

static struct Edge
SupportEdgeForSegment(const cpSegmentShape *seg, const cpVect n)
{
	if(cpvdot(seg->tn, n) > 0.0){
		return (struct Edge){{seg->ta, CP_HASH_PAIR(seg, 0)}, {seg->tb, CP_HASH_PAIR(seg, 1)}, seg->r, seg->tn};
	} else {
		return (struct Edge){{seg->tb, CP_HASH_PAIR(seg, 1)}, {seg->ta, CP_HASH_PAIR(seg, 0)}, seg->r, cpvneg(seg->tn)};
	}
}

static inline cpFloat
ClosestT(const cpVect a, const cpVect b)
{
	cpVect delta = cpvsub(b, a);
	return -cpfclamp(cpvdot(delta, cpvadd(a, b))/cpvlengthsq(delta), -1.0f, 1.0f);
}

static inline cpVect
LerpT(const cpVect a, const cpVect b, const cpFloat t)
{
	cpFloat ht = 0.5f*t;
	return cpvadd(cpvmult(a, 0.5f - ht), cpvmult(b, 0.5f + ht));
}

struct ClosestPoints {
	cpVect a, b;
	cpVect n;
	cpFloat d;
	cpCollisionID id;
};

static inline struct ClosestPoints
ClosestPointsNew(const struct MinkowskiPoint v0, const struct MinkowskiPoint v1)
{
	cpFloat t = ClosestT(v0.ab, v1.ab);
	cpVect p = LerpT(v0.ab, v1.ab, t);
	
	cpVect pa = LerpT(v0.a, v1.a, t);
	cpVect pb = LerpT(v0.b, v1.b, t);
	cpCollisionID id = (v0.id & 0xFFFF)<<16 | (v1.id & 0xFFFF);
	
	cpVect delta = cpvsub(v1.ab, v0.ab);
	cpVect n = cpvnormalize(cpvperp(delta));
	cpFloat d = -cpvdot(n, p);
	
	if(d <= 0.0f || (0.0f < t && t < 1.0f)){
		struct ClosestPoints points = {pa, pb, cpvneg(n), d, id};
		return points;
	} else {
		cpFloat d2 = cpvlength(p);
		cpVect n = cpvmult(p, 1.0f/(d2 + CPFLOAT_MIN));
		
		struct ClosestPoints points = {pa, pb, n, d2, id};
		return points;
	}
}

//MARK: EPA Functions

static inline cpFloat
ClosestDist(const cpVect v0,const cpVect v1)
{
	return cpvlengthsq(LerpT(v0, v1, ClosestT(v0, v1)));
}

static struct ClosestPoints
EPARecurse(const struct SupportContext *ctx, const int count, const struct MinkowskiPoint *hull, const int i)
{
	int mini = 0;
	cpFloat minDist = INFINITY;
	
	// TODO: precalculate this when building the hull and save a step.
	for(int j=0, i=count-1; j<count; i=j, j++){
		cpFloat d = ClosestDist(hull[i].ab, hull[j].ab);
		if(d < minDist){
			minDist = d;
			mini = i;
		}
	}
	
	struct MinkowskiPoint v0 = hull[mini];
	struct MinkowskiPoint v1 = hull[(mini + 1)%count];
	cpAssertSoft(!cpveql(v0.ab, v1.ab), "Internal Error: EPA vertexes are the same (%d and %d)", mini, (mini + 1)%count);
	
	struct MinkowskiPoint p = Support(ctx, cpvperp(cpvsub(v1.ab, v0.ab)));
	
#if DRAW_EPA
	cpVect verts[count];
	for(int i=0; i<count; i++) verts[i] = hull[i].ab;
	
	ChipmunkDebugDrawPolygon(count, verts, RGBAColor(1, 1, 0, 1), RGBAColor(1, 1, 0, 0.25));
	ChipmunkDebugDrawSegment(v0.ab, v1.ab, RGBAColor(1, 0, 0, 1));
	
	ChipmunkDebugDrawPoints(5, 1, (cpVect[]){p.ab}, RGBAColor(1, 1, 1, 1));
#endif
	
	cpFloat area2x = cpvcross(cpvsub(v1.ab, v0.ab), cpvadd(cpvsub(p.ab, v0.ab), cpvsub(p.ab, v1.ab)));
	if(area2x > 0.0f && i < MAX_EPA_ITERATIONS){
		int count2 = 1;
		struct MinkowskiPoint *hull2 = alloca((count + 1)*sizeof(struct MinkowskiPoint));
		hull2[0] = p;
		
		for(int i=0; i<count; i++){
			int index = (mini + 1 + i)%count;
			
			cpVect h0 = hull2[count2 - 1].ab;
			cpVect h1 = hull[index].ab;
			cpVect h2 = (i + 1 < count ? hull[(index + 1)%count] : p).ab;
			
			// TODO: Should this be changed to an area2x check?
			if(cpvcross(cpvsub(h2, h0), cpvsub(h1, h0)) > 0.0f){
				hull2[count2] = hull[index];
				count2++;
			}
		}
		
		return EPARecurse(ctx, count2, hull2, i + 1);
	} else {
		cpAssertWarn(i<WARN_EPA_ITERATIONS, "High EPA iterations: %d", i);
		return ClosestPointsNew(v0, v1);
	}
}

static struct ClosestPoints
EPA(const struct SupportContext *ctx, const struct MinkowskiPoint v0, const struct MinkowskiPoint v1, const struct MinkowskiPoint v2)
{
	// TODO: allocate a NxM array here and do an in place convex hull reduction in EPARecurse
	struct MinkowskiPoint hull[3] = {v0, v1, v2};
	return EPARecurse(ctx, 3, hull, 1);
}

//MARK: GJK Functions.

static inline struct ClosestPoints
GJKRecurse(const struct SupportContext *ctx, const struct MinkowskiPoint v0, const struct MinkowskiPoint v1, const int i)
{
	if(i > MAX_GJK_ITERATIONS){
		cpAssertWarn(i < WARN_GJK_ITERATIONS, "High GJK iterations: %d", i);
		return ClosestPointsNew(v0, v1);
	}
	
	cpVect delta = cpvsub(v1.ab, v0.ab);
	if(cpvcross(delta, cpvadd(v0.ab, v1.ab)) > 0.0f){
		// Origin is behind axis. Flip and try again.
		return GJKRecurse(ctx, v1, v0, i + 1);
	} else {
		cpFloat t = ClosestT(v0.ab, v1.ab);
		cpVect n = (-1.0f < t && t < 1.0f ? cpvperp(delta) : cpvneg(LerpT(v0.ab, v1.ab, t)));
		struct MinkowskiPoint p = Support(ctx, n);
		
#if DRAW_GJK
		ChipmunkDebugDrawSegment(v0.ab, v1.ab, RGBAColor(1, 1, 1, 1));
		cpVect c = cpvlerp(v0.ab, v1.ab, 0.5);
		ChipmunkDebugDrawSegment(c, cpvadd(c, cpvmult(cpvnormalize(n), 5.0)), RGBAColor(1, 0, 0, 1));
		
		ChipmunkDebugDrawPoints(5.0, 1, &p.ab, RGBAColor(1, 1, 1, 1));
#endif
		
		if(
			cpvcross(cpvsub(v1.ab, p.ab), cpvadd(v1.ab, p.ab)) > 0.0f &&
			cpvcross(cpvsub(v0.ab, p.ab), cpvadd(v0.ab, p.ab)) < 0.0f
		){
			cpAssertWarn(i < WARN_GJK_ITERATIONS, "High GJK->EPA iterations: %d", i);
			// The triangle v0, p, v1 contains the origin. Use EPA to find the MSA.
			return EPA(ctx, v0, p, v1);
		} else {
			// The new point must be farther along the normal than the existing points.
			if(cpvdot(p.ab, n) <= cpfmax(cpvdot(v0.ab, n), cpvdot(v1.ab, n))){
				cpAssertWarn(i < WARN_GJK_ITERATIONS, "High GJK iterations: %d", i);
				return ClosestPointsNew(v0, v1);
			} else {
				if(ClosestDist(v0.ab, p.ab) < ClosestDist(p.ab, v1.ab)){
					return GJKRecurse(ctx, v0, p, i + 1);
				} else {
					return GJKRecurse(ctx, p, v1, i + 1);
				}
			}
		}
	}
}

static struct SupportPoint
ShapePoint(const int count, const cpVect *verts, const int i)
{
	int index = (i < count ? i : 0);
	return SupportPointNew(verts[i], index);
}

static struct ClosestPoints
GJK(const struct SupportContext *ctx, cpCollisionID *id)
{
#if DRAW_GJK || DRAW_EPA
	// draw the minkowski difference origin
	cpVect origin = cpvzero;
	ChipmunkDebugDrawPoints(5.0, 1, &origin, RGBAColor(1,0,0,1));
	
	int mdiffCount = ctx->count1*ctx->count2;
	cpVect *mdiffVerts = alloca(mdiffCount*sizeof(cpVect));
	
	for(int i=0; i<ctx->count1; i++){
		for(int j=0; j<ctx->count2; j++){
			cpVect v1 = ShapePoint(ctx->count1, ctx->verts1, i).p;
			cpVect v2 = ShapePoint(ctx->count2, ctx->verts2, j).p;
			mdiffVerts[i*ctx->count2 + j] = cpvsub(v2, v1);
		}
	}
	 
	cpVect *hullVerts = alloca(mdiffCount*sizeof(cpVect));
	int hullCount = cpConvexHull(mdiffCount, mdiffVerts, hullVerts, NULL, 0.0);
	
	ChipmunkDebugDrawPolygon(hullCount, hullVerts, RGBAColor(1, 0, 0, 1), RGBAColor(1, 0, 0, 0.25));
	ChipmunkDebugDrawPoints(2.0, mdiffCount, mdiffVerts, RGBAColor(1, 0, 0, 1));
#endif
	
	struct MinkowskiPoint v0, v1;
	if(*id && ENABLE_CACHING){
		v0 = MinkoskiPointNew(ShapePoint(ctx->count1, ctx->verts1, (*id>>24)&0xFF), ShapePoint(ctx->count2, ctx->verts2, (*id>>16)&0xFF));
		v1 = MinkoskiPointNew(ShapePoint(ctx->count1, ctx->verts1, (*id>> 8)&0xFF), ShapePoint(ctx->count2, ctx->verts2, (*id    )&0xFF));
	} else {
		cpVect axis = cpvperp(cpvsub(cpBBCenter(ctx->shape1->bb), cpBBCenter(ctx->shape2->bb)));
		v0 = Support(ctx, axis);
		v1 = Support(ctx, cpvneg(axis));
	}
	
	struct ClosestPoints points = GJKRecurse(ctx, v0, v1, 1);
	*id = points.id;
	return points;
}

//MARK: Contact Clipping

//static inline void
//Contact1(cpFloat dist, cpVect a, cpVect b, cpFloat refr, cpFloat incr, cpVect n, cpHashValue hash, cpCollisionInfo *info)
//{
//	cpFloat rsum = refr + incr;
//	cpFloat alpha = (rsum > 0.0f ? refr/rsum : 0.5f);
//	cpVect point = cpvlerp(a, b, alpha);
//	
////	cpContactInit(arr, point, n, dist - rsum, hash);
//	cpContactTempPush(contacts, cpvsub(point, contacts->a->p), cpvsub(point, contacts->b->p), n, dist - rsum, hash);
//}
//
//static inline int
//Contact2(cpVect refp, cpVect inca, cpVect incb, cpFloat refr, cpFloat incr, cpVect refn, cpVect n, cpHashValue hash, cpCollisionInfo *info)
//{
//	cpFloat cian = cpvcross(inca, refn);
//	cpFloat cibn = cpvcross(incb, refn);
//	cpFloat crpn = cpvcross(refp, refn);
//	cpFloat t = 1.0f - cpfclamp01((cibn - crpn)/(cibn - cian));
//	
//	cpVect point = cpvlerp(inca, incb, t);
//	cpFloat pd = cpvdot(cpvsub(point, refp), refn);
//	
//	if(t > 0.0f && pd <= 0.0f){
//		cpFloat rsum = refr + incr;
//		cpFloat alpha = (rsum > 0.0f ? incr*(1.0f - (rsum + pd)/rsum) : -0.5f*pd);
//		
////		cpContactInit(arr, cpvadd(point, cpvmult(refn, alpha)), n, pd, hash);
//		cpVect p = cpvadd(point, cpvmult(refn, alpha));
//		cpContactTempPush(contacts, cpvsub(p, contacts->a->p), cpvsub(p, contacts->b->p), n, pd, hash);
//		return 1;
//	} else {
//		return 0;
//	}
//}
//
//static inline int
//ClipContacts(const struct Edge ref, const struct Edge inc, const struct ClosestPoints points, const cpFloat nflip, cpCollisionInfo *info)
//{
//	cpVect inc_offs = cpvmult(inc.n, inc.r);
//	cpVect ref_offs = cpvmult(ref.n, ref.r);
//	
//	cpVect inca = cpvadd(inc.a.p, inc_offs);
//	cpVect incb = cpvadd(inc.b.p, inc_offs);
//	
//	cpVect closest_inca = cpClosetPointOnSegment(inc.a.p, ref.a.p, ref.b.p);
//	cpVect closest_incb = cpClosetPointOnSegment(inc.b.p, ref.a.p, ref.b.p);
//	
//	cpVect msa = cpvmult(points.n, nflip*points.d);
//	cpFloat cost_a = cpvdistsq(cpvsub(inc.a.p, closest_inca), msa);
//	cpFloat cost_b = cpvdistsq(cpvsub(inc.b.p, closest_incb), msa);
//	
//#if DRAW_CLIP
//	ChipmunkDebugDrawSegment(ref.a.p, ref.b.p, RGBAColor(1, 0, 0, 1));
//	ChipmunkDebugDrawSegment(inc.a.p, inc.b.p, RGBAColor(0, 1, 0, 1));
//	ChipmunkDebugDrawSegment(inca, incb, RGBAColor(0, 1, 0, 1));
//	
//	cpVect cref = cpvlerp(ref.a.p, ref.b.p, 0.5);
//	ChipmunkDebugDrawSegment(cref, cpvadd(cref, cpvmult(ref.n, 5.0)), RGBAColor(1, 0, 0, 1));
//	
//	cpVect cinc = cpvlerp(inc.a.p, inc.b.p, 0.5);
//	ChipmunkDebugDrawSegment(cinc, cpvadd(cinc, cpvmult(inc.n, 5.0)), RGBAColor(1, 0, 0, 1));
//	
//	ChipmunkDebugDrawPoints(5.0, 2, (cpVect[]){ref.a.p, inc.a.p}, RGBAColor(1, 1, 0, 1));
//	ChipmunkDebugDrawPoints(5.0, 2, (cpVect[]){ref.b.p, inc.b.p}, RGBAColor(0, 1, 1, 1));
//	
//	if(cost_a < cost_b){
//		ChipmunkDebugDrawSegment(closest_inca, inc.a.p, RGBAColor(1, 0, 1, 1));
//	} else {
//		ChipmunkDebugDrawSegment(closest_incb, inc.b.p, RGBAColor(1, 0, 1, 1));
//	}
//#endif
//	
//	cpHashValue hash_iarb = CP_HASH_PAIR(inc.a.hash, ref.b.hash);
//	cpHashValue hash_ibra = CP_HASH_PAIR(inc.b.hash, ref.a.hash);
//	
//	if(cost_a < cost_b){
//		cpVect refp = cpvadd(ref.a.p, ref_offs);
//		Contact1(points.d, closest_inca, inc.a.p, ref.r, inc.r, points.n, hash_iarb, contacts);
//		return Contact2(refp, inca, incb, ref.r, inc.r, ref.n, points.n, hash_ibra, contacts) + 1;
//	} else {
//		cpVect refp = cpvadd(ref.b.p, ref_offs);
//		Contact1(points.d, closest_incb, inc.b.p, ref.r, inc.r, points.n, hash_ibra, contacts);
//		return Contact2(refp, incb, inca, ref.r, inc.r, ref.n, points.n, hash_iarb, contacts) + 1;
//	}
//}
//
//static inline int
//ContactPoints(const struct Edge e1, const struct Edge e2, const struct ClosestPoints points, cpCollisionInfo *info)
//{
//	cpFloat mindist = e1.r + e2.r;
//	if(points.d <= mindist){
//		cpFloat pick = cpvdot(e1.n, points.n) + cpvdot(e2.n, points.n);
//		
//		if(
//			(pick != 0.0f && pick > 0.0f) ||
//			// If the edges are both perfectly aligned weird things happen.
//			// This is *very* common at the start of a simulation.
//			// Pick the longest edge as the reference to break the tie.
//			(pick == 0.0f && (cpvdistsq(e1.a.p, e1.b.p) > cpvdistsq(e2.a.p, e2.b.p)))
//		){
//			return ClipContacts(e1, e2, points,  1.0f, contacts);
//		} else {
//			return ClipContacts(e2, e1, points, -1.0f, contacts);
//		}
//	} else {
//		return 0;
//	}
//}

static inline void
ContactPoints(const struct Edge e1, const struct Edge e2, const struct ClosestPoints points, cpCollisionInfo *info)
{
	cpFloat mindist = e1.r + e2.r;
	if(points.d <= mindist){
		cpVect n = points.n;//(cpvdot(e1.n, points.n) + cpvdot(e2.n, points.n) > 0.0f ? e1.n : cpvneg(e2.n));
		
//		ChipmunkDebugDrawSegment(e1.a.p, e1.b.p, RGBAColor(0, 1, 0, 1));
//		ChipmunkDebugDrawSegment(e2.a.p, e2.b.p, RGBAColor(0, 1, 0, 1));
		
		// Distances along the axis parallel to n
		cpFloat d_e1_a = cpvcross(e1.a.p, n);
		cpFloat d_e1_b = cpvcross(e1.b.p, n);
		cpFloat d_e2_a = cpvcross(e2.a.p, n);
		cpFloat d_e2_b = cpvcross(e2.b.p, n);
		
		cpFloat e1_denom = 1.0f/(d_e1_b - d_e1_a);
		cpFloat e2_denom = 1.0f/(d_e2_b - d_e2_a);
		
		cpHashValue hash_1a2b = CP_HASH_PAIR(e1.a.hash, e2.b.hash);
		cpHashValue hash_1b2a = CP_HASH_PAIR(e1.b.hash, e2.a.hash);
		
		{
			cpVect r1 = cpvadd(cpvmult(n,  e1.r), cpvlerp(e1.a.p, e1.b.p, cpfclamp01((d_e2_b - d_e1_a)*e1_denom)));
			cpVect r2 = cpvadd(cpvmult(n, -e2.r), cpvlerp(e2.a.p, e2.b.p, cpfclamp01((d_e1_a - d_e2_a)*e2_denom)));
			cpFloat dist = cpvdot(cpvsub(r2, r1), n);
//			ChipmunkDemoPrintString("dist: %f\n", dist);
			if(dist <= 0.0f)
			{
				cpCollisionInfoPushContact(info, cpvsub(r1, info->a->body->p), cpvsub(r2, info->b->body->p), n, dist, hash_1a2b);
			}
		}{
			cpVect r1 = cpvadd(cpvmult(n,  e1.r), cpvlerp(e1.a.p, e1.b.p, cpfclamp01((d_e2_a - d_e1_a)*e1_denom)));
			cpVect r2 = cpvadd(cpvmult(n, -e2.r), cpvlerp(e2.a.p, e2.b.p, cpfclamp01((d_e1_b - d_e2_a)*e2_denom)));
			cpFloat dist = cpvdot(cpvsub(r2, r1), n);
//			ChipmunkDemoPrintString("dist: %f\n", dist);
			if(dist <= 0.0f)
			{
				cpCollisionInfoPushContact(info, cpvsub(r1, info->a->body->p), cpvsub(r2, info->b->body->p), n, dist, hash_1b2a);
			}
		}
	}
}

//MARK: Collision Functions

typedef void (*CollisionFunc)(const cpShape *a, const cpShape *b, cpCollisionInfo *info);

// Collide circle shapes.
static void
circle2circle(const cpCircleShape *c1, const cpCircleShape *c2, cpCollisionInfo *info)
{
	circle2circleQuery(c1->tc, c2->tc, c1->r, c2->r, 0, info);
}

static void
circle2segment(const cpCircleShape *circleShape, const cpSegmentShape *segmentShape, cpCollisionInfo *info)
{
	cpVect seg_a = segmentShape->ta;
	cpVect seg_b = segmentShape->tb;
	cpVect center = circleShape->tc;
	
	cpVect seg_delta = cpvsub(seg_b, seg_a);
	cpFloat closest_t = cpfclamp01(cpvdot(seg_delta, cpvsub(center, seg_a))/cpvlengthsq(seg_delta));
	cpVect closest = cpvadd(seg_a, cpvmult(seg_delta, closest_t));
	
	circle2circleQuery(center, closest, circleShape->r, segmentShape->r, 0, info);
	if(info->count > 0){
		cpVect n = info->n;
		
		// Reject endcap collisions if tangents are provided.
		if(
			(closest_t != 0.0f || cpvdot(n, cpvrotate(segmentShape->a_tangent, segmentShape->shape.body->rot)) >= 0.0) &&
			(closest_t != 1.0f || cpvdot(n, cpvrotate(segmentShape->b_tangent, segmentShape->shape.body->rot)) >= 0.0)
		){
		} else {
			info->count = 0;
		}
	}
}

static void
segment2segment(const cpSegmentShape *seg1, const cpSegmentShape *seg2, cpCollisionInfo *info)
{
	struct SupportContext context = {(cpShape *)seg1, (cpShape *)seg2, 2, 2, &seg1->ta, &seg2->ta};
	struct ClosestPoints points = GJK(&context, &info->id);
	
#if DRAW_CLOSEST
#if PRINT_LOG
//	ChipmunkDemoPrintString("Distance: %.2f\n", points.d);
#endif
	
	ChipmunkDebugDrawPoints(6.0, 2, (cpVect[]){points.a, points.b}, RGBAColor(1, 1, 1, 1));
	ChipmunkDebugDrawSegment(points.a, points.b, RGBAColor(1, 1, 1, 1));
	ChipmunkDebugDrawSegment(points.a, cpvadd(points.a, cpvmult(points.n, 10.0)), RGBAColor(1, 0, 0, 1));
#endif
	
	cpVect n = points.n;
	cpVect rot1 = seg1->shape.body->rot;
	cpVect rot2 = seg2->shape.body->rot;
	if(
		points.d <= (seg1->r + seg2->r) &&
		(
			(!cpveql(points.a, seg1->ta) || cpvdot(n, cpvrotate(seg1->a_tangent, rot1)) <= 0.0) &&
			(!cpveql(points.a, seg1->tb) || cpvdot(n, cpvrotate(seg1->b_tangent, rot1)) <= 0.0) &&
			(!cpveql(points.b, seg2->ta) || cpvdot(n, cpvrotate(seg2->a_tangent, rot2)) >= 0.0) &&
			(!cpveql(points.b, seg2->tb) || cpvdot(n, cpvrotate(seg2->b_tangent, rot2)) >= 0.0)
		)
	){
		ContactPoints(SupportEdgeForSegment(seg1, n), SupportEdgeForSegment(seg2, cpvneg(n)), points, info);
	}
}

static void
poly2poly(const cpPolyShape *poly1, const cpPolyShape *poly2, cpCollisionInfo *info)
{
	struct SupportContext context = {(cpShape *)poly1, (cpShape *)poly2, poly1->numVerts, poly2->numVerts, poly1->tVerts, poly2->tVerts};
	struct ClosestPoints points = GJK(&context, &info->id);
	
#if DRAW_CLOSEST
#if PRINT_LOG
//	ChipmunkDemoPrintString("Distance: %.2f\n", points.d);
#endif
	
	ChipmunkDebugDrawPoints(3.0, 2, (cpVect[]){points.a, points.b}, RGBAColor(1, 1, 1, 1));
	ChipmunkDebugDrawSegment(points.a, points.b, RGBAColor(1, 1, 1, 1));
	ChipmunkDebugDrawSegment(points.a, cpvadd(points.a, cpvmult(points.n, 10.0)), RGBAColor(1, 0, 0, 1));
#endif
	
	if(points.d - poly1->r - poly2->r <= 0.0){
		ContactPoints(SupportEdgeForPoly(poly1, points.n), SupportEdgeForPoly(poly2, cpvneg(points.n)), points, info);
	}
}

static void
seg2poly(const cpSegmentShape *seg, const cpPolyShape *poly, cpCollisionInfo *info)
{
	struct SupportContext context = {(cpShape *)seg, (cpShape *)poly, 2, poly->numVerts, &seg->ta, poly->tVerts};
	struct ClosestPoints points = GJK(&context, &info->id);
	
#if DRAW_CLOSEST
#if PRINT_LOG
//	ChipmunkDemoPrintString("Distance: %.2f\n", points.d);
#endif
	
	ChipmunkDebugDrawPoints(3.0, 2, (cpVect[]){points.a, points.b}, RGBAColor(1, 1, 1, 1));
	ChipmunkDebugDrawSegment(points.a, points.b, RGBAColor(1, 1, 1, 1));
	ChipmunkDebugDrawSegment(points.a, cpvadd(points.a, cpvmult(points.n, 10.0)), RGBAColor(1, 0, 0, 1));
#endif
	
	// Reject endcap collisions if tangents are provided.
	cpVect n = points.n;
	cpVect rot = seg->shape.body->rot;
	if(
		points.d - seg->r - poly->r <= 0.0 &&
		(
			(!cpveql(points.a, seg->ta) || cpvdot(n, cpvrotate(seg->a_tangent, rot)) <= 0.0) &&
			(!cpveql(points.a, seg->tb) || cpvdot(n, cpvrotate(seg->b_tangent, rot)) <= 0.0)
		)
	){
		ContactPoints(SupportEdgeForSegment(seg, n), SupportEdgeForPoly(poly, cpvneg(n)), points, info);
	}
}

// This one is less gross, but still gross.
// TODO: Comment me!
// TODO respect poly radius
static void
circle2poly(const cpCircleShape *circle, const cpPolyShape *poly, cpCollisionInfo *info)
{
	cpSplittingPlane *planes = poly->tPlanes;
	
	int numVerts = poly->numVerts;
	int mini = 0;
	cpFloat min = cpSplittingPlaneCompare(planes[0], circle->tc) - circle->r;
	for(int i=0; i<poly->numVerts; i++){
		cpFloat dist = cpSplittingPlaneCompare(planes[i], circle->tc) - circle->r;
		if(dist > 0.0f){
			return;
		} else if(dist > min) {
			min = dist;
			mini = i;
		}
	}
	
	cpVect n = planes[mini].n;
	cpVect a = poly->tVerts[(mini - 1 + numVerts)%numVerts];
	cpVect b = poly->tVerts[mini];
	cpFloat dta = cpvcross(n, a);
	cpFloat dtb = cpvcross(n, b);
	cpFloat dt = cpvcross(n, circle->tc);
	
	if(dt < dtb){
		circle2circleQuery(circle->tc, b, circle->r, poly->r, 0, info);
	} else if(dt < dta) {
		cpVect point = cpvsub(circle->tc, cpvmult(n, circle->r + min/2.0f));
//		cpContactInit(con, point, cpvneg(n), min, 0);
		cpCollisionInfoPushContact(info, cpvsub(point, info->a->body->p), cpvsub(point, info->b->body->p), cpvneg(n), min, 0);
	} else {
		circle2circleQuery(circle->tc, a, circle->r, poly->r, 0, info);
	}
}

static const CollisionFunc builtinCollisionFuncs[9] = {
	(CollisionFunc)circle2circle,
	NULL,
	NULL,
	(CollisionFunc)circle2segment,
	NULL,
	NULL,
	(CollisionFunc)circle2poly,
	(CollisionFunc)seg2poly,
	(CollisionFunc)poly2poly,
};
static const CollisionFunc *colfuncs = builtinCollisionFuncs;

static const CollisionFunc segmentCollisions[9] = {
	(CollisionFunc)circle2circle,
	NULL,
	NULL,
	(CollisionFunc)circle2segment,
	(CollisionFunc)segment2segment,
	NULL,
	(CollisionFunc)circle2poly,
	(CollisionFunc)seg2poly,
	(CollisionFunc)poly2poly,
};

void
cpEnableSegmentToSegmentCollisions(void)
{
	colfuncs = segmentCollisions;
}

struct cpCollisionInfo
cpCollideShapes(cpShape *a, cpShape *b, cpCollisionID id, cpContact *contacts)
{
	cpCollisionInfo info = {a, b, id, cpvzero, 0, contacts};
	
	// Their shape types must be in order.
	cpAssertSoft(a->klass->type <= b->klass->type, "Internal Error: Collision shapes passed to cpCollideShapes() are not sorted.");
	
	CollisionFunc cfunc = colfuncs[a->klass->type + b->klass->type*CP_NUM_SHAPES];
	
	if(cfunc) cfunc(a, b, &info);
	cpAssertSoft(info.count <= CP_MAX_CONTACTS_PER_ARBITER, "Internal error: Too many contact points returned.");
	
	if(0){
		cpVect n = info.n;
		
		cpBody *a = info.a->body;
		cpBody *b = info.b->body;
		
		for(int i=0; i<info.count; i++){
			cpVect r1 = cpvadd(a->p, info.arr[i].r1);
			cpVect r2 = cpvadd(b->p, info.arr[i].r2);
			cpVect delta = cpvmult(n, cpfmax(5.0, -0.5*info.arr[i].dist));
			
			ChipmunkDebugDrawSegment(r1, cpvsub(r1, delta), RGBAColor(1, 0, 0, 1));
			ChipmunkDebugDrawSegment(r2, cpvadd(r2, delta), RGBAColor(0, 0, 1, 1));
		}
	}
	
	return info;
}

