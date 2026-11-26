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

// AIHook.h ///////////////////////////////////////////////////////////////////////////////////////
// zh-mcp AI hook: lets an external process (the zh-mcp bridge) read the local player's visible
// game state and issue orders over a localhost TCP socket. See zh-mcp docs/protocol.md (v1).
//
// The hook is OFF unless "-aihook" is on the command line (or env ZH_AI_HOOK=1).
// Port: "-aiport <n>" or env ZH_AI_PORT, default 27950. Bound to 127.0.0.1 only.
//
// The only entry point is AIHook::update(), called once per GameEngine::update() on the main
// (logic) thread, just before TheMessageStream->propagateMessages(). Without -aihook it returns
// immediately. Orders are issued as GameMessages on TheMessageStream exactly like the UI does.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

namespace AIHook
{
	void update();
}
