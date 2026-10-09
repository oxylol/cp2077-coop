@wrapMethod(WorldMappinsContainerController)
public func CreateMappinUIProfile(mappin: wref<IMappin>, mappinVariant: gamedataMappinVariant, customData: ref<MappinControllerCustomData>) -> MappinUIProfile {
    if Equals(mappinVariant, gamedataMappinVariant.CPO_RemotePlayerVariant) {
        return MappinUIProfile.Create(r"base\\gameplay\\gui\\widgets\\mappins\\quest\\default_mappin.inkwidget", t"MappinUISpawnProfile.Always", t"WorldMappinUIProfile.GameplayRole");
    }
    return wrappedMethod(mappin, mappinVariant, customData);
}

@wrapMethod(MinimapContainerController)
public func CreateMappinUIProfile(mappin: wref<IMappin>, mappinVariant: gamedataMappinVariant, customData: ref<MappinControllerCustomData>) -> MappinUIProfile {
    if Equals(mappinVariant, gamedataMappinVariant.CPO_RemotePlayerVariant) {
        return MappinUIProfile.Create(r"mods\\cyberpunkmp\\minimap_mappin_player.inkwidget", t"MappinUISpawnProfile.Always", t"MinimapMappinUIProfile.CPORemote");
    }
    return wrappedMethod(mappin, mappinVariant, customData);
}

@wrapMethod(MappinUIUtils)
public final static func MappinToTexturePart(mappinVariant: gamedataMappinVariant) -> CName {
    if Equals(mappinVariant, gamedataMappinVariant.CPO_RemotePlayerVariant) {
        return n"codex_character";
    }
    return wrappedMethod(mappinVariant);
}