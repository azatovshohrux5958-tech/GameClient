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

// AIHook.cpp /////////////////////////////////////////////////////////////////////////////////////
// Main-thread side of the AI hook: enable/port detection, request dispatch, responses, match
// start/end tracking and the shared visibility helpers used by state and events.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "AIHook/AIHook.h"
#include "AIHook/AIHookInternal.h"

#include "Common/GlobalData.h"
#include "Common/KindOf.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/Module/StealthUpdate.h"

#include "gitinfo.h"

#pragma comment(lib, "shell32.lib")	// CommandLineToArgvW

namespace AIHook
{

namespace
{
	bool s_initialized = false;
	bool s_enabled = false;
	bool s_clientConnected = false;
	unsigned s_clientGen = 0;
	bool s_wasInMatch = false;
	unsigned s_lastEventFrame = 0xffffffffu;

	//---------------------------------------------------------------------------------------------
	/// Command line: "-aihook" enables, "-aiport <n>" sets the port. Env ZH_AI_HOOK / ZH_AI_PORT.
	/// Parsed here instead of in CommandLine.cpp to keep the engine diff minimal; the engine's own
	/// parser ignores flags it doesn't know.
	void readConfig(bool& enabled, int& port)
	{
		enabled = false;
		port = DEFAULT_PORT;

		const char* envHook = getenv("ZH_AI_HOOK");
		if (envHook && (strcmp(envHook, "1") == 0 || _stricmp(envHook, "true") == 0))
			enabled = true;
		const char* envPort = getenv("ZH_AI_PORT");
		if (envPort && atoi(envPort) > 0)
			port = atoi(envPort);

		int argc = 0;
		LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
		if (argvW)
		{
			for (int i = 1; i < argc; ++i)
			{
				if (_wcsicmp(argvW[i], L"-aihook") == 0)
					enabled = true;
				else if (_wcsicmp(argvW[i], L"-aiport") == 0 && i + 1 < argc)
				{
					int p = _wtoi(argvW[i + 1]);
					if (p > 0 && p < 65536)
						port = p;
					++i;
				}
			}
			LocalFree(argvW);
		}
	}

	void sendJson(const Json& j)
	{
		serverSend(s_clientGen, j.dump(-1, ' ', false, Json::error_handler_t::replace));
	}

	void sendError(const Json& id, const std::string& code, const std::string& message)
	{
		Json resp;
		resp["id"] = id;
		resp["ok"] = false;
		resp["frame"] = protoFrame();
		Json err;
		err["code"] = code;
		err["message"] = message;
		resp["error"] = err;
		sendJson(resp);
	}

	void handleLine(const std::string& line)
	{
		Json msg = Json::parse(line, nullptr, false);
		if (msg.is_discarded() || !msg.is_object() || !msg.contains("id") || !msg.contains("cmd") || !msg["cmd"].is_string())
		{
			Json data;
			data["code"] = "BAD_ARGS";
			data["message"] = "request must be a JSON object with \"id\" and string \"cmd\"";
			emitEvent("error", data);
			return;
		}

		const Json id = msg["id"];
		const std::string cmd = msg["cmd"].get<std::string>();
		Json args = Json::object();
		if (msg.contains("args") && !msg["args"].is_null())
		{
			if (!msg["args"].is_object())
			{
				sendError(id, "BAD_ARGS", "args must be an object");
				return;
			}
			args = msg["args"];
		}

		if (!isKnownCommand(cmd))
		{
			sendError(id, "UNKNOWN_CMD", strf("unknown command '%s'", cmd.c_str()));
			return;
		}
		if (cmd != "ping" && cmd != "get_status" && !inMatch())
		{
			sendError(id, "NOT_IN_GAME", "no match running");
			return;
		}

		try
		{
			Json result = runCommand(cmd, args);
			Json resp;
			resp["id"] = id;
			resp["ok"] = true;
			resp["frame"] = protoFrame();
			resp["result"] = result;
			sendJson(resp);
		}
		catch (const CmdError& e)
		{
			sendError(id, e.code, e.message);
		}
		catch (const std::exception& e)
		{
			// a json type error from a mistyped argument we didn't check explicitly
			sendError(id, "BAD_ARGS", e.what());
		}
	}

