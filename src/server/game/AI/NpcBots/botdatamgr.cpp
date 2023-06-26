#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "bot_ai.h"
#include "botdatamgr.h"
#include "botmgr.h"
#include "botspell.h"
#include "botwanderful.h"
#include "bpet_ai.h"
#include "Containers.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GroupMgr.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "WorldDatabase.h"
/*
Npc Bot Data Manager by Trickerer (onlysuffering@gmail.com)
NpcBots DB Data management
%Complete: ???
*/

#ifdef _MSC_VER
# pragma warning(push, 4)
#endif

typedef std::unordered_map<uint32 /*entry*/, NpcBotData*> NpcBotDataMap;
typedef std::unordered_map<uint32 /*entry*/, NpcBotAppearanceData*> NpcBotAppearanceDataMap;
typedef std::unordered_map<uint32 /*entry*/, NpcBotExtras*> NpcBotExtrasMap;
typedef std::unordered_map<uint32 /*entry*/, NpcBotTransmogData*> NpcBotTransmogDataMap;
NpcBotDataMap _botsData;
NpcBotAppearanceDataMap _botsAppearanceData;
NpcBotExtrasMap _botsExtras;
NpcBotTransmogDataMap _botsTransmogData;
NpcBotRegistry _existingBots;

std::map<uint32, uint8> _wpMinSpawnLevelPerMapId;
std::map<uint32, uint8> _wpMaxSpawnLevelPerMapId;
std::map<uint8, std::set<uint32>> _spareBotIdsPerClassMap;
CreatureTemplateContainer _botsWanderCreatureTemplates;
std::unordered_map<uint32, EquipmentInfo const*> _botsWanderCreatureEquipmentTemplates;
std::list<std::pair<uint32, WanderNode const*>> _botsWanderCreaturesToSpawn;
std::set<uint32> _botsWanderCreaturesToDespawn;

ItemPerBotClassMap _botsWanderCreaturesSortedGear;

typedef std::unordered_map<ObjectGuid /*playerGuid*/, BotBankItemContainer> BotGearStorageMap;
BotGearStorageMap _botStoredGearMap;

static bool allBotsLoaded = false;

static uint32 next_wandering_bot_spawn_delay = 0;

static EventProcessor botSpawnEvents;
static std::unordered_map<ObjectGuid, EventProcessor> botBGJoinEvents;

bool BotBankItemCompare::operator()(Item const* item1, Item const* item2) const
{
    ItemTemplate const* proto1 = item1->GetTemplate();
    ItemTemplate const* proto2 = item2->GetTemplate();
    return proto1->Name1 < proto2->Name1;
}

class BotBattlegroundEnterEvent : public BasicEvent
{
    const ObjectGuid _playerGUID;
    const ObjectGuid _botGUID;
    const BattlegroundQueueTypeId _bgQueueTypeId;
    const uint64 _removeTime;

public:
    BotBattlegroundEnterEvent(ObjectGuid playerGUID, ObjectGuid botGUID, BattlegroundQueueTypeId bgQueueTypeId, uint64 removeTime)
        : _playerGUID(playerGUID), _botGUID(botGUID), _bgQueueTypeId(bgQueueTypeId), _removeTime(removeTime) {}

    void AbortMe()
    {
        LOG_ERROR("npcbots", "BotBattlegroundEnterEvent: Aborting bot {} bg {}!", _botGUID.GetEntry(), uint32(_bgQueueTypeId));
        sBattlegroundMgr->GetBattlegroundQueue(_bgQueueTypeId).RemovePlayer(_botGUID, true);
        BotDataMgr::DespawnWandererBot(_botGUID.GetEntry());
    }

    void AbortAll()
    {
        LOG_ERROR("npcbots", "BotBattlegroundEnterEvent: Aborting ALL bots by {} bg {}!", _playerGUID.GetCounter(), uint32(_bgQueueTypeId));
        AbortMe();
        botBGJoinEvents.at(_playerGUID).KillAllEvents(false);
    }

    bool Execute(uint64 e_time, uint32 /*p_time*/) override
    {
        if (e_time >= _removeTime)
        {
            AbortMe();
            return true;
        }
        else if (Creature const* bot = BotDataMgr::FindBot(_botGUID.GetEntry()))
        {
            Player const* bgPlayer = ObjectAccessor::FindConnectedPlayer(_playerGUID);
            if (bgPlayer && bgPlayer->IsInWorld() && bgPlayer->InBattleground() && bgPlayer->GetMap()->IsBattlegroundOrArena())
            {
                Battleground* bg = ASSERT_NOTNULL(bgPlayer->GetBattleground());

                //full, some players connected
                if (bg->GetPlayersCountByTeam(TEAM_ALLIANCE) + bg->GetPlayersCountByTeam(TEAM_HORDE) >= bg->GetMaxPlayersPerTeam() * 2)
                {
                    AbortAll();
                    return true;
                }

                BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(_bgQueueTypeId);
                if (!queue.IsBotInvited(_botGUID, bg->GetInstanceID()))
                {
                    AbortMe();
                    return true;
                }

                queue.RemovePlayer(bot->GetGUID(), false);

                //BG is set second time in Battleground::AddBot() but it's the same value so this is alright
                bot->GetBotAI()->SetBG(bg);

                TeamId teamId = BotDataMgr::GetTeamIdForFaction(bot->GetFaction());
                BotMgr::TeleportBot(const_cast<Creature*>(bot), bgPlayer->GetMap(), bg->GetTeamStartPosition(teamId), true, false);
            }
            else if (bgPlayer && bgPlayer->InBattlegroundQueue() && bgPlayer->GetBattlegroundQueueIndex(_bgQueueTypeId) < PLAYER_MAX_BATTLEGROUND_QUEUES)
                botBGJoinEvents.at(_playerGUID).AddEventAtOffset(new BotBattlegroundEnterEvent(_playerGUID, _botGUID, _bgQueueTypeId, _removeTime), 2s);
            else
                AbortAll();
        }

        return true;
    }

    void Abort(uint64 /*e_time*/) override { AbortMe(); }
};

void SpawnWandererBot(uint32 bot_id, WanderNode const* spawnLoc, NpcBotRegistry* registry)
{
    CreatureTemplate const& bot_template = _botsWanderCreatureTemplates.at(bot_id);
    NpcBotData const* bot_data = BotDataMgr::SelectNpcBotData(bot_id);
    NpcBotExtras const* bot_extras = BotDataMgr::SelectNpcBotExtras(bot_id);
    Position spawnPos = spawnLoc->GetPosition();

    ASSERT(bot_data);
    ASSERT(bot_extras);

    Map* map = sMapMgr->CreateBaseMap(spawnLoc->GetMapId());
    map->LoadGrid(spawnLoc->m_positionX, spawnLoc->m_positionY);

    LOG_DEBUG("npcbots", "Spawning wandering bot: {} ({}) class {} race {} fac {}, location: mapId {} {} ({})",
        bot_template.Name.c_str(), bot_id, uint32(bot_extras->bclass), uint32(bot_extras->race), bot_data->faction,
        spawnLoc->GetMapId(), spawnLoc->ToString().c_str(), spawnLoc->GetName().c_str());

    Creature* bot = new Creature();
    if (!bot->LoadBotCreatureFromDB(0, map, true, true, bot_id, &spawnPos))
    {
        delete bot;
        LOG_FATAL("server.loading", "Cannot load npcbot from DB!");
        ASSERT(false);
    }

    if (registry)
        registry->insert(bot);
}

void BotDataMgr::DespawnWandererBot(uint32 entry)
{
    Creature const* bot = FindBot(entry);
    if (bot && bot->IsWandererBot())
    {
        if (bot->GetBotAI())
            bot->GetBotAI()->canUpdate = false;
        _botsWanderCreaturesToDespawn.insert(entry);
    }
    else
        LOG_ERROR("npcbots", "DespawnWandererBot(): trying to despawn non-existing wanderer bot {} '{}'!", entry, bot ? bot->GetName().c_str() : "unknown");
}

struct WanderingBotsGenerator
{
private:
    using NodeVec = std::vector<WanderNode const*>;

    const std::map<uint8, uint32> wbot_faction_for_ex_class = {
        {BOT_CLASS_BM, 14u/*2u*/},
        {BOT_CLASS_SPHYNX, 14u},
        {BOT_CLASS_ARCHMAGE, 14u/*1u*/},
        {BOT_CLASS_DREADLORD, 14u},
        {BOT_CLASS_SPELLBREAKER, 14u/*1610u*/},
        {BOT_CLASS_DARK_RANGER, 14u},
        {BOT_CLASS_NECROMANCER, 14u},
        {BOT_CLASS_SEA_WITCH, 14u},
        {BOT_CLASS_CRYPT_LORD, 14u}
    };

    uint32 next_bot_id;
    uint32 enabledBotsCount;

    WanderingBotsGenerator()
    {
        next_bot_id = BOT_ENTRY_CREATE_BEGIN - 1;
        QueryResult result = CharacterDatabase.Query("SELECT value FROM worldstates WHERE entry = {}", uint32(BOT_GIVER_ENTRY));
        if (!result)
        {
            LOG_WARN("server.loading", "Next bot id for autogeneration is not found! Resetting! (client cache may interfere with names)");
            for (uint32 bot_cid : BotDataMgr::GetExistingNPCBotIds())
                if (bot_cid > next_bot_id)
                    next_bot_id = bot_cid;
            CharacterDatabase.DirectExecute("INSERT INTO worldstates (entry, value, comment) VALUES ({}, {}, '{}')",
                uint32(BOT_GIVER_ENTRY), next_bot_id, "NPCBOTS MOD - last autogenerated bot entry");
        }
        else
            next_bot_id = result->Fetch()[0].Get<uint32>();

        ASSERT(next_bot_id > BOT_ENTRY_BEGIN);

        for (uint8 c = BOT_CLASS_WARRIOR; c < BOT_CLASS_END; ++c)
            if (BotMgr::IsClassEnabled(c) && _spareBotIdsPerClassMap.find(c) == _spareBotIdsPerClassMap.cend())
                _spareBotIdsPerClassMap.insert({ c, {} });

        for (decltype(_botsExtras)::value_type const& vt : _botsExtras)
        {
            uint8 c = vt.second->bclass;
            if (c != BOT_CLASS_NONE && BotMgr::IsClassEnabled(c))
            {
                ++enabledBotsCount;
                if (_botsData.find(vt.first) == _botsData.end())
                {
                    ASSERT(_spareBotIdsPerClassMap.find(c) != _spareBotIdsPerClassMap.cend());
                    _spareBotIdsPerClassMap.at(c).insert(vt.first);
                }
            }
        }

        for (uint8 c = BOT_CLASS_WARRIOR; c < BOT_CLASS_END; ++c)
            if (_spareBotIdsPerClassMap.find(c) != _spareBotIdsPerClassMap.cend() && _spareBotIdsPerClassMap.at(c).empty())
                _spareBotIdsPerClassMap.erase(c);
    }

    uint32 GetDefaultFactionForRaceClass(uint8 bot_class, uint8 bot_race) const
    {
        ChrRacesEntry const* rentry = sChrRacesStore.LookupEntry(bot_race);
        return (bot_class >= BOT_CLASS_EX_START) ? wbot_faction_for_ex_class.find(bot_class)->second : rentry ? rentry->FactionID : 14u;
    }

    bool GenerateWanderingBotToSpawn(std::map<uint8, std::set<uint32>>& spareBotIdsPerClass, uint8 desired_bracket,
        NodeVec const& spawns_a, NodeVec const& spawns_h, NodeVec const& spawns_n,
        bool immediate, PvPDifficultyEntry const* bracketEntry, NpcBotRegistry* registry)
    {
        ASSERT(!spareBotIdsPerClass.empty());

        CreatureTemplateContainer const* all_templates = sObjectMgr->GetCreatureTemplates();

        while (all_templates->find(++next_bot_id) != all_templates->cend()) {}

        auto const& spareBotPair = Acore::Containers::SelectRandomContainerElement(spareBotIdsPerClass);
        const uint8 bot_class = spareBotPair.first;
        auto const& cSet = spareBotPair.second;
        ASSERT(!cSet.empty());
        uint32 orig_entry = cSet.size() == 1 ? *cSet.cbegin() : Acore::Containers::SelectRandomContainerElement(cSet);
        CreatureTemplate const* orig_template = ASSERT_NOTNULL(sObjectMgr->GetCreatureTemplate(orig_entry));
        NpcBotExtras const* orig_extras = ASSERT_NOTNULL(BotDataMgr::SelectNpcBotExtras(orig_entry));
        uint32 bot_faction = GetDefaultFactionForRaceClass(bot_class, orig_extras->race);

        NodeVec const* bot_spawn_nodes;
        TeamId bot_team = BotDataMgr::GetTeamIdForFaction(bot_faction);
        switch (bot_team)
        {
            case TEAM_ALLIANCE:
                bot_spawn_nodes = &spawns_a;
                break;
            case TEAM_HORDE:
                bot_spawn_nodes = &spawns_h;
                break;
            default:
                bot_spawn_nodes = &spawns_n;
                break;
        }
        NodeVec level_nodes;
        level_nodes.reserve(bot_spawn_nodes->size());
        desired_bracket = std::max<uint8>(desired_bracket, BotDataMgr::GetMinLevelForBotClass(bot_class) / 10);
        for (WanderNode const* node : *bot_spawn_nodes)
        {
            if (desired_bracket * 10 + 9 >= node->GetLevels().first && node->GetLevels().second >= desired_bracket * 10)
                level_nodes.push_back(node);
        }

        ASSERT(!level_nodes.empty());
        WanderNode const* spawnLoc = Acore::Containers::SelectRandomContainerElement(level_nodes);

        CreatureTemplate& bot_template = _botsWanderCreatureTemplates[next_bot_id];
        //copy all fields
        bot_template = *orig_template;
        bot_template.Entry = next_bot_id;
        bot_template.SubName = "";
        bot_template.speed_run = BotMgr::GetBotWandererSpeedMod();
        bot_template.KillCredit[0] = orig_entry;
        if (bracketEntry)
        {
            //force level range for bgs
            bot_template.minlevel = std::min<uint32>(bracketEntry->minLevel, DEFAULT_MAX_LEVEL);
            bot_template.maxlevel = std::min<uint32>(bracketEntry->maxLevel, DEFAULT_MAX_LEVEL);
        }
        else
        {
            bot_template.minlevel = std::min<uint32>(std::max<uint32>(desired_bracket * 10, spawnLoc->GetLevels().first), DEFAULT_MAX_LEVEL);
            bot_template.maxlevel = std::min<uint32>(std::min<uint32>(desired_bracket * 10 + 9, spawnLoc->GetLevels().second), DEFAULT_MAX_LEVEL);
            bot_template.flags_extra &= ~(CREATURE_FLAG_EXTRA_NO_XP);
        }

        bot_template.InitializeQueryData();

        uint8 bot_spec = bot_ai::SelectSpecForClass(bot_class);
        NpcBotData* bot_data = new NpcBotData(bot_ai::DefaultRolesForClass(bot_class, bot_spec), bot_faction, bot_spec);
        _botsData[next_bot_id] = bot_data;
        NpcBotExtras* bot_extras = new NpcBotExtras();
        bot_extras->bclass = bot_class;
        bot_extras->race = orig_extras->race;
        _botsExtras[next_bot_id] = bot_extras;
        if (NpcBotAppearanceData const* orig_apdata = BotDataMgr::SelectNpcBotAppearance(orig_entry))
        {
            NpcBotAppearanceData* bot_apdata = new NpcBotAppearanceData();
            bot_apdata->face = orig_apdata->face;
            bot_apdata->features = orig_apdata->features;
            bot_apdata->gender = orig_apdata->gender;
            bot_apdata->hair = orig_apdata->hair;
            bot_apdata->haircolor = orig_apdata->haircolor;
            bot_apdata->skin = orig_apdata->skin;
            _botsAppearanceData[next_bot_id] = bot_apdata;
        }
        int8 beqId = 1;
        _botsWanderCreatureEquipmentTemplates[next_bot_id] = sObjectMgr->GetEquipmentInfo(orig_entry, beqId);

        //We do not create CreatureData for generated bots

        CellCoord c = Acore::ComputeCellCoord(spawnLoc->m_positionX, spawnLoc->m_positionY);
        GridCoord g = Acore::ComputeGridCoord(spawnLoc->m_positionX, spawnLoc->m_positionY);
        ASSERT(c.IsCoordValid(), "Invalid Cell coord!");
        ASSERT(g.IsCoordValid(), "Invalid Grid coord!");
        Map* map = sMapMgr->CreateBaseMap(spawnLoc->GetMapId());
        ASSERT(map->GetEntry()->IsContinent() || map->GetEntry()->IsBattlegroundOrArena(), map->GetDebugInfo().c_str());

        if (immediate)
            SpawnWandererBot(next_bot_id, spawnLoc, registry);
        else
            _botsWanderCreaturesToSpawn.push_back({ next_bot_id, spawnLoc });

        _spareBotIdsPerClassMap.at(bot_class).erase(orig_entry);
        if (_spareBotIdsPerClassMap.at(bot_class).empty())
            _spareBotIdsPerClassMap.erase(bot_class);

        spareBotIdsPerClass.at(bot_class).erase(orig_entry);
        if (spareBotIdsPerClass.at(bot_class).empty())
            spareBotIdsPerClass.erase(bot_class);

        return true;
    }

public:
    uint32 GetEnabledBotsCount() const { return enabledBotsCount; }

