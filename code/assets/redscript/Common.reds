public func CreateInputHint(label: CName, action: CName, hold: Bool) -> InputHintData {
    let data: InputHintData;
    data.source = n"CyberpunkMP";
    data.action = action;
    if hold {
        data.holdIndicationType = inkInputHintHoldIndicationType.Hold;
    } else {
        data.holdIndicationType = inkInputHintHoldIndicationType.Press;
    }
    data.sortingPriority = 0;
    data.enableHoldAnimation = false;
    data.localizedLabel = GetLocalizedTextByKey(label);
    if StrLen( data.localizedLabel) == 0 {
            data.localizedLabel = ToString(label);
    };
    // data.groupId = n"CyberpunkMP";
    return data;
}