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
#include "tr_local.h"

/*
================
R_CullSurface

Tries to cull surfaces before they are lighted or
added to the sorting list.
================
*/
static bool R_CullSurface(msurface_t* surf) {
	if(surf->cullinfo.type == CULLINFO_NONE) {
		return false;
	}

	if(surf->cullinfo.type & CULLINFO_PLANE) {
		// Only true for SF_FACE, so treat like its own function
		float d;
		cullType_t ct;

		ct = surf->shader->cullType;

		if(ct == CT_TWO_SIDED) {
			return false;
		}

		if((tr.viewParms.flags & (VPF_SHADOWMAPS | VPF_DEPTHSHADOW))) {
			// Back end draws this surface twice: first with the seam-fixing
			// cull rule, then with the old rule. Don't cull it here, otherwise
			// the second pass can never fill the holes.
			return false;
		}

		if((tr.viewParms.flags & (VPF_SHADOWMAPS | VPF_DEPTHSHADOW)) && !(tr.viewParms.flags & VPF_ORTHOGRAPHIC)) {  // FIX
			if(ct == CT_FRONT_SIDED) {
				ct = CT_BACK_SIDED;
			} else {
				ct = CT_FRONT_SIDED;
			}
		}

		// do proper cull for orthographic projection
		if(tr.viewParms.flags & VPF_ORTHOGRAPHIC) {
			d = DotProduct(tr.viewParms.or.axis[0], surf->cullinfo.plane.normal);
			if(ct == CT_FRONT_SIDED) {
				if(d > 0) return true;
			} else {
				if(d < 0) return true;
			}
			return false;
		}

		d = DotProduct(tr.or.viewOrigin, surf->cullinfo.plane.normal);

		// don't cull exactly on the plane, because there are levels of rounding
		// through the BSP, ICD, and hardware that may cause pixel gaps if an
		// epsilon isn't allowed here
		if(ct == CT_FRONT_SIDED) {
			if(d < surf->cullinfo.plane.dist - 8) {
				return true;
			}
		} else {
			if(d > surf->cullinfo.plane.dist + 8) {
				return true;
			}
		}

		return false;
	}

	/*
	 * R_AddWorldSurface calls R_FrustumCullSurface immediately before this
	 * function. That function already checks the box when present, or the
	 * sphere when it is the only bounds type. Only check a sphere here when
	 * both bounds are present (the frustum path checks the box first).
	 */
	if((surf->cullinfo.type & (CULLINFO_BOX | CULLINFO_SPHERE)) == (CULLINFO_BOX | CULLINFO_SPHERE)) {
		int sphereCull;

		if(tr.currentEntityNum != REFENTITYNUM_WORLD) {
			sphereCull = R_CullLocalPointAndRadius(surf->cullinfo.localOrigin, surf->cullinfo.radius);
		} else {
			sphereCull = R_CullPointAndRadius(surf->cullinfo.localOrigin, surf->cullinfo.radius);
		}

		if(sphereCull == CULL_OUT) {
			return true;
		}
	}

	return false;
}

static bool R_DlightCullSurface(msurface_t* surf, dlightMask_t* dlightBits) {
	int i;
	dlight_t* dl;

	if(!DlightMask_Any(dlightBits)) {
		if(!(tr.viewParms.flags & (VPF_SHADOWMAPS | VPF_DEPTHSHADOW))) {
			switch(*surf->data) {
				case SF_FACE:
				case SF_GRID:
				case SF_TRIANGLES: DlightMask_Clear(&((srfBspSurface_t*)surf->data)->dlightBits); break;
				default: break;
			}
		}
		tr.pc.c_dlightSurfacesCulled++;
		return true;
	}

	for(i = 0; i < tr.refdef.num_dlights; i++) {
		if(DlightMask_Test(dlightBits, i) && !(tr.refdef.dlights[i].flags & REF_SURFACE_DLIGHT)) {
			DlightMask_ClearBit(dlightBits, i);
		}
	}

	if(r_dynamicLightCull->integer && tr.currentEntityNum == REFENTITYNUM_WORLD) {
		if(surf->cullinfo.type & (CULLINFO_SPHERE | CULLINFO_BOX)) {
			// sqrt нужен один раз на поверхность (не на источник!)
			float surfRadius = sqrtf(surf->cullinfo.radiusSquared);

			for(i = 0; i < tr.refdef.num_dlights; i++) {
				vec3_t toLight;
				float cullDist, distSq;

				if(!DlightMask_Test(dlightBits, i)) continue;
				dl = &tr.refdef.dlights[i];

				cullDist = dl->radius * 1.5f + surfRadius;
				cullDist *= cullDist;

				VectorSubtract(dl->origin, surf->cullinfo.center, toLight);
				distSq = VectorLengthSquared(toLight);

				if(distSq > cullDist) {
					DlightMask_ClearBit(dlightBits, i);
				}
			}
		}
	}

	switch(*surf->data) {
		case SF_FACE:
		case SF_GRID:
		case SF_TRIANGLES:
			if(!(tr.viewParms.flags & (VPF_SHADOWMAPS | VPF_DEPTHSHADOW))) {
				DlightMask_Copy(&((srfBspSurface_t*)surf->data)->dlightBits, dlightBits);
			}
			break;
		default: DlightMask_Clear(dlightBits); break;
	}

	if(DlightMask_Any(dlightBits)) {
		tr.pc.c_dlightSurfaces++;
		return false;
	}

	tr.pc.c_dlightSurfacesCulled++;
	return true;
}