    uint32 GetSpareBotsCount(TeamId teamId = TEAM_NEUTRAL) const
    {
        uint32 count = 0;
        for (auto const& kv : _spareBotIdsPerClassMap)
        {
            if (teamId == TEAM_NEUTRAL)
                count += kv.second.size();
            else
            {
                if (kv.first >= BOT_CLASS_EX_START)
                {
                    auto cit = wbot_faction_for_ex_class.find(kv.first);
                    if (cit != wbot_faction_for_ex_class.cend() && cit->second == FACTION_MONSTER)
                        continue;
                }

                for (uint32 entry : kv.second)
                {
                    NpcBotExtras const* extras = ASSERT_NOTNULL(BotDataMgr::SelectNpcBotExtras(entry));
                    uint32 bot_faction = GetDefaultFactionForRaceClass(kv.first, extras->race);
                    TeamId bot_team = BotDataMgr::GetTeamIdForFaction(bot_faction);
                    if (teamId == bot_team)
                        ++count;
                }
            }
        }
        return count;
    }

    bool GenerateWanderingBotsToSpawn(uint32 count, int32 map_id, int32 team, bool immediate, PvPDifficultyEntry const* bracketEntry, NpcBotRegistry* registry, uint32& spawned)
    {
        using NodeVec = std::vector<WanderNode const*>;

        if (_spareBotIdsPerClassMap.empty())
            return false;

        NodeVec spawns_a, spawns_h, spawns_n;
        for (NodeVec* vec : { &spawns_a, &spawns_h, &spawns_n })
            vec->reserve(WanderNode::GetWPMapsCount() * 20u);

        WanderNode::DoForAllWPs([map_id = map_id, &spawns_a, &spawns_h, &spawns_n](WanderNode const* wp) {
            MapEntry const* mapEntry = sMapStore.LookupEntry(wp->GetMapId());
            if ((map_id == -1) ? mapEntry->IsWorldMap() : (int32(mapEntry->MapID) == map_id))
            {
                if (wp->HasFlag(BotWPFlags::BOTWP_FLAG_SPAWN))
                {
                    if (wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY))
                        spawns_a.push_back(wp);
                    else if (wp->HasFlag(BotWPFlags::BOTWP_FLAG_HORDE_ONLY))
                        spawns_h.push_back(wp);
                    else
                    {
                        spawns_a.push_back(wp);
                        spawns_h.push_back(wp);
                        spawns_n.push_back(wp);
                    }
                }
            }
        });

        bool found_maxlevel_node_a = false;
        bool found_maxlevel_node_h = false;
        bool found_maxlevel_node_n = false;
        const uint8 maxof_minclasslvl_nor = BotDataMgr::GetMinLevelForBotClass(BOT_CLASS_DEATH_KNIGHT); // 55
        const uint8 maxof_minclasslvl_ex = BotDataMgr::GetMinLevelForBotClass(BOT_CLASS_DREADLORD); // 60
        for (WanderNode const* wp : spawns_a)
        {
            if (wp->GetLevels().second >= maxof_minclasslvl_nor)
            {
                found_maxlevel_node_a = true;
                break;
            }
        }
        for (WanderNode const* wp : spawns_h)
        {
            if (wp->GetLevels().second >= maxof_minclasslvl_nor)
            {
                found_maxlevel_node_h = true;
                break;
            }
        }
        for (WanderNode const* wp : spawns_n)
        {
            if (wp->GetLevels().second >= maxof_minclasslvl_ex)
            {
                found_maxlevel_node_n = true;
                break;
            }
        }

        decltype (_spareBotIdsPerClassMap) teamSpareBotIdsPerClass;
        BotBrackets bracketPcts{};
        BotBrackets bots_per_bracket{};

        if (team == -1)
        {
            if (!found_maxlevel_node_a || !found_maxlevel_node_h || !found_maxlevel_node_n)
                return false;

            //make a full copy
            teamSpareBotIdsPerClass = _spareBotIdsPerClassMap;
            bracketPcts = BotMgr::GetBotWandererLevelBrackets();
        }
        else
        {
            bracketPcts[bracketEntry->minLevel / 10] = 100u;
            switch (team)
            {
                case ALLIANCE:
                    if (!found_maxlevel_node_a)
                        return false;
                    break;
                case HORDE:
                    if (!found_maxlevel_node_h)
                        return false;
                    break;
                case TEAM_OTHER:
                default:
                    if (!found_maxlevel_node_n)
                        return false;
                    break;
            }

            for (auto const& kv : _spareBotIdsPerClassMap)
            {
                for (uint32 spareBotId : kv.second)
                {
                    NpcBotExtras const* orig_extras = ASSERT_NOTNULL(BotDataMgr::SelectNpcBotExtras(spareBotId));
                    uint32 bot_faction = GetDefaultFactionForRaceClass(kv.first, orig_extras->race);
                    uint32 botTeam = BotDataMgr::GetTeamForFaction(bot_faction);

                    if (int32(botTeam) != team)
                        continue;

                    if (bracketEntry)
                    {
                        uint8 botminlevel = BotDataMgr::GetMinLevelForBotClass(kv.first);
                        if (botminlevel > bracketEntry->maxLevel)
                            continue;
                    }

                    teamSpareBotIdsPerClass[kv.first].insert(spareBotId);
                }
            }
        }

        if (teamSpareBotIdsPerClass.empty())
            return false;

        uint32 total_bots_in_brackets = 0;
        for (size_t k = 0; k < BracketsCount; ++k)
        {
            if (!bracketPcts[k])
                continue;
            bots_per_bracket[k] = CalculatePct(count, bracketPcts[k]);
            total_bots_in_brackets += bots_per_bracket[k];
        }
        for (int32 j = BracketsCount - 1; j >= 0; --j)
        {
            if (bots_per_bracket[j])
            {
                bots_per_bracket[j] += count - total_bots_in_brackets;
                break;
            }
        }

        std::vector<uint8> brackets_shuffled;
        brackets_shuffled.reserve(count);
        for (uint8 bracket = 0; bracket < BracketsCount; ++bracket)
        {
            while (bots_per_bracket[bracket])
            {
                brackets_shuffled.push_back(bracket);
                --bots_per_bracket[bracket];
            }
        }
        Acore::Containers::RandomShuffle(brackets_shuffled);

        for (size_t i = 0; i < brackets_shuffled.size() && !teamSpareBotIdsPerClass.empty();) // i is a counter, NOT used as index or value
        {
            uint8 bracket = brackets_shuffled[i];

            int8 tries = 100;
            do {
                --tries;
                if (GenerateWanderingBotToSpawn(teamSpareBotIdsPerClass, bracket, spawns_a, spawns_h, spawns_n, immediate, bracketEntry, registry))
                {
                    ++i;
                    ++spawned;
                    break;
                }
            } while (tries >= 0);

            if (tries < 0)
                return false;
        }

        CharacterDatabase.Execute("UPDATE worldstates SET value = {} WHERE entry = {}", next_bot_id, uint32(BOT_GIVER_ENTRY));

        return true;
    }

    static WanderingBotsGenerator* instance()
    {
        static WanderingBotsGenerator _instance;
        return &_instance;
    }
};
#define sBotGen WanderingBotsGenerator::instance()

void BotDataMgr::Update(uint32 diff)
{
    botSpawnEvents.Update(diff);
    for (auto& kv : botBGJoinEvents)
        kv.second.Update(diff);

    if (!_botsWanderCreaturesToDespawn.empty())
    {
        LOG_DEBUG("npcbots", "Bots to despawn: {}", uint32(_botsWanderCreaturesToDespawn.size()));

        while (!_botsWanderCreaturesToDespawn.empty())
        {
            uint32 bot_despawn_id = *_botsWanderCreaturesToDespawn.begin();

            Creature* bot = const_cast<Creature*>(FindBot(bot_despawn_id));
            ASSERT(bot);

            if (!bot->IsInWorld())
                break;

            _botsWanderCreaturesToDespawn.erase(bot_despawn_id);

            uint32 origEntry = _botsWanderCreatureTemplates.at(bot_despawn_id).KillCredit[0];
            std::string botName = bot->GetName();

            _spareBotIdsPerClassMap[bot->GetBotClass()].insert(origEntry);

            BotMgr::CleanupsBeforeBotDelete(bot);
            bot->GetBotAI()->canUpdate = false;
            bot->GetMap()->AddObjectToRemoveList(bot);

            auto bditr = _botsData.find(bot_despawn_id);
            auto beitr = _botsExtras.find(bot_despawn_id);
            auto baditr = _botsAppearanceData.find(bot_despawn_id);
            auto bwcetitr = _botsWanderCreatureEquipmentTemplates.find(bot_despawn_id);
            auto bwctitr = _botsWanderCreatureTemplates.find(bot_despawn_id);

            ASSERT(bditr != _botsData.end());
            ASSERT(beitr != _botsExtras.end());
            //ASSERT(baditr != _botsAppearanceData.end()); may not exist
            ASSERT(bwcetitr != _botsWanderCreatureEquipmentTemplates.end());
            ASSERT(bwctitr != _botsWanderCreatureTemplates.end());

            delete bditr->second;
            _botsData.erase(bditr);
            delete beitr->second;
            _botsExtras.erase(beitr);
            if (baditr != _botsAppearanceData.end())
            {
                delete baditr->second;
                _botsAppearanceData.erase(baditr);
            }
            _botsWanderCreatureEquipmentTemplates.erase(bwcetitr);
            _botsWanderCreatureTemplates.erase(bwctitr);

            LOG_DEBUG("npcbots", "Despawned wanderer bot {} '{}' (orig {})", bot_despawn_id, botName.c_str(), origEntry);
        }
    }

    if (!_botsWanderCreaturesToSpawn.empty())
    {
        static const uint32 WANDERING_BOT_SPAWN_DELAY = 500;

        next_wandering_bot_spawn_delay += diff;

        while (next_wandering_bot_spawn_delay >= WANDERING_BOT_SPAWN_DELAY && !_botsWanderCreaturesToSpawn.empty())
        {
            next_wandering_bot_spawn_delay -= WANDERING_BOT_SPAWN_DELAY;

            auto const& p = _botsWanderCreaturesToSpawn.front();

            uint32 bot_id = p.first;
            WanderNode const* spawnLoc = p.second;

            _botsWanderCreaturesToSpawn.pop_front();

            SpawnWandererBot(bot_id, spawnLoc, nullptr);
        }

        return;
    }
}

std::shared_mutex* BotDataMgr::GetLock()
{
    static std::shared_mutex _lock;
    return &_lock;
}

bool BotDataMgr::AllBotsLoaded()
{
    return allBotsLoaded;
}

