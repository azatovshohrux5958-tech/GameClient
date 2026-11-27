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

// AIHookReplay.cpp ///////////////////////////////////////////////////////////////////////////////
// Replay playback support (protocol v1 additive, "replay" section; proposal P4):
// - the hook serves state from a chosen player's point of view (POV) while a replay plays back;
// - an order_issued event per replayed command, with the issuing player's selection resolved;
// - replay_info / set_pov / replay_control commands (fast-forward and hold-at-frame pacing).
//
// Replayed commands go straight from TheRecorder into TheCommandList inside GameLogic::update,
// so they never pass through TheMessageStream. Instead of touching the engine, the hook reads
// the replay file itself (read-only, its own handle) and emits each command just before the logic
// executes it: when the hook runs at logic frame F, the next logic update executes the commands
// recorded for frame F. Selections are mirrored per player from the selection/team messages in
// the same order the logic processes them.
//
// Nothing here changes game-logic state: no AIGroups, no name keys, no RNG. Fast-forward uses the
// replay's own client-side fast mode (TheGlobalData->m_TiVOFastMode), and holds just block the
// main thread inside AIHook::update while the client reads state.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>

#include "AIHook/AIHookInternal.h"

#include "Common/GlobalData.h"
#include "Common/MessageStream.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/Recorder.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/VictoryConditions.h"

namespace AIHook
{

namespace
{
	enum { NUM_TEAMS = 10, MAX_REPLAY_BYTES = 64 * 1024 * 1024 };

	struct Arg
	{
		int type;					///< GameMessageArgumentDataType
		long long i;			///< INTEGER / BOOLEAN / OBJECTID / DRAWABLEID / TEAMID / TIMESTAMP / WIDECHAR
		double f[3];			///< REAL (f[0]) / LOCATION (x, y, z)
		int px[4];				///< PIXEL (2) / PIXELREGION (4)
	};

	struct Record
	{
		unsigned logicFrame;
		int type;					///< GameMessage::Type
		int player;
		std::vector<Arg> args;
	};

	struct Mirror
	{
		std::vector<ObjectID> selection;
		std::vector<ObjectID> teams[NUM_TEAMS];
	};

	bool s_active = false;					///< a replay is playing back and the hook tracks it
	std::string s_file;
	std::vector<Record> s_records;
	size_t s_next = 0;
	std::string s_parseError;
	std::map<int, Mirror> s_mirror;	///< per player index
	Player* s_pov = nullptr;				///< chosen point of view
	Player* s_povTemp = nullptr;		///< get_state {"pov"} override for one call
	long long s_holdAt = -1;				///< protocol frame to hold at, -1 = none
	bool s_fast = false;
	unsigned s_ordersEmitted = 0;

	//---------------------------------------------------------------------------------------------
	// Replay file reader. Mirrors RecorderClass::readReplayHeader / playbackFile / appendNextCommand.
	//---------------------------------------------------------------------------------------------
	struct Cursor
	{
		const std::vector<unsigned char>& b;
		size_t p;
		bool ok;
		Cursor(const std::vector<unsigned char>& buf) : b(buf), p(0), ok(true) {}
		bool take(void* out, size_t n)
		{
			if (!ok || p + n > b.size()) { ok = false; return false; }
			memcpy(out, &b[p], n);
			p += n;
			return true;
		}
		void skip(size_t n) { if (!ok || p + n > b.size()) ok = false; else p += n; }
		void skipWide() { WideChar c = 1; while (ok && c != 0) take(&c, sizeof(c)); }
		void skipAscii() { char c = 1; while (ok && c != 0) take(&c, 1); }
	};

	bool loadFile(const std::string& path, std::vector<unsigned char>& out)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (f == nullptr)
			return false;
		unsigned char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		{
			out.insert(out.end(), buf, buf + n);
			if (out.size() > MAX_REPLAY_BYTES)
				break;
		}
		fclose(f);
		return true;
	}

