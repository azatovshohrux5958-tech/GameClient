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
// Event detection (protocol §10) by diffing a read-only snapshot once per protocol frame.
// The first scan after a reset (client connect / match start) only builds the baseline.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include <map>
#include <set>
#include <utility>

#include "AIHook/AIHookInternal.h"

#include "Common/Energy.h"
#include "Common/KindOf.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "GameLogic/Damage.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/ProductionUpdate.h"

namespace AIHook
{

namespace
{
	const unsigned UNDER_ATTACK_THROTTLE = 3 * PROTO_FPS;	///< max 1 under_attack per object per 3 s

	struct Track
	{
		std::string type;
		bool own;
		bool underConstruction;
		UnsignedInt lastDamage;		///< logic frame of the last damage taken (own only)
		unsigned lastAttackEvent;	///< protocol frame of the last under_attack sent
		bool attackEventSent;
	};

	std::map<ObjectID, Track> s_tracked;		///< own objects + non-own objects visible last scan
	std::set<ObjectID> s_spotted;					///< enemies ever seen (enemy_spotted fires once each)
	std::set<std::pair<ObjectID, std::string> > s_queuedUpgrades;
	std::map<std::string, bool> s_powerReady;
	bool s_baseline = true;
	bool s_lowPower = false;
	int s_sciencePoints = 0;
	bool s_gameEndSent = false;

	const UpgradeTemplate* upgradeByName(const std::string& name)
	{
		for (const UpgradeTemplate* ut = TheUpgradeCenter->firstUpgradeTemplate(); ut; ut = ut->friend_getNext())
			if (name == ut->getUpgradeName().str())
				return ut;
		return nullptr;
	}

	Json attackerOf(const BodyModuleInterface* body)
	{
		const DamageInfo* di = body->getLastDamageInfo();
		if (di == nullptr || di->in.m_sourceID == INVALID_ID)
			return Json();
		Object* src = TheGameLogic->findObjectByID(di->in.m_sourceID);
		if (src == nullptr || !isVisibleToLocal(src))
			return Json();	// don't reveal hidden attackers
		return Json((unsigned)src->getID());
	}
}

//-------------------------------------------------------------------------------------------------
void eventsReset()
{
	s_tracked.clear();
	s_spotted.clear();
	s_queuedUpgrades.clear();
	s_powerReady.clear();
	s_baseline = true;
	s_lowPower = false;
	s_sciencePoints = 0;
	s_gameEndSent = false;
}

bool eventsGameEndSent()
{
	return s_gameEndSent;
}

void eventsSendGameEnd(const char* result)
{
	if (s_gameEndSent)
		return;
	s_gameEndSent = true;
	Json d;
	d["result"] = result;
	emitEvent("game_end", d);
}

//-------------------------------------------------------------------------------------------------
void eventsUpdate()
{
	Player* me = localPlayer();
	if (me == nullptr)
		return;
	const bool silent = s_baseline;
	s_baseline = false;
	const unsigned now = protoFrame();

	std::map<ObjectID, Track> next;
	std::set<std::pair<ObjectID, std::string> > queued;
	std::vector<const Object*> newlySpotted;

	for (Object* obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		if (!isReportable(obj))
			continue;
		const ObjectID id = obj->getID();
		std::map<ObjectID, Track>::const_iterator prev = s_tracked.find(id);

		if (obj->getControllingPlayer() == me)
		{
			const BodyModuleInterface* body = obj->getBodyModule();
			Track t;
			t.type = obj->getTemplate()->getName().str();
			t.own = true;
			t.underConstruction = obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) != 0;
			t.lastDamage = body ? body->getLastDamageTimestamp() : 0;
			t.lastAttackEvent = 0;
			t.attackEventSent = false;

			if (prev == s_tracked.end() || !prev->second.own)
			{
				if (!silent)
				{
					Json d;
					d["id"] = (unsigned)id;
					d["type"] = t.type;
					d["kind"] = objectKind(obj);
					emitEvent("object_created", d);
				}
			}
			else
			{
				t.lastAttackEvent = prev->second.lastAttackEvent;
				t.attackEventSent = prev->second.attackEventSent;
				if (prev->second.underConstruction && !t.underConstruction && !silent)
				{
					Json d;
					d["id"] = (unsigned)id;
					d["type"] = t.type;
					emitEvent("construction_done", d);
				}
				if (body && t.lastDamage > prev->second.lastDamage)
				{
					if (!silent && (!t.attackEventSent || now - t.lastAttackEvent >= UNDER_ATTACK_THROTTLE))
					{
						Json d;
						d["id"] = (unsigned)id;
						d["type"] = t.type;
						d["attacker_id"] = attackerOf(body);
						emitEvent("under_attack", d);
						t.lastAttackEvent = now;
						t.attackEventSent = true;
					}
				}
			}

			ProductionUpdateInterface* pu = obj->getProductionUpdateInterface();
			if (pu)
			{
				for (const ProductionEntry* e = pu->firstProduction(); e; e = pu->nextProduction(e))
					if (e->getProductionType() == PRODUCTION_UPGRADE && e->getProductionUpgrade())
						queued.insert(std::make_pair(id, std::string(e->getProductionUpgrade()->getUpgradeName().str())));
			}
			next[id] = t;
		}
		else
		{
			if (!isVisibleToLocal(obj))
				continue;
			Track t;
			t.type = apparentTemplate(obj)->getName().str();
			t.own = false;
			t.underConstruction = false;
			t.lastDamage = 0;
			t.lastAttackEvent = 0;
			t.attackEventSent = false;
			next[id] = t;
			if (isEnemyOfLocal(obj) && s_spotted.find(id) == s_spotted.end())
			{
				s_spotted.insert(id);
				if (!silent)
					newlySpotted.push_back(obj);
			}
		}
	}

