/*
 * snd_fx.c -- Source-style sound effects (EFX) for the OpenAL backend.
 *
 * Every sound belongs to one of two groups:
 *   LOCAL - UI, own player, soundscape layers          cvars snd_local_<effect>
 *   3D    - positional sounds of the world             cvars snd_3d_<effect>
 *
 * The cvars are the only interface: console, renderer sound zones, cgame, JS -
 * whoever changes them is heard immediately (values glide over s_fxFade seconds).
 * An empty value means "default". Run `snd_fxlist` to see all effects.
 */

#ifdef USE_OPENAL

#include "snd_local.h"
#include "qal.h"

#ifdef USE_INTERNAL_OPENAL_HEADERS
#include "AL/efx.h"
#include "AL/efx-presets.h"
#elif defined(_MSC_VER) || defined(__APPLE__)
#include <efx.h>
#include <efx-presets.h>
#else
#include <AL/efx.h>
#include <AL/efx-presets.h>
#endif

// ---------------------------------------------------------------- effect tables

enum { F_VOLUME, F_PITCH, F_LOWPASS, F_HIGHPASS, F_WALL, F_REVERB_MIX, F_REVERB_DECAY, F_ECHO, F_ECHO_DELAY, F_ECHO_FEEDBACK, F_MOD_MIX, F_MOD_AMOUNT, F_MOD_RATE, F_COUNT };

static const struct {
	const char* name;
	float def;
} fxNum[F_COUNT] = {
    {"volume", 1.0f},         // group gain 0..1
    {"pitch", 1.0f},          // playback speed multiplier
    {"lowpass", 1.0f},        // 1 = clear, 0 = muffled (high frequencies cut)
    {"highpass", 1.0f},       // 1 = clear, 0 = thin (low frequencies cut)
    {"wall", 1.0f},           // 3D only: strength of muffling by walls between you and the source
    {"reverb_mix", 0.5f},     // reverb wet level 0..1 (needs `reverb` preset)
    {"reverb_decay", 0.0f},   // override preset decay time in seconds (0 = preset)
    {"echo", 0.0f},           // echo wet level 0..1
    {"echo_delay", 0.2f},     // seconds, max 0.207
    {"echo_feedback", 0.4f},  // 0..1
    {"mod_mix", 0.5f},        // wet level of `mod`
    {"mod_amount", 0.5f},     // depth / edge 0..1
    {"mod_rate", 0.0f},       // Hz (0 = effect default)
};

// string effects: snd_*_reverb <preset name|number|off> (presets come from dsp_presets.cfg), snd_*_mod <chorus|flanger|distortion|ringmod|off>
static const char* modName[] = {"off", "chorus", "flanger", "distortion", "ringmod"};
static const ALenum modEfx[] = {AL_EFFECT_NULL, AL_EFFECT_CHORUS, AL_EFFECT_FLANGER, AL_EFFECT_DISTORTION, AL_EFFECT_RING_MODULATOR};
#define NUM_MODS 5

// EAX reverb = 26 floats in EFXEAXREVERBPROPERTIES order (+ trailing int we don't touch); 0 = pan slot, set via the *fv call
#define RV_N 26
typedef struct {
	float f[RV_N];
} rv_t;
typedef char rv_size_check[sizeof(EFXEAXREVERBPROPERTIES) == (RV_N + 1) * sizeof(float) ? 1 : -1];

// reverb presets are loaded from dsp_presets.cfg ("name" { "key" "value" ... }); keys that are not given keep the generic values
typedef struct {
	char name[32];
	rv_t p;
	int hfLimit;
} preset_t;
static preset_t* presets;
static int numPresets, maxPresets;

