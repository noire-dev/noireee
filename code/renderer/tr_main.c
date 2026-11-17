/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_main.c -- main control flow for each frame

#include "tr_local.h"

#include <string.h>  // memcpy

trGlobals_t tr;

static float s_flipMatrix[16] = {// convert from our coordinate system (looking down X)
                                 // to OpenGL's coordinate system (looking down -Z)
                                 0,
                                 0,
                                 -1,
                                 0,
                                 -1,
                                 0,
                                 0,
                                 0,
                                 0,
                                 1,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 1};

refimport_t ri;

// entities that will have procedurally generated surfaces will just
// point at this for their sorting surface
surfaceType_t entitySurface = SF_ENTITY;

/*
================
R_CompareVert
================
*/
bool R_CompareVert(srfVert_t* v1, srfVert_t* v2, bool checkST) {
	int i;

	for(i = 0; i < 3; i++) {
		if(floor(v1->xyz[i] + 0.1) != floor(v2->xyz[i] + 0.1)) {
			return false;
		}

		if(checkST && ((v1->st[0] != v2->st[0]) || (v1->st[1] != v2->st[1]))) {
			return false;
		}
	}

	return true;
}

/*
=============
R_CalcTexDirs

Lengyel, Eric. "Computing Tangent Space Basis Vectors for an Arbitrary Mesh". Terathon Software 3D Graphics Library, 2001.
http://www.terathon.com/code/tangent.html
=============
*/
void R_CalcTexDirs(vec3_t sdir, vec3_t tdir, const vec3_t v1, const vec3_t v2, const vec3_t v3, const vec2_t w1, const vec2_t w2, const vec2_t w3) {
	float x1, x2, y1, y2, z1, z2;
	float s1, s2, t1, t2, r;

	x1 = v2[0] - v1[0];
	x2 = v3[0] - v1[0];
	y1 = v2[1] - v1[1];
	y2 = v3[1] - v1[1];
	z1 = v2[2] - v1[2];
	z2 = v3[2] - v1[2];

	s1 = w2[0] - w1[0];
	s2 = w3[0] - w1[0];
	t1 = w2[1] - w1[1];
	t2 = w3[1] - w1[1];

	r = s1 * t2 - s2 * t1;
	if(r) r = 1.0f / r;

	VectorSet(sdir, (t2 * x1 - t1 * x2) * r, (t2 * y1 - t1 * y2) * r, (t2 * z1 - t1 * z2) * r);
	VectorSet(tdir, (s1 * x2 - s2 * x1) * r, (s1 * y2 - s2 * y1) * r, (s1 * z2 - s2 * z1) * r);
}

/*
=============
R_CalcTangentSpace

Lengyel, Eric. "Computing Tangent Space Basis Vectors for an Arbitrary Mesh". Terathon Software 3D Graphics Library, 2001.
http://www.terathon.com/code/tangent.html
=============
*/
vec_t R_CalcTangentSpace(vec3_t tangent, vec3_t bitangent, const vec3_t normal, const vec3_t sdir, const vec3_t tdir) {
	vec3_t n_cross_t;
	vec_t n_dot_t, handedness;

	// Gram-Schmidt orthogonalize
	n_dot_t = DotProduct(normal, sdir);
	VectorMA(sdir, -n_dot_t, normal, tangent);
	VectorNormalize(tangent);

	// Calculate handedness
	CrossProduct(normal, sdir, n_cross_t);
	handedness = (DotProduct(n_cross_t, tdir) < 0.0f) ? -1.0f : 1.0f;

	// Calculate orthogonal bitangent, if necessary
	if(bitangent) CrossProduct(normal, tangent, bitangent);

	return handedness;
}

bool R_CalcTangentVectors(srfVert_t* dv[3]) {
	int i;
	float bb, s, t;
	vec3_t bary;

	/* calculate barycentric basis for the triangle */
	bb = (dv[1]->st[0] - dv[0]->st[0]) * (dv[2]->st[1] - dv[0]->st[1]) - (dv[2]->st[0] - dv[0]->st[0]) * (dv[1]->st[1] - dv[0]->st[1]);
	if(fabs(bb) < 0.00000001f) return false;

	/* do each vertex */
	for(i = 0; i < 3; i++) {
		vec4_t tangent;
		vec3_t normal, bitangent, nxt;

		// calculate s tangent vector
		s = dv[i]->st[0] + 10.0f;
		t = dv[i]->st[1];
		bary[0] = ((dv[1]->st[0] - s) * (dv[2]->st[1] - t) - (dv[2]->st[0] - s) * (dv[1]->st[1] - t)) / bb;
		bary[1] = ((dv[2]->st[0] - s) * (dv[0]->st[1] - t) - (dv[0]->st[0] - s) * (dv[2]->st[1] - t)) / bb;
		bary[2] = ((dv[0]->st[0] - s) * (dv[1]->st[1] - t) - (dv[1]->st[0] - s) * (dv[0]->st[1] - t)) / bb;

		tangent[0] = bary[0] * dv[0]->xyz[0] + bary[1] * dv[1]->xyz[0] + bary[2] * dv[2]->xyz[0];
		tangent[1] = bary[0] * dv[0]->xyz[1] + bary[1] * dv[1]->xyz[1] + bary[2] * dv[2]->xyz[1];
		tangent[2] = bary[0] * dv[0]->xyz[2] + bary[1] * dv[1]->xyz[2] + bary[2] * dv[2]->xyz[2];

		VectorSubtract(tangent, dv[i]->xyz, tangent);
		VectorNormalize(tangent);

		// calculate t tangent vector
		s = dv[i]->st[0];
		t = dv[i]->st[1] + 10.0f;
		bary[0] = ((dv[1]->st[0] - s) * (dv[2]->st[1] - t) - (dv[2]->st[0] - s) * (dv[1]->st[1] - t)) / bb;
		bary[1] = ((dv[2]->st[0] - s) * (dv[0]->st[1] - t) - (dv[0]->st[0] - s) * (dv[2]->st[1] - t)) / bb;
		bary[2] = ((dv[0]->st[0] - s) * (dv[1]->st[1] - t) - (dv[1]->st[0] - s) * (dv[0]->st[1] - t)) / bb;

		bitangent[0] = bary[0] * dv[0]->xyz[0] + bary[1] * dv[1]->xyz[0] + bary[2] * dv[2]->xyz[0];
		bitangent[1] = bary[0] * dv[0]->xyz[1] + bary[1] * dv[1]->xyz[1] + bary[2] * dv[2]->xyz[1];
		bitangent[2] = bary[0] * dv[0]->xyz[2] + bary[1] * dv[1]->xyz[2] + bary[2] * dv[2]->xyz[2];

		VectorSubtract(bitangent, dv[i]->xyz, bitangent);
		VectorNormalize(bitangent);

		// store bitangent handedness
		R_VaoUnpackNormal(normal, dv[i]->normal);
		CrossProduct(normal, tangent, nxt);
		tangent[3] = (DotProduct(nxt, bitangent) < 0.0f) ? -1.0f : 1.0f;

		R_VaoPackTangent(dv[i]->tangent, tangent);

		// debug code
		//% Sys_FPrintf( SYS_VRB, "%d S: (%f %f %f) T: (%f %f %f)\n", i,
		//%     stv[ i ][ 0 ], stv[ i ][ 1 ], stv[ i ][ 2 ], ttv[ i ][ 0 ], ttv[ i ][ 1 ], ttv[ i ][ 2 ] );
	}

	return true;
}

/*
=================
R_CullLocalBox

Returns CULL_IN, CULL_CLIP, or CULL_OUT
=================
*/
int R_CullLocalBox(vec3_t localBounds[2]) {
	int j;
	vec3_t transformed;
	vec3_t v;
	vec3_t worldBounds[2];

	// transform into world space
	ClearBounds(worldBounds[0], worldBounds[1]);

	for(j = 0; j < 8; j++) {
		v[0] = localBounds[j & 1][0];
		v[1] = localBounds[(j >> 1) & 1][1];
		v[2] = localBounds[(j >> 2) & 1][2];

		R_LocalPointToWorld(v, transformed);

		AddPointToBounds(transformed, worldBounds[0], worldBounds[1]);
	}

	return R_CullBox(worldBounds);
}

/*
=================
R_CullBox

Returns CULL_IN, CULL_CLIP, or CULL_OUT
=================
*/
int R_CullBox(vec3_t worldBounds[2]) {
	int i;
	cplane_t* frust;
	bool anyClip;
	int r, numPlanes;

	numPlanes = (tr.viewParms.flags & VPF_FARPLANEFRUSTUM) ? 5 : 4;

	// check against frustum planes
	anyClip = false;
	for(i = 0; i < numPlanes; i++) {
		frust = &tr.viewParms.frustum[i];

		r = BoxOnPlaneSide(worldBounds[0], worldBounds[1], frust);

		if(r == 2) {
			// completely outside frustum
			return CULL_OUT;
		}
		if(r == 3) {
			anyClip = true;
		}
	}

	if(!anyClip) {
		// completely inside frustum
		return CULL_IN;
	}

	// partially clipped
	return CULL_CLIP;
}

/*
** R_CullLocalPointAndRadius
*/
int R_CullLocalPointAndRadius(const vec3_t pt, float radius) {
	vec3_t transformed;

	R_LocalPointToWorld(pt, transformed);

	return R_CullPointAndRadius(transformed, radius);
}

/*
** R_CullPointAndRadius
*/
int R_CullPointAndRadiusEx(const vec3_t pt, float radius, const cplane_t* frustum, int numPlanes) {
	int i;
	float dist;
	const cplane_t* frust;
	bool mightBeClipped = false;

	// check against frustum planes
	for(i = 0; i < numPlanes; i++) {
		frust = &frustum[i];

		dist = DotProduct(pt, frust->normal) - frust->dist;
		if(dist < -radius) {
			return CULL_OUT;
		} else if(dist <= radius) {
			mightBeClipped = true;
		}
	}

	if(mightBeClipped) {
		return CULL_CLIP;
	}

	return CULL_IN;  // completely inside frustum
}

/*
** R_CullPointAndRadius
*/
int R_CullPointAndRadius(const vec3_t pt, float radius) { return R_CullPointAndRadiusEx(pt, radius, tr.viewParms.frustum, (tr.viewParms.flags & VPF_FARPLANEFRUSTUM) ? 5 : 4); }

/*
=================
R_LocalNormalToWorld

=================
*/
void R_LocalNormalToWorld(const vec3_t local, vec3_t world) {
	world[0] = local[0] * tr.or.axis[0][0] + local[1] * tr.or.axis[1][0] + local[2] * tr.or.axis[2][0];
	world[1] = local[0] * tr.or.axis[0][1] + local[1] * tr.or.axis[1][1] + local[2] * tr.or.axis[2][1];
	world[2] = local[0] * tr.or.axis[0][2] + local[1] * tr.or.axis[1][2] + local[2] * tr.or.axis[2][2];
}

/*
=================
R_LocalPointToWorld

=================
*/
void R_LocalPointToWorld(const vec3_t local, vec3_t world) {
	world[0] = local[0] * tr.or.axis[0][0] + local[1] * tr.or.axis[1][0] + local[2] * tr.or.axis[2][0] + tr.or.origin[0];
	world[1] = local[0] * tr.or.axis[0][1] + local[1] * tr.or.axis[1][1] + local[2] * tr.or.axis[2][1] + tr.or.origin[1];
	world[2] = local[0] * tr.or.axis[0][2] + local[1] * tr.or.axis[1][2] + local[2] * tr.or.axis[2][2] + tr.or.origin[2];
}

/*
=================
R_WorldToLocal

=================
*/
void R_WorldToLocal(const vec3_t world, vec3_t local) {
	local[0] = DotProduct(world, tr.or.axis[0]);
	local[1] = DotProduct(world, tr.or.axis[1]);
	local[2] = DotProduct(world, tr.or.axis[2]);
}

/*
==========================
R_TransformModelToClip

==========================
*/
void R_TransformModelToClip(const vec3_t src, const float* modelMatrix, const float* projectionMatrix, vec4_t eye, vec4_t dst) {
	int i;

	for(i = 0; i < 4; i++) {
		eye[i] = src[0] * modelMatrix[i + 0 * 4] + src[1] * modelMatrix[i + 1 * 4] + src[2] * modelMatrix[i + 2 * 4] + 1 * modelMatrix[i + 3 * 4];
	}

	for(i = 0; i < 4; i++) {
		dst[i] = eye[0] * projectionMatrix[i + 0 * 4] + eye[1] * projectionMatrix[i + 1 * 4] + eye[2] * projectionMatrix[i + 2 * 4] + eye[3] * projectionMatrix[i + 3 * 4];
	}
}

