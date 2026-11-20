/*
 * tr_sound.c -- func_soundscape
 *
 * Рендерер уже умеет искать брашевые области под камерой (func_display), поэтому звуковые среды живут здесь.
 *
 *   func_soundscape  (brush)  playlooping / playlooping_volume
 *                             playrandom<1..5>  playrandom_time<n>  playrandom_rtime<n>
 *                             playrandom_volumeMin<n>  playrandom_volumeMax<n>   (время в секундах)
 *                             effect_*                       эффекты звуковой системы
 *   worldspawn               effect_* + всё то же: глобальные значения по умолчанию,
 *                            действуют, пока слушатель вне func_soundscape
 *
 *   effect_<name> "value"         обе группы звуков (локальные и 3D)
 *   effect_local_<name> "value"   только локальные      effect_3d_<name> "value"   только позиционные
 *   <name> и значения — см. консольную команду snd_fxlist (reverb, echo, lowpass, ...)
 *
 * Эффекты — это просто cvar'ы snd_local_<name> / snd_3d_<name>, RE_SetSoundEffect их и ставит.
 * Громкость и радиус звуков крутит cgame (trap_S_*Ex), soundscape отдаётся ему через RE_GetSoundscape.
 *
 * Из перекрывающихся soundscapes выигрывает самая маленькая и целиком заменяет остальные:
 * её эффекты и её звуки действуют до выхода.
 */

#include "tr_local.h"

static struct {
	int scape;          // индекс w->soundEnts под слушателем, -1 = нет
	int serial;         // уникальные id soundscape (меняются при смене карты)
	sndEffect_t* list;  // что мы сейчас выставили: при смене soundscape это сбрасывается в дефолт
	int num, max;
} snd = {-1, 0, NULL, 0, 0};

/*
=================
RE_SetSoundEffect

Единственный интерфейс к звуковым эффектам. Пустое значение = дефолт.
=================
*/
void RE_SetSoundEffect(const char* name, const char* value, bool applyLocal, bool apply3D) {
	if(applyLocal) ri.Cvar_Set(va("snd_local_%s", name), value);
	if(apply3D) ri.Cvar_Set(va("snd_3d_%s", name), value);
}

static void R_SoundZonesApply(const world_t* w, int scape) {
	int i;

	// всё, что ставил прошлый soundscape, возвращаем в дефолт (если новый это же и задаёт — перезапишется ниже)
	for(i = 0; i < snd.num; i++) RE_SetSoundEffect(snd.list[i].name, "", snd.list[i].local, snd.list[i].d3);
	snd.num = 0;

	if(w && scape >= 0) {
		const soundEnt_t* e = &w->soundEnts[scape];

		for(i = 0; i < e->numEffects; i++) {
			RE_SetSoundEffect(e->effects[i].name, e->effects[i].value, e->effects[i].local, e->effects[i].d3);

			if(snd.num == snd.max) {  // список растёт сколько нужно
				sndEffect_t* nl = ri.Malloc((snd.max ? snd.max * 2 : 16) * sizeof(sndEffect_t));
				if(snd.list) {
					memcpy(nl, snd.list, snd.num * sizeof(sndEffect_t));
					ri.Free(snd.list);
				}
				snd.list = nl;
				snd.max = snd.max ? snd.max * 2 : 16;
			}
			snd.list[snd.num++] = e->effects[i];
		}
	}
	snd.scape = scape;
}

/*
=================
RE_SoundZones

Вызывается клиентом каждый кадр с позицией слушателя. Эффекты меняются только при входе/выходе из soundscape;
плавность перехода обеспечивает звуковая система (s_fxFade).
=================
*/
void RE_SoundZones(const vec3_t o) {
	const world_t* w = tr.world;
	int i, scape = -1;
	float sv = 0;

	for(i = 0; w && i < w->numSoundRegions; i++) {
		const soundRegion_t* r = &w->soundRegions[i];

		if(o[0] < r->bounds[0][0] || o[1] < r->bounds[0][1] || o[2] < r->bounds[0][2] || o[0] > r->bounds[1][0] || o[1] > r->bounds[1][1] || o[2] > r->bounds[1][2]) continue;

		if(scape < 0 || r->volume < sv) scape = r->ent, sv = r->volume;
	}

	if(scape != snd.scape) R_SoundZonesApply(w, scape);
}

/*
=================
RE_GetSoundscape

soundscape под слушателем для cgame. false, если слушатель не внутри func_soundscape.
=================
*/
bool RE_GetSoundscape(soundscape_t* out) {
	if(!tr.world || snd.scape < 0) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	*out = tr.world->soundEnts[snd.scape].def;
	return true;
}