static const struct {
	const char* key;
	int idx;
	bool vec;  // "x y z"
} rvKey[] = {
    {"density", 0, false},         {"diffusion", 1, false},       {"gain", 2, false},           {"gainhf", 3, false},
    {"gainlf", 4, false},          {"decaytime", 5, false},       {"decayhfratio", 6, false},   {"decaylfratio", 7, false},
    {"reflectionsgain", 8, false}, {"reflectionsdelay", 9, false}, {"reflectionspan", 10, true}, {"latereverbgain", 13, false},
    {"latereverbdelay", 14, false}, {"latereverbpan", 15, true},  {"echotime", 18, false},      {"echodepth", 19, false},
    {"modulationtime", 20, false}, {"modulationdepth", 21, false}, {"airabsorptiongainhf", 22, false}, {"hfreference", 23, false},
    {"lfreference", 24, false},    {"roomrolloff", 25, false},
};
static const ALenum eaxId[RV_N] = {AL_EAXREVERB_DENSITY,       AL_EAXREVERB_DIFFUSION,
                                   AL_EAXREVERB_GAIN,          AL_EAXREVERB_GAINHF,
                                   AL_EAXREVERB_GAINLF,        AL_EAXREVERB_DECAY_TIME,
                                   AL_EAXREVERB_DECAY_HFRATIO, AL_EAXREVERB_DECAY_LFRATIO,
                                   AL_EAXREVERB_REFLECTIONS_GAIN, AL_EAXREVERB_REFLECTIONS_DELAY,
                                   AL_EAXREVERB_REFLECTIONS_PAN,  0,
                                   0,                          AL_EAXREVERB_LATE_REVERB_GAIN,
                                   AL_EAXREVERB_LATE_REVERB_DELAY, AL_EAXREVERB_LATE_REVERB_PAN,
                                   0,                          0,
                                   AL_EAXREVERB_ECHO_TIME,     AL_EAXREVERB_ECHO_DEPTH,
                                   AL_EAXREVERB_MODULATION_TIME, AL_EAXREVERB_MODULATION_DEPTH,
                                   AL_EAXREVERB_AIR_ABSORPTION_GAINHF, AL_EAXREVERB_HFREFERENCE,
                                   AL_EAXREVERB_LFREFERENCE,   AL_EAXREVERB_ROOM_ROLLOFF_FACTOR};

// ---------------------------------------------------------------- state

typedef struct {
	cvar_t* num[F_COUNT];
	cvar_t *reverb, *mod;
	float cur[F_COUNT];  // smoothed values actually applied
	rv_t rv;             // smoothed reverb parameters
	int hfLimit;
	int modType;
	ALuint slot[3], eff[3];  // reverb, echo, modulation
	float appCur[F_COUNT];   // what the AL objects currently hold
	rv_t appRv;
	int appMod, appHfLimit;
} fxGroup_t;

typedef struct {
	int group;  // routing currently set on the AL source (-1 = unknown)
	float gain, lf, hf, pitch;
	float occ, occTarget;
	bool fresh;
} fxSrc_t;

static struct {
	bool on;
	int sends, numSrc, frame, last, uw;
	float dt, k;
	bool water;
	vec3_t listener;
	fxGroup_t grp[2];
	fxSrc_t* src;
	ALuint* filter;
} fx;

static cvar_t *s_fxFade, *s_waterHF, *s_waterGain, *s_waterReverb, *s_wallHF, *s_wallGain;

static LPALGENEFFECTS fxGenEffects;
static LPALDELETEEFFECTS fxDeleteEffects;
static LPALEFFECTI fxEffecti;
static LPALEFFECTF fxEffectf;
static LPALEFFECTFV fxEffectfv;
static LPALGENFILTERS fxGenFilters;
static LPALDELETEFILTERS fxDeleteFilters;
static LPALFILTERI fxFilteri;
static LPALFILTERF fxFilterf;
static LPALGENAUXILIARYEFFECTSLOTS fxGenSlots;
static LPALDELETEAUXILIARYEFFECTSLOTS fxDeleteSlots;
static LPALAUXILIARYEFFECTSLOTI fxSloti;
static LPALAUXILIARYEFFECTSLOTF fxSlotf;

static const char* const groupName[2] = {"local", "3d"};

// ---------------------------------------------------------------- helpers

