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
// tr_light.c
//
// Единый рантайм-путь освещения динамических энтити/моделей: солнце
// (tr.sunLight / tr.sunDirection) + light-энтити карты (classname "light")
// + dlight-ы. Запечённый lightgrid полностью удалён.

#include "tr_local.h"

#define DLIGHT_AT_RADIUS 16
// at the edge of a dlight's influence, this amount of light will be added

#define SUN_TRACE_DISTANCE 65536.0f
// дальность луча от точки к солнцу (вдоль tr.sunDirection)

#define SUN_TRACE_START_OFFSET 2.0f
// старт солнечного луча на столько единиц выше точки вдоль sunDirection,
// чтобы луч не начался внутри пола/поверхности, на которой стоит энтити

static void R_ComputeRuntimeEntityLight(trRefEntity_t* ent, entityLightState_t* state);

/*
===============
R_TransformDlights

Transforms the origins of an array of dlights.
Used by both the front end (for DlightBmodel) and
the back end (before doing the lighting calculation)
===============
*/
void R_TransformDlights(int count, dlight_t* dl, orientationr_t* or) {
	int i;
	vec3_t temp;

	for(i = 0; i < count; i++, dl++) {
		VectorSubtract(dl->origin, or->origin, temp);
		dl->transformed[0] = DotProduct(temp, or->axis[0]);
		dl->transformed[1] = DotProduct(temp, or->axis[1]);
		dl->transformed[2] = DotProduct(temp, or->axis[2]);
	}
}

/*
=================
R_DlightEntityMask

Return the bit mask of lights whose bounding spheres intersect an entity's
local-space bounds.  The mask is consumed by the regular additive dlight pass,
so it is also what enables per-pixel shadows on MD3/IQM entities.
=================
*/
dlightMask_t R_DlightEntityMask(const vec3_t bounds[2]) {
	int i, j;
	dlightMask_t mask;
	dlight_t* dl;

	DlightMask_Clear(&mask);
	if(!tr.refdef.num_dlights || !bounds) return mask;

	R_TransformDlights(tr.refdef.num_dlights, tr.refdef.dlights, &tr.or);

	for(i = 0; i < tr.refdef.num_dlights && i < MAX_DLIGHTS; i++) {
		vec3_t localForward;
		dl = &tr.refdef.dlights[i];

		if(r_dynamicLightCull->integer) {
			for(j = 0; j < 3; j++) {
				if(dl->transformed[j] - bounds[1][j] > dl->radius) break;
				if(bounds[0][j] - dl->transformed[j] > dl->radius) break;
			}
			if(j != 3) continue;

			// For projectors, also reject entity bounds that are wholly outside
			// the cone. Transform the direction into the same local space as the
			// transformed light origin before testing the entity's local bounds.
			if(dl->type == DLIGHT_PROJECTOR) {
				vec3_t center, halfSize, toCenter;
				float boundRadius, forwardDistance, radialSq, coneRadius;
				VectorSet(center, (bounds[0][0] + bounds[1][0]) * 0.5f, (bounds[0][1] + bounds[1][1]) * 0.5f, (bounds[0][2] + bounds[1][2]) * 0.5f);
				VectorSet(halfSize, (bounds[1][0] - bounds[0][0]) * 0.5f, (bounds[1][1] - bounds[0][1]) * 0.5f, (bounds[1][2] - bounds[0][2]) * 0.5f);
				boundRadius = VectorLength(halfSize);
				VectorSubtract(center, dl->transformed, toCenter);
				for(j = 0; j < 3; j++) localForward[j] = DotProduct(dl->forward, tr.or.axis[j]);
				VectorNormalize(localForward);
				forwardDistance = DotProduct(toCenter, localForward);
				if(forwardDistance + boundRadius <= 0.0f || forwardDistance - boundRadius >= dl->radius) continue;
				radialSq = VectorLengthSquared(toCenter) - forwardDistance * forwardDistance;
				coneRadius = Com_Clamp(0.0f, dl->radius, forwardDistance) * dl->coneTangent + boundRadius;
				if(radialSq > coneRadius * coneRadius) continue;
			}
		}

		DlightMask_Set(&mask, i);
	}

	return mask;
}