static bool R_FrustumCullSurface(msurface_t* surf) {
	int frustumCull;

	if(surf->cullinfo.type & CULLINFO_BOX) {
		if(tr.currentEntityNum != REFENTITYNUM_WORLD)
			frustumCull = R_CullLocalBox(surf->cullinfo.bounds);
		else
			frustumCull = R_CullBox(surf->cullinfo.bounds);

		if(frustumCull == CULL_OUT) {
			return true;
		} else {
			return false;
		}
	} else if(surf->cullinfo.type & CULLINFO_SPHERE) {
		if(tr.currentEntityNum != REFENTITYNUM_WORLD)
			frustumCull = R_CullLocalPointAndRadius(surf->cullinfo.localOrigin, surf->cullinfo.radius);
		else
			frustumCull = R_CullPointAndRadius(surf->cullinfo.localOrigin, surf->cullinfo.radius);

		if(frustumCull == CULL_OUT) {
			return true;
		} else {
			return false;
		}
	}
}

static void R_AddWorldSurface(msurface_t* surf, const dlightMask_t* sourceDlightBits) {
	dlightMask_t dlightBits;
	int fogIndex;

	// func_display hideSky suppresses both the 2D sky dome and the VOLUME_SKY
	// geometry used by the 3D skybox. Keep this check centralized so it applies
	// equally to the main view, portal views, reflections, and auxiliary passes.
	if(tr.hideSky && (surf->shader->isSky || (surf->volumeFlags & VOLUME_SKY))) return;

	/*
	 * Правила видимости объёмов ЭТОЙ камеры (маску ставит R_SetupVolumeRules).
	 * Небесный купол (isSky, "портальное небо") объёмами не скрывается никогда —
	 * это фон кадра, без него проход 2 3D-скайбокса оставил бы дыры. Объёмами
	 * управляется КОПИЯ неба: 3D-скайбокс (мини-мир), помеченный func_volume
	 * со spawnflags 2 (ТОЛЬКО VOLUME_SKY), и весь остальной мир.
	 *
	 * Проход 3D-скайбокса (VPF_SKYBOX3D) получил СТРОГО ~VOLUME_SKY (0xFFFFFFFD) —
	 * здесь остаются только поверхности группы неба. Все остальные камеры
	 * (мир, отражения, кубмапы, тени) получили правила сцены: worldspawn
	 * "volumeflags" (по умолчанию VOLUME_SKY — мир не рисует поверхности
	 * скайбокса) либо func_display, если камера внутри него.
	 */
	if(!surf->shader->isSky) {
		if(tr.viewParms.volumeShowOnly) {
			// func_display whitelist: РИСУЙ ТОЛЬКО поверхности,
			// чьи volumeFlags входят в белый список
			if(!(tr.viewParms.volumeShowFlags & surf->volumeFlags)) return;
		} else if(tr.viewParms.volumeFlags & surf->volumeFlags) {
			// worldspawn "volumeflags" / func_display blacklist:
			// НЕ РИСУЙ поверхности с пересекающимися volumeFlags
			return;
		}
	}

	/*
	 * func_occlusion: поверхность скрыта, только если скрыты ВСЕ группы, которых она коснулась
	 * (видна хотя бы одна группа — рисуем). occlusionHidden != 0 лишь у основного вида игрока
	 * (R_SetupVolumeRules), поэтому зеркала, отражения, кубмапы и тени этот фильтр не затрагивает.
	 * Поверхности без групп (occlusionMask == 0) и небо рисуются всегда.
	 */
	if(surf->occlusionMask && !(surf->occlusionMask & ~tr.viewParms.occlusionHidden)) return;

	// Правило объёмов: группа VOLUME_SKY (небо и 3D-скайбокс) не участвует в
	// тенях от солнца (VPF_DEPTHSHADOW ортографические каскады) и в динамических
	// тенях от dlight.
	// Проверка после видимости: скрытые поверхности не бросают теней.
	if((surf->volumeFlags & VOLUME_SKY) && (tr.viewParms.flags & VPF_DEPTHSHADOW)) return;

	// 3D skybox, проход 2: небо уже нарисовано проходом 1 и просвечивает
	// сквозь просветы SURF_SKY — сами небесные поверхности не рисуем
	if((tr.viewParms.flags & VPF_SKYDRAWN) && surf->shader->isSky) return;

	// the planar reflection of the water must not contain the water itself
	if((tr.viewParms.flags & VPF_WATERREFLECTION) && (surf->shader->surfaceFlags & SURF_CUBEMAP_WATER)) return;

	if(sourceDlightBits)
		DlightMask_Copy(&dlightBits, sourceDlightBits);
	else
		DlightMask_Clear(&dlightBits);

	if(R_FrustumCullSurface(surf)) return;

	if(!(tr.viewParms.targetFbo == tr.dlightShadowFbo && (tr.viewParms.flags & VPF_DEPTHSHADOW))) {
		if(R_CullSurface(surf)) return;
	}

	bool noDlights = R_DlightCullSurface(surf, &dlightBits);
	fogIndex = surf->hasFog ? surf->fogIndex : 0;

	if(surf->isWaterPlane) R_NoteWaterSurface(surf);

	R_AddDrawSurf(surf->data, surf->shader, fogIndex, !noDlights, surf->useCubemap, surf->cubemapOrigin, surf->cubemapBound);
}