static float FX_Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// close enough = snap, otherwise glide
static float FX_Step(float c, float t, float k) {
	float d = t - c;
	return Q_fabs(d) < 1e-3f ? t : c + d * k;
}

// an empty cvar means "use the default"
static float FX_Val(const cvar_t* cv, float def) { return cv->string[0] ? cv->value : def; }

static int FX_Preset(const char* s) {
	int i;
	if(!*s || !Q_stricmp(s, "off") || !Q_stricmp(s, "none")) return -1;
	if(*s >= '0' && *s <= '9') {
		i = atoi(s);
		return (i >= 1 && i <= numPresets) ? i - 1 : -1;
	}
	for(i = 0; i < numPresets; i++)
		if(!Q_stricmp(s, presets[i].name)) return i;
	return -1;
}

static int FX_ModType(const char* s) {
	int i;
	for(i = 1; i < NUM_MODS; i++)
		if(!Q_stricmp(s, modName[i])) return i;
	return 0;
}

// ---------------------------------------------------------------- dsp_presets.cfg

static const EFXEAXREVERBPROPERTIES generic = EFX_REVERB_PRESET_GENERIC;  // base for keys a preset does not set

static void FX_LoadPresets(void) {
	char* buf;
	char* p;
	char name[32], key[64];
	long len;

	if(presets) Z_Free(presets);
	presets = NULL;
	numPresets = maxPresets = 0;

	len = FS_ReadFile("dsp_presets.cfg", (void**)&buf);
	if(len <= 0 || !buf) {
		Com_Printf("WARNING: dsp_presets.cfg not found, no reverb presets\n");
		return;
	}

	for(p = buf;;) {
		preset_t* pr;
		char* tok;

		Q_strncpyz(name, COM_Parse(&p), sizeof(name));
		if(!name[0]) break;

		if(numPresets == maxPresets) {  // grows as needed, no limit
			preset_t* np = Z_Malloc((maxPresets ? maxPresets * 2 : 32) * sizeof(preset_t));
			if(presets) {
				memcpy(np, presets, numPresets * sizeof(preset_t));
				Z_Free(presets);
			}
			presets = np;
			maxPresets = maxPresets ? maxPresets * 2 : 32;
		}
		pr = &presets[numPresets];
		Q_strncpyz(pr->name, name, sizeof(pr->name));
		memcpy(&pr->p, &generic, sizeof(pr->p));
		pr->hfLimit = generic.iDecayHFLimit;

		if(*COM_Parse(&p) != '{') {
			Com_Printf("dsp_presets.cfg: expected '{' after \"%s\"\n", pr->name);
			break;
		}
		while(1) {
			int k;
			Q_strncpyz(key, COM_Parse(&p), sizeof(key));
			if(!key[0] || key[0] == '}') break;
			tok = COM_Parse(&p);

			if(!Q_stricmp(key, "decayhflimit")) {
				pr->hfLimit = atoi(tok) != 0;
				continue;
			}
			for(k = 0; k < (int)(sizeof(rvKey) / sizeof(rvKey[0])); k++) {
				if(Q_stricmp(key, rvKey[k].key)) continue;
				if(rvKey[k].vec)
					sscanf(tok, "%f %f %f", &pr->p.f[rvKey[k].idx], &pr->p.f[rvKey[k].idx + 1], &pr->p.f[rvKey[k].idx + 2]);
				else
					pr->p.f[rvKey[k].idx] = atof(tok);
				break;
			}
			if(k == (int)(sizeof(rvKey) / sizeof(rvKey[0]))) Com_Printf("dsp_presets.cfg: unknown key \"%s\" in \"%s\"\n", key, pr->name);
		}
		numPresets++;
	}
	FS_FreeFile(buf);
	Com_Printf("...loaded %d reverb presets from dsp_presets.cfg\n", numPresets);
}

static void FX_Reload_f(void) {
	FX_LoadPresets();
	fx.uw = FX_Preset("underwater");
}

