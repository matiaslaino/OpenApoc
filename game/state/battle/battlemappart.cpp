#include "game/state/battle/battlemappart.h"
#include "game/state/battle/battle.h"
#include "game/state/battle/battledoor.h"
#include "game/state/battle/battleitem.h"
#include "game/state/battle/battlemappart_type.h"
#include "library/strings_format.h"
#include "game/state/city/projectile.h"
#include "game/state/gamestate.h"
#include "game/state/rules/damage.h"
#include "game/state/tileview/collision.h"
#include "game/state/tileview/tile.h"
#include "game/state/tileview/tileobject_battlemappart.h"
#include "game/state/tileview/tileobject_battleitem.h"
#include <algorithm>
#include <set>

namespace OpenApoc
{

void BattleMapPart::die(GameState &state, bool violently)
{
	if (violently)
	{
		// FIXME: Explode if nessecary
	}

	// If falling just cease to be, do damage
	if (falling)
	{
		this->tileObject->removeFromMap();
		this->tileObject.reset();
		destroyed = true;

		LogWarning("Deal falling damage to units!");
		return;
	}

	// Doodad
	auto doodad = state.current_battle->placeDoodad({ &state, "DOODAD_29_EXPLODING_TERRAIN" },
		tileObject->getCenter());

	// Replace with damaged / destroyed
	if (type->damaged_map_part)
	{
		this->type = type->damaged_map_part;
		if (findSupport())
		{
			this->damaged = true;
		}
		else
		{
			queueCollapse();
		}
	}
	else
	{
		// Don't destroy bottom tiles, else everything will leak out
		// Replace ground with destroyed
		if (this->position.z == 0 && this->type->type == BattleMapPartType::Type::Ground)
		{
			this->type = type->destroyed_ground_tile;
		}
		// Destroy map part
		else
		{
			destroyed = true;
		}
	}
	
	// Cease functioning
	ceaseDoorFunction();
	ceaseSupportProvision();

	// Destroy if destroyed
	if (destroyed)
	{
		this->tileObject->removeFromMap();
		this->tileObject.reset();
	}
}


int BattleMapPart::getMaxFrames()
{
	return alternative_type ? (int)alternative_type->animation_frames.size()
	                        : (int)type->animation_frames.size();
}

int BattleMapPart::getAnimationFrame()
{
	if (door)
	{
		return std::min(getMaxFrames() - 1, door->getAnimationFrame());
	}
	else
	{
		return type->animation_frames.size() == 0
		           ? -1
		           : animation_frame_ticks / TICKS_PER_FRAME_MAP_PART;
	}
}

bool BattleMapPart::handleCollision(GameState &state, Collision &c)
{
	if (!this->tileObject)
	{
		// It's possible multiple projectiles hit the same tile in the same
		// tick, so if the object has already been destroyed just NOP this.
		// The projectile will still 'hit' this tile though.
		return false;
	}
	if (this->falling)
	{
		// Already falling, just continue
		return false;
	}

	// Calculate damage (hmm, apparently Apoc uses 50-150 damage model for terrain, unlike UFO1&2
	// which used 25-75
	int damage = randDamage050150(state.rng, c.projectile->damageType->dealDamage(
	                                             c.projectile->damage, type->damageModifier));
	if (damage <= type->constitution)
	{
		return false;
	}

	// If we came this far, map part has been damaged and must cease to be
	die(state);
	return false;
}

void BattleMapPart::ceaseDoorFunction()
{
	if (!door)
	{
		return;
	}

	if (alternative_type)
		type = alternative_type;
	// Remove from door's map parts
	wp<BattleMapPart> sft = shared_from_this();
	door->mapParts.remove_if([sft](wp<BattleMapPart> p) {
		auto swp = sft.lock();
		auto sp = p.lock();
		if (swp && sp)
			return swp == sp;
		return false;
	});
	door.clear();
}

bool BattleMapPart::attachToSomething(bool checkType)
{
	auto pos = tileObject->getOwningTile()->position;
	auto &map = tileObject->map;
	auto tileType = tileObject->getType();
	auto sft = shared_from_this();

	// List of directions (for ground and feature)
	static const std::list<Vec3<int>> directionGDFTList =
	{
		{ 0, 0, -1 },
		{ 0, -1, 0 },
		{ 1, 0, 0 },
		{ 0, 1, 0 },
		{ -1, 0, 0 },
		{ 0, 0, 1 },
	};

	// List of directions for left wall
	static const std::list<Vec3<int>> directionLWList =
	{
		{ 0, 0, -1 },
		{ 0, -1, 0 },
		{ 0, 1, 0 },
		{ 0, 0, 1 },
	};

	// List of directions for right wall
	static const std::list<Vec3<int>> directionRWList =
	{
		{ 0, 0, -1 },
		{ 1, 0, 0 },
		{ -1, 0, 0 },
		{ 0, 0, 1 },
	};

	auto &directionList = tileType == TileObject::Type::LeftWall
		? directionLWList
		: (tileType == TileObject::Type::RightWall
			? directionRWList
			: directionGDFTList);

	// Search for map parts
	for (auto &dir : directionList)
	{
		int x = pos.x + dir.x;
		int y = pos.y + dir.y;
		int z = pos.z + dir.z;
		if (x < 0 || x >= map.size.x
			|| y < 0 || y >= map.size.y
			|| z < 0 || z >= map.size.z)
		{
			continue;
		}
		auto tile = map.getTile(x, y, z);
		for (auto &o : tile->ownedObjects)
		{
			// Even if we're not doing type checking, we cannot allow for walls to cling to other type of walls
			if (o->getType() == tileType || (!checkType 
				&& (o->getType() == TileObject::Type::Ground
				|| o->getType() == TileObject::Type::Feature
				|| (o->getType() == TileObject::Type::LeftWall && tileType != TileObject::Type::RightWall)
				|| (o->getType() == TileObject::Type::RightWall && tileType != TileObject::Type::LeftWall))))
			{
				auto mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
				if (mp != sft && mp->isAlive())
				{
					bool canSupport = !mp->damaged
						&& (mp->type->type != BattleMapPartType::Type::Ground || z == pos.z)
						&& (mp->type->provides_support || z <= pos.z);
					if (canSupport)
					{
						mp->supportedParts.emplace_back(position, type->type);
						return true;
					}
				}
			}
		}
	}
	return false;
}

bool BattleMapPart::findSupport()
{
	// Initial setup and quick checks
	providesHardSupport = true;
	if (type->floating)
	{
		return true;
	}
	auto pos = tileObject->getOwningTile()->position;
	if (pos.z == 0)
	{
		return true;
	}
	auto &map = tileObject->map;
	auto tileType = tileObject->getType();
	auto sft = shared_from_this();

	// Clean support providers for this map part
	for (int x = pos.x - 1; x <= pos.x + 1; x++)
	{
		for (int y = pos.y - 1; y <= pos.y + 1; y++)
		{
			for (int z = pos.z - 1; z <= pos.z + 1; z++)
			{
				if (x < 0 || x >= map.size.x
					|| y < 0 || y >= map.size.y
					|| z < 0 || z >= map.size.z)
				{
					continue;
				}
				auto tile = map.getTile(x, y, z);
				for (auto &o : tile->ownedObjects)
				{
					if (o->getType() == TileObject::Type::Ground
						|| o->getType() == TileObject::Type::Feature
						|| o->getType() == TileObject::Type::LeftWall
						|| o->getType() == TileObject::Type::RightWall)
					{
						auto mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
						auto it = mp->supportedParts.begin();
						while (it!=mp->supportedParts.end())
						{
							auto &p = *it;
							if (p.first == pos && p.second == type->type)
							{
								it = supportedParts.erase(it);
							}
							else
							{
								it++;
							}
						}
					}
				}
			}
		}
	}

	// There are several ways battle map part can get supported:
	//
	// (following conditions provide "hard" support)
	//
	// Ground:
	//  - G1: Feature Current/Below
	//  - G2: Wall Adjacent Below
	//  - G3: Feature Adjacent Below
	//
	// Feature:
	//  - F1: Ground Current
	//  - F2: Feature Below
	//  - F3: Feature Above (if "Above" supported by) 
	//
	// Wall:
	//  - W2: Feature Current
	//  - W2: Ground Adjacent Current
	//  - W3: Feature Adjacent Below
	//  - W4: Wall Below
	//  - W5: Wall Above (if "Above" supported by) 
	// 
	// Then, there is a specified "Supported By Direction" condition:
	//  - Ground will get support from a Ground only
	//  - Feature will get support from a Feature or a matching perpendicular Wall
	//    (Right if N/S, Left if E/W)
	//  - Wall will get support from the same type of Wall
	//	  (provided the Wall's type matches direction: Left for N/S, Right for E/W)
	//
	// If "Supported By Type: is also specified, then:
	//  - Ground/Wall allows support from Ground/Wall on a current level
	//  - Feature allows support from Feature on a level below  
	//
	// (following conditions provide "soft" support)
	//
	// Then, object with no direction specified can cling to two adjacent objects:
	//  - Ground and Feature can cling to objects of the same type
	//  - Walls can cling to walls of their type or a Feature
	//
	// Finally, every map part can be supported if it has established support lines
	// on both sides that connect to an object providing "hard" support
	//  - Object "shoots" a line in both directions and as long as there is an object on every tile
	//    the line continues, and if an object providing hard support is reached, 
	//	  then "soft" support can be attained
	//
	// Implementation:
	//  - We will first check for special conditions
	//  - Then we will gather information about adjacent map parts and check for "Supported By" 
	//  - Finally we will try to cling to two objects of the same type
	//  - If all fails, we will scan on axes in search of distant support
	
	// Step 01: Check for special conditions
	
	// Bounds to check for special conditions in
	int startX = pos.x - 1;
	int endX = pos.x + 1;
	int startY = pos.y - 1;
	int endY = pos.y + 1;
	int startZ = pos.z - 1;
	int endZ = pos.z + 1;
	switch (type->type)
	{
		case BattleMapPartType::Type::Ground:
			// We're only interested in tiles current or below
			endZ = pos.z;
			break;
		case BattleMapPartType::Type::Feature:
			// We're only interested in tiles with matching x and y
			startX = pos.x;
			endX = startX;
			startY = pos.y;
			endY = startY;
			break;
		case BattleMapPartType::Type::LeftWall:
			// We're only interested in tiles above/below and to the west
			endX = pos.x;
			startY = pos.y;
			endY = startY;
			break;
		case BattleMapPartType::Type::RightWall:
			// We're only interested in tiles above/below and to the north
			endY = pos.y;
			startX = pos.x;
			endX = startX;
			break;
	}
	// Do the check
	for (int x = startX; x <= endX; x++)
	{
		for (int y = startY; y <= endY; y++)
		{
			for (int z = startZ; z <= endZ; z++)
			{
				if (x < 0 || x >= map.size.x
					|| y < 0 || y >= map.size.y
					|| z < 0 || z >= map.size.z)
				{
					continue;
				}
				auto tile = map.getTile(x, y, z);
				for (auto &o : tile->ownedObjects)
				{
					bool canSupport = false;
					
					switch (type->type)
					{
					case BattleMapPartType::Type::Ground:
						//  - G1: Feature Current/Below
						canSupport = canSupport || (x == pos.x && y == pos.y && o->getType() == TileObject::Type::Feature);
						//  - G2: Wall Adjacent Below
						if ((x >= pos.x || y >= pos.y) && z < pos.z)
						{
							canSupport = canSupport || (x >= pos.x && o->getType() == TileObject::Type::LeftWall);
							canSupport = canSupport || (y >= pos.y && o->getType() == TileObject::Type::RightWall);
						}
						//  - G3: Feature Adjacent Below
						if ((x == pos.x || y == pos.y) && z < pos.z)
						{
							canSupport = canSupport || (o->getType() == TileObject::Type::Feature);
						}
						break;
					case BattleMapPartType::Type::Feature:
						//  - F1: Ground Current
						canSupport = canSupport || (z == pos.z && o->getType() == TileObject::Type::Ground);
						//  - F2: Feature Below
						canSupport = canSupport || (z < pos.z && o->getType() == TileObject::Type::Feature);
						//  - F3: Feature Above (if "Above" supported by) 
						canSupport = canSupport || (z > pos.z && o->getType() == TileObject::Type::Feature 
							&& type->supportedByAbove);
						break;
					case BattleMapPartType::Type::LeftWall:
						//  - W1: Feature Current
						canSupport = canSupport || (x == pos.x && y == pos.y && z == pos.z && o->getType() == TileObject::Type::Feature);
						//  - W2: Ground Adjacent Current
						canSupport = canSupport || (z == pos.z && o->getType() == TileObject::Type::Ground);
						//  - W3: Feature Adjacent Below
						canSupport = canSupport || (z < pos.z && o->getType() == TileObject::Type::Feature);
						//  - W4: Wall Below
						canSupport = canSupport || (x == pos.x && z < pos.z 
							&& o->getType() == TileObject::Type::LeftWall);
						//  - W5: Wall Above (if "Above" supported by) 
						canSupport = canSupport || (x == pos.x && z > pos.z 
							&& o->getType() == TileObject::Type::LeftWall
							&& type->supportedByAbove);
						break;
					case BattleMapPartType::Type::RightWall:
						//  - W1: Feature Current
						canSupport = canSupport || (x == pos.x && y == pos.y && z == pos.z && o->getType() == TileObject::Type::Feature);
						//  - W2: Ground Adjacent Current
						canSupport = canSupport || (z == pos.z && o->getType() == TileObject::Type::Ground);
						//  - W3: Feature Adjacent Below
						canSupport = canSupport || (z < pos.z && o->getType() == TileObject::Type::Feature);
						//  - W4: Wall Below
						canSupport = canSupport || (y == pos.y && z < pos.z 
							&& o->getType() == TileObject::Type::RightWall);
						//  - W5: Wall Above (if "Above" supported by) 
						canSupport = canSupport || (y == pos.y && z > pos.z 
							&& o->getType() == TileObject::Type::RightWall
							&& type->supportedByAbove);
						break;
					}
					
					if (canSupport)
					{
						auto mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
						// Seems that "provide support" flag only matters for providing support upwards
						if (mp != sft && mp->isAlive() && !mp->damaged && (mp->type->provides_support || mp->type->type == BattleMapPartType::Type::Ground || z <= pos.z))
						{
							mp->supportedParts.emplace_back(position, type->type);
							return true;
						}
					}
				}
			}
		}
	}

	// Then, there is a specified "Supported By Direction" condition:
	//  - Ground will get support from a Ground only
	//  - Feature will get support from a Feature or a matching perpendicular Wall
	//    (Right if N/S, Left if E/W)
	//  - Wall will get support from the same type of Wall
	//	  (provided the Wall's type matches direction: Left for N/S, Right for E/W)
	//
	// If "Supported By Type: is also specified, then:
	//  - Ground/Wall allows support from Ground/Wall on a current level
	//  - Feature allows support from Feature on a level below  
	//

	// Step 02: Check "supported by direction" condition
	if (!type->supportedByDirections.empty())
	{
		// List of types to search for and locations where
		std::list<std::pair<Vec3<int>, TileObject::Type>> partList;
		// List for types to search for and z-where
		std::list<std::pair<int, TileObject::Type>> typeList;
		// Every type is supported by it's type on the same level
		typeList.emplace_back(0, tileType);
		// Add other supported by types
		for (auto t : type->supportedByTypes)
		{
			switch (t)
			{
				case BattleMapPartType::Type::Ground:
					typeList.emplace_back(0, TileObject::Type::Ground);
					break;
				case BattleMapPartType::Type::LeftWall:
					typeList.emplace_back(0, TileObject::Type::LeftWall);
					break;
				case BattleMapPartType::Type::RightWall:
					typeList.emplace_back(0, TileObject::Type::RightWall);
					break;
				case BattleMapPartType::Type::Feature:
					typeList.emplace_back(-1, TileObject::Type::Feature);
					break;
			
			}
		}
		// Fill parts list based on direction
		for (auto d : type->supportedByDirections)
		{
			for (auto p : typeList)
			{
				// Feature to feature on the same level also allows for a matching wall
				if (type->type == BattleMapPartType::Type::Feature && p.first == 0 && p.second == TileObject::Type::Feature)
				{
					switch (d)
					{
					case MapDirection::North:
						partList.emplace_back(Vec3<int>{ pos.x, pos.y, pos.z + p.first}, TileObject::Type::RightWall);
						break;
					case MapDirection::East:
						partList.emplace_back(Vec3<int>{ pos.x + 1, pos.y, pos.z + p.first}, TileObject::Type::LeftWall);
						break;
					case MapDirection::South:
						partList.emplace_back(Vec3<int>{ pos.x, pos.y + 1, pos.z + p.first}, TileObject::Type::RightWall);
						break;
					case MapDirection::West:
						partList.emplace_back(Vec3<int>{ pos.x, pos.y, pos.z + p.first}, TileObject::Type::LeftWall);
						break;
					}
				}

				// Going N/S for Right Wall or E/W for Left Wall is impossible for same type walls
				if ((p.second == TileObject::Type::RightWall && tileType == TileObject::Type::RightWall
					&& (d == MapDirection::North || d == MapDirection::South))
					||(p.second == TileObject::Type::LeftWall && tileType == TileObject::Type::LeftWall
					&& (d == MapDirection::East || d == MapDirection::West)))
				{
					continue;
				}
				// Going North into Right Wall and West into Left Wall means checking our own tile
				// (South for Right and East for Left is fine))
				int negInc = -1;
				if ((d == MapDirection::North && p.second == TileObject::Type::RightWall)
					|| (d == MapDirection::West && p.second == TileObject::Type::LeftWall))
				{
					negInc = 0;
				}

				// Get part in this direction
				int dx = 0;
				int dy = 0;
				switch (d)
				{
					case MapDirection::North:
						dy = negInc;
						break;
					case MapDirection::East:
						dx = 1;
						break;
					case MapDirection::South:
						dy = 1;
						break;
					case MapDirection::West:
						dx = negInc;
						break;
				}
				partList.emplace_back(Vec3<int>{ pos.x + dx, pos.y + dy, pos.z + p.first }, p.second);
					
				// Get diagonal directions
				for (auto d2 : type->supportedByDirections)
				{
					if (d2 == d || p.second == TileObject::Type::LeftWall || p.second == TileObject::Type::RightWall)
					{
						continue;
					}
					switch (d)
					{
					case MapDirection::North:
					case MapDirection::South:
						switch (d)
						{
						case MapDirection::East:
							dx = 1;
							break;
						case MapDirection::West:
							dx = -1;
							break;
						case MapDirection::North:
						case MapDirection::South:
							continue;
						}
						break;
					case MapDirection::East:
					case MapDirection::West:
						switch (d)
						{
						case MapDirection::North:
							dy = -1;
							break;
						case MapDirection::South:
							dy = 1;
							break;
						case MapDirection::East:
						case MapDirection::West:
							continue;
						}
						break;
					}
					partList.emplace_back(Vec3<int>{ pos.x + dx, pos.y + dy, pos.z + p.first }, p.second);
				}
			}
		}
		// Look for parts
		for (auto pair : partList)
		{
			if (pair.first.x < 0 || pair.first.x >= map.size.x
				|| pair.first.y < 0 || pair.first.y >= map.size.y
				|| pair.first.z < 0 || pair.first.z >= map.size.z)
			{
				continue;
			}
			auto tile = map.getTile(pair.first.x, pair.first.y, pair.first.z);
			for (auto &o : tile->ownedObjects)
			{
				// Matching battle map parts that fit the criteria of axis differences
				// Also must provide support
				if (o->getType() == pair.second)
				{
					auto mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
					if (mp != sft && mp->isAlive())
					{
						bool canSupport = !mp->damaged 
							&& (mp->type->type != BattleMapPartType::Type::Ground || pair.first.z == pos.z) 
							&& (mp->type->provides_support || pair.first.z <= pos.z);
						if (canSupport)
						{
							mp->supportedParts.emplace_back(position, type->type);
							return true;
						}
					}
				}
			}
		}
	}
	
	// If we reached this - we can not provide hard support
	providesHardSupport = false;

	// Step 03: Try to cling to two adjacent objects of the same type
	// (wall can also cling to feature)

	// List of four directions (for ground and feature)
	static const std::list<Vec3<int>> directionGDFTList =
	{
		{ 0, -1, 0 },
		{ 1, 0, 0 },
		{ 0, 1, 0 },
		{ -1, 0, 0 },
	};

	// List of directions for left wall
	static const std::list<Vec3<int>> directionLWList =
	{
		{ 0, -1, 0 },
		{ 0, 1, 0 },
	};

	// List of directions for right wall
	static const std::list<Vec3<int>> directionRWList =
	{
		{ 1, 0, 0 },
		{ -1, 0, 0 },
	};

	auto &directionList = tileType == TileObject::Type::LeftWall 
		? directionLWList 
		: (tileType == TileObject::Type::RightWall
			? directionRWList 
			: directionGDFTList);

	// List of found map parts to cling on to
	std::list<sp<BattleMapPart>> supports;
	// Search for map parts
	for (auto &dir : directionList)
	{
		int x = pos.x + dir.x;
		int y = pos.y + dir.y;
		int z = pos.z + dir.z;
		if (x < 0 || x >= map.size.x
			|| y < 0 || y >= map.size.y
			|| z < 0 || z >= map.size.z)
		{
			continue;
		}
		auto tile = map.getTile(x, y, z);
		for (auto &o : tile->ownedObjects)
		{
			if (o->getType() == tileType 
				||( o->getType() == TileObject::Type::Feature 
					&& (tileType == TileObject::Type::LeftWall 
						|| tileType == TileObject::Type::RightWall)))
			{
				auto mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
				if (mp != sft && mp->isAlive())
				{
					bool canSupport = !mp->damaged
						&& (mp->type->type != BattleMapPartType::Type::Ground || z == pos.z)
						&& (mp->type->provides_support || z <= pos.z);
					if (canSupport)
					{
						supports.emplace_back(mp);
						// No need to further look in this area
						break;
					}
				}
			}
		}
	}
	// Calculate if we have enough supports (map edge counts as support)
	auto supportCount = supports.size();
	if (pos.x == 0 || pos.x == map.size.x - 1)
	{
		supportCount++;
	}
	if (pos.y == 0 || pos.y == map.size.y - 1)
	{
		supportCount++;
	}
	// Get support if we have enough
	if (supportCount >= 2)
	{
		for (auto mp : supports)
		{
			mp->supportedParts.emplace_back(position, type->type);
		}
		return true;
	}

	// Step 04: Shoot "support lines" and try to find something

	// Scan on X
	if (type->type != BattleMapPartType::Type::LeftWall)
	{
		int y = pos.y;
		int z = pos.z;
		
		bool found;
		for (int increment = -1; increment <= 1; increment += 2)
		{
			found = false;
			int x = pos.x + increment;
			do
			{
				if (x < 0 || x >= map.size.x)
				{
					found = true;
					break;
				}
				sp<BattleMapPart> mp = nullptr;
				auto tile = map.getTile(x, y, z);
				for (auto &o : tile->ownedObjects)
				{
					if (o->getType() == tileType)
					{
						mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
					}
				}
				// Could not find map part of this type or it cannot provide support
				// We ignore those that have positive "ticksUntilFalling" as those can be saved yet
				if (!mp || mp->destroyed || mp->damaged || mp->falling)
				{
					// fail
					break;
				}
				// Found map part that provides hard support
				if (mp->providesHardSupport)
				{
					// success
					found = true;
					break;
				}
				// continue
				x += increment;
			} while (true);
			if (!found)
			{
				break;
			}
		}
		// If found both ways - cling to neighbours on X
		if (found)
		{
			for (int increment = -1; increment <= 1; increment += 2)
			{
				int x = pos.x + increment;
				if (x < 0 || x >= map.size.x)
				{
					continue;
				}
				sp<BattleMapPart> mp = nullptr;
				auto tile = map.getTile(x, y, z);
				for (auto &o : tile->ownedObjects)
				{
					if (o->getType() == tileType)
					{
						mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
					}
				}
				if (!mp)
				{
					LogError("Map part disappeared? %d %d %d", x, y, z);
					return false;
				}
				mp->supportedParts.emplace_back(position, type->type);
			}
			return true;
		}
	}

	// Scan on Y
	if (type->type != BattleMapPartType::Type::RightWall)
	{
		int x = pos.x;
		int z = pos.z;

		bool found;
		for (int increment = -1; increment <= 1; increment += 2)
		{
			found = false;
			int y = pos.y + increment;
			do
			{
				if (y < 0 || y >= map.size.y)
				{
					found = true;
					break;
				}
				sp<BattleMapPart> mp = nullptr;
				auto tile = map.getTile(x, y, z);
				for (auto &o : tile->ownedObjects)
				{
					if (o->getType() == tileType)
					{
						mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
					}
				}
				// Could not find map part of this type or it cannot provide support
				// We ignore those that have positive "ticksUntilFalling" as those can be saved yet
				if (!mp || mp->destroyed || mp->damaged || mp->falling)
				{
					// fail
					break;
				}
				// Found map part that provides hard support
				if (mp->providesHardSupport)
				{
					// success
					found = true;
					break;
				}
				// continue
				y += increment;
			} while (true);
			if (!found)
			{
				break;
			}
		}
		// If found both ways - cling to neighbours on Y
		if (found)
		{
			for (int increment = -1; increment <= 1; increment += 2)
			{
				int y = pos.y + increment;
				if (y < 0 || y >= map.size.y)
				{
					continue;
				}
				sp<BattleMapPart> mp = nullptr;
				auto tile = map.getTile(x, y, z);
				for (auto &o : tile->ownedObjects)
				{
					if (o->getType() == tileType)
					{
						mp = std::static_pointer_cast<TileObjectBattleMapPart>(o)->getOwner();
					}
				}
				if (!mp)
				{
					LogError("Map part disappeared? %d %d %d", x, y, z);
					return false;
				}
				mp->supportedParts.emplace_back(position, type->type);
			}
			return true;
		}
	}

	return false;
}

sp<std::set<BattleMapPart*>> BattleMapPart::getSupportedParts()
{
	sp<std::set<BattleMapPart*>> collapseList = mksp<std::set<BattleMapPart*>>();
	auto &map = tileObject->map;
	for (auto &p : this->supportedParts)
	{
		auto tile = map.getTile(p.first);
		for (auto obj : tile->ownedObjects)
		{
			if (obj->getType() == TileObjectBattleMapPart::convertType(p.second))
			{
				auto mp = std::static_pointer_cast<TileObjectBattleMapPart>(obj)->getOwner();
				collapseList->insert(mp.get());
			}
		}
	}
	return collapseList;
}

void BattleMapPart::ceaseSupportProvision()
{
	providesHardSupport = false;
	attemptReLinkSupports(getSupportedParts());
	supportedParts.clear();
	if (supportedItems)
	{
		for (auto obj : this->tileObject->getOwningTile()->ownedObjects)
		{
			if (obj->getType() == TileObject::Type::Item)
			{
				std::static_pointer_cast<TileObjectBattleItem>(obj)->getItem()->tryCollapse();
			}
		}
		supportedItems = false;
	}
}

void BattleMapPart::attemptReLinkSupports(sp<std::set<BattleMapPart*>> set)
{
	if (set->empty())
	{
		return;
	}

	UString log = "ATTEMPTING RE-LINK OF SUPPORTS";

	// First mark all those in list as about to fall
	for (auto mp : *set)
	{
		mp->queueCollapse();
	}

	// Then try to re-establish support links
	bool listChanged;
	do
	{
		LogWarning("%s", log.cStr());
		log = "";
		log += format("\nIteration begins. List contains %d items:", (int)set->size());
		for (auto mp : *set)
		{
			auto pos = mp->tileObject->getOwningTile()->position;
			log += format("\n %s at %d %d %d", mp->type.id, pos.x, pos.y, pos.z);
		}
		log += format("\n");

		auto nextSet = mksp<std::set<BattleMapPart*>>();
		listChanged = false;
		for (auto mp : *set)
		{
			auto supportedByThisMp = mp->getSupportedParts();
			for (auto newmp : *supportedByThisMp)
			{
				newmp->queueCollapse(mp->ticksUntilCollapse);
				newmp->providesHardSupport = false;
			}
			auto pos = mp->tileObject->getOwningTile()->position;
			if (mp->findSupport())
			{
				log += format("\n Processing %s at %d %d %d: OK %s", mp->type.id, pos.x, pos.y, pos.z, mp->providesHardSupport ? "HARD" : "SOFT");
				{
					auto t = pos;
					auto &map = mp->tileObject->map;
					for (int x = t.x - 1; x <= t.x + 1; x++)
					{
						for (int y = t.y - 1; y <= t.y + 1; y++)
						{
							for (int z = t.z - 1; z <= t.z + 1; z++)
							{
								if (x < 0 || x >= map.size.x
									|| y < 0 || y >= map.size.y
									|| z < 0 || z >= map.size.z)
								{
									continue;
								}
								auto tile2 = map.getTile(x, y, z);
								for (auto &o2 : tile2->ownedObjects)
								{
									if (o2->getType() == TileObject::Type::Ground
										|| o2->getType() == TileObject::Type::Feature
										|| o2->getType() == TileObject::Type::LeftWall
										|| o2->getType() == TileObject::Type::RightWall)
									{
										auto mp2 = std::static_pointer_cast<TileObjectBattleMapPart>(o2)->getOwner();
										for (auto &p : mp2->supportedParts)
										{
											if (p.first == t && p.second == mp->type->type)
											{
												log += format("\n - Supported by %s at %d %d %d", mp2->type.id, x - t.x, y - t.y, z - t.z);
											}
										}
									}
								}
							}
						}
					}

				}
				mp->cancelCollapse();
				for (auto newmp : *supportedByThisMp)
				{
					newmp->cancelCollapse();
				}
				listChanged = true;
			}
			else
			{
				log += format("\n Processing %s at %d %d %d: FAIL, remains in next iteration", mp->type.id, pos.x, pos.y, pos.z);
				nextSet->insert(mp);
				mp->supportedParts.clear();
				for (auto newmp : *supportedByThisMp)
				{
					auto newpos = newmp->tileObject->getOwningTile()->position;
					log += format("\n - %s at %d %d %d added to next iteration", newmp->type.id, newpos.x, newpos.y, newpos.z);
					nextSet->insert(newmp);
					listChanged = true;
				}
			}
		} 
		log += format("\n");
		set = nextSet;
	} while (listChanged);

	LogWarning("%s", log.cStr());

	// At this point only those that should fall are left
	// They will fall when their time comes
	for (auto mp : *set)
	{
		auto pos = mp->tileObject->getOwningTile()->position;
		LogWarning("MP %s SBT %d at %d %d %d is going to fall", mp->type.id.cStr(), (int)mp->type->getVanillaSupportedById(), pos.x, pos.y, pos.z);
	}
}

void BattleMapPart::collapse()
{
	// If it's already falling or destroyed or supported do nothing
	if (falling || !tileObject)
	{
		return;
	}
	falling = true;
	ceaseSupportProvision();
	ceaseDoorFunction();
}

void BattleMapPart::update(GameState &state, unsigned int ticks)
{
	if (!tileObject)
	{
		return;
	}

	if (ticksUntilCollapse > 0)
	{
		if (ticksUntilCollapse > ticks)
		{
			ticksUntilCollapse -= ticks;
		}
		else
		{
			ticksUntilCollapse = 0;
			collapse();
		}
	}

	// Process falling
	if (falling)
	{
		auto fallTicksRemaining = ticks;
		auto newPosition = position;
		while (fallTicksRemaining-- > 0)
		{
			fallingSpeed += FALLING_ACCELERATION_MAP_PART;
			newPosition -= Vec3<float>{0.0f, 0.0f, (fallingSpeed / TICK_SCALE)} /
				VELOCITY_SCALE_BATTLE;
		}
		
		// Collision with this tile happens when map part moves from this tile to the next
		if (newPosition.z < 0 || floorf(newPosition.z) != floorf(position.z))
		{
			sp<BattleMapPart> rubble;
			for (auto &obj : tileObject->getOwningTile()->ownedObjects)
			{
				switch (obj->getType())
				{
					// If there's a live ground or map mart of our type here - die
					case TileObject::Type::Ground:
					case TileObject::Type::LeftWall:
					case TileObject::Type::RightWall:
					case TileObject::Type::Feature:
					{
						auto mp = std::static_pointer_cast<TileObjectBattleMapPart>(obj)->getOwner();
						
						// Find if we collide into it
						if (mp->type->type == type->type || mp->type->type == BattleMapPartType::Type::Ground)
						{
							if (tileObject && mp->isAlive())
							{
								destroyed = true;
							}
						}
						
						// Find if we deposit rubble into it
						if ((type->type != BattleMapPartType::Type::Ground && mp->type->type == type->type)
							|| (type->type == BattleMapPartType::Type::Ground && mp->type->type == BattleMapPartType::Type::Feature))
						{
							if (mp->isAlive())
							{
								rubble = mp;
							}
						}

						break;
					}
					default:
						// Ignore other object types?
						break;
				}
			}

			if (destroyed)
			{
				if (!type->rubble.empty())
				{
					if (!rubble)
					{
						// If no rubble present - spawn rubble
						auto rubble = mksp<BattleMapPart>();
						Vec3<int> initialPosition = position;
						rubble->position = initialPosition;
						rubble->position += Vec3<float>(0.5f, 0.5f, 0.0f);
						rubble->type = type->rubble.front();
						state.current_battle->map_parts.push_back(rubble);
						state.current_battle->map->addObjectToMap(rubble);
						LogWarning("Implement smoke when rubble falls");
					}
					else
					{
						// If rubble present - increment it if possible
						auto it = std::find(type->rubble.begin(), type->rubble.end(), rubble->type);
						if (it != type->rubble.end() && ++it != type->rubble.end())
						{
							rubble->type = *it;
							rubble->setPosition(rubble->position);
							LogWarning("Implement smoke when rubble falls");
						}
					}
				}

				die(state);
				return;
			}
		}

		setPosition(newPosition);
		return;
	}

	// Animate non-doors
	if (!door && type->animation_frames.size() > 0)
	{
		animation_frame_ticks += ticks;
		animation_frame_ticks %= TICKS_PER_FRAME_MAP_PART * type->animation_frames.size();
	}
}

void BattleMapPart::setPosition(const Vec3<float> &pos)
{
	this->position = pos;
	if (!this->tileObject)
	{
		LogError("setPosition called on map part with no tile object");
		return;
	}
	else
	{
		this->tileObject->setPosition(pos);
	}
		
}

bool BattleMapPart::isAlive() const
{
	if (falling || destroyed || willCollapse())
		return false;
	return true;
}

void BattleMapPart::queueCollapse(unsigned additionalDelay)
{
	ticksUntilCollapse = TICKS_MULTIPLIER + additionalDelay;
	providesHardSupport = false;
}

void BattleMapPart::cancelCollapse()
{
	ticksUntilCollapse = 0;
}
}