/*
==========================
R_TransformClipToWindow

==========================
*/
void R_TransformClipToWindow(const vec4_t clip, const viewParms_t* view, vec4_t normalized, vec4_t window) {
	normalized[0] = clip[0] / clip[3];
	normalized[1] = clip[1] / clip[3];
	normalized[2] = (clip[2] + clip[3]) / (2 * clip[3]);

	window[0] = 0.5f * (1.0f + normalized[0]) * view->viewportWidth;
	window[1] = 0.5f * (1.0f + normalized[1]) * view->viewportHeight;
	window[2] = normalized[2];

	window[0] = (int)(window[0] + 0.5);
	window[1] = (int)(window[1] + 0.5);
}

/*
==========================
myGlMultMatrix

==========================
*/
void myGlMultMatrix(const float* a, const float* b, float* out) {
	int i, j;

	for(i = 0; i < 4; i++) {
		for(j = 0; j < 4; j++) {
			out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
		}
	}
}

/*
=================
R_RotateForEntity

Generates an orientation for an entity and viewParms
Does NOT produce any GL calls
Called by both the front end and the back end
=================
*/
void R_RotateForEntity(const trRefEntity_t* ent, const viewParms_t* viewParms, orientationr_t* or) {
	float glMatrix[16];
	vec3_t delta;
	float axisLength;

	if(ent->e.reType != RT_MODEL) {
		*or = viewParms->world;
		return;
	}

	VectorCopy(ent->e.origin, or->origin);

	VectorCopy(ent->e.axis[0], or->axis[0]);
	VectorCopy(ent->e.axis[1], or->axis[1]);
	VectorCopy(ent->e.axis[2], or->axis[2]);

	glMatrix[0] = or->axis[0][0];
	glMatrix[4] = or->axis[1][0];
	glMatrix[8] = or->axis[2][0];
	glMatrix[12] = or->origin[0];

	glMatrix[1] = or->axis[0][1];
	glMatrix[5] = or->axis[1][1];
	glMatrix[9] = or->axis[2][1];
	glMatrix[13] = or->origin[1];

	glMatrix[2] = or->axis[0][2];
	glMatrix[6] = or->axis[1][2];
	glMatrix[10] = or->axis[2][2];
	glMatrix[14] = or->origin[2];

	glMatrix[3] = 0;
	glMatrix[7] = 0;
	glMatrix[11] = 0;
	glMatrix[15] = 1;

	Mat4Copy(glMatrix, or->transformMatrix);
	myGlMultMatrix(glMatrix, viewParms->world.modelMatrix, or->modelMatrix);

	// calculate the viewer origin in the model's space
	// needed for fog, specular, and environment mapping
	VectorSubtract(viewParms->or.origin, or->origin, delta);

	// compensate for scale in the axes if necessary
	if(ent->e.nonNormalizedAxes) {
		axisLength = VectorLength(ent->e.axis[0]);
		if(!axisLength) {
			axisLength = 0;
		} else {
			axisLength = 1.0f / axisLength;
		}
	} else {
		axisLength = 1.0f;
	}

	or->viewOrigin[0] = DotProduct(delta, or->axis[0]) * axisLength;
	or->viewOrigin[1] = DotProduct(delta, or->axis[1]) * axisLength;
	or->viewOrigin[2] = DotProduct(delta, or->axis[2]) * axisLength;
}

/*
=================
R_RotateForViewer

Sets up the modelview matrix for a given viewParm
=================
*/
void R_RotateForViewer(void) {
	float viewerMatrix[16];
	vec3_t origin;

	Com_Memset(&tr.or, 0, sizeof(tr.or));
	tr.or.axis[0][0] = 1;
	tr.or.axis[1][1] = 1;
	tr.or.axis[2][2] = 1;
	VectorCopy(tr.viewParms.or.origin, tr.or.viewOrigin);

	// transform by the camera placement
	VectorCopy(tr.viewParms.or.origin, origin);

	viewerMatrix[0] = tr.viewParms.or.axis[0][0];
	viewerMatrix[4] = tr.viewParms.or.axis[0][1];
	viewerMatrix[8] = tr.viewParms.or.axis[0][2];
	viewerMatrix[12] = -origin[0] * viewerMatrix[0] + -origin[1] * viewerMatrix[4] + -origin[2] * viewerMatrix[8];

	viewerMatrix[1] = tr.viewParms.or.axis[1][0];
	viewerMatrix[5] = tr.viewParms.or.axis[1][1];
	viewerMatrix[9] = tr.viewParms.or.axis[1][2];
	viewerMatrix[13] = -origin[0] * viewerMatrix[1] + -origin[1] * viewerMatrix[5] + -origin[2] * viewerMatrix[9];

	viewerMatrix[2] = tr.viewParms.or.axis[2][0];
	viewerMatrix[6] = tr.viewParms.or.axis[2][1];
	viewerMatrix[10] = tr.viewParms.or.axis[2][2];
	viewerMatrix[14] = -origin[0] * viewerMatrix[2] + -origin[1] * viewerMatrix[6] + -origin[2] * viewerMatrix[10];

	viewerMatrix[3] = 0;
	viewerMatrix[7] = 0;
	viewerMatrix[11] = 0;
	viewerMatrix[15] = 1;

	// convert from our coordinate system (looking down X)
	// to OpenGL's coordinate system (looking down -Z)
	myGlMultMatrix(viewerMatrix, s_flipMatrix, tr.or.modelMatrix);

	tr.viewParms.world = tr.or;
}

/*
** SetFarClip
*/
static void R_SetFarClip(void) {
	float farthestCornerDistance = 0;
	int i;

	// if not rendering the world (icons, menus, etc)
	// set a 2k far clip plane
	if(tr.refdef.rdflags & RDF_NOWORLDMODEL) {
		tr.viewParms.zFar = 2048;
		return;
	}

	/*
	 * Portal/mirror views use one fixed far clip distance derived only from
	 * the size of the whole map. Do not measure from the virtual camera: its
	 * position changes at every portal transition, which made the visible
	 * depth shrink/grow as the player moved or looked through another portal.
	 *
	 * The diagonal of the world-model AABB is the maximum distance between
	 * any two points inside the map bounds. Add a small fixed safety margin
	 * for brush/entity geometry just outside those bounds. This value is
	 * identical for every portal and every recursion level.
	 */
	if(tr.viewParms.isPortal) {
		if(tr.world && tr.world->numBModels > 0) {
			const bmodel_t* worldModel = &tr.world->bmodels[0];
			vec3_t mapSize;
			float mapDiagonal;

			for(i = 0; i < 3; i++) {
				mapSize[i] = fabs(worldModel->bounds[1][i] - worldModel->bounds[0][i]);
			}

			mapDiagonal = VectorLength(mapSize);
			if(mapDiagonal > 1.0f) {
				tr.viewParms.zFar = MAX(mapDiagonal + 1024.0f, 4096.0f);
			} else {
				// Defensive fallback for a malformed/empty world-model bounds.
				tr.viewParms.zFar = 65536.0f;
			}
		} else {
			// Portal views without a loaded world model should still have a
			// generous, deterministic depth range.
			tr.viewParms.zFar = 65536.0f;
		}
		return;
	}

	// Set far clipping planes dynamically for the primary view.
	farthestCornerDistance = 0;
	for(i = 0; i < 8; i++) {
		vec3_t v;
		vec3_t vecTo;
		float distance;

		if(i & 1) {
			v[0] = tr.viewParms.visBounds[0][0];
		} else {
			v[0] = tr.viewParms.visBounds[1][0];
		}

		if(i & 2) {
			v[1] = tr.viewParms.visBounds[0][1];
		} else {
			v[1] = tr.viewParms.visBounds[1][1];
		}

		if(i & 4) {
			v[2] = tr.viewParms.visBounds[0][2];
		} else {
			v[2] = tr.viewParms.visBounds[1][2];
		}

		VectorSubtract(v, tr.viewParms.or.origin, vecTo);

		distance = vecTo[0] * vecTo[0] + vecTo[1] * vecTo[1] + vecTo[2] * vecTo[2];

		if(distance > farthestCornerDistance) {
			farthestCornerDistance = distance;
		}
	}
	tr.viewParms.zFar = sqrt(farthestCornerDistance);
}

/*
=================
R_SetupFrustum

Set up the culling frustum planes for the current view using the results we got from computing the first two rows of
the projection matrix.
=================
*/
void R_SetupFrustum(viewParms_t* dest, float xmin, float xmax, float ymax, float zProj, float zFar) {
	vec3_t ofsorigin;
	float oppleg, adjleg, length;
	int i;

	// symmetric case can be simplified
	VectorCopy(dest->or.origin, ofsorigin);

	length = sqrt(xmax * xmax + zProj * zProj);
	oppleg = xmax / length;
	adjleg = zProj / length;

	VectorScale(dest->or.axis[0], oppleg, dest->frustum[0].normal);
	VectorMA(dest->frustum[0].normal, adjleg, dest->or.axis[1], dest->frustum[0].normal);

	VectorScale(dest->or.axis[0], oppleg, dest->frustum[1].normal);
	VectorMA(dest->frustum[1].normal, -adjleg, dest->or.axis[1], dest->frustum[1].normal);

	length = sqrt(ymax * ymax + zProj * zProj);
	oppleg = ymax / length;
	adjleg = zProj / length;

	VectorScale(dest->or.axis[0], oppleg, dest->frustum[2].normal);
	VectorMA(dest->frustum[2].normal, adjleg, dest->or.axis[2], dest->frustum[2].normal);

	VectorScale(dest->or.axis[0], oppleg, dest->frustum[3].normal);
	VectorMA(dest->frustum[3].normal, -adjleg, dest->or.axis[2], dest->frustum[3].normal);

	for(i = 0; i < 4; i++) {
		dest->frustum[i].type = PLANE_NON_AXIAL;
		dest->frustum[i].dist = DotProduct(ofsorigin, dest->frustum[i].normal);
		SetPlaneSignbits(&dest->frustum[i]);
	}

	if(zFar != 0.0f) {
		vec3_t farpoint;

		VectorMA(ofsorigin, zFar, dest->or.axis[0], farpoint);
		VectorScale(dest->or.axis[0], -1.0f, dest->frustum[4].normal);

		dest->frustum[4].type = PLANE_NON_AXIAL;
		dest->frustum[4].dist = DotProduct(farpoint, dest->frustum[4].normal);
		SetPlaneSignbits(&dest->frustum[4]);
		dest->flags |= VPF_FARPLANEFRUSTUM;
	}
}

/*
===============
R_SetupProjection
===============
*/
void R_SetupProjection(viewParms_t* dest, float zProj, float zFar, bool computeFrustum) {
	float xmin, xmax, ymin, ymax;
	float width, height, stereoSep = 0;

	/*
	 * offset the view origin of the viewer for stereo rendering
	 * by setting the projection matrix appropriately.
	 */

	ymax = zProj * tan(dest->fovY * M_PI / 360.0f);
	ymin = -ymax;

	xmax = zProj * tan(dest->fovX * M_PI / 360.0f);
	xmin = -xmax;

	width = xmax - xmin;
	height = ymax - ymin;

	dest->projectionMatrix[0] = 2 * zProj / width;
	dest->projectionMatrix[4] = 0;
	dest->projectionMatrix[8] = (xmax + xmin + 2 * stereoSep) / width;
	dest->projectionMatrix[12] = 2 * zProj * stereoSep / width;

	dest->projectionMatrix[1] = 0;
	dest->projectionMatrix[5] = 2 * zProj / height;
	dest->projectionMatrix[9] = (ymax + ymin) / height;  // normally 0
	dest->projectionMatrix[13] = 0;

	dest->projectionMatrix[3] = 0;
	dest->projectionMatrix[7] = 0;
	dest->projectionMatrix[11] = -1;
	dest->projectionMatrix[15] = 0;

	// Now that we have all the data for the projection matrix we can also setup the view frustum.
	if(computeFrustum) R_SetupFrustum(dest, xmin, xmax, ymax, zProj, zFar);
}

/*
===============
R_SetupProjectionZ

Sets the z-component transformation part in the projection matrix
===============
*/
static float R_PortalSignNotZero(float value) {
	// Lengyel's oblique near-plane projection needs an actual far-frustum
	// corner. Returning zero for a zero plane component selects the centre ray
	// instead and can make the resulting far plane collapse toward the centre.
	return value >= 0.0f ? 1.0f : -1.0f;
}