	bool parseReplay(const std::vector<unsigned char>& bytes, std::vector<Record>& out, std::string& err)
	{
		Cursor c(bytes);
		char magic[6];
		if (!c.take(magic, 6) || memcmp(magic, "GENREP", 6) != 0)
		{
			err = "not a replay file (no GENREP)";
			return false;
		}
		c.skip(2 * sizeof(int32_t));			// start / end time (replay_time_t)
		c.skip(sizeof(UnsignedInt));			// frame count
		c.skip(2 * sizeof(Bool));					// desync, quit early
		c.skip(MAX_SLOTS * sizeof(Bool));	// player disconnects
		c.skipWide();											// replay name
		c.skip(sizeof(SYSTEMTIME));
		c.skipWide();											// version string
		c.skipWide();											// version time string
		c.skip(3 * sizeof(UnsignedInt));	// version number, exe CRC, ini CRC
		c.skipAscii();										// game options
		c.skipAscii();										// local player index
		c.skip(4 * sizeof(Int));					// difficulty, original game mode, rank points, max FPS
		if (!c.ok)
		{
			err = "truncated replay header";
			return false;
		}

		while (c.ok && c.p < bytes.size())
		{
			Int frame = 0, type = 0, player = -1;
			if (!c.take(&frame, sizeof(frame)) || !c.take(&type, sizeof(type)) || !c.take(&player, sizeof(player)))
				break;
			UnsignedByte numTypes = 0;
			if (!c.take(&numTypes, 1))
				break;
			std::vector<std::pair<UnsignedByte, UnsignedByte> > layout;
			for (UnsignedByte i = 0; i < numTypes && c.ok; ++i)
			{
				UnsignedByte t = 0, n = 0;
				c.take(&t, 1);
				c.take(&n, 1);
				layout.push_back(std::make_pair(t, n));
			}
			Record r;
			r.logicFrame = (unsigned)frame;
			r.type = type;
			r.player = player;
			for (size_t li = 0; li < layout.size() && c.ok; ++li)
			{
				for (UnsignedByte k = 0; k < layout[li].second && c.ok; ++k)
				{
					Arg a;
					memset(&a, 0, sizeof(a));
					a.type = layout[li].first;
					switch (a.type)
					{
						case ARGUMENTDATATYPE_INTEGER: { Int v = 0; c.take(&v, sizeof(v)); a.i = v; break; }
						case ARGUMENTDATATYPE_REAL: { Real v = 0; c.take(&v, sizeof(v)); a.f[0] = v; break; }
						case ARGUMENTDATATYPE_BOOLEAN: { Bool v = 0; c.take(&v, sizeof(v)); a.i = v ? 1 : 0; break; }
						case ARGUMENTDATATYPE_OBJECTID: { ObjectID v = INVALID_ID; c.take(&v, sizeof(v)); a.i = (long long)v; break; }
						case ARGUMENTDATATYPE_DRAWABLEID: { DrawableID v = INVALID_DRAWABLE_ID; c.take(&v, sizeof(v)); a.i = (long long)v; break; }
						case ARGUMENTDATATYPE_TEAMID: { UnsignedInt v = 0; c.take(&v, sizeof(v)); a.i = v; break; }
						case ARGUMENTDATATYPE_LOCATION: { Coord3D v; c.take(&v, sizeof(v)); a.f[0] = v.x; a.f[1] = v.y; a.f[2] = v.z; break; }
						case ARGUMENTDATATYPE_PIXEL: { ICoord2D v; c.take(&v, sizeof(v)); a.px[0] = v.x; a.px[1] = v.y; break; }
						case ARGUMENTDATATYPE_PIXELREGION: { IRegion2D v; c.take(&v, sizeof(v)); a.px[0] = v.lo.x; a.px[1] = v.lo.y; a.px[2] = v.hi.x; a.px[3] = v.hi.y; break; }
						case ARGUMENTDATATYPE_TIMESTAMP: { UnsignedInt v = 0; c.take(&v, sizeof(v)); a.i = v; break; }
						case ARGUMENTDATATYPE_WIDECHAR: { WideChar v = 0; c.take(&v, sizeof(v)); a.i = v; break; }
						default:
							err = strf("unknown argument type %d at byte %u", a.type, (unsigned)c.p);
							return !out.empty();
					}
					r.args.push_back(a);
				}
			}
			if (!c.ok)
				break;
			out.push_back(r);
		}
		return true;
	}

