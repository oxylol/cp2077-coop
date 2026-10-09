module CyberpunkMP.Ink

import CyberpunkMP.*
import CyberpunkMP.World.*

// The co-op HUD: the phone-style icon, the input hints and the three hold actions.
//   not in a session: hold "/" hosts one (UIConnectToServer), hold "." joins one (UIJoinSession)
//   in a session:     hold "/" leaves it (UIDisconnectFromServer)
// Based on NewHudPhoneGameController and PauseMenuBackgroundGameController.
public class MultiplayerGameController extends inkGameController {
    public let m_uiSystem: wref<UISystem>;
    protected let m_player: wref<PlayerPuppet>;
    private let m_connectedToServer: Bool = false;
    private let m_comDeviceBB: wref<IBlackboard>;
    private let m_comDeviceBBDef: ref<UI_ComDeviceDef>;
    public let m_activePhoneElementsListener: ref<CallbackHandle>;
    public let m_phoneIconAnimProxy: ref<inkAnimProxy>;
    public let m_startupAnimProxy: ref<inkAnimProxy>;
    public let m_isInVehicle: Int32 = 2;
    public let m_phoneIconWidget: wref<inkWidget>;
    private let m_connectedToServerCallback: ref<CallbackHandle>;

    protected cb func OnInitialize() -> Bool {
        FTLog(s"[MultiplayerGameController] OnInitialize");

        this.m_comDeviceBBDef = GetAllBlackboardDefs().UI_ComDevice;
        this.m_comDeviceBB = this.GetBlackboardSystem().Get(this.m_comDeviceBBDef);
        if IsDefined(this.m_comDeviceBB) {
            this.m_activePhoneElementsListener = this.m_comDeviceBB.RegisterListenerUint(this.m_comDeviceBBDef.ActivatePhoneElements, this, n"OnActivatePhoneElements", true);
        }

        let blackboardSystem: ref<BlackboardSystem> = GameInstance.GetBlackboardSystem(GetGameInstance());
        let uiBlackboard: ref<IBlackboard> = blackboardSystem.Get(GetAllBlackboardDefs().UIGameData);
        let connectedToServerDef = GetAllBlackboardDefs().UIGameData.UIMultiplayerConnectedToServer;
        this.m_connectedToServerCallback = uiBlackboard.RegisterListenerBool(connectedToServerDef, this, n"OnConnectedToServer");

        this.m_player = this.GetPlayerControlledObject() as PlayerPuppet;
        this.m_uiSystem = GameInstance.GetUISystem(this.m_player.GetGame());

        this.m_player.RegisterInputListener(this, n"UIConnectToServer");
        this.m_player.RegisterInputListener(this, n"UIJoinSession");
        this.UpdateInputHints();

        this.AsyncSpawnFromLocal(this.GetWidget(n"hud"), n"phone_device", this, n"OnHotKeySpawn");
    }

    protected cb func OnUninitialize() -> Bool {
        if IsDefined(this.m_comDeviceBB) {
            this.m_comDeviceBB.UnregisterListenerUint(this.m_comDeviceBBDef.ActivatePhoneElements, this.m_activePhoneElementsListener);
        };
        this.m_comDeviceBB = null;

        let blackboardSystem: ref<BlackboardSystem> = GameInstance.GetBlackboardSystem(GetGameInstance());
        let uiBlackboard: ref<IBlackboard> = blackboardSystem.Get(GetAllBlackboardDefs().UIGameData);
        uiBlackboard.UnregisterListenerBool(GetAllBlackboardDefs().UIGameData.UIMultiplayerConnectedToServer, this.m_connectedToServerCallback);
    }

// Hotkey

