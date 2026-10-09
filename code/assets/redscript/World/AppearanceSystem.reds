module CyberpunkMP.World

import Codeware.*
import CyberpunkMP.*

public native class AppearanceSystem extends IScriptable {
    public native func GetEntityItems(entityID: EntityID) -> array<TweakDBID>;
    //public native func SetMorphWeights(player: ref<GameObject>) -> Void;
    public native func ApplyAppearance(player: ref<GameObject>) -> Bool;

    private let m_callbackSystem: wref<CallbackSystem>;
    public let m_entities: array<wref<GameObject>>;

    public func OnWorldAttached() -> Void {
        this.m_callbackSystem = GameInstance.GetCallbackSystem();
        // if IsDefined(this.m_callbackSystem) {
        this.m_callbackSystem.RegisterCallback(n"Entity/Attached", this, n"OnEntityAttached")
            .AddTarget(DynamicEntityTarget.Tag(n"CyberpunkMP.Puppet"));
        //     LogChannel(n"DEBUG", s"[AppearanceSystem] Entity/Attached callback registered");
        // } else {            
        //     LogChannel(n"DEBUG", s"[AppearanceSystem] No CallbackSystem");
        // }
    }

    public func OnBeforeWorldDetach() -> Void {
        if IsDefined(this.m_callbackSystem) {
            this.m_callbackSystem.UnregisterCallback(n"Entity/Attached", this);
        }
    }

    // The worn items (AppearanceSystem::GetPlayerItems sends their ids).
    private func GetPlayerItems() -> array<TweakDBID> {
        let player = GetPlayer(GetGameInstance());
        let items: array<TweakDBID>;
        let equipData: ref<EquipmentSystemPlayerData> = EquipmentSystem.GetData(player);
        let equipAreas: array<SEquipArea>;
        if IsDefined(equipData) {
            equipAreas = equipData.GetPaperDollEquipAreas();
        }
        let i: Int32 = 0;
        while i < ArraySize(equipAreas) {
            let item = equipData.GetVisualItemInSlot(equipAreas[i].areaType);
            let tdbid = ItemID.GetTDBID(item);
            if TDBID.IsValid(tdbid) {
                ArrayPush(items, tdbid);
            }
            i += 1;
        }
        return items;
    }

    private cb func OnEntityAttached(event: ref<EntityLifecycleEvent>) {
        let entity = event.GetEntity() as GameObject;

        if IsDefined(entity) { // && entity.HasTag(n"CyberpunkMP.Puppet") {
            // LogChannel(n"DEBUG", s"[AppearanceSystem] OnEntityAssemble our muppet");
            if this.ApplyAppearance(entity) {
                ArrayPush(this.m_entities, entity);
            }

            let mappinData: MappinData;
            let mappinSystem: ref<MappinSystem> = GameInstance.GetMappinSystem(GetGameInstance());
            mappinData.mappinType = t"Mappins.CPO_RemotePlayerMappinDefinition";
            // barcode thing, ref base\\gameplay\\gui\\widgets\\mappins\\cyberspace\\cyberspace_object_mappin.inkwidget
            // mappinData.variant = gamedataMappinVariant.CyberspaceObject;
            // red phone thing, ref base\\gameplay\\gui\\widgets\\mappins\\interaction\\quick_hack_mappin.inkwidget
            // mappinData.variant = gamedataMappinVariant.PhoneCallVariant;
            // custom unused
            mappinData.variant = gamedataMappinVariant.CPO_RemotePlayerVariant;
            mappinData.active = true;
            // mappinData.visibleThroughWalls = true;
            // might need to adjust up a bit?
            mappinSystem.RegisterMappinWithObject(mappinData, entity, n"Nameplate");

            // this.SetMorphWeights(entity);
        } else {
            // LogChannel(n"DEBUG", s"[AppearanceSystem] not our muppet");
        }
    }

    // private func AddBodyParts(entity: ref<GameObject>, isMale: Bool) {
    //     let transactionSystem: ref<TransactionSystem> = GameInstance.GetTransactionSystem(entity.GetGame());
    //     if isMale {
    //         let head = ItemID.FromTDBID(t"Items.MuppetMaHead");
    //         transactionSystem.GiveItem(entity, head, 1);
    //         transactionSystem.AddItemToSlot(entity, EquipmentSystem.GetPlacementSlot(head), head);
    //     } else {
    //         let head = ItemID.FromTDBID(t"Items.MuppetWaHead");
    //         transactionSystem.GiveItem(entity, head, 1);
    //         transactionSystem.AddItemToSlot(entity, EquipmentSystem.GetPlacementSlot(head), head);
    //     }

    //     let arms = ItemID.FromTDBID(t"Items.MuppetArms");
    //     transactionSystem.GiveItem(entity, arms, 1);
    //     transactionSystem.AddItemToSlot(entity, EquipmentSystem.GetPlacementSlot(arms), arms);
    // }

    private func AddItems(entity: ref<GameObject>) {
        LogChannel(n"DEBUG", s"[AppearanceSystem] AddItems");
        let i: Int32;
        let transactionSystem: ref<TransactionSystem> = GameInstance.GetTransactionSystem(entity.GetGame());

        let equipment = this.GetEntityItems(entity.GetEntityID());
        i = 0;
        while i < ArraySize(equipment) {
            LogChannel(n"DEBUG", "Setting: " + TDBID.ToStringDEBUG(equipment[i]));
            let item = ItemID.FromTDBID(equipment[i]);
            let placementSlot = EquipmentSystem.GetPlacementSlot(item);

            // if EquipmentSystem.IsClothing(item) {
                if !transactionSystem.HasItem(entity, item) {
                    transactionSystem.GiveItem(entity, item, 1);
                    if !transactionSystem.HasItemInSlot(entity, placementSlot, item) {
                        transactionSystem.AddItemToSlot(entity, placementSlot, item);
                    }
                }
            // }

            // if EquipmentSystem.IsClothing(item) {
            //     transactionSystem.GivePreviewItemByItemID(entity, item);
            // } else {
            //     let itemData = transactionSystem.GetItemData(GetPlayer(entity.GetGame()), item);
            //     transactionSystem.GivePreviewItemByItemData(entity, itemData);
            // };
            // transactionSystem.AddItemToSlot(entity, placementSlot, transactionSystem.CreatePreviewItemID(item));

            i += 1;
        }
    }
}