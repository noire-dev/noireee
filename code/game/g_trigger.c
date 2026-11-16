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
//
#include "g_local.h"

void InitTrigger(gentity_t* self) {
	if(!VectorCompare(self->s.angles, vec3_origin)) G_SetMovedir(self->s.angles, self->movedir);

	trap_SetBrushModel(self, self->model);
	self->r.contents = CONTENTS_TRIGGER;  // replaces the -1 from trap_SetBrushModel
	self->r.svFlags = SVF_NOCLIENT;
}

// the wait time has passed, so set back up for another activation
void multi_wait(gentity_t* ent) { ent->nextthink = 0; }

// the trigger was just activated
// ent->activator should be set to the activator so it can be held through a delay
// so wait for the delay time before firing
void multi_trigger(gentity_t* ent, gentity_t* activator) {
	ent->activator = activator;
	if(ent->nextthink) return;  // can't retrigger until the wait is over

	G_UseTargets(ent, ent->activator);

	if(ent->wait > 0) {
		ent->think = multi_wait;
		ent->nextthink = level.time + (ent->wait + ent->random * crandom()) * 1000;
	} else {
		// we can't just remove (self) here, because this is a touch function
		// called while looping through area links...
		ent->touch = 0;
		ent->nextthink = level.time + FRAMETIME;
		ent->think = G_FreeEntity;
	}
}

void Use_Multi(gentity_t* ent, gentity_t* other, gentity_t* activator) { multi_trigger(ent, activator); }

void Touch_Multi(gentity_t* self, gentity_t* other, trace_t* trace) {
	if(!other->client) {
		return;
	}
	multi_trigger(self, other);
}

/*QUAKED trigger_multiple (.5 .5 .5) ? RED_ONLY BLUE_ONLY
"wait" : Seconds between triggerings, 0.5 default, -1 = one time only.
"random"	wait variance, default is 0
Variable sized repeatable trigger.  Must be targeted at one or more entities.
so, the basic time between firing is a random time between
(wait - random) and (wait + random)
*/
void SP_trigger_multiple(gentity_t* ent) {
	G_SpawnFloat("wait", "0.5", &ent->wait);
	G_SpawnFloat("random", "0", &ent->random);

	if(ent->random >= ent->wait && ent->wait >= 0) {
		ent->random = ent->wait - FRAMETIME;
		G_Printf("trigger_multiple has random >= wait\n");
	}

	ent->touch = Touch_Multi;
	ent->use = Use_Multi;

	InitTrigger(ent);
	trap_LinkEntity(ent);
}

/*
==============================================================================

trigger_always

==============================================================================
*/

void trigger_always_think(gentity_t* ent) {
	G_UseTargets(ent, ent);
	G_FreeEntity(ent);
}

/*QUAKED trigger_always (.5 .5 .5) (-8 -8 -8) (8 8 8)
This trigger will always fire.  It is activated by the world.
*/
void SP_trigger_always(gentity_t* ent) {
	// we must have some delay to make sure our use targets are present
	ent->nextthink = level.time + 300;
	ent->think = trigger_always_think;
}

/*
==============================================================================

trigger_teleport

==============================================================================
*/

void trigger_teleporter_touch(gentity_t* self, gentity_t* other, trace_t* trace) {
	gentity_t* dest;

	if(!other->client) {
		return;
	}
	if(other->client->ps.pm_type == PM_DEAD) {
		return;
	}

	dest = G_PickTarget(self->target);
	if(!dest) {
		G_Printf("Couldn't find teleporter destination\n");
		return;
	}

	TeleportPlayer(other, dest->s.origin, dest->s.angles);
}

/*QUAKED trigger_teleport (.5 .5 .5) ? SPECTATOR
Allows client side prediction of teleportation events.
Must point at a target_position, which will be the teleport destination.
*/
void SP_trigger_teleport(gentity_t* self) {
	InitTrigger(self);

	// make sure the client precaches this sound
	G_SoundIndex("sound/world/jumppad.wav");

	self->s.eType = ET_TELEPORT_TRIGGER;
	self->touch = trigger_teleporter_touch;

	trap_LinkEntity(self);
}

static void portal_touch(gentity_t* self, gentity_t* other, trace_t* trace) {
	gentity_t* out;
	vec3_t delta, local, rotated, angles;
	vec3_t f_s, r_s, u_s, f_d, r_d, u_d;

	if(!other->client) return;
	if(other->client->ps.pm_type == PM_DEAD) return;

	out = G_Find(NULL, FOFS(targetname), self->target);
	if(!out) return;

	// Базисы обоих порталов
	AngleVectors(self->s.angles, f_s, r_s, u_s);
	AngleVectors(out->s.angles, f_d, r_d, u_d);

	// --- Позиция: world -> local(src) -> world(dst) ---
	VectorSubtract(other->client->ps.origin, self->r.currentOrigin, delta);
	local[0] = DotProduct(delta, f_s);
	local[1] = DotProduct(delta, r_s);
	local[2] = DotProduct(delta, u_s);

	VectorScale(f_d, local[0], rotated);
	VectorMA(rotated, local[1], r_d, rotated);
	VectorMA(rotated, local[2], u_d, rotated);
	VectorAdd(out->r.currentOrigin, rotated, other->client->ps.origin);

	// --- Смещение выхода (originDiff выходного портала) ---
	VectorAdd(other->client->ps.origin, self->s.origin2, other->client->ps.origin);

	// --- Скорость: та же ротация ---
	VectorCopy(other->client->ps.velocity, delta);
	local[0] = DotProduct(delta, f_s);
	local[1] = DotProduct(delta, r_s);
	local[2] = DotProduct(delta, u_s);

	VectorScale(f_d, local[0], rotated);
	VectorMA(rotated, local[1], r_d, rotated);
	VectorMA(rotated, local[2], u_d, rotated);
	VectorCopy(rotated, other->client->ps.velocity);

	// --- Углы обзора: прибавляем РАЗНОСТЬ углов ---
	VectorCopy(other->client->ps.viewangles, angles);
	angles[YAW] += out->s.angles[YAW] - self->s.angles[YAW];
	// angles[PITCH] += out->s.angles[PITCH] - self->s.angles[PITCH];
	// angles[ROLL]  += out->s.angles[ROLL]  - self->s.angles[ROLL];
	SetClientViewAngle(other, angles);

	other->client->ps.eFlags ^= EF_TELEPORT_BIT;
	BG_PlayerStateToEntityState(&other->client->ps, &other->s, true);
	VectorCopy(other->client->ps.origin, other->r.currentOrigin);
	trap_LinkEntity(other);
}

