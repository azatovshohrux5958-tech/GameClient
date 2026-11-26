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
	// Core orders (B3)
	//---------------------------------------------------------------------------------------------
	Json groupOrderAtPos(const Json& args, GameMessage::Type type, Bool withGuardMode)
	{
		requireCanIssueOrders();
		UnitList units = collectUnits(args);
		double x = argReal(args, "x"), y = argReal(args, "y");
		requireOnMap(x, y);
		appendSelect(units.ok);
		GameMessage* msg = TheMessageStream->appendMessage(type);
		msg->appendLocationArgument(groundPos(x, y));
		if (withGuardMode)
			msg->appendIntegerArgument(GUARDMODE_NORMAL);
		return orderResult(units);
	}

	Json cmdMove(const Json& a) { return groupOrderAtPos(a, GameMessage::MSG_DO_MOVETO, FALSE); }
	Json cmdAttackMove(const Json& a) { return groupOrderAtPos(a, GameMessage::MSG_DO_ATTACKMOVETO, FALSE); }
	Json cmdForceAttackGround(const Json& a) { return groupOrderAtPos(a, GameMessage::MSG_DO_FORCE_ATTACK_GROUND, FALSE); }
	Json cmdGuard(const Json& a) { return groupOrderAtPos(a, GameMessage::MSG_DO_GUARD_POSITION, TRUE); }

	Json cmdStop(const Json& args)
	{
		requireCanIssueOrders();
		UnitList units = collectUnits(args);
		appendSelect(units.ok);
		TheMessageStream->appendMessage(GameMessage::MSG_DO_STOP);
		return orderResult(units);
	}

	Json cmdAttack(const Json& args)
	{
		requireCanIssueOrders();
		UnitList units = collectUnits(args);
		if (!hasArg(args, "target_id"))
			throw CmdError("BAD_ARGS", "target_id is required");
		ObjectID tid = (ObjectID)argObjectID(args["target_id"], "target_id");
		Object* target = findAlive(tid);
		if (target == nullptr || !isVisibleToLocal(target))
			throw CmdError("UNIT_DEAD", strf("target %u does not exist or is not visible", (unsigned)tid));
		appendSelect(units.ok);
		// non-enemies (own, allied, civilian) can only be attacked with a force-attack, as in the UI
		GameMessage::Type type = isEnemyOfLocal(target) ? GameMessage::MSG_DO_ATTACK_OBJECT : GameMessage::MSG_DO_FORCE_ATTACK_OBJECT;
		GameMessage* msg = TheMessageStream->appendMessage(type);
		msg->appendObjectIDArgument(tid);
		return orderResult(units);
	}

	//---------------------------------------------------------------------------------------------
	// Economy orders (B4)
	//---------------------------------------------------------------------------------------------
	const char* legalBuildCodeText(LegalBuildCode lbc)
	{
		switch (lbc)
		{
			case LBC_RESTRICTED_TERRAIN: return "restricted terrain";
			case LBC_NOT_FLAT_ENOUGH: return "ground not flat enough";
			case LBC_OBJECTS_IN_THE_WAY: return "objects in the way";
			case LBC_NO_CLEAR_PATH: return "no clear path for the builder";
			case LBC_SHROUD: return "location is shrouded";
			case LBC_TOO_CLOSE_TO_SUPPLIES: return "too close to supplies";
			default: return "placement check failed";
		}
	}

	Json cmdBuildStructure(const Json& args)
	{
		requireCanIssueOrders();
		Player* me = localPlayer();
		if (!hasArg(args, "dozer_id"))
			throw CmdError("BAD_ARGS", "dozer_id is required");
		Object* dozer = requireOwnObject(args["dozer_id"], "dozer");
		std::string name = argStr(args, "template");
		double x = argReal(args, "x"), y = argReal(args, "y");
		double angle = argRealOr(args, "angle", 0.0);

		const ThingTemplate* tt = findThing(name);
		if (tt == nullptr || !tt->isKindOf(KINDOF_STRUCTURE))
			throw CmdError("INVALID_TEMPLATE", strf("unknown structure '%s'", name.c_str()));
		if (dozer->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) || findBuildButton(dozer, tt) == nullptr)
			throw CmdError("WRONG_PRODUCER", strf("%s %u can't build %s", dozer->getTemplate()->getName().str(),
				(unsigned)dozer->getID(), name.c_str()));
		requireOnMap(x, y);

		CanMakeType cmt = TheBuildAssistant->canMakeUnit(dozer, tt);
		switch (cmt)
		{
			case CANMAKE_OK: break;
			case CANMAKE_NO_PREREQ: throwMissingPrereq(tt, me);
			case CANMAKE_NO_MONEY:
				throw CmdError("NO_MONEY", strf("needs $%d, have $%u", tt->calcCostToBuild(me), me->getMoney()->countMoney()));
			case CANMAKE_QUEUE_FULL:
			case CANMAKE_PARKING_PLACES_FULL: throw CmdError("QUEUE_FULL", "builder is busy");
			case CANMAKE_MAXED_OUT_FOR_PLAYER: throw CmdError("CANNOT_DO", strf("maximum number of %s reached", name.c_str()));
			case CANMAKE_FACTORY_IS_DISABLED: throw CmdError("CANNOT_DO", "builder is disabled");
			default: throw CmdError("CANNOT_DO", "the game refuses to build this");
		}

		Coord3D pos = groundPos(x, y);
		LegalBuildCode lbc = TheBuildAssistant->isLocationLegalToBuild(&pos, tt, (Real)angle,
			BuildAssistant::USE_QUICK_PATHFIND | BuildAssistant::TERRAIN_RESTRICTIONS | BuildAssistant::CLEAR_PATH |
			BuildAssistant::NO_OBJECT_OVERLAP | BuildAssistant::SHROUD_REVEALED | BuildAssistant::IGNORE_STEALTHED |
			BuildAssistant::FAIL_STEALTHED_WITHOUT_FEEDBACK,
			dozer, nullptr);
		if (lbc != LBC_OK)
			throw CmdError("CANNOT_BUILD_HERE", strf("%s at (%.0f,%.0f): %s", name.c_str(), x, y, legalBuildCodeText(lbc)));

		appendSelect(dozer->getID());
		GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_DOZER_CONSTRUCT);
		msg->appendIntegerArgument(tt->getTemplateID());
		msg->appendLocationArgument(pos);
		msg->appendRealArgument((Real)angle);
		return orderResult(dozer->getID());
	}

	Object* requireFinishedProducer(const Json& args, const char* key)
	{
		if (!hasArg(args, key))
			throw CmdError("BAD_ARGS", strf("%s is required", key));
		Object* obj = requireOwnObject(args[key], key);
		if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) || obj->testStatus(OBJECT_STATUS_SOLD) ||
				obj->getProductionUpdateInterface() == nullptr)
			throw CmdError("WRONG_PRODUCER", strf("%s %u can't produce now", obj->getTemplate()->getName().str(), (unsigned)obj->getID()));
		return obj;
	}

	Json cmdTrainUnit(const Json& args)
	{
		requireCanIssueOrders();
		Player* me = localPlayer();
		Object* factory = requireFinishedProducer(args, "factory_id");
		std::string name = argStr(args, "template");
		long long count = argIntOr(args, "count", 1);

		const ThingTemplate* tt = findThing(name);
		if (tt == nullptr || tt->isKindOf(KINDOF_STRUCTURE))
			throw CmdError("INVALID_TEMPLATE", strf("unknown unit '%s'", name.c_str()));
		if (findBuildButton(factory, tt) == nullptr)
			throw CmdError("WRONG_PRODUCER", strf("%s can't build %s", factory->getTemplate()->getName().str(), name.c_str()));
		if (count < 1 || count > 9)
			throw CmdError("BAD_ARGS", "count must be 1..9");

		CanMakeType cmt = TheBuildAssistant->canMakeUnit(factory, tt);
		switch (cmt)
		{
			case CANMAKE_OK: break;
			case CANMAKE_NO_PREREQ: throwMissingPrereq(tt, me);
			case CANMAKE_NO_MONEY:
				throw CmdError("NO_MONEY", strf("needs $%d, have $%u", tt->calcCostToBuild(me), me->getMoney()->countMoney()));
			case CANMAKE_QUEUE_FULL: throw CmdError("QUEUE_FULL", "production queue is full");
			case CANMAKE_PARKING_PLACES_FULL: throw CmdError("QUEUE_FULL", "no free parking places");
			case CANMAKE_MAXED_OUT_FOR_PLAYER: throw CmdError("CANNOT_DO", strf("maximum number of %s reached", name.c_str()));
			case CANMAKE_FACTORY_IS_DISABLED: throw CmdError("CANNOT_DO", "factory is disabled");
			default: throw CmdError("CANNOT_DO", "the game refuses to build this");
		}

		// queue as many as we can afford right now (the game charges when each one starts)
		Int cost = tt->calcCostToBuild(me);
		long long affordable = cost > 0 ? (long long)(me->getMoney()->countMoney() / (UnsignedInt)cost) : count;
		long long n = count < affordable ? count : affordable;
		if (n < 1)
			n = 1;

		ProductionUpdateInterface* pu = factory->getProductionUpdateInterface();
		appendSelect(factory->getID());
		for (long long i = 0; i < n; ++i)
		{
			GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UNIT_CREATE);
			msg->appendIntegerArgument(tt->getTemplateID());
			msg->appendIntegerArgument(pu->requestUniqueUnitID());	// as ControlBarCommandProcessing does
		}
		Json res = orderResult(factory->getID());
		res["count"] = n;
		return res;
	}

	Json cmdCancelTrain(const Json& args)
	{
		requireCanIssueOrders();
		Object* factory = requireFinishedProducer(args, "factory_id");
		std::string name = argStr(args, "template");
		const ThingTemplate* tt = findThing(name);
		if (tt == nullptr)
			throw CmdError("INVALID_TEMPLATE", strf("unknown unit '%s'", name.c_str()));

		ProductionUpdateInterface* pu = factory->getProductionUpdateInterface();
		const ProductionEntry* found = nullptr;
		for (const ProductionEntry* e = pu->firstProduction(); e; e = pu->nextProduction(e))
			if (e->getProductionType() == PRODUCTION_UNIT && e->getProductionObject() && e->getProductionObject()->isEquivalentTo(tt))
				found = e;	// cancel the last one queued
		if (found == nullptr)
			throw CmdError("CANNOT_DO", strf("%s is not in the queue", name.c_str()));

		appendSelect(factory->getID());
		GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_CANCEL_UNIT_CREATE);
		msg->appendIntegerArgument(found->getProductionID());
		return orderResult(factory->getID());
	}

	Json cmdResearchUpgrade(const Json& args)
	{
		requireCanIssueOrders();
		Player* me = localPlayer();
		Object* building = requireFinishedProducer(args, "building_id");
		std::string name = argStr(args, "upgrade");
		const UpgradeTemplate* ut = findUpgradeByName(name);
		if (ut == nullptr)
			throw CmdError("INVALID_TEMPLATE", strf("unknown upgrade '%s'", name.c_str()));
		const CommandButton* button = findUpgradeButton(building, ut);
		if (button == nullptr)
			throw CmdError("WRONG_PRODUCER", strf("%s can't research %s", building->getTemplate()->getName().str(), name.c_str()));

		ProductionUpdateInterface* pu = building->getProductionUpdateInterface();
		if (ut->getUpgradeType() == UPGRADE_TYPE_PLAYER)
		{
			if (me->hasUpgradeComplete(ut))
				throw CmdError("CANNOT_DO", strf("%s is already researched", name.c_str()));
			if (me->hasUpgradeInProduction(ut))
				throw CmdError("CANNOT_DO", strf("%s is already in production", name.c_str()));
		}
		else
		{
			if (building->hasUpgrade(ut))
				throw CmdError("CANNOT_DO", strf("%s already has %s", building->getTemplate()->getName().str(), name.c_str()));
			if (!building->affectedByUpgrade(ut))
				throw CmdError("CANNOT_DO", strf("%s does not apply to this building", name.c_str()));
		}
		if (pu->isUpgradeInQueue(ut))
			throw CmdError("CANNOT_DO", strf("%s is already queued", name.c_str()));

		// science-gated buttons (e.g. general-specific upgrades)
		const ScienceVec& sv = button->getScienceVec();
		for (size_t i = 0; i < sv.size(); ++i)
			if (!me->hasScience(sv[i]))
				throw CmdError("MISSING_PREREQ", "needs " + std::string(TheScienceStore->getInternalNameForScience(sv[i]).str()));

		if (!TheUpgradeCenter->canAffordUpgrade(me, ut, FALSE))
			throw CmdError("NO_MONEY", strf("needs $%d, have $%u", ut->calcCostToBuild(me), me->getMoney()->countMoney()));
		CanMakeType cmt = pu->canQueueUpgrade(ut);
		if (cmt == CANMAKE_QUEUE_FULL)
			throw CmdError("QUEUE_FULL", "production queue is full");
		if (cmt != CANMAKE_OK)
			throw CmdError("CANNOT_DO", "the game refuses to queue this upgrade");

		appendSelect(building->getID());
		GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_QUEUE_UPGRADE);
		msg->appendObjectIDArgument(building->getID());
		msg->appendIntegerArgument(ut->getUpgradeNameKey());
		return orderResult(building->getID());
	}

	Json cmdPurchaseScience(const Json& args)
	{
		requireCanIssueOrders();
		Player* me = localPlayer();
		std::string name = argStr(args, "science");
		ScienceType st = findScienceByName(name);
		if (st == SCIENCE_INVALID || !TheScienceStore->isValidScience(st))
			throw CmdError("INVALID_TEMPLATE", strf("unknown science '%s'", name.c_str()));
		if (me->hasScience(st))
			throw CmdError("NOT_READY", strf("%s is already owned", name.c_str()));
		if (me->isScienceDisabled(st) || me->isScienceHidden(st))
			throw CmdError("NOT_READY", strf("%s is not available", name.c_str()));
		Int cost = TheScienceStore->getSciencePurchaseCost(st);
		if (cost <= 0)
			throw CmdError("NOT_READY", strf("%s can't be purchased", name.c_str()));
		if (!TheScienceStore->playerHasPrereqsForScience(me, st))
			throw CmdError("NOT_READY", strf("%s: rank or prerequisite sciences missing", name.c_str()));
		if (cost > me->getSciencePurchasePoints())
			throw CmdError("NOT_READY", strf("%s costs %d points, have %d", name.c_str(), cost, me->getSciencePurchasePoints()));

		GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_PURCHASE_SCIENCE);
		msg->appendIntegerArgument(st);
		Json res;
		res["queued"] = true;
		res["unit_ids"] = Json::array();
		return res;
	}

	/// Options of the UI button that fires this power (targeting flags etc.), 0 if none.
	UnsignedInt powerButtonOptions(const SpecialPowerTemplate* sp)
	{
		if (TheControlBar == nullptr)
			return 0;
		for (const CommandButton* b = TheControlBar->getCommandButtons(); b; b = const_cast<CommandButton*>(b)->friend_getNext())
			if (b->getSpecialPowerTemplate() == sp &&
					(b->getCommandType() == GUI_COMMAND_SPECIAL_POWER || b->getCommandType() == GUI_COMMAND_SPECIAL_POWER_FROM_SHORTCUT))
				return b->getOptions();
		return 0;
	}

	Bool moduleUsable(Object* obj, SpecialPowerModuleInterface* spm, Player* me)
	{
		if (spm == nullptr || spm->isScriptOnly())
			return FALSE;
		if (obj->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) || obj->testStatus(OBJECT_STATUS_SOLD) || obj->isEffectivelyDead())
			return FALSE;
		ScienceType req = spm->getRequiredScience();
		if (req != SCIENCE_INVALID && !me->hasScience(req))
			return FALSE;
		return TRUE;
	}

	Json cmdFireSpecialPower(const Json& args)
	{
		requireCanIssueOrders();
		Player* me = localPlayer();
		std::string name = argStr(args, "power");
		const SpecialPowerTemplate* sp = findPowerByName(name);
		if (sp == nullptr)
			throw CmdError("INVALID_TEMPLATE", strf("unknown special power '%s'", name.c_str()));

		Object* source = nullptr;
		SpecialPowerModuleInterface* spm = nullptr;
		if (hasArg(args, "source_id"))
		{
			source = requireOwnObject(args["source_id"], "source");
			spm = source->getSpecialPowerModule(sp);
			if (!moduleUsable(source, spm, me))
				throw CmdError("CANNOT_DO", strf("%s %u can't use %s", source->getTemplate()->getName().str(), (unsigned)source->getID(), name.c_str()));
		}
		else
		{
			UnsignedInt best = 0xffffffffu;
			for (Object* obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
			{
				if (obj->getControllingPlayer() != me)
					continue;
				SpecialPowerModuleInterface* m = obj->getSpecialPowerModule(sp);
				if (!moduleUsable(obj, m, me))
					continue;
				UnsignedInt rf = obj->isDisabled() ? 0xfffffff0u : m->getReadyFrame();
				if (source == nullptr || rf < best)
				{
					source = obj;
					spm = m;
					best = rf;
				}
			}
			if (source == nullptr)
				throw CmdError("NOT_READY", strf("%s is not unlocked", name.c_str()));
		}

		UnsignedInt now = TheGameLogic->getFrame();
		if (source->isDisabled())
			throw CmdError("NOT_READY", strf("%s: source is disabled", name.c_str()));
		UnsignedInt readyFrame = spm->getReadyFrame();
		if (readyFrame > now)
			throw CmdError("NOT_READY", strf("%s ready in %.0f s", name.c_str(), (double)(readyFrame - now) / logicFps()));

		UnsignedInt options = powerButtonOptions(sp);
		const Bool needsObject = (options & COMMAND_OPTION_NEED_OBJECT_TARGET) != 0;
		const Bool needsPos = (options & NEED_TARGET_POS) != 0;

		if (hasArg(args, "target_id"))
		{
			ObjectID tid = (ObjectID)argObjectID(args["target_id"], "target_id");
			Object* target = findAlive(tid);
			if (target == nullptr || !isVisibleToLocal(target))
				throw CmdError("UNIT_DEAD", strf("target %u does not exist or is not visible", (unsigned)tid));
			if (needsPos && !needsObject)
			{
				// location power aimed at an object: fire at its position
				GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
				msg->appendIntegerArgument(sp->getID());
				msg->appendLocationArgument(*target->getPosition());
				msg->appendRealArgument(INVALID_ANGLE);
				msg->appendObjectIDArgument(tid);
				msg->appendIntegerArgument(options);
				msg->appendObjectIDArgument(source->getID());
			}
			else
			{
				GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT);
				msg->appendIntegerArgument(sp->getID());
				msg->appendObjectIDArgument(tid);
				msg->appendIntegerArgument(options);
				msg->appendObjectIDArgument(source->getID());
			}
		}
		else if (hasArg(args, "x") || hasArg(args, "y"))
		{
			if (needsObject)
				throw CmdError("BAD_ARGS", strf("%s needs target_id", name.c_str()));
			double x = argReal(args, "x"), y = argReal(args, "y");
			requireOnMap(x, y);
			GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION);
			msg->appendIntegerArgument(sp->getID());
			msg->appendLocationArgument(groundPos(x, y));
			msg->appendRealArgument(INVALID_ANGLE);
			msg->appendObjectIDArgument(INVALID_ID);
			msg->appendIntegerArgument(options);
			msg->appendObjectIDArgument(source->getID());
		}
		else
		{
			if (needsObject)
				throw CmdError("BAD_ARGS", strf("%s needs target_id", name.c_str()));
			if (needsPos)
				throw CmdError("BAD_ARGS", strf("%s needs x and y", name.c_str()));
			GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_DO_SPECIAL_POWER);
			msg->appendIntegerArgument(sp->getID());
			msg->appendIntegerArgument(options);
			msg->appendObjectIDArgument(source->getID());
		}
		Json res = orderResult(source->getID());
		res["source_id"] = (unsigned)source->getID();
		return res;
	}

	Json cmdSell(const Json& args)
	{
		requireCanIssueOrders();
		if (!hasArg(args, "building_id"))
			throw CmdError("BAD_ARGS", "building_id is required");
		Object* b = requireOwnObject(args["building_id"], "building");
		if (!b->isKindOf(KINDOF_STRUCTURE))
			throw CmdError("CANNOT_DO", "only structures can be sold");
		if (b->testStatus(OBJECT_STATUS_SOLD))
			throw CmdError("CANNOT_DO", "already being sold");
		appendSelect(b->getID());
		TheMessageStream->appendMessage(GameMessage::MSG_SELL);
		return orderResult(b->getID());
	}

	Json cmdSetRallyPoint(const Json& args)
	{
		requireCanIssueOrders();
		if (!hasArg(args, "building_id"))
			throw CmdError("BAD_ARGS", "building_id is required");
		Object* b = requireOwnObject(args["building_id"], "building");
		double x = argReal(args, "x"), y = argReal(args, "y");
		requireOnMap(x, y);
		if (b->getObjectExitInterface() == nullptr)
			throw CmdError("CANNOT_DO", strf("%s has no rally point", b->getTemplate()->getName().str()));
		GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_SET_RALLY_POINT);
		msg->appendObjectIDArgument(b->getID());
		msg->appendLocationArgument(groundPos(x, y));
		return orderResult(b->getID());
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
		{ "move", cmdMove },
		{ "attack_move", cmdAttackMove },
		{ "attack", cmdAttack },
		{ "force_attack_ground", cmdForceAttackGround },
		{ "stop", cmdStop },
		{ "guard", cmdGuard },
		{ "build_structure", cmdBuildStructure },
		{ "train_unit", cmdTrainUnit },
		{ "cancel_train", cmdCancelTrain },
		{ "research_upgrade", cmdResearchUpgrade },
		{ "purchase_science", cmdPurchaseScience },
		{ "fire_special_power", cmdFireSpecialPower },
		{ "sell", cmdSell },
		{ "set_rally_point", cmdSetRallyPoint },
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