// эффекты прошлой карты / soundscape сбрасываем в дефолт
static void R_SoundZonesReset(void) { R_SoundZonesApply(NULL, -1); }

void R_SoundZonesShutdown(void) {
	R_SoundZonesReset();
	if(snd.list) ri.Free(snd.list);
	snd.list = NULL;
	snd.num = snd.max = 0;
}

// ключ слота: "playrandom_time3"; у первого слота допустимо и без цифры
static const char* R_SoundSlotKey(int n, char keys[MAX_MAP_ENTITY_KEYS][MAX_MAP_ENTITY_VALUENAME], char values[MAX_MAP_ENTITY_KEYS][MAX_MAP_ENTITY_VALUENAME], const char* base, int slot) {
	const char* r = R_EntityValue(n, keys, values, va("%s%d", base, slot));
	return (!r && slot == 1) ? R_EntityValue(n, keys, values, base) : r;
}

/*
=================
R_LoadSoundEffects

effect_* ключи энтити -> список эффектов. Общие для brush-soundscapes и worldspawn.
=================
*/
static void R_LoadSoundEffects(soundEnt_t* e, int numKeys, char keys[MAX_MAP_ENTITY_KEYS][MAX_MAP_ENTITY_VALUENAME], char values[MAX_MAP_ENTITY_KEYS][MAX_MAP_ENTITY_VALUENAME]) {
	int i;

	for(i = 0; i < numKeys; i++)
		if(!Q_stricmpn(keys[i], "effect_", 7)) e->numEffects++;
	if(e->numEffects) e->effects = ri.Hunk_Alloc(e->numEffects * sizeof(sndEffect_t), h_low);
	e->numEffects = 0;
	for(i = 0; i < numKeys; i++) {
		const char* name = keys[i] + 7;
		sndEffect_t* fx = &e->effects[e->numEffects];

		if(Q_stricmpn(keys[i], "effect_", 7)) continue;
		fx->local = fx->d3 = true;
		if(!Q_stricmpn(name, "local_", 6))
			name += 6, fx->d3 = false;
		else if(!Q_stricmpn(name, "3d_", 3))
			name += 3, fx->local = false;
		Q_strncpyz(fx->name, name, sizeof(fx->name));
		Q_strncpyz(fx->value, values[i], sizeof(fx->value));
		e->numEffects++;
	}
}

/*
=================
R_LoadSoundscapeDef

playlooping / playrandom* ключи -> soundscape def. Общие для func_soundscape и worldspawn.
=================
*/
static void R_LoadSoundscapeDef(soundscape_t* d, int numKeys, char keys[MAX_MAP_ENTITY_KEYS][MAX_MAP_ENTITY_VALUENAME], char values[MAX_MAP_ENTITY_KEYS][MAX_MAP_ENTITY_VALUENAME]) {
	const char* v;
	int i;

	d->id = ++snd.serial;
	v = R_EntityValue(numKeys, keys, values, "playlooping");
	if(v) Q_strncpyz(d->loop, v, sizeof(d->loop));
	v = R_EntityValue(numKeys, keys, values, "playlooping_volume");
	d->loopVolume = v ? atof(v) : 1.0f;

	for(i = 0; i < SNDSCAPE_RANDOMS; i++) {
		int n = i + 1;
		v = R_SoundSlotKey(numKeys, keys, values, "playrandom", n);
		if(v) Q_strncpyz(d->random[i].sound, v, sizeof(d->random[i].sound));
		v = R_SoundSlotKey(numKeys, keys, values, "playrandom_time", n);
		d->random[i].time = v ? atof(v) : 10.0f;
		v = R_SoundSlotKey(numKeys, keys, values, "playrandom_rtime", n);
		d->random[i].rtime = v ? atof(v) : 0.0f;
		v = R_SoundSlotKey(numKeys, keys, values, "playrandom_volumeMin", n);
		d->random[i].volMin = v ? atof(v) : 1.0f;
		v = R_SoundSlotKey(numKeys, keys, values, "playrandom_volumeMax", n);
		d->random[i].volMax = v ? atof(v) : d->random[i].volMin;
	}
}