// ---------------------------------------------------------------- group update

static void FX_Apply(fxGroup_t* g) {
	const float* c = g->cur;
	int i;

	// reverb
	for(i = 0; i < RV_N; i++) {
		if(!eaxId[i]) continue;
		if(i == 10 || i == 15)
			fxEffectfv(g->eff[0], eaxId[i], &g->rv.f[i]);
		else
			fxEffectf(g->eff[0], eaxId[i], g->rv.f[i]);
	}
	fxEffecti(g->eff[0], AL_EAXREVERB_DECAY_HFLIMIT, g->hfLimit);
	fxSloti(g->slot[0], AL_EFFECTSLOT_EFFECT, g->eff[0]);
	fxSlotf(g->slot[0], AL_EFFECTSLOT_GAIN, FX_Clamp(c[F_REVERB_MIX], 0, 1));

	// echo
	fxEffectf(g->eff[1], AL_ECHO_DELAY, FX_Clamp(c[F_ECHO_DELAY], 0.001f, AL_ECHO_MAX_DELAY));
	fxEffectf(g->eff[1], AL_ECHO_FEEDBACK, FX_Clamp(c[F_ECHO_FEEDBACK], 0, 1));
	fxSloti(g->slot[1], AL_EFFECTSLOT_EFFECT, g->eff[1]);
	fxSlotf(g->slot[1], AL_EFFECTSLOT_GAIN, FX_Clamp(c[F_ECHO], 0, 1));

	// modulation family
	if(g->modType != g->appMod) fxEffecti(g->eff[2], AL_EFFECT_TYPE, modEfx[g->modType]);
	switch(g->modType) {
		case 1: fxEffectf(g->eff[2], AL_CHORUS_DEPTH, FX_Clamp(c[F_MOD_AMOUNT], 0, 1)); fxEffectf(g->eff[2], AL_CHORUS_RATE, c[F_MOD_RATE] > 0 ? FX_Clamp(c[F_MOD_RATE], 0, 10) : 1.1f); break;
		case 2: fxEffectf(g->eff[2], AL_FLANGER_DEPTH, FX_Clamp(c[F_MOD_AMOUNT], 0, 1)); fxEffectf(g->eff[2], AL_FLANGER_RATE, c[F_MOD_RATE] > 0 ? FX_Clamp(c[F_MOD_RATE], 0, 10) : 0.27f); break;
		case 3: fxEffectf(g->eff[2], AL_DISTORTION_EDGE, FX_Clamp(c[F_MOD_AMOUNT], 0, 1)); break;
		case 4: fxEffectf(g->eff[2], AL_RING_MODULATOR_FREQUENCY, c[F_MOD_RATE] > 0 ? FX_Clamp(c[F_MOD_RATE], 0, 8000) : 440.0f); break;
	}
	fxSloti(g->slot[2], AL_EFFECTSLOT_EFFECT, g->eff[2]);
	fxSlotf(g->slot[2], AL_EFFECTSLOT_GAIN, g->modType ? FX_Clamp(c[F_MOD_MIX], 0, 1) : 0.0f);

	memcpy(g->appCur, g->cur, sizeof(g->cur));
	g->appRv = g->rv;
	g->appMod = g->modType;
	g->appHfLimit = g->hfLimit;
}