void BotDataMgr::LoadNpcBots(bool spawn)
{
    if (allBotsLoaded)
        return;

    LOG_INFO("server.loading", "Starting NpcBot system...");

    GenerateBotCustomSpells();

    uint32 botoldMSTime = getMSTime();

    Field* field;
    uint8 index;

    //                                                      1       2     3     4     5          6
    QueryResult result = WorldDatabase.Query("SELECT entry, gender, skin, face, hair, haircolor, features FROM creature_template_npcbot_appearance");
    if (result)
    {
        do
        {
            field = result->Fetch();
            index = 0;
            uint32 entry = field[  index].Get<uint32>();

            if (!sObjectMgr->GetCreatureTemplate(entry))
            {
                LOG_ERROR("server.loading", "Bot entry {} has appearance data but doesn't exist in `creature_template` table! Skipped.", entry);
                continue;
            }

            NpcBotAppearanceData* appearanceData = new NpcBotAppearanceData();
            appearanceData->gender =    field[++index].Get<uint8>();
            appearanceData->skin =      field[++index].Get<uint8>();
            appearanceData->face =      field[++index].Get<uint8>();
            appearanceData->hair =      field[++index].Get<uint8>();
            appearanceData->haircolor = field[++index].Get<uint8>();
            appearanceData->features =  field[++index].Get<uint8>();

            _botsAppearanceData[entry] = appearanceData;

        } while (result->NextRow());

        LOG_INFO("server.loading", ">> Bot appearance data loaded");
    }
    else
        LOG_INFO("server.loading", ">> Bots appearance data is not loaded. Table `creature_template_npcbot_appearance` is empty!");

    //                                          1      2
    result = WorldDatabase.Query("SELECT entry, class, race FROM creature_template_npcbot_extras");
    if (result)
    {
        do
        {
            field = result->Fetch();
            index = 0;
            uint32 entry =      field[  index].Get<uint32>();

            if (!sObjectMgr->GetCreatureTemplate(entry))
            {
                LOG_ERROR("server.loading", "Bot entry {} has extras data but doesn't exist in `creature_template` table! Skipped.", entry);
                continue;
            }

            NpcBotExtras* extras = new NpcBotExtras();
            extras->bclass =    field[++index].Get<uint8>();
            extras->race =      field[++index].Get<uint8>();

            _botsExtras[entry] = extras;

        } while (result->NextRow());

        LOG_INFO("server.loading", ">> Bot race data loaded");
    }
    else
        LOG_INFO("server.loading", ">> Bots race data is not loaded. Table `creature_template_npcbot_extras` is empty!");

    //                                              1     2        3
    result = CharacterDatabase.Query("SELECT entry, slot, item_id, fake_id FROM characters_npcbot_transmog");
    if (result)
    {
        do
        {
            field = result->Fetch();
            index = 0;
            uint32 entry =          field[  index].Get<uint32>();

            if (!sObjectMgr->GetCreatureTemplate(entry))
            {
                LOG_ERROR("server.loading", "Bot entry {} has transmog data but doesn't exist in `creature_template` table! Skipped.", entry);
                continue;
            }

            if (_botsTransmogData.count(entry) == 0)
                _botsTransmogData[entry] = new NpcBotTransmogData();

            //load data
            uint8 slot =            field[++index].Get<uint8>();
            uint32 item_id =        field[++index].Get<uint32>();
            int32 fake_id =         field[++index].Get<int32>();

            _botsTransmogData[entry]->transmogs[slot] = { item_id, fake_id };

        } while (result->NextRow());

        LOG_INFO("server.loading", ">> Bot transmog data loaded");
    }
    else
        LOG_INFO("server.loading", ">> Bots transmog data is not loaded. Table `characters_npcbot_transmog` is empty!");

    //                                       0      1      2      3     4        5          6          7          8          9               10          11          12         13
    result = CharacterDatabase.Query("SELECT entry, owner, roles, spec, faction, equipMhEx, equipOhEx, equipRhEx, equipHead, equipShoulders, equipChest, equipWaist, equipLegs, equipFeet,"
    //   14          15          16         17         18            19            20             21             22         23
        "equipWrist, equipHands, equipBack, equipBody, equipFinger1, equipFinger2, equipTrinket1, equipTrinket2, equipNeck, spells_disabled FROM characters_npcbot");

    if (result)
    {
        uint32 botcounter = 0;
        uint32 datacounter = 0;
        std::set<uint32> botgrids;
        QueryResult infores;
        CreatureTemplate const* proto;
        NpcBotData* botData;
        std::list<uint32> entryList;

        do
        {
            field = result->Fetch();
            index = 0;
            uint32 entry =          field[  index].Get<uint32>();

            if (!sObjectMgr->GetCreatureTemplate(entry))
            {
                LOG_ERROR("server.loading", "Bot entry {} doesn't exist in `creature_template` table! Skipped.", entry);
                continue;
            }

            //load data
            botData = new NpcBotData(0, 0);
            botData->owner =        field[++index].Get<uint32>();
            botData->roles =        field[++index].Get<uint32>();
            botData->spec =         field[++index].Get<uint8>();
            botData->faction =      field[++index].Get<uint32>();

            for (uint8 i = BOT_SLOT_MAINHAND; i != BOT_INVENTORY_SIZE; ++i)
                botData->equips[i] = field[++index].Get<uint32>();

            std::string disabled_spells_str = field[++index].Get<std::string>();
            if (!disabled_spells_str.empty())
            {
                std::vector<std::string_view> tok = Acore::Tokenize(disabled_spells_str, ' ', false);
                for (std::vector<std::string_view>::size_type i = 0; i != tok.size(); ++i)
                    botData->disabled_spells.insert(*(Acore::StringTo<uint32>(tok[i])));
            }

            entryList.push_back(entry);
            _botsData[entry] = botData;
            ++datacounter;

        } while (result->NextRow());

        LOG_INFO("server.loading", ">> Loaded {} bot data entries", datacounter);

        if (spawn)
        {
            for (std::list<uint32>::const_iterator itr = entryList.cbegin(); itr != entryList.cend(); ++itr)
            {
                uint32 entry = *itr;
                proto = sObjectMgr->GetCreatureTemplate(entry);
                //                                     1     2    3           4            5           6
                infores = WorldDatabase.Query("SELECT guid, map, position_x, position_y"/*, position_z, orientation*/" FROM creature WHERE id1 = {}", entry);
                if (!infores)
                {
                    LOG_ERROR("server.loading", "Cannot spawn npcbot {} (id: {}), not found in `creature` table!", proto->Name.c_str(), entry);
                    continue;
                }

                field = infores->Fetch();
                uint32 tableGuid = field[0].Get<uint32>();
                uint32 mapId = uint32(field[1].Get<uint16>());
                float pos_x = field[2].Get<float>();
                float pos_y = field[3].Get<float>();
                //float pos_z = field[4].GetFloat();
                //float ori = field[5].GetFloat();

                CellCoord c = Acore::ComputeCellCoord(pos_x, pos_y);
                GridCoord g = Acore::ComputeGridCoord(pos_x, pos_y);
                ASSERT(c.IsCoordValid(), "Invalid Cell coord!");
                ASSERT(g.IsCoordValid(), "Invalid Grid coord!");
                Map* map = sMapMgr->CreateBaseMap(mapId);
                map->LoadGrid(pos_x, pos_y);

                ObjectGuid Guid(HighGuid::Unit, entry, tableGuid);
                LOG_DEBUG("server.loading", "bot {}: spawnId {}, full {}", entry, tableGuid, Guid.ToString().c_str());
                Creature* bot = map->GetCreature(Guid);
                if (!bot) //not in map, use storage
                {
                    //TC_LOG_DEBUG("server.loading", "bot %u: spawnId %u, is not in map on load", entry, tableGuid);
                    typedef Map::CreatureBySpawnIdContainer::const_iterator SpawnIter;
                    std::pair<SpawnIter, SpawnIter> creBounds = map->GetCreatureBySpawnIdStore().equal_range(tableGuid);
                    if (creBounds.first == creBounds.second)
                    {
                        LOG_ERROR("server.loading", "bot {} is not in spawns list, consider re-spawning it!", entry);
                        continue;
                    }
                    bot = creBounds.first->second;
                }
                ASSERT(bot);
                if (!bot->FindMap())
                    LOG_ERROR("server.loading", "bot {} is not in map!", entry);
                if (!bot->IsInWorld())
                    LOG_ERROR("server.loading", "bot {} is not in world!", entry);
                if (!bot->IsAlive())
                {
                    LOG_ERROR("server.loading", "bot {} is dead, respawning!", entry);
                    bot->Respawn();
                }

                LOG_DEBUG("server.loading", ">> Spawned npcbot {} (id: {}, map: {}, grid: {}, cell: {})", proto->Name.c_str(), entry, mapId, g.GetId(), c.GetId());
                botgrids.insert(g.GetId());
                ++botcounter;
            }

            LOG_INFO("server.loading", ">> Spawned {} npcbot(s) within {} grid(s) in {} ms", botcounter, uint32(botgrids.size()), GetMSTimeDiffToNow(botoldMSTime));
        }
    }
    else
        LOG_INFO("server.loading", ">> Loaded 0 npcbots. Table `characters_npcbot` is empty!");

    allBotsLoaded = true;
}

void BotDataMgr::LoadNpcBotGroupData()
{
    LOG_INFO("server.loading", "Loading NPCBot group members...");

    uint32 oldMSTime = getMSTime();

    CharacterDatabase.DirectExecute("DELETE FROM characters_npcbot_group_member WHERE guid NOT IN (SELECT guid FROM `groups`)");
    CharacterDatabase.DirectExecute("DELETE FROM characters_npcbot_group_member WHERE entry NOT IN (SELECT entry FROM characters_npcbot)");

    //                                                   0     1      2            3         4
    QueryResult result = CharacterDatabase.Query("SELECT guid, entry, memberFlags, subgroup, roles FROM characters_npcbot_group_member ORDER BY guid");
    if (!result)
    {
        LOG_INFO("server.loading", ">> Loaded 0 NPCBot group members. DB table `characters_npcbot_group_member` is empty!");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();

        uint32 creature_id = fields[1].Get<uint32>();
        if (!SelectNpcBotExtras(creature_id))
        {
            LOG_WARN("server.loading", "Table `characters_npcbot_group_member` contains non-NPCBot creature {} which will not be loaded!", creature_id);
            continue;
        }

        if (Group* group = sGroupMgr->GetGroupByGUID(fields[0].Get<uint32>()))
        {
            group->LoadCreatureMemberFromDB(creature_id, fields[2].Get<uint8>(), fields[3].Get<uint8>(), fields[4].Get<uint8>());
            BotMgr::SetBotGroup(creature_id, group);
        }
        else
            LOG_ERROR("misc", "BotDataMgr::LoadNpcBotGroupData: Consistency failed, can't find group (storage id: {})", fields[0].Get<uint32>());

        ++count;

    } while (result->NextRow());

    LOG_INFO("server.loading", ">> Loaded {} NPCBot group members in {} ms", count, GetMSTimeDiffToNow(oldMSTime));
}

void BotDataMgr::LoadNpcBotGearStorage()
{
    LOG_INFO("server.loading", "Loading NPCBot items storage...");

    uint32 oldMSTime = getMSTime();

    QueryResult result = CharacterDatabase.Query(
    //          0               1                   2         3            4           5         6                7                    8              9              10       11       12            13             14       15
        "SELECT ii.creatorGuid, ii.giftCreatorGuid, ii.count, ii.duration, ii.charges, ii.flags, ii.enchantments, ii.randomPropertyId, ii.durability, ii.playedTime, ii.text, ii.guid, ii.itemEntry, ii.owner_guid, gs.guid, gs.item_guid"
        " FROM  characters_npcbot_gear_storage gs JOIN item_instance ii ON gs.item_guid = ii.guid ORDER BY gs.guid, gs.item_guid");
    if (!result)
    {
        LOG_INFO("server.loading", ">> Loaded 0 NPCBot stored gear items. DB table `characters_npcbot_gear_storage` is empty!");
        return;
    }

    uint32 count = 0;
    std::set<uint32> player_guids;
    do
    {
        Field* fields = result->Fetch();

        uint32 item_id = fields[12].Get<uint32>();
        uint32 player_guidlow = fields[14].Get<uint32>();
        uint32 item_guidlow = fields[15].Get<uint32>();

        Item* item = new Item();
        ObjectGuid player_guid = ObjectGuid::Create<HighGuid::Player>(player_guidlow);
        ASSERT(item->LoadFromDB(item_guidlow, player_guid, fields, item_id), "LoadNpcBotGearStorage(): unable to load item {} id {}! Owner: {}", item_guidlow, item_id, player_guid.ToString().c_str());

        _botStoredGearMap[player_guid].insert(item);
        player_guids.insert(player_guidlow);
        ++count;

    } while (result->NextRow());

    LOG_INFO("server.loading", ">> Loaded {} NPCBot stored items for {} bot owners in {} ms", count, uint32(player_guids.size()), GetMSTimeDiffToNow(oldMSTime));
}