/*
=============================================================

    BRUSH MODELS

=============================================================
*/

/*
=================
R_AddBrushModelSurfaces
=================
*/
void R_AddBrushModelSurfaces(trRefEntity_t* ent) {
	bmodel_t* bmodel;
	int clip;
	model_t* pModel;
	int i;

	pModel = R_GetModelByHandle(ent->e.hModel);

	bmodel = pModel->bmodel;

	clip = R_CullLocalBox(bmodel->bounds);
	if(clip == CULL_OUT) {
		return;
	}

	R_SetupEntityLighting(&tr.refdef, ent);
	R_DlightBmodel(bmodel);

	for(i = 0; i < bmodel->numSurfaces; i++) {
		int surf = bmodel->firstSurface + i;

		if(tr.world->surfacesViewCount[surf] != tr.viewCount) {
			tr.world->surfacesViewCount[surf] = tr.viewCount;
			R_AddWorldSurface(tr.world->surfaces + surf, &tr.currentEntity->needDlights);
		}
	}
}

/*
=============================================================

    WORLD MODEL

=============================================================
*/

static bool R_CullDlightShadowNode(const mnode_t* node) {
	int index;
	dlight_t* dl;

	if(!(tr.viewParms.flags & VPF_DLIGHTSHADOW) || !r_dynamicLightCull->integer) return false;

	index = tr.viewParms.targetFboCubemapIndex;

	if(index < 0 || index >= tr.refdef.num_dlights) return true;

	dl = &tr.refdef.dlights[index];

	if(node->surfMaxs[0] <= dl->origin[0] - dl->radius || node->surfMins[0] >= dl->origin[0] + dl->radius || node->surfMaxs[1] <= dl->origin[1] - dl->radius || node->surfMins[1] >= dl->origin[1] + dl->radius || node->surfMaxs[2] <= dl->origin[2] - dl->radius || node->surfMins[2] >= dl->origin[2] + dl->radius) {
		return true;
	}

	return false;
}