void R_SetupProjectionZ(viewParms_t* dest) {
	float zNear, zFar, depth;

	zNear = 1.0f;
	zFar = dest->zFar;

	depth = zFar - zNear;

	dest->projectionMatrix[2] = 0;
	dest->projectionMatrix[6] = 0;
	dest->projectionMatrix[10] = -(zFar + zNear) / depth;
	dest->projectionMatrix[14] = -2 * zFar * zNear / depth;

	if(dest->isPortal) {
		float plane[4];
		float plane2[4];
		vec4_t q, c;

		// transform portal plane into camera space
		plane[0] = dest->portalPlane.normal[0];
		plane[1] = dest->portalPlane.normal[1];
		plane[2] = dest->portalPlane.normal[2];
		plane[3] = dest->portalPlane.dist;

		plane2[0] = -DotProduct(dest->or.axis[1], plane);
		plane2[1] = DotProduct(dest->or.axis[2], plane);
		plane2[2] = -DotProduct(dest->or.axis[0], plane);
		plane2[3] = DotProduct(plane, dest->or.origin) - plane[3];

		// Lengyel, Eric. "Modifying the Projection Matrix to Perform Oblique Near-plane Clipping".
		// Terathon Software 3D Graphics Library, 2004. http://www.terathon.com/code/oblique.html
		q[0] = (R_PortalSignNotZero(plane2[0]) + dest->projectionMatrix[8]) / dest->projectionMatrix[0];
		q[1] = (R_PortalSignNotZero(plane2[1]) + dest->projectionMatrix[9]) / dest->projectionMatrix[5];
		q[2] = -1.0f;
		q[3] = (1.0f + dest->projectionMatrix[10]) / dest->projectionMatrix[14];

		{
			float clipDot = DotProduct4(plane2, q);

			/*
			 * With the portal plane oriented into the visible destination
			 * half-space and q chosen as the far corner, clipDot must be positive.
			 * A negative value indicates an invalid/back-facing clip plane; using
			 * fabs() here used to accept it and could turn the far plane into a
			 * narrow wedge whenever the view moved off the portal centreline.
			 */
			if(clipDot > 0.000001f) {
				VectorScale4(plane2, 2.0f / clipDot, c);

				dest->projectionMatrix[2] = c[0];
				dest->projectionMatrix[6] = c[1];
				dest->projectionMatrix[10] = c[2] + 1.0f;
				dest->projectionMatrix[14] = c[3];
			}
		}
	}
}

/*
=================
R_SetupFrustumFromProjection

Extract the culling planes from the final projection matrix. Oblique portal
near-plane clipping changes the projection's far plane at oblique angles.
Using the old symmetric frustum here culls valid BSP surfaces into a wedge,
which was the apparent zFar problem. The extracted planes use the same clip
inequalities as OpenGL, so CPU BSP culling and GPU clipping agree.
=================
*/
static void R_SetupFrustumFromProjection(viewParms_t* dest) {
	float row[4][4];
	float clipPlanes[5][4];
	int i, j;

	// projectionMatrix is column-major; form its four mathematical rows.
	row[0][0] = dest->projectionMatrix[0];
	row[0][1] = dest->projectionMatrix[4];
	row[0][2] = dest->projectionMatrix[8];
	row[0][3] = dest->projectionMatrix[12];
	row[1][0] = dest->projectionMatrix[1];
	row[1][1] = dest->projectionMatrix[5];
	row[1][2] = dest->projectionMatrix[9];
	row[1][3] = dest->projectionMatrix[13];
	row[2][0] = dest->projectionMatrix[2];
	row[2][1] = dest->projectionMatrix[6];
	row[2][2] = dest->projectionMatrix[10];
	row[2][3] = dest->projectionMatrix[14];
	row[3][0] = dest->projectionMatrix[3];
	row[3][1] = dest->projectionMatrix[7];
	row[3][2] = dest->projectionMatrix[11];
	row[3][3] = dest->projectionMatrix[15];

	// OpenGL's inside half-spaces: left, right, bottom, top and far.
	for(j = 0; j < 4; j++) {
		clipPlanes[0][j] = row[3][j] + row[0][j];
		clipPlanes[1][j] = row[3][j] - row[0][j];
		clipPlanes[2][j] = row[3][j] + row[1][j];
		clipPlanes[3][j] = row[3][j] - row[1][j];
		clipPlanes[4][j] = row[3][j] - row[2][j];
	}

	for(i = 0; i < 5; i++) {
		vec3_t normal;
		float dist, normalLength;

		/*
		 * World-to-eye coordinates are (-axis1, +axis2, -axis0),
		 * matching R_RotateForViewer() and s_flipMatrix.
		 */
		VectorClear(normal);
		VectorMA(normal, -clipPlanes[i][0], dest->or.axis[1], normal);
		VectorMA(normal, clipPlanes[i][1], dest->or.axis[2], normal);
		VectorMA(normal, -clipPlanes[i][2], dest->or.axis[0], normal);
		dist = DotProduct(normal, dest->or.origin) - clipPlanes[i][3];

		normalLength = VectorNormalize(normal);
		if(normalLength <= 0.000001f) {
			// Keep a harmless plane rather than generating NaNs in BSP culling.
			VectorSet(normal, 0.0f, 0.0f, 1.0f);
			dist = -1.0e30f;
		} else {
			dist /= normalLength;
		}

		VectorCopy(normal, dest->frustum[i].normal);
		dest->frustum[i].dist = dist;
		dest->frustum[i].type = PLANE_NON_AXIAL;
		SetPlaneSignbits(&dest->frustum[i]);
	}

	dest->flags |= VPF_FARPLANEFRUSTUM;
}

/*
===============
R_SetupProjectionOrtho
===============
*/
void R_SetupProjectionOrtho(viewParms_t* dest, vec3_t viewBounds[2]) {
	float xmin, xmax, ymin, ymax, znear, zfar;
	// viewParms_t *dest = &tr.viewParms;
	int i;
	vec3_t pop;

	// Quake3:   Projection:
	//
	//    Z  X   Y  Z
	//    | /    | /
	//    |/     |/
	// Y--+      +--X

	xmin = viewBounds[0][1];
	xmax = viewBounds[1][1];
	ymin = -viewBounds[1][2];
	ymax = -viewBounds[0][2];
	znear = viewBounds[0][0];
	zfar = viewBounds[1][0];

	dest->projectionMatrix[0] = 2 / (xmax - xmin);
	dest->projectionMatrix[4] = 0;
	dest->projectionMatrix[8] = 0;
	dest->projectionMatrix[12] = (xmax + xmin) / (xmax - xmin);

	dest->projectionMatrix[1] = 0;
	dest->projectionMatrix[5] = 2 / (ymax - ymin);
	dest->projectionMatrix[9] = 0;
	dest->projectionMatrix[13] = (ymax + ymin) / (ymax - ymin);

	dest->projectionMatrix[2] = 0;
	dest->projectionMatrix[6] = 0;
	dest->projectionMatrix[10] = -2 / (zfar - znear);
	dest->projectionMatrix[14] = -(zfar + znear) / (zfar - znear);

	dest->projectionMatrix[3] = 0;
	dest->projectionMatrix[7] = 0;
	dest->projectionMatrix[11] = 0;
	dest->projectionMatrix[15] = 1;

	VectorScale(dest->or.axis[1], 1.0f, dest->frustum[0].normal);
	VectorMA(dest->or.origin, viewBounds[0][1], dest->frustum[0].normal, pop);
	dest->frustum[0].dist = DotProduct(pop, dest->frustum[0].normal);

	VectorScale(dest->or.axis[1], -1.0f, dest->frustum[1].normal);
	VectorMA(dest->or.origin, -viewBounds[1][1], dest->frustum[1].normal, pop);
	dest->frustum[1].dist = DotProduct(pop, dest->frustum[1].normal);

	VectorScale(dest->or.axis[2], 1.0f, dest->frustum[2].normal);
	VectorMA(dest->or.origin, viewBounds[0][2], dest->frustum[2].normal, pop);
	dest->frustum[2].dist = DotProduct(pop, dest->frustum[2].normal);

	VectorScale(dest->or.axis[2], -1.0f, dest->frustum[3].normal);
	VectorMA(dest->or.origin, -viewBounds[1][2], dest->frustum[3].normal, pop);
	dest->frustum[3].dist = DotProduct(pop, dest->frustum[3].normal);

	VectorScale(dest->or.axis[0], -1.0f, dest->frustum[4].normal);
	VectorMA(dest->or.origin, -viewBounds[1][0], dest->frustum[4].normal, pop);
	dest->frustum[4].dist = DotProduct(pop, dest->frustum[4].normal);

	for(i = 0; i < 5; i++) {
		dest->frustum[i].type = PLANE_NON_AXIAL;
		SetPlaneSignbits(&dest->frustum[i]);
	}

	dest->flags |= VPF_FARPLANEFRUSTUM;
}

/*
=================
R_MirrorPoint
=================
*/
void R_MirrorPoint(vec3_t in, orientation_t* surface, orientation_t* camera, vec3_t out) {
	int i;
	vec3_t local;
	vec3_t transformed;
	float d;

	VectorSubtract(in, surface->origin, local);

	VectorClear(transformed);
	for(i = 0; i < 3; i++) {
		d = DotProduct(local, surface->axis[i]);
		VectorMA(transformed, d, camera->axis[i], transformed);
	}

	VectorAdd(transformed, camera->origin, out);
}

void R_MirrorVector(vec3_t in, orientation_t* surface, orientation_t* camera, vec3_t out) {
	int i;
	float d;

	VectorClear(out);
	for(i = 0; i < 3; i++) {
		d = DotProduct(in, surface->axis[i]);
		VectorMA(out, d, camera->axis[i], out);
	}
}

/*
=============
R_PlaneForSurface
=============
*/
void R_PlaneForSurface(surfaceType_t* surfType, cplane_t* plane) {
	srfBspSurface_t* tri;
	srfPoly_t* poly;
	srfVert_t *v1, *v2, *v3;
	vec4_t plane4;

	if(!surfType) {
		Com_Memset(plane, 0, sizeof(*plane));
		plane->normal[0] = 1;
		return;
	}
	switch(*surfType) {
		case SF_FACE: *plane = ((srfBspSurface_t*)surfType)->cullPlane; return;
		case SF_TRIANGLES:
			tri = (srfBspSurface_t*)surfType;
			v1 = tri->verts + tri->indexes[0];
			v2 = tri->verts + tri->indexes[1];
			v3 = tri->verts + tri->indexes[2];
			PlaneFromPoints(plane4, v1->xyz, v2->xyz, v3->xyz);
			VectorCopy(plane4, plane->normal);
			plane->dist = plane4[3];
			return;
		case SF_POLY:
			poly = (srfPoly_t*)surfType;
			PlaneFromPoints(plane4, poly->verts[0].xyz, poly->verts[1].xyz, poly->verts[2].xyz);
			VectorCopy(plane4, plane->normal);
			plane->dist = plane4[3];
			return;
		default:
			Com_Memset(plane, 0, sizeof(*plane));
			plane->normal[0] = 1;
			return;
	}
}

/*
=================
R_PortalBuildStableFrame

Build a portal frame from its inward face normal. The frame's Up axis is
anchored to world +Z (projected onto the portal plane), so matching doorways
cannot acquire arbitrary 180-degree roll flips from BSP vertex winding.
=================
*/
static void R_PortalBuildStableFrame(const vec3_t origin, const vec3_t inwardNormal, orientation_t* frame) {
	vec3_t upReference;

	VectorCopy(origin, frame->origin);
	VectorCopy(inwardNormal, frame->axis[0]);
	if(VectorNormalize(frame->axis[0]) <= 0.0001f) {
		VectorSet(frame->axis[0], 1.0f, 0.0f, 0.0f);
	}

	// Project world up onto the portal plane. For normal upright door portals
	// this is exactly (0, 0, 1), independent of face winding or edge order.
	VectorSet(upReference, 0.0f, 0.0f, 1.0f);
	VectorMA(upReference, -DotProduct(upReference, frame->axis[0]), frame->axis[0], upReference);
	if(VectorNormalize(upReference) <= 0.0001f) {
		// A horizontal portal has no unique world-up projection. Use world +Y
		// as a deterministic fallback, then +X for the fully degenerate case.
		VectorSet(upReference, 0.0f, 1.0f, 0.0f);
		VectorMA(upReference, -DotProduct(upReference, frame->axis[0]), frame->axis[0], upReference);
		if(VectorNormalize(upReference) <= 0.0001f) {
			VectorSet(upReference, 1.0f, 0.0f, 0.0f);
			VectorMA(upReference, -DotProduct(upReference, frame->axis[0]), frame->axis[0], upReference);
			VectorNormalize(upReference);
		}
	}

	VectorCopy(upReference, frame->axis[2]);
	CrossProduct(frame->axis[2], frame->axis[0], frame->axis[1]);
	VectorNormalize(frame->axis[1]);
	CrossProduct(frame->axis[0], frame->axis[1], frame->axis[2]);
	VectorNormalize(frame->axis[2]);
}

