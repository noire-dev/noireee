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
// This file must be identical in the quake and utils directories

// contents flags are separate bits
// a given brush can contribute multiple content bits

// these definitions also need to be in q_shared.h!
// clang-format off
#define CONTENTS_SOLID         0x00000001  // an eye is never valid in a solid
#define CONTENTS_LAVA          0x00000008
#define CONTENTS_SLIME         0x00000010
#define CONTENTS_WATER         0x00000020
#define CONTENTS_FOG           0x00000040
#define CONTENTS_NOTTEAM1      0x00000080
#define CONTENTS_NOTTEAM2      0x00000100
#define CONTENTS_NOBOTCLIP     0x00000200
#define CONTENTS_AREAPORTAL    0x00008000
#define CONTENTS_PLAYERCLIP    0x00010000
#define CONTENTS_MONSTERCLIP   0x00020000
#define CONTENTS_TELEPORTER    0x00040000
#define CONTENTS_JUMPPAD       0x00080000
#define CONTENTS_CLUSTERPORTAL 0x00100000
#define CONTENTS_DONOTENTER    0x00200000
#define CONTENTS_BOTCLIP       0x00400000
#define CONTENTS_MOVER         0x00800000
#define CONTENTS_ORIGIN        0x01000000  // removed before bsping an entity
#define CONTENTS_BODY          0x02000000  // should never be on a brush, only in game
#define CONTENTS_CORPSE        0x04000000
#define CONTENTS_DETAIL        0x08000000  // brushes not used for the bsp
#define CONTENTS_STRUCTURAL    0x10000000  // brushes used for the bsp
#define CONTENTS_TRANSLUCENT   0x20000000  // don't consume surface fragments inside
#define CONTENTS_TRIGGER       0x40000000
#define CONTENTS_NODROP        0x80000000  // don't leave bodies or items (death fog, lava)

// general flags
#define SURF_NODAMAGE       0x00000001 // never give falling damage
#define SURF_SLICK          0x00000002 // effects game physics
#define SURF_SKY            0x00000004 // lighting from environment map
//#define SURF_LADDER       0x00000008 // unused, replaced by SURF_LADDER
//#define SURF_NOIMPACT     0x00000010 // unused, replaced by SURF_CUBEMAP
#define SURF_NOMARKS        0x00000020 // don't leave missile marks
//#define SURF_FLESH        0x00000040 // unused, replaced by SURF_CUBEMAP_WATER
#define SURF_NODRAW         0x00000080 // don't generate a drawsurface at all
#define SURF_HINT           0x00000100 // make a primary bsp splitter
#define SURF_SKIP           0x00000200 // completely ignore, allowing non-closed brushes
#define SURF_NOLIGHTMAP     0x00000400 // surface doesn't need a lightmap
#define SURF_POINTLIGHT     0x00000800 // generate lighting info at vertexes
//#define SURF_METALSTEPS   0x00001000 // unused, replaced by SURF_METAL
#define SURF_NOSTEPS        0x00002000 // unused
#define SURF_NONSOLID       0x00004000 // don't collide against curves with this set
#define SURF_LIGHTFILTER    0x00008000 // act as a light filter during q3map -light
#define SURF_ALPHASHADOW    0x00010000 // do per-pixel light shadow casting in q3map
#define SURF_NODLIGHT       0x00020000 // unused
#define SURF_DUST           0x00040000 // maybe will be use for dust with materials

// pbr flags
#define SURF_CUBEMAP        0x00000010 // reflective?
#define SURF_CUBEMAP_WATER  0x00000040 // correct reflect?

// material flags
#define SURF_CHAINLINK      0x00080000
#define SURF_CONCRETE       0x00100000
#define SURF_DIRT           0x00200000
#define SURF_DUCT           0x00400000
#define SURF_GRASS          0x00800000
#define SURF_GRAVEL         0x01000000
#define SURF_LADDER         0x00000008 // from SURF_LADDER
#define SURF_METAL          0x00001000 // from SURF_METALSTEPS
#define SURF_METALGRATE     0x02000000
#define SURF_MUD            0x04000000
#define SURF_SAND           0x08000000
#define SURF_SLOSH          0x10000000
#define SURF_TILE           0x20000000
#define SURF_WOOD           0x40000000
#define SURF_WOODPANEL      0x80000000
// clang-format on