	void sendGameStart()
	{
		Player* p = localPlayer();
		Json data;
		data["map"] = mapName();
		data["local_player"] = p ? (int)p->getPlayerIndex() : -1;
		data["side"] = p ? p->getSide().str() : "";
		emitEvent("game_start", data);
	}
}

//-------------------------------------------------------------------------------------------------
// Common helpers
//-------------------------------------------------------------------------------------------------
std::string strf(const char* format, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	buf[sizeof(buf) - 1] = 0;
	return std::string(buf);
}

bool inMatch()
{
	if (TheGameLogic == nullptr || ThePlayerList == nullptr)
		return false;
	if (!TheGameLogic->isInInteractiveGame() || TheGameLogic->isLoadingMap() || TheGameLogic->isClearingGameData())
		return false;
	return ThePlayerList->getLocalPlayer() != nullptr;
}

double logicFps()
{
	return (double)LOGICFRAMES_PER_SECOND;
}

unsigned protoFrame()
{
	if (TheGameLogic == nullptr)
		return 0;
	// GeneralsOnline may run logic at 60 Hz; the protocol counts at 30 Hz.
	return (unsigned)(((unsigned long long)TheGameLogic->getFrame() * PROTO_FPS) / LOGICFRAMES_PER_SECOND);
}

Player* localPlayer()
{
	return ThePlayerList ? ThePlayerList->getLocalPlayer() : nullptr;
}

void emitEvent(const char* name, const Json& data)
{
	if (!s_clientConnected)
		return;
	Json ev;
	ev["event"] = name;
	ev["frame"] = protoFrame();
	ev["data"] = data;
	sendJson(ev);
}

double r1(double v)
{
	return floor(v * 10.0 + 0.5) / 10.0;
}

std::string mapName()
{
	if (TheGlobalData == nullptr)
		return std::string();
	std::string path = TheGlobalData->m_mapName.str();
	size_t slash = path.find_last_of("\\/");
	if (slash != std::string::npos)
		path = path.substr(slash + 1);
	size_t dot = path.find_last_of('.');
	if (dot != std::string::npos)
		path = path.substr(0, dot);
	return path;
}

const char* objectKind(const Object* obj)
{
	const ThingTemplate* tt = obj->getTemplate();
	if (tt->isKindOf(KINDOF_SUPPLY_SOURCE) && !tt->isKindOf(KINDOF_CASH_GENERATOR))
		return "other";
	if (tt->isKindOf(KINDOF_STRUCTURE))
		return "structure";
	if (tt->isKindOf(KINDOF_INFANTRY) || tt->isKindOf(KINDOF_VEHICLE) || tt->isKindOf(KINDOF_AIRCRAFT) ||
			tt->isKindOf(KINDOF_HUGE_VEHICLE) || tt->isKindOf(KINDOF_BOAT) || tt->isKindOf(KINDOF_DOZER) ||
			tt->isKindOf(KINDOF_HARVESTER))
		return "unit";
	if (obj->getAIUpdateInterface() != nullptr && obj->isSelectable())
		return "unit";
	return "other";
}

bool isReportable(const Object* obj)
{
	if (obj == nullptr || obj->isDestroyed() || obj->isEffectivelyDead())
		return false;
	const ThingTemplate* tt = obj->getTemplate();
	if (tt->isKindOf(KINDOF_PROJECTILE) || tt->isKindOf(KINDOF_SHRUBBERY) || tt->isKindOf(KINDOF_OPTIMIZED_TREE) ||
			tt->isKindOf(KINDOF_PROP) || tt->isKindOf(KINDOF_DRAWABLE_ONLY) || tt->isKindOf(KINDOF_BRIDGE) ||
			tt->isKindOf(KINDOF_BRIDGE_TOWER) || tt->isKindOf(KINDOF_LANDMARK_BRIDGE) || tt->isKindOf(KINDOF_SMALL_MISSILE) ||
			tt->isKindOf(KINDOF_BALLISTIC_MISSILE) || tt->isKindOf(KINDOF_PARACHUTE) || tt->isKindOf(KINDOF_BLAST_CRATER) ||
			tt->isKindOf(KINDOF_INERT))
		return false;
	if (tt->isKindOf(KINDOF_STRUCTURE) || tt->isKindOf(KINDOF_SUPPLY_SOURCE) || tt->isKindOf(KINDOF_CRATE) ||
			tt->isKindOf(KINDOF_MINE) || tt->isKindOf(KINDOF_TECH_BUILDING))
		return true;
	return obj->isSelectable() || obj->getAIUpdateInterface() != nullptr;
}

bool isVisibleToLocal(const Object* obj)
{
	Player* me = localPlayer();
	if (me == nullptr || ThePartitionManager == nullptr)
		return false;
	if (obj->getControllingPlayer() == me)
		return true;
	// units inside a transport or garrison aren't individually visible
	if (obj->isContained())
		return false;
	if (ThePartitionManager->getShroudStatusForPlayer(me->getPlayerIndex(), obj->getPosition()) != CELLSHROUD_CLEAR)
		return false;
	// protocol §5: stealthed-and-undetected enemies are excluded even on clear cells
	if (obj->testStatus(OBJECT_STATUS_STEALTHED) && !obj->testStatus(OBJECT_STATUS_DETECTED))
	{
		const Player* owner = obj->getControllingPlayer();
		if (owner == nullptr || me->getRelationship(obj->getTeam()) != ALLIES)
			return false;
	}
	return true;
}

bool isEnemyOfLocal(const Object* obj)
{
	Player* me = localPlayer();
	if (me == nullptr || obj->getTeam() == nullptr)
		return false;
	return me->getRelationship(obj->getTeam()) == ENEMIES;
}

const ThingTemplate* apparentTemplate(const Object* obj)
{
	Player* me = localPlayer();
	if (me && obj->getControllingPlayer() != me && obj->testStatus(OBJECT_STATUS_DISGUISED) &&
			!obj->testStatus(OBJECT_STATUS_DETECTED))
	{
		StealthUpdate* su = obj->getStealth();
		if (su && su->isDisguised() && su->getDisguisedTemplate())
			return su->getDisguisedTemplate();
	}
	return obj->getTemplate();
}

int apparentOwner(const Object* obj)
{
	Player* me = localPlayer();
	if (me && obj->getControllingPlayer() != me && obj->testStatus(OBJECT_STATUS_DISGUISED) &&
			!obj->testStatus(OBJECT_STATUS_DETECTED))
	{
		StealthUpdate* su = obj->getStealth();
		if (su && su->isDisguised())
			return su->getDisguisedPlayerIndex();
	}
	const Player* p = obj->getControllingPlayer();
	if (p == nullptr || (ThePlayerList && p == ThePlayerList->getNeutralPlayer()))
		return -1;
	return (int)p->getPlayerIndex();
}

//-------------------------------------------------------------------------------------------------
// Entry point, once per GameEngine::update on the main thread.
//-------------------------------------------------------------------------------------------------
void update()
{
	if (!s_initialized)
	{
		s_initialized = true;
		int port = DEFAULT_PORT;
		readConfig(s_enabled, port);
		if (s_enabled)
		{
			s_enabled = serverStart(port, std::string(GitShortSHA1));
			DEBUG_LOG(("AIHook: %s on 127.0.0.1:%d", s_enabled ? "listening" : "FAILED to bind", port));
		}
	}
	if (!s_enabled)
		return;

	const bool match = inMatch();
	serverPublishStatus(match, protoFrame());

	// match start / end transitions (tracked even without a client: read-only)
	if (match && !s_wasInMatch)
	{
		s_wasInMatch = true;
		stateOnMatchStart();
		eventsReset();
		s_lastEventFrame = 0xffffffffu;
		if (s_clientConnected)
			sendGameStart();
	}
	else if (!match && s_wasInMatch)
	{
		s_wasInMatch = false;
		if (s_clientConnected && !eventsGameEndSent())
			eventsSendGameEnd("unknown");
		stateOnMatchEnd();
	}

	std::vector<Inbound> inbound;
	serverPoll(inbound);
	bool processedRequests = false;
	for (size_t i = 0; i < inbound.size(); ++i)
	{
		const Inbound& in = inbound[i];
		switch (in.kind)
		{
			case Inbound::CONNECTED:
				s_clientConnected = true;
				s_clientGen = in.gen;
				if (match)
				{
					eventsReset();
					s_lastEventFrame = 0xffffffffu;
					sendGameStart();
				}
				break;
			case Inbound::DISCONNECTED:
				if (in.gen == s_clientGen)
					s_clientConnected = false;
				break;
			case Inbound::LINE:
				if (s_clientConnected && in.gen == s_clientGen)
				{
					handleLine(in.line);
					processedRequests = true;
				}
				break;
		}
	}
	if (processedRequests)
		commandsEndOfBatch();

	// events once per protocol frame
	if (s_clientConnected && match)
	{
		unsigned f = protoFrame();
		if (f != s_lastEventFrame)
		{
			s_lastEventFrame = f;
			eventsUpdate();
		}
	}
}

}	// namespace AIHook