	//---------------------------------------------------------------------------------------------
	bool isGamePlayer(const Player* p)
	{
		if (p == nullptr || (ThePlayerList && p == ThePlayerList->getNeutralPlayer()))
			return false;
		const PlayerTemplate* pt = p->getPlayerTemplate();
		if (pt == nullptr || pt->getName().isEmpty())
			return false;
		return strcmp(pt->getName().str(), "FactionObserver") != 0 && strcmp(pt->getName().str(), "FactionCivilian") != 0;
	}

	Player* playerByIndex(long long idx)
	{
		if (ThePlayerList == nullptr || idx < 0 || idx >= ThePlayerList->getPlayerCount())
			return nullptr;
		return ThePlayerList->getNthPlayer((Int)idx);
	}

	Player* defaultPov()
	{
		if (ThePlayerList == nullptr)
			return nullptr;
		for (Int i = 0; i < ThePlayerList->getPlayerCount(); ++i)
		{
			Player* p = ThePlayerList->getNthPlayer(i);
			if (isGamePlayer(p))
				return p;
		}
		return nullptr;
	}

	unsigned toProto(unsigned logicFrame)
	{
		return (unsigned)(((unsigned long long)logicFrame * PROTO_FPS) / LOGICFRAMES_PER_SECOND);
	}

	std::string displayName(Player* p)
	{
		AsciiString name;
		name.translate(p->getPlayerDisplayName());
		return name.str();
	}

	//---------------------------------------------------------------------------------------------
	// Argument accessors (null/false when the record doesn't have that argument type there)
	//---------------------------------------------------------------------------------------------
	const Arg* argAt(const Record& r, size_t i, int type)
	{
		if (i >= r.args.size() || r.args[i].type != type)
			return nullptr;
		return &r.args[i];
	}

	void putLocation(Json& d, const Record& r, size_t i, const char* xk = "x", const char* yk = "y")
	{
		const Arg* a = argAt(r, i, ARGUMENTDATATYPE_LOCATION);
		if (a)
		{
			d[xk] = r1(a->f[0]);
			d[yk] = r1(a->f[1]);
		}
	}

	void putObject(Json& d, const Record& r, size_t i, const char* key)
	{
		const Arg* a = argAt(r, i, ARGUMENTDATATYPE_OBJECTID);
		if (a == nullptr || a->i == (long long)INVALID_ID)
			return;
		d[key] = (unsigned)a->i;
		Object* obj = TheGameLogic->findObjectByID((ObjectID)a->i);
		std::string tk = std::string(key);
		tk = tk.substr(0, tk.size() - 2) + "type";	// target_id -> target_type
		if (obj)
		{
			d[tk] = obj->getTemplate()->getName().str();
			d[tk.substr(0, tk.size() - 4) + "owner"] = apparentOwnerRaw(obj);
		}
	}

	void putTemplate(Json& d, const Record& r, size_t i)
	{
		const Arg* a = argAt(r, i, ARGUMENTDATATYPE_INTEGER);
		if (a == nullptr || TheThingFactory == nullptr)
			return;
		const ThingTemplate* tt = TheThingFactory->findByTemplateID((UnsignedShort)a->i);
		if (tt)
			d["template"] = tt->getName().str();
	}

	void putUpgrade(Json& d, const Record& r, size_t i)
	{
		const Arg* a = argAt(r, i, ARGUMENTDATATYPE_INTEGER);
		if (a == nullptr || TheUpgradeCenter == nullptr)
			return;
		const UpgradeTemplate* ut = TheUpgradeCenter->findUpgradeByKey((NameKeyType)a->i);
		if (ut)
			d["upgrade"] = ut->getUpgradeName().str();
	}

