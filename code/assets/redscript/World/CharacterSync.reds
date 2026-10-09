module CyberpunkMP.World

import Codeware.*
import CyberpunkMP.*

// What the others see of a player besides where they are: crouching, the weapon in hand, aiming, reloading and
// shooting. C++ (code/client/App/World/CharacterSync.cpp) polls ReadLocalState and sends what changed; for the other
// players' characters it calls the Apply functions below.
public native class CharacterSync extends IScriptable {
    public native func SetLocalState(locomotion: Int32, upperBody: Int32, weaponState: Int32, weapon: TweakDBID, aimPitch: Float) -> Void;
    public native func OnLocalShot() -> Void;

    // This player's state machine: what they're doing, what's in their right hand, and where they look, up or down.
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
        let forward = GameInstance.GetCameraSystem(player.GetGame()).GetActiveCameraForward();
        this.SetLocalState(psm.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Locomotion),
                           psm.GetInt(GetAllBlackboardDefs().PlayerStateMachine.UpperBody),
                           psm.GetInt(GetAllBlackboardDefs().PlayerStateMachine.Weapon),
                           weapon,
                           AsinF(ClampF(forward.Z, -1.0, 1.0)));
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

        CoopBlockReactions(npc);

        let crouched = locomotion == EnumInt(gamePSMLocomotionStates.Crouch)
            || locomotion == EnumInt(gamePSMLocomotionStates.CrouchSprint)
            || locomotion == EnumInt(gamePSMLocomotionStates.CrouchDodge)
            || locomotion == EnumInt(gamePSMLocomotionStates.Slide)
            || locomotion == EnumInt(gamePSMLocomotionStates.SlideFall);
        let stance = new AnimFeature_NPCState();
        stance.state = crouched ? EnumInt(gamedataNPCStanceState.Crouch) : EnumInt(gamedataNPCStanceState.Stand);
        AnimationControllerComponent.ApplyFeature(npc, n"stanceState", stance);
        AnimationControllerComponent.SetAnimWrapperWeightOnOwnerAndItems(npc, n"inCrouch", crouched ? 1.0 : 0.0);
        // The character is the player's own body (player_ma_tpp_cutscene.ent), whose graph may take the player's
        // inputs rather than an NPC's: the player's state machine crouches it with this (locomotionTransitions.script).
        AnimationControllerComponent.SetInputFloat(npc, n"crouch", crouched ? 1.0 : 0.0);

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

        // Aiming down the sights, as the player's state machine tells the player's body (defaultTransition.script
        // SetZoomStateAnimFeature), on the character and its weapon.
        let aiming = !reloading && upperBody == EnumInt(gamePSMUpperBodyStates.Aim);
        let aim = new AnimFeature_AimPlayer();
        aim.SetAimState(aiming ? animAimState.Aimed : animAimState.Unaimed);
        aim.SetZoomState(aiming ? animAimState.Aimed : animAimState.Unaimed);
        aim.SetAimInTime(0.2);
        aim.SetAimOutTime(0.2);
        AnimationControllerComponent.ApplyFeature(npc, n"AnimFeature_AimPlayer", aim);
        let held = ScriptedPuppet.GetWeaponRight(npc);
        if IsDefined(held) {
            AnimationControllerComponent.ApplyFeature(held, n"AnimFeature_AimPlayer", aim);
        }

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

    // Weapon drawn: the character's chest, arms and weapon turn to where its player aims (pitch: up or down, radians),
    // as NPCs aim with a look-at (reactionComponent.script). Otherwise, or with no character, the look-at goes.
    public func ApplyAim(id: EntityID, armed: Bool, pitch: Float) -> Void {
        let aims = GameInstance.GetScriptableSystemsContainer(GetGameInstance()).Get(n"CyberpunkMP.World.CoopAims") as CoopAims;
        if IsDefined(aims) {
            aims.Apply(this.Puppet(id), id, armed, pitch);
        }
    }

    // The character goes (its player left, the session ended): its look-at with it, not left pointing at nothing.
    public func ForgetAim(id: EntityID) -> Void {
        let aims = GameInstance.GetScriptableSystemsContainer(GetGameInstance()).Get(n"CyberpunkMP.World.CoopAims") as CoopAims;
        if IsDefined(aims) {
            aims.Apply(this.Puppet(id), id, false, 0.0);
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

// The others' characters' look-ats while they aim (CharacterSync.ApplyAim), at a point that follows them.
public class CoopAims extends ScriptableSystem {
    private let m_ids: array<EntityID>;
    private let m_events: array<ref<LookAtAddEvent>>;
    private let m_targets: array<ref<IPositionProvider>>;

    public func Apply(npc: ref<ScriptedPuppet>, id: EntityID, armed: Bool, pitch: Float) -> Void {
        let index = ArrayFindFirst(this.m_ids, id);
        if !armed || !IsDefined(npc) {
            if index >= 0 {
                if IsDefined(npc) {
                    LookAtRemoveEvent.QueueRemoveLookatEvent(npc, this.m_events[index]);
                }
                ArrayErase(this.m_ids, index);
                ArrayErase(this.m_events, index);
                ArrayErase(this.m_targets, index);
            }
            return;
        }

        if index < 0 {
            let target = IPositionProvider.CreateEntityPositionProvider(npc);
            let lookAt = new LookAtAddEvent();
            lookAt.SetPositionProvider(target);
            lookAt.SetStyle(animLookAtStyle.Fast);
            lookAt.request.limits.softLimitDegrees = 360.0;
            lookAt.request.limits.hardLimitDegrees = 270.0;
            lookAt.request.limits.backLimitDegrees = 210.0;
            lookAt.request.limits.hardLimitDistance = GetLookAtLimitDistanceValue(animLookAtLimitDistanceType.None);
            lookAt.bodyPart = n"Chest";
            let head: LookAtPartRequest;
            head.partName = n"Head";
            head.weight = 1.0;
            head.suppress = 0.0;
            head.mode = 0;
            let parts: array<LookAtPartRequest>;
            ArrayPush(parts, head);
            lookAt.SetAdditionalPartsArray(parts);
            npc.QueueEvent(lookAt);
            ArrayPush(this.m_ids, id);
            ArrayPush(this.m_events, lookAt);
            ArrayPush(this.m_targets, target);
            index = ArraySize(this.m_ids) - 1;
        }

        // 10 m ahead of the character, from about its shoulders, at the pitch.
        let forward = Vector4.Normalize2D(npc.GetWorldForward());
        let reach = 10.0 * CosF(pitch);
        this.m_targets[index].SetWorldOffset(new Vector4(forward.X * reach, forward.Y * reach, 1.5 + 10.0 * SinF(pitch), 0.0));
    }
}

// A stand-in for another player doesn't react on its own (cower, flee, get out of its car): its player's game says
// what it does. The game's own switch (aiHitReactionTasks.script blocks reactions with it for a while).
public static func CoopBlockReactions(npc: ref<GameObject>) -> Void {
    let puppet = npc as ScriptedPuppet;
    if !IsDefined(puppet) || !IsDefined(puppet.GetStimReactionComponent()) {
        return;
    }
    let reactions = puppet.GetStimReactionComponent().GetPuppetReactionBlackboard();
    if IsDefined(reactions) {
        reactions.SetBool(GetAllBlackboardDefs().PuppetReaction.blockReactionFlag, true);
    }
}

// Sitting down in a car: the weapon goes (players put theirs away), and with it the stances CharacterSync applied,
// for the car's own (VehicleSystem.reds). CharacterSync shows them all again once out.
public static func CoopClearForVehicle(npc: ref<GameObject>) -> Void {
    let transactions = GameInstance.GetTransactionSystem(npc.GetGame());
    if IsDefined(transactions.GetItemInSlot(npc, t"AttachmentSlots.WeaponRight")) {
        transactions.RemoveItemFromSlot(npc, t"AttachmentSlots.WeaponRight");
    }
    AnimationControllerComponent.SetAnimWrapperWeightOnOwnerAndItems(npc, n"inCrouch", 0.0);
    AnimationControllerComponent.SetAnimWrapperWeightOnOwnerAndItems(npc, n"combatLocomotion", 0.0);
    let highLevel = new AnimFeature_NPCState();
    highLevel.state = EnumInt(gamedataNPCHighLevelState.Relaxed);
    AnimationControllerComponent.ApplyFeature(npc, n"highLevelState", highLevel);
    GameInstance.GetAnimationSystem(npc.GetGame()).ExitCombatMode(npc.GetEntityID());
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