/*
=============
R_DlightBmodel

Determine which dynamic lights may effect this bmodel
=============
*/
void R_DlightBmodel(bmodel_t* bmodel) {
	int i;
	dlightMask_t mask;
	msurface_t* surf;

	mask = R_DlightEntityMask(bmodel->bounds);
	DlightMask_Copy(&tr.currentEntity->needDlights, &mask);

	// Set the light mask on all the surfaces in this inline model.
	for(i = 0; i < bmodel->numSurfaces; i++) {
		surf = tr.world->surfaces + bmodel->firstSurface + i;

		switch(*surf->data) {
			case SF_FACE:
			case SF_GRID:
			case SF_TRIANGLES: DlightMask_Copy(&((srfBspSurface_t*)surf->data)->dlightBits, &mask); break;
			default: break;
		}
	}
}

/*
=============================================================================

RUNTIME ENTITY LIGHTING

Единый расчёт освещения энтити на точке: вклад солнца, ближайших light-энтити
карты (с опциональной трассировкой) и dlight-ов складывается в ambientLight /
directedLight, направление результирующего направленного света — lightDir.
lightgrid не используется.

=============================================================================
*/

/*
=================
R_TraceWorld

Луч через мир (только world-модель, brush-модели движков игры рендереру
недоступны). Рендерер подгружается движком как отдельная библиотека, поэтому
qcommon вызывается через refimport (ri.CM_BoxTrace / ri.CM_InlineModel).
Возвращает false, если трассировка недоступна (указатели не заполнены) —
тогда вызвавшие функции считают лучи неперекрытыми (r_entityLightTrace
фактически выключена).
=================
*/
static bool R_TraceWorld(const vec3_t start, const vec3_t end, trace_t* trace) {
	static bool warnedMissing;

	if(!ri.CM_BoxTrace || !ri.CM_InlineModel) {
		if(!warnedMissing) {
			warnedMissing = true;
			ri.Printf(PRINT_WARNING, "WARNING: r_entityLightTrace disabled - refimport does not provide CM_BoxTrace/CM_InlineModel\n");
		}
		return false;
	}

	if(!tr.world) return false;

	ri.CM_BoxTrace(trace, start, end, vec3_origin, vec3_origin, ri.CM_InlineModel(0), CONTENTS_SOLID, false);
	return true;
}

/*
=================
R_LightTraceBlocked

Перекрыт ли луч от энтити к источнику (light-энтити, dlight-ы) обычной
геометрией. true — свет перекрыт.
=================
*/
static bool R_LightTraceBlocked(const vec3_t start, const vec3_t end) {
	trace_t trace;

	if(!R_TraceWorld(start, end, &trace)) return false;
	return (trace.fraction < 1.0f);
}

/*
=================
R_SunTraceBlocked

Луч от точки к солнцу вдоль tr.sunDirection. Небо (SURF_SKY) солнцу не
помеха: луч попадает в первый solid на своём пути, и если это sky-браш, то
между точкой и небом обычной геометрии нет, а за скайбоксом её и не бывает —
значит солнце достаёт до точки. Стены/крыши возвращают true.
=================
*/
static bool R_SunTraceBlocked(const vec3_t point) {
	trace_t trace;
	vec3_t start, end;

	VectorMA(point, SUN_TRACE_START_OFFSET, tr.sunDirection, start);
	VectorMA(point, SUN_TRACE_DISTANCE, tr.sunDirection, end);

	if(!R_TraceWorld(start, end, &trace)) return false;
	if(trace.fraction >= 1.0f) return false;         // до конца луча геометрии нет
	if(trace.surfaceFlags & SURF_SKY) return false;  // небо не перекрывает солнце
	return true;
}

