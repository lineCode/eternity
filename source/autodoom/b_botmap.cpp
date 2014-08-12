// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 2013 Ioan Chera
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
// Additional terms and conditions compatible with the GPLv3 apply. See the
// file COPYING-EE for details.
//
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//      Bot helper map
//      Contains an equivalent map of the real map in which an actor with 0
//      width can fit in the same space as one with width on the real map
//
//-----------------------------------------------------------------------------

#include <fstream>
#include <unordered_map>
#include "../z_zone.h"

#include "b_botmap.h"
#include "b_botmaptemp.h"
#include "b_classifier.h"
#include "b_glbsp.h"
#include "b_msector.h"
#include "b_util.h"
#include "../d_files.h"
#include "../doomstat.h"
#include "../ev_specials.h"
#include "../m_bbox.h"
#include "../m_buffer.h"
#include "../m_hash.h"
#include "../m_misc.h"
#include "../m_qstr.h"
#include "../p_maputl.h"
#include "../p_setup.h"
#include "../r_defs.h"
#include "../r_main.h"
#include "../r_state.h"

BotMap *botMap;

bool BotMap::demoPlayingFlag;

const char* const KEY_JSON_VERTICES = "vertices";
const char* const KEY_JSON_SEGS =		"segs";
const char* const KEY_JSON_LINES =	"lines";
const char* const KEY_JSON_SSECTORS = "subsectors";
const char* const KEY_JSON_NODES =	"nodes";
const char* const KEY_JSON_METASECTORS = "metasectors";

const int CACHE_BUFFER_SIZE = 512 * 1024;


//
// BotMap::getTouchedBlocks
//
// Lists the blocks the line touches into a collection, code from P_Setup.cpp,
// P_CreateBlockmap
//
void BotMap::getTouchedBlocks(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2,
                              const std::function<void(int)> &func) const
{
   fixed_t minx = botMap->bMapOrgX >> FRACBITS;
   fixed_t miny = botMap->bMapOrgY >> FRACBITS;
   unsigned tot = botMap->bMapWidth * botMap->bMapHeight;
   int x = (x1 >> FRACBITS) - minx;
   int y = (y1 >> FRACBITS) - miny;
   
   // x-y deltas
   int adx = (x2 - x1) >> FRACBITS, dx = adx < 0 ? -1 : 1;
   int ady = (y2 - y1) >> FRACBITS, dy = ady < 0 ? -1 : 1;
   
   // difference in preferring to move across y (>0)
   // instead of x (<0)
   int diff = !adx ? 1 : !ady ? -1 :
   (((x / BOTMAPBLOCKUNITS) * BOTMAPBLOCKUNITS) +
    (dx > 0 ? BOTMAPBLOCKUNITS-1 : 0) - x) * (ady = D_abs(ady)) * dx -
   (((y / BOTMAPBLOCKUNITS) * BOTMAPBLOCKUNITS) +
    (dy > 0 ? BOTMAPBLOCKUNITS-1 : 0) - y) * (adx = D_abs(adx)) * dy;
   
   // starting block, and pointer to its blocklist structure
   int b = (y / BOTMAPBLOCKUNITS) * botMap->bMapWidth + (x / BOTMAPBLOCKUNITS);
   
   // ending block
   int bend = (((y2 >> FRACBITS) - miny) / BOTMAPBLOCKUNITS) *
   botMap->bMapWidth + (((x2 >> FRACBITS) - minx) / BOTMAPBLOCKUNITS);
   
   // delta for pointer when moving across y
   dy *= botMap->bMapWidth;
   
   // deltas for diff inside the loop
   adx *= BOTMAPBLOCKUNITS;
   ady *= BOTMAPBLOCKUNITS;
   
   // Now we simply iterate block-by-block until we reach the end block.
   while((unsigned int) b < tot)    // failsafe -- should ALWAYS be true
   {
      func(b);
      
      // If we have reached the last block, exit
      if(b == bend)
         break;
      
      // Move in either the x or y direction to the next block
      if(diff < 0)
         diff += ady, b += dx;
      else
         diff -= adx, b += dy;
   }
}