/*
=================
R_PortalFrameToWorld

Transform a portal face center and inward normal from brush-local coordinates
using the orientation of the currently rendered func_portal_in entity.
=================
*/
static void R_PortalFrameToWorld(const portalInfo_t* portal, orientation_t* frame) {
	vec3_t worldOrigin, worldNormal;
	int i;

	VectorCopy(tr.or.origin, worldOrigin);
	VectorClear(worldNormal);
	for(i = 0; i < 3; i++) {
		VectorMA(worldOrigin, portal->localOrigin[i], tr.or.axis[i], worldOrigin);
		VectorMA(worldNormal, portal->localNormal[i], tr.or.axis[i], worldNormal);
	}
	R_PortalBuildStableFrame(worldOrigin, worldNormal, frame);
}

static void R_PortalFrameFromMap(const portalInfo_t* portal, orientation_t* frame) {
	vec3_t entityAxis[3];
	vec3_t worldOrigin, worldNormal;
	int i;

	AnglesToAxis(portal->angles, entityAxis);
	VectorCopy(portal->origin, worldOrigin);
	VectorClear(worldNormal);
	for(i = 0; i < 3; i++) {
		VectorMA(worldOrigin, portal->localOrigin[i], entityAxis[i], worldOrigin);
		VectorMA(worldNormal, portal->localNormal[i], entityAxis[i], worldNormal);
	}
	R_PortalBuildStableFrame(worldOrigin, worldNormal, frame);
}

/*
=================
R_PortalOrthonormalizeViewAxis

Remove tiny floating-point drift after each recursive portal transform while
preserving the transformed forward and up directions (and therefore roll).
=================
*/
static void R_PortalOrthonormalizeViewAxis(vec3_t axis[3]) {
	if(VectorNormalize(axis[0]) <= 0.0001f) return;
	VectorMA(axis[2], -DotProduct(axis[2], axis[0]), axis[0], axis[2]);
	if(VectorNormalize(axis[2]) <= 0.0001f) {
		VectorMA(axis[1], -DotProduct(axis[1], axis[0]), axis[0], axis[1]);
		if(VectorNormalize(axis[1]) <= 0.0001f) return;
		CrossProduct(axis[0], axis[1], axis[2]);
		VectorNormalize(axis[2]);
	} else {
		CrossProduct(axis[2], axis[0], axis[1]);
		VectorNormalize(axis[1]);
		CrossProduct(axis[0], axis[1], axis[2]);
		VectorNormalize(axis[2]);
	}
}

/*
=================
R_GetPortalOrientations

Resolve the visible func_portal_in brush to its paired func_portal_out endpoint.
No RT_PORTALSURFACE / misc_portal_surface entity is involved.
=================
*/
static bool R_GetPortalOrientations(int entityNum, orientation_t* surface, orientation_t* camera, vec3_t pvsOrigin, bool* mirror) {
	const refEntity_t* entity;
	portalInfo_t* input = NULL;
	portalInfo_t* output;
	int i;

	if(!tr.world || entityNum < 0 || entityNum >= tr.refdef.num_entities) return false;
	entity = &tr.refdef.entities[entityNum].e;

	for(i = 0; i < tr.world->numPortals; i++) {
		if(tr.world->portals[i].isInput && tr.world->portals[i].modelHandle == entity->hModel) {
			vec3_t delta;
			VectorSubtract(entity->origin, tr.world->portals[i].origin, delta);
			if(VectorLengthSquared(delta) < 1.0f) {
				input = &tr.world->portals[i];
				break;
			}
			// Keep a model-handle fallback for callers that apply an entity offset.
			if(!input) input = &tr.world->portals[i];
		}
	}
	if(!input || input->targetIndex < 0 || input->targetIndex >= tr.world->numPortals) return false;

	output = &tr.world->portals[input->targetIndex];
	if(output->isInput) return false;

	// The source endpoint may be submitted as a normal inline brush model.
	R_RotateForEntity(&tr.refdef.entities[entityNum], &tr.viewParms, &tr.or);
	R_PortalFrameToWorld(input, surface);
	R_PortalFrameFromMap(output, camera);

	// Source-style portal transform: rotate the destination frame 180 degrees
	// around its stable Up axis. The same frame transform is applied to camera
	// position and all three view axes at every recursion depth.
	VectorScale(camera->axis[0], -1.0f, camera->axis[0]);
	VectorScale(camera->axis[1], -1.0f, camera->axis[1]);

	VectorCopy(camera->origin, pvsOrigin);
	// axis[0] was reversed above, so it points toward the side where the
	// remote camera is placed. Offset the PVS origin just off the exit plane.
	VectorMA(pvsOrigin, 2.0f, camera->axis[0], pvsOrigin);

	*mirror = false;
	return true;
}

static bool R_GetMirrorOrientations(drawSurf_t* drawSurf, int entityNum, orientation_t* surface, orientation_t* camera) {
	cplane_t localPlane, worldPlane;
	int i;

	float normalLength;

	R_PlaneForSurface(drawSurf->surface, &localPlane);
	normalLength = VectorNormalize(localPlane.normal);
	if(normalLength <= 0.0001f) return false;
	localPlane.dist /= normalLength;

	if(entityNum != REFENTITYNUM_WORLD) {
		if(entityNum < 0 || entityNum >= tr.refdef.num_entities) return false;
		tr.currentEntityNum = entityNum;
		tr.currentEntity = &tr.refdef.entities[entityNum];
		R_RotateForEntity(tr.currentEntity, &tr.viewParms, &tr.or);
		R_LocalNormalToWorld(localPlane.normal, worldPlane.normal);
		normalLength = VectorLength(worldPlane.normal);
		if(normalLength <= 0.0001f) return false;
		worldPlane.dist = (localPlane.dist + DotProduct(worldPlane.normal, tr.or.origin)) / normalLength;
		VectorScale(worldPlane.normal, 1.0f / normalLength, worldPlane.normal);
	} else {
		worldPlane = localPlane;
	}

	VectorCopy(worldPlane.normal, surface->axis[0]);
	PerpendicularVector(surface->axis[1], surface->axis[0]);
	CrossProduct(surface->axis[0], surface->axis[1], surface->axis[2]);
	VectorScale(surface->axis[0], worldPlane.dist, surface->origin);

	VectorCopy(surface->origin, camera->origin);
	VectorScale(surface->axis[0], -1.0f, camera->axis[0]);
	VectorCopy(surface->axis[1], camera->axis[1]);
	VectorCopy(surface->axis[2], camera->axis[2]);

	for(i = 0; i < 3; i++) VectorNormalize(camera->axis[i]);
	return true;
}

/*
** R_GetViewScissorRect
**
** Returns the active scissor rectangle, or the full viewport when none is set.
*/
static void R_GetViewScissorRect(const viewParms_t* parms, int rect[4]) {
	if(parms->scissorWidth > 0 && parms->scissorHeight > 0) {
		rect[0] = parms->scissorX;
		rect[1] = parms->scissorY;
		rect[2] = parms->scissorX + parms->scissorWidth;
		rect[3] = parms->scissorY + parms->scissorHeight;
	} else {
		rect[0] = parms->viewportX;
		rect[1] = parms->viewportY;
		rect[2] = parms->viewportX + parms->viewportWidth;
		rect[3] = parms->viewportY + parms->viewportHeight;
	}
}

/*
** SurfIsOffscreen
**
** Determines if a surface is completely offscreen and, when visible, returns
** a conservative screen-space rectangle for its aperture. The child portal
** view keeps the parent's projection viewport and uses this rectangle only
** as a scissor, so each visible portal paints its own part of the framebuffer.
*/
static bool SurfIsOffscreen(const drawSurf_t* drawSurf, int outScissor[4]) {
	float shortest = 100000000;
	int entityNum;
	int numTriangles;
	shader_t* shader;
	int fogNum;
	int dlighted;
	vec4_t clip, eye;
	int i;
	unsigned int pointAnd = (unsigned int)~0;
	float minX = 1.0e30f, minY = 1.0e30f;
	float maxX = -1.0e30f, maxY = -1.0e30f;
	bool crossesEye = false;
	int parentScissor[4];

	R_DecomposeSort(drawSurf->sort, &entityNum, &shader, &fogNum, &dlighted);

	if(entityNum == REFENTITYNUM_WORLD) {
		R_RotateForViewer();
	} else {
		if(entityNum < 0 || entityNum >= tr.refdef.num_entities) return true;
		tr.currentEntityNum = entityNum;
		tr.currentEntity = &tr.refdef.entities[entityNum];
		R_RotateForEntity(tr.currentEntity, &tr.viewParms, &tr.or);
	}

	RB_BeginSurface(shader, fogNum, drawSurf->useCubemap, drawSurf->cubemapOrigin, drawSurf->cubemapBound);
	rb_surfaceTable[*drawSurf->surface](drawSurf->surface);

	assert(tess.numVertexes < 128);

	for(i = 0; i < tess.numVertexes; i++) {
		int j;
		unsigned int pointFlags = 0;
		float sx, sy;

		R_TransformModelToClip(tess.xyz[i], tr.or.modelMatrix, tr.viewParms.projectionMatrix, eye, clip);

		for(j = 0; j < 3; j++) {
			if(clip[j] >= clip[3]) {
				pointFlags |= (1 << (j * 2));
			} else if(clip[j] <= -clip[3]) {
				pointFlags |= (1 << (j * 2 + 1));
			}
		}
		pointAnd &= pointFlags;

		// A vertex behind the eye can make projected bounds unbounded. In that
		// unusual case use the inherited scissor rather than risk clipping the
		// portal image away.
		if(clip[3] <= 0.001f) {
			crossesEye = true;
			continue;
		}

		sx = tr.viewParms.viewportX + (clip[0] / clip[3] * 0.5f + 0.5f) * tr.viewParms.viewportWidth;
		sy = tr.viewParms.viewportY + (clip[1] / clip[3] * 0.5f + 0.5f) * tr.viewParms.viewportHeight;
		if(sx < minX) minX = sx;
		if(sx > maxX) maxX = sx;
		if(sy < minY) minY = sy;
		if(sy > maxY) maxY = sy;
	}

	// trivially reject
	if(pointAnd) return true;

	// determine if the surface is backfaced and also determine the distance
	// to the nearest vertex so we can cull based on portal range.
	numTriangles = tess.numIndexes / 3;

	for(i = 0; i < tess.numIndexes; i += 3) {
		vec3_t normal, tNormal;
		float len;

		VectorSubtract(tess.xyz[tess.indexes[i]], tr.or.viewOrigin, normal);
		len = VectorLengthSquared(normal);
		if(len < shortest) shortest = len;

		R_VaoUnpackNormal(tNormal, tess.normal[tess.indexes[i]]);
		if(DotProduct(normal, tNormal) >= 0) numTriangles--;
	}
	if(!numTriangles) return true;

	// Mirror surfaces do not fade out by portalRange. Portal surfaces still use
	// the configured range as a cheap visibility limit.
	if(tess.shader->isMirror) {
		// no range test
	} else if(tess.shader->isPortal && shortest > (tess.shader->portalRange * tess.shader->portalRange)) {
		return true;
	}

	R_GetViewScissorRect(&tr.viewParms, parentScissor);
	if(crossesEye || minX > maxX || minY > maxY) {
		outScissor[0] = parentScissor[0];
		outScissor[1] = parentScissor[1];
		outScissor[2] = parentScissor[2];
		outScissor[3] = parentScissor[3];
	} else {
		int x0 = (int)floorf(minX);
		int y0 = (int)floorf(minY);
		int x1 = (int)ceilf(maxX);
		int y1 = (int)ceilf(maxY);

		// Clamp to this view's viewport before intersecting the inherited
		// scissor. The resulting rectangle is conservative for clipped faces.
		if(x0 < tr.viewParms.viewportX) x0 = tr.viewParms.viewportX;
		if(y0 < tr.viewParms.viewportY) y0 = tr.viewParms.viewportY;
		if(x1 > tr.viewParms.viewportX + tr.viewParms.viewportWidth) x1 = tr.viewParms.viewportX + tr.viewParms.viewportWidth;
		if(y1 > tr.viewParms.viewportY + tr.viewParms.viewportHeight) y1 = tr.viewParms.viewportY + tr.viewParms.viewportHeight;

		outScissor[0] = MAX(x0, parentScissor[0]);
		outScissor[1] = MAX(y0, parentScissor[1]);
		outScissor[2] = MIN(x1, parentScissor[2]);
		outScissor[3] = MIN(y1, parentScissor[3]);
	}

	if(outScissor[2] <= outScissor[0] || outScissor[3] <= outScissor[1]) return true;
	return false;
}

