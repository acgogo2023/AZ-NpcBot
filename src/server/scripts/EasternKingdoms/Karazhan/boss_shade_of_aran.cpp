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

#include "GameObject.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "karazhan.h"
#include "TaskScheduler.h"

enum Texts
{
    SAY_AGGRO              = 0,
    SAY_FLAMEWREATH        = 1,
    SAY_BLIZZARD           = 2,
    SAY_EXPLOSION          = 3,
    SAY_DRINK              = 4,
    SAY_ELEMENTALS         = 5,
    SAY_KILL               = 6,
    SAY_TIMEOVER           = 7,
    SAY_DEATH              = 8,
    SAY_ATIESH             = 9,
    EMOTE_ARCANE_EXPLOSION = 10
};

enum Spells
{
    //Spells
    SPELL_FROSTBOLT           = 29954,
    SPELL_FIREBALL            = 29953,
    SPELL_ARCMISSLE           = 29955,
    SPELL_CHAINSOFICE         = 29991,
    SPELL_DRAGONSBREATH       = 29964,
    SPELL_MASSSLOW            = 30035,
    SPELL_FLAME_WREATH        = 29946,
    SPELL_AOE_CS              = 29961,
    SPELL_PLAYERPULL          = 32265,
    SPELL_AEXPLOSION          = 29973,
    SPELL_MASS_POLY           = 29963,
    SPELL_BLINK_CENTER        = 29967,
    SPELL_CONJURE             = 29975,
    SPELL_DRINK               = 30024,
    SPELL_POTION              = 32453,
    SPELL_AOE_PYROBLAST       = 29978,

    SPELL_SUMMON_WELEMENTAL_1 = 29962,
    SPELL_SUMMON_WELEMENTAL_2 = 37051,
    SPELL_SUMMON_WELEMENTAL_3 = 37052,
    SPELL_SUMMON_WELEMENTAL_4 = 37053,

    SPELL_SUMMON_BLIZZARD     = 29969, // Activates the Blizzard NPC

    SPELL_SHADOW_PYRO         = 29978
};

enum Creatures
{
    NPC_SHADOW_OF_ARAN  = 18254
};

enum SuperSpell
{
    SUPER_FLAME = 0,
    SUPER_BLIZZARD,
    SUPER_AE,
};

enum Groups
{
    GROUP_FLAMEWREATH   = 0,
    GROUP_DRINKING      = 1
};

Position const roomCenter = {-11158.f, -1920.f};

Position const elementalPos[4] =
{
    {-11168.1f, -1939.29f, 232.092f, 1.46f},
    {-11138.2f, -1915.38f, 232.092f, 3.00f},
    {-11161.7f, -1885.36f, 232.092f, 4.59f},
    {-11192.4f, -1909.36f, 232.092f, 6.19f}
};

struct boss_shade_of_aran : public BossAI
{
    boss_shade_of_aran(Creature* creature) : BossAI(creature, DATA_ARAN)
    {
        scheduler.SetValidator([this]
        {
            return !me->HasUnitState(UNIT_STATE_CASTING);
        });
    }

    void Reset() override
    {
        BossAI::Reset();
        _drinkScheduler.CancelAll();
        _lastSuperSpell = rand() % 3;

        for (uint8 i = 0; i < 3; ++i)
            FlameWreathTarget[i].Clear();

        CurrentNormalSpell = 0;

        _arcaneCooledDown = true;
        _fireCooledDown = true;
        _frostCooledDown = true;

        _drinking = false;
        _hasDrunk = false;

        if (GameObject* libraryDoor = instance->instance->GetGameObject(instance->GetGuidData(DATA_GO_LIBRARY_DOOR)))
        {
            libraryDoor->SetGoState(GO_STATE_ACTIVE);
            libraryDoor->RemoveGameObjectFlag(GO_FLAG_NOT_SELECTABLE);
        }

        ScheduleHealthCheckEvent(40, [&]{
            Talk(SAY_ELEMENTALS);

            std::vector<uint32> elementalSpells = { SPELL_SUMMON_WELEMENTAL_1, SPELL_SUMMON_WELEMENTAL_2, SPELL_SUMMON_WELEMENTAL_3, SPELL_SUMMON_WELEMENTAL_4 };

            for (auto const& spell : elementalSpells)
            {
                DoCastAOE(spell, true);
            }
        });
    }