static bool R_LeafVisible(const mnode_t* leaf);

static void R_RecursiveWorldNode(mnode_t* node, uint32_t planeBits, const dlightMask_t* sourceDlightBits) {
	int i, r;
	dlight_t* dl;
	dlightMask_t dlightBits;
	DlightMask_Copy(&dlightBits, sourceDlightBits);

	do {
		/*
		 * Dlight shadow uses light volume + frustum.
		 * Do not use PVS here.
		 */
		if(tr.viewParms.flags & VPF_DLIGHTSHADOW) {
			if(R_CullDlightShadowNode(node)) return;
		}

		/*
		 * Normal view frustum culling.
		 */
		if(planeBits & 1) {
			r = BoxOnPlaneSide(node->mins, node->maxs, &tr.viewParms.frustum[0]);

			if(r == 2) return;

			if(r == 1) planeBits &= ~1;
		}

		if(planeBits & 2) {
			r = BoxOnPlaneSide(node->mins, node->maxs, &tr.viewParms.frustum[1]);

			if(r == 2) return;

			if(r == 1) planeBits &= ~2;
		}

		if(planeBits & 4) {
			r = BoxOnPlaneSide(node->mins, node->maxs, &tr.viewParms.frustum[2]);

			if(r == 2) return;

			if(r == 1) planeBits &= ~4;
		}

		if(planeBits & 8) {
			r = BoxOnPlaneSide(node->mins, node->maxs, &tr.viewParms.frustum[3]);

			if(r == 2) return;

			if(r == 1) planeBits &= ~8;
		}

		if(planeBits & 16) {
			r = BoxOnPlaneSide(node->mins, node->maxs, &tr.viewParms.frustum[4]);

			if(r == 2) return;

			if(r == 1) planeBits &= ~16;
		}

		/*
		 * Normal dynamic light culling.
		 * Not used by dlight shadow views.
		 */
		if(r_dynamicLightCull->integer && DlightMask_Any(&dlightBits)) {
			for(i = 0; i < tr.refdef.num_dlights; i++) {
				if(DlightMask_Test(&dlightBits, i)) {
					vec3_t center, halfSize, toCenter;
					float boundRadius;
					dl = &tr.refdef.dlights[i];

					if(node->surfMins[0] >= (dl->origin[0] + dl->radius) || node->surfMaxs[0] <= (dl->origin[0] - dl->radius) || node->surfMins[1] >= (dl->origin[1] + dl->radius) || node->surfMaxs[1] <= (dl->origin[1] - dl->radius) || node->surfMins[2] >= (dl->origin[2] + dl->radius) || node->surfMaxs[2] <= (dl->origin[2] - dl->radius)) {
						DlightMask_ClearBit(&dlightBits, i);
						continue;
					}

					// The node bounds cover all surfaces below this node. For a
					// projector, conservatively reject the subtree when its bounding
					// sphere cannot intersect the light cone.
					if(dl->type == DLIGHT_PROJECTOR) {
						float forwardDistance, radialSq, coneRadius;
						VectorSet(center, (node->surfMins[0] + node->surfMaxs[0]) * 0.5f, (node->surfMins[1] + node->surfMaxs[1]) * 0.5f, (node->surfMins[2] + node->surfMaxs[2]) * 0.5f);
						VectorSet(halfSize, (node->surfMaxs[0] - node->surfMins[0]) * 0.5f, (node->surfMaxs[1] - node->surfMins[1]) * 0.5f, (node->surfMaxs[2] - node->surfMins[2]) * 0.5f);
						boundRadius = VectorLength(halfSize);
						VectorSubtract(center, dl->origin, toCenter);
						forwardDistance = DotProduct(toCenter, dl->forward);
						if(forwardDistance + boundRadius <= 0.0f || forwardDistance - boundRadius >= dl->radius) {
							DlightMask_ClearBit(&dlightBits, i);
							continue;
						}
						radialSq = VectorLengthSquared(toCenter) - forwardDistance * forwardDistance;
						coneRadius = Com_Clamp(0.0f, dl->radius, forwardDistance) * dl->coneTangent + boundRadius;
						if(radialSq > coneRadius * coneRadius) DlightMask_ClearBit(&dlightBits, i);
					}
				}
			}
		}

		if(node->isLeaf) break;

		R_RecursiveWorldNode(node->children[0], planeBits, &dlightBits);

		node = node->children[1];

	} while(1);

	if(!R_LeafVisible(node)) return;

	{
		int c;
		int surf, *view;

		tr.pc.c_leafs++;

		if(node->mins[0] < tr.viewParms.visBounds[0][0]) tr.viewParms.visBounds[0][0] = node->mins[0];

		if(node->mins[1] < tr.viewParms.visBounds[0][1]) tr.viewParms.visBounds[0][1] = node->mins[1];

		if(node->mins[2] < tr.viewParms.visBounds[0][2]) tr.viewParms.visBounds[0][2] = node->mins[2];

		if(node->maxs[0] > tr.viewParms.visBounds[1][0]) tr.viewParms.visBounds[1][0] = node->maxs[0];

		if(node->maxs[1] > tr.viewParms.visBounds[1][1]) tr.viewParms.visBounds[1][1] = node->maxs[1];

		if(node->maxs[2] > tr.viewParms.visBounds[1][2]) tr.viewParms.visBounds[1][2] = node->maxs[2];

		view = tr.world->marksurfaces + node->firstmarksurface;
		c = node->nummarksurfaces;

		while(c--) {
			surf = *view;

			if(tr.world->surfacesViewCount[surf] != tr.viewCount) {
				tr.world->surfacesViewCount[surf] = tr.viewCount;
				if(!(tr.viewParms.flags & VPF_DLIGHTSHADOW)) DlightMask_Copy(&tr.world->surfacesDlightBits[surf], &dlightBits);
			} else if(!(tr.viewParms.flags & VPF_DLIGHTSHADOW)) {
				DlightMask_Or(&tr.world->surfacesDlightBits[surf], &dlightBits);
			}

			view++;
		}
	}
}