/*
========================
R_RenderPortalSkybox

Render the map's existing 3D skybox from a portal's virtual camera. The
portal aperture scissor and target framebuffer are inherited from the portal
view. This is only called for recursion level one; nested portal views keep
the already-rendered parent skybox in the color buffer.
========================
*/
static bool R_RenderPortalSkybox(viewParms_t* portalParms) {
	viewParms_t skyParms;
	bool insideSkyboxVolume = false;

	if(!portalParms || !tr.world || !tr.world->hasSkybox || !r_3dsky->integer || tr.hideSky) return false;
	if(tr.world->skyboxScale <= 0.0001f) return false;

	skyParms = *portalParms;

	// Match RE_RenderScene's existing misc_skybox camera placement. If the
	// virtual camera is inside any individual VOLUME_SKY brush area, it is
	// already expressed in mini-world coordinates and must not be scaled.
	insideSkyboxVolume = R_PointInSkyGroupVolume(tr.world, portalParms->or.origin);

	if(insideSkyboxVolume) {
		VectorCopy(portalParms->or.origin, skyParms.or.origin);
	} else {
		VectorMA(tr.world->skyboxOrigin, 1.0f / tr.world->skyboxScale, portalParms->or.origin, skyParms.or.origin);
	}
	VectorCopy(skyParms.or.origin, skyParms.pvsOrigin);

	// The skybox is a separate mini-world pass: it must not inherit the
	// portal clip plane, mirror winding, sunlight/shadow flags, or recursion.
	// Keep the portal viewport/scissor/FBO so only this aperture is painted.
	skyParms.isPortal = false;
	skyParms.isMirror = false;
	skyParms.portalRecursion = 8;
	skyParms.flags = VPF_SKYBOX3D | VPF_NOVIEWMODEL;
	skyParms.hasWaterReflection = false;

	R_RenderView(&skyParms);
	portalParms->flags |= VPF_SKYDRAWN;
	return true;
}

/*
========================
R_MirrorViewBySurface

Returns true if another view has been rendered
========================
*/
bool R_MirrorViewBySurface(drawSurf_t* drawSurf, int entityNum) {
	viewParms_t newParms;
	viewParms_t oldParms;
	orientation_t surface, camera;
	int portalScissor[4];
	int recursionLimit;

	// r_recursionCount is the number of portal/mirror transitions allowed
	// below the primary camera. Clamp it to keep malformed maps from causing
	// unbounded recursive view generation.
	recursionLimit = Com_Clamp(0, 8, r_recursionCount ? r_recursionCount->integer : 2);
	if(tr.viewParms.portalRecursion >= recursionLimit) return false;

	// func_occlusion: зеркало мира, которого не видно (оклюзия прошлого кадра), — целый
	// лишний вид сцены не строим. Сама поверхность рисуется как обычно.
	if(entityNum == REFENTITYNUM_WORLD && R_OcclusionMirrorHidden(drawSurf->surface)) return false;

	// trivially reject portal/mirror
	if(SurfIsOffscreen(drawSurf, portalScissor)) {
		return false;
	}

	// save old viewParms so we can return to it after the mirror view
	oldParms = tr.viewParms;

	newParms = tr.viewParms;
	newParms.isPortal = true;
	newParms.portalRecursion = oldParms.portalRecursion + 1;
	newParms.scissorX = portalScissor[0];
	newParms.scissorY = portalScissor[1];
	newParms.scissorWidth = portalScissor[2] - portalScissor[0];
	newParms.scissorHeight = portalScissor[3] - portalScissor[1];
	newParms.zFar = 0.0f;
	newParms.flags &= ~VPF_FARPLANEFRUSTUM;
	if(tess.shader->isMirror) {
		if(!R_GetMirrorOrientations(drawSurf, entityNum, &surface, &camera)) return false;
		newParms.isMirror = true;
		VectorCopy(surface.origin, newParms.pvsOrigin);
	} else if(tess.shader->isPortal) {
		if(!R_GetPortalOrientations(entityNum, &surface, &camera, newParms.pvsOrigin, &newParms.isMirror)) return false;
	} else {
		return false;
	}

	// Never draw viewmodels in portal or mirror views.
	newParms.flags |= VPF_NOVIEWMODEL;

	R_MirrorPoint(oldParms.or.origin, &surface, &camera, newParms.or.origin);

	// The oblique near plane must point from the virtual camera through the
	// aperture into the visible room. func_portal_in/out frames have already
	// had their destination forward axis reversed in R_GetPortalOrientations,
	// while the mirror frame uses the opposite convention.
	if(tess.shader->isPortal) {
		VectorCopy(camera.axis[0], newParms.portalPlane.normal);
	} else {
		VectorScale(camera.axis[0], -1.0f, newParms.portalPlane.normal);
	}
	newParms.portalPlane.dist = DotProduct(camera.origin, newParms.portalPlane.normal);

	// Apply the exact same source-to-destination transform to the camera basis.
	// Nested portals must compose the transform from their parent view instead
	// of copying the primary camera axes (the old behaviour lost every other turn).
	R_MirrorVector(oldParms.or.axis[0], &surface, &camera, newParms.or.axis[0]);
	R_MirrorVector(oldParms.or.axis[1], &surface, &camera, newParms.or.axis[1]);
	R_MirrorVector(oldParms.or.axis[2], &surface, &camera, newParms.or.axis[2]);
	if(tess.shader->isPortal) R_PortalOrthonormalizeViewAxis(newParms.or.axis);

	/*
	 * The primary camera may already have drawn its own 3D skybox and carries
	 * VPF_SKYDRAWN into this child view. That image belongs to the player's
	 * location, not to the portal destination. Redraw the destination skybox
	 * once for each first-level portal. Deeper recursion intentionally keeps
	 * the parent's skybox in the color buffer and does not overwrite it.
	 */
	if(tess.shader->isPortal && newParms.portalRecursion == 1 && r_portalRedrawSkybox && r_portalRedrawSkybox->integer) {
		newParms.flags &= ~VPF_SKYDRAWN;
		R_RenderPortalSkybox(&newParms);
	}

	// OPTIMIZE: restrict the viewport on the mirrored view

	// render the mirror view
	R_RenderView(&newParms);

	tr.viewParms = oldParms;

	return true;
}

/*
=================
R_SpriteFogNum

See if a sprite is inside a fog volume
=================
*/
int R_SpriteFogNum(trRefEntity_t* ent) {
	int i, j;
	fog_t* fog;

	if(tr.refdef.rdflags & RDF_NOWORLDMODEL) {
		return 0;
	}

	if(ent->e.renderfx & RF_CROSSHAIR) {
		return 0;
	}

	for(i = 1; i < tr.world->numfogs; i++) {
		fog = &tr.world->fogs[i];
		for(j = 0; j < 3; j++) {
			if(ent->e.origin[j] - ent->e.radius >= fog->bounds[1][j]) {
				break;
			}
			if(ent->e.origin[j] + ent->e.radius <= fog->bounds[0][j]) {
				break;
			}
		}
		if(j == 3) {
			return i;
		}
	}

	return 0;
}

/*
==========================================================================================

DRAWSURF SORTING

==========================================================================================
*/

/*
===============
R_Radix
===============
*/
static ID_INLINE void R_Radix(int offset, int size, drawSurf_t* source, drawSurf_t* dest) {
	int count[256] = {0};
	int index[256];
	int i;
	unsigned char* sortKey = NULL;
	unsigned char* end = NULL;

	sortKey = ((unsigned char*)&source[0].sort) + offset;
	end = sortKey + (size * sizeof(drawSurf_t));
	for(; sortKey < end; sortKey += sizeof(drawSurf_t)) ++count[*sortKey];

	index[0] = 0;

	for(i = 1; i < 256; ++i) index[i] = index[i - 1] + count[i - 1];

	sortKey = ((unsigned char*)&source[0].sort) + offset;
	for(i = 0; i < size; ++i, sortKey += sizeof(drawSurf_t)) dest[index[*sortKey]++] = source[i];
}

/*
===============
R_RadixSort

Radix sort with 4 byte size buckets
===============
*/
static void R_RadixSort(drawSurf_t* source, int size) {
	static drawSurf_t scratch[MAX_DRAWSURFS];
#ifdef Q3_LITTLE_ENDIAN
	R_Radix(0, size, source, scratch);
	R_Radix(1, size, scratch, source);
	R_Radix(2, size, source, scratch);
	R_Radix(3, size, scratch, source);
	R_Radix(4, size, source, scratch);
#else
	R_Radix(4, size, scratch, source);
	R_Radix(3, size, source, scratch);
	R_Radix(2, size, scratch, source);
	R_Radix(1, size, source, scratch);
	R_Radix(0, size, scratch, source);
#endif
}

//==========================================================================================

/*
=================
R_AddDrawSurf
=================
*/
void R_AddDrawSurf(surfaceType_t* surface, shader_t* shader, int fogIndex, int dlightMap, bool useCubemap, vec3_t cubemapOrigin, float cubemapBound) {
	int index = tr.refdef.numDrawSurfs & DRAWSURF_MASK;

	tr.refdef.drawSurfs[index].sort = ((uint64_t)shader->sortedIndex << QSORT_SHADERNUM_SHIFT) | ((uint64_t)tr.shiftedEntityNum) | ((uint64_t)fogIndex << QSORT_FOGNUM_SHIFT) | (uint64_t)(dlightMap & 1);

	tr.refdef.drawSurfs[index].useCubemap = useCubemap;
	VectorCopy(cubemapOrigin, tr.refdef.drawSurfs[index].cubemapOrigin);
	tr.refdef.drawSurfs[index].cubemapBound = cubemapBound;
	tr.refdef.drawSurfs[index].surface = surface;
	tr.refdef.numDrawSurfs++;
}

/*
=================
R_DecomposeSort
=================
*/
void R_DecomposeSort(uint64_t sort, int* entityNum, shader_t** shader, int* fogNum, int* dlightMap) {
	*fogNum = (sort >> QSORT_FOGNUM_SHIFT) & 31;
	*shader = tr.sortedShaders[(sort >> QSORT_SHADERNUM_SHIFT) & (MAX_SHADERS - 1)];
	*entityNum = (int)((sort >> QSORT_REFENTITYNUM_SHIFT) & (uint64_t)REFENTITYNUM_MASK);
	*dlightMap = (int)(sort & 1);
}

/*
=================
R_SortDrawSurfs
=================
*/
void R_SortDrawSurfs(drawSurf_t* drawSurfs, int numDrawSurfs) {
	shader_t* shader;
	int fogNum;
	int entityNum;
	int dlighted;
	int i;

	// it is possible for some views to not have any surfaces
	if(numDrawSurfs < 1) {
		// we still need to add it for hyperspace cases
		R_AddDrawSurfCmd(drawSurfs, numDrawSurfs);
		return;
	}

	// sort the drawsurfs by sort type, then orientation, then shader
	R_RadixSort(drawSurfs, numDrawSurfs);

	// skip pass through drawing if rendering a shadow map
	if(tr.viewParms.flags & (VPF_SHADOWMAPS | VPF_DEPTHSHADOW)) {
		R_AddDrawSurfCmd(drawSurfs, numDrawSurfs);
		return;
	}

	// check for any pass through drawing, which
	// may cause another view to be rendered first
	for(i = 0; i < numDrawSurfs; i++) {
		R_DecomposeSort((drawSurfs + i)->sort, &entityNum, &shader, &fogNum, &dlighted);

		if(shader->sort > SS_PORTAL) {
			break;
		}

		// no shader should ever have this sort type
		if(shader->sort == SS_BAD) {
			ri.Error(ERR_DROP, "Shader '%s'with sort == SS_BAD", shader->name);
		}

		// Render every visible portal/mirror surface. Each child view is
		// restricted to that aperture's screen-space scissor, so later portals
		// do not overwrite unrelated portal images. Nested views repeat this
		// pass until r_recursionCount is reached.
		R_MirrorViewBySurface((drawSurfs + i), entityNum);
	}

	R_AddDrawSurfCmd(drawSurfs, numDrawSurfs);
}