    protected cb func OnHotKeySpawn(widget: ref<inkWidget>, userData: ref<IScriptable>) -> Bool {
        this.m_phoneIconWidget = widget;
        this.m_phoneIconWidget.SetVisible(false);
        let element = this.m_comDeviceBB.GetUint(this.m_comDeviceBBDef.ActivatePhoneElements);
        this.m_isInVehicle = 0;
        if Cast<Bool>(element & Cast<Uint32>(EnumInt(gameuiActivePhoneElement.InVehicle))) {
            this.m_isInVehicle = 1;
        }
        let options: inkAnimOptions;
        options.customTimeDilation = 10.00;
        options.applyCustomTimeDilation = true;
        if this.m_isInVehicle == 1 {
            this.m_startupAnimProxy = this.PlayLibraryAnimation(n"2Vehicle", options);
        } else {
            this.m_startupAnimProxy = this.PlayLibraryAnimation(n"2Phone", options);
        }
        this.m_startupAnimProxy.RegisterToCallback(inkanimEventType.OnFinish, this, n"OnPositionAnimationFinish");
    }

    protected cb func OnPositionAnimationFinish(anim: ref<inkAnimProxy>) -> Bool {
        this.m_startupAnimProxy.UnregisterFromAllCallbacks(inkanimEventType.OnFinish);
        this.m_phoneIconWidget.SetVisible(true);
    }

    private cb func OnActivatePhoneElements(element: Uint32) -> Bool {
        FTLog(s"[MultiplayerGameController] OnActivatePhoneElements: \(element)");
        let isInVehicle = 0;
        if Cast<Bool>(element & Cast<Uint32>(EnumInt(gameuiActivePhoneElement.InVehicle))) {
            isInVehicle = 1;
        }
        if NotEquals(this.m_isInVehicle, isInVehicle) {
            this.m_isInVehicle = isInVehicle;
            if IsDefined(this.m_phoneIconAnimProxy) {
                this.m_phoneIconAnimProxy.Stop();
                this.m_phoneIconAnimProxy = null;
            };
            if this.m_isInVehicle == 1 {
                this.m_phoneIconAnimProxy = this.PlayLibraryAnimation(n"2Vehicle");
            } else {
                this.m_phoneIconAnimProxy = this.PlayLibraryAnimation(n"2Phone");
            }
        }
    }

    private func UpdateInputHints() -> Void {
        let evt = new UpdateInputHintMultipleEvent();
        evt.targetHintContainer = n"GameplayInputHelper";
        evt.AddInputHint(CreateInputHint(n"Host co-op", n"UIConnectToServer", true), !this.m_connectedToServer);
        evt.AddInputHint(CreateInputHint(n"Join co-op", n"UIJoinSession", true), !this.m_connectedToServer);
        evt.AddInputHint(CreateInputHint(n"Leave co-op", n"UIDisconnectFromServer", true), this.m_connectedToServer);
        this.m_uiSystem.QueueEvent(evt);
    }

    protected func OnConnectedToServer(connected: Bool) -> Void {
        FTLog(s"[MultiplayerGameController] OnConnectedToServer \(connected)");
        this.m_connectedToServer = connected;
        if connected {
            this.m_player.UnregisterInputListener(this, n"UIConnectToServer");
            this.m_player.UnregisterInputListener(this, n"UIJoinSession");
            this.m_player.RegisterInputListener(this, n"UIDisconnectFromServer");
        } else {
            this.m_player.RegisterInputListener(this, n"UIConnectToServer");
            this.m_player.RegisterInputListener(this, n"UIJoinSession");
            this.m_player.UnregisterInputListener(this, n"UIDisconnectFromServer");
        }
        this.UpdateInputHints();
    }

    protected cb func OnAction(action: ListenerAction, consumer: ListenerActionConsumer) -> Bool {
        let actionName: CName = ListenerAction.GetName(action);
        let actionType: gameinputActionType = ListenerAction.GetType(action);
        if !Equals(actionType, gameinputActionType.BUTTON_HOLD_COMPLETE) {
            return false;
        }
        if !this.m_connectedToServer {
            if Equals(actionName, n"UIConnectToServer") {
                GameInstance.GetNetworkWorldSystem().Host();
                return true;
            }
            if Equals(actionName, n"UIJoinSession") {
                GameInstance.GetNetworkWorldSystem().Join();
                return true;
            }
        } else if Equals(actionName, n"UIDisconnectFromServer") {
            GameInstance.GetNetworkWorldSystem().Leave();
            return true;
        }
        return false;
    }
}
