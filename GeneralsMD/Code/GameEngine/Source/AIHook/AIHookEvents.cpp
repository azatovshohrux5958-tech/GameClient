/*
**	zh-mcp AI hook for Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 zh-mcp contributors
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// AIHookEvents.cpp ///////////////////////////////////////////////////////////////////////////////
// Event detection. (B1: placeholders; filled in by later milestones.)
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "AIHook/AIHookInternal.h"

namespace AIHook
{

namespace
{
	bool s_gameEndSent = false;
}

void eventsReset() { s_gameEndSent = false; }
void eventsUpdate() {}
bool eventsGameEndSent() { return s_gameEndSent; }

void eventsSendGameEnd(const char* result)
{
	if (s_gameEndSent)
		return;
	s_gameEndSent = true;
	Json d;
	d["result"] = result;
	emitEvent("game_end", d);
}

}	// namespace AIHook
