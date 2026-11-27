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
	//---------------------------------------------------------------------------------------------
	// Static map knowledge, captured when the match starts (what a human knows from the preview)
	//---------------------------------------------------------------------------------------------
	struct Feature
	{
		unsigned id;
		std::string type;
		double x, y;
	};

	bool s_haveStatic = false;
	Region3D s_extent;
	int s_cellsW = 0, s_cellsH = 0;
	std::string s_cells;						///< one char per pathfind cell, row-major from lo.y
	std::vector<Feature> s_features;
	Json s_startPositions;

	// get_unit_data caches (template data is static for a match)
	bool s_haveBuiltBy = false;
	std::map<std::string, std::vector<std::string> > s_builtBy;

	char cellChar(PathfindCell* c)
	{
		if (c == nullptr)
			return '#';
		switch (c->getType())
		{
			case PathfindCell::CELL_CLEAR:
			case PathfindCell::CELL_RUBBLE: return '.';
			case PathfindCell::CELL_WATER: return '~';
			case PathfindCell::CELL_CLIFF: return 'c';
			default: return '#';	// obstacles (map-placed structures at match start) and impassable
		}
	}

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

	//---------------------------------------------------------------------------------------------
	// get_unit_data pieces
	//---------------------------------------------------------------------------------------------
	void buildBuiltByCache()
	{
		if (s_haveBuiltBy)
			return;
		s_haveBuiltBy = true;
		s_builtBy.clear();
		if (TheControlBar == nullptr)
			return;
		for (const ThingTemplate* t = TheThingFactory->firstTemplate(); t; t = t->friend_getNextTemplate())
		{
			const AsciiString& csName = t->friend_getCommandSetString();
			if (csName.isEmpty())
				continue;
			const CommandSet* cs = TheControlBar->findCommandSet(csName);
			if (cs == nullptr)
				continue;
			for (Int i = 0; i < MAX_COMMANDS_PER_SET; ++i)
			{
				const CommandButton* b = cs->getCommandButton(i);
				if (b == nullptr || (b->getCommandType() != GUI_COMMAND_UNIT_BUILD && b->getCommandType() != GUI_COMMAND_DOZER_CONSTRUCT))
					continue;
				if (!b->getThingTemplate())
					continue;
				std::vector<std::string>& v = s_builtBy[b->getThingTemplate()->getName().str()];
				std::string builder = t->getName().str();
				if (std::find(v.begin(), v.end(), builder) == v.end())
					v.push_back(builder);
			}
		}
	}

	std::set<std::string> playableSides()
	{
		std::set<std::string> sides;
		if (ThePlayerTemplateStore == nullptr)
			return sides;
		for (Int i = 0; i < ThePlayerTemplateStore->getPlayerTemplateCount(); ++i)
		{
			const PlayerTemplate* pt = ThePlayerTemplateStore->getNthPlayerTemplate(i);
			if (pt && pt->isPlayableSide())
				sides.insert(pt->getSide().str());
		}
		return sides;
	}

	double templateMaxHealth(const ThingTemplate* tt)
	{
		const ModuleInfo& mi = tt->getBehaviorModuleInfo();
		for (size_t i = 0; i < (size_t)mi.getCount(); ++i)
		{
			const ActiveBodyModuleData* body = dynamic_cast<const ActiveBodyModuleData*>(mi.getNthData(i));
			if (body)
				return body->m_maxHealth;
		}
		return 0.0;
	}

	double templateSpeed(const ThingTemplate* tt)
	{
		const AIUpdateModuleData* ai = const_cast<ThingTemplate*>(tt)->friend_getAIModuleInfo();
		if (ai == nullptr)
			return 0.0;
		const LocomotorTemplateVector* v = ai->findLocomotorTemplateVector(LOCOMOTORSET_NORMAL);
		if (v == nullptr)
			return 0.0;
		for (size_t i = 0; i < v->size(); ++i)
			if ((*v)[i])
				return r1((*v)[i]->friend_getMaxSpeed() * logicFps());	// dist/frame -> dist/s
		return 0.0;
	}

	Json antiList(Int mask)
	{
		Json a = Json::array();
		if (mask & WEAPON_ANTI_GROUND)
			a.push_back("ground");
		if (mask & (WEAPON_ANTI_AIRBORNE_VEHICLE | WEAPON_ANTI_AIRBORNE_INFANTRY))
			a.push_back("air");
		if (mask & (WEAPON_ANTI_PROJECTILE | WEAPON_ANTI_SMALL_MISSILE | WEAPON_ANTI_BALLISTIC_MISSILE))
			a.push_back("missile");
		if (mask & WEAPON_ANTI_MINE)
			a.push_back("mine");
		return a;
	}

	Json templateWeapons(const ThingTemplate* tt)
	{
		Json out = Json::array();
		const WeaponTemplateSetVector& sets = tt->getWeaponTemplateSets();
		if (sets.empty())
			return out;
		const WeaponTemplateSet& ws = sets[0];	// default set (no conditions)
		WeaponBonus bonus;
		for (Int slot = PRIMARY_WEAPON; slot < WEAPONSLOT_COUNT; ++slot)
		{
			const WeaponTemplate* wt = ws.getNth((WeaponSlotType)slot);
			if (wt == nullptr)
				continue;
			Json w;
			w["name"] = wt->getName().str();
			w["damage"] = r1(wt->getPrimaryDamage(bonus));
			w["range"] = r1(wt->getUnmodifiedAttackRange());
			double delayFrames = 0.5 * (wt->friend_getMinDelayBetweenShots() + wt->friend_getMaxDelayBetweenShots());
			w["reload_s"] = floor(delayFrames / logicFps() * 100.0 + 0.5) / 100.0;
			if (wt->getClipSize() > 0)
			{
				w["clip_size"] = wt->getClipSize();
				w["clip_reload_s"] = floor(wt->getClipReloadTime(bonus) / logicFps() * 100.0 + 0.5) / 100.0;
			}
			const char* dt = DamageTypeFlags::getNameFromSingleBit((Int)wt->getDamageType());
			w["damage_type"] = dt ? dt : "";
			w["radius"] = r1(wt->getPrimaryDamageRadius(bonus));
			w["anti"] = antiList(wt->getAntiMask());
			w["slot"] = slot == PRIMARY_WEAPON ? "primary" : (slot == SECONDARY_WEAPON ? "secondary" : "tertiary");
			out.push_back(w);
		}
		return out;
	}

	/// The armor's damage multipliers that differ from 100% (the ArmorTemplate keeps no name).
	Json templateArmorCoefficients(const ThingTemplate* tt)
	{
		Json out = Json::object();
		const ArmorTemplateSet* as = tt->findArmorTemplateSet(ArmorSetFlags());
		const ArmorTemplate* at = as ? as->getArmorTemplate() : nullptr;
		if (at == nullptr)
			return out;
		for (Int i = 0; i < DAMAGE_NUM_TYPES; ++i)
		{
			Real v = at->adjustDamage((DamageType)i, 100.0f);
			if (v < 99.95f || v > 100.05f)
			{
				const char* n = DamageTypeFlags::getNameFromSingleBit(i);
				if (n)
					out[n] = r1(v);
			}
		}
		return out;
	}

	std::string templateArmorName(const ThingTemplate* tt)
	{
		const ArmorTemplateSet* as = tt->findArmorTemplateSet(ArmorSetFlags());
		const ArmorTemplate* at = as ? as->getArmorTemplate() : nullptr;
		if (at == nullptr || TheArmorStore == nullptr)
			return std::string();
		NameKeyType key = TheArmorStore->friend_findKeyOf(at);
		return key == NAMEKEY_INVALID ? std::string() : std::string(TheNameKeyGenerator->keyToName(key).str());
	}

	Json templateInfo(const ThingTemplate* tt, Player* me)
	{
		Json d;
		d["side"] = tt->getDefaultOwningSide().str();
		d["kind"] = templateKind(tt);
		d["cost"] = tt->calcCostToBuild(me);
		d["build_time_s"] = floor(tt->calcTimeToBuild(me) / logicFps() * 10.0 + 0.5) / 10.0;
		d["max_health"] = r1(templateMaxHealth(tt));
		d["speed"] = templateSpeed(tt);
		d["vision"] = r1(tt->friend_calcVisionRange());
		d["weapons"] = templateWeapons(tt);
		d["armor"] = templateArmorName(tt);
		d["armor_coefficients"] = templateArmorCoefficients(tt);
		d["prereqs"] = unitPrereqNames(tt);
		d["science_prereqs"] = sciencePrereqNames(tt);
		std::map<std::string, std::vector<std::string> >::const_iterator it = s_builtBy.find(tt->getName().str());
		d["built_by"] = it != s_builtBy.end() ? it->second : std::vector<std::string>();
		Json kinds = Json::array();
		for (Int k = KINDOF_FIRST; k < KINDOF_COUNT; ++k)
		{
			if (tt->isKindOf((KindOfType)k))
			{
				const char* n = KindOfMaskType::getNameFromSingleBit(k);
				if (n)
					kinds.push_back(n);
			}
		}
		d["kind_of"] = kinds;
		Int power = tt->getEnergyProduction();
		if (power != 0)
			d["power"] = power;
		return d;
	}

	/// Was pathfind cell (cx, cy) blocked by a map-placed structure at match start?
	bool staticObstacleAt(int cx, int cy)
	{
		if (s_cells.empty())
			return false;
		const int i = cx - (int)floor(s_extent.lo.x / PATHFIND_CELL_SIZE_F);
		const int j = cy - (int)floor(s_extent.lo.y / PATHFIND_CELL_SIZE_F);
		if (i < 0 || j < 0 || i >= s_cellsW || j >= s_cellsH)
			return true;
		return s_cells[(size_t)j * s_cellsW + i] == '#';
	}

	//---------------------------------------------------------------------------------------------
	// query_path: A* on the ground pathfind layer, using only read accessors
	//---------------------------------------------------------------------------------------------
	double gridPathLength(Pathfinder* pf, LocomotorSurfaceTypeMask surfaces, const Coord3D& from, const Coord3D& to, bool& ok)
	{
		ok = false;
		ICoord2D s, g;
		pf->worldToCell(&from, &s);
		pf->worldToCell(&to, &g);
		const ICoord2D* hi = pf->getExtent();
		const int W = hi->x + 1, H = hi->y + 1;
		if (W <= 0 || H <= 0 || s.x < 0 || s.y < 0 || g.x < 0 || g.y < 0 || s.x >= W || s.y >= H || g.x >= W || g.y >= H)
			return 0.0;
		const int start = s.y * W + s.x, goal = g.y * W + g.x;
		if (start == goal)
		{
			ok = true;
			return 0.0;
		}

		std::vector<signed char> pass((size_t)W * H, -1);	// -1 unknown, 0 blocked, 1 open
		auto passable = [&](int idx) -> bool
		{
			signed char& p = pass[(size_t)idx];
			if (p < 0)
			{
				PathfindCell* c = pf->getCell(LAYER_GROUND, idx % W, idx / W);
				if (c && c->getType() == PathfindCell::CELL_OBSTACLE)
					p = staticObstacleAt(idx % W, idx / W) ? 0 : 1;	// only what the map preview shows
				else
					p = (c && pf->validMovementPosition(FALSE, surfaces, c)) ? 1 : 0;
			}
			return p == 1;
		};
		pass[(size_t)start] = 1;	// we're standing here, whatever the cell says

		std::vector<float> gcost((size_t)W * H, 1e30f);
		typedef std::pair<float, int> Node;
		std::priority_queue<Node, std::vector<Node>, std::greater<Node> > open;
		auto h = [&](int idx) -> float
		{
			int dx = abs(idx % W - g.x), dy = abs(idx / W - g.y);
			int mn = dx < dy ? dx : dy, mx = dx < dy ? dy : dx;
			return (float)(mx - mn) + 1.41421356f * (float)mn;
		};
		gcost[(size_t)start] = 0.0f;
		open.push(Node(h(start), start));
		const int dxs[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
		const int dys[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
		int expansions = 0;
		while (!open.empty())
		{
			Node n = open.top();
			open.pop();
			int cur = n.second;
			float gc = gcost[(size_t)cur];
			if (n.first > gc + h(cur) + 1e-3f)
				continue;	// stale entry
			if (cur == goal)
			{
				ok = true;
				return gc * PATHFIND_CELL_SIZE_F;
			}
			if (++expansions > 1500000)
				break;
			int cx = cur % W, cy = cur / W;
			for (int k = 0; k < 8; ++k)
			{
				int nx = cx + dxs[k], ny = cy + dys[k];
				if (nx < 0 || ny < 0 || nx >= W || ny >= H)
					continue;
				int ni = ny * W + nx;
				if (!passable(ni))
					continue;
				if (k >= 4 && (!passable(cy * W + nx) || !passable(ny * W + cx)))
					continue;	// no corner cutting
				float ng = gc + (k >= 4 ? 1.41421356f : 1.0f);
				if (ng < gcost[(size_t)ni])
				{
					gcost[(size_t)ni] = ng;
					open.push(Node(ng + h(ni), ni));
				}
			}
		}
		return 0.0;
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

std::vector<std::string> sciencePrereqNames(const ThingTemplate* tt)
{
	std::vector<std::string> out;
	for (Int i = 0; i < tt->getPrereqCount(); ++i)
	{
		const ScienceVec& sv = tt->getNthPrereq(i)->friend_getPrereqSciences();
		for (size_t k = 0; k < sv.size(); ++k)
		{
			std::string n = TheScienceStore->getInternalNameForScience(sv[k]).str();
			if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end())
				out.push_back(n);
		}
	}
	return out;
}

std::vector<std::string> missingPrereqNames(const ThingTemplate* tt, Player* player, std::string* text)
{
	std::vector<std::string> out;
	std::vector<std::string> shown;	// same entries for the message, with science display names
	for (Int i = 0; i < tt->getPrereqCount(); ++i)
	{
		const ProductionPrerequisite* pre = tt->getNthPrereq(i);
		if (pre->isSatisfied(player))
			continue;
		size_t before = out.size();

		// object prerequisites: the first alternative block (as unitPrereqNames lists it)
		const ThingTemplate* alts[16];
		Int n = pre->getAllPossibleBuildFacilityTemplates(alts, 16);
		std::string s;
		Int validAlts = 0;
		const ThingTemplate* valid[16];
		for (Int k = 0; k < n; ++k)
		{
			if (alts[k] == nullptr)
				continue;
			valid[validAlts++] = alts[k];
			if (!s.empty())
				s += "|";
			s += alts[k]->getName().str();
		}
		if (validAlts > 0)
		{
			Int counts[16];
			player->countObjectsByThingTemplate(validAlts, valid, false, counts);	// what isSatisfied counts
			Int owned = 0;
			for (Int k = 0; k < validAlts; ++k)
				owned += counts[k];
			if (owned == 0)
			{
				out.push_back(s);
				shown.push_back(s);
			}
		}

		// science prerequisites
		const ScienceVec& sv = pre->friend_getPrereqSciences();
		for (size_t k = 0; k < sv.size(); ++k)
		{
			if (player->hasScience(sv[k]))
				continue;
			std::string name = TheScienceStore->getInternalNameForScience(sv[k]).str();
			if (name.empty() || std::find(out.begin(), out.end(), name) != out.end())
				continue;
			out.push_back(name);
			UnicodeString disp, desc;
			std::string label = name;
			if (TheScienceStore->getNameAndDescription(sv[k], disp, desc) && !disp.isEmpty())
			{
				AsciiString a;
				a.translate(disp);
				if (!a.isEmpty())
					label += std::string(" (") + a.str() + ")";
			}
			shown.push_back(label);
		}

		if (out.size() == before)
		{
			// unmet for a reason we can't name (e.g. a second object block): the game's own text, message only
			AsciiString a;
			a.translate(pre->getRequiresList(player));
			if (!a.isEmpty())
				shown.push_back(a.str());
		}
	}
	if (text)
	{
		text->clear();
		for (size_t i = 0; i < shown.size(); ++i)
		{
			if (i)
				*text += ", ";
			*text += shown[i];
		}
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

	Json own = Json::array(), enemies = Json::array(), neutral = Json::array(), allies = Json::array();
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
			else if (isNeutralPlayerObject(obj))
			{
				if (incNeutral)
					neutral.push_back(otherObjectJson(obj));
			}
			else if (incEnemies)
				allies.push_back(otherObjectJson(obj));	// additive: allied players' objects
		}
	}
	if (incOwn)
		out["own"] = own;
	if (incEnemies)
		out["enemies"] = enemies;
	if (incNeutral)
		out["neutral"] = neutral;
	if (incEnemies)
		out["allies"] = allies;
	return out;
}

//-------------------------------------------------------------------------------------------------
void stateOnMatchStart()
{
	s_haveStatic = false;
	s_haveBuiltBy = false;
	s_builtBy.clear();
	s_features.clear();
	s_cells.clear();
	s_startPositions = Json::array();

	if (TheTerrainLogic == nullptr || TheAI == nullptr || TheAI->pathfinder() == nullptr)
		return;
	TheTerrainLogic->getExtent(&s_extent);

	// terrain classes at match start (only map-placed structures and the starting buildings exist)
	Pathfinder* pf = TheAI->pathfinder();
	s_cellsW = (int)((s_extent.hi.x - s_extent.lo.x) / PATHFIND_CELL_SIZE_F);
	s_cellsH = (int)((s_extent.hi.y - s_extent.lo.y) / PATHFIND_CELL_SIZE_F);
	if (s_cellsW < 0) s_cellsW = 0;
	if (s_cellsH < 0) s_cellsH = 0;
	s_cells.resize((size_t)s_cellsW * s_cellsH);
	for (int j = 0; j < s_cellsH; ++j)
	{
		for (int i = 0; i < s_cellsW; ++i)
		{
			Coord3D pos;
			pos.x = s_extent.lo.x + (i + 0.5f) * PATHFIND_CELL_SIZE_F;
			pos.y = s_extent.lo.y + (j + 0.5f) * PATHFIND_CELL_SIZE_F;
			pos.z = 0.0f;
			s_cells[(size_t)j * s_cellsW + i] = cellChar(pf->getCell(LAYER_GROUND, &pos));
		}
	}

	// a human only knows map-placed buildings from the preview: un-block cells under structures
	// owned by real players (the starting bases), which would reveal random start assignments
	{
		Player* neutralP = ThePlayerList->getNeutralPlayer();
		for (Object* obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		{
			const Player* owner = obj->getControllingPlayer();
			if (owner == nullptr || owner == neutralP || !obj->isKindOf(KINDOF_STRUCTURE))
				continue;
			const Coord3D* c = obj->getPosition();
			const Real r = obj->getGeometryInfo().getBoundingCircleRadius() + PATHFIND_CELL_SIZE_F;
			const int i0 = (int)((c->x - r - s_extent.lo.x) / PATHFIND_CELL_SIZE_F), i1 = (int)((c->x + r - s_extent.lo.x) / PATHFIND_CELL_SIZE_F);
			const int j0 = (int)((c->y - r - s_extent.lo.y) / PATHFIND_CELL_SIZE_F), j1 = (int)((c->y + r - s_extent.lo.y) / PATHFIND_CELL_SIZE_F);
			for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < s_cellsH; ++j)
				for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < s_cellsW; ++i)
				{
					Coord3D pos;
					pos.x = s_extent.lo.x + (i + 0.5f) * PATHFIND_CELL_SIZE_F;
					pos.y = s_extent.lo.y + (j + 0.5f) * PATHFIND_CELL_SIZE_F;
					pos.z = 0.0f;
					PathfindCell* cell = pf->getCell(LAYER_GROUND, &pos);
					if (cell && cell->getType() == PathfindCell::CELL_OBSTACLE)
						s_cells[(size_t)j * s_cellsW + i] = '.';
				}
		}
	}

	// start spots: "index" is a player index (like local_player); only the local player's own spot
	// is known, the others are reported with index null and their 0-based "slot"
	const Coord3D* ownAnchor = nullptr;
	{
		Player* me = localPlayer();
		for (Object* obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		{
			if (obj->getControllingPlayer() != me)
				continue;
			if (ownAnchor == nullptr || obj->isKindOf(KINDOF_COMMANDCENTER))
				ownAnchor = obj->getPosition();
			if (obj->isKindOf(KINDOF_COMMANDCENTER))
				break;
		}
	}
	int ownSlot = -1;
	if (ownAnchor)
	{
		Real best = 1e30f;
		for (Int n = 1; n <= MAX_PLAYER_COUNT; ++n)
		{
			AsciiString wpName;
			wpName.format("Player_%d_Start", n);
			Waypoint* wp = TheTerrainLogic->getWaypointByName(wpName);
			if (wp == nullptr)
				continue;
			Real dx = wp->getLocation()->x - ownAnchor->x, dy = wp->getLocation()->y - ownAnchor->y;
			if (dx * dx + dy * dy < best)
			{
				best = dx * dx + dy * dy;
				ownSlot = n - 1;
			}
		}
	}
	for (Int n = 1; n <= MAX_PLAYER_COUNT; ++n)
	{
		AsciiString wpName;
		wpName.format("Player_%d_Start", n);
		Waypoint* wp = TheTerrainLogic->getWaypointByName(wpName);
		if (wp == nullptr)
			continue;
		Json sp;
		if (n - 1 == ownSlot)
			sp["index"] = (int)localPlayer()->getPlayerIndex();
		else
			sp["index"] = nullptr;
		sp["slot"] = n - 1;
		sp["x"] = r1(wp->getLocation()->x);
		sp["y"] = r1(wp->getLocation()->y);
		s_startPositions.push_back(sp);
	}

	// map-placed neutral economy/tech objects
	Player* neutralPlayer = ThePlayerList->getNeutralPlayer();
	for (Object* obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
	{
		const Player* owner = obj->getControllingPlayer();
		if (owner != nullptr && owner != neutralPlayer)
			continue;
		const ThingTemplate* tt = obj->getTemplate();
		if (!(tt->isKindOf(KINDOF_SUPPLY_SOURCE) || tt->isKindOf(KINDOF_TECH_BUILDING) || tt->isKindOf(KINDOF_CASH_GENERATOR) ||
				(tt->isKindOf(KINDOF_STRUCTURE) && tt->isKindOf(KINDOF_CAPTURABLE))))
			continue;
		if (!isReportable(obj))
			continue;
		Feature f;
		f.id = (unsigned)obj->getID();
		f.type = tt->getName().str();
		f.x = r1(obj->getPosition()->x);
		f.y = r1(obj->getPosition()->y);
		s_features.push_back(f);
	}
	s_haveStatic = true;
}

void stateOnMatchEnd()
{
	s_haveStatic = false;
	s_haveBuiltBy = false;
	s_builtBy.clear();
	s_features.clear();
	s_cells.clear();
}

//-------------------------------------------------------------------------------------------------
Json cmdGetMap(const Json& args)
{
	long long stride = argIntOr(args, "cell_stride", 4);
	if (stride < 1)
		stride = 1;
	if (stride > 32)
		stride = 32;
	if (!s_haveStatic)
		stateOnMatchStart();

	const int gw = s_cellsW / (int)stride, gh = s_cellsH / (int)stride;
	std::string passable;
	passable.reserve((size_t)gw * gh);
	for (int j = 0; j < gh; ++j)
	{
		for (int i = 0; i < gw; ++i)
		{
			int ci = i * (int)stride + (int)stride / 2, cj = j * (int)stride + (int)stride / 2;
			passable.push_back(s_cells[(size_t)cj * s_cellsW + ci]);
		}
	}

	Json m;
	m["name"] = mapName();
	m["width"] = r1(s_extent.hi.x - s_extent.lo.x);
	m["height"] = r1(s_extent.hi.y - s_extent.lo.y);
	m["origin"] = { { "x", r1(s_extent.lo.x) }, { "y", r1(s_extent.lo.y) } };
	m["cell_size"] = (double)PATHFIND_CELL_SIZE_F;
	m["cell_stride"] = stride;
	m["grid_w"] = gw;
	m["grid_h"] = gh;
	m["passable"] = passable;
	m["start_positions"] = s_startPositions;
	Json feats = Json::array();
	for (size_t i = 0; i < s_features.size(); ++i)
	{
		Json f;
		f["type"] = s_features[i].type;
		f["x"] = s_features[i].x;
		f["y"] = s_features[i].y;
		f["id"] = s_features[i].id;
		feats.push_back(f);
	}
	m["features"] = feats;
	return m;
}

//-------------------------------------------------------------------------------------------------
Json cmdQueryPath(const Json& args)
{
	if (!hasArg(args, "unit_id"))
		throw CmdError("BAD_ARGS", "unit_id is required");
	Object* unit = requireOwnObject(args["unit_id"], "unit");
	double x = argReal(args, "x"), y = argReal(args, "y");
	requireOnMap(x, y);

	Json r;
	AIUpdateInterface* ai = unit->getAIUpdateInterface();
	if (ai == nullptr || unit->isKindOf(KINDOF_STRUCTURE))
	{
		r["reachable"] = false;
		r["distance"] = 0.0;
		return r;
	}

	Coord3D to;
	to.x = (Real)x;
	to.y = (Real)y;
	to.z = TheTerrainLogic->getGroundHeight(to.x, to.y);
	const Coord3D* from = unit->getPosition();
	const LocomotorSet& ls = ai->getLocomotorSet();
	const double straight = sqrt((to.x - from->x) * (to.x - from->x) + (to.y - from->y) * (to.y - from->y));

	if (ls.getValidSurfaces() & LOCOMOTORSURFACE_AIR)
	{
		r["reachable"] = true;
		r["distance"] = r1(straight);
		return r;
	}

	if (!s_haveStatic)
		stateOnMatchStart();
	Pathfinder* pf = TheAI->pathfinder();
	// terrain-only check (the full one sees shrouded/stealthed buildings)
	Bool reachable = pf->clientSafeQuickDoesPathExistForUI(ls, from, &to);
	r["reachable"] = (bool)reachable;
	if (!reachable)
	{
		r["distance"] = 0.0;
		return r;
	}
	bool ok = false;
	double d = gridPathLength(pf, ls.getValidSurfaces(), *from, to, ok);
	if (ok)
		r["distance"] = r1(d);
	else
	{
		// reachable per the game, but not on the ground layer alone (bridges): straight-line estimate
		r["distance"] = r1(straight);
		r["approx"] = true;
	}
	return r;
}

//-------------------------------------------------------------------------------------------------
Json cmdGetUnitData(const Json& args)
{
	buildBuiltByCache();
	Player* me = localPlayer();
	Json out = Json::object();

	if (hasArg(args, "templates"))
	{
		if (!args["templates"].is_array())
			throw CmdError("BAD_ARGS", "templates must be a list of strings");
		const Json& names = args["templates"];
		for (size_t i = 0; i < names.size(); ++i)
		{
			if (!names[i].is_string())
				throw CmdError("BAD_ARGS", "templates must be a list of strings");
			std::string n = names[i].get<std::string>();
			const ThingTemplate* tt = n.empty() ? nullptr : TheThingFactory->findTemplate(AsciiString(n.c_str()), FALSE);
			if (tt == nullptr)
				throw CmdError("INVALID_TEMPLATE", strf("unknown template '%s'", n.c_str()));
			out[n] = templateInfo(tt, me);
		}
	}
	else
	{
		// default: every template some playable side can build
		std::set<std::string> sides = playableSides();
		for (std::map<std::string, std::vector<std::string> >::const_iterator it = s_builtBy.begin(); it != s_builtBy.end(); ++it)
		{
			const ThingTemplate* tt = TheThingFactory->findTemplate(AsciiString(it->first.c_str()), FALSE);
			if (tt == nullptr || sides.find(tt->getDefaultOwningSide().str()) == sides.end())
				continue;
			out[it->first] = templateInfo(tt, me);
		}
	}
	Json r;
	r["templates"] = out;
	return r;
}

}	// namespace AIHook
