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

#include "g_local.h"

void SP_null(gentity_t* self) { G_FreeEntity(self); }

void SP_notnull(gentity_t* self) { G_SetOrigin(self, self->s.origin); }

void TeleportPlayer(gentity_t* player, vec3_t origin, vec3_t angles) {
	bool noAngles;

	noAngles = (angles[0] > 999999.0);
	G_TempEntity(player->client->ps.origin, EV_PARTICLE, PP_TELEPORT_OUT);
	G_TempEntity(origin, EV_PARTICLE, PP_TELEPORT_IN);

	// unlink to make sure it can't possibly interfere with G_KillBox
	trap_UnlinkEntity(player);

	VectorCopy(origin, player->client->ps.origin);
	player->client->ps.origin[2] += 1;
	if(!noAngles) {
		// spit the player out
		AngleVectors(angles, player->client->ps.velocity, NULL, NULL);
		VectorScale(player->client->ps.velocity, 400, player->client->ps.velocity);
		player->client->ps.pm_time = 160;  // hold time
		player->client->ps.pm_flags |= PMF_TIME_KNOCKBACK;
		// set angles
		SetClientViewAngle(player, angles);
	}
	// toggle the teleport bit so the client knows to not lerp
	player->client->ps.eFlags ^= EF_TELEPORT_BIT;
	// kill anything at the destination
	G_KillBox(player);

	// save results of pmove
	BG_PlayerStateToEntityState(&player->client->ps, &player->s, true);

	// use the precise origin for linking
	VectorCopy(player->client->ps.origin, player->r.currentOrigin);

	trap_LinkEntity(player);
}

void SP_misc_teleporter_dest(gentity_t* ent) {}