    bool CheckAranInRoom()
    {
        return me->GetDistance2d(roomCenter.GetPositionX(), roomCenter.GetPositionY()) < 45.0f;
    }

    void AttackStart(Unit* who) override
    {
        if (who && who->isTargetableForAttack() && me->GetReactState() != REACT_PASSIVE)
        {
            if (me->Attack(who, false))
            {
                me->GetMotionMaster()->MoveChase(who, 45.0f, 0);
                me->AddThreat(who, 0.0f);
            }
        }
    }

    void KilledUnit(Unit* /*victim*/) override
    {
        Talk(SAY_KILL);
    }

    void TriggerArcaneCooldown()
    {
        scheduler.Schedule(5s, [this](TaskContext)
        {
            _arcaneCooledDown = true;
        });
    }

    void TriggerFireCooldown()
    {
        scheduler.Schedule(5s, [this](TaskContext)
        {
            _fireCooledDown = true;
        });
    }

    void TriggerFrostCooldown()
    {
        scheduler.Schedule(5s, [this](TaskContext)
        {
            _frostCooledDown = true;
        });
    }

    void JustDied(Unit* /*killer*/) override
    {
        Talk(SAY_DEATH);
        _JustDied();

        if (GameObject* libraryDoor = instance->instance->GetGameObject(instance->GetGuidData(DATA_GO_LIBRARY_DOOR)))
        {
            libraryDoor->SetGoState(GO_STATE_ACTIVE);
            libraryDoor->RemoveGameObjectFlag(GO_FLAG_NOT_SELECTABLE);
        }
    }