	void putPower(Json& d, const Record& r, size_t i)
	{
		const Arg* a = argAt(r, i, ARGUMENTDATATYPE_INTEGER);
		if (a == nullptr || TheSpecialPowerStore == nullptr)
			return;
		const SpecialPowerTemplate* sp = TheSpecialPowerStore->findSpecialPowerTemplateByID((UnsignedInt)a->i);
		if (sp)
			d["power"] = sp->getName().str();
	}

	Json rawArgs(const Record& r)
	{
		Json a = Json::array();
		for (size_t i = 0; i < r.args.size(); ++i)
		{
			const Arg& x = r.args[i];
			switch (x.type)
			{
				case ARGUMENTDATATYPE_REAL: a.push_back(r1(x.f[0])); break;
				case ARGUMENTDATATYPE_LOCATION: { Json l = Json::array(); l.push_back(r1(x.f[0])); l.push_back(r1(x.f[1])); a.push_back(l); break; }
				case ARGUMENTDATATYPE_PIXEL: { Json l = Json::array(); l.push_back(x.px[0]); l.push_back(x.px[1]); a.push_back(l); break; }
				case ARGUMENTDATATYPE_PIXELREGION: { Json l = Json::array(); for (int k = 0; k < 4; ++k) l.push_back(x.px[k]); a.push_back(l); break; }
				case ARGUMENTDATATYPE_BOOLEAN: a.push_back(x.i != 0); break;
				default: a.push_back(x.i); break;
			}
		}
		return a;
	}

	//---------------------------------------------------------------------------------------------
	// Selection mirror (GameLogic::onCreateSelectedGroup etc., Player::process*TeamGameMessage)
	//---------------------------------------------------------------------------------------------
	Object* liveObject(ObjectID id)
	{
		Object* o = TheGameLogic->findObjectByID(id);
		return (o && !o->isDestroyed() && !o->isEffectivelyDead()) ? o : nullptr;
	}

	void addUnique(std::vector<ObjectID>& v, ObjectID id)
	{
		if (std::find(v.begin(), v.end(), id) == v.end())
			v.push_back(id);
	}

	/// Returns true when the record is a selection/team message (consumed by the mirror, not emitted).
	bool mirrorSelection(const Record& r)
	{
		Mirror& m = s_mirror[r.player];
		const int t = r.type;
		if (t == GameMessage::MSG_CREATE_SELECTED_GROUP || t == GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND)
		{
			const bool newGroup = !r.args.empty() && r.args[0].type == ARGUMENTDATATYPE_BOOLEAN && r.args[0].i != 0;
			bool first = true;
			for (size_t i = 1; i < r.args.size(); ++i)
			{
				if (r.args[i].type != ARGUMENTDATATYPE_OBJECTID || liveObject((ObjectID)r.args[i].i) == nullptr)
					continue;
				if (newGroup && first)
					m.selection.clear();
				first = false;
				addUnique(m.selection, (ObjectID)r.args[i].i);
			}
			return true;
		}
		if (t == GameMessage::MSG_REMOVE_FROM_SELECTED_GROUP)
		{
			for (size_t i = 0; i < r.args.size(); ++i)
				m.selection.erase(std::remove(m.selection.begin(), m.selection.end(), (ObjectID)r.args[i].i), m.selection.end());
			return true;
		}
		if (t == GameMessage::MSG_DESTROY_SELECTED_GROUP)
		{
			m.selection.clear();
			return true;
		}
		if (t >= GameMessage::MSG_CREATE_TEAM0 && t <= GameMessage::MSG_CREATE_TEAM9)
		{
			const int n = t - GameMessage::MSG_CREATE_TEAM0;
			m.teams[n].clear();
			for (size_t i = 0; i < r.args.size(); ++i)
			{
				const ObjectID id = (ObjectID)r.args[i].i;
				if (liveObject(id) == nullptr)
					continue;
				for (int k = 0; k < NUM_TEAMS; ++k)
					m.teams[k].erase(std::remove(m.teams[k].begin(), m.teams[k].end(), id), m.teams[k].end());
				m.teams[n].push_back(id);
			}
			return true;
		}
		if (t >= GameMessage::MSG_SELECT_TEAM0 && t <= GameMessage::MSG_SELECT_TEAM9)
		{
			m.selection.clear();
			const std::vector<ObjectID>& team = m.teams[t - GameMessage::MSG_SELECT_TEAM0];
			for (size_t i = 0; i < team.size(); ++i)
				if (liveObject(team[i]))
					addUnique(m.selection, team[i]);
			return true;
		}
		if (t >= GameMessage::MSG_ADD_TEAM0 && t <= GameMessage::MSG_ADD_TEAM9)
		{
			const std::vector<ObjectID>& team = m.teams[t - GameMessage::MSG_ADD_TEAM0];
			for (size_t i = 0; i < team.size(); ++i)
				if (liveObject(team[i]))
					addUnique(m.selection, team[i]);
			return true;
		}
		return false;
	}