static void R_AddEntitySurface(int entityNum) {
	trRefEntity_t* ent;
	shader_t* shader;

	tr.currentEntityNum = entityNum;

	ent = tr.currentEntity = &tr.refdef.entities[tr.currentEntityNum];

	DlightMask_Clear(&ent->needDlights);

	// preshift the value we are going to OR into the drawsurf sort
	tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;

	//
	// the weapon model must be handled special --
	// we don't want the hacked weapon position showing in
	// mirrors, because the true body position will already be drawn
	//
	if((ent->e.renderfx & RF_FIRST_PERSON) && (tr.viewParms.flags & VPF_NOVIEWMODEL)) {
		return;
	}

	if((ent->e.renderfx & RF_NO_REFLECT) && (tr.viewParms.flags & VPF_CUBEMAP)) {
		return;
	}

	// simple generated models, like sprites and beams, are not culled
	switch(ent->e.reType) {
		case RT_PORTALSURFACE: break;  // legacy surface entities are ignored; portals use func_portal_in/out brushes
		case RT_SPRITE:
			// self blood sprites, talk balloons, etc should not be drawn in the primary
			// view.  We can't just do this check for all entities, because md3
			// entities may still want to cast shadows from them
			if((ent->e.renderfx & RF_THIRD_PERSON) && !tr.viewParms.isPortal) {
				return;
			}
			shader = R_GetShaderByHandle(ent->e.customShader);
			R_AddDrawSurf(&entitySurface, shader, R_SpriteFogNum(ent), 0, 0, vec3_origin, 0.0f);
			break;

		case RT_MODEL:
			// we must set up parts of tr.or for model culling
			R_RotateForEntity(ent, &tr.viewParms, &tr.or);

			tr.currentModel = R_GetModelByHandle(ent->e.hModel);
			if(!tr.currentModel) {
				R_AddDrawSurf(&entitySurface, tr.defaultShader, 0, 0, 0, vec3_origin, 0.0f);
			} else {
				switch(tr.currentModel->type) {
					case MOD_MESH: R_AddMD3Surfaces(ent); break;
					case MOD_IQM: R_AddIQMSurfaces(ent); break;
					case MOD_BRUSH: R_AddBrushModelSurfaces(ent); break;
					case MOD_BAD:  // null model axis
						if((ent->e.renderfx & RF_THIRD_PERSON) && !tr.viewParms.isPortal) {
							break;
						}
						R_AddDrawSurf(&entitySurface, tr.defaultShader, 0, 0, 0, vec3_origin, 0.0f);
						break;
					default: ri.Error(ERR_DROP, "R_AddEntitySurfaces: Bad modeltype"); break;
				}
			}
			break;
		default: ri.Error(ERR_DROP, "R_AddEntitySurfaces: Bad reType");
	}
}

/*
=============
R_AddEntitySurfaces
=============
*/
void R_AddEntitySurfaces(void) {
	int i;

	for(i = 0; i < tr.refdef.num_entities; i++) R_AddEntitySurface(i);
}

/*
====================
R_GenerateDrawSurfs
====================
*/
void R_GenerateDrawSurfs(void) {
	R_AddWorldSurfaces();

	if(!(tr.viewParms.flags & VPF_CUBEMAP)) R_AddPolygonSurfaces();

	// The portal projection (including its oblique depth row and matching
	// extracted frustum) is finalized before BSP traversal in R_RenderView.
	// The primary camera still derives zFar from visBounds after world visibility.
	if(!(tr.viewParms.flags & VPF_SHADOWMAPS) && !tr.viewParms.isPortal) R_SetFarClip();
	if(!tr.viewParms.isPortal) R_SetupProjectionZ(&tr.viewParms);

	R_AddEntitySurfaces();
}

