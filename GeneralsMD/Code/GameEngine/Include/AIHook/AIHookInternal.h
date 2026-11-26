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

// AIHookInternal.h ///////////////////////////////////////////////////////////////////////////////
// Shared declarations between the AIHook translation units. Not for use outside AIHook.
// Everything here except the server* functions runs on the main (logic) thread only.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <string>
#include <vector>

#include "GameNetwork/GeneralsOnline/json.hpp"

class Object;
class Player;
class ThingTemplate;

namespace AIHook
{
	typedef nlohmann::ordered_json Json;

	enum { PROTOCOL_VERSION = 1, DEFAULT_PORT = 27950, MAX_LINE_BYTES = 4 * 1024 * 1024, PROTO_FPS = 30 };

	//---------------------------------------------------------------------------------------------
	// Socket server (AIHookServer.cpp). Thread-safe; the socket lives on its own thread.
	//---------------------------------------------------------------------------------------------
	struct Inbound
	{
		enum Kind { LINE, CONNECTED, DISCONNECTED };
		Kind kind;
		unsigned gen;			///< connection generation the item belongs to
		std::string line;
	};

	bool serverStart(int port, const std::string& build);	///< bind 127.0.0.1:port and start the thread
	void serverPoll(std::vector<Inbound>& out);						///< take everything received since the last poll
	void serverSend(unsigned gen, const std::string& line);	///< queue one line (newline added) for client 'gen'
	void serverPublishStatus(bool inGame, unsigned frame);	///< values the socket thread puts in "hello"

	//---------------------------------------------------------------------------------------------
	// Errors: commands throw CmdError; the dispatcher turns it into ok:false.
	//---------------------------------------------------------------------------------------------
	struct CmdError
	{
		std::string code;
		std::string message;
		CmdError(const char* c, const std::string& m) : code(c), message(m) {}
	};

	std::string strf(const char* format, ...);

	//---------------------------------------------------------------------------------------------
	// Common helpers (AIHook.cpp)
	//---------------------------------------------------------------------------------------------
	bool inMatch();											///< a match is running (not shell, not loading)
	unsigned protoFrame();							///< logic frame in protocol units (30/s)
	double logicFps();									///< logic frames per second of this build
	Player* localPlayer();
	void emitEvent(const char* name, const Json& data);
	double r1(double v);								///< round to 0.1
	std::string mapName();

	const char* objectKind(const Object* obj);			///< "unit" | "structure" | "other"
	bool isReportable(const Object* obj);						///< filters out projectiles, trees, props, ...
	bool isVisibleToLocal(const Object* obj);				///< fog + stealth rule of protocol §5 (non-own objects)
	bool isEnemyOfLocal(const Object* obj);
	const ThingTemplate* apparentTemplate(const Object* obj);	///< honours disguise for non-own objects
	int apparentOwner(const Object* obj);								///< player index, -1 for the neutral player

	//---------------------------------------------------------------------------------------------
	// Argument helpers (AIHookCommands.cpp). All throw CmdError(BAD_ARGS, ...).
	//---------------------------------------------------------------------------------------------
	bool hasArg(const Json& args, const char* key);
	long long argInt(const Json& args, const char* key);
	long long argIntOr(const Json& args, const char* key, long long def);
	double argReal(const Json& args, const char* key);
	double argRealOr(const Json& args, const char* key, double def);
	std::string argStr(const Json& args, const char* key);
	unsigned argObjectID(const Json& v, const char* what);
	Object* requireOwnObject(const Json& idValue, const char* what);	///< UNIT_DEAD / NOT_OWNED
	void requireOnMap(double x, double y);															///< CANNOT_DO when off the map

	//---------------------------------------------------------------------------------------------
	// Commands (AIHookCommands.cpp)
	//---------------------------------------------------------------------------------------------
	bool isKnownCommand(const std::string& cmd);
	Json runCommand(const std::string& cmd, const Json& args);	///< throws CmdError
	void commandsEndOfBatch();				///< restores the human's UI selection if orders changed it

	//---------------------------------------------------------------------------------------------
	// State (AIHookState.cpp)
	//---------------------------------------------------------------------------------------------
	Json cmdGetState(const Json& args);
	Json cmdGetMap(const Json& args);
	Json cmdQueryPath(const Json& args);
	Json cmdGetUnitData(const Json& args);
	void stateOnMatchStart();		///< snapshot static map knowledge (terrain, start spots, map-placed neutrals)
	void stateOnMatchEnd();

	Json playerJson();					///< the "player" block of get_state
	std::vector<std::string> unitPrereqNames(const ThingTemplate* tt);
	std::vector<std::string> missingPrereqNames(const ThingTemplate* tt, Player* player);

	//---------------------------------------------------------------------------------------------
	// Events (AIHookEvents.cpp)
	//---------------------------------------------------------------------------------------------
	void eventsReset();					///< rebuild baselines silently (new client / new match)
	void eventsUpdate();				///< once per protocol frame while a client is connected in a match
	bool eventsGameEndSent();
	void eventsSendGameEnd(const char* result);
}
