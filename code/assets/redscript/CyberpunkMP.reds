// UiPhone/UiVendor method

@addField(UIGameDataDef)
public let UIMultiplayerContextRequest: BlackboardID_Bool;

@addField(UIGameDataDef)
public let UIMultiplayerConnectedToServer: BlackboardID_Bool;

@addField(PlayerPuppet)
public let m_multiplayerUIActiveListener: ref<CallbackHandle>;

@wrapMethod(PlayerPuppet)
private final func EnableUIBlackboardListener(enable: Bool) -> Void {
    wrappedMethod(enable);
    let blackboardSystem: ref<BlackboardSystem> = GameInstance.GetBlackboardSystem(GetGameInstance());
    let uiBlackboard: ref<IBlackboard> = blackboardSystem.Get(GetAllBlackboardDefs().UIGameData);
    if enable {
        this.m_multiplayerUIActiveListener = uiBlackboard.RegisterListenerBool(GetAllBlackboardDefs().UIGameData.UIMultiplayerContextRequest, this, n"OnUIMultiplayerContextChanged");
    } else {
        uiBlackboard.UnregisterListenerBool(GetAllBlackboardDefs().UIGameData.UIMultiplayerContextRequest, this.m_multiplayerUIActiveListener);
        this.m_multiplayerUIActiveListener = null;
    }
}

@addMethod(PlayerPuppet)
protected cb func OnUIMultiplayerContextChanged(value: Bool) -> Bool {
    let psmEvent: ref<PSMPostponedParameterBool>;
    if value {
        psmEvent = new PSMPostponedParameterBool();
        psmEvent.id = n"OnUIMultiplayerContextActive";
    } else {
        psmEvent = new PSMPostponedParameterBool();
        psmEvent.id = n"OnUIMultiplayerContextInactive";
    };
    psmEvent.value = true;
    this.QueueEvent(psmEvent);
}

public class UiMultiplayerContextDecisions extends InputContextTransitionDecisions {

  protected const func EnterCondition(const stateContext: ref<StateContext>, const scriptInterface: ref<StateGameScriptInterface>) -> Bool {
    let psmMultiplayerResult: StateResultBool = stateContext.GetTemporaryBoolParameter(n"OnUIMultiplayerContextActive");
    return psmMultiplayerResult.value;
  }

  protected const func ExitCondition(const stateContext: ref<StateContext>, const scriptInterface: ref<StateGameScriptInterface>) -> Bool {
    let psmMultiplayerResult: StateResultBool = stateContext.GetTemporaryBoolParameter(n"OnUIMultiplayerContextInactive");
    return psmMultiplayerResult.value;
  }
}

