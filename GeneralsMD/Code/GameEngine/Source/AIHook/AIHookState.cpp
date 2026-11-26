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

// AIHookState.cpp ////////////////////////////////////////////////////////////////////////////////
// Read-only state queries: get_state (§7), get_map (§8), query_path, get_unit_data (§9).
//
// Rules: only what the local player can see (§5); never consume the logic RNG; never call
// anything that changes logic state. That rules out Locomotor construction (RNG) and
// WeaponTemplate::getDelayBetweenShots (RNG), and the logic pathfinder's findPath (shares the
// per-frame path budget); query_path runs its own A* over the pathfind grid instead.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>

#include "AIHook/AIHookInternal.h"

#include "Common/Energy.h"
#include "Common/KindOf.h"
#include "Common/Money.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/ProductionPrerequisite.h"
#include "Common/Science.h"
#include "Common/SpecialPower.h"
#include "Common/ThingFactory.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameClient/ControlBar.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIPathfind.h"
#include "GameLogic/ArmorSet.h"
#include "GameLogic/Armor.h"
#include "GameLogic/Damage.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/LocomotorSet.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/TerrainLogic.h"
#include "GameLogic/Weapon.h"
#include "GameLogic/WeaponSet.h"
#include "GameLogic/Module/ActiveBody.h"
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BehaviorModule.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/DozerAIUpdate.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"

namespace AIHook
{

namespace
{
	const char* templateKind(const ThingTemplate* tt)
	{
		if (tt->isKindOf(KINDOF_SUPPLY_SOURCE) && !tt->isKindOf(KINDOF_CASH_GENERATOR))
			return "other";
		if (tt->isKindOf(KINDOF_STRUCTURE))
			return "structure";
		if (tt->isKindOf(KINDOF_INFANTRY) || tt->isKindOf(KINDOF_VEHICLE) || tt->isKindOf(KINDOF_AIRCRAFT) ||
				tt->isKindOf(KINDOF_HUGE_VEHICLE) || tt->isKindOf(KINDOF_BOAT) || tt->isKindOf(KINDOF_DOZER) ||
				tt->isKindOf(KINDOF_HARVESTER))
			return "unit";
		return "other";
	}

	//---------------------------------------------------------------------------------------------
	// get_state pieces
	//---------------------------------------------------------------------------------------------
	const char* orderOf(const Object* obj)
	{
		const AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			return "idle";
		const DozerAIInterface* dozer = ai->getDozerAIInterface();
		if (dozer)
		{
			DozerTask task = dozer->getCurrentTask();
			if (task == DOZER_TASK_BUILD)
				return "build";
			if (task == DOZER_TASK_REPAIR)
				return "repair";
		}
		switch (ai->getAIStateType())
		{
			case AI_IDLE:
			case AI_WAIT:
			case AI_BUSY:
			case AI_FACE_OBJECT:
			case AI_FACE_POSITION:
				return "idle";
			case AI_MOVE_TO:
			case AI_FOLLOW_WAYPOINT_PATH_AS_TEAM:
			case AI_FOLLOW_WAYPOINT_PATH_AS_INDIVIDUALS:
			case AI_FOLLOW_WAYPOINT_PATH_AS_TEAM_EXACT:
			case AI_FOLLOW_WAYPOINT_PATH_AS_INDIVIDUALS_EXACT:
			case AI_FOLLOW_PATH:
			case AI_FOLLOW_EXITPRODUCTION_PATH:
			case AI_MOVE_OUT_OF_THE_WAY:
			case AI_MOVE_AND_TIGHTEN:
			case AI_MOVE_AND_EVACUATE:
			case AI_MOVE_AND_EVACUATE_AND_EXIT:
			case AI_MOVE_AND_DELETE:
			case AI_MOVE_AWAY_FROM_REPULSORS:
				return "move";
			case AI_ATTACK_POSITION:
			case AI_ATTACK_OBJECT:
			case AI_FORCE_ATTACK_OBJECT:
			case AI_ATTACK_AND_FOLLOW_OBJECT:
			case AI_ATTACK_SQUAD:
			case AI_ATTACK_AREA:
				return "attack";
			case AI_ATTACK_MOVE_TO:
			case AI_ATTACKFOLLOW_WAYPOINT_PATH_AS_INDIVIDUALS:
			case AI_ATTACKFOLLOW_WAYPOINT_PATH_AS_TEAM:
			case AI_HUNT:
				return "attack_move";
			case AI_GUARD:
			case AI_GUARD_RETALIATE:
			case AI_GUARD_TUNNEL_NETWORK:
				return "guard";
			case AI_DOCK:
				return obj->isKindOf(KINDOF_HARVESTER) ? "gather" : "other";
			default:
				return "other";
		}
	}

