#ifndef _BOTMGR_H
#define _BOTMGR_H

#include "botcommon.h"
#include "DBCEnums.h"

#include <functional>
#include <mutex>

class Battleground;
class Creature;
class GameObject;
class Group;
class Map;
class Player;
class Spell;
class SpellInfo;
class Unit;
class Vehicle;
class WorldLocation;
class WorldObject;
class WorldPacket;

class DPSTracker;

struct AreaTrigger;
struct CleanDamage;
struct GroupQueueInfo;
struct Position;

enum BattlegroundTypeId : uint8;
enum CurrentSpellTypes : uint8;
enum DamageEffectType : uint8;

constexpr size_t TargetIconNamesCacheSize = 8u; // Group.h TARGETICONCOUNT
constexpr size_t BracketsCount = DEFAULT_MAX_LEVEL / 10 + 1; //0-9, 10-19, 20-29, 30-39, 40-49, 50-59, 60-69, 70-79, 80-83

enum BotAddResult
{
    BOT_ADD_DISABLED                    = 0x001,
    BOT_ADD_ALREADY_HAVE                = 0x002,
    BOT_ADD_MAX_EXCEED                  = 0x004,
    BOT_ADD_MAX_CLASS_EXCEED            = 0x008,
    BOT_ADD_CANT_AFFORD                 = 0x010,
    BOT_ADD_INSTANCE_LIMIT              = 0x020,
    BOT_ADD_BUSY                        = 0x040,
    BOT_ADD_NOT_AVAILABLE               = 0x080,

    BOT_ADD_SUCCESS                     = 0x100,

    BOT_ADD_FATAL                       = (BOT_ADD_DISABLED | BOT_ADD_CANT_AFFORD | BOT_ADD_MAX_EXCEED | BOT_ADD_MAX_CLASS_EXCEED)
};

enum BotRemoveType
{
    BOT_REMOVE_LOGOUT                   = 0,
    BOT_REMOVE_DISMISS                  = 1,
    BOT_REMOVE_UNSUMMON                 = 2,
    BOT_REMOVE_UNBIND                   = 3,
    BOT_REMOVE_BY_DEFAULT               = BOT_REMOVE_LOGOUT
};

enum BotAttackRange
{
    BOT_ATTACK_RANGE_SHORT              = 1,
    BOT_ATTACK_RANGE_LONG               = 2,
    BOT_ATTACK_RANGE_EXACT              = 3
};

enum BotAttackAngle
{
    BOT_ATTACK_ANGLE_NORMAL             = 1,
    BOT_ATTACK_ANGLE_AVOID_FRONTAL_AOE  = 2
};

typedef std::unordered_map<ObjectGuid /*guid*/, Creature* /*bot*/> BotMap;
typedef std::array<uint32, BracketsCount> BotBrackets;

class AC_GAME_API BotMgr
{
    public:
        using delayed_teleport_callback_type = std::function<void(void)>;
        using delayed_teleport_mutex_type = std::mutex;
        using delayed_teleport_lock_type = std::unique_lock<delayed_teleport_mutex_type>;

        BotMgr(Player* const master);
        ~BotMgr();

        Player* GetOwner() const { return _owner; }

        BotMap const* GetBotMap() const { return &_bots; }
        BotMap* GetBotMap() { return &_bots; }