	bool isSilent(int t)
	{
		return t == GameMessage::MSG_LOGIC_CRC || t == GameMessage::MSG_SET_REPLAY_CAMERA ||
			t == GameMessage::MSG_BEGIN_NETWORK_MESSAGES || t == GameMessage::MSG_PLACE_BEACON ||
			t == GameMessage::MSG_REMOVE_BEACON || t == GameMessage::MSG_SET_BEACON_TEXT ||
			t == GameMessage::MSG_FRAME_TICK || t == GameMessage::MSG_NEW_GAME;
	}

	/// Protocol command name for a message type, or nullptr.
	const char* protocolName(int t)
	{
		switch (t)
		{
			case GameMessage::MSG_DO_MOVETO: case GameMessage::MSG_DO_FORCEMOVETO: return "move";
			case GameMessage::MSG_DO_ATTACKMOVETO: return "attack_move";
			case GameMessage::MSG_DO_ATTACK_OBJECT: case GameMessage::MSG_DO_FORCE_ATTACK_OBJECT: return "attack";
			case GameMessage::MSG_DO_FORCE_ATTACK_GROUND: return "force_attack_ground";
			case GameMessage::MSG_DO_STOP: return "stop";
			case GameMessage::MSG_DO_GUARD_POSITION: case GameMessage::MSG_DO_GUARD_OBJECT: return "guard";
			case GameMessage::MSG_DOZER_CONSTRUCT: case GameMessage::MSG_DOZER_CONSTRUCT_LINE: return "build_structure";
			case GameMessage::MSG_QUEUE_UNIT_CREATE: return "train_unit";
			case GameMessage::MSG_CANCEL_UNIT_CREATE: return "cancel_train";
			case GameMessage::MSG_QUEUE_UPGRADE: return "research_upgrade";
			case GameMessage::MSG_PURCHASE_SCIENCE: return "purchase_science";
			case GameMessage::MSG_DO_SPECIAL_POWER: case GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION:
			case GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT: return "fire_special_power";
			case GameMessage::MSG_SELL: return "sell";
			case GameMessage::MSG_SET_RALLY_POINT: return "set_rally_point";
			case GameMessage::MSG_DOCK: return "gather";
			default: return nullptr;
		}
	}