	Json targetOf(Object* obj)
	{
		AIUpdateInterface* ai = obj->getAIUpdateInterface();
		if (ai == nullptr)
			return Json();
		Object* t = ai->getCurrentVictim();
		if (t == nullptr)
			t = ai->getGoalObject();
		if (t == nullptr || t == obj || !isVisibleToLocal(t))
			return Json();
		return Json((unsigned)t->getID());
	}

	Json statusOf(const Object* obj)
	{
		Json st = Json::array();
		if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION))
			st.push_back("under_construction");
		if (obj->testStatus(OBJECT_STATUS_SOLD))
			st.push_back("sold");
		if (obj->isContained())
		{
			const Object* c = obj->getContainedBy();
			if (c && c->isKindOf(KINDOF_STRUCTURE))
				st.push_back("garrisoned");
			else
				st.push_back("contained");
		}
		if (obj->testStatus(OBJECT_STATUS_STEALTHED))
			st.push_back("stealthed");
		if (obj->isDisabled())
			st.push_back("disabled");
		return st;
	}

	Json queueOf(Object* obj)
	{
		Json q = Json::array();
		ProductionUpdateInterface* pu = obj->getProductionUpdateInterface();
		if (pu == nullptr)
			return q;
		for (const ProductionEntry* e = pu->firstProduction(); e; e = pu->nextProduction(e))
		{
			Json item;
			if (e->getProductionType() == PRODUCTION_UNIT && e->getProductionObject())
			{
				item["type"] = e->getProductionObject()->getName().str();
				item["kind"] = "unit";
			}
			else if (e->getProductionType() == PRODUCTION_UPGRADE && e->getProductionUpgrade())
			{
				item["type"] = e->getProductionUpgrade()->getUpgradeName().str();
				item["kind"] = "upgrade";
			}
			else
				continue;
			double p = e->getPercentComplete() / 100.0;
			item["progress"] = floor((p < 0 ? 0 : (p > 1 ? 1 : p)) * 1000.0 + 0.5) / 1000.0;
			q.push_back(item);
		}
		return q;
	}

	void healthOf(const Object* obj, Json& d)
	{
		const BodyModuleInterface* body = obj->getBodyModule();
		d["health"] = body ? r1(body->getHealth()) : 0.0;
		d["max_health"] = body ? r1(body->getMaxHealth()) : 0.0;
	}

	Json ownObjectJson(Object* obj)
	{
		Json d;
		d["id"] = (unsigned)obj->getID();
		d["type"] = obj->getTemplate()->getName().str();
		d["kind"] = objectKind(obj);
		d["x"] = r1(obj->getPosition()->x);
		d["y"] = r1(obj->getPosition()->y);
		healthOf(obj, d);
		d["veterancy"] = (int)obj->getVeterancyLevel();
		d["order"] = orderOf(obj);
		d["target_id"] = targetOf(obj);
		d["status"] = statusOf(obj);
		if (obj->isKindOf(KINDOF_STRUCTURE))
			d["queue"] = queueOf(obj);
		return d;
	}

	/// Public view of a non-own object: no orders, queues, upgrades or money (§5).
	Json otherObjectJson(const Object* obj)
	{
		const ThingTemplate* tt = apparentTemplate(obj);
		Json d;
		d["id"] = (unsigned)obj->getID();
		d["type"] = tt->getName().str();
		d["kind"] = templateKind(tt);
		int owner = apparentOwner(obj);
		if (owner < 0)
			d["owner"] = nullptr;
		else
			d["owner"] = owner;
		d["x"] = r1(obj->getPosition()->x);
		d["y"] = r1(obj->getPosition()->y);
		healthOf(obj, d);
		d["veterancy"] = (int)obj->getVeterancyLevel();
		return d;
	}

}