/*
=================
R_RuntimeLightForPoint

Единый расчёт освещения в точке: солнце + light-энтити карты + dlight-ы
(+ проход имитации подсвета r_entityExtraLight).
Возвращает сумму вкладов в ambientLight / directedLight (шкала 0-255) и
направление доминирующего источника lightDir (нормализован, world space).
world — таблица light-энтити (tr.world или загружаемая карта при расчёте
вершинных направлений), traceLights — включена ли трассировка; с ней лучи
проводятся к солнцу (tr.sunDirection), к отобранным light-энтити и к
dlight-ам с включёнными тенями (dl->shadows).
Типы источников отключаются кварами: r_entityBspLights, r_entitySun,
r_entityDlights, r_entityProjectors.
=================
*/
float R_BSPLightRadius(const lightEnt_t* light) {
	if(!light || light->brightness <= 0.0f) return 0.0f;

	/* Preserve the original linear light-key-to-radius mapping. */
	return light->brightness * r_entityLightDistance->value;
}

static float R_LinearLightFalloff(float dist, float radius) {
	if(radius <= 0.0f || dist >= radius) return 0.0f;
	return 1.0f - dist / radius;
}

/*
 * selectedBspLights != NULL selects the entity path: BSP point lights are
 * cached individually and later rendered in separate per-pixel passes.
 * A NULL pointer preserves the aggregate behavior of the public point-light
 * query helpers (R_LightForPoint / R_LightDirForPoint).
 */