	Json orderJson(const Record& r)
	{
		Json d;
		d["player"] = r.player;
		const char* raw = GameMessage::getCommandTypeAsString((GameMessage::Type)r.type);
		d["msg"] = raw ? raw : "";
		const char* cmd = protocolName(r.type);
		d["cmd"] = cmd ? Json(cmd) : Json();

		Json ids = Json::array(), types = Json::array();
		Player* p = playerByIndex(r.player);
		std::vector<ObjectID>& sel = s_mirror[r.player].selection;
		for (size_t i = 0; i < sel.size(); ++i)
		{
			Object* o = liveObject(sel[i]);
			if (o == nullptr || o->getControllingPlayer() != p)
				continue;
			ids.push_back((unsigned)o->getID());
			types.push_back(o->getTemplate()->getName().str());
		}
		d["unit_ids"] = ids;
		d["unit_types"] = types;

		switch (r.type)
		{
			case GameMessage::MSG_DO_MOVETO: case GameMessage::MSG_DO_FORCEMOVETO:
			case GameMessage::MSG_DO_ATTACKMOVETO: case GameMessage::MSG_DO_FORCE_ATTACK_GROUND:
			case GameMessage::MSG_COMBATDROP_AT_LOCATION: case GameMessage::MSG_DO_GUARD_POSITION:
			case GameMessage::MSG_ADD_WAYPOINT:
				putLocation(d, r, 0);
				break;
			case GameMessage::MSG_DO_ATTACK_OBJECT: case GameMessage::MSG_DO_FORCE_ATTACK_OBJECT:
			case GameMessage::MSG_COMBATDROP_AT_OBJECT: case GameMessage::MSG_DOCK:
			case GameMessage::MSG_DO_GUARD_OBJECT: case GameMessage::MSG_GET_REPAIRED:
			case GameMessage::MSG_GET_HEALED: case GameMessage::MSG_DO_REPAIR:
			case GameMessage::MSG_RESUME_CONSTRUCTION:
				putObject(d, r, 0, "target_id");
				break;
			case GameMessage::MSG_ENTER:
				putObject(d, r, 1, "target_id");
				break;
			case GameMessage::MSG_DO_WEAPON_AT_LOCATION:
				putLocation(d, r, 1);
				break;
			case GameMessage::MSG_DO_WEAPON_AT_OBJECT:
				putObject(d, r, 1, "target_id");
				break;
			case GameMessage::MSG_QUEUE_UNIT_CREATE:
				putTemplate(d, r, 0);
				break;
			case GameMessage::MSG_DOZER_CONSTRUCT: case GameMessage::MSG_DOZER_CONSTRUCT_LINE:
				putTemplate(d, r, 0);
				putLocation(d, r, 1);
				if (const Arg* a = argAt(r, 2, ARGUMENTDATATYPE_REAL))
					d["angle"] = floor(a->f[0] * 1000.0 + 0.5) / 1000.0;
				if (r.type == GameMessage::MSG_DOZER_CONSTRUCT_LINE)
					putLocation(d, r, 3, "x2", "y2");
				break;
			case GameMessage::MSG_QUEUE_UPGRADE:
				putObject(d, r, 0, "building_id");
				putUpgrade(d, r, 1);
				break;
			case GameMessage::MSG_CANCEL_UPGRADE:
				putUpgrade(d, r, 0);
				break;
			case GameMessage::MSG_PURCHASE_SCIENCE:
				if (const Arg* a = argAt(r, 0, ARGUMENTDATATYPE_INTEGER))
					if (TheScienceStore)
						d["science"] = TheScienceStore->getInternalNameForScience((ScienceType)a->i).str();
				break;
			case GameMessage::MSG_DO_SPECIAL_POWER:
				putPower(d, r, 0);
				putObject(d, r, 2, "source_id");
				break;
			case GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION:
			{
				putPower(d, r, 0);
				putLocation(d, r, 1);
				const size_t src = r.args.size() >= 6 ? 5 : 4;
				putObject(d, r, src, "source_id");
				break;
			}
			case GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT:
				putPower(d, r, 0);
				putObject(d, r, 1, "target_id");
				putObject(d, r, 3, "source_id");
				break;
			case GameMessage::MSG_SET_RALLY_POINT:
				putObject(d, r, 0, "building_id");
				putLocation(d, r, 1);
				break;
			default:
				break;
		}
		if (cmd == nullptr && !r.args.empty())
			d["args"] = rawArgs(r);
		return d;
	}
}