void BotDataMgr::LoadWanderMap(bool reload)
{
    using SpawnMapEx = std::map<uint32, bool>;
    using SpawnVector = std::vector<WanderNode const*>;

    const std::array<uint32, 4> ALL_CONTINENT_MAPS = { 0u, 1u, 530, 571 };

    if (WanderNode::GetAllWPsCount() > 0u)
    {
        if (!reload)
            return;

        WanderNode::RemoveAllWPs();
    }

    _wpMinSpawnLevelPerMapId.clear();
    _wpMaxSpawnLevelPerMapId.clear();

    uint32 botoldMSTime = getMSTime();

    LOG_INFO("server.loading", "Setting up wander map...");

    //                                             0    1   2 3 4 5   6      7       8        9      10   11    12
    QueryResult wres = WorldDatabase.Query("SELECT id,mapid,x,y,z,o,zoneId,areaId,minlevel,maxlevel,flags,name,links FROM creature_template_npcbot_wander_nodes ORDER BY mapid,id");
    if (!wres)
    {
        LOG_FATAL("server.loading", "Failed to load wander points: table `creature_template_npcbot_wander_nodes` is empty!");
        ASSERT(false);
    }

    const uint32 maxof_minclasslvl_nr = GetMinLevelForBotClass(BOT_CLASS_DEATH_KNIGHT); // 55
    const uint32 maxof_minclasslvl_ex = GetMinLevelForBotClass(BOT_CLASS_DREADLORD); // 60
    SpawnVector all_spawn_nodes;
    SpawnMapEx spawn_node_exists_a;
    SpawnMapEx spawn_node_exists_h;
    SpawnMapEx spawn_node_exists_n;
    std::unordered_map<uint32, std::pair<WanderNode*, std::vector<std::pair<std::string, std::string>>>> links_to_create;

    all_spawn_nodes.reserve(wres->GetRowCount() >> 8);
    for (SpawnMapEx* smap : { &spawn_node_exists_a, &spawn_node_exists_h, &spawn_node_exists_n })
        for (uint32 mapId : ALL_CONTINENT_MAPS)
            if (BotMgr::IsBotGenerationEnabledWorldMapId(mapId))
                smap->emplace(mapId, false);

    uint32 disabled_nodes = 0;
    do
    {
        Field* fields = wres->Fetch();
        uint32 index = 0;

        uint32 id             = fields[  index].Get<uint32>();
        uint32 mapId          = fields[++index].Get<uint16>();
        float x               = fields[++index].Get<float>();
        float y               = fields[++index].Get<float>();
        float z               = fields[++index].Get<float>();
        float o               = fields[++index].Get<float>();
        uint32 zoneId         = fields[++index].Get<uint32>();
        uint32 areaId         = fields[++index].Get<uint32>();
        uint8 minLevel        = fields[++index].Get<uint8>();
        uint8 maxLevel        = fields[++index].Get<uint8>();
        uint32 flags          = fields[++index].Get<uint32>();
        std::string name      = fields[++index].Get<std::string>();
        std::string_view lstr = fields[++index].Get<std::string_view>();

        WanderNode::nextWPId = std::max<uint32>(WanderNode::nextWPId, id);

        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        if (!mapEntry)
        {
            LOG_ERROR("server.loading", "WP {} has invalid map id {}!", id, mapId);
            continue;
        }

        if (minLevel == 1u && maxLevel == DEFAULT_MAX_LEVEL)
            LOG_WARN("server.loading", "WP {} has no levels set.", id);

        if (!minLevel || !maxLevel || minLevel > DEFAULT_MAX_LEVEL || maxLevel > DEFAULT_MAX_LEVEL || minLevel > maxLevel)
        {
            LOG_WARN("server.loading", "WP {} has invalid levels min {} max {}! Setting to default...",
                id, uint32(minLevel), uint32(maxLevel));
            minLevel = 1;
            maxLevel = DEFAULT_MAX_LEVEL;
        }

        if (flags >= AsUnderlyingType(BotWPFlags::BOTWP_FLAG_END))
        {
            LOG_WARN("server.loading", "WP {} has invalid flags {}! Removing all invalid flags...", id, flags);
            flags &= (AsUnderlyingType(BotWPFlags::BOTWP_FLAG_END) - 1);
        }

        const uint32 nonbg_flags = AsUnderlyingType(BotWPFlags::BOTWP_FLAG_BG_FLAG_PICKUP_TARGET) | AsUnderlyingType(BotWPFlags::BOTWP_FLAG_BG_FLAG_DELIVER_TARGET);
        if ((flags & nonbg_flags) && !mapEntry->IsBattleground())
        {
            LOG_WARN("server.loading", "WP {} has BG-only flags {} for non-BG map {}! Removing...", id, (flags & nonbg_flags), mapEntry->MapID);
            flags &= ~nonbg_flags;
        }

        const uint32 conflicting_flags_1 = AsUnderlyingType(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY) | AsUnderlyingType(BotWPFlags::BOTWP_FLAG_HORDE_ONLY);
        if ((flags & conflicting_flags_1) == conflicting_flags_1)
        {
            LOG_WARN("server.loading", "WP {} has conflicting flags {}+{}! Removing both...",
                id, AsUnderlyingType(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY), AsUnderlyingType(BotWPFlags::BOTWP_FLAG_HORDE_ONLY));
            flags &= ~conflicting_flags_1;
        }

        if (mapEntry->IsContinent() && !BotMgr::IsBotGenerationEnabledWorldMapId(mapId))
        {
            ++disabled_nodes;
            continue;
        }

        WanderNode* wp = new WanderNode(id, mapId, x, y, z, o, zoneId, areaId, name);
        wp->SetLevels(minLevel, maxLevel);
        wp->SetFlags(BotWPFlags(flags));

        if (wp->HasFlag(BotWPFlags::BOTWP_FLAG_SPAWN) && !lstr.empty())
            all_spawn_nodes.push_back(wp);

        if (lstr.empty())
        {
            LOG_ERROR("server.loading", "WP {} has no links!", id);
            continue;
        }
        std::vector<std::string_view> tok = Acore::Tokenize(lstr, ' ', false);
        for (std::vector<std::string_view>::size_type i = 0; i != tok.size(); ++i)
        {
            std::vector<std::string_view> link_str = Acore::Tokenize(tok[i], ':', false);
            ASSERT(link_str.size() == 2u, "Invalid links_str format: '{}'", std::string(tok[i].data(), tok[i].length()).c_str());
            ASSERT(link_str[0].find(" ") == std::string_view::npos);
            ASSERT(link_str[1].find(" ") == std::string_view::npos);
            ASSERT(Acore::StringTo<uint32>(link_str[0]) != std::nullopt, "Invalid links_str format: '{}'", std::string(tok[i].data(), tok[i].length()).c_str());
            ASSERT(Acore::StringTo<uint32>(link_str[1]) != std::nullopt, "Invalid links_str format: '{}'", std::string(tok[i].data(), tok[i].length()).c_str());

            std::pair<std::string, std::string> tok_pair = { std::string(link_str[0].data(), link_str[0].length()), std::string(link_str[1].data(), link_str[1].length()) };
            if (links_to_create.find(id) == links_to_create.cend())
                links_to_create[id] = { wp, {std::move(tok_pair)} };
            else
                links_to_create.at(id).second.push_back(std::move(tok_pair));
        }

    } while (wres->NextRow());

    for (WanderNode const* wp : all_spawn_nodes)
    {
        uint32 mapId = wp->GetMapId();
        auto [minLevel, maxLevel] = wp->GetLevels();

        spawn_node_exists_a[mapId] |= (maxLevel >= maxof_minclasslvl_nr && !wp->HasFlag(BotWPFlags::BOTWP_FLAG_HORDE_ONLY));
        spawn_node_exists_h[mapId] |= (maxLevel >= maxof_minclasslvl_nr && !wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY));
        spawn_node_exists_n[mapId] |= (maxLevel >= maxof_minclasslvl_ex && !wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_OR_HORDE_ONLY));

        decltype(_wpMinSpawnLevelPerMapId)::const_iterator mincit = _wpMinSpawnLevelPerMapId.find(mapId);
        _wpMinSpawnLevelPerMapId[mapId] = std::min<uint8>((mincit != _wpMinSpawnLevelPerMapId.cend()) ? mincit->second : uint8(DEFAULT_MAX_LEVEL), minLevel);
        decltype(_wpMaxSpawnLevelPerMapId)::const_iterator maxcit = _wpMaxSpawnLevelPerMapId.find(mapId);
        _wpMaxSpawnLevelPerMapId[mapId] = std::max<uint8>((maxcit != _wpMaxSpawnLevelPerMapId.cend()) ? maxcit->second : 1u, maxLevel);
    }

    bool spawn_node_minclasslvl_exists_all = true;
    for (auto& kv : spawn_node_exists_a)
    {
        if (!kv.second)
        {
            LOG_FATAL("server.loading", "No valid Alliance spawn node for at least level {} on map {}! Spawning wandering bots is impossible! Aborting.",
                maxof_minclasslvl_nr, kv.first);
            spawn_node_minclasslvl_exists_all = false;
        }
    }
    for (auto& kv : spawn_node_exists_h)
    {
        if (!kv.second)
        {
            LOG_FATAL("server.loading", "No valid Horde spawn node for at least level {} on map {}! Spawning wandering bots is impossible! Aborting.",
                maxof_minclasslvl_nr, kv.first);
            spawn_node_minclasslvl_exists_all = false;
        }
    }
    for (auto& kv : spawn_node_exists_n)
    {
        if (!kv.second)
        {
            if (sMapStore.LookupEntry(kv.first)->IsBattlegroundOrArena())
                LOG_INFO("server.loading", "No valid Neutral spawn node for at least level {} on non-continent map {}.", maxof_minclasslvl_ex, kv.first);
            else
            {
                LOG_FATAL("server.loading", "No valid Neutral spawn node for at least level {} on map {}! Spawning wandering bots is impossible! Aborting.",
                    maxof_minclasslvl_ex, kv.first);
                spawn_node_minclasslvl_exists_all = false;
            }
        }
    }
    if (!spawn_node_minclasslvl_exists_all)
        ABORT();

    const uint8 TEAMS_COUNT = TEAM_NEUTRAL + 1;
    char const* const team_strs[TEAMS_COUNT] = { "Alliance", "Horde", "Neutral" };
    std::array<bool, DEFAULT_MAX_LEVEL> spawn_node_levels[TEAMS_COUNT]{ { false } };
    uint8 min_spawn_level = DEFAULT_MAX_LEVEL;
    uint8 max_spawn_level = 0;
    for (WanderNode const* wp : all_spawn_nodes)
    {
        if (sMapStore.LookupEntry(wp->GetMapId())->IsContinent() && BotMgr::IsBotGenerationEnabledWorldMapId(wp->GetMapId()))
        {
            auto [minLevel, maxLevel] = wp->GetLevels();
            min_spawn_level = std::min<uint32>(min_spawn_level, minLevel);
            max_spawn_level = std::max<uint32>(max_spawn_level, maxLevel);
        }
    }
    for (WanderNode const* wp : all_spawn_nodes)
    {
        if (sMapStore.LookupEntry(wp->GetMapId())->IsContinent())
        {
            auto [minLevel, maxLevel] = wp->GetLevels();
            minLevel = std::max<uint8>(minLevel, 1);
            maxLevel = std::min<uint8>(maxLevel, max_spawn_level);
            for (uint8 k = 0; k < TEAMS_COUNT; ++k)
            {
                if ((k == 0 && !wp->HasFlag(BotWPFlags::BOTWP_FLAG_HORDE_ONLY)) ||
                    (k == 1 && !wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY)) ||
                    (k == 2 && !wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_OR_HORDE_ONLY)))
                {
                    for (uint8 i = minLevel; i <= maxLevel; ++i)
                        spawn_node_levels[k][i - 1] = true;
                }
            }
        }
    }
    for (uint8 k = 0; k < TEAMS_COUNT; ++k)
    {
        auto const& vec = spawn_node_levels[k];
        for (uint32 i = min_spawn_level; i <= max_spawn_level; ++i)
        {
            if (vec[i - 1] == false)
                LOG_ERROR("server.loading", "No {} spawn node found for level {}! Wandering bots may cause a crash!", team_strs[k], i);
        }
    }

    float mindist = 50000.f;
    float maxdist = 0.f;
    for (auto const& vt : links_to_create)
    {
        for (auto const& p : vt.second.second)
        {
            uint32 lid = *Acore::StringTo<uint32>(p.first);
            if (lid == vt.first)
            {
                LOG_ERROR("server.loading", "WP {} has link {} which links to itself! Skipped.", vt.first, lid);
                continue;
            }

            WanderNode* lwp = WanderNode::FindInAllWPs(lid);
            if (!lwp)
            {
                LOG_ERROR("server.loading", "WP {} has link {} which does not exist!", vt.first, lid);
                continue;
            }
            if (lwp->GetMapId() != vt.second.first->GetMapId())
            {
                LOG_ERROR("server.loading", "WP {} map {} has link {} ON A DIFFERENT MAP {}!", vt.first, vt.second.first->GetMapId(), lid, lwp->GetMapId());
                continue;
            }

            bool is_continent = sMapStore.LookupEntry(vt.second.first->GetMapId())->IsContinent();
            float lwpdist2d = vt.second.first->GetExactDist2d(lwp);
            if (lwpdist2d > MAX_WANDER_NODE_DISTANCE)
                LOG_WARN("server.loading", "Warning! Link distance between WP {} and {} is too great ({})", vt.first, lid, lwpdist2d);
            if (lwpdist2d < MIN_WANDER_NODE_DISTANCE && is_continent)
                LOG_WARN("server.loading", "Warning! Link distance between WP {} and {} is low ({})", vt.first, lid, lwpdist2d);

            vt.second.first->Link(lwp, true);

            if (is_continent)
            {
                float dist2d = vt.second.first->GetExactDist2d(lwp);
                if (dist2d < mindist)
                    mindist = dist2d;
                if (dist2d > maxdist)
                    maxdist = dist2d;
            }
        }
    }

    std::set<WanderNode const*> tops;
    WanderNode::DoForAllWPs([&](WanderNode const* wp) {
        if (tops.count(wp) == 0u && wp->GetLinks().size() == 1u)
        {
            LOG_DEBUG("server.loading", "Node {} ('{}') has single connection!", wp->GetWPId(), wp->GetName().c_str());
            WanderNode const* tn = wp->GetLinks().front();
            std::vector<WanderNode const*> sc_chain;
            sc_chain.push_back(wp);
            tops.emplace(wp);
            while (tn != wp)
            {
                if (tn->GetLinks().size() != 2u)
                {
                    sc_chain.push_back(tn);
                    break;
                }
                uint32 prevId = sc_chain.back()->GetWPId();
                sc_chain.push_back(tn);
                tn = *std::find_if_not(std::cbegin(tn->GetLinks()), std::cend(tn->GetLinks()), [nId = prevId](WanderNode const* lwp) {
                    return lwp->GetWPId() == nId;
                });
            }
            if (sc_chain.back()->GetLinks().size() == 1u)
            {
                LOG_DEBUG("server.loading", "Node {} ('{}') has single connection!", tn->GetWPId(), tn->GetName().c_str());
                tops.emplace(sc_chain.back());
                std::ostringstream ss;
                ss << "Node " << (sc_chain.size() == 2u ? "pair " : "chain ");
                for (uint32 i = 0u; i < sc_chain.size(); ++i)
                {
                    ss << sc_chain[i]->GetWPId();
                    if (i < sc_chain.size() - 1u)
                        ss << '-';
                }
                ss << " is isolated!";
                LOG_INFO("server.loading", ss.str().c_str());
            }
        }
    });

    LOG_INFO("server.loading", ">> Loaded {} bot wander nodes on {} maps (total {} tops) in {} ms",
        uint32(WanderNode::GetAllWPsCount()), disabled_nodes, uint32(WanderNode::GetWPMapsCount()), uint32(tops.size()), GetMSTimeDiffToNow(botoldMSTime));
}

void BotDataMgr::GenerateWanderingBots()
{
    const uint32 wandering_bots_desired = BotMgr::GetDesiredWanderingBotsCount();

    if (wandering_bots_desired == 0)
        return;

    LOG_INFO("server.loading", "Spawning wandering bots...");

    uint32 oldMSTime = getMSTime();

    uint32 maxbots = sBotGen->GetSpareBotsCount();
    uint32 enabledbots = sBotGen->GetEnabledBotsCount();

    if (maxbots < wandering_bots_desired)
    {
        LOG_FATAL("server.loading", "Only {} out of {} bots of enabled classes aren't spawned. Desired amount of wandering bots ({}) cannot be created. Aborting!",
            maxbots, enabledbots, wandering_bots_desired);
        ASSERT(false);
    }

    uint32 spawned_count = 0;
    if (!sBotGen->GenerateWanderingBotsToSpawn(wandering_bots_desired, -1, -1, false, nullptr, nullptr, spawned_count))
    {
        LOG_FATAL("server.loading", "Failed to spawn all {} bots ({} succeeded)!", wandering_bots_desired, spawned_count);
        ASSERT(false);
    }

    LOG_INFO("server.loading", ">> Set up spawning of {} wandering bots in {} ms", spawned_count, GetMSTimeDiffToNow(oldMSTime));
}