static void R_RuntimeLightForPoint(world_t* world, const vec3_t point,
	vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir, bool traceLights,
	int* selectedBspLights, int* numSelectedBspLights) {
	vec3_t direction;
	vec3_t delta;
	vec3_t sunDirected;
	float sunDirectedLength;
	int best[MAX_ENTITY_LIGHTS];
	float bestDistSq[MAX_ENTITY_LIGHTS];
	int numBest = 0;
	int numBestMax;
	int i, l;

	if(numSelectedBspLights) *numSelectedBspLights = 0;

	VectorClear(ambientLight);
	VectorClear(directedLight);
	VectorClear(direction);

	/* Sun is genuinely directional, so direct and ambient scales are independent. */
	if(r_entitySun->integer != 0) {
		bool sunVisible = true;
		float sunScale = r_entityLightScale->value * r_entitySunScale->value;
		float sunAmbientScale = r_entityLightAmbScale->value * r_entitySunAmbScale->value;

		if(traceLights && R_SunTraceBlocked(point)) sunVisible = false;
		if(sunVisible) {
			VectorScale(tr.sunLight, sunScale, sunDirected);
			sunDirectedLength = VectorLength(sunDirected);
			if(sunDirectedLength > 0.0f) {
				VectorAdd(directedLight, sunDirected, directedLight);
				VectorMA(direction, sunDirectedLength, tr.sunDirection, direction);
			}
			if(sunAmbientScale != 0.0f) {
				VectorMA(ambientLight, sunAmbientScale, tr.sunLight, ambientLight);
			}
		}
	}

	/*
	 * BSP point lights: select up to r_entityLightNum nearest lights within
	 * the linear influence radius (brightness * r_entityLightDistance).
	 * Entity rendering stores indices and draws each source independently;
	 * query helpers still receive an aggregate.
	 */
	numBestMax = Com_Clamp(0, MAX_ENTITY_LIGHTS, r_entityLightNum->integer);
	if(r_entityBspLights->integer != 0 && world && world->numLightEnts &&
		world->lightEnts && numBestMax > 0) {
		for(l = 0; l < world->numLightEnts; l++) {
			lightEnt_t* le = &world->lightEnts[l];
			float radius = R_BSPLightRadius(le);
			float distSq;
			int j;

			if(radius <= 0.0f) continue;

			VectorSubtract(le->origin, point, delta);
			distSq = VectorLengthSquared(delta);
			if(distSq >= radius * radius) continue;

			if(numBest == 0) {
				numBest = 1;
				best[0] = l;
				bestDistSq[0] = distSq;
				continue;
			}
			if(numBest >= numBestMax && distSq >= bestDistSq[numBest - 1]) continue;
			j = numBest < numBestMax ? numBest++ : numBest - 1;
			while(j > 0 && bestDistSq[j - 1] > distSq) {
				bestDistSq[j] = bestDistSq[j - 1];
				best[j] = best[j - 1];
				j--;
			}
			bestDistSq[j] = distSq;
			best[j] = l;
		}

		for(i = 0; i < numBest; i++) {
			lightEnt_t* le = &world->lightEnts[best[i]];
			float distSq = bestDistSq[i];
			float dist = sqrtf(distSq);
			float radius = R_BSPLightRadius(le);
			float photons = le->brightness;
			float ambientScale = r_entityLightAmbScale->value * r_entityBSPLightAmbScale->value;
			float directScale = r_entityLightScale->value * r_entityBSPLightScale->value;
			float falloff;

			if(radius <= 0.0f || dist >= radius) continue;
			falloff = R_LinearLightFalloff(dist, radius);
			if(falloff <= 0.0f) continue;
			if(traceLights && R_LightTraceBlocked(point, le->origin)) continue;

			if(selectedBspLights) {
				if(numSelectedBspLights && *numSelectedBspLights < MAX_ENTITY_LIGHTS) {
					selectedBspLights[*numSelectedBspLights] = best[i];
					(*numSelectedBspLights)++;
				}
			} else if(directScale != 0.0f) {
				vec3_t contribution;
				float weight;

				VectorScale(le->color, photons * falloff * directScale, contribution);
				VectorAdd(directedLight, contribution, directedLight);

				weight = VectorLength(contribution);
				if(weight > 0.0f && dist > 0.001f) {
					VectorSubtract(le->origin, point, delta);
					VectorScale(delta, 1.0f / dist, delta);
					VectorMA(direction, weight, delta, direction);
				}
			}

			if(ambientScale != 0.0f) {
				vec3_t ambientContribution;
				VectorScale(le->color, photons * falloff * ambientScale, ambientContribution);
				VectorAdd(ambientLight, ambientContribution, ambientLight);
			}
		}
	}

	/*
	 * Dynamic lights also contribute a separate entity-lightmap tint.
	 * Their native per-pixel shader pass is rendered independently in
	 * ForwardDlightEnt(), including projector cookies and dynamic 3D shadows.
	 * These toggles/scales affect only this cached entity-lighting contribution.
	 */
	for(l = 0; l < tr.refdef.num_dlights && l < MAX_DLIGHTS; l++) {
		dlight_t* dl = &tr.refdef.dlights[l];
		float dist, falloff, ambientScale, lightScale;
		vec3_t contribution;

		if(dl->radius <= 0.0f) continue;
		if(dl->type == DLIGHT_PROJECTOR) {
			if(r_entityProjectors->integer == 0) continue;
			lightScale = r_entityProjectorScale->value;
		} else {
			if(r_entityDlights->integer == 0) continue;
			lightScale = r_entityDlightScale->value;
		}
		if(lightScale <= 0.0f) continue;

		if(dl->type == DLIGHT_PROJECTOR) {
			vec3_t toPoint, forward;
			float forwardDistance, radialSq, coneRadius;

			VectorSubtract(point, dl->origin, toPoint);
			VectorCopy(dl->forward, forward);
			VectorNormalize(forward);
			forwardDistance = DotProduct(toPoint, forward);
			if(forwardDistance <= 0.0f) continue;
			radialSq = VectorLengthSquared(toPoint) - forwardDistance * forwardDistance;
			coneRadius = Com_Clamp(0.0f, dl->radius, forwardDistance) * dl->coneTangent;
			if(radialSq > coneRadius * coneRadius) continue;
		}

		VectorSubtract(dl->origin, point, delta);
		dist = VectorLength(delta);
		if(dist >= dl->radius) continue;
		falloff = 1.0f - dist / dl->radius;
		if(falloff <= 0.0f) continue;

		if(dl->shadows && traceLights && R_LightTraceBlocked(point, dl->origin)) continue;

		ambientScale = r_entityLightAmbScale->value * lightScale *
			(dl->type == DLIGHT_PROJECTOR ? r_entityProjectorAmbScale->value : r_entityDlightAmbScale->value);
		if(ambientScale != 0.0f) {
			VectorScale(dl->color,
				dl->intensity * 255.0f * falloff * ambientScale, contribution);
			VectorAdd(ambientLight, contribution, ambientLight);
		}

		/* Legacy query mode: keep directed output for non-entity callers. */
		if(!selectedBspLights) {
			float directScale = r_entityLightScale->value * lightScale;
			if(directScale != 0.0f) {
				float weight;
				VectorScale(dl->color, dl->intensity * 255.0f * falloff * directScale, contribution);
				VectorAdd(directedLight, contribution, directedLight);
				weight = VectorLength(contribution);
				if(weight > 0.0f && dist > 0.001f) {
					VectorScale(delta, 1.0f / dist, delta);
					VectorMA(direction, weight, delta, direction);
				}
			}
		}
	}

	/*
	 * Extra light is a cheap ambient bounce approximation. It never creates
	 * another fake dominant direction; distance and intensity stay independent.
	 */
	if(r_entityExtraLight->value > 0.0f && r_entityExtraLightScale->value != 0.0f) {
		float extraDistance = r_entityExtraLight->value;
		float extraScale = r_entityExtraLightScale->value;
		float distSq, dist, radius, falloff, contributionValue;
		vec3_t contribution;

		if(r_entityBspLights->integer != 0 && world && world->lightEnts) {
			for(l = 0; l < world->numLightEnts; l++) {
				lightEnt_t* le = &world->lightEnts[l];
				radius = R_BSPLightRadius(le) * extraDistance;
				if(radius <= 0.0f) continue;

				VectorSubtract(le->origin, point, delta);
				distSq = VectorLengthSquared(delta);
				if(distSq >= radius * radius) continue;
				dist = sqrtf(distSq);
				falloff = R_LinearLightFalloff(dist, radius);
				if(falloff <= 0.0f) continue;

				contributionValue = le->brightness * falloff *
					extraScale * r_entityLightAmbScale->value * r_entityBSPLightAmbScale->value;
				VectorScale(le->color, contributionValue, contribution);
				VectorAdd(ambientLight, contribution, ambientLight);
			}
		}

		if(r_entityDlights->integer != 0) {
			for(l = 0; l < tr.refdef.num_dlights && l < MAX_DLIGHTS; l++) {
				dlight_t* dl = &tr.refdef.dlights[l];

				if(dl->type != DLIGHT_POINT || dl->radius <= 0.0f) continue;
				radius = dl->radius * extraDistance;
				VectorSubtract(dl->origin, point, delta);
				distSq = VectorLengthSquared(delta);
				if(distSq >= radius * radius) continue;

				dist = sqrtf(distSq);
				falloff = R_LinearLightFalloff(dist, radius);
				if(falloff <= 0.0f) continue;

				contributionValue = (dl->intensity * 255.0f) * falloff *
					extraScale * r_entityLightAmbScale->value * r_entityDlightAmbScale->value *
					r_entityDlightScale->value;
				VectorScale(dl->color, contributionValue, contribution);
				VectorAdd(ambientLight, contribution, ambientLight);
			}
		}
	}

	if(!VectorNormalize2(direction, lightDir)) {
		VectorCopy(tr.sunDirection, lightDir);
	}
}