/*
===============
R_LeafVisible
===============
*/
static bool R_LeafVisible(const mnode_t* leaf) {
	int cluster;

	if(tr.viewParms.flags & VPF_DLIGHTSHADOW) return true;

	if(leaf->hasSkySurface) return true;

	cluster = leaf->cluster;
	if(cluster < 0 || cluster >= tr.world->numClusters) return false;

	if(!tr.viewParms.isPortal && !(tr.viewParms.flags & VPF_SKYBOX3D)) {
		if(tr.refdef.areamask[leaf->area >> 3] & (1 << (leaf->area & 7))) return false;
	}

	return true;
}

/*
=============
R_AddWorldSurfaces
=============
*/
void R_AddWorldSurfaces(void) {
	uint32_t planeBits;
	dlightMask_t dlightBits;

	if(tr.refdef.rdflags & RDF_NOWORLDMODEL) return;
	if(!tr.world) return;

	tr.currentEntityNum = REFENTITYNUM_WORLD;
	tr.shiftedEntityNum = tr.currentEntityNum << QSORT_REFENTITYNUM_SHIFT;

	// clear out the visible min/max
	ClearBounds(tr.viewParms.visBounds[0], tr.viewParms.visBounds[1]);

	// perform frustum culling and flag all the potentially visible surfaces
	if(tr.refdef.num_dlights > MAX_DLIGHTS) tr.refdef.num_dlights = MAX_DLIGHTS;

	planeBits = (tr.viewParms.flags & VPF_FARPLANEFRUSTUM) ? 31 : 15;

	DlightMask_Clear(&dlightBits);
	if(!(tr.viewParms.flags & VPF_DEPTHSHADOW)) {
		int i;
		for(i = 0; i < tr.refdef.num_dlights; i++) DlightMask_Set(&dlightBits, i);
	}

	R_RecursiveWorldNode(tr.world->nodes, planeBits, &dlightBits);

	// now add all the potentially visible surfaces
	// also mask invisible dlights for next frame
	{
		int i;

		DlightMask_Clear(&tr.refdef.dlightMask);

		for(i = 0; i < tr.world->numWorldSurfaces; i++) {
			if(tr.world->surfacesViewCount[i] != tr.viewCount) continue;

			R_AddWorldSurface(tr.world->surfaces + i, &tr.world->surfacesDlightBits[i]);
			DlightMask_Or(&tr.refdef.dlightMask, &tr.world->surfacesDlightBits[i]);
		}

		DlightMask_Not(&tr.refdef.dlightMask);
	}
}