//
// BotMap::getBoxTouchedBlocks
//
// Obtains blocks touched by a rectangular box
//
void BotMap::getBoxTouchedBlocks(fixed_t top, fixed_t bottom,
                                  fixed_t left, fixed_t right,
                                  const std::function<void(int b)> &func) const
{
   
   int xl, xh, yl, yh, bx, by;
   
   xl = (left - bMapOrgX) / BOTMAPBLOCKSIZE;
   xh = (right - bMapOrgX) / BOTMAPBLOCKSIZE;
   yl = (bottom - bMapOrgY) / BOTMAPBLOCKSIZE;
   yh = (top - bMapOrgY) / BOTMAPBLOCKSIZE;
   
   for (bx = xl; bx <= xh; ++bx)
   {
      for (by = yl; by <= yh; ++by)
      {
         func(bx + by * bMapWidth);
      }
   }
}



BotMap::Subsec &BotMap::pointInSubsector(fixed_t x, fixed_t y) const
{
   int nodenum = this->numnodes - 1;
   while(!(nodenum & NF_SUBSECTOR))
      nodenum = this->nodes[nodenum].child[pointOnSide(x, y,
                                                       this->nodes[nodenum])];
   return ssectors[nodenum & ~NF_SUBSECTOR];
}

int BotMap::pointOnSide(fixed_t x, fixed_t y, const Node &node) const
{
   if(!node.dx)
      return x <= node.x ? node.dy > 0 : node.dy < 0;
   
   if(!node.dy)
      return y <= node.y ? node.dx < 0 : node.dx > 0;
   
   x -= node.x;
   y -= node.y;
   
   // Try to quickly decide by looking at sign bits.
   if((node.dy ^ node.dx ^ x ^ y) < 0)
      return (node.dy ^ x) < 0;  // (left is negative)
   // IOANCH: fixed an underflow problem happening when it was FixedMul with
   // >>FRACBITS on a factor
   return FixedMul64(y, node.dx) >= FixedMul64(node.dy, x);
}

//
// BotMap::unsetThingPosition
//
// Unsets a thing's position
//
void BotMap::unsetThingPosition(const Mobj *thing)
{
   if(mobjSecMap.count(thing))
   {
      for (auto it = mobjSecMap[thing].begin(); 
         it != mobjSecMap[thing].end(); 
         ++it) 
      {
         (*it)->mobjlist.erase(thing);
      }
      mobjSecMap[thing].makeEmpty();
   }
}

//
// BotMap::setThingPosition
//
// Sets a thing's position
//
void BotMap::setThingPosition(const Mobj *thing)
{
   fixed_t rad = thing->radius + botMap->radius;
   
   fixed_t top = thing->y + rad;
   fixed_t bottom = thing->y - rad;
   fixed_t left = thing->x - rad;
   fixed_t right = thing->x + rad;
   
   bool foundlines = false;
   getBoxTouchedBlocks(top, bottom, left, right, [&](int b)->void
                       {
                          // Iterate through all segs in the caught block
                          for(auto it = segBlocks[b].begin();
                              it != segBlocks[b].end(); ++it)
                          {
                             Seg *sg = *it;
                             if(!sg->owner)
                                continue;
                             if (right <= sg->bbox[BOXLEFT] ||
                                 left >= sg->bbox[BOXRIGHT] ||
                                 top <= sg->bbox[BOXBOTTOM] ||
                                 bottom >= sg->bbox[BOXTOP])
                             {
                                continue;
                             }
                             if (B_BoxOnLineSide(top, bottom, left, right,
                                                 sg->v[0]->x, sg->v[0]->y,
                                                 sg->dx,
                                                 sg->dy) == -1)
                             {

                                // if seg crosses thing bbox, add it
                                mobjSecMap[thing].add(sg->owner);
                                sg->owner->mobjlist.insert(thing);
                                foundlines = true;
                             }
                          }
                       });
   
   if(!foundlines)
   {
      // not found any intersections, now it's time to set the pointInSubsector
      Subsec &thingSec = pointInSubsector(thing->x, thing->y);
      thingSec.mobjlist.insert(thing);
      mobjSecMap[thing].add(&thingSec);
   }
}

//
// BotMap::unsetLinePositions
//
// Removes all line-subsec references. Used by bot goal manager
//
void BotMap::unsetLinePositions(const line_t &line)
{
   if(lineSecMap.count(&line))
   {
      for (auto it = lineSecMap.at(&line).begin();
           it != lineSecMap.at(&line).end();
           ++it)
      {
         (*it)->linelist.erase(&line);
      }
      lineSecMap.at(&line).makeEmpty();
   }
}

//
// BotMap::operator new
//
// Overloaded to support user
//
void *BotMap::operator new(size_t size, int tag, BotMap **user)
{
   return ZoneObject::operator new(size, tag, (void **)user);
}