bool BotDataMgr::GenerateBattlegroundBots(Player const* groupLeader, [[maybe_unused]] Group const* group, BattlegroundQueue* queue, PvPDifficultyEntry const* bracketEntry, GroupQueueInfo const* gqinfo)
{
    if (!BotMgr::IsBotGenerationEnabledBGs())
        return true;

    BattlegroundTypeId bgTypeId = gqinfo->BgTypeId;
    uint8 atype = gqinfo->ArenaType;
    uint32 ammr = gqinfo->ArenaMatchmakerRating;
    BattlegroundBracketId bracketId = bracketEntry->GetBracketId();
    BattlegroundQueueTypeId bgqTypeId = sBattlegroundMgr->BGQueueTypeId(bgTypeId, atype);

    uint32 tarteamplayers = BotMgr::GetBGTargetTeamPlayersCount(bgTypeId);

    if (tarteamplayers == 0)
    {
        LOG_INFO("npcbots", "[Disabled] BG {} wandering bots generation is disabled (not implemented?)", uint32(bgTypeId));
        return true;
    }

    //find running BG
    auto const& all_bgs = sBattlegroundMgr->GetBgDataStore();
    for (auto const& kv : all_bgs)
    {
        if (kv.first == bgTypeId)
        {
            for (auto const& real_bg_pair : kv.second._Battlegrounds)
            {
                Battleground const* real_bg = real_bg_pair.second;
                if (real_bg->GetInstanceID() != 0 && real_bg->GetBracketId() == bracketId &&
                    real_bg->GetStatus() < STATUS_WAIT_LEAVE && real_bg->HasFreeSlots())
                {
                    LOG_INFO("npcbots", "[Already running] Found running BG {} inited by player {} ({}). Not generating bots",
                        uint32(bgTypeId), groupLeader->GetName().c_str(), groupLeader->GetGUID().GetCounter());
                    return true;
                }
            }
        }
    }

    Battleground const* bg_template = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);

    if (!bg_template)
        return false;

    uint32 minteamplayers = bg_template->GetMinPlayersPerTeam();
    uint32 maxteamplayers = bg_template->GetMaxPlayersPerTeam();

    RoundToInterval(tarteamplayers, minteamplayers, maxteamplayers);

    uint32 queued_players_a = 0;
    uint32 queued_players_h = 0;
    for (uint8 i = 0; i < BG_QUEUE_CFBG; ++i)
    {
        for (GroupQueueInfo const* qgr : queue->m_QueuedGroups[bracketId][i])
        {
            if (qgr->teamId == TEAM_ALLIANCE)
                queued_players_a += qgr->Players.size();
            else
                queued_players_h += qgr->Players.size();
        }
    }

    uint32 needed_bots_count_a = (queued_players_a < tarteamplayers) ? (tarteamplayers - queued_players_a) : 0;
    uint32 needed_bots_count_h = (queued_players_h < tarteamplayers) ? (tarteamplayers - queued_players_h) : 0;

    ASSERT(needed_bots_count_a <= maxteamplayers);
    ASSERT(needed_bots_count_h <= maxteamplayers);

    if (needed_bots_count_a + needed_bots_count_h == 0)
    {
        LOG_INFO("npcbots", "[No bots required] Failed to generate bots for BG {} inited by player {} ({})",
            uint32(bgTypeId), groupLeader->GetName().c_str(), groupLeader->GetGUID().GetCounter());
        return true;
    }

    uint32 spare_bots_a = sBotGen->GetSpareBotsCount(TEAM_ALLIANCE);
    uint32 spare_bots_h = sBotGen->GetSpareBotsCount(TEAM_HORDE);

    if (queued_players_a + spare_bots_a < minteamplayers)
    {
        LOG_INFO("npcbots", "[Not enough A bots] Failed to generate bots for BG {} inited by player {} ({})",
            uint32(bgTypeId), groupLeader->GetName().c_str(), groupLeader->GetGUID().GetCounter());
        return false;
    }
    if (queued_players_h + spare_bots_h < minteamplayers)
    {
        LOG_INFO("npcbots", "[Not enough H bots] Failed to generate bots for BG {} inited by player {} ({})",
            uint32(bgTypeId), groupLeader->GetName().c_str(), groupLeader->GetGUID().GetCounter());
        return false;
    }

    needed_bots_count_a = std::min<uint32>(needed_bots_count_a, spare_bots_a);
    needed_bots_count_h = std::min<uint32>(needed_bots_count_h, spare_bots_h);

    uint32 spawned_a = 0;
    uint32 spawned_h = 0;
    NpcBotRegistry spawned_bots_a;
    NpcBotRegistry spawned_bots_h;

    if (needed_bots_count_a)
    {
        if (!sBotGen->GenerateWanderingBotsToSpawn(needed_bots_count_a, bg_template->GetMapId(), ALLIANCE, true, bracketEntry, &spawned_bots_a, spawned_a))
        {
            LOG_WARN("npcbots", "Failed to spawn {} ALLIANCE bots for BG {} '{}' queued A {} H {} req A {} H {} spare {}",
                needed_bots_count_a, uint32(bg_template->GetBgTypeID()), bg_template->GetName().c_str(),
                queued_players_a, queued_players_h, needed_bots_count_a, needed_bots_count_h, spare_bots_a);
            for (NpcBotRegistry const* registry1 : { &spawned_bots_a, &spawned_bots_h })
                for (Creature const* bot : *registry1)
                    DespawnWandererBot(bot->GetEntry());
            return false;
        }
    }
    if (needed_bots_count_h)
    {
        if (!sBotGen->GenerateWanderingBotsToSpawn(needed_bots_count_h, bg_template->GetMapId(), HORDE, true, bracketEntry, &spawned_bots_h, spawned_h))
        {
            LOG_WARN("npcbots", "Failed to spawn {} HORDE bots for BG {} '{}' queued A {} H {} req A {} H {} spare {}",
                needed_bots_count_h, uint32(bg_template->GetBgTypeID()), bg_template->GetName().c_str(),
                queued_players_a, queued_players_h, needed_bots_count_a, needed_bots_count_h, spare_bots_h);
            for (NpcBotRegistry const* registry2 : { &spawned_bots_a, &spawned_bots_h })
                for (Creature const* bot : *registry2)
                    DespawnWandererBot(bot->GetEntry());
            return false;
        }
    }

    ASSERT(uint32(spawned_bots_a.size()) == needed_bots_count_a);
    ASSERT(uint32(spawned_bots_h.size()) == needed_bots_count_h);

    botBGJoinEvents[groupLeader->GetGUID()].AddEventAtOffset([ammr = ammr, atype = atype, bgqTypeId = bgqTypeId, bgTypeId = bgTypeId, bracketId = bracketId]() {
        sBattlegroundMgr->ScheduleQueueUpdate(ammr, atype, bgqTypeId, bgTypeId, bracketId);
    }, Seconds(2));

    for (NpcBotRegistry const* registry3 : { &spawned_bots_a, &spawned_bots_h })
    {
        uint32 seconds_delay = 5;
        for (Creature const* bot : *registry3)
        {
            bot->GetBotAI()->SetBotCommandState(BOT_COMMAND_STAY);
            bot->GetBotAI()->canUpdate = false;

            const_cast<Creature*>(bot)->SetPvP(true);
            queue->AddBotAsGroup(bot->GetGUID(), GetTeamIdForFaction(bot->GetFaction()),
                bgTypeId, bracketEntry, atype, false, gqinfo->ArenaTeamRating, ammr);

            seconds_delay += std::max<uint32>(1u, uint32((MINUTE / 2) / std::min<uint32>(needed_bots_count_a, needed_bots_count_h)));

            BotBattlegroundEnterEvent* bbe = new BotBattlegroundEnterEvent(groupLeader->GetGUID(), bot->GetGUID(), bgqTypeId,
                botBGJoinEvents[groupLeader->GetGUID()].CalculateTime(Milliseconds(uint32(INVITE_ACCEPT_WAIT_TIME) + uint32(BG_START_DELAY_2M)).count()));
            botBGJoinEvents[groupLeader->GetGUID()].AddEventAtOffset(bbe, Seconds(seconds_delay));
        }
    }

    return true;
}

ItemPerBotClassMap const& BotDataMgr::GetWanderingBotsSortedGearMap()
{
    return _botsWanderCreaturesSortedGear;
}