/*
=================
R_ResetEntityLighting

Сброс кэша рантайм-освещения: при R_Init и загрузке новой карты, чтобы слоты
не наследовали свет предыдущей сцены.
=================
*/
void R_ResetEntityLighting(void) {
	int i;

	for(i = 0; i < MAX_REFENTITIES; i++) {
		tr.entityLightStates[i].frame = -1;
		tr.entityLightStates[i].numBspLights = 0;
	}
	tr.entityLightBudget = 0;
	tr.entityLightCursor = 0;
}

/*
=================
R_EntityLightingCvarsChanged

Entity lighting values are cached per entity. Invalidate the cache when any
runtime-light cvar changes so tint toggles and scales take effect immediately
instead of waiting for the round-robin refresh budget.
=================
*/
static bool R_EntityLightingCvarsChanged(void) {
	cvar_t* lightCvars[] = {
		r_entityLightTrace,
		r_entityLightNum,
		r_entityLightDistance,
		r_entityLightScale,
		r_entityLightAmbScale,
		r_entityDlightScale,
		r_entityDlightAmbScale,
		r_entityProjectorScale,
		r_entityProjectorAmbScale,
		r_entitySunScale,
		r_entitySunAmbScale,
		r_entityBSPLightScale,
		r_entityBSPLightAmbScale,
		r_entityBspLights,
		r_entitySun,
		r_entityDlights,
		r_entityProjectors,
		r_entityExtraLight,
		r_entityExtraLightScale
	};
	int i;
	bool changed = false;

	for(i = 0; i < ARRAY_LEN(lightCvars); i++) {
		if(lightCvars[i] && lightCvars[i]->modified) {
			lightCvars[i]->modified = false;
			changed = true;
		}
	}

	return changed;
}