/*
================
R_DebugPolygon
================
*/
void R_DebugPolygon(int color, int numPoints, float* points) {
	// FIXME: implement this
#if 0
	int		i;

	GL_State( GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );

	// draw solid shade

	qglColor3f( color&1, (color>>1)&1, (color>>2)&1 );
	qglBegin( GL_POLYGON );
	for ( i = 0 ; i < numPoints ; i++ ) {
		qglVertex3fv( points + i * 3 );
	}
	qglEnd();

	// draw wireframe outline
	GL_State( GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	qglDepthRange( 0, 0 );
	qglColor3f( 1, 1, 1 );
	qglBegin( GL_POLYGON );
	for ( i = 0 ; i < numPoints ; i++ ) {
		qglVertex3fv( points + i * 3 );
	}
	qglEnd();
	qglDepthRange( 0, 1 );
#endif
}

/*
=================
R_NoteWaterSurface

Called for every visible planar surface marked with the `water` shader keyword.
Stores the closest eligible world-space plane, including transformed brush faces.
=================
*/
void R_NoteWaterSurface(const msurface_t* surf) {
	const vec_t* origin = tr.viewParms.or.origin;
	cplane_t plane;
	vec3_t center, bounds[2], corner, worldCorner;
	float signedDistance, distSq = 0.0f;
	float normalLength;
	int i, bits;

	if(!r_planarReflections->integer || !tr.waterReflectionFbo || !surf->isWaterPlane) return;

	// func_occlusion: воды не видно — не выбираем её для планарного отражения (лишний вид сцены)
	if(R_OcclusionWaterHidden(surf)) return;

	// Only the primary view may choose a reflection plane. Do not recurse into
	// portals, mirrors, cubemaps, or shadow passes.
	if(tr.viewParms.isPortal || (tr.viewParms.flags & (VPF_CUBEMAP | VPF_WATERREFLECTION | VPF_SHADOWMAPS | VPF_DEPTHSHADOW | VPF_DLIGHTSHADOW)) || (tr.refdef.rdflags & RDF_NOWORLDMODEL)) return;

	// World surfaces are already in world space. Inline brush surfaces are
	// stored in model-local space and must follow the current entity transform.
	if(tr.currentEntityNum == REFENTITYNUM_WORLD) {
		plane = surf->waterPlane;
		VectorCopy(surf->waterCenter, center);
		for(i = 0; i < 3; i++) {
			bounds[0][i] = surf->cullinfo.bounds[0][i];
			bounds[1][i] = surf->cullinfo.bounds[1][i];
		}
	} else {
		R_LocalNormalToWorld(surf->waterPlane.normal, plane.normal);
		normalLength = VectorLength(plane.normal);
		if(normalLength <= 0.0001f) return;
		plane.dist = (surf->waterPlane.dist + DotProduct(plane.normal, tr.or.origin)) / normalLength;
		VectorScale(plane.normal, 1.0f / normalLength, plane.normal);
		R_LocalPointToWorld(surf->waterCenter, center);

		ClearBounds(bounds[0], bounds[1]);
		for(bits = 0; bits < 8; bits++) {
			for(i = 0; i < 3; i++) corner[i] = (bits & (1 << i)) ? surf->cullinfo.bounds[1][i] : surf->cullinfo.bounds[0][i];
			R_LocalPointToWorld(corner, worldCorner);
			AddPointToBounds(worldCorner, bounds[0], bounds[1]);
		}
	}

	// Keep the authored face orientation: only reflect when the camera is on
	// its front side. This works for vertical and sloped planes as well as Z-up.
	signedDistance = DotProduct(origin, plane.normal) - plane.dist;
	if(signedDistance <= 1.0f) return;

	// Distance from the camera to the transformed surface AABB. This lets
	// world and inline-brush water participate in the same nearest-plane choice.
	for(i = 0; i < 3; i++) {
		float d = 0.0f;
		if(origin[i] < bounds[0][i])
			d = bounds[0][i] - origin[i];
		else if(origin[i] > bounds[1][i])
			d = origin[i] - bounds[1][i];
		distSq += d * d;
	}

	if(!tr.waterFound || distSq < tr.waterFoundDistSq) {
		tr.waterFound = true;
		tr.waterFoundDistSq = distSq;
		tr.waterFoundPlane = plane;
		VectorCopy(center, tr.waterFoundCenter);
	}
}

/*
=================
R_RenderWaterReflection

Render the scene from a camera reflected across the closest visible `water`
shader plane. The plane is arbitrary in world space; it is not restricted to Z.
=================
*/
void R_RenderWaterReflection(void) {
	viewParms_t oldParms, newParms;
	orientation_t surface, camera;
	cplane_t plane = tr.waterFoundPlane;
	mat4_t viewProj;
	int i;
	float normalLength;

	normalLength = VectorNormalize(plane.normal);
	if(normalLength <= 0.0001f) return;
	plane.dist /= normalLength;

	oldParms = tr.viewParms;
	newParms = tr.viewParms;

	// Build a stable orthonormal frame with its first axis along the plane normal.
	VectorScale(plane.normal, plane.dist, surface.origin);
	VectorCopy(plane.normal, surface.axis[0]);
	PerpendicularVector(surface.axis[1], surface.axis[0]);
	CrossProduct(surface.axis[0], surface.axis[1], surface.axis[2]);
	VectorCopy(surface.origin, camera.origin);
	VectorScale(surface.axis[0], -1.0f, camera.axis[0]);
	VectorCopy(surface.axis[1], camera.axis[1]);
	VectorCopy(surface.axis[2], camera.axis[2]);

	newParms.isPortal = true;  // oblique near-plane clipping
	newParms.isMirror = true;  // mirrored axes require reversed face culling
	// Keep the historical behavior: water reflection views do not spawn a
	// second tree of portal/mirror views. r_recursionCount is for real portal
	// and mirror apertures, not for this auxiliary reflection pass.
	newParms.portalRecursion = Com_Clamp(0, 8, r_recursionCount ? r_recursionCount->integer : 2);
	newParms.zFar = 0.0f;
	newParms.flags &= ~(VPF_FARPLANEFRUSTUM | VPF_USESUNLIGHT | VPF_SKYDRAWN);
	newParms.flags |= VPF_NOVIEWMODEL | VPF_WATERREFLECTION;
	newParms.hasWaterReflection = false;

	R_MirrorPoint(oldParms.or.origin, &surface, &camera, newParms.or.origin);
	for(i = 0; i < 3; i++) R_MirrorVector(oldParms.or.axis[i], &surface, &camera, newParms.or.axis[i]);

	// Keep the half-space on the side from which the original camera viewed water.
	VectorCopy(plane.normal, newParms.portalPlane.normal);
	newParms.portalPlane.dist = plane.dist;

	VectorCopy(tr.waterFoundCenter, newParms.pvsOrigin);
	VectorMA(newParms.pvsOrigin, 2.0f, plane.normal, newParms.pvsOrigin);

	newParms.viewportX = 0;
	newParms.viewportY = 0;
	newParms.viewportWidth = tr.waterReflectionFbo->width;
	newParms.viewportHeight = tr.waterReflectionFbo->height;
	newParms.targetFbo = tr.waterReflectionFbo;
	newParms.targetFboLayer = 0;

	if(tr.world && tr.world->hasSkybox && r_3dsky->integer && !tr.hideSky && r_planarReflectionsSkybox->integer) {
		viewParms_t skyParms = newParms;
		VectorMA(tr.world->skyboxOrigin, 1.0f / tr.world->skyboxScale, newParms.or.origin, skyParms.or.origin);
		VectorCopy(skyParms.or.origin, skyParms.pvsOrigin);
		skyParms.flags = newParms.flags | VPF_SKYBOX3D;
		// The water clip plane belongs to the main world, not the separate 3D skybox.
		skyParms.isPortal = false;
		R_RenderView(&skyParms);
		newParms.flags |= VPF_SKYDRAWN | VPF_KEEPCOLOR;
	}

	R_RenderView(&newParms);
	Mat4Multiply(tr.viewParms.projectionMatrix, tr.viewParms.world.modelMatrix, viewProj);

	tr.viewParms = oldParms;
	tr.viewParms.hasWaterReflection = true;
	tr.viewParms.waterReflectPlane = plane;
	Mat4Copy(viewProj, tr.viewParms.waterReflectMatrix);
}

/*
================
R_OcclusionPrimaryView

Основной вид игрока: не портал/зеркало, не отражение воды, не кубмапа, не тени, не проход
3D-скайбокса и сцена с миром (а не 2D/viewmodel-сцена).
================
*/
static bool R_OcclusionPrimaryView(const viewParms_t* parms) {
	if(parms->isPortal || parms->portalRecursion > 0) return false;
	if(parms->flags & (VPF_SHADOWMAPS | VPF_DEPTHSHADOW | VPF_DLIGHTSHADOW | VPF_CUBEMAP | VPF_WATERREFLECTION | VPF_SKYBOX3D)) return false;
	if(tr.refdef.rdflags & RDF_NOWORLDMODEL) return false;
	return true;
}

/*
================
R_SetupVolumeRules

Правила видимости объёмов для ОДНОЙ камеры (применяет R_AddWorldSurface).

По умолчанию каждая камера получает правила сцены из RE_BeginScene:
  - вне func_display — worldspawn "volumeflags" (blacklist; по умолчанию VOLUME_SKY — мир не рисует поверхности 3D-скайбокса);
  - внутри func_display — его spawnflags (blacklist) или whitelist.

Единственное исключение — проход 3D-скайбокса (VPF_SKYBOX3D): он рендерится
СТРОГО с volumeFlags = ~VOLUME_SKY, т.е. 1111 1111 1111 1111
1111 1111 1111 1101 (0xFFFFFFFD) — рисует ТОЛЬКО поверхности группы неба
(мини-мир + купол) и ничего, кроме неё. worldspawn / func_display на этот
проход не влияют.
================
*/
static void R_SetupVolumeRules(viewParms_t* parms) {
	parms->volumeFlags = tr.volumeFlags;
	parms->volumeShowOnly = tr.volumeShowOnly;
	parms->volumeShowFlags = tr.volumeShowFlags;

	if(parms->flags & VPF_SKYBOX3D) {  // 3D-скайбокс: СТРОГО только группа неба (~VOLUME_SKY = 0xFFFFFFFD)
		parms->volumeFlags = ~VOLUME_SKY;
		parms->volumeShowOnly = false;
		parms->volumeShowFlags = 0;
	}

	parms->occlusionHidden = 0;
	parms->flags &= ~VPF_OCCLUSIONTEST;
	if(tr.occlusion.active && R_OcclusionPrimaryView(parms)) {
		parms->occlusionHidden = tr.occlusion.hiddenMask;
		parms->flags |= VPF_OCCLUSIONTEST;
	}
}

/*
================
R_RenderView

A view may be either the actual camera view,
or a mirror / remote location
================
*/
void R_RenderView(viewParms_t* parms) {
	int firstDrawSurf;
	int numDrawSurfs;

	if(parms->viewportWidth <= 0 || parms->viewportHeight <= 0) {
		return;
	}

	tr.viewCount++;

	tr.viewParms = *parms;
	tr.viewParms.frameSceneNum = tr.frameSceneNum;
	tr.viewParms.frameCount = tr.frameCount;
	R_SetupVolumeRules(&tr.viewParms);

	// Portal cameras need their map-sized far clip before projection/frustum
	// setup. The primary view keeps its existing visBounds-based calculation.
	if(tr.viewParms.isPortal && !(tr.viewParms.flags & VPF_SHADOWMAPS)) R_SetFarClip();

	firstDrawSurf = tr.refdef.numDrawSurfs;

	tr.viewCount++;

	// set viewParms.world
	R_RotateForViewer();

	R_SetupProjection(&tr.viewParms, 64, tr.viewParms.zFar, true);
	if(tr.viewParms.isPortal) {
		/*
		 * Finalize the oblique portal projection BEFORE BSP traversal.
		 * Its far clip plane is no longer perpendicular to the view axis at
		 * oblique angles, so culling must use planes extracted from this exact
		 * matrix rather than the pre-oblique symmetric frustum.
		 */
		R_SetupProjectionZ(&tr.viewParms);
		R_SetupFrustumFromProjection(&tr.viewParms);
	}

	// R_AddWorldSurface() notes the nearest planar water surface of this view
	tr.waterFound = false;

	R_GenerateDrawSurfs();

	// if we overflowed MAX_DRAWSURFS, the drawsurfs
	// wrapped around in the buffer and we will be missing
	// the first surfaces, not the last ones
	numDrawSurfs = tr.refdef.numDrawSurfs;
	if(numDrawSurfs > MAX_DRAWSURFS) {
		numDrawSurfs = MAX_DRAWSURFS;
	}

	// planar water reflection (r_planarReflections): the mirrored view has to be queued before this view
	if(tr.waterFound && r_planarReflections->integer && tr.waterReflectionFbo) R_RenderWaterReflection();

	R_SortDrawSurfs(tr.refdef.drawSurfs + firstDrawSurf, numDrawSurfs - firstDrawSurf);
}

static void R_RenderOneDlightCubemap(const refdef_t* fd, int lightIndex, int face, int savedNumDrawSurfs) {
	viewParms_t shadowParms;
	(void)fd;
	int j, jStart, jEnd;

	Com_Memset(&shadowParms, 0, sizeof(shadowParms));
	shadowParms.viewportX = 0;
	shadowParms.viewportY = 0;
	shadowParms.viewportWidth = r_dynamicLightResolution->integer;
	shadowParms.viewportHeight = r_dynamicLightResolution->integer;
	shadowParms.isPortal = false;
	shadowParms.isMirror = true;
	shadowParms.fovX = 90;
	shadowParms.fovY = 90;
	shadowParms.targetFbo = tr.dlightShadowFbo;
	shadowParms.flags = VPF_SHADOWMAPS | VPF_DEPTHSHADOW | VPF_DLIGHTSHADOW | VPF_NOVIEWMODEL;
	shadowParms.zFar = tr.refdef.dlights[lightIndex].radius;
	VectorCopy(tr.refdef.dlights[lightIndex].origin, shadowParms.or.origin);
	VectorCopy(tr.refdef.dlights[lightIndex].origin, shadowParms.pvsOrigin);

	if(face >= 0 && face < 6) {
		jStart = face;
		jEnd = face + 1;
	} else {
		jStart = 0;
		jEnd = 6;
	}

	for(j = jStart; j < jEnd; j++) {
		switch(j) {
			case 0:
				VectorSet(shadowParms.or.axis[0], -1, 0, 0);
				VectorSet(shadowParms.or.axis[1], 0, 0, -1);
				VectorSet(shadowParms.or.axis[2], 0, 1, 0);
				break;
			case 1:
				VectorSet(shadowParms.or.axis[0], 1, 0, 0);
				VectorSet(shadowParms.or.axis[1], 0, 0, 1);
				VectorSet(shadowParms.or.axis[2], 0, 1, 0);
				break;
			case 2:
				VectorSet(shadowParms.or.axis[0], 0, -1, 0);
				VectorSet(shadowParms.or.axis[1], 1, 0, 0);
				VectorSet(shadowParms.or.axis[2], 0, 0, -1);
				break;
			case 3:
				VectorSet(shadowParms.or.axis[0], 0, 1, 0);
				VectorSet(shadowParms.or.axis[1], 1, 0, 0);
				VectorSet(shadowParms.or.axis[2], 0, 0, 1);
				break;
			case 4:
				VectorSet(shadowParms.or.axis[0], 0, 0, -1);
				VectorSet(shadowParms.or.axis[1], 1, 0, 0);
				VectorSet(shadowParms.or.axis[2], 0, 1, 0);
				break;
			case 5:
				VectorSet(shadowParms.or.axis[0], 0, 0, 1);
				VectorSet(shadowParms.or.axis[1], -1, 0, 0);
				VectorSet(shadowParms.or.axis[2], 0, 1, 0);
				break;
		}

		shadowParms.targetFboLayer = j;
		shadowParms.targetFboCubemapIndex = lightIndex;
		tr.refdef.numDrawSurfs = savedNumDrawSurfs;
		R_RenderView(&shadowParms);
		R_IssuePendingRenderCommands();
		tr.refdef.numDrawSurfs = savedNumDrawSurfs;
	}
}

void R_RenderDlightCubemaps(const refdef_t* fd) {
	static int nextLightToUpdate;
	static int nextFaceToUpdate;
	static int lastUpdateFrame = -1;
	int i, mode, savedNumDrawSurfs, numLights;

	(void)fd;
	R_IssuePendingRenderCommands();

	// Dynamic shadow cubemaps require FBO support. Dynamic light contribution
	// itself remains enabled when this path is unavailable.
	if(!glRefConfig.framebufferObject || !tr.dlightShadowFbo) return;

	savedNumDrawSurfs = tr.refdef.numDrawSurfs;
	mode = Com_Clamp(0, 3, r_dynamicLightUpdate->integer);
	numLights = tr.refdef.num_dlights;

	if(mode == 0) {
		if(lastUpdateFrame >= 0 && tr.frameCount - lastUpdateFrame < 2) return;
	} else if(lastUpdateFrame == tr.frameCount) {
		return;
	}

	if(numLights <= 0) {
		lastUpdateFrame = tr.frameCount;
		return;
	}

	if(mode == 3) {
		for(i = 0; i < numLights; i++) {
			if(!tr.refdef.dlights[i].shadows || !tr.shadowCubemaps[i]) continue;
			R_RenderOneDlightCubemap(fd, i, -1, savedNumDrawSurfs);
		}
	} else if(mode == 2) {
		for(i = 0; i < numLights; i++) {
			int candidate = (nextLightToUpdate + i) % numLights;
			if(!tr.refdef.dlights[candidate].shadows || !tr.shadowCubemaps[candidate]) continue;

			R_RenderOneDlightCubemap(fd, candidate, -1, savedNumDrawSurfs);
			nextLightToUpdate = (candidate + 1) % numLights;
			lastUpdateFrame = tr.frameCount;
			tr.refdef.numDrawSurfs = savedNumDrawSurfs;
			return;
		}
	} else {
		for(i = 0; i < numLights; i++) {
			int candidate = (nextLightToUpdate + i) % numLights;
			if(!tr.refdef.dlights[candidate].shadows || !tr.shadowCubemaps[candidate]) continue;

			R_RenderOneDlightCubemap(fd, candidate, nextFaceToUpdate, savedNumDrawSurfs);

			if(++nextFaceToUpdate >= 6) {
				nextFaceToUpdate = 0;
				nextLightToUpdate = (candidate + 1) % numLights;
			} else {
				nextLightToUpdate = candidate;
			}

			lastUpdateFrame = tr.frameCount;
			tr.refdef.numDrawSurfs = savedNumDrawSurfs;
			return;
		}
	}

	lastUpdateFrame = tr.frameCount;
	tr.refdef.numDrawSurfs = savedNumDrawSurfs;
}

static float CalcSplit(float n, float f, float i, float m) { return (n * pow(f / n, i / m) + (f - n) * i / m) / 2.0f; }

void R_RenderSunShadowMaps(const refdef_t* fd, int level) {
	viewParms_t shadowParms;
	vec4_t lightDir;
	vec3_t lightViewAxis[3];
	vec3_t lightOrigin;
	float splitZNear, splitZFar, splitBias;
	float viewZNear, viewZFar;
	vec3_t lightviewBounds[2];
	bool lightViewIndependentOfCameraView = false;

	VectorCopy4(tr.refdef.sunDir, lightDir);

	viewZNear = 16.0f;
	viewZFar = r_shadowDistance->value;
	splitBias = 0.0f;

	switch(level) {
		case 0:
		default:
			splitZNear = viewZNear;
			splitZFar = CalcSplit(viewZNear, viewZFar, 1, 3) + splitBias;
			break;
		case 1:
			splitZNear = CalcSplit(viewZNear, viewZFar, 1, 3) + splitBias;
			splitZFar = CalcSplit(viewZNear, viewZFar, 2, 3) + splitBias;
			break;
		case 2:
			splitZNear = CalcSplit(viewZNear, viewZFar, 2, 3) + splitBias;
			splitZFar = viewZFar;
			break;
	}

	VectorCopy(fd->vieworg, lightOrigin);

	// Make up a projection
	VectorScale(lightDir, -1.0f, lightViewAxis[0]);

	if(level == 3 || lightViewIndependentOfCameraView) {
		// Use world up as light view up
		VectorSet(lightViewAxis[2], 0, 0, 1);
	} else if(level == 0) {
		// Level 0 tries to use a diamond texture orientation relative to camera view
		// Use halfway between camera view forward and left for light view up
		VectorAdd(fd->viewaxis[0], fd->viewaxis[1], lightViewAxis[2]);
	} else {
		// Use camera view up as light view up
		VectorCopy(fd->viewaxis[2], lightViewAxis[2]);
	}

	// Check if too close to parallel to light direction
	if(fabsf(DotProduct(lightViewAxis[2], lightViewAxis[0])) > 0.9f) {
		if(level == 3 || lightViewIndependentOfCameraView) {
			// Use world left as light view up
			VectorSet(lightViewAxis[2], 0, 1, 0);
		} else if(level == 0) {
			// Level 0 tries to use a diamond texture orientation relative to camera view
			// Use halfway between camera view forward and up for light view up
			VectorAdd(fd->viewaxis[0], fd->viewaxis[2], lightViewAxis[2]);
		} else {
			// Use camera view left as light view up
			VectorCopy(fd->viewaxis[1], lightViewAxis[2]);
		}
	}

	// clean axes
	CrossProduct(lightViewAxis[2], lightViewAxis[0], lightViewAxis[1]);
	VectorNormalize(lightViewAxis[1]);
	CrossProduct(lightViewAxis[0], lightViewAxis[1], lightViewAxis[2]);

	// Create bounds for light projection using slice of view projection
	{
		mat4_t lightViewMatrix;
		vec4_t point, base, lightViewPoint;
		float lx, ly;

		base[3] = 1;
		point[3] = 1;
		lightViewPoint[3] = 1;

		Mat4View(lightViewAxis, lightOrigin, lightViewMatrix);

		ClearBounds(lightviewBounds[0], lightviewBounds[1]);

		if(level != 3) {
			// add view near plane
			lx = splitZNear * tan(fd->fov_x * M_PI / 360.0f);
			ly = splitZNear * tan(fd->fov_y * M_PI / 360.0f);
			VectorMA(fd->vieworg, splitZNear, fd->viewaxis[0], base);

			VectorMA(base, lx, fd->viewaxis[1], point);
			VectorMA(point, ly, fd->viewaxis[2], point);
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			VectorMA(base, -lx, fd->viewaxis[1], point);
			VectorMA(point, ly, fd->viewaxis[2], point);
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			VectorMA(base, lx, fd->viewaxis[1], point);
			VectorMA(point, -ly, fd->viewaxis[2], point);
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			VectorMA(base, -lx, fd->viewaxis[1], point);
			VectorMA(point, -ly, fd->viewaxis[2], point);
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			// add view far plane
			lx = splitZFar * tan(fd->fov_x * M_PI / 360.0f);
			ly = splitZFar * tan(fd->fov_y * M_PI / 360.0f);
			VectorMA(fd->vieworg, splitZFar, fd->viewaxis[0], base);

			VectorMA(base, lx, fd->viewaxis[1], point);
			VectorMA(point, ly, fd->viewaxis[2], point);
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			VectorMA(base, -lx, fd->viewaxis[1], point);
			VectorMA(point, ly, fd->viewaxis[2], point);
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			VectorMA(base, lx, fd->viewaxis[1], point);
			VectorMA(point, -ly, fd->viewaxis[2], point);
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			VectorMA(base, -lx, fd->viewaxis[1], point);
			VectorMA(point, -ly, fd->viewaxis[2], point);
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);
		} else {
			// use light grid size as level size
			// FIXME: could be tighter
			vec3_t bounds;

			bounds[0] = tr.world->lightGridSize[0] * tr.world->lightGridBounds[0];
			bounds[1] = tr.world->lightGridSize[1] * tr.world->lightGridBounds[1];
			bounds[2] = tr.world->lightGridSize[2] * tr.world->lightGridBounds[2];

			point[0] = tr.world->lightGridOrigin[0];
			point[1] = tr.world->lightGridOrigin[1];
			point[2] = tr.world->lightGridOrigin[2];
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			point[0] = tr.world->lightGridOrigin[0] + bounds[0];
			point[1] = tr.world->lightGridOrigin[1];
			point[2] = tr.world->lightGridOrigin[2];
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			point[0] = tr.world->lightGridOrigin[0];
			point[1] = tr.world->lightGridOrigin[1] + bounds[1];
			point[2] = tr.world->lightGridOrigin[2];
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			point[0] = tr.world->lightGridOrigin[0] + bounds[0];
			point[1] = tr.world->lightGridOrigin[1] + bounds[1];
			point[2] = tr.world->lightGridOrigin[2];
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			point[0] = tr.world->lightGridOrigin[0];
			point[1] = tr.world->lightGridOrigin[1];
			point[2] = tr.world->lightGridOrigin[2] + bounds[2];
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			point[0] = tr.world->lightGridOrigin[0] + bounds[0];
			point[1] = tr.world->lightGridOrigin[1];
			point[2] = tr.world->lightGridOrigin[2] + bounds[2];
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			point[0] = tr.world->lightGridOrigin[0];
			point[1] = tr.world->lightGridOrigin[1] + bounds[1];
			point[2] = tr.world->lightGridOrigin[2] + bounds[2];
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);

			point[0] = tr.world->lightGridOrigin[0] + bounds[0];
			point[1] = tr.world->lightGridOrigin[1] + bounds[1];
			point[2] = tr.world->lightGridOrigin[2] + bounds[2];
			Mat4Transform(lightViewMatrix, point, lightViewPoint);
			AddPointToBounds(lightViewPoint, lightviewBounds[0], lightviewBounds[1]);
		}

		if(!glRefConfig.depthClamp) lightviewBounds[0][0] = lightviewBounds[1][0] - 8192;

		// Moving the Light in Texel-Sized Increments
		// from http://msdn.microsoft.com/en-us/library/windows/desktop/ee416324%28v=vs.85%29.aspx
		//
		if(lightViewIndependentOfCameraView) {
			float cascadeBound, worldUnitsPerTexel, invWorldUnitsPerTexel;

			cascadeBound = MAX(lightviewBounds[1][0] - lightviewBounds[0][0], lightviewBounds[1][1] - lightviewBounds[0][1]);
			cascadeBound = MAX(cascadeBound, lightviewBounds[1][2] - lightviewBounds[0][2]);
			worldUnitsPerTexel = cascadeBound / tr.sunShadowFbo[level]->width;
			invWorldUnitsPerTexel = 1.0f / worldUnitsPerTexel;

			VectorScale(lightviewBounds[0], invWorldUnitsPerTexel, lightviewBounds[0]);
			lightviewBounds[0][0] = floor(lightviewBounds[0][0]);
			lightviewBounds[0][1] = floor(lightviewBounds[0][1]);
			lightviewBounds[0][2] = floor(lightviewBounds[0][2]);
			VectorScale(lightviewBounds[0], worldUnitsPerTexel, lightviewBounds[0]);

			VectorScale(lightviewBounds[1], invWorldUnitsPerTexel, lightviewBounds[1]);
			lightviewBounds[1][0] = floor(lightviewBounds[1][0]);
			lightviewBounds[1][1] = floor(lightviewBounds[1][1]);
			lightviewBounds[1][2] = floor(lightviewBounds[1][2]);
			VectorScale(lightviewBounds[1], worldUnitsPerTexel, lightviewBounds[1]);
		}

		// ri.Printf(PRINT_ALL, "level %d znear %f zfar %f\n", level, lightviewBounds[0][0], lightviewBounds[1][0]);
		// ri.Printf(PRINT_ALL, "xmin %f xmax %f ymin %f ymax %f\n", lightviewBounds[0][1], lightviewBounds[1][1], -lightviewBounds[1][2],
		// -lightviewBounds[0][2]);
	}

	{
		int firstDrawSurf;

		Com_Memset(&shadowParms, 0, sizeof(shadowParms));

		if(glRefConfig.framebufferObject) {
			shadowParms.viewportX = 0;
			shadowParms.viewportY = 0;
		} else {
			shadowParms.viewportX = tr.refdef.x;
			shadowParms.viewportY = glConfig.vidHeight - (tr.refdef.y + tr.sunShadowFbo[level]->height);
		}
		shadowParms.viewportWidth = tr.sunShadowFbo[level]->width;
		shadowParms.viewportHeight = tr.sunShadowFbo[level]->height;
		shadowParms.isPortal = false;
		shadowParms.isMirror = false;

		shadowParms.fovX = 90;
		shadowParms.fovY = 90;

		if(glRefConfig.framebufferObject) shadowParms.targetFbo = tr.sunShadowFbo[level];

		shadowParms.flags = VPF_DEPTHSHADOW | VPF_DEPTHCLAMP | VPF_ORTHOGRAPHIC | VPF_NOVIEWMODEL;
		shadowParms.zFar = lightviewBounds[1][0];

		VectorCopy(lightOrigin, shadowParms.or.origin);

		VectorCopy(lightViewAxis[0], shadowParms.or.axis[0]);
		VectorCopy(lightViewAxis[1], shadowParms.or.axis[1]);
		VectorCopy(lightViewAxis[2], shadowParms.or.axis[2]);

		VectorCopy(lightOrigin, shadowParms.pvsOrigin);

		{
			tr.viewCount++;

			tr.viewParms = shadowParms;
			tr.viewParms.frameSceneNum = tr.frameSceneNum;
			tr.viewParms.frameCount = tr.frameCount;
			// каскады солнца идут мимо R_RenderView — правила объёмов ставим явно
			R_SetupVolumeRules(&tr.viewParms);

			firstDrawSurf = tr.refdef.numDrawSurfs;

			tr.viewCount++;

			// set viewParms.world
			R_RotateForViewer();

			R_SetupProjectionOrtho(&tr.viewParms, lightviewBounds);

			R_AddWorldSurfaces();
			// R_AddPolygonSurfaces();

			if(level != 3 && (level == 0 || r_shadowUpdate->integer != 0)) R_AddEntitySurfaces();

			R_SortDrawSurfs(tr.refdef.drawSurfs + firstDrawSurf, tr.refdef.numDrawSurfs - firstDrawSurf);
		}

		Mat4Multiply(tr.viewParms.projectionMatrix, tr.viewParms.world.modelMatrix, tr.refdef.sunShadowMvp[level]);
	}
}