        static bool IsNpcBotModEnabled();
        static bool IsNpcBotDungeonFinderEnabled();
        static bool DisplayEquipment();
        static bool ShowEquippedCloak();
        static bool ShowEquippedHelm();
        static bool SendEquipListItems();
        static bool IsGearBankEnabled();
        static bool IsTransmogEnabled();
        static bool MixArmorClasses();
        static bool MixWeaponClasses();
        static bool MixWeaponInventoryTypes();
        static bool TransmogUseEquipmentSlots();
        static bool IsClassEnabled(uint8 m_class);
        static bool IsEnrageOnDimissEnabled();
        static bool IsBotStatsLimitsEnabled();
        static bool IsPvPEnabled();
        static bool IsFoodInterruptedByMovement();
        static bool FilterRaces();
        static bool IsBotGenerationEnabledBGs();
        static bool IsBotLevelCappedByConfigBG();
        static bool IsBotGenerationEnabledWorldMapId(uint32 mapId);
        static bool IsBotHKEnabled();
        static bool IsBotHKMessageEnabled();
        static bool IsBotHKAchievementsEnabled();
        static uint8 GetMaxClassBots();
        static uint8 GetHealTargetIconFlags();
        static uint8 GetTankTargetIconFlags();
        static uint8 GetOffTankTargetIconFlags();
        static uint8 GetDPSTargetIconFlags();
        static uint8 GetRangedDPSTargetIconFlags();
        static uint8 GetNoDPSTargetIconFlags();
        static uint32 GetBaseUpdateDelay();
        static uint32 GetOwnershipExpireTime();
        static uint32 GetDesiredWanderingBotsCount();
        static uint32 GetBGTargetTeamPlayersCount(BattlegroundTypeId bgTypeId);
        static float GetBotHKHonorRate();
        static float GetBotStatLimitDodge();
        static float GetBotStatLimitParry();
        static float GetBotStatLimitBlock();
        static float GetBotStatLimitCrit();
        static float GetBotDamageModPhysical();
        static float GetBotDamageModSpell();
        static float GetBotHealingMod();
        static float GetBotHPMod();
        static float GetBotWandererDamageMod();
        static float GetBotWandererHealingMod();
        static float GetBotWandererHPMod();
        static float GetBotWandererSpeedMod();
        static BotBrackets GetBotWandererLevelBrackets();
        static float GetBotDamageModByClass(uint8 botclass);
        static float GetBotDamageModByLevel(uint8 botlevel);

        static void Initialize();
        static void ReloadConfig();
        static void LoadConfig(bool reload = false);
        static void ResolveConfigConflicts();

        //onEvent hooks
        static void OnBotWandererKilled(Creature const* bot, Player* looter);
        static void OnBotWandererKilled(GameObject* go);
        static void OnBotSpellInterrupt(Unit const* caster, CurrentSpellTypes spellType);
        static void OnBotSpellGo(Unit const* caster, Spell const* spell, bool ok = true);
        static void OnBotOwnerSpellGo(Unit const* caster, Spell const* spell, bool ok = true);
        static void OnBotChannelFinish(Unit const* caster, Spell const* spell);
        static void OnVehicleSpellGo(Unit const* caster, Spell const* spell, bool ok = true);
        static void OnVehicleAttackedBy(Unit* attacker, Unit const* victim);
        static void OnBotDamageTaken(Unit* attacker, Unit* victim, uint32 damage, CleanDamage const* cleanDamage, DamageEffectType damagetype, SpellInfo const* spellInfo);
        static void OnBotDamageDealt(Unit* attacker, Unit* victim, uint32 damage, CleanDamage const* cleanDamage, DamageEffectType damagetype, SpellInfo const* spellInfo);
        static void OnBotDispelDealt(Unit* dispeller, Unit* dispelled, uint8 num);
        static void OnBotEnterVehicle(Creature const* passenger, Vehicle const* vehicle);
        static void OnBotExitVehicle(Creature const* passenger, Vehicle const* vehicle);
        static void OnBotOwnerEnterVehicle(Player const* passenger, Vehicle const* vehicle);
        static void OnBotOwnerExitVehicle(Player const* passenger, Vehicle const* vehicle);
        static void OnBotPartyEngage(Player const* owner);
        //mod hooks
        static void ApplyBotEffectMods(Unit const* caster, SpellInfo const* spellInfo, uint8 effIndex, float& value);
        static void ApplyBotThreatMods(Unit const* attacker, SpellInfo const* spellInfo, float& threat);
        static void ApplyBotEffectValueMultiplierMods(Unit const* caster, SpellInfo const* spellInfo, SpellEffIndex effIndex, float& multiplier);
        static float GetBotDamageTakenMod(Creature const* bot, bool magic);
        static int32 GetBotStat(Creature const* bot, BotStatMods stat);
        static float GetBotResilience(Creature const* botOrPet);

        void Update(uint32 diff);