/*
=================
R_LoadSoundZones

Два прохода по энтити-лампе: посчитать и заполнить. Лимитов на число зон, brush-ей и effect_ ключей нет.
=================
*/
void R_LoadSoundZones(world_t* w) {
	static char keys[MAX_MAP_ENTITY_KEYS][MAX_MAP_ENTITY_VALUENAME];
	static char values[MAX_MAP_ENTITY_KEYS][MAX_MAP_ENTITY_VALUENAME];
	int pass, numKeys, numEnts = 0, numRegions = 0;

	R_SoundZonesReset();
	w->numSoundEnts = w->numSoundRegions = 0;
	w->soundEnts = NULL;
	w->soundRegions = NULL;
	if(!w->entityString) return;

	for(pass = 0; pass < 2; pass++) {
		char* p = w->entityString;

		if(pass == 1) {
			if(!numEnts) return;
			w->soundEnts = ri.Hunk_Alloc(numEnts * sizeof(soundEnt_t), h_low);
			w->soundRegions = ri.Hunk_Alloc(numRegions * sizeof(soundRegion_t), h_low);
		}

		while((numKeys = R_ParseEntityKeys(&p, keys, values)) >= 0) {
			const char* cn = R_EntityValue(numKeys, keys, values, "classname");
			const char* v;
			bool world;
			int mi, i, brush, added = 0;
			bmodel_t* bm;
			soundEnt_t* e;

			if(!cn) continue;
			world = !Q_stricmp(cn, "worldspawn");
			if(!world && Q_stricmp(cn, "func_soundscape")) continue;

			// worldspawn — глобальный soundscape по умолчанию: эффекты и звуки одним энтити. Область
			// на всю карту с объёмом больше любого func_soundscape, поэтому реальный soundscape всегда
			// перекрывает его; глобальные эффекты/звуки слышны, только пока слушатель вне soundscapes.
			if(world) {
				bool has = false;
				for(i = 0; i < numKeys; i++)
					if(!Q_stricmpn(keys[i], "effect_", 7) || !Q_stricmpn(keys[i], "playlooping", 11) || !Q_stricmpn(keys[i], "playrandom", 10)) has = true;
				if(!has) continue;
				if(pass == 0) {
					numEnts++;
					numRegions++;
					continue;
				}
				e = &w->soundEnts[w->numSoundEnts];
				memset(e, 0, sizeof(*e));
				R_LoadSoundEffects(e, numKeys, keys, values);
				R_LoadSoundscapeDef(&e->def, numKeys, keys, values);
				soundRegion_t* r = &w->soundRegions[w->numSoundRegions++];
				r->bounds[0][0] = r->bounds[0][1] = r->bounds[0][2] = -1e9f;
				r->bounds[1][0] = r->bounds[1][1] = r->bounds[1][2] = 1e9f;
				r->volume = 8e27f;
				r->ent = w->numSoundEnts++;
				continue;
			}

			v = R_EntityValue(numKeys, keys, values, "model");
			mi = (v && v[0] == '*') ? atoi(v + 1) : 0;
			if(mi <= 0 || mi >= w->numBModels) {
				if(pass == 0) ri.Printf(PRINT_WARNING, "WARNING: %s with bad or missing model key, ignored\n", cn);
				continue;
			}
			bm = &w->bmodels[mi];

			if(pass == 0) {
				numEnts++;
				numRegions += bm->numBrushes > 0 ? bm->numBrushes : 1;
				continue;
			}

			e = &w->soundEnts[w->numSoundEnts];
			memset(e, 0, sizeof(*e));

			// effect_* -> эффекты
			R_LoadSoundEffects(e, numKeys, keys, values);

			// звуки
			R_LoadSoundscapeDef(&e->def, numKeys, keys, values);

			// по области на каждый brush; нет brush-данных — общая AABB модели
			for(brush = 0; brush < bm->numBrushes && bm->brushBounds; brush++) {
				soundRegion_t* r = &w->soundRegions[w->numSoundRegions];

				if(bm->brushBounds[brush][0][0] > bm->brushBounds[brush][1][0] || bm->brushBounds[brush][0][1] > bm->brushBounds[brush][1][1] || bm->brushBounds[brush][0][2] > bm->brushBounds[brush][1][2]) continue;

				VectorCopy(bm->brushBounds[brush][0], r->bounds[0]);
				VectorCopy(bm->brushBounds[brush][1], r->bounds[1]);
				r->ent = w->numSoundEnts;
				w->numSoundRegions++;
				added++;
			}
			if(!added) {
				soundRegion_t* r = &w->soundRegions[w->numSoundRegions++];
				VectorCopy(bm->bounds[0], r->bounds[0]);
				VectorCopy(bm->bounds[1], r->bounds[1]);
				r->ent = w->numSoundEnts;
			}
			for(i = w->numSoundRegions - (added ? added : 1); i < w->numSoundRegions; i++) {
				soundRegion_t* r = &w->soundRegions[i];
				r->volume = (r->bounds[1][0] - r->bounds[0][0]) * (r->bounds[1][1] - r->bounds[0][1]) * (r->bounds[1][2] - r->bounds[0][2]);
			}
			w->numSoundEnts++;
		}
	}
	ri.Printf(PRINT_ALL, "...loaded %d soundscapes (%d regions)\n", w->numSoundEnts, w->numSoundRegions);
}