//
// ZoneObject::operator delete
//
// Needed
//
void BotMap::operator delete (void *p)
{
   ZoneObject::operator delete(p);
}

//
// ZoneObject::operator delete
//
// Needed
//
void BotMap::operator delete (void *p, int a, BotMap ** b)
{
   ZoneObject::operator delete(p, a, (void**)b);
}

//
// BotMap::canPass
//
// Returns true if one can pass from s1's metasec to s2's
//
bool BotMap::canPass(const BSubsec &s1, const BSubsec &s2, fixed_t height) const
{
   const MetaSector &ms1 = *s1.msector, &ms2 = *s2.msector;

   if (&ms1 == &ms2)
       return true;
   
   fixed_t flh1 = ms1.getFloorHeight(), clh1 = ms1.getCeilingHeight(),
   flh2 = ms2.getFloorHeight(), clh2 = ms2.getCeilingHeight();
   
   if(flh2 == D_MAXINT || clh2 == D_MININT)
      return false;
   if(flh2 - flh1 > 24 * FRACUNIT)
      return false;
   if(clh2 - flh1 < height)
      return false;
   if(clh2 - flh2 < height)
      return false;
   if(clh1 - flh2 < height)
      return false;
   
   return true;
}

//
// B_setMobjPositions
//
// Makes sure to have all mobjs registered on the blockmap
//
static void B_setMobjPositions()
{
   for(Thinker *th = thinkercap.next; th != &thinkercap; th = th->next)
   {
      Mobj *mo;
      
      if(!(mo = thinker_cast<Mobj *>(th)))
         continue;
      
      botMap->setThingPosition(mo);
      
   }
}

//
// B_setSpecLinePositions
//
// Records all special lines on the bot map
//
static void B_setSpecLinePositions()
{
//   fixed_t addx, addy, len;
//   int n, j;
//   fixed_t lenx, leny;
   
   fixed_t botsize = 4 * botMap->radius / 2;
   
   BSubsec *ss;
   
   for (int i = 0; i < numlines; ++i)
   {
      const line_t &line = lines[i];
      const ev_action_t *action = EV_ActionForSpecial(line.special);
      if (action &&
          (action->type == &W1ActionType || action->type == &WRActionType))
          // just add these
      {
          //         printf("Added at %d %d\n", B_Frac2Int(line.v1->x), B_Frac2Int(line.v1->y));
          ss = &botMap->pointInSubsector((line.v1->x + line.v2->x) / 2, (line.v1->y + line.v2->y) / 2);
          ss->linelist.insert(&line);
          botMap->lineSecMap[&line].add(ss);

          //ss = &botMap->pointInSubsector(line.v1->x,line.v1->y);
          //ss->linelist.insert(&line);
          //botMap->lineSecMap[&line].add(ss);
          //
          //ss = &botMap->pointInSubsector(line.v2->x,line.v2->y);
          //ss->linelist.insert(&line);
          //botMap->lineSecMap[&line].add(ss);
          //

          //lenx = D_abs(line.dx);
          //leny = D_abs(line.dy);
          //if(!lenx && !leny)
          //   continue;
          //len = lenx > leny ? lenx : leny;
          //if(len > botsize)
          //{
          //   // size bigger than a player could fit
          //   if(lenx > leny)
          //   {
          //      addx = botsize;
          //      addy = FixedMul(FixedDiv(leny, lenx), addx);
          //      n = (lenx - FRACUNIT) / addx;
          //   }
          //   else
          //   {
          //      addy = botsize;
          //      addx = FixedMul(FixedDiv(lenx, leny), addy);
          //      n = (leny - FRACUNIT) / addy;
          //   }
          //   if(line.dx < 0)
          //      addx = -addx;
          //   if(line.dy < 0)
          //      addy = -addy;
          //   for (j = 1; j <= n; ++j)
          //   {
          //      ss = &botMap->pointInSubsector(line.v1->x + j * addx,
          //                                     line.v1->y + j * addy);
          //      ss->linelist.insert(&line);
          //      botMap->lineSecMap[&line].add(ss);
          //   }
          //}
      }
         else if(action && (action->type == &S1ActionType || action->type == &SRActionType
             || action->type == &DRActionType))
         {
            v2fixed_t mid = {(line.v1->x + line.v2->x) / 2,
               (line.v1->y + line.v2->y) / 2};
            angle_t ang = P_PointToAngle(line.v1->x, line.v1->y,
                                         line.v2->x, line.v2->y) - ANG90;
            mid.x += FixedMul(botsize, B_AngleCosine(ang));
            mid.y += FixedMul(botsize, B_AngleSine(ang));
            
            ss = &botMap->pointInSubsector(mid.x, mid.y);
            ss->linelist.insert(&line);
            botMap->lineSecMap[&line].add(ss);
         }
      
   }
}

