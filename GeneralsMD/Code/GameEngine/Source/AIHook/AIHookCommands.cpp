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

// AIHookCommands.cpp /////////////////////////////////////////////////////////////////////////////
// Command dispatch, up-front validation and GameMessage building (protocol §6).
//
// Orders are appended to TheMessageStream for the local player exactly as the UI does:
// MSG_CREATE_SELECTED_GROUP_NO_SOUND with the object IDs, then the order message. They then go
// through the normal command list / network path. AIUpdateInterface / AIGroup are never called.
// After a batch of orders the human's own UI selection is re-sent, so a human player sharing the
// game keeps commanding what they have selected.
//
// Name lookups never go through TheNameKeyGenerator for unknown names (that would create keys).
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <algorithm>

#include "AIHook/AIHookInternal.h"

#include "Common/BuildAssistant.h"
#include "Common/GameType.h"
#include "Common/KindOf.h"
#include "Common/MessageStream.h"
#include "Common/Money.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Recorder.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"

namespace AIHook
{

namespace
{
	Object* findAlive(ObjectID id)
	{
		Object* obj = TheGameLogic->findObjectByID(id);
		if (obj == nullptr || obj->isDestroyed() || obj->isEffectivelyDead())
			return nullptr;
		return obj;
	}

	//---------------------------------------------------------------------------------------------
	// Info
	//---------------------------------------------------------------------------------------------
	Json cmdPing(const Json&)
	{
		Json r;
		r["pong"] = true;
		return r;
	}

	Json cmdGetStatus(const Json&)
	{
		Json r;
		bool match = inMatch();
		r["in_game"] = match;
		if (match)
		{
			r["map"] = mapName();
			r["local_player"] = (int)localPlayer()->getPlayerIndex();
		}
		else
		{
			r["map"] = nullptr;
			r["local_player"] = nullptr;
		}
		r["frame"] = protoFrame();
		return r;
	}

	//---------------------------------------------------------------------------------------------
	typedef Json (*CommandFn)(const Json&);
	struct CommandEntry
	{
		const char* name;
		CommandFn fn;
	};

	const CommandEntry s_commands[] =
	{
		{ "ping", cmdPing },
		{ "get_status", cmdGetStatus },
	};

	CommandFn findCommand(const std::string& cmd)
	{
		for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); ++i)
			if (cmd == s_commands[i].name)
				return s_commands[i].fn;
		return nullptr;
	}
}

//-------------------------------------------------------------------------------------------------
// Argument helpers
//-------------------------------------------------------------------------------------------------
bool hasArg(const Json& args, const char* key)
{
	return args.is_object() && args.contains(key) && !args[key].is_null();
}

long long argInt(const Json& args, const char* key)
{
	if (!hasArg(args, key) || !args[key].is_number_integer())
		throw CmdError("BAD_ARGS", strf("%s must be an integer", key));
	return args[key].get<long long>();
}

long long argIntOr(const Json& args, const char* key, long long def)
{
	return hasArg(args, key) ? argInt(args, key) : def;
}

double argReal(const Json& args, const char* key)
{
	if (!hasArg(args, key) || !args[key].is_number())
		throw CmdError("BAD_ARGS", strf("%s must be a number", key));
	double v = args[key].get<double>();
	if (!(v == v))	// NaN
		throw CmdError("BAD_ARGS", strf("%s must be a number", key));
	return v;
}

double argRealOr(const Json& args, const char* key, double def)
{
	return hasArg(args, key) ? argReal(args, key) : def;
}

std::string argStr(const Json& args, const char* key)
{
	if (!hasArg(args, key) || !args[key].is_string())
		throw CmdError("BAD_ARGS", strf("%s must be a string", key));
	return args[key].get<std::string>();
}

unsigned argObjectID(const Json& v, const char* what)
{
	if (!v.is_number_integer() || v.get<long long>() <= 0 || v.get<long long>() > 0x7fffffffLL)
		throw CmdError("BAD_ARGS", strf("%s must be an object id (positive integer)", what));
	return (unsigned)v.get<long long>();
}

Object* requireOwnObject(const Json& idValue, const char* what)
{
	ObjectID id = (ObjectID)argObjectID(idValue, what);
	Object* obj = findAlive(id);
	if (obj == nullptr)
		throw CmdError("UNIT_DEAD", strf("object %u does not exist", (unsigned)id));
	if (obj->getControllingPlayer() != localPlayer())
		throw CmdError("NOT_OWNED", strf("object %u is not yours", (unsigned)id));
	return obj;
}

void requireOnMap(double x, double y)
{
	Region3D ext;
	TheTerrainLogic->getExtent(&ext);
	if (x < ext.lo.x || y < ext.lo.y || x >= ext.hi.x || y >= ext.hi.y)
		throw CmdError("CANNOT_DO", strf("(%.1f,%.1f) is off the map", x, y));
}

//-------------------------------------------------------------------------------------------------
bool isKnownCommand(const std::string& cmd)
{
	return findCommand(cmd) != nullptr;
}

Json runCommand(const std::string& cmd, const Json& args)
{
	CommandFn fn = findCommand(cmd);
	if (fn == nullptr)
		throw CmdError("UNKNOWN_CMD", strf("unknown command '%s'", cmd.c_str()));
	return fn(args);
}

void commandsEndOfBatch()
{
}

}	// namespace AIHook