        Creature* GetBot(ObjectGuid guid) const;
        Creature* GetBotByName(std::string_view name) const;
        std::list<Creature*> GetAllBotsByClass(uint8 botclass) const;

        bool HaveBot() const { return !_bots.empty(); }
        uint8 GetNpcBotsCount() const;
        uint8 GetNpcBotsCountByRole(uint32 roles) const;
        uint8 GetNpcBotsCountByVehicleEntry(uint32 creEntry) const;
        uint8 GetNpcBotSlot(Creature const* bot) const;
        uint8 GetNpcBotSlotByRole(uint32 roles, Creature const* bot) const;
        uint32 GetAllNpcBotsClassMask() const;
        static uint8 GetMaxNpcBots();
        static uint8 GetNpcBotXpReduction();
        static uint8 GetNpcBotXpReductionStartingNumber();
        static int32 GetBotInfoPacketsLimit();
        static bool LimitBots(Map const* map);
        static bool CanBotParryWhileCasting(Creature const* bot);
        static bool IsWanderingWorldBot(Creature const* bot);
        static bool IsBotContestedPvP(Creature const* bot);
        static void SetBotContestedPvP(Creature const* bot);
        bool RestrictBots(Creature const* bot, bool add) const;
        bool IsPartyInCombat() const;
        bool HasBotClass(uint8 botclass) const;
        bool HasBotWithSpec(uint8 spec, bool alive = true) const;
        bool HasBotPetType(uint32 petType) const;
        bool IsBeingResurrected(WorldObject const* corpse) const;

        static uint32 GetNpcBotCost(uint8 level, uint8 botclass);
        static std::string GetNpcBotCostStr(uint8 level, uint8 botclass);
        static uint8 BotClassByClassName(std::string const& className);
        static uint8 GetBotPlayerClass(uint8 bot_class);
        static uint8 GetBotPlayerRace(uint8 bot_class, uint8 bot_race);
        static uint8 GetBotPlayerClass(Creature const* bot);
        static uint8 GetBotPlayerRace(Creature const* bot);

        std::string GetTargetIconString(uint8 icon) const;

        void OnTeleportFar(uint32 mapId, float x, float y, float z, float ori = 0.f);
        void OnOwnerSetGameMaster(bool on);
        void ReviveAllBots();
        void SendBotCommandState(uint32 state);
        void SendBotCommandStateRemove(uint32 state);
        void SendBotAwaitState(uint8 state);
        void RecallAllBots(bool teleport = false);
        void RecallBot(Creature* bot);
        void KillAllBots();
        void KillBot(Creature* bot);

        void CleanupsBeforeBotDelete(ObjectGuid guid, uint8 removetype = BOT_REMOVE_LOGOUT);
        static void CleanupsBeforeBotDelete(Creature* bot);
        void RemoveAllBots(uint8 removetype = BOT_REMOVE_LOGOUT);
        void RemoveBot(ObjectGuid guid, uint8 removetype = BOT_REMOVE_LOGOUT);
        void UnbindBot(ObjectGuid guid);
        [[nodiscard]] BotAddResult RebindBot(Creature* bot);
        [[nodiscard]] BotAddResult AddBot(Creature* bot);
        bool AddBotToGroup(Creature* bot);
        void RemoveBotFromBGQueue(Creature const* bot);
        bool RemoveBotFromGroup(Creature* bot);
        bool RemoveAllBotsFromGroup();

        static uint8 GetBotFollowDistDefault() { return 100; }
        uint8 GetBotFollowDist() const { return _followdist; }
        void SetBotFollowDist(uint8 dist) { _followdist = dist; }

        uint8 GetBotExactAttackRange() const { return _exactAttackRange; }
        uint8 GetBotAttackRangeMode() const { return _attackRangeMode; }
        void SetBotAttackRangeMode(uint8 mode, uint8 exactRange = 0) { _attackRangeMode = mode; _setBotExactAttackRange(exactRange); }

        uint8 GetBotAttackAngleMode() const { return _attackAngleMode; }
        void SetBotAttackAngleMode(uint8 mode) { _attackAngleMode = mode; }