//-------------------------------------------------------------------------------------------------
// Prerequisite names (shared with the commands)
//-------------------------------------------------------------------------------------------------
std::vector<std::string> unitPrereqNames(const ThingTemplate* tt)
{
	std::vector<std::string> out;
	for (Int i = 0; i < tt->getPrereqCount(); ++i)
	{
		const ThingTemplate* alts[16];
		Int n = tt->getNthPrereq(i)->getAllPossibleBuildFacilityTemplates(alts, 16);
		std::string s;
		for (Int k = 0; k < n; ++k)
		{
			if (alts[k] == nullptr)
				continue;
			if (!s.empty())
				s += "|";
			s += alts[k]->getName().str();
		}
		if (!s.empty())
			out.push_back(s);
	}
	return out;
}

std::vector<std::string> missingPrereqNames(const ThingTemplate* tt, Player* player)
{
	std::vector<std::string> out;
	for (Int i = 0; i < tt->getPrereqCount(); ++i)
	{
		const ProductionPrerequisite* pre = tt->getNthPrereq(i);
		if (pre->isSatisfied(player))
			continue;
		const ThingTemplate* alts[16];
		Int n = pre->getAllPossibleBuildFacilityTemplates(alts, 16);
		std::string s;
		for (Int k = 0; k < n; ++k)
		{
			if (alts[k] == nullptr)
				continue;
			if (!s.empty())
				s += "|";
			s += alts[k]->getName().str();
		}
		if (s.empty())
		{
			// science prerequisite: only the localized text is available
			AsciiString a;
			a.translate(pre->getRequiresList(player));
			s = a.str();
		}
		if (!s.empty())
			out.push_back(s);
	}
	return out;
}

//-------------------------------------------------------------------------------------------------
Json playerJson()
{
	Player* me = localPlayer();
	Json p;
	p["index"] = (int)me->getPlayerIndex();
	AsciiString name;
	name.translate(me->getPlayerDisplayName());
	p["name"] = name.str();
	p["side"] = me->getSide().str();
	p["base_side"] = me->getBaseSide().str();
	p["money"] = me->getMoney()->countMoney();
	Json power;
	power["produced"] = me->getEnergy()->getProduction();
	power["consumed"] = me->getEnergy()->getConsumption();
	p["power"] = power;
	p["rank"] = me->getRankLevel();
	p["science_points"] = me->getSciencePurchasePoints();

	Json sciences = Json::array();
	std::vector<AsciiString> names = TheScienceStore->friend_getScienceNames();
	for (size_t i = 0; i < names.size(); ++i)
	{
		ScienceType st = TheScienceStore->getScienceFromInternalName(names[i]);	// existing key
		if (st != SCIENCE_INVALID && me->hasScience(st))
			sciences.push_back(names[i].str());
	}
	p["sciences"] = sciences;

	// special powers the player can use, by name, with the soonest-ready source
	struct PowerInfo { UnsignedInt readyFrame; unsigned source; };
	std::map<std::string, PowerInfo> powers;
	const UnsignedInt now = TheGameLogic->getFrame();
	for (Object* obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (obj->getControllingPlayer() != me || !obj->hasAnySpecialPower())
			continue;
		if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) || obj->testStatus(OBJECT_STATUS_SOLD) || obj->isEffectivelyDead())
			continue;
		for (BehaviorModule** m = obj->getBehaviorModules(); m && *m; ++m)
		{
			SpecialPowerModuleInterface* spm = (*m)->getSpecialPower();
			if (spm == nullptr || spm->isScriptOnly() || spm->getSpecialPowerTemplate() == nullptr)
				continue;
			ScienceType req = spm->getRequiredScience();
			if (req != SCIENCE_INVALID && !me->hasScience(req))
				continue;
			UnsignedInt rf = obj->isDisabled() ? 0xfffffff0u : spm->getReadyFrame();
			std::string pname = spm->getSpecialPowerTemplate()->getName().str();
			std::map<std::string, PowerInfo>::iterator it = powers.find(pname);
			if (it == powers.end() || rf < it->second.readyFrame)
			{
				PowerInfo info = { rf, (unsigned)obj->getID() };
				powers[pname] = info;
			}
		}
	}
	Json sp = Json::array();
	for (std::map<std::string, PowerInfo>::const_iterator it = powers.begin(); it != powers.end(); ++it)
	{
		Json e;
		e["name"] = it->first;
		UnsignedInt waitLogic = it->second.readyFrame > now ? it->second.readyFrame - now : 0;
		e["ready_in_frames"] = (unsigned)(((unsigned long long)waitLogic * PROTO_FPS) / LOGICFRAMES_PER_SECOND);
		e["source_id"] = it->second.source;
		sp.push_back(e);
	}
	p["special_powers"] = sp;
	return p;
}