//
// BotMap::createBlockMap
//
// Creates the blockmap
//
void BotMap::createBlockMap()
{
	// derive from level blockmap's size
	fixed_t extend = radius / 2 + 8 * FRACUNIT;
	bMapOrgX = bmaporgx - extend;
	bMapOrgY = bmaporgy - extend;
	// assume bmapwidth is exactly level width
	bMapWidth = (2 * extend + bmapwidth * MAPBLOCKSIZE)
		/ BOTMAPBLOCKSIZE + 1;
	bMapHeight = (2 * extend + bmapheight * MAPBLOCKSIZE)
		/ BOTMAPBLOCKSIZE + 1;
	// now, how can i forget about the level blockmap sizes?

	int bsz = botMap->bMapWidth * botMap->bMapHeight;

	for (int i = 0; i < bsz; ++i)
	{
		// Create botmap finals
		segBlocks.add();
	}
}

//
// B_buildBotMapFromScratch
//
// Builds bot map from scratch and saves result to JSON
//
static void B_buildTempBotMapFromScratch(fixed_t radius, const char *digest, const char* cachePath)
{
	// First create the transient bot map
	tempBotMap = new TempBotMap;

	// Make the cache file
	OutBuffer cacheStream;
	cacheStream.CreateFile(cachePath, CACHE_BUFFER_SIZE, OutBuffer::LENDIAN);

	// Generate it
	B_BEGIN_CLOCK
	tempBotMap->generateForRadius(radius, cacheStream);
	B_MEASURE_CLOCK(generateForRadius)
	
	// Move the metasector list to the final bot map
	botMap->msecList.head = tempBotMap->getMsecList().head;
	
	// Feed it into GLBSP. botMap will get in turn all needed data
	B_NEW_CLOCK
	B_GLBSP_Start(&cacheStream);
	B_MEASURE_CLOCK(B_GLBSP_Start)

	// Write the cache file
	cacheStream.Close();
		
	// Prevent tempBotMap from crashing
	tempBotMap->getMsecList().head = nullptr;
	
	// Delete the temp. map
	B_NEW_CLOCK
	delete tempBotMap;
	B_MEASURE_CLOCK(deleteTempBotMap)

}

void BotMap::getAllLivingMonsters()
{
    Thinker* th;
    const Mobj* mo;
    livingMonsters.clear();
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mo = thinker_cast<const Mobj*>(th);
        if (mo && !mo->player && mo->flags & MF_SHOOTABLE && !(mo->flags & MF_NOBLOCKMAP) && mo->health > 0)
        {
            livingMonsters.insert(mo);
        }
    }
}

//
// B_BuildBotMap
//
// The main call to build bot map
//
void BotMap::Build()
{
   
   // Create the BotMap
   B_BEGIN_CLOCK
   botMap = new (PU_LEVEL, &botMap) BotMap;
   B_MEASURE_CLOCK(newBotMap)

   fixed_t radius = 16 * FRACUNIT;
   botMap->radius = radius;

   // Create blockmap
   B_NEW_CLOCK
	botMap->createBlockMap();
   B_MEASURE_CLOCK(createBlockMap)

	// Check for hash existence
	char* digest = g_levelHash.digestToString();
   qstring hashFileName("botmap-");
   hashFileName << digest << ".cache";
   

   B_Log("Looking for level cache %s...\n", hashFileName.constPtr());
   D_CheckAutoDoomPathFile(hashFileName.constPtr(), false);
   
//   if (!fpath)
   {
	   B_Log("Level cache not found\n");
	   B_buildTempBotMapFromScratch(radius, digest, M_SafeFilePath(g_autoDoomPath, hashFileName.constPtr()));
   }
//   else
//   {
//		// TODO
//   }
   efree(digest);
   
   // Place all mobjs on it
   B_setMobjPositions();
   
   // Place all special lines on it
   B_setSpecLinePositions();

   // Find all living monsters
   botMap->getAllLivingMonsters();
}

// EOF