    void DamageTaken(Unit* doneBy, uint32& damage, DamageEffectType damagetype, SpellSchoolMask damageSchoolMask) override
    {
        BossAI::DamageTaken(doneBy, damage, damagetype, damageSchoolMask);

        if ((damagetype == DIRECT_DAMAGE || damagetype == SPELL_DIRECT_DAMAGE) && _drinking && me->GetReactState() == REACT_PASSIVE)
        {
            me->RemoveAurasDueToSpell(SPELL_DRINK);
            me->SetStandState(UNIT_STAND_STATE_STAND);
            me->SetReactState(REACT_AGGRESSIVE);
            me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA) - 32000);
            _drinkScheduler.CancelGroup(GROUP_DRINKING);
            _drinkScheduler.Schedule(1s, [this](TaskContext)
            {
                DoCastSelf(SPELL_AOE_PYROBLAST, false);
                _drinking = false;
            });
        }
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        _JustEngagedWith();
        Talk(SAY_AGGRO);

        //handle timed closing door
        scheduler.Schedule(15s, [this](TaskContext)
        {
            if (GameObject* libraryDoor = instance->instance->GetGameObject(instance->GetGuidData(DATA_GO_LIBRARY_DOOR)))
            {
                libraryDoor->SetGoState(GO_STATE_READY);
                libraryDoor->SetGameObjectFlag(GO_FLAG_NOT_SELECTABLE);
            }
        }).Schedule(1s, [this](TaskContext context)
        {
            context.Repeat(2s);

            if (!_drinking)
            {
                if (me->IsNonMeleeSpellCast(false))
                {
                    return;
                }

                uint32 Spells[3];
                uint8 AvailableSpells = 0;

                //Check for what spells are not on cooldown
                if (_arcaneCooledDown)
                {
                    Spells[AvailableSpells] = SPELL_ARCMISSLE;
                    ++AvailableSpells;
                }
                if (_fireCooledDown)
                {
                    Spells[AvailableSpells] = SPELL_FIREBALL;
                    ++AvailableSpells;
                }
                if (_frostCooledDown)
                {
                    Spells[AvailableSpells] = SPELL_FROSTBOLT;
                    ++AvailableSpells;
                }

                // Should drink at 10%, need 10% mana for mass polymorph
                if (!_hasDrunk && me->GetMaxPower(POWER_MANA) && (me->GetPower(POWER_MANA) * 100 / me->GetMaxPower(POWER_MANA)) < 13)
                {
                    _drinking = true;
                    _hasDrunk = true;
                    me->InterruptNonMeleeSpells(true);
                    Talk(SAY_DRINK);
                    DoCastAOE(SPELL_MASS_POLY);
                    me->SetReactState(REACT_PASSIVE);

                    // Start drinking after conjuring drinks
                    _drinkScheduler.Schedule(2s, GROUP_DRINKING, [this](TaskContext)
                    {
                        DoCastSelf(SPELL_CONJURE);
                    }).Schedule(4s, GROUP_DRINKING, [this](TaskContext)
                    {
                        me->SetStandState(UNIT_STAND_STATE_SIT);
                        DoCastSelf(SPELL_DRINK);
                    });

                    _drinkScheduler.Schedule(10s, GROUP_DRINKING, [this](TaskContext)
                    {
                        me->SetStandState(UNIT_STAND_STATE_STAND);
                        me->SetReactState(REACT_AGGRESSIVE);
                        me->SetPower(POWER_MANA, me->GetMaxPower(POWER_MANA) - 32000);
                        DoCastSelf(SPELL_AOE_PYROBLAST);
                        _drinkScheduler.CancelGroup(GROUP_DRINKING);
                        _drinking = false;
                    });

                    return;
                }

                //If no available spells wait 1 second and try again
                if (AvailableSpells)
                {
                    CurrentNormalSpell = Spells[rand() % AvailableSpells];

                    if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(CurrentNormalSpell))
                    {
                        if (int32(me->GetPower(POWER_MANA)) < spellInfo->CalcPowerCost(me, (SpellSchoolMask)spellInfo->SchoolMask))
                        {
                            DoCastSelf(SPELL_POTION);
                        }
                        else
                        {
                            if (!me->CanCastSpell(CurrentNormalSpell))
                            {
                                me->SetWalk(false);
                                me->ResumeChasingVictim();
                            }
                            else
                            {
                                DoCastRandomTarget(CurrentNormalSpell, 0, 100.0f);
                                if (me->GetVictim())
                                {
                                    me->GetMotionMaster()->MoveChase(me->GetVictim(), 45.0f);
                                }
                            }
                        }
                    }
                }
            }
        }).Schedule(5s, [this](TaskContext context)
        {
            if (!_drinking)
            {
                switch (urand(0, 1))
                {
                    case 0:
                        DoCastSelf(SPELL_AOE_CS);
                        break;
                    case 1:
                        DoCastRandomTarget(SPELL_CHAINSOFICE);
                        break;
                }
            }
            context.Repeat(5s, 20s);
        }).Schedule(6s, [this](TaskContext context)
        {
            if (!_drinking)
            {
                me->ClearProhibitedSpellTimers();

                DoCastSelf(SPELL_BLINK_CENTER, true);

                uint8 Available[2];

                switch (_lastSuperSpell)
                {
                    case SUPER_AE:
                        Available[0] = SUPER_FLAME;
                        Available[1] = SUPER_BLIZZARD;
                        break;
                    case SUPER_FLAME:
                        Available[0] = SUPER_AE;
                        Available[1] = SUPER_BLIZZARD;
                        break;
                    case SUPER_BLIZZARD:
                        Available[0] = SUPER_FLAME;
                        Available[1] = SUPER_AE;
                        break;
                }

                _lastSuperSpell = Available[urand(0, 1)];

                switch (_lastSuperSpell)
                {
                    case SUPER_AE:
                        Talk(SAY_EXPLOSION);
                        Talk(EMOTE_ARCANE_EXPLOSION);
                        DoCastSelf(SPELL_PLAYERPULL, true);
                        DoCastSelf(SPELL_MASSSLOW, true);
                        DoCastSelf(SPELL_AEXPLOSION, false);
                        break;

                    case SUPER_FLAME:
                        Talk(SAY_FLAMEWREATH);

                        scheduler.Schedule(20s, GROUP_FLAMEWREATH, [this](TaskContext)
                        {
                            scheduler.CancelGroup(GROUP_FLAMEWREATH);
                        }).Schedule(500ms, GROUP_FLAMEWREATH, [this](TaskContext context)
                        {
                            for (uint8 i = 0; i < 3; ++i)
                            {
                                if (!FlameWreathTarget[i])
                                    continue;

                                Unit* unit = ObjectAccessor::GetUnit(*me, FlameWreathTarget[i]);
                                if (unit && !unit->IsWithinDist2d(FWTargPosX[i], FWTargPosY[i], 3))
                                {
                                    unit->CastSpell(unit, 20476, true, 0, 0, me->GetGUID());
                                    FlameWreathTarget[i].Clear();
                                }
                            }
                            context.Repeat(500ms);
                        });

                        FlameWreathTarget[0].Clear();
                        FlameWreathTarget[1].Clear();
                        FlameWreathTarget[2].Clear();

                        FlameWreathEffect();
                        break;

                    case SUPER_BLIZZARD:
                        Talk(SAY_BLIZZARD);
                        DoCastAOE(SPELL_SUMMON_BLIZZARD);
                        break;
                }
            }
            context.Repeat(35s, 40s);
        }).Schedule(12min, [this](TaskContext context)
        {
            for (uint32 i = 0; i < 5; ++i)
            {
                if (Creature* unit = me->SummonCreature(NPC_SHADOW_OF_ARAN, 0.0f, 0.0f, 0.0f, 0.0f, TEMPSUMMON_TIMED_DESPAWN_OUT_OF_COMBAT, 5000))
                {
                    unit->Attack(me->GetVictim(), true);
                    unit->SetFaction(me->GetFaction());
                }
            }

            Talk(SAY_TIMEOVER);

            context.Repeat(1min);
        });
    }

    void FlameWreathEffect()
    {
        std::vector<Unit*> targets;
        ThreatContainer::StorageType const& t_list = me->GetThreatMgr().GetThreatList();

        if (t_list.empty())
            return;

        //store the threat list in a different container
        for (ThreatContainer::StorageType::const_iterator itr = t_list.begin(); itr != t_list.end(); ++itr)
        {
            Unit* target = ObjectAccessor::GetUnit(*me, (*itr)->getUnitGuid());
            //only on alive players
            if (target && target->IsAlive() && target->GetTypeId() == TYPEID_PLAYER)
                targets.push_back(target);
        }

        //cut down to size if we have more than 3 targets
        while (targets.size() > 3)
            targets.erase(targets.begin() + rand() % targets.size());

        uint32 i = 0;
        for (std::vector<Unit*>::const_iterator itr = targets.begin(); itr != targets.end(); ++itr)
        {
            if (*itr)
            {
                FlameWreathTarget[i] = (*itr)->GetGUID();
                FWTargPosX[i] = (*itr)->GetPositionX();
                FWTargPosY[i] = (*itr)->GetPositionY();
                DoCast((*itr), SPELL_FLAME_WREATH, true);
                ++i;
            }
        }
    }

    void UpdateAI(uint32 diff) override
    {
        scheduler.Update(diff);
        _drinkScheduler.Update(diff);

        if (!UpdateVictim())
            return;

        if (!CheckAranInRoom())
        {
            EnterEvadeMode();
            return;
        }

        if (_arcaneCooledDown && _fireCooledDown && _frostCooledDown && !_drinking)
            DoMeleeAttackIfReady();
    }

    void SpellHit(Unit* /*pAttacker*/, SpellInfo const* Spell) override
    {
        //We only care about interrupt effects and only if they are durring a spell currently being cast
        if ((Spell->Effects[0].Effect != SPELL_EFFECT_INTERRUPT_CAST &&
                Spell->Effects[1].Effect != SPELL_EFFECT_INTERRUPT_CAST &&
                Spell->Effects[2].Effect != SPELL_EFFECT_INTERRUPT_CAST) || !me->IsNonMeleeSpellCast(false))
            return;

        //Normally we would set the cooldown equal to the spell duration
        //but we do not have access to the DurationStore

        switch (CurrentNormalSpell)
        {
            case SPELL_ARCMISSLE:
                TriggerArcaneCooldown();
                break;
            case SPELL_FIREBALL:
                TriggerFireCooldown();
                break;
            case SPELL_FROSTBOLT:
                TriggerFrostCooldown();
                break;
        }
    }
private:
    TaskScheduler _drinkScheduler;

    uint32 _lastSuperSpell;

    ObjectGuid FlameWreathTarget[3];
    float FWTargPosX[3];
    float FWTargPosY[3];

    uint32 CurrentNormalSpell;

    bool _arcaneCooledDown;
    bool _fireCooledDown;
    bool _frostCooledDown;
    bool _drinking;
    bool _hasDrunk;
};

void AddSC_boss_shade_of_aran()
{
    RegisterKarazhanCreatureAI(boss_shade_of_aran);
}