static void FX_UpdateGroup(fxGroup_t* g) {
	float t[F_COUNT];
	rv_t target;
	bool silent;
	int i, pi = FX_Preset(g->reverb->string);

	for(i = 0; i < F_COUNT; i++) t[i] = FX_Val(g->num[i], fxNum[i].def);

	// underwater: everything muffled and wrapped in the "underwater" reverb
	if(fx.water) {
		t[F_VOLUME] *= s_waterGain->value;
		t[F_LOWPASS] *= s_waterHF->value;
		if(s_waterReverb->value > 0 && fx.uw >= 0) {
			pi = fx.uw;
			t[F_REVERB_MIX] = s_waterReverb->value;
		}
	}
	if(pi < 0) t[F_REVERB_MIX] = 0;  // keep old parameters so the tail fades out instead of jumping

	silent = g->cur[F_REVERB_MIX] < 0.001f;
	for(i = 0; i < F_COUNT; i++) g->cur[i] = FX_Step(g->cur[i], t[i], fx.k);

	if(pi >= 0) {
		memcpy(&target, &presets[pi].p, sizeof(target));
		g->hfLimit = presets[pi].hfLimit;
		if(t[F_REVERB_DECAY] > 0) target.f[5] = FX_Clamp(t[F_REVERB_DECAY], 0.1f, 20.0f);
		if(silent) g->rv = target;  // was inaudible: no need to morph from a stale preset
		else
			for(i = 0; i < RV_N; i++) g->rv.f[i] = FX_Step(g->rv.f[i], target.f[i], fx.k);
	}
	g->modType = FX_ModType(g->mod->string);

	if(memcmp(g->cur, g->appCur, sizeof(g->cur)) || memcmp(&g->rv, &g->appRv, sizeof(rv_t)) || g->modType != g->appMod || g->hfLimit != g->appHfLimit) FX_Apply(g);
}

// ---------------------------------------------------------------- public interface

void S_FX_Listener(const vec3_t origin, int inwater) {
	VectorCopy(origin, fx.listener);
	fx.water = inwater != 0;
}

// once per frame: read cvars, glide, push to OpenAL when something changed
void S_FX_Update(void) {
	int now, g;
	if(!fx.on) return;

	now = Sys_Milliseconds();
	fx.dt = FX_Clamp((now - fx.last) * 0.001f, 0.0f, 0.25f);
	fx.last = now;
	fx.k = s_fxFade->value > 0 ? FX_Clamp(fx.dt / s_fxFade->value, 0, 1) : 1.0f;
	fx.frame++;

	for(g = 0; g < 2; g++) FX_UpdateGroup(&fx.grp[g]);
}

// called by the source setup code: the AL source was reset (pitch = 1), forget our cached values
void S_FX_Reset(int idx) {
	if(!fx.on || idx < 0 || idx >= fx.numSrc) return;
	fx.src[idx].pitch = 1.0f;
	fx.src[idx].fresh = true;
}

// route one source through its group: sends, direct filter (volume/lowpass/highpass/wall), pitch
void S_FX_Source(int idx, unsigned int al, bool local, const vec3_t origin) {
	fxGroup_t* g;
	fxSrc_t* s;
	float gain, lf, hf, pitch;
	int k;

	if(!fx.on || idx < 0 || idx >= fx.numSrc) return;
	g = &fx.grp[local ? 0 : 1];
	s = &fx.src[idx];

	if(s->group != (local ? 0 : 1)) {
		for(k = 0; k < fx.sends; k++) qalSource3i(al, AL_AUXILIARY_SEND_FILTER, g->slot[k], k, AL_FILTER_NULL);
		s->group = local ? 0 : 1;
	}

	// wall muffling: one ray from the listener to the source, smoothed
	if(!local && origin && g->cur[F_WALL] > 0 && CM_NumInlineModels() > 0) {
		if(s->fresh || (fx.frame + idx) % 4 == 0) {
			trace_t tr;
			CM_BoxTrace(&tr, fx.listener, origin, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, false);
			s->occTarget = tr.fraction < 1.0f ? 1.0f : 0.0f;
			if(s->fresh) s->occ = s->occTarget;
		}
		s->occ += (s->occTarget - s->occ) * FX_Clamp(fx.dt * 10.0f, 0, 1);
	} else
		s->occ = s->occTarget = 0;

	gain = g->cur[F_VOLUME];
	lf = g->cur[F_HIGHPASS];
	hf = g->cur[F_LOWPASS];
	if(s->occ > 0.001f) {
		float w = s->occ * FX_Clamp(g->cur[F_WALL], 0, 1);
		gain *= 1.0f + (s_wallGain->value - 1.0f) * w;
		hf *= 1.0f + (s_wallHF->value - 1.0f) * w;
	}
	gain = FX_Clamp(gain, 0, 1);
	lf = FX_Clamp(lf, 0, 1);
	hf = FX_Clamp(hf, 0, 1);

	if(s->fresh || Q_fabs(gain - s->gain) > 0.002f || Q_fabs(lf - s->lf) > 0.002f || Q_fabs(hf - s->hf) > 0.002f) {
		fxFilterf(fx.filter[idx], AL_BANDPASS_GAIN, gain);
		fxFilterf(fx.filter[idx], AL_BANDPASS_GAINLF, lf);
		fxFilterf(fx.filter[idx], AL_BANDPASS_GAINHF, hf);
		qalSourcei(al, AL_DIRECT_FILTER, fx.filter[idx]);
		s->gain = gain;
		s->lf = lf;
		s->hf = hf;
	}

	pitch = FX_Clamp(g->cur[F_PITCH], 0.25f, 4.0f);
	if(Q_fabs(pitch - s->pitch) > 0.001f) {
		qalSourcef(al, AL_PITCH, pitch);
		s->pitch = pitch;
	}
	s->fresh = false;
}