        bool GetBotAllowCombatPositioning() const { return _allowCombatPositioning; }
        void SetBotAllowCombatPositioning(bool allow) { _allowCombatPositioning = allow; }

        uint32 GetEngageDelayDPS() const { return _npcBotEngageDelayDPS; }
        uint32 GetEngageDelayHeal() const { return _npcBotEngageDelayHeal; }
        void SetEngageDelayDPS(uint32 delay) { _npcBotEngageDelayDPS = delay; }
        void SetEngageDelayHeal(uint32 delay) { _npcBotEngageDelayHeal = delay; }
        void PropagateEngageTimers() const;

        void SetBotsHidden(bool hidden) { _botsHidden = hidden; }

        void SetBotsShouldUpdateStats();
        void UpdatePhaseForBots();
        void UpdatePvPForBots();

        static void BuildBotPartyMemberStatsPacket(ObjectGuid bot_guid, WorldPacket* data);
        static void BuildBotPartyMemberStatsChangedPacket(Creature const* bot, WorldPacket* data);
        //static uint32 GetBotGroupUpdateFlag(Creature const* bot);
        static void SetBotGroupUpdateFlag(Creature const* bot, uint32 flag);
        static uint64 GetBotAuraUpdateMaskForRaid(Creature const* bot);
        static void SetBotAuraUpdateMaskForRaid(Creature const* bot, uint8 slot);
        static void ResetBotAuraUpdateMaskForRaid(Creature const* bot);
        static uint64 GetBotPetAuraUpdateMaskForRaid(Creature const* botpet);
        static void SetBotPetAuraUpdateMaskForRaid(Creature const* botpet, uint8 slot);
        static void ResetBotPetAuraUpdateMaskForRaid(Creature const* botpet);

        void TrackDamage(Unit const* u, uint32 damage);
        uint32 GetDPSTaken(Unit const* u) const;
        int32 GetHPSTaken(Unit const* unit) const;

        static void ReviveBot(Creature* bot, WorldLocation* dest = nullptr) { _reviveBot(bot, dest); }

        //TELEPORT BETWEEN MAPS
        //CONFIRMEND UNSAFE (charmer,owner)
        static void TeleportBot(Creature* bot, Map* newMap, Position const* pos, bool quick = false, bool reset = false);

        AoeSpotsVec const& GetAoeSpots() const { return _aoespots; }
        AoeSpotsVec& GetAoeSpots() { return _aoespots; }

        void UpdateTargetIconName(uint8 id, std::string const& name);
        void ResetTargetIconNames();

        static std::vector<Unit*> GetAllGroupMembers(Group const* group);
        static std::vector<Unit*> GetAllGroupMembers(Unit const* source);
        static void InviteBotToBG(ObjectGuid botguid, GroupQueueInfo* ginfo, Battleground* bg);

        static bool IsBotInAreaTriggerRadius(Creature const* bot, AreaTrigger const* trigger);

        static void AddDelayedTeleportCallback(delayed_teleport_callback_type&& callback);
        static void HandleDelayedTeleports();

    private:
        static void _teleportBot(Creature* bot, Map* newMap, float x, float y, float z, float ori, bool quick, bool reset);
        static void _reviveBot(Creature* bot, WorldLocation* dest = nullptr);
        void _setBotExactAttackRange(uint8 exactRange) { _exactAttackRange = exactRange; }
        static delayed_teleport_mutex_type* _getTpLock();

        Player* const _owner;
        BotMap _bots;
        std::list<ObjectGuid> _removeList;
        DPSTracker* const _dpstracker;

        uint8 _followdist;
        uint8 _exactAttackRange;
        uint8 _attackRangeMode;
        uint8 _attackAngleMode;
        bool _allowCombatPositioning;
        uint32 _npcBotEngageDelayDPS;
        uint32 _npcBotEngageDelayHeal;

        bool _botsHidden;
        bool _quickrecall;

        AoeSpotsVec _aoespots;

        std::array<std::string, TargetIconNamesCacheSize> _targetIconNamesCache;
};

void AddNpcBotScripts();

#endif
