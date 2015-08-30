// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 2013 James Haley et al.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
// DESCRIPTION:
// 
// Shared intermission code
//
//-----------------------------------------------------------------------------

#ifndef IN_LUDE_H__
#define IN_LUDE_H__

#include "p_mobj.h" // HEADER-FIXME

struct wbstartstruct_t;

// Intermission object struct
struct interfns_t
{
   void (*Ticker)(void);         // called by IN_Ticker
   void (*DrawBackground)(void); // called various places
   void (*Drawer)(void);         // called by IN_Drawer
   void (*Start)(wbstartstruct_t *wbstartstruct); // called by IN_Start
   // IOANCH: added for the bot to know
   bool (*TallyDone)();
};

// intercam
#define MAXCAMERAS 128

extern int intertime;
extern int acceleratestage;

class MobjCollection;
extern MobjCollection camerathings;
extern Mobj *wi_camera;

extern char *in_fontname;
extern char *in_bigfontname;
extern char *in_bignumfontname;

void IN_AddCameras(void);
void IN_slamBackground(void);
void IN_checkForAccelerate(void);
void IN_Ticker(void);
void IN_Drawer(void);
void IN_DrawBackground(void);
void IN_Start(wbstartstruct_t *wbstartstruct);

#endif

// EOF