/*
=================
R_UpdateEntityLighting

Round-robin бюджет: один раз за RE_RenderScene пересчитываем максимум
r_entityFrameCount энтити; курсор сдвигается по списку энтити сцены, так что
каждая энтитя получает свежий свет поочерёдно. Энтити, чей слот ещё никогда
не считался (frame < 0), считаются вне бюджета — первый кадр не бывает чёрным.
=================
*/
void R_UpdateEntityLighting(void) {
	trRefdef_t* refdef = &tr.refdef;
	int numEntities, start, k, i;

	if(R_EntityLightingCvarsChanged()) {
		for(i = 0; i < MAX_REFENTITIES; i++) {
			tr.entityLightStates[i].frame = -1;
		}
	}

	tr.entityLightBudget = Com_Clamp(0, MAX_REFENTITIES, r_entityFrameCount->integer);

	if(refdef->rdflags & RDF_NOWORLDMODEL) return;  // превью моделей без мира

	numEntities = refdef->num_entities;
	if(numEntities <= 0) return;

	start = tr.entityLightCursor % numEntities;
	for(k = 0; k < numEntities; k++) {
		int slot = (start + k) % numEntities;
		trRefEntity_t* ent = &refdef->entities[slot];
		entityLightState_t* state = &tr.entityLightStates[slot];

		if(ent->e.reType != RT_MODEL) continue;      // спрайты/лучи свет не запрашивают
		if(state->frame == tr.frameCount) continue;  // уже посчитана в этом кадре

		if(state->frame < 0) {
			// первый расчёт слота — вне бюджета, чтобы первый кадр не был чёрным
		} else {
			// round-robin: при исчерпании бюджета останавливаем расчёт до следующего кадра
			if(tr.entityLightBudget <= 0) break;
			tr.entityLightBudget--;
		}

		R_ComputeRuntimeEntityLight(ent, state);
	}
	tr.entityLightCursor = (start + k) % numEntities;
}

/*
=================
R_ComputeRuntimeEntityLight

Заполняет кэш-слот энтити значениями единого рантайм-расчёта.
=================
*/
static void R_ComputeRuntimeEntityLight(trRefEntity_t* ent, entityLightState_t* state) {
	vec3_t lightOrigin;

	if(ent->e.renderfx & RF_LIGHTING_ORIGIN) {
		VectorCopy(ent->e.lightingOrigin, lightOrigin);
	} else {
		VectorCopy(ent->e.origin, lightOrigin);
	}

	R_RuntimeLightForPoint(tr.world, lightOrigin, state->ambientLight, state->directedLight,
		state->lightDir, (r_entityLightTrace->integer != 0),
		state->bspLightIndices, &state->numBspLights);

	state->frame = tr.frameCount;
}

/*
=================
R_FinishEntityLighting

Общая финализация: clamp 0-255, преобразование направления в модельное
пространство.
=================
*/
static void R_FinishEntityLighting(trRefEntity_t* ent) {
	vec3_t lightDir;
	float d;

	// clamp lights
	{
		float r, g, b, max;

		r = ent->ambientLight[0];
		g = ent->ambientLight[1];
		b = ent->ambientLight[2];

		max = MAX(MAX(r, g), b);

		if(max > 255.0f) {
			max = 255.0f / max;
			ent->ambientLight[0] *= max;
			ent->ambientLight[1] *= max;
			ent->ambientLight[2] *= max;
		}

		r = ent->directedLight[0];
		g = ent->directedLight[1];
		b = ent->directedLight[2];

		max = MAX(MAX(r, g), b);

		if(max > 255.0f) {
			max = 255.0f / max;
			ent->directedLight[0] *= max;
			ent->directedLight[1] *= max;
			ent->directedLight[2] *= max;
		}
	}

	d = VectorLength(ent->directedLight);
	VectorScale(ent->lightDir, d, lightDir);

	// transform the direction to local space
	VectorNormalize(lightDir);
	ent->modelLightDir[0] = DotProduct(lightDir, ent->e.axis[0]);
	ent->modelLightDir[1] = DotProduct(lightDir, ent->e.axis[1]);
	ent->modelLightDir[2] = DotProduct(lightDir, ent->e.axis[2]);

	VectorCopy(lightDir, ent->lightDir);
}