//-------------------------------------------------------------------------------------------------
Json cmdGetState(const Json& args)
{
	bool incPlayer = true, incOwn = true, incEnemies = true, incNeutral = true;
	if (hasArg(args, "include"))
	{
		if (!args["include"].is_array())
			throw CmdError("BAD_ARGS", "include must be a list of strings");
		incPlayer = incOwn = incEnemies = incNeutral = false;
		const Json& inc = args["include"];
		for (size_t i = 0; i < inc.size(); ++i)
		{
			if (!inc[i].is_string())
				throw CmdError("BAD_ARGS", "include must be a list of strings");
			std::string s = inc[i].get<std::string>();
			if (s == "player") incPlayer = true;
			else if (s == "own") incOwn = true;
			else if (s == "enemies") incEnemies = true;
			else if (s == "neutral") incNeutral = true;
			else throw CmdError("BAD_ARGS", strf("unknown include '%s'", s.c_str()));
		}
	}

	Player* me = localPlayer();
	Json out;
	out["frame"] = protoFrame();
	if (incPlayer)
		out["player"] = playerJson();

	Json own = Json::array(), enemies = Json::array(), neutral = Json::array();
	if (incOwn || incEnemies || incNeutral)
	{
		for (Object* obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		{
			if (!isReportable(obj))
				continue;
			if (obj->getControllingPlayer() == me)
			{
				if (incOwn)
					own.push_back(ownObjectJson(obj));
				continue;
			}
			if (!(incEnemies || incNeutral) || !isVisibleToLocal(obj))
				continue;
			if (isEnemyOfLocal(obj))
			{
				if (incEnemies)
					enemies.push_back(otherObjectJson(obj));
			}
			else if (incNeutral)
				neutral.push_back(otherObjectJson(obj));
		}
	}
	if (incOwn)
		out["own"] = own;
	if (incEnemies)
		out["enemies"] = enemies;
	if (incNeutral)
		out["neutral"] = neutral;
	return out;
}

//-------------------------------------------------------------------------------------------------
void stateOnMatchStart() {}
void stateOnMatchEnd() {}

Json cmdGetMap(const Json&) { throw CmdError("CANNOT_DO", "not implemented yet"); }
Json cmdQueryPath(const Json&) { throw CmdError("CANNOT_DO", "not implemented yet"); }
Json cmdGetUnitData(const Json&) { throw CmdError("CANNOT_DO", "not implemented yet"); }

}	// namespace AIHook