void SP_func_portal_in(gentity_t* self) {
	vec3_t angleDiff, originDiff;

	G_SpawnVector("angleDiff", "0 0 0", angleDiff);
	VectorCopy(angleDiff, self->s.angles);

	G_SpawnVector("originDiff", "0 0 0", originDiff);
	VectorCopy(originDiff, self->s.origin2);

	self->s.eType = ET_PORTAL;
	self->touch = portal_touch;
	trap_SetBrushModel(self, self->model);
	self->r.contents = CONTENTS_TRIGGER;
	self->r.svFlags = SVF_NOCLIENT;
	trap_LinkEntity(self);
}

void SP_func_portal_out(gentity_t* self) {
	self->r.contents = CONTENTS_TRIGGER;
	self->touch = NULL;
	self->r.svFlags |= SVF_NOCLIENT;
	trap_LinkEntity(self);
}

/*
==============================================================================

trigger_hurt

==============================================================================
*/

/*QUAKED trigger_hurt (.5 .5 .5) ? START_OFF - SILENT NO_PROTECTION SLOW
Any entity that touches this will be hurt.
It does dmg points of damage each server frame
Targeting the trigger will toggle its on / off state.

SILENT			suppresses playing the sound
SLOW			changes the damage rate to once per second
NO_PROTECTION	*nothing* stops the damage

"dmg"			default 5 (whole numbers only)

*/
void hurt_use(gentity_t* self, gentity_t* other, gentity_t* activator) {
	if(self->r.linked) {
		trap_UnlinkEntity(self);
	} else {
		trap_LinkEntity(self);
	}
}

void hurt_touch(gentity_t* self, gentity_t* other, trace_t* trace) {
	int dflags;

	if(!other->takedamage) {
		return;
	}

	if(self->timestamp > level.time) {
		return;
	}

	if(self->spawnflags & 16) {
		self->timestamp = level.time + 1000;
	} else {
		self->timestamp = level.time + FRAMETIME;
	}

	// play sound
	if(!(self->spawnflags & 4)) G_Sound(other, CHAN_AUTO, G_SoundIndex("sound/world/electro.wav"));

	if(self->spawnflags & 8)
		dflags = DAMAGE_NO_PROTECTION;
	else
		dflags = 0;
	G_Damage(other, self, self, NULL, NULL, self->damage, dflags, MOD_TRIGGER_HURT);
}

void SP_trigger_hurt(gentity_t* self) {
	InitTrigger(self);

	self->touch = hurt_touch;

	if(!self->damage) {
		self->damage = 5;
	}

	self->use = hurt_use;

	// link in to the world if starting active
	if(self->spawnflags & 1) {
		trap_UnlinkEntity(self);
	} else {
		trap_LinkEntity(self);
	}
}

/*
==============================================================================

timer

==============================================================================
*/

/*QUAKED func_timer (0.3 0.1 0.6) (-8 -8 -8) (8 8 8) START_ON
This should be renamed trigger_timer...
Repeatedly fires its targets.
Can be turned on or off by using.

"wait"			base time between triggering all targets, default is 1
"random"		wait variance, default is 0
so, the basic time between firing is a random time between
(wait - random) and (wait + random)

*/
void func_timer_think(gentity_t* self) {
	G_UseTargets(self, self->activator);
	// set time before next firing
	self->nextthink = level.time + 1000 * (self->wait + crandom() * self->random);
}

void func_timer_use(gentity_t* self, gentity_t* other, gentity_t* activator) {
	self->activator = activator;

	// if on, turn it off
	if(self->nextthink) {
		self->nextthink = 0;
		return;
	}

	// turn it on
	func_timer_think(self);
}

void SP_func_timer(gentity_t* self) {
	G_SpawnFloat("random", "1", &self->random);
	G_SpawnFloat("wait", "1", &self->wait);

	self->use = func_timer_use;
	self->think = func_timer_think;

	if(self->random >= self->wait) {
		self->random = self->wait - FRAMETIME;
		G_Printf("func_timer at %s has random >= wait\n", vtos(self->s.origin));
	}

	if(self->spawnflags & 1) {
		self->nextthink = level.time + FRAMETIME;
		self->activator = self;
	}

	self->r.svFlags = SVF_NOCLIENT;
}