/*
=================
R_ApplyEntityLighting

Копирует кэш-слот в энтитю. BSP point lights остаются отдельными
источниками и рендерятся per-pixel аддитивными проходами.
=================
*/
static void R_ApplyEntityLighting(trRefEntity_t* ent, const entityLightState_t* state) {
	VectorCopy(state->ambientLight, ent->ambientLight);
	VectorCopy(state->directedLight, ent->directedLight);
	VectorCopy(state->lightDir, ent->lightDir);
	ent->numBspLights = state->numBspLights;
	if(ent->numBspLights > 0) {
		memcpy(ent->bspLightIndices, state->bspLightIndices,
			ent->numBspLights * sizeof(ent->bspLightIndices[0]));
	}

	R_FinishEntityLighting(ent);
}

/*
=================
R_SetupEntityLighting

Calculates all the lighting values that will be used
by the Calc_* functions
=================
*/
void R_SetupEntityLighting(const trRefdef_t* refdef, trRefEntity_t* ent) {
	// lighting calculations
	if(ent->lightingCalculated) {
		return;
	}
	ent->lightingCalculated = true;

	if(!(refdef->rdflags & RDF_NOWORLDMODEL)) {
		int slot = ent - refdef->entities;

		if(slot >= 0 && slot < refdef->num_entities) {
			entityLightState_t* state = &tr.entityLightStates[slot];

			if(state->frame != tr.frameCount && tr.entityLightBudget > 0) {
				// страховая ветка для энтити, не покрытых pre-pass'ом
				// (суб-виды кубмап/зеркал): бюджет общий с round-robin
				R_ComputeRuntimeEntityLight(ent, state);
				tr.entityLightBudget--;
			}

			R_ApplyEntityLighting(ent, state);
			return;
		}
	}

	// Keep the legacy preview/menu fallback for standalone model scenes.
	ent->ambientLight[0] = ent->ambientLight[1] = ent->ambientLight[2] = tr.identityLight * 150;
	ent->directedLight[0] = ent->directedLight[1] = ent->directedLight[2] = tr.identityLight * 150;
	VectorCopy(tr.sunDirection, ent->lightDir);
	ent->numBspLights = 0;

	R_FinishEntityLighting(ent);
}

/*
=================
R_LightForPoint
=================
*/
int R_LightForPoint(vec3_t point, vec3_t ambientLight, vec3_t directedLight, vec3_t lightDir) {
	if(!tr.world) return false;
	if(!tr.world->numLightEnts && VectorLengthSquared(tr.sunLight) <= 0.0f) return false;

	R_RuntimeLightForPoint(tr.world, point, ambientLight, directedLight, lightDir,
		(r_entityLightTrace->integer != 0), NULL, NULL);

	return true;
}

int R_LightDirForPoint(vec3_t point, vec3_t lightDir, vec3_t normal, world_t* world) {
	vec3_t ambientLight, directedLight;

	if(!world) return false;

	// вершинные направления мира считаются на загрузке: трассировка до
	// light-энтити здесь слишком дорога, направление берётся без трассировки
	R_RuntimeLightForPoint(world, point, ambientLight, directedLight, lightDir, false, NULL, NULL);

	// направление, смотрящее сквозь поверхность, заменяется её нормалью
	if(DotProduct(lightDir, normal) <= 0.2f) VectorCopy(normal, lightDir);

	return true;
}

bool R_CubemapForShader(bool applyCubemap) {
	if(r_cubeReflections->integer && applyCubemap) return true;
	return false;
}