void R_RenderCubemapSide(int cubemapSide, bool subscene) {
	refdef_t refdef;
	viewParms_t parms;

	memset(&refdef, 0, sizeof(refdef));
	refdef.rdflags = 0;
	VectorCopy(tr.cubemap->origin, refdef.vieworg);

	switch(cubemapSide) {
		case 0:
			// -X
			VectorSet(refdef.viewaxis[0], -1, 0, 0);
			VectorSet(refdef.viewaxis[1], 0, 0, -1);
			VectorSet(refdef.viewaxis[2], 0, 1, 0);
			break;
		case 1:
			// +X
			VectorSet(refdef.viewaxis[0], 1, 0, 0);
			VectorSet(refdef.viewaxis[1], 0, 0, 1);
			VectorSet(refdef.viewaxis[2], 0, 1, 0);
			break;
		case 2:
			// -Y
			VectorSet(refdef.viewaxis[0], 0, -1, 0);
			VectorSet(refdef.viewaxis[1], 1, 0, 0);
			VectorSet(refdef.viewaxis[2], 0, 0, -1);
			break;
		case 3:
			// +Y
			VectorSet(refdef.viewaxis[0], 0, 1, 0);
			VectorSet(refdef.viewaxis[1], 1, 0, 0);
			VectorSet(refdef.viewaxis[2], 0, 0, 1);
			break;
		case 4:
			// -Z
			VectorSet(refdef.viewaxis[0], 0, 0, -1);
			VectorSet(refdef.viewaxis[1], 1, 0, 0);
			VectorSet(refdef.viewaxis[2], 0, 1, 0);
			break;
		case 5:
			// +Z
			VectorSet(refdef.viewaxis[0], 0, 0, 1);
			VectorSet(refdef.viewaxis[1], -1, 0, 0);
			VectorSet(refdef.viewaxis[2], 0, 1, 0);
			break;
	}

	refdef.fov_x = 90;
	refdef.fov_y = 90;

	refdef.x = 0;
	refdef.y = 0;
	refdef.width = tr.renderCubeFbo->width;
	refdef.height = tr.renderCubeFbo->height;

	refdef.time = 0;

	if(!subscene) RE_BeginScene(&refdef);

	Com_Memset(&parms, 0, sizeof(parms));

	parms.viewportX = 0;
	parms.viewportY = 0;
	parms.viewportWidth = tr.renderCubeFbo->width;
	parms.viewportHeight = tr.renderCubeFbo->height;
	parms.isPortal = false;
	parms.isMirror = true;
	parms.flags = VPF_NOVIEWMODEL | VPF_CUBEMAP;

	parms.fovX = 90;
	parms.fovY = 90;

	VectorCopy(refdef.vieworg, parms.or.origin);
	VectorCopy(refdef.viewaxis[0], parms.or.axis[0]);
	VectorCopy(refdef.viewaxis[1], parms.or.axis[1]);
	VectorCopy(refdef.viewaxis[2], parms.or.axis[2]);

	VectorCopy(refdef.vieworg, parms.pvsOrigin);

	parms.targetFbo = tr.renderCubeFbo;
	parms.targetFboLayer = cubemapSide;

	// 3D skybox в кубмапах отражений: тот же двухпроходный приём, что и в
	// основном виде, — сначала небо и мини-мир камерой скайбокса, поверх —
	// геометрия основного мира. Управляется r_cubeReflectionsSkybox.
	if(tr.world && tr.world->hasSkybox && r_3dsky->integer && !tr.hideSky && r_cubeReflectionsSkybox->integer) {
		viewParms_t skyParms = parms;

		VectorMA(tr.world->skyboxOrigin, 1.0f / tr.world->skyboxScale, parms.or.origin, skyParms.or.origin);
		VectorCopy(skyParms.or.origin, skyParms.pvsOrigin);
		skyParms.flags = parms.flags | VPF_SKYBOX3D;

		R_RenderView(&skyParms);

		// второй проход рисуется поверх первого: цветовой буфер (небо и мини-мир) не очищается, глубина очищается как обычно
		parms.flags |= VPF_SKYDRAWN | VPF_KEEPCOLOR;
	}

	R_RenderView(&parms);

	if(!subscene) RE_EndScene();
}