static void FX_List_f(void) {
	int i;
	Com_Printf("cvars snd_local_<name> (UI/own/soundscape) and snd_3d_<name> (positional); empty = default\n");
	for(i = 0; i < F_COUNT; i++) Com_Printf("  %-14s default %g\n", fxNum[i].name, fxNum[i].def);
	Com_Printf("  reverb         off | 1..%d | name from dsp_presets.cfg (snd_fxreload re-reads it)\n", numPresets);
	for(i = 0; i < numPresets; i++) Com_Printf("%s%s", presets[i].name, i % 6 == 5 || i == numPresets - 1 ? "\n" : " ");
	Com_Printf("  mod            off | chorus flanger distortion ringmod\n");
}

// extra context attributes: every source needs 3 aux sends (reverb, echo, mod)
const int* S_FX_ContextAttribs(void) {
	static const int attribs[] = {ALC_MAX_AUXILIARY_SENDS, 3, 0};
	return attribs;
}

bool S_FX_Init(void* device, int numSources) {
	bool ok = true;
	int g, i, sends = 0;

	memset(&fx, 0, sizeof(fx));

	s_fxFade = Cvar_Get("s_fxFade", "0.4", CVAR_ARCHIVE);        // seconds effect values glide to a new target (0 = instant)
	s_waterHF = Cvar_Get("s_waterHF", "0.05", CVAR_ARCHIVE);     // underwater lowpass
	s_waterGain = Cvar_Get("s_waterGain", "0.7", CVAR_ARCHIVE);  // underwater volume
	s_waterReverb = Cvar_Get("s_waterReverb", "0.6", CVAR_ARCHIVE);  // underwater reverb level (0 = off)
	s_wallHF = Cvar_Get("s_wallHF", "0.08", CVAR_ARCHIVE);       // lowpass for a sound fully behind a wall
	s_wallGain = Cvar_Get("s_wallGain", "0.6", CVAR_ARCHIVE);    // volume for a sound fully behind a wall

	if(!qalcIsExtensionPresent((ALCdevice*)device, "ALC_EXT_EFX")) {
		Com_Printf("OpenAL: no EFX, sound effects disabled.\n");
		return false;
	}
	qalcGetIntegerv((ALCdevice*)device, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
	if(sends < 1) {
		Com_Printf("OpenAL: no auxiliary sends, sound effects disabled.\n");
		return false;
	}
	fx.sends = sends > 3 ? 3 : sends;

#define LOAD(p, name) \
	*(void**)&p = (void*)qalGetProcAddress(name); \
	ok &= p != NULL;
	LOAD(fxGenEffects, "alGenEffects") LOAD(fxDeleteEffects, "alDeleteEffects") LOAD(fxEffecti, "alEffecti") LOAD(fxEffectf, "alEffectf") LOAD(fxEffectfv, "alEffectfv")
	LOAD(fxGenFilters, "alGenFilters") LOAD(fxDeleteFilters, "alDeleteFilters") LOAD(fxFilteri, "alFilteri") LOAD(fxFilterf, "alFilterf")
	LOAD(fxGenSlots, "alGenAuxiliaryEffectSlots") LOAD(fxDeleteSlots, "alDeleteAuxiliaryEffectSlots") LOAD(fxSloti, "alAuxiliaryEffectSloti") LOAD(fxSlotf, "alAuxiliaryEffectSlotf")
#undef LOAD
	if(!ok) {
		Com_Printf("OpenAL: EFX functions missing, sound effects disabled.\n");
		return false;
	}

	qalGetError();
	FX_LoadPresets();
	fx.numSrc = numSources;
	fx.src = Z_Malloc(numSources * sizeof(fxSrc_t));
	fx.filter = Z_Malloc(numSources * sizeof(ALuint));
	fxGenFilters(numSources, fx.filter);
	for(i = 0; i < numSources; i++) {
		fxFilteri(fx.filter[i], AL_FILTER_TYPE, AL_FILTER_BANDPASS);
		fx.src[i].group = -1;
		fx.src[i].pitch = 1.0f;
		fx.src[i].fresh = true;
	}

	for(g = 0; g < 2; g++) {
		fxGroup_t* gr = &fx.grp[g];
		for(i = 0; i < F_COUNT; i++) {
			gr->num[i] = Cvar_Get(va("snd_%s_%s", groupName[g], fxNum[i].name), va("%g", fxNum[i].def), CVAR_TEMP);
			gr->cur[i] = FX_Val(gr->num[i], fxNum[i].def);
		}
		gr->reverb = Cvar_Get(va("snd_%s_reverb", groupName[g]), "off", CVAR_TEMP);
		gr->mod = Cvar_Get(va("snd_%s_mod", groupName[g]), "off", CVAR_TEMP);
		gr->cur[F_REVERB_MIX] = 0;  // start silent, fade in
		gr->cur[F_ECHO] = 0;

		fxGenSlots(3, gr->slot);
		fxGenEffects(3, gr->eff);
		fxEffecti(gr->eff[0], AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
		fxEffecti(gr->eff[1], AL_EFFECT_TYPE, AL_EFFECT_ECHO);
		memcpy(&gr->rv, &generic, sizeof(gr->rv));
		gr->hfLimit = generic.iDecayHFLimit;
		gr->appMod = -1;
		gr->appHfLimit = -1;
	}

	if(qalGetError() != AL_NO_ERROR) {
		Com_Printf("OpenAL: EFX setup failed, sound effects disabled.\n");
		S_FX_Shutdown();
		return false;
	}

	fx.uw = FX_Preset("underwater");
	fx.last = Sys_Milliseconds();
	fx.on = true;
	Cmd_AddCommand("snd_fxlist", FX_List_f);
	Cmd_AddCommand("snd_fxreload", FX_Reload_f);
	Com_Printf("OpenAL: EFX sound effects enabled (%d sends).\n", fx.sends);
	return true;
}

// sources must already be deleted
void S_FX_Shutdown(void) {
	int g;

	Cmd_RemoveCommand("snd_fxlist");
	Cmd_RemoveCommand("snd_fxreload");
	if(presets) Z_Free(presets);
	presets = NULL;
	numPresets = maxPresets = 0;
	if(fxDeleteSlots) {
		for(g = 0; g < 2; g++) {
			if(fx.grp[g].slot[0]) fxDeleteSlots(3, fx.grp[g].slot);
			if(fx.grp[g].eff[0]) fxDeleteEffects(3, fx.grp[g].eff);
		}
		if(fx.filter) fxDeleteFilters(fx.numSrc, fx.filter);
	}
	if(fx.src) Z_Free(fx.src);
	if(fx.filter) Z_Free(fx.filter);
	memset(&fx, 0, sizeof(fx));
}

#endif