void BotDataMgr::CreateWanderingBotsSortedGear()
{
    LOG_INFO("server.loading", "Sorting wandering bot's gear...");

    uint32 oldMSTime = getMSTime();

    std::set<uint32> disabled_item_ids;
    QueryResult dires = WorldDatabase.Query("SELECT id FROM creature_template_npcbot_disabled_items");
    if (dires)
    {
        do
        {
            uint32 id = dires->Fetch()->Get<uint32>();
            disabled_item_ids.insert(id);

        } while (dires->NextRow());

        LOG_INFO("server.loading", ">> Loaded {} disabled wandering bots gear items", uint32(disabled_item_ids.size()));
    }
    else
        LOG_INFO("server.loading", ">> Loaded 0 disabled wandering bots gear items. Table `creature_template_npcbot_disabled_items` is empty!");

    const std::map<uint32, uint8> InvTypeToBotSlot = {
        {INVTYPE_HEAD, BOT_SLOT_HEAD},
        {INVTYPE_SHOULDERS, BOT_SLOT_SHOULDERS},
        {INVTYPE_CHEST, BOT_SLOT_CHEST},
        {INVTYPE_ROBE, BOT_SLOT_CHEST},
        {INVTYPE_WAIST, BOT_SLOT_WAIST},
        {INVTYPE_LEGS, BOT_SLOT_LEGS},
        {INVTYPE_FEET, BOT_SLOT_FEET},
        {INVTYPE_WRISTS, BOT_SLOT_WRIST},
        {INVTYPE_HANDS, BOT_SLOT_HANDS}
    };

    auto push_gear_to_classes = [](ItemTemplate const& itt, uint8 slot, uint8 lstep, std::initializer_list<BotClasses> const& cs) {
        for (BotClasses c : cs)
        {
            if (c == BOT_CLASS_SPHYNX &&
                (itt.InventoryType == INVTYPE_FINGER || itt.InventoryType == INVTYPE_TRINKET || itt.InventoryType == INVTYPE_CLOAK || itt.InventoryType == INVTYPE_NECK || itt.InventoryType == INVTYPE_SHIELD))
                continue;
            if (!itt.AllowableClass || !!(itt.AllowableClass & ((1u << MAX_CLASSES) - 1)) || !!(itt.AllowableClass & (1 << (c - 1))))
                _botsWanderCreaturesSortedGear[c][slot][lstep].push_back(itt.ItemId);
        }
    };

    const std::initializer_list<BotClasses> IntUsers = { BOT_CLASS_PALADIN, BOT_CLASS_HUNTER, BOT_CLASS_PRIEST, BOT_CLASS_SHAMAN, BOT_CLASS_MAGE, BOT_CLASS_WARLOCK, BOT_CLASS_DRUID, BOT_CLASS_SPHYNX, BOT_CLASS_ARCHMAGE, BOT_CLASS_DREADLORD, BOT_CLASS_NECROMANCER, BOT_CLASS_SEA_WITCH, BOT_CLASS_CRYPT_LORD };
    const std::initializer_list<BotClasses> StrUsers = { BOT_CLASS_WARRIOR, BOT_CLASS_ROGUE, BOT_CLASS_DEATH_KNIGHT, BOT_CLASS_BM, BOT_CLASS_SPELLBREAKER, BOT_CLASS_DARK_RANGER };

    ItemTemplateContainer const* all_item_templates = sObjectMgr->GetItemTemplateStore();
    for (auto const& kv : *all_item_templates)
    {
        ItemTemplate const& proto = kv.second;

        if (!proto.ItemLevel || proto.RequiredSpell)
            continue;

        bool skip = false;
        switch (proto.Quality)
        {
            case ITEM_QUALITY_NORMAL:
                if (std::any_of(std::cbegin(proto.Spells), std::cend(proto.Spells), [](_Spell const& spell) { return !!spell.SpellId; }))
                    skip = true;
                if (proto.RequiredLevel > 14)
                    skip = true;
                break;
            case ITEM_QUALITY_UNCOMMON:
                if (proto.RequiredLevel > 75)
                    skip = true;
            [[fallthrough]];
            case ITEM_QUALITY_RARE:
                if (proto.RequiredLevel > 75 && proto.RequiredLevel < 80)
                    skip = true;
            [[fallthrough]];
            case ITEM_QUALITY_EPIC:
                if (!(proto.RequiredLevel >= 2 && proto.RequiredLevel <= DEFAULT_MAX_LEVEL))
                    skip = true;
                if (!proto.RandomProperty && !proto.RandomSuffix && !proto.StatsCount &&
                    !(proto.Class == ITEM_CLASS_WEAPON && proto.SubClass == ITEM_SUBCLASS_WEAPON_WAND) &&
                    !(proto.Class == ITEM_CLASS_WEAPON && proto.SubClass == ITEM_SUBCLASS_WEAPON_BOW && proto.RequiredLevel < 45))
                    skip = true;
                break;
            default:
                skip = true;
                break;
        }
        if (skip)
            continue;

        if (!!disabled_item_ids.count(proto.ItemId))
        {
            //TC_LOG_INFO("server.loading", "Item %u is disabled...", proto.ItemId);
            continue;
        }

        if (std::any_of(std::cbegin(proto.ItemStat), std::cend(proto.ItemStat), [](_ItemStat const& stat) {
            return (stat.ItemStatType == ITEM_MOD_DEFENSE_SKILL_RATING || stat.ItemStatType == ITEM_MOD_DODGE_RATING ||
                stat.ItemStatType == ITEM_MOD_PARRY_RATING || stat.ItemStatType == ITEM_MOD_BLOCK_VALUE) &&
                stat.ItemStatValue > 0;
        }))
            continue;

        uint8 reqLstep = (((proto.RequiredLevel == 1) ? 0 : proto.RequiredLevel) + ITEM_SORTING_LEVEL_STEP - 1) / ITEM_SORTING_LEVEL_STEP;
        bool is_caster_item = proto.StatsCount > 0 && std::any_of(std::cbegin(proto.ItemStat), std::cend(proto.ItemStat), [](_ItemStat const& stat) {
            return (stat.ItemStatType == ITEM_MOD_INTELLECT || stat.ItemStatType == ITEM_MOD_SPELL_POWER ||
                stat.ItemStatType == ITEM_MOD_SPELL_PENETRATION || stat.ItemStatType == ITEM_MOD_MANA_REGENERATION) &&
                stat.ItemStatValue > 0;
        });

        switch (proto.Class)
        {
            case ITEM_CLASS_ARMOR:
                switch (proto.InventoryType)
                {
                    case INVTYPE_NECK:
                        if (proto.Quality < ITEM_QUALITY_UNCOMMON)
                            break;
                        if (is_caster_item)
                            push_gear_to_classes(proto, BOT_SLOT_NECK, reqLstep, IntUsers);
                        else
                            push_gear_to_classes(proto, BOT_SLOT_NECK, reqLstep, StrUsers);
                        break;
                    case INVTYPE_FINGER:
                        if (proto.Quality < ITEM_QUALITY_UNCOMMON)
                            break;
                        if (is_caster_item || proto.RequiredLevel < 55)
                        {
                            push_gear_to_classes(proto, BOT_SLOT_FINGER1, reqLstep, IntUsers);
                            push_gear_to_classes(proto, BOT_SLOT_FINGER2, reqLstep, IntUsers);
                        }
                        if (!is_caster_item)
                        {
                            push_gear_to_classes(proto, BOT_SLOT_FINGER1, reqLstep, StrUsers);
                            push_gear_to_classes(proto, BOT_SLOT_FINGER2, reqLstep, StrUsers);
                        }
                        break;
                    case INVTYPE_TRINKET:
                        if (proto.Quality < ITEM_QUALITY_UNCOMMON)
                            break;
                        if (!is_caster_item)
                        {
                            push_gear_to_classes(proto, BOT_SLOT_TRINKET1, reqLstep, StrUsers);
                            push_gear_to_classes(proto, BOT_SLOT_TRINKET2, reqLstep, StrUsers);
                        }
                        push_gear_to_classes(proto, BOT_SLOT_TRINKET1, reqLstep, IntUsers);
                        push_gear_to_classes(proto, BOT_SLOT_TRINKET2, reqLstep, IntUsers);
                        break;
                    case INVTYPE_CLOAK:
                        if (!is_caster_item || proto.StatsCount == 0)
                            push_gear_to_classes(proto, BOT_SLOT_BACK, reqLstep, StrUsers);
                        if (is_caster_item || proto.StatsCount == 0)
                            push_gear_to_classes(proto, BOT_SLOT_BACK, reqLstep, IntUsers);
                        break;
                    case INVTYPE_HOLDABLE:
                        if (proto.Quality < ITEM_QUALITY_UNCOMMON)
                            break;
                        push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_PRIEST, BOT_CLASS_MAGE, BOT_CLASS_WARLOCK, BOT_CLASS_DRUID });
                        break;
                    case INVTYPE_SHIELD:
                        if (proto.Armor == 0)
                            break;
                        if (is_caster_item || proto.RequiredLevel < 60 || (proto.RequiredLevel < 69 && (proto.RandomProperty || proto.RandomSuffix)))
                            push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_SHAMAN, BOT_CLASS_SPELLBREAKER });
                        break;
                    case INVTYPE_HEAD:
                    case INVTYPE_SHOULDERS:
                    case INVTYPE_CHEST:
                    case INVTYPE_ROBE:
                    case INVTYPE_WAIST:
                    case INVTYPE_LEGS:
                    case INVTYPE_FEET:
                    case INVTYPE_WRISTS:
                    case INVTYPE_HANDS:
                    {
                        if (proto.Armor == 0)
                            break;
                        decltype(InvTypeToBotSlot)::const_iterator ci = InvTypeToBotSlot.find(proto.InventoryType);
                        ASSERT(ci != InvTypeToBotSlot.cend());
                        uint8 slot = ci->second;
                        switch (proto.SubClass)
                        {
                            case ItemSubclassArmor::ITEM_SUBCLASS_ARMOR_CLOTH:
                                if (slot == BOT_SLOT_CHEST && proto.InventoryType != INVTYPE_ROBE)
                                    break;
                                push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_PRIEST, BOT_CLASS_MAGE, BOT_CLASS_WARLOCK, BOT_CLASS_ARCHMAGE, BOT_CLASS_NECROMANCER, BOT_CLASS_SEA_WITCH });
                                break;
                            case ItemSubclassArmor::ITEM_SUBCLASS_ARMOR_LEATHER:
                                if (!is_caster_item)
                                    push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_ROGUE, BOT_CLASS_DARK_RANGER });
                                push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_DRUID });
                                if (proto.RequiredLevel < 40)
                                    push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_HUNTER, BOT_CLASS_SHAMAN });
                                break;
                            case ItemSubclassArmor::ITEM_SUBCLASS_ARMOR_MAIL:
                                if (proto.RequiredLevel < 40)
                                {
                                    if (!is_caster_item)
                                        push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_WARRIOR });
                                    push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_PALADIN });
                                }
                                else
                                    push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_HUNTER, BOT_CLASS_SHAMAN });
                                if (!is_caster_item)
                                    push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_BM, BOT_CLASS_SPELLBREAKER });
                                push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_SPHYNX, BOT_CLASS_CRYPT_LORD });
                                break;
                            case ItemSubclassArmor::ITEM_SUBCLASS_ARMOR_PLATE:
                                if (!is_caster_item)
                                    push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_WARRIOR, BOT_CLASS_DEATH_KNIGHT, BOT_CLASS_BM, BOT_CLASS_SPELLBREAKER });
                                if (is_caster_item || proto.RequiredLevel < 60 || (proto.RequiredLevel < 78 && (proto.RandomProperty || proto.RandomSuffix)))
                                    push_gear_to_classes(proto, slot, reqLstep, { BOT_CLASS_PALADIN, BOT_CLASS_SPHYNX, BOT_CLASS_DREADLORD, BOT_CLASS_CRYPT_LORD });
                                break;
                            default:
                                break;
                        }
                        break;
                    }
                    default:
                        break;
                }
                break;
            case ITEM_CLASS_WEAPON:
                if (proto.Damage[0].DamageMin < 1.0f || proto.Damage[0].DamageMax < 2.0f || proto.Delay < 1000)
                    break;
                switch (proto.SubClass)
                {
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_WAND:
                        if (proto.InventoryType != INVTYPE_RANGED && proto.InventoryType != INVTYPE_RANGEDRIGHT)
                            break;
                        push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_SPHYNX });
                        push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_SPHYNX });
                        push_gear_to_classes(proto, BOT_SLOT_RANGED, reqLstep, { BOT_CLASS_PRIEST, BOT_CLASS_MAGE, BOT_CLASS_WARLOCK });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_GUN:
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_CROSSBOW:
                        if (proto.InventoryType != INVTYPE_RANGED && proto.InventoryType != INVTYPE_RANGEDRIGHT)
                            break;
                        push_gear_to_classes(proto, BOT_SLOT_RANGED, reqLstep, { BOT_CLASS_WARRIOR, BOT_CLASS_ROGUE, BOT_CLASS_HUNTER });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_BOW:
                        if (proto.InventoryType != INVTYPE_RANGED && proto.InventoryType != INVTYPE_RANGEDRIGHT)
                            break;
                        push_gear_to_classes(proto, BOT_SLOT_RANGED, reqLstep, { BOT_CLASS_WARRIOR, BOT_CLASS_ROGUE, BOT_CLASS_HUNTER, BOT_CLASS_DARK_RANGER, BOT_CLASS_SEA_WITCH });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_THROWN:
                        if (proto.InventoryType != INVTYPE_THROWN)
                            break;
                        push_gear_to_classes(proto, BOT_SLOT_RANGED, reqLstep, { BOT_CLASS_WARRIOR, BOT_CLASS_ROGUE });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_STAFF:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        if (is_caster_item || proto.RequiredLevel < 50)
                            push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_PRIEST, BOT_CLASS_MAGE, BOT_CLASS_WARLOCK, BOT_CLASS_DRUID, BOT_CLASS_SHAMAN, BOT_CLASS_ARCHMAGE, BOT_CLASS_NECROMANCER, BOT_CLASS_DREADLORD, BOT_CLASS_CRYPT_LORD });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_AXE2:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        if (!is_caster_item)
                        {
                            push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR, BOT_CLASS_PALADIN, BOT_CLASS_DEATH_KNIGHT, BOT_CLASS_BM });
                            if (proto.RequiredLevel >= 60 - ITEM_SORTING_LEVEL_STEP)
                                push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_WARRIOR });
                        }
                        push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_HUNTER, BOT_CLASS_SHAMAN, BOT_CLASS_DREADLORD, BOT_CLASS_CRYPT_LORD });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_SWORD2:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        if (!is_caster_item)
                        {
                            push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR, BOT_CLASS_PALADIN, BOT_CLASS_DEATH_KNIGHT, BOT_CLASS_BM });
                            if (proto.RequiredLevel >= 60 - ITEM_SORTING_LEVEL_STEP)
                                push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_WARRIOR });
                        }
                        push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_HUNTER, BOT_CLASS_DREADLORD, BOT_CLASS_CRYPT_LORD });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_POLEARM:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        if (!is_caster_item)
                            push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR, BOT_CLASS_PALADIN, BOT_CLASS_DEATH_KNIGHT, BOT_CLASS_BM });
                        push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_DRUID, BOT_CLASS_HUNTER, BOT_CLASS_DREADLORD, BOT_CLASS_CRYPT_LORD });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_MACE2:
                        if (proto.InventoryType != INVTYPE_2HWEAPON)
                            break;
                        if (!is_caster_item)
                        {
                            push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR, BOT_CLASS_PALADIN, BOT_CLASS_HUNTER, BOT_CLASS_DEATH_KNIGHT });
                            if (proto.RequiredLevel >= 60 - ITEM_SORTING_LEVEL_STEP)
                                push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_WARRIOR });
                        }
                        push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_DRUID, BOT_CLASS_DREADLORD, BOT_CLASS_CRYPT_LORD });
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_AXE:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_ROGUE, BOT_CLASS_SPELLBREAKER });
                            }
                            push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_SHAMAN });
                        }
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_ROGUE });
                            }
                            push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_SHAMAN });
                        }
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_MACE:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_ROGUE, BOT_CLASS_SPELLBREAKER });
                            }
                            push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_SHAMAN });
                            if (is_caster_item || proto.RequiredLevel < 55 || (proto.RequiredLevel < 78 && (proto.RandomProperty || proto.RandomSuffix)))
                                push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_DRUID, BOT_CLASS_PRIEST });
                        }
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_ROGUE });
                            }
                            push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_SHAMAN });
                        }
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_SWORD:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_ROGUE, BOT_CLASS_SPELLBREAKER, BOT_CLASS_DARK_RANGER });
                            }
                            if (is_caster_item || proto.RequiredLevel < 55 || (proto.RequiredLevel < 78 && (proto.RandomProperty || proto.RandomSuffix)))
                                push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_MAGE, BOT_CLASS_WARLOCK });
                        }
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_ROGUE, BOT_CLASS_DARK_RANGER });
                            }
                        }
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_FIST:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_SHAMAN, BOT_CLASS_ROGUE, BOT_CLASS_SPELLBREAKER });
                            }
                        }
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_SHAMAN, BOT_CLASS_ROGUE });
                            }
                        }
                        break;
                    case ItemSubclassWeapon::ITEM_SUBCLASS_WEAPON_DAGGER:
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONMAINHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_ROGUE, BOT_CLASS_SPELLBREAKER, BOT_CLASS_DARK_RANGER });
                            }
                            push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_SHAMAN });
                            if (is_caster_item || proto.RequiredLevel < 55 || (proto.RequiredLevel < 78 && (proto.RandomProperty || proto.RandomSuffix)))
                                push_gear_to_classes(proto, BOT_SLOT_MAINHAND, reqLstep, { BOT_CLASS_PRIEST, BOT_CLASS_MAGE, BOT_CLASS_WARLOCK, BOT_CLASS_DRUID, BOT_CLASS_SEA_WITCH });
                        }
                        if (proto.InventoryType == INVTYPE_WEAPON || proto.InventoryType == INVTYPE_WEAPONOFFHAND)
                        {
                            if (!is_caster_item)
                            {
                                if (proto.RequiredLevel < 60 - ITEM_SORTING_LEVEL_STEP)
                                    push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_WARRIOR });
                                push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_ROGUE, BOT_CLASS_DARK_RANGER });
                            }
                            push_gear_to_classes(proto, BOT_SLOT_OFFHAND, reqLstep, { BOT_CLASS_SHAMAN, BOT_CLASS_SEA_WITCH });
                        }
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }

    for (uint32 c = BOT_CLASS_WARRIOR; c < BOT_CLASS_END; ++c)
    {
        if (c == 10)
            continue;
        ItemPerSlot const& ips_arr = _botsWanderCreaturesSortedGear.at(c);
        for (uint32 s = BOT_SLOT_MAINHAND; s < BOT_INVENTORY_SIZE; ++s)
        {
            if (s == BOT_SLOT_FINGER2 || s == BOT_SLOT_TRINKET1 || s == BOT_SLOT_TRINKET2 || s == BOT_SLOT_BODY)
                continue;
            if ((s == BOT_SLOT_FINGER1 || s == BOT_SLOT_NECK || s == BOT_SLOT_BACK) && c == BOT_CLASS_SPHYNX)
                continue;
            if (s == BOT_SLOT_RANGED && !(c == BOT_CLASS_HUNTER || c == BOT_CLASS_ROGUE || c == BOT_CLASS_WARRIOR || c == BOT_CLASS_PRIEST ||
                c == BOT_CLASS_MAGE || c == BOT_CLASS_WARLOCK || c == BOT_CLASS_DARK_RANGER || c == BOT_CLASS_SEA_WITCH))
                continue;
            ItemLeveledArr const& il_arr = ips_arr[s];
            for (uint32 lstep = 0; lstep < LEVEL_STEPS; ++lstep)
            {
                if ((s == BOT_SLOT_SHOULDERS || s == BOT_SLOT_FINGER1 || s == BOT_SLOT_NECK) && lstep < 4)
                    continue;
                if ((s == BOT_SLOT_HEAD || s == BOT_SLOT_TRINKET1) && lstep < 6)
                    continue;
                if (s == BOT_SLOT_OFFHAND &&
                    (lstep < 3 || c == BOT_CLASS_PALADIN || c == BOT_CLASS_HUNTER || c == BOT_CLASS_DEATH_KNIGHT || c == BOT_CLASS_BM || c == BOT_CLASS_ARCHMAGE ||
                        c == BOT_CLASS_SPHYNX || c == BOT_CLASS_DREADLORD || c == BOT_CLASS_NECROMANCER || c == BOT_CLASS_CRYPT_LORD))
                    continue;
                if ((c == BOT_CLASS_DREADLORD || c == BOT_CLASS_DEATH_KNIGHT) && lstep < 8)
                    continue;
                if (il_arr[lstep].empty())
                {
                    uint32 minlvl = std::max<uint32>(lstep * ITEM_SORTING_LEVEL_STEP, 1);
                    uint32 maxlvl = (lstep + 1) * ITEM_SORTING_LEVEL_STEP - 1;
                    LOG_WARN("server.loading", "No items for class {} slot {} at levels {}-{}!", c, s, minlvl, maxlvl);
                }
            }
        }
    }

    LOG_INFO("server.loading", ">> Sorted wandering bots gear in {} ms", GetMSTimeDiffToNow(oldMSTime));
}

Item* BotDataMgr::GenerateWanderingBotItem(uint8 slot, uint8 botclass, uint8 level, std::function<bool(ItemTemplate const*)>&& check)
{
    ASSERT(slot < BOT_INVENTORY_SIZE);
    ASSERT(botclass < BOT_CLASS_END);
    ASSERT(level <= DEFAULT_MAX_LEVEL + 4);

    uint8 lvl = level;
    ItemIdVector const* itemIdVec = &_botsWanderCreaturesSortedGear[botclass][slot][lvl / ITEM_SORTING_LEVEL_STEP];

    while (itemIdVec->empty() && lvl > ITEM_SORTING_LEVEL_STEP)
    {
        lvl -= ITEM_SORTING_LEVEL_STEP;
        itemIdVec = &_botsWanderCreaturesSortedGear[botclass][slot][lvl / ITEM_SORTING_LEVEL_STEP];
    }

    if (!itemIdVec->empty())
    {
        ItemIdVector validVec;
        validVec.reserve(itemIdVec->size() / 4);
        for (uint32 iid : *itemIdVec)
        {
            if (check(sObjectMgr->GetItemTemplate(iid)))
                validVec.push_back(iid);
        }

        if (!validVec.empty())
        {
            uint32 itemId = Acore::Containers::SelectRandomContainerElement(*itemIdVec);
            if (Item* newItem = Item::CreateItem(itemId, 1, nullptr))
            {
                if (uint32 randomPropertyId = Item::GenerateItemRandomPropertyId(itemId))
                    newItem->SetItemRandomProperties(randomPropertyId);

                return newItem;
            }
        }
    }

    return nullptr;
}

CreatureTemplate const* BotDataMgr::GetBotExtraCreatureTemplate(uint32 entry)
{
    CreatureTemplateContainer::const_iterator cit = _botsWanderCreatureTemplates.find(entry);
    return cit == _botsWanderCreatureTemplates.cend() ? nullptr : &cit->second;
}

EquipmentInfo const* BotDataMgr::GetBotEquipmentInfo(uint32 entry)
{
    decltype(_botsWanderCreatureEquipmentTemplates)::const_iterator cit = _botsWanderCreatureEquipmentTemplates.find(entry);
    if (cit == _botsWanderCreatureEquipmentTemplates.cend())
    {
        static int8 eqId = 1;
        return sObjectMgr->GetEquipmentInfo(entry, eqId);
    }
    else
        return cit->second;
}

