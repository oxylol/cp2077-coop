module CyberpunkMP.World

import Codeware.*
import CyberpunkMP.*

// What the others see of a player besides where they are: crouching, the weapon in hand, aiming, reloading and
// shooting. C++ (code/client/App/World/CharacterSync.cpp) polls ReadLocalState and sends what changed; for the other
// players' characters it calls the Apply functions below.
public native class CharacterSync extends IScriptable {
    public native func SetLocalState(locomotion: Int32, upperBody: Int32, weaponState: Int32, weapon: TweakDBID) -> Void;
    public native func OnLocalShot() -> Void;

    // This player's state machine: what they're doing, and what's in their right hand.
    public func ReadLocalState() -> Void {
        let player = GetPlayer(GetGameInstance());
        if !IsDefined(player) {
            return;
        }
        let psm = player.GetPlayerStateMachineBlackboard();
        if !IsDefined(psm) {
            return;
        }
        let weapon: TweakDBID;
        let item = GameInstance.GetTransactionSystem(player.GetGame()).GetItemInSlot(player, t"AttachmentSlots.WeaponRight");
        if IsDefined(item) {
            weapon = ItemID.GetTDBID(item.GetItemID());
        }
        this.SetLocalState(psm.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Locomotion),
                           psm.GetInt(GetAllBlackboardDefs().PlayerStateMachine.UpperBody),
                           psm.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Weapon),
                           weapon);
    }

    private func Puppet(id: EntityID) -> ref<ScriptedPuppet> {
        return GameInstance.GetDynamicEntitySystem().GetEntity(id) as ScriptedPuppet;
    }

    // Crouched, aiming, reloading, weapon drawn, as the NPC animation graph takes them (NPCStatesComponent's anim
    // features and wrappers; the NPC's own AI isn't involved). Starts the reload animation when reloading begins.
    // Returns whether reloading.
    public func ApplyStance(id: EntityID, locomotion: Int32, upperBody: Int32, weaponState: Int32, armed: Bool, wasReloading: Bool) -> Bool {
        let npc = this.Puppet(id);
        if !IsDefined(npc) {
            return wasReloading;
        }

        let crouched = locomotion == EnumInt(gamePSMLocomotionStates.Crouch)
            || locomotion == EnumInt(gamePSMLocomotionStates.CrouchSprint)
            || locomotion == EnumInt(gamePSMLocomotionStates.CrouchDodge)
            || locomotion == EnumInt(gamePSMLocomotionStates.Slide)
            || locomotion == EnumInt(gamePSMLocomotionStates.SlideFall);
        let stance = new AnimFeature_NPCState();
        stance.state = crouched ? EnumInt(gamedataNPCStanceState.Crouch) : EnumInt(gamedataNPCStanceState.Stand);
        AnimationControllerComponent.ApplyFeature(npc, n"stanceState", stance);
        AnimationControllerComponent.SetAnimWrapperWeightOnOwnerAndItems(npc, n"inCrouch", crouched ? 1.0 : 0.0);

        // A drawn weapon is held ready, as NPCs in combat hold theirs.
        let highLevel = new AnimFeature_NPCState();
        highLevel.state = armed ? EnumInt(gamedataNPCHighLevelState.Combat) : EnumInt(gamedataNPCHighLevelState.Relaxed);
        AnimationControllerComponent.ApplyFeature(npc, n"highLevelState", highLevel);
        AnimationControllerComponent.SetAnimWrapperWeightOnOwnerAndItems(npc, n"combatLocomotion", armed ? 1.0 : 0.0);

        // The graph's own numbers (NPCStatesComponent.GetUpperBodyStateForAnimGraph): aim 1, normal 6, reload 8.
        let reloading = weaponState == EnumInt(gamePSMRangedWeaponStates.Reload) || upperBody == EnumInt(gamePSMUpperBodyStates.Reload);
        let upper = new AnimFeature_NPCState();
        if reloading {
            upper.state = 8;
        } else {
            if upperBody == EnumInt(gamePSMUpperBodyStates.Aim) {
                upper.state = 1;
            } else {
                upper.state = 6;
            }
        }
        AnimationControllerComponent.ApplyFeature(npc, n"upperBodyState", upper);

        if reloading && !wasReloading {
            AnimationControllerComponent.PushEventToReplicate(npc, n"Reload");
            let weapon = ScriptedPuppet.GetWeaponRight(npc);
            if IsDefined(weapon) {
                AnimationControllerComponent.PushEventToReplicate(weapon, n"Reload");
            }
        }
        return reloading;
    }

    // The animation system's combat mode, as NPCs enter it with their weapon out. Called when it changes.
    public func ApplyCombatMode(id: EntityID, armed: Bool) -> Void {
        if armed {
            GameInstance.GetAnimationSystem(GetGameInstance()).EnterCombatMode(id);
        } else {
            GameInstance.GetAnimationSystem(GetGameInstance()).ExitCombatMode(id);
        }
    }

    // Whether the character holds the weapon (none: an empty TweakDBID) in its right hand.
    public func HoldsWeapon(id: EntityID, weapon: TweakDBID) -> Bool {
        let npc = this.Puppet(id);
        if !IsDefined(npc) {
            return false;
        }
        let held = GameInstance.GetTransactionSystem(npc.GetGame()).GetItemInSlot(npc, t"AttachmentSlots.WeaponRight");
        if !IsDefined(held) {
            return !TDBID.IsValid(weapon);
        }
        return ItemID.GetTDBID(held.GetItemID()) == weapon;
    }

    // Puts the weapon (none: an empty TweakDBID) in the character's right hand: animated, asking the NPC to draw or
    // holster it as NPCs do; otherwise directly.
    public func HoldWeapon(id: EntityID, weapon: TweakDBID, animated: Bool) -> Void {
        let npc = this.Puppet(id);
        if !IsDefined(npc) {
            return;
        }
        let transactions = GameInstance.GetTransactionSystem(npc.GetGame());
        let slot = t"AttachmentSlots.WeaponRight";
        let ai = npc.GetAIControllerComponent();
        let animate = animated && IsDefined(ai);

        if !TDBID.IsValid(weapon) {
            if animate {
                let unequip = new AIUnequipCommand();
                unequip.slotId = slot;
                ai.SendCommand(unequip);
            } else {
                transactions.RemoveItemFromSlot(npc, slot);
            }
            return;
        }

        let item = ItemID.FromTDBID(weapon);
        if !transactions.HasItem(npc, item) {
            transactions.GiveItem(npc, item, 1);
        }
        if animate {
            let equip = new AIEquipCommand();
            equip.slotId = slot;
            equip.itemId = weapon;
            equip.failIfItemNotFound = false;
            ai.SendCommand(equip);
        } else {
            if IsDefined(transactions.GetItemInSlot(npc, slot)) {
                transactions.RemoveItemFromSlot(npc, slot);
            }
            transactions.AddItemToSlot(npc, slot, item);
        }
    }

    // Shots: the character's and the weapon's recoil, and the muzzle flash. Effects only, nothing is hit.
    public func ApplyShots(id: EntityID, count: Int32) -> Void {
        let npc = this.Puppet(id);
        if !IsDefined(npc) || count <= 0 {
            return;
        }
        AnimationControllerComponent.PushEventToReplicate(npc, n"Shoot");
        let weapon = ScriptedPuppet.GetWeaponRight(npc);
        if IsDefined(weapon) {
            AnimationControllerComponent.PushEventToReplicate(weapon, n"Shoot");
            WeaponObject.TriggerWeaponEffects(weapon, gamedataFxAction.Shoot);
        }
    }
}

// Every shot this player fires (the player state machine enters this state once per shot).
@wrapMethod(ShootEvents)
protected final func OnEnter(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
    wrappedMethod(stateContext, scriptInterface);
    let sync = GameInstance.GetNetworkWorldSystem().GetCharacterSync();
    if IsDefined(sync) {
        sync.OnLocalShot();
    }
}
