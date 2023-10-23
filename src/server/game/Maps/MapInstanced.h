/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ACORE_MAP_INSTANCED_H
#define ACORE_MAP_INSTANCED_H

#include "DBCEnums.h"
#include "InstanceSaveMgr.h"
#include "Map.h"

class MapInstanced : public Map
{
    friend class MapMgr;
public:
    using InstancedMaps = std::unordered_map<uint32, Map*>;

    MapInstanced(uint32 id, time_t expiry);
    ~MapInstanced() override {}

    // functions overwrite Map versions
    void Update(const uint32, const uint32, bool thread = true) override;
    void DelayedUpdate(const uint32 diff) override;
    //void RelocationNotify();
    void UnloadAll() override;
    EnterState CannotEnter(Player* player, bool loginCheck = false) override;

    Map* CreateInstanceForPlayer(const uint32 mapId, Player* player);
    Map* FindInstanceMap(uint32 instanceId) const
    {
        InstancedMaps::const_iterator i = m_InstancedMaps.find(instanceId);
        return(i == m_InstancedMaps.end() ? nullptr : i->second);
    }
    bool DestroyInstance(InstancedMaps::iterator& itr);

    void AddGridMapReference(GridCoord const& p)
    {
        ++GridMapReference[p.x_coord][p.y_coord];
        SetUnloadReferenceLock(GridCoord((MAX_NUMBER_OF_GRIDS - 1) - p.x_coord, (MAX_NUMBER_OF_GRIDS - 1) - p.y_coord), true);
    }

    void RemoveGridMapReference(GridCoord const& p)
    {
        --GridMapReference[p.x_coord][p.y_coord];
        if (!GridMapReference[p.x_coord][p.y_coord])
            SetUnloadReferenceLock(GridCoord((MAX_NUMBER_OF_GRIDS - 1) - p.x_coord, (MAX_NUMBER_OF_GRIDS - 1) - p.y_coord), false);
    }

    InstancedMaps& GetInstancedMaps() { return m_InstancedMaps; }
    void InitVisibilityDistance() override;

private:
    InstanceMap* CreateInstance(uint32 InstanceId, InstanceSave* save, Difficulty difficulty);
    BattlegroundMap* CreateBattleground(uint32 InstanceId, Battleground* bg);

    InstancedMaps m_InstancedMaps;

    uint16 GridMapReference[MAX_NUMBER_OF_GRIDS][MAX_NUMBER_OF_GRIDS];
};
#endif