void BotDataMgr::AddNpcBotData(uint32 entry, uint32 roles, uint8 spec, uint32 faction)
{
    //botData must be allocated explicitly
    NpcBotDataMap::iterator itr = _botsData.find(entry);
    if (itr == _botsData.end())
    {
        NpcBotData* botData = new NpcBotData(roles, faction, spec);
        _botsData[entry] = botData;

        CharacterDatabasePreparedStatement* bstmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_NPCBOT);
        //"INSERT INTO characters_npcbot (entry, roles, spec, faction) VALUES (?, ?, ?, ?)", CONNECTION_ASYNC);
        bstmt->SetData(0, entry);
        bstmt->SetData(1, roles);
        bstmt->SetData(2, spec);
        bstmt->SetData(3, faction);
        CharacterDatabase.Execute(bstmt);

        return;
    }

    LOG_ERROR("sql.sql", "BotMgr::AddNpcBotData(): trying to add new data but entry already exists! entry = {}", entry);
}
NpcBotData const* BotDataMgr::SelectNpcBotData(uint32 entry)
{
    NpcBotDataMap::const_iterator itr = _botsData.find(entry);
    return itr != _botsData.cend() ? itr->second : nullptr;
}
void BotDataMgr::UpdateNpcBotData(uint32 entry, NpcBotDataUpdateType updateType, void* data)
{
    NpcBotDataMap::iterator itr = _botsData.find(entry);
    if (itr == _botsData.end())
        return;

    CharacterDatabasePreparedStatement* bstmt;
    switch (updateType)
    {
        case NPCBOT_UPDATE_OWNER:
            if (itr->second->owner == *(uint32*)(data))
                break;
            itr->second->owner = *(uint32*)(data);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_OWNER);
            //"UPDATE characters_npcbot SET owner = ? WHERE entry = ?", CONNECTION_ASYNC
            bstmt->SetData(0, itr->second->owner);
            bstmt->SetData(1, entry);
            CharacterDatabase.Execute(bstmt);
            //break; //no break: erase transmogs
        [[fallthrough]];
        case NPCBOT_UPDATE_TRANSMOG_ERASE:
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_NPCBOT_TRANSMOG);
            //"DELETE FROM characters_npcbot_transmog WHERE entry = ?", CONNECTION_ASYNC
            bstmt->SetData(0, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        case NPCBOT_UPDATE_ROLES:
            itr->second->roles = *(uint32*)(data);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_ROLES);
            //"UPDATE character_npcbot SET roles = ? WHERE entry = ?", CONNECTION_ASYNC
            bstmt->SetData(0, itr->second->roles);
            bstmt->SetData(1, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        case NPCBOT_UPDATE_SPEC:
            itr->second->spec = *(uint8*)(data);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_SPEC);
            //"UPDATE characters_npcbot SET spec = ? WHERE entry = ?", CONNECTION_ASYNCH
            bstmt->SetData(0, itr->second->spec);
            bstmt->SetData(1, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        case NPCBOT_UPDATE_FACTION:
            itr->second->faction = *(uint32*)(data);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_FACTION);
            //"UPDATE characters_npcbot SET faction = ? WHERE entry = ?", CONNECTION_ASYNCH
            bstmt->SetData(0, itr->second->faction);
            bstmt->SetData(1, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        case NPCBOT_UPDATE_DISABLED_SPELLS:
        {
            NpcBotData::DisabledSpellsContainer const* spells = (NpcBotData::DisabledSpellsContainer const*)(data);
            std::ostringstream ss;
            for (NpcBotData::DisabledSpellsContainer::const_iterator citr = spells->begin(); citr != spells->end(); ++citr)
                ss << (*citr) << ' ';

            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_DISABLED_SPELLS);
            //"UPDATE characters_npcbot SET spells_disabled = ? WHERE entry = ?", CONNECTION_ASYNCH
            bstmt->SetData(0, ss.str());
            bstmt->SetData(1, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        }
        case NPCBOT_UPDATE_EQUIPS:
        {
            Item** items = (Item**)(data);

            EquipmentInfo const* einfo = BotDataMgr::GetBotEquipmentInfo(entry);

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_EQUIP);
            //"UPDATE character_npcbot SET equipMhEx = ?, equipOhEx = ?, equipRhEx = ?, equipHead = ?, equipShoulders = ?, equipChest = ?, equipWaist = ?, equipLegs = ?,
            //equipFeet = ?, equipWrist = ?, equipHands = ?, equipBack = ?, equipBody = ?, equipFinger1 = ?, equipFinger2 = ?, equipTrinket1 = ?, equipTrinket2 = ?, equipNeck = ? WHERE entry = ?", CONNECTION_ASYNC
            CharacterDatabasePreparedStatement* stmt;
            uint8 k;
            for (k = BOT_SLOT_MAINHAND; k != BOT_INVENTORY_SIZE; ++k)
            {
                itr->second->equips[k] = items[k] ? items[k]->GetGUID().GetCounter() : 0;
                if (Item const* botitem = items[k])
                {
                    bool standard = false;
                    for (uint8 i = 0; i != MAX_EQUIPMENT_ITEMS; ++i)
                    {
                        if (einfo->ItemEntry[i] == botitem->GetEntry())
                        {
                            itr->second->equips[k] = 0;
                            bstmt->SetData(k, uint32(0));
                            standard = true;
                            break;
                        }
                    }
                    if (standard)
                        continue;

                    uint8 index = 0;
                    stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_ITEM_INSTANCE);
                    //REPLACE INTO item_instance (itemEntry, owner_guid, creatorGuid, giftCreatorGuid, count, duration, charges, flags, enchantments, randomPropertyId, durability, playedTime, text, guid)
                    //VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", CONNECTION_ASYNC : 0-13
                    stmt->SetData(  index, botitem->GetEntry());
                    stmt->SetData(++index, botitem->GetOwnerGUID().GetCounter());
                    stmt->SetData(++index, botitem->GetGuidValue(ITEM_FIELD_CREATOR).GetCounter());
                    stmt->SetData(++index, botitem->GetGuidValue(ITEM_FIELD_GIFTCREATOR).GetCounter());
                    stmt->SetData(++index, botitem->GetCount());
                    stmt->SetData(++index, botitem->GetUInt32Value(ITEM_FIELD_DURATION));

                    std::ostringstream ssSpells;
                    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                        ssSpells << botitem->GetSpellCharges(i) << ' ';
                    stmt->SetData(++index, ssSpells.str());

                    stmt->SetData(++index, botitem->GetUInt32Value(ITEM_FIELD_FLAGS));

                    std::ostringstream ssEnchants;
                    for (uint8 i = 0; i < MAX_ENCHANTMENT_SLOT; ++i)
                    {
                        ssEnchants << botitem->GetEnchantmentId(EnchantmentSlot(i)) << ' ';
                        ssEnchants << botitem->GetEnchantmentDuration(EnchantmentSlot(i)) << ' ';
                        ssEnchants << botitem->GetEnchantmentCharges(EnchantmentSlot(i)) << ' ';
                    }
                    stmt->SetData(++index, ssEnchants.str());

                    stmt->SetData (++index, int16(botitem->GetItemRandomPropertyId()));
                    stmt->SetData(++index, uint16(botitem->GetUInt32Value(ITEM_FIELD_DURABILITY)));
                    stmt->SetData(++index, botitem->GetUInt32Value(ITEM_FIELD_CREATE_PLAYED_TIME));
                    stmt->SetData(++index, botitem->GetText());
                    stmt->SetData(++index, botitem->GetGUID().GetCounter());

                    trans->Append(stmt);

                    Item::DeleteFromInventoryDB(trans, botitem->GetGUID().GetCounter()); //prevent duplicates

                    bstmt->SetData(k, botitem->GetGUID().GetCounter());
                }
                else
                    bstmt->SetData(k, uint32(0));
            }

            bstmt->SetData(k, entry);
            trans->Append(bstmt);
            CharacterDatabase.CommitTransaction(trans);
            break;
        }
        case NPCBOT_UPDATE_ERASE:
        {
            NpcBotDataMap::iterator bitr = _botsData.find(entry);
            ASSERT(bitr != _botsData.end());
            delete bitr->second;
            _botsData.erase(bitr);
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_NPCBOT);
            //"DELETE FROM characters_npcbot WHERE entry = ?", CONNECTION_ASYNC
            bstmt->SetData(0, entry);
            CharacterDatabase.Execute(bstmt);
            break;
        }
        default:
            LOG_ERROR("sql.sql", "BotDataMgr:UpdateNpcBotData: unhandled updateType {}", uint32(updateType));
            break;
    }
}
void BotDataMgr::UpdateNpcBotDataAll(uint32 playerGuid, NpcBotDataUpdateType updateType, void* data)
{
    CharacterDatabasePreparedStatement* bstmt;
    switch (updateType)
    {
        case NPCBOT_UPDATE_OWNER:
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_NPCBOT_OWNER_ALL);
            //"UPDATE characters_npcbot SET owner = ? WHERE owner = ?", CONNECTION_ASYNC
            bstmt->SetData(0, *(uint32*)(data));
            bstmt->SetData(1, playerGuid);
            CharacterDatabase.Execute(bstmt);
            //break; //no break: erase transmogs
        [[fallthrough]];
        case NPCBOT_UPDATE_TRANSMOG_ERASE:
            bstmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_NPCBOT_TRANSMOG_ALL);
            //"DELETE FROM characters_npcbot_transmog WHERE entry IN (SELECT entry FROM characters_npcbot WHERE owner = ?)", CONNECTION_ASYNC
            bstmt->SetData(0, playerGuid);
            CharacterDatabase.Execute(bstmt);
            break;
        //case NPCBOT_UPDATE_ROLES:
        //case NPCBOT_UPDATE_FACTION:
        //case NPCBOT_UPDATE_EQUIPS:
        default:
            LOG_ERROR("sql.sql", "BotDataMgr:UpdateNpcBotDataAll: unhandled updateType {}", uint32(updateType));
            break;
    }
}

void BotDataMgr::SaveNpcBotStats(NpcBotStats const* stats)
{
    CharacterDatabasePreparedStatement* bstmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_NPCBOT_STATS);
    //"REPLACE INTO characters_npcbot_stats
    //(entry, maxhealth, maxpower, strength, agility, stamina, intellect, spirit, armor, defense,
    //resHoly, resFire, resNature, resFrost, resShadow, resArcane, blockPct, dodgePct, parryPct, critPct,
    //attackPower, spellPower, spellPen, hastePct, hitBonusPct, expertise, armorPenPct) VALUES
    //(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", CONNECTION_ASYNC

    uint32 index = 0;
    bstmt->SetData(  index, stats->entry);
    bstmt->SetData(++index, stats->maxhealth);
    bstmt->SetData(++index, stats->maxpower);
    bstmt->SetData(++index, stats->strength);
    bstmt->SetData(++index, stats->agility);
    bstmt->SetData(++index, stats->stamina);
    bstmt->SetData(++index, stats->intellect);
    bstmt->SetData(++index, stats->spirit);
    bstmt->SetData(++index, stats->armor);
    bstmt->SetData(++index, stats->defense);
    bstmt->SetData(++index, stats->resHoly);
    bstmt->SetData(++index, stats->resFire);
    bstmt->SetData(++index, stats->resNature);
    bstmt->SetData(++index, stats->resFrost);
    bstmt->SetData(++index, stats->resShadow);
    bstmt->SetData(++index, stats->resArcane);
    bstmt->SetData(++index, stats->blockPct);
    bstmt->SetData(++index, stats->dodgePct);
    bstmt->SetData(++index, stats->parryPct);
    bstmt->SetData(++index, stats->critPct);
    bstmt->SetData(++index, stats->attackPower);
    bstmt->SetData(++index, stats->spellPower);
    bstmt->SetData(++index, stats->spellPen);
    bstmt->SetData(++index, stats->hastePct);
    bstmt->SetData(++index, stats->hitBonusPct);
    bstmt->SetData(++index, stats->expertise);
    bstmt->SetData(++index, stats->armorPenPct);

    CharacterDatabase.Execute(bstmt);
}

NpcBotAppearanceData const* BotDataMgr::SelectNpcBotAppearance(uint32 entry)
{
    NpcBotAppearanceDataMap::const_iterator itr = _botsAppearanceData.find(entry);
    return itr != _botsAppearanceData.cend() ? itr->second : nullptr;
}

NpcBotExtras const* BotDataMgr::SelectNpcBotExtras(uint32 entry)
{
    NpcBotExtrasMap::const_iterator itr = _botsExtras.find(entry);
    return itr != _botsExtras.cend() ? itr->second : nullptr;
}

NpcBotTransmogData const* BotDataMgr::SelectNpcBotTransmogs(uint32 entry)
{
    NpcBotTransmogDataMap::const_iterator itr = _botsTransmogData.find(entry);
    return itr != _botsTransmogData.cend() ? itr->second : nullptr;
}
void BotDataMgr::UpdateNpcBotTransmogData(uint32 entry, uint8 slot, uint32 item_id, int32 fake_id, bool update_db)
{
    ASSERT(slot < BOT_TRANSMOG_INVENTORY_SIZE);

    NpcBotTransmogDataMap::const_iterator itr = _botsTransmogData.find(entry);
    if (itr == _botsTransmogData.cend())
        _botsTransmogData[entry] = new NpcBotTransmogData();

    _botsTransmogData[entry]->transmogs[slot] = { item_id, fake_id };

    if (update_db)
    {
        CharacterDatabasePreparedStatement* bstmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_NPCBOT_TRANSMOG);
        //"REPLACE INTO characters_npcbot_transmog (entry, slot, item_id, fake_id) VALUES (?, ?, ?, ?)", CONNECTION_ASYNC
        bstmt->SetData(0, entry);
        bstmt->SetData(1, slot);
        bstmt->SetData(2, item_id);
        bstmt->SetData(3, fake_id);
        CharacterDatabase.Execute(bstmt);
    }
}

void BotDataMgr::ResetNpcBotTransmogData(uint32 entry, bool update_db)
{
    NpcBotTransmogDataMap::const_iterator itr = _botsTransmogData.find(entry);
    if (itr == _botsTransmogData.cend())
        return;

    if (update_db)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        for (uint8 i = 0; i != BOT_TRANSMOG_INVENTORY_SIZE; ++i)
        {
            if (_botsTransmogData[entry]->transmogs[i].first == 0 && _botsTransmogData[entry]->transmogs[i].second == -1)
                continue;

            CharacterDatabasePreparedStatement* bstmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_NPCBOT_TRANSMOG);
            //"REPLACE INTO characters_npcbot_transmog (entry, slot, item_id, fake_id) VALUES (?, ?, ?, ?)", CONNECTION_ASYNC
            bstmt->SetData(0, entry);
            bstmt->SetData(1, i);
            bstmt->SetData(2, 0);
            bstmt->SetData(3, -1);
            trans->Append(bstmt);
        }

        if (trans->GetSize() > 0)
            CharacterDatabase.CommitTransaction(trans);
    }

    for (uint8 i = 0; i != BOT_TRANSMOG_INVENTORY_SIZE; ++i)
        _botsTransmogData[entry]->transmogs[i] = { 0, -1 };
}

void BotDataMgr::RegisterBot(Creature const* bot)
{
    if (_existingBots.find(bot) != _existingBots.end())
    {
        LOG_ERROR("entities.unit", "BotDataMgr::RegisterBot: bot {} ({}) already registered!",
            bot->GetEntry(), bot->GetName().c_str());
        return;
    }

    std::unique_lock<std::shared_mutex> lock(*GetLock());

    _existingBots.insert(bot);
    //TC_LOG_ERROR("entities.unit", "BotDataMgr::RegisterBot: registered bot %u (%s)", bot->GetEntry(), bot->GetName().c_str());
}
void BotDataMgr::UnregisterBot(Creature const* bot)
{
    if (_existingBots.find(bot) == _existingBots.end())
    {
        LOG_ERROR("entities.unit", "BotDataMgr::UnregisterBot: bot {} ({}) not found!",
            bot->GetEntry(), bot->GetName().c_str());
        return;
    }

    std::unique_lock<std::shared_mutex> lock(*GetLock());

    _existingBots.erase(bot);
    //TC_LOG_ERROR("entities.unit", "BotDataMgr::UnregisterBot: unregistered bot %u (%s)", bot->GetEntry(), bot->GetName().c_str());
}
Creature const* BotDataMgr::FindBot(uint32 entry)
{
    std::shared_lock<std::shared_mutex> lock(*GetLock());

    for (NpcBotRegistry::const_iterator ci = _existingBots.cbegin(); ci != _existingBots.cend(); ++ci)
    {
        if ((*ci)->GetEntry() == entry)
            return *ci;
    }
    return nullptr;
}
Creature const* BotDataMgr::FindBot(std::string_view name, LocaleConstant loc, std::vector<uint32> const* not_ids)
{
    std::wstring wname;
    if (Utf8toWStr(name, wname))
    {
        wstrToLower(wname);
        std::shared_lock<std::shared_mutex> lock(*GetLock());
        for (NpcBotRegistry::const_iterator ci = _existingBots.cbegin(); ci != _existingBots.cend(); ++ci)
        {
            if (not_ids && std::find(not_ids->cbegin(), not_ids->cend(), (*ci)->GetEntry()) != not_ids->cend())
                continue;

            std::string basename = (*ci)->GetName();
            if (CreatureLocale const* creatureInfo = sObjectMgr->GetCreatureLocale((*ci)->GetEntry()))
            {
                if (creatureInfo->Name.size() > loc && !creatureInfo->Name[loc].empty())
                    basename = creatureInfo->Name[loc];
            }

            std::wstring wbname;
            if (!Utf8toWStr(basename, wbname))
                continue;

            wstrToLower(wbname);
            if (wbname == wname)
                return *ci;
        }
    }

    return nullptr;
}