//-------------------------------------------------------------------------------------------------
int apparentOwnerRaw(const Object* obj)
{
	const Player* p = obj->getControllingPlayer();
	if (p == nullptr || (ThePlayerList && p == ThePlayerList->getNeutralPlayer()))
		return -1;
	return (int)p->getPlayerIndex();
}

bool replayActive()
{
	return s_active;
}

Player* replayPov()
{
	if (!s_active)
		return nullptr;
	return s_povTemp ? s_povTemp : s_pov;
}

void replayOnMatchStart()
{
	s_active = false;
	s_records.clear();
	s_mirror.clear();
	s_next = 0;
	s_parseError.clear();
	s_holdAt = -1;
	s_ordersEmitted = 0;
	s_povTemp = nullptr;
	s_pov = nullptr;
	if (TheRecorder == nullptr || !TheRecorder->isPlaybackMode() || TheGameLogic == nullptr || !TheGameLogic->isInReplayGame())
		return;
	s_active = true;
	s_file = TheRecorder->getCurrentReplayFilename().str();
	s_pov = defaultPov();

	std::string path = std::string(TheRecorder->getReplayDir().str()) + s_file;
	std::vector<unsigned char> bytes;
	if (!loadFile(path, bytes))
		s_parseError = strf("cannot open %s", path.c_str());
	else if (!parseReplay(bytes, s_records, s_parseError))
		s_records.clear();
	DEBUG_LOG(("AIHook: replay %s, %u commands%s%s", s_file.c_str(), (unsigned)s_records.size(),
		s_parseError.empty() ? "" : ", ", s_parseError.c_str()));
}

void replayOnMatchEnd()
{
	if (s_fast && TheWritableGlobalData)
		TheWritableGlobalData->m_TiVOFastMode = FALSE;
	s_fast = false;
	s_active = false;
	s_records.clear();
	s_mirror.clear();
	s_pov = s_povTemp = nullptr;
	s_holdAt = -1;
}

void replayUpdate(bool emit)
{
	if (!s_active || TheGameLogic == nullptr)
		return;
	const unsigned now = TheGameLogic->getFrame();
	while (s_next < s_records.size() && s_records[s_next].logicFrame <= now)
	{
		const Record& r = s_records[s_next++];
		if (mirrorSelection(r) || isSilent(r.type))
			continue;
		if (r.type < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || r.type > GameMessage::MSG_END_NETWORK_MESSAGES)
			continue;
		if (emit)
		{
			++s_ordersEmitted;
			emitEvent("order_issued", orderJson(r));
		}
	}
}

bool replayHolding()
{
	return s_active && s_holdAt >= 0 && (long long)protoFrame() >= s_holdAt;
}

void replayReleaseHold()
{
	s_holdAt = -1;
}

