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
	bool s_selectionDirty = false;

	//---------------------------------------------------------------------------------------------
	std::string idText(const Json& v)
	{
		return v.dump(-1, ' ', false, Json::error_handler_t::replace);
	}

	Object* findAlive(ObjectID id)
	{
		Object* obj = TheGameLogic->findObjectByID(id);
		if (obj == nullptr || obj->isDestroyed() || obj->isEffectivelyDead())
			return nullptr;
		return obj;
	}

	void requireCanIssueOrders()
	{
		Player* me = localPlayer();
		if (me == nullptr || !me->isPlayerActive())
			throw CmdError("CANNOT_DO", "the local player is not an active player (observer or defeated)");
		if (TheRecorder && TheRecorder->isPlaybackMode())
			throw CmdError("CANNOT_DO", "orders are not possible during replay playback");
	}

	Coord3D groundPos(double x, double y)
	{
		Coord3D pos;
		pos.x = (Real)x;
		pos.y = (Real)y;
		pos.z = TheTerrainLogic->getGroundHeight(pos.x, pos.y);
		return pos;
	}

	//---------------------------------------------------------------------------------------------
	/// Select exactly these objects for the local player (like a UI click), for the next order.
	void appendSelect(const std::vector<ObjectID>& ids)
	{
		GameMessage* sel = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		sel->appendBooleanArgument(TRUE);	// new group
		for (size_t i = 0; i < ids.size(); ++i)
			sel->appendObjectIDArgument(ids[i]);
		s_selectionDirty = true;
	}

	void appendSelect(ObjectID id)
	{
		std::vector<ObjectID> ids(1, id);
		appendSelect(ids);
	}

	//---------------------------------------------------------------------------------------------
	struct UnitList
	{
		std::vector<ObjectID> ok;
		Json rejected;
	};

	UnitList collectUnits(const Json& args)
	{
		if (!args.contains("unit_ids") || !args["unit_ids"].is_array() || args["unit_ids"].empty())
			throw CmdError("BAD_ARGS", "unit_ids must be a non-empty list of object ids");

		UnitList out;
		out.rejected = Json::array();
		Player* me = localPlayer();
		const Json& ids = args["unit_ids"];
		for (size_t i = 0; i < ids.size(); ++i)
		{
			const Json& v = ids[i];
			const char* code = nullptr;
			ObjectID id = INVALID_ID;
			if (!v.is_number_integer() || v.get<long long>() <= 0)
				code = "BAD_ARGS";
			else
			{
				id = (ObjectID)v.get<long long>();
				Object* obj = findAlive(id);
				if (obj == nullptr)
					code = "UNIT_DEAD";
				else if (obj->getControllingPlayer() != me)
					code = "NOT_OWNED";
				else if (obj->isKindOf(KINDOF_STRUCTURE) || obj->getAIUpdateInterface() == nullptr ||
						obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
					code = "CANNOT_DO";
			}
			if (code)
			{
				Json r;
				r["id"] = v;
				r["code"] = code;
				out.rejected.push_back(r);
			}
			else if (std::find(out.ok.begin(), out.ok.end(), id) == out.ok.end())
				out.ok.push_back(id);
		}
		if (out.ok.empty())
			throw CmdError(out.rejected[0]["code"].get<std::string>().c_str(),
				strf("no valid units in %s", idText(ids).c_str()));
		return out;
	}

	Json orderResult(const UnitList& units)
	{
		Json res;
		res["queued"] = true;
		res["unit_ids"] = units.ok;
		if (!units.rejected.empty())
			res["rejected"] = units.rejected;
		return res;
	}

	Json orderResult(ObjectID id)
	{
		Json res;
		res["queued"] = true;
		Json ids = Json::array();
		ids.push_back(id);
		res["unit_ids"] = ids;
		return res;
	}

	//---------------------------------------------------------------------------------------------
	const CommandSet* commandSetOf(const Object* obj)
	{
		if (TheControlBar == nullptr)
			return nullptr;
		return TheControlBar->findCommandSet(obj->getCommandSetString());
	}

	/// The build button for 'tt' in the producer's command set, as BuildAssistant::isPossibleToMakeUnit finds it.
	const CommandButton* findBuildButton(const Object* producer, const ThingTemplate* tt)
	{
		const CommandSet* cs = commandSetOf(producer);
		if (cs == nullptr)
			return nullptr;
		for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton* b = cs->getCommandButton(i);
			if (b && (b->getCommandType() == GUI_COMMAND_UNIT_BUILD || b->getCommandType() == GUI_COMMAND_DOZER_CONSTRUCT) &&
					b->getThingTemplate() && b->getThingTemplate()->isEquivalentTo(tt))
				return b;
		}
		return nullptr;
	}

	const CommandButton* findUpgradeButton(const Object* producer, const UpgradeTemplate* ut)
	{
		const CommandSet* cs = commandSetOf(producer);
		if (cs == nullptr)
			return nullptr;
		for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
		{
			const CommandButton* b = cs->getCommandButton(i);
			if (b && (b->getCommandType() == GUI_COMMAND_PLAYER_UPGRADE || b->getCommandType() == GUI_COMMAND_OBJECT_UPGRADE) &&
					b->getUpgradeTemplate() == ut)
				return b;
		}
		return nullptr;
	}

	const ThingTemplate* findThing(const std::string& name)
	{
		if (name.empty())
			return nullptr;
		return TheThingFactory->findTemplate(AsciiString(name.c_str()), FALSE);
	}

	const UpgradeTemplate* findUpgradeByName(const std::string& name)
	{
		for (const UpgradeTemplate* ut = TheUpgradeCenter->firstUpgradeTemplate(); ut; ut = ut->friend_getNext())
			if (name == ut->getUpgradeName().str())
				return ut;
		return nullptr;
	}

	const SpecialPowerTemplate* findPowerByName(const std::string& name)
	{
		Int n = TheSpecialPowerStore->getNumSpecialPowers();
		for (Int i = 0; i < n; ++i)
		{
			const SpecialPowerTemplate* sp = TheSpecialPowerStore->getSpecialPowerTemplateByIndex((UnsignedInt)i);
			if (sp && name == sp->getName().str())
				return sp;
		}
		return nullptr;
	}

	ScienceType findScienceByName(const std::string& name)
	{
		std::vector<AsciiString> names = TheScienceStore->friend_getScienceNames();
		for (size_t i = 0; i < names.size(); ++i)
			if (name == names[i].str())
				return TheScienceStore->getScienceFromInternalName(names[i]);	// key already exists
		return SCIENCE_INVALID;
	}

	std::string joinNames(const std::vector<std::string>& v)
	{
		std::string s;
		for (size_t i = 0; i < v.size(); ++i)
		{
			if (i)
				s += ", ";
			s += v[i];
		}
		return s;
	}

	[[noreturn]] void throwMissingPrereq(const ThingTemplate* tt, Player* me)
	{
		std::vector<std::string> miss = missingPrereqNames(tt, me);
		throw CmdError("MISSING_PREREQ", miss.empty() ? std::string("prerequisites not met") : "needs " + joinNames(miss));
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
		{ "get_state", cmdGetState },
		{ "get_map", cmdGetMap },
		{ "query_path", cmdQueryPath },
		{ "get_unit_data", cmdGetUnitData },
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
	if (!s_selectionDirty)
		return;
	s_selectionDirty = false;
	if (!inMatch() || TheInGameUI == nullptr)
		return;

	// put the human's selection back (logic-side selection is per player and orders replaced it)
	std::vector<ObjectID> ids;
	const DrawableList* sel = TheInGameUI->getAllSelectedDrawables();
	if (sel)
	{
		for (DrawableList::const_iterator it = sel->begin(); it != sel->end(); ++it)
		{
			const Drawable* d = *it;
			const Object* obj = d ? d->getObject() : nullptr;
			if (obj && obj->isLocallyControlled())
				ids.push_back(obj->getID());
		}
	}
	if (ids.empty())
		TheMessageStream->appendMessage(GameMessage::MSG_DESTROY_SELECTED_GROUP);
	else
	{
		GameMessage* m = TheMessageStream->appendMessage(GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND);
		m->appendBooleanArgument(TRUE);
		for (size_t i = 0; i < ids.size(); ++i)
			m->appendObjectIDArgument(ids[i]);
	}
}

}	// namespace AIHook