NpcBotRegistry const& BotDataMgr::GetExistingNPCBots()
{
    return _existingBots;
}

void BotDataMgr::GetNPCBotGuidsByOwner(std::vector<ObjectGuid> &guids_vec, ObjectGuid owner_guid)
{
    ASSERT(AllBotsLoaded());

    std::shared_lock<std::shared_mutex> lock(*GetLock());

    for (NpcBotRegistry::const_iterator ci = _existingBots.cbegin(); ci != _existingBots.cend(); ++ci)
    {
        if (_botsData[(*ci)->GetEntry()]->owner == owner_guid.GetCounter())
            guids_vec.push_back((*ci)->GetGUID());
    }
}

ObjectGuid BotDataMgr::GetNPCBotGuid(uint32 entry)
{
    ASSERT(AllBotsLoaded());

    std::shared_lock<std::shared_mutex> lock(*GetLock());

    for (NpcBotRegistry::const_iterator ci = _existingBots.cbegin(); ci != _existingBots.cend(); ++ci)
    {
        if ((*ci)->GetEntry() == entry)
            return (*ci)->GetGUID();
    }

    return ObjectGuid::Empty;
}

std::vector<uint32> BotDataMgr::GetExistingNPCBotIds()
{
    ASSERT(AllBotsLoaded());

    std::vector<uint32> existing_ids;
    existing_ids.reserve(_botsData.size());
    for (decltype(_botsData)::value_type const& bot_data_pair : _botsData)
        existing_ids.push_back(bot_data_pair.first);

    return existing_ids;
}

uint8 BotDataMgr::GetOwnedBotsCount(ObjectGuid owner_guid, uint32 class_mask)
{
    uint8 count = 0;
    for (decltype(_botsData)::value_type const& bdata : _botsData)
        if (bdata.second->owner == owner_guid.GetCounter() && (!class_mask || !!(class_mask & (1u << (_botsExtras[bdata.first]->bclass - 1)))))
            ++count;

    return count;
}

uint8 BotDataMgr::GetLevelBonusForBotRank(uint32 rank)
{
    switch (rank)
    {
        case CREATURE_ELITE_RARE:
            return 1;
        case CREATURE_ELITE_ELITE:
            return 2;
        case CREATURE_ELITE_RAREELITE:
            return 3;
        default:
            return 0;
    }
}

uint8 BotDataMgr::GetMinLevelForMapId(uint32 mapId)
{
    decltype(_wpMinSpawnLevelPerMapId)::const_iterator cit = _wpMinSpawnLevelPerMapId.find(mapId);
    if (cit != _wpMinSpawnLevelPerMapId.cend())
        return cit->second;

    switch (mapId)
    {
        case 0:
        case 1:
            return 1;
        case 530:
            return 61;
        case 571:
            return 71;
        default:
            return 1;
    }
}
uint8 BotDataMgr::GetMaxLevelForMapId(uint32 mapId)
{
    decltype(_wpMaxSpawnLevelPerMapId)::const_iterator cit = _wpMaxSpawnLevelPerMapId.find(mapId);
    if (cit != _wpMaxSpawnLevelPerMapId.cend())
        return cit->second;

    switch (mapId)
    {
        case 0:
        case 1:
            return 60;
        case 530:
            return 70;
        case 571:
            return 80;
        default:
            return 80;
    }
}

uint8 BotDataMgr::GetMinLevelForBotClass(uint8 m_class)
{
    switch (m_class)
    {
        case BOT_CLASS_DEATH_KNIGHT:
            return 55;
        case BOT_CLASS_ARCHMAGE:
        case BOT_CLASS_SPELLBREAKER:
        case BOT_CLASS_NECROMANCER:
            return 20;
        case BOT_CLASS_DARK_RANGER:
            return 40;
        case BOT_CLASS_SPHYNX:
        case BOT_CLASS_DREADLORD:
            return 60;
        default:
            return 1;
    }
}

int32 BotDataMgr::GetBotBaseReputation(Creature const* bot, FactionEntry const* factionEntry)
{
    if (!factionEntry)
        return 0;

    if (bot->IsNPCBotPet())
        bot = bot->GetBotPetAI()->GetPetsOwner();

    uint32 raceMask = bot->GetFaction() == 14 ? 0 : bot->GetRaceMask();
    uint32 classMask = bot->GetClassMask();

    int32 minRep = 42999;
    for (uint8 i = 0; i < 4; ++i)
    {
        if (raceMask == 0)
            minRep = std::min<int32>(minRep, factionEntry->BaseRepValue[i]);
        if ((factionEntry->BaseRepRaceMask[i] & raceMask || (factionEntry->BaseRepRaceMask[i] == 0 && factionEntry->BaseRepClassMask[i] != 0)) &&
            (factionEntry->BaseRepClassMask[i] & classMask || factionEntry->BaseRepClassMask[i] == 0))
        {
            return factionEntry->BaseRepValue[i];
        }
    }

    return std::min<int32>(minRep, 0);
}

TeamId BotDataMgr::GetTeamIdForFaction(uint32 factionTemplateId)
{
    if (FactionTemplateEntry const* fte = sFactionTemplateStore.LookupEntry(factionTemplateId))
    {
        if (fte->ourMask & FACTION_MASK_ALLIANCE)
            return TEAM_ALLIANCE;
        else if (fte->ourMask & FACTION_MASK_HORDE)
            return TEAM_HORDE;
    }

    return TEAM_NEUTRAL;
}

uint32 BotDataMgr::GetTeamForFaction(uint32 factionTemplateId)
{
    switch (GetTeamIdForFaction(factionTemplateId))
    {
        case TEAM_ALLIANCE:
            return ALLIANCE;
        case TEAM_HORDE:
            return HORDE;
        default:
            return TEAM_OTHER;
    }
}

bool BotDataMgr::IsWanderNodeAvailableForBotFaction(WanderNode const* wp, uint32 factionTemplateId, bool teleport)
{
    if (!teleport)
    {
        if (wp->HasFlag(BotWPFlags::BOTWP_FLAG_MOVEMENT_IGNORES_FACTION))
            return true;
    }
    else
    {
        MapEntry const* mapEntry = sMapStore.LookupEntry(wp->GetMapId());
        if (!mapEntry->IsContinent())
            return false;
    }

    switch (GetTeamIdForFaction(factionTemplateId))
    {
        case TEAM_ALLIANCE:
            return !wp->HasFlag(BotWPFlags::BOTWP_FLAG_HORDE_ONLY);
        case TEAM_HORDE:
            return !wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY);
        case TEAM_NEUTRAL:
            return !wp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_OR_HORDE_ONLY);
        default:
            return true;
    }
}

WanderNode const* BotDataMgr::GetNextWanderNode(WanderNode const* curNode, WanderNode const* lastNode, Position const* fromPos, Creature const* bot, uint8 lvl, bool random)
{
    using NodeList = std::list<WanderNode const*>;

    static auto node_viable = [](WanderNode const* wp, uint8 lvl) -> bool {
        return (lvl + 2 >= wp->GetLevels().first && lvl <= wp->GetLevels().second);
    };

    uint32 faction = bot->GetFaction();

    //Node got deleted (or forced)! Select close point and go from there
    NodeList links;
    if (curNode->GetLinks().empty() || random)
    {
        if (bot->IsInWorld() && !bot->GetMap()->IsBattlegroundOrArena())
        {
            WanderNode::DoForAllMapWPs(curNode->GetMapId(), [&links, lvl = lvl, fac = faction, pos = fromPos](WanderNode const* wp) {
                if (pos->GetExactDist2d(wp) < MAX_WANDER_NODE_DISTANCE &&
                    IsWanderNodeAvailableForBotFaction(wp, fac, true) && node_viable(wp, lvl))
                    links.push_back(wp);
            });
            if (!links.empty())
                return links.size() == 1u ? links.front() : Acore::Containers::SelectRandomContainerElement(links);
        }

        //Select closest
        WanderNode const* node_new = nullptr;
        float mindist = 50000.0f; // Anywhere
        WanderNode::DoForAllMapWPs(curNode->GetMapId(), [&node_new, &mindist, lvl = lvl, fac = faction, pos = fromPos](WanderNode const* wp) {
            float dist = pos->GetExactDist2d(wp);
            if (dist < mindist &&
                IsWanderNodeAvailableForBotFaction(wp, fac, false) && node_viable(wp, lvl))
            {
                mindist = dist;
                node_new = wp;
            }
        });
        return node_new;
    }

    if (bot_ai::IsFlagCarrier(bot))
    {
        NodeList flagDropNodes;
        TeamId teamId = GetTeamIdForFaction(faction);
        static const auto is_my_flag_drop_node = [](WanderNode const* dwp, TeamId tId) {
            if (dwp->HasFlag(BotWPFlags::BOTWP_FLAG_BG_FLAG_DELIVER_TARGET))
            {
                //must only select own faction drop node
                return (!dwp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_OR_HORDE_ONLY) ||
                    (tId == TEAM_ALLIANCE && dwp->HasFlag(BotWPFlags::BOTWP_FLAG_ALLIANCE_ONLY)) ||
                    (tId == TEAM_HORDE && dwp->HasFlag(BotWPFlags::BOTWP_FLAG_HORDE_ONLY)));
            }
            return false;
        };
        //check two levels of links, enough for: WSG
        for (WanderNode const* dwp : curNode->GetLinks())
        {
            if (is_my_flag_drop_node(dwp, teamId))
                flagDropNodes.push_back(dwp);
            else
            {
                for (WanderNode const* dwpl : dwp->GetLinks())
                {
                    if (dwpl != curNode && is_my_flag_drop_node(dwpl, teamId))
                    {
                        flagDropNodes.push_back(dwp);
                        break;
                    }
                }
            }
        }
        if (!flagDropNodes.empty())
            return flagDropNodes.size() == 1u ? flagDropNodes.front() : Acore::Containers::SelectRandomContainerElement(flagDropNodes);
    }

    for (WanderNode const* wp : curNode->GetLinks())
    {
        if (IsWanderNodeAvailableForBotFaction(wp, faction, false) && node_viable(wp, lvl))
            links.push_back(wp);
    }
    if (links.size() > 1 && lastNode && !curNode->HasFlag(BotWPFlags::BOTWP_FLAG_CAN_BACKTRACK_FROM))
        links.remove(lastNode);

    //Overleveled or died: no viable nodes in reach, find one for teleport
    if (links.empty())
    {
        WanderNode::DoForAllWPs([&links, lvl = lvl, fac = faction](WanderNode const* wp) {
            if (IsWanderNodeAvailableForBotFaction(wp, fac, true) && wp->HasFlag(BotWPFlags::BOTWP_FLAG_SPAWN) && node_viable(wp, lvl))
                links.push_back(wp);
        });
    }

    ASSERT(!links.empty());
    return links.size() == 1u ? links.front() : Acore::Containers::SelectRandomContainerElement(links);
}

WanderNode const* BotDataMgr::GetClosestWanderNode(WorldLocation const* loc)
{
    float mindist = 50000.0f;
    WanderNode const* closestNode = nullptr;
    WanderNode::DoForAllMapWPs(loc->GetMapId(), [&mindist, &closestNode, loc = loc](WanderNode const* wp) {
        float dist = wp->GetExactDist2d(loc);
        if (dist < mindist)
        {
            mindist = dist;
            closestNode = wp;
        }
    });

    return closestNode;
}

BotBankItemContainer const* BotDataMgr::GetBotBankItems(ObjectGuid playerGuid)
{
    decltype(_botStoredGearMap)::iterator mci = _botStoredGearMap.find(playerGuid);
    return mci == _botStoredGearMap.cend() ? nullptr : &mci->second;
}

Item* BotDataMgr::WithdrawBotBankItem(ObjectGuid playerGuid, ObjectGuid::LowType itemGuidLow)
{
    decltype(_botStoredGearMap)::iterator mci = _botStoredGearMap.find(playerGuid);
    if (mci != _botStoredGearMap.cend())
    {
        auto ici = std::find_if(std::cbegin(mci->second), std::cend(mci->second), [guidLow = itemGuidLow](Item const* item) {
            return item->GetGUID().GetCounter() == guidLow;
        });
        if (ici != mci->second.cend())
        {
            Item* item = *ici;
            mci->second.erase(ici);
            return item;
        }
    }

    return nullptr;
}

void BotDataMgr::DepositBotBankItem(ObjectGuid playerGuid, Item* item)
{
    _botStoredGearMap[playerGuid].insert(item);
}

void BotDataMgr::SaveNpcBotStoredGear(ObjectGuid playerGuid, CharacterDatabaseTransaction trans)
{
    decltype(_botStoredGearMap)::iterator mci = _botStoredGearMap.find(playerGuid);
    // we don't check if container is empty!
    // we have to be able to erase items always
    if (mci == _botStoredGearMap.cend())
        return;

    trans->Append("DELETE FROM characters_npcbot_gear_storage WHERE guid = {}", mci->first.GetCounter());
    for (Item* item : mci->second)
    {
        //order is important here
        item->SaveToDB(trans);
        item->DeleteFromInventoryDB(trans);
        trans->Append("INSERT INTO characters_npcbot_gear_storage (guid, item_guid) VALUES ({}, {})", mci->first.GetCounter(), item->GetGUID().GetCounter());
    }
}

class AC_GAME_API WanderingBotXpGainFormulaScript : public FormulaScript
{
    static constexpr float WANDERING_BOT_XP_GAIN_MULT = 10.0f;

public:
    WanderingBotXpGainFormulaScript() : FormulaScript("WanderingBotXpGainFormulaScript") {}

    void OnGainCalculation(uint32& gain, Player* /*player*/, Unit* unit) override
    {
        if (gain && unit->IsNPCBot() && unit->ToCreature()->IsWandererBot())
            gain *= WANDERING_BOT_XP_GAIN_MULT;
    }
};

class AC_GAME_API BotDataMgrShutdownScript : public WorldScript
{
public:
    BotDataMgrShutdownScript() : WorldScript("BotDataMgrShutdownScript") {}

    void OnShutdown() override
    {
        botSpawnEvents.KillAllEvents(true);
        for (auto& kv : botBGJoinEvents)
            kv.second.KillAllEvents(true);
    }
};

void AddSC_botdatamgr_scripts()
{
    new WanderingBotXpGainFormulaScript();
    new BotDataMgrShutdownScript();
}

#ifdef _MSC_VER
# pragma warning(pop)
#endif