const char* replayResultFor(Player* p)
{
	if (p == nullptr || TheVictoryConditions == nullptr)
		return nullptr;
	if (TheVictoryConditions->hasAchievedVictory(p))
		return "win";
	if (TheVictoryConditions->hasBeenDefeated(p) || TheVictoryConditions->hasSinglePlayerBeenDefeated(p))
		return "loss";
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
// Commands
//-------------------------------------------------------------------------------------------------
static void requireReplay()
{
	if (!s_active)
		throw CmdError("CANNOT_DO", "no replay is playing back");
}

Json replayInfoJson()
{
	Json r;
	r["replay"] = s_active;
	if (!s_active)
		return r;
	r["file"] = s_file;
	r["map"] = mapName();
	r["frame"] = protoFrame();
	r["frame_count"] = toProto(TheRecorder ? TheRecorder->getPlaybackFrameCount() : 0);
	r["pov"] = s_pov ? Json((int)s_pov->getPlayerIndex()) : Json();
	r["fast"] = s_fast;
	r["hold_at"] = s_holdAt >= 0 ? Json(s_holdAt) : Json();
	r["commands"] = (unsigned)s_records.size();
	r["commands_done"] = (unsigned)s_next;
	r["orders_emitted"] = s_ordersEmitted;
	if (!s_parseError.empty())
		r["parse_error"] = s_parseError;

	Json players = Json::array();
	for (Int i = 0; ThePlayerList && i < ThePlayerList->getPlayerCount(); ++i)
	{
		Player* p = ThePlayerList->getNthPlayer(i);
		if (!isGamePlayer(p))
			continue;
		Json e;
		e["index"] = (int)p->getPlayerIndex();
		e["name"] = displayName(p);
		e["side"] = p->getSide().str();
		e["base_side"] = p->getBaseSide().str();
		e["human"] = p->getPlayerType() == PLAYER_HUMAN;
		e["active"] = (bool)p->isPlayerActive();
		Json allies = Json::array(), enemies = Json::array();
		for (Int k = 0; k < ThePlayerList->getPlayerCount(); ++k)
		{
			Player* o = ThePlayerList->getNthPlayer(k);
			if (o == p || !isGamePlayer(o) || o->getDefaultTeam() == nullptr)
				continue;
			Relationship rel = p->getRelationship(o->getDefaultTeam());
			if (rel == ALLIES) allies.push_back((int)o->getPlayerIndex());
			else if (rel == ENEMIES) enemies.push_back((int)o->getPlayerIndex());
		}
		e["allies"] = allies;
		e["enemies"] = enemies;
		const char* res = replayResultFor(p);
		e["result"] = res ? Json(res) : Json();
		players.push_back(e);
	}
	r["players"] = players;
	return r;
}

Json cmdReplayInfo(const Json&)
{
	return replayInfoJson();
}

/// Validates a player index argument for a POV; throws BAD_ARGS / CANNOT_DO.
Player* requirePovPlayer(const Json& v)
{
	if (!v.is_number_integer())
		throw CmdError("BAD_ARGS", "player must be a player index (integer)");
	Player* p = playerByIndex(v.get<long long>());
	if (!isGamePlayer(p))
		throw CmdError("CANNOT_DO", strf("player %lld is not a game player", v.get<long long>()));
	return p;
}

Json cmdSetPov(const Json& args)
{
	requireReplay();
	if (!hasArg(args, "player"))
		throw CmdError("BAD_ARGS", "player is required");
	Player* p = requirePovPlayer(args["player"]);
	if (p != s_pov)
	{
		s_pov = p;
		eventsReset();		// a new baseline for the new POV (silent)
	}
	Json r;
	r["pov"] = (int)p->getPlayerIndex();
	r["side"] = p->getSide().str();
	return r;
}

Json cmdReplayControl(const Json& args)
{
	requireReplay();
	if (hasArg(args, "fast"))
	{
		if (!args["fast"].is_boolean())
			throw CmdError("BAD_ARGS", "fast must be a boolean");
		s_fast = args["fast"].get<bool>();
		if (TheWritableGlobalData)
			TheWritableGlobalData->m_TiVOFastMode = s_fast ? TRUE : FALSE;
	}
	if (args.contains("hold_at"))
	{
		const Json& h = args["hold_at"];
		if (h.is_null())
			s_holdAt = -1;
		else if (h.is_number_integer() && h.get<long long>() >= 0)
			s_holdAt = h.get<long long>();
		else
			throw CmdError("BAD_ARGS", "hold_at must be a protocol frame (integer >= 0) or null");
	}
	Json r;
	r["fast"] = s_fast;
	r["hold_at"] = s_holdAt >= 0 ? Json(s_holdAt) : Json();
	r["frame"] = protoFrame();
	r["holding"] = replayHolding();
	return r;
}

Json cmdGetStateReplay(const Json& args)
{
	if (!args.contains("pov") || args["pov"].is_null())
		return cmdGetState(args);
	requireReplay();
	Player* p = requirePovPlayer(args["pov"]);
	s_povTemp = p;
	try
	{
		Json out = cmdGetState(args);
		s_povTemp = nullptr;
		return out;
	}
	catch (...)
	{
		s_povTemp = nullptr;
		throw;
	}
}

}	// namespace AIHook