	// destroyed: own objects, and non-own objects that were visible on the previous scan
	if (!silent)
	{
		for (std::map<ObjectID, Track>::const_iterator it = s_tracked.begin(); it != s_tracked.end(); ++it)
		{
			if (next.find(it->first) != next.end())
				continue;
			Object* obj = TheGameLogic->findObjectByID(it->first);
			if (obj != nullptr && !obj->isEffectivelyDead() && !obj->isDestroyed())
				continue;	// still alive: captured, contained or went into the fog
			Json d;
			d["id"] = (unsigned)it->first;
			d["type"] = it->second.type;
			d["own"] = it->second.own;
			emitEvent("object_destroyed", d);
		}
	}
	s_tracked.swap(next);

	if (!newlySpotted.empty())
	{
		Json ids = Json::array(), types = Json::array();
		double sx = 0.0, sy = 0.0;
		for (size_t i = 0; i < newlySpotted.size(); ++i)
		{
			ids.push_back((unsigned)newlySpotted[i]->getID());
			types.push_back(apparentTemplate(newlySpotted[i])->getName().str());
			sx += newlySpotted[i]->getPosition()->x;
			sy += newlySpotted[i]->getPosition()->y;
		}
		Json d;
		d["ids"] = ids;
		d["types"] = types;
		d["x"] = r1(sx / newlySpotted.size());
		d["y"] = r1(sy / newlySpotted.size());
		emitEvent("enemy_spotted", d);
	}

	// upgrades that left a queue and are now complete
	if (!silent)
	{
		for (std::set<std::pair<ObjectID, std::string> >::const_iterator it = s_queuedUpgrades.begin(); it != s_queuedUpgrades.end(); ++it)
		{
			if (queued.find(*it) != queued.end())
				continue;
			const UpgradeTemplate* ut = upgradeByName(it->second);
			if (ut == nullptr)
				continue;
			Bool done = FALSE;
			if (ut->getUpgradeType() == UPGRADE_TYPE_PLAYER)
				done = me->hasUpgradeComplete(ut);
			else
			{
				Object* obj = TheGameLogic->findObjectByID(it->first);
				done = obj != nullptr && obj->hasUpgrade(ut);
			}
			if (done)
			{
				Json d;
				d["upgrade"] = it->second;
				d["building_id"] = (unsigned)it->first;
				emitEvent("upgrade_done", d);
			}
		}
	}
	s_queuedUpgrades.swap(queued);

	// power
	const Int produced = me->getEnergy()->getProduction();
	const Int consumed = me->getEnergy()->getConsumption();
	const bool low = consumed > produced;
	if (low && !s_lowPower && !silent)
	{
		Json d;
		d["produced"] = produced;
		d["consumed"] = consumed;
		emitEvent("low_power", d);
	}
	s_lowPower = low;

	// special powers becoming ready (also when newly unlocked)
	Json player = playerJson();
	std::map<std::string, bool> ready;
	const Json& powers = player["special_powers"];
	for (size_t i = 0; i < powers.size(); ++i)
	{
		std::string name = powers[i]["name"].get<std::string>();
		bool isReady = powers[i]["ready_in_frames"].get<unsigned>() == 0;
		ready[name] = isReady;
		std::map<std::string, bool>::const_iterator prev = s_powerReady.find(name);
		if (isReady && !silent && (prev == s_powerReady.end() || !prev->second))
		{
			Json d;
			d["name"] = name;
			emitEvent("special_power_ready", d);
		}
	}
	s_powerReady.swap(ready);

	// science points
	const int points = me->getSciencePurchasePoints();
	if (points > s_sciencePoints && !silent)
	{
		Json d;
		d["points"] = points;
		emitEvent("science_available", d);
	}
	s_sciencePoints = points;

	// match result
	if (!s_gameEndSent && TheVictoryConditions)
	{
		if (TheVictoryConditions->isLocalAlliedVictory())
			eventsSendGameEnd("win");
		else if (TheVictoryConditions->isLocalAlliedDefeat() || TheVictoryConditions->isLocalDefeat())
			eventsSendGameEnd("loss");
	}
}

}	// namespace AIHook